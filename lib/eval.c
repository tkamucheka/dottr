#include "dottr/eval.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dottr/host.h"
#include "dottr/util.h"

typedef enum {
    UNIT_PENDING,
    UNIT_SATISFIED,
    UNIT_FAILED,
    UNIT_SKIPPED,
} UnitState;

typedef struct {
    const char *name;
    bool value;
} Fact;

/* Variables and facts of the unit being evaluated. */
typedef struct {
    /* Locals in binding order; also the environment of shell blocks. */
    HostVariable *locals;
    size_t localCount;
    size_t localCapacity;
    Fact *facts;
    size_t factCount;
    size_t factCapacity;
} Scope;

typedef struct {
    const Manifest *manifest;
    EvalMode mode;
    Diag *diag;
    Arena arena;
} Evaluator;

static const char *lookupLocal(const Scope *scope, const char *name)
{
    for (size_t i = 0; i < scope->localCount; i++) {
        if (strcmp(scope->locals[i].name, name) == 0) {
            return scope->locals[i].value;
        }
    }
    return NULL;
}

static void bindLocal(Evaluator *ev, Scope *scope, const char *name,
                      const char *value)
{
    scope->locals = arenaGrow(&ev->arena, scope->locals, scope->localCount,
                              &scope->localCapacity, sizeof *scope->locals);
    scope->locals[scope->localCount].name = name;
    scope->locals[scope->localCount].value = value;
    scope->localCount++;
}

static const Fact *lookupFact(const Scope *scope, const char *name)
{
    for (size_t i = 0; i < scope->factCount; i++) {
        if (strcmp(scope->facts[i].name, name) == 0) {
            return &scope->facts[i];
        }
    }
    return NULL;
}

static void recordFact(Evaluator *ev, Scope *scope, const char *name,
                       bool value)
{
    for (size_t i = 0; i < scope->factCount; i++) {
        if (strcmp(scope->facts[i].name, name) == 0) {
            scope->facts[i].value = value;
            return;
        }
    }
    scope->facts = arenaGrow(&ev->arena, scope->facts, scope->factCount,
                             &scope->factCapacity, sizeof *scope->facts);
    scope->facts[scope->factCount].name = name;
    scope->facts[scope->factCount].value = value;
    scope->factCount++;
}

/* Performs the interpolation pass. */
static bool render(Evaluator *ev, const Scope *scope, const Template *template,
                   const char **result)
{
    StrBuf buf;
    strBufInit(&buf);

    for (size_t i = 0; i < template->count; i++) {
        const TemplatePart *part = &template->parts[i];
        if (part->kind == TEMPLATE_TEXT) {
            strBufAppend(&buf, part->text, part->length);
            continue;
        }

        const char *value = NULL;
        if (strcmp(part->text, "dottr.env.home") == 0) {
            value = hostHomeDirectory();
            if (!value) {
                diagError(ev->diag, part->line, part->column,
                          "cannot determine the home directory: HOME is "
                          "unset and the password database has no entry");
                strBufFree(&buf);
                return false;
            }
        } else {
            value = lookupLocal(scope, part->text);
            if (!value) {
                diagError(ev->diag, part->line, part->column,
                          "undefined variable `%s`", part->text);
                strBufFree(&buf);
                return false;
            }
        }
        strBufAppendStr(&buf, value);
    }

    *result = arenaStrndup(&ev->arena, buf.data, buf.length);
    strBufFree(&buf);
    return true;
}

static bool evaluatePredicate(PredicateKind kind, const char *value)
{
    switch (kind) {
    case PREDICATE_COMMAND: return hostCommandExists(value);
    case PREDICATE_FILE: return hostIsFile(value);
    case PREDICATE_DIRECTORY: return hostIsDirectory(value);
    }
    return false;
}

/* Builds the package manager command. */
static void packageCommand(const char *package, const char **argv)
{
    size_t n = 0;
    if (!hostIsRoot()) {
        argv[n++] = "sudo";
    }
    argv[n++] = "pacman";
    argv[n++] = "-S";
    argv[n++] = "--needed";
    argv[n++] = "--noconfirm";
    argv[n++] = package;
    argv[n] = NULL;
}

static void printCommand(const char **argv)
{
    for (size_t i = 0; argv[i]; i++) {
        printf("%s%s", i > 0 ? " " : "", argv[i]);
    }
}

static bool installPackage(Evaluator *ev, const char *package,
                           const char *childName, int line, int column)
{
    const char *argv[8];
    packageCommand(package, argv);

    if (ev->mode == EVAL_PLAN) {
        printf("  install package %s", package);
        if (childName) {
            printf(" (child unit %s)", childName);
        }
        printf(": ");
        printCommand(argv);
        printf("\n");
        return true;
    }

    printf("  -> install package %s", package);
    if (childName) {
        printf(" (child unit %s)", childName);
    }
    printf("\n");

    int status = hostRun((char *const *)argv, NULL, 0);
    if (status == 0) {
        return true;
    }
    if (status < 0) {
        diagError(ev->diag, line, column,
                  "package installation of `%s` could not be started",
                  package);
    } else {
        diagError(ev->diag, line, column,
                  "package installation of `%s` failed with status %d",
                  package, status);
    }
    return false;
}

static void printScript(const char *script)
{
    const char *line = script;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length > 0) {
            printf("      %.*s", (int)length, line);
        }
        printf("\n");
        if (!end) {
            break;
        }
        line = end + 1;
    }
}

static bool runActions(Evaluator *ev, const Scope *scope,
                       const ActionList *actions)
{
    for (size_t i = 0; i < actions->count; i++) {
        const Action *action = &actions->items[i];
        const char *interpreter = shellDialectInterpreter(action->dialect);

        /* A raw block holds only text, so rendering cannot fail here. */
        const char *script;
        if (!render(ev, scope, &action->script, &script)) {
            return false;
        }

        if (ev->mode == EVAL_PLAN) {
            printf("    shell (%s)\n", interpreter);
            printScript(script);
            continue;
        }

        printf("  -> shell (%s), line %d\n", interpreter, action->line);
        const char *argv[] = {interpreter, "-e", "-c", script, NULL};
        int status = hostRun((char *const *)argv, scope->locals,
                             scope->localCount);
        if (status != 0) {
            if (status < 0) {
                diagError(ev->diag, action->line, action->column,
                          "shell block could not be started");
            } else {
                diagError(ev->diag, action->line, action->column,
                          "shell block exited with status %d", status);
            }
            return false;
        }
    }
    return true;
}

static bool runApply(Evaluator *ev, const Scope *scope, const Stmt *stmt)
{
    const char *variable = stmt->as.apply.variable.text;

    if (ev->mode == EVAL_PLAN) {
        bool ok = true;
        switch (stmt->kind) {
        case STMT_APPLY:
            printf("  apply\n");
            break;
        case STMT_APPLY_IF:
            printf("  apply_if %s\n", variable);
            break;
        default:
            printf("  apply_unless %s\n", variable);
            break;
        }
        ok = runActions(ev, scope, &stmt->as.apply.thenActions) && ok;
        if (stmt->as.apply.hasElse) {
            printf("  else\n");
            ok = runActions(ev, scope, &stmt->as.apply.elseActions) && ok;
        }
        return ok;
    }

    if (stmt->kind == STMT_APPLY) {
        return runActions(ev, scope, &stmt->as.apply.thenActions);
    }

    const Fact *fact = lookupFact(scope, variable);
    bool value = fact && fact->value;
    if (stmt->kind == STMT_APPLY_IF) {
        if (value) {
            printf("  -> apply_if %s: fact is true, evaluating first branch\n",
                   variable);
            return runActions(ev, scope, &stmt->as.apply.thenActions);
        }
        if (stmt->as.apply.hasElse) {
            printf("  -> apply_if %s: fact is false, evaluating else "
                   "branch\n",
                   variable);
            return runActions(ev, scope, &stmt->as.apply.elseActions);
        }
        printf("  -> apply_if %s: fact is false, nothing to do\n", variable);
        return true;
    }

    if (value) {
        printf("  -> apply_unless %s: fact is true, nothing to do\n", variable);
        return true;
    }
    printf("  -> apply_unless %s: fact is false, evaluating body\n", variable);
    return runActions(ev, scope, &stmt->as.apply.thenActions);
}

static bool runStmt(Evaluator *ev, Scope *scope, const Stmt *stmt)
{
    bool plan = ev->mode == EVAL_PLAN;

    switch (stmt->kind) {
    case STMT_LOCAL: {
        const char *value;
        if (!render(ev, scope, &stmt->as.local.value, &value)) {
            return false;
        }
        bindLocal(ev, scope, stmt->as.local.name.text, value);
        if (plan) {
            printf("  local %s = \"%s\"\n", stmt->as.local.name.text, value);
        }
        return true;
    }

    case STMT_NEEDS:
        if (plan) {
            printf("  needs ");
            for (size_t i = 0; i < stmt->as.needs.count; i++) {
                printf("%s%s", i > 0 ? ", " : "", stmt->as.needs.names[i].text);
            }
            printf("\n");
        }
        return true;

    case STMT_CHILD_PKG:
        return installPackage(ev, stmt->as.childPkg.package.text,
                              stmt->as.childPkg.childName, stmt->line,
                              stmt->column);

    case STMT_CHECK: {
        const char *name = stmt->as.predicate.variable.text;
        const char *kind = predicateKindName(stmt->as.predicate.kind);
        const char *value = lookupLocal(scope, name);
        bool result = evaluatePredicate(stmt->as.predicate.kind, value);
        recordFact(ev, scope, name, result);
        if (plan) {
            printf("  check %s %s: %s (currently %s)\n", kind, name, value,
                   result ? "true" : "false");
        } else {
            printf("  -> check %s %s: %s\n", kind, name,
                   result ? "true" : "false");
        }
        return true;
    }

    case STMT_CREATE: {
        const char *name = stmt->as.predicate.variable.text;
        const char *value = lookupLocal(scope, name);
        if (plan) {
            printf("  create directory %s: %s\n", name, value);
            return true;
        }
        printf("  -> create directory %s: %s\n", name, value);
        char message[512];
        if (!hostCreateDirectory(value, message, sizeof message)) {
            diagError(ev->diag, stmt->line, stmt->column, "%s", message);
            return false;
        }
        return true;
    }

    case STMT_APPLY:
    case STMT_APPLY_IF:
    case STMT_APPLY_UNLESS:
        return runApply(ev, scope, stmt);
    }
    return false;
}

static bool runUnit(Evaluator *ev, const Unit *unit)
{
    if (ev->mode == EVAL_PLAN) {
        printf("unit %s\n", unit->name.text);
    } else {
        printf("==> unit %s\n", unit->name.text);
    }

    if (!unit->isBlock) {
        return installPackage(ev, unit->name.text, NULL, unit->name.line,
                              unit->name.column);
    }

    Scope scope = {0};
    bool ok = true;
    for (size_t i = 0; i < unit->stmtCount; i++) {
        if (!runStmt(ev, &scope, &unit->stmts[i])) {
            ok = false;
            if (ev->mode == EVAL_APPLY) {
                break;
            }
        }
    }
    return ok;
}

static void visit(const Manifest *manifest, const Unit *unit, bool *visited,
                  const Unit **order, size_t *count)
{
    size_t index = (size_t)(unit - manifest->units);
    if (visited[index]) {
        return;
    }
    visited[index] = true;
    for (size_t i = 0; i < unit->depCount; i++) {
        visit(manifest, unit->deps[i], visited, order, count);
    }
    order[(*count)++] = unit;
}

int evaluateManifest(const Manifest *manifest, EvalMode mode,
                     Unit *const *selected, size_t selectedCount, Diag *diag)
{
    Evaluator ev = {manifest, mode, diag, {0}};
    arenaInit(&ev.arena);

    size_t total = manifest->count;
    bool *visited = arenaAlloc(&ev.arena, (total + 1) * sizeof *visited);
    const Unit **order = arenaAlloc(&ev.arena, (total + 1) * sizeof *order);
    UnitState *states = arenaAlloc(&ev.arena, (total + 1) * sizeof *states);
    size_t count = 0;

    for (size_t i = 0; i < total; i++) {
        const Unit *unit = &manifest->units[i];
        bool wanted = selectedCount == 0;
        for (size_t j = 0; j < selectedCount && !wanted; j++) {
            wanted = selected[j] == unit;
        }
        if (wanted) {
            visit(manifest, unit, visited, order, &count);
        }
    }

    size_t satisfied = 0;
    size_t failed = 0;
    size_t skipped = 0;

    for (size_t i = 0; i < count; i++) {
        const Unit *unit = order[i];
        size_t index = (size_t)(unit - manifest->units);

        if (mode == EVAL_PLAN) {
            if (i > 0) {
                printf("\n");
            }
            bool ok = runUnit(&ev, unit);
            states[index] = ok ? UNIT_SATISFIED : UNIT_FAILED;
            failed += !ok;
            continue;
        }

        const Unit *blocker = NULL;
        for (size_t j = 0; j < unit->depCount && !blocker; j++) {
            size_t depIndex = (size_t)(unit->deps[j] - manifest->units);
            if (states[depIndex] != UNIT_SATISFIED) {
                blocker = unit->deps[j];
            }
        }
        if (blocker) {
            printf("==> unit %s skipped: dependency %s was not satisfied\n",
                   unit->name.text, blocker->name.text);
            states[index] = UNIT_SKIPPED;
            skipped++;
            continue;
        }

        if (runUnit(&ev, unit)) {
            states[index] = UNIT_SATISFIED;
            satisfied++;
        } else {
            printf("==> unit %s failed\n", unit->name.text);
            states[index] = UNIT_FAILED;
            failed++;
        }
    }

    if (mode == EVAL_APPLY) {
        printf("summary: %zu satisfied, %zu failed, %zu skipped\n", satisfied,
               failed, skipped);
    }
    fflush(stdout);

    arenaFree(&ev.arena);
    return failed == 0 && skipped == 0 ? 0 : 1;
}
