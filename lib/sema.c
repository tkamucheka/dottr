#include "dottr/sema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const builtinReferences[] = {
    "dottr.env.home",
};

/*
 * Environment variables that a local may not replace, because they control
 * how a shell starts or finds commands.
 */
static const char *const protectedVariables[] = {
    /* POSIX shells. */
    "BASHOPTS", "BASH_ENV", "CDPATH", "ENV", "HOME", "IFS", "OLDPWD", "PATH",
    "PWD", "SHELL", "SHELLOPTS", "ZDOTDIR",
    /* PowerShell and process creation on Windows. */
    "APPDATA", "ComSpec", "LOCALAPPDATA", "PATHEXT", "PSModulePath",
    "SystemRoot", "TEMP", "TMP", "USERPROFILE",
};

typedef struct {
    const char **names;
    size_t count;
    size_t capacity;
} NameSet;

typedef enum {
    VISIT_NEW,
    VISIT_ACTIVE,
    VISIT_DONE,
} VisitState;

typedef struct {
    Arena *arena;
    Diag *diag;
    const Manifest *manifest;
    /* The whole manifest, of which manifest is the view of one system. */
    const Manifest *full;
} Analyzer;

static bool nameSetHas(const NameSet *set, const char *name)
{
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void nameSetAdd(Arena *arena, NameSet *set, const char *name)
{
    if (nameSetHas(set, name)) {
        return;
    }
    set->names = arenaGrow(arena, set->names, set->count, &set->capacity,
                           sizeof *set->names);
    set->names[set->count++] = name;
}

static bool isIdentifier(const char *text)
{
    if (!((*text >= 'A' && *text <= 'Z') || (*text >= 'a' && *text <= 'z') ||
          *text == '_')) {
        return false;
    }
    for (const char *c = text; *c; c++) {
        bool valid = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                     (*c >= '0' && *c <= '9') || *c == '_';
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool isBuiltinReference(const char *path)
{
    for (size_t i = 0; i < sizeof builtinReferences / sizeof builtinReferences[0];
         i++) {
        if (strcmp(builtinReferences[i], path) == 0) {
            return true;
        }
    }
    return false;
}

Unit *findUnit(const Manifest *manifest, const char *name)
{
    for (size_t i = 0; i < manifest->count; i++) {
        if (strcmp(manifest->units[i].name.text, name) == 0) {
            return &manifest->units[i];
        }
    }
    return NULL;
}

/* Returns the line of the first `local NAME` in the unit, or 0. */
static int localLine(const Unit *unit, const char *name)
{
    for (size_t i = 0; i < unit->stmtCount; i++) {
        const Stmt *stmt = &unit->stmts[i];
        if (stmt->kind == STMT_LOCAL &&
            strcmp(stmt->as.local.name.text, name) == 0) {
            return stmt->line;
        }
    }
    return 0;
}

static void reportUnbound(Analyzer *an, const Unit *unit, const char *name,
                          int line, int column)
{
    int boundAt = localLine(unit, name);
    if (boundAt == line) {
        diagError(an->diag, line, column,
                  "variable `%s` is used in its own definition", name);
    } else if (boundAt > 0) {
        diagError(an->diag, line, column,
                  "variable `%s` is used before it is bound on line %d", name,
                  boundAt);
    } else {
        diagError(an->diag, line, column, "undefined variable `%s`", name);
    }
}

static void checkTemplate(Analyzer *an, const Unit *unit,
                          const Template *template, const NameSet *locals)
{
    for (size_t i = 0; i < template->count; i++) {
        const TemplatePart *part = &template->parts[i];
        if (part->kind != TEMPLATE_REF) {
            continue;
        }
        if (strchr(part->text, '.') == NULL) {
            if (!nameSetHas(locals, part->text)) {
                reportUnbound(an, unit, part->text, part->line, part->column);
            }
        } else if (!isBuiltinReference(part->text)) {
            if (strncmp(part->text, "dottr.", 6) == 0) {
                diagError(an->diag, part->line, part->column,
                          "unknown built-in value `%s`; version 1 defines "
                          "dottr.env.home",
                          part->text);
            } else {
                diagError(an->diag, part->line, part->column,
                          "unknown reference `%s`; a reference containing '.' "
                          "must name a built-in value under `dottr`",
                          part->text);
            }
        }
    }
}

static bool equalIgnoringCase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == *b;
}

const char *protectedVariable(const char *name)
{
    for (size_t i = 0;
         i < sizeof protectedVariables / sizeof protectedVariables[0]; i++) {
        if (equalIgnoringCase(protectedVariables[i], name)) {
            return protectedVariables[i];
        }
    }
    return NULL;
}

static void analyzeUnitBody(Analyzer *an, const Unit *unit)
{
    NameSet locals = {0};
    NameSet checked = {0};

    for (size_t i = 0; i < unit->stmtCount; i++) {
        const Stmt *stmt = &unit->stmts[i];
        switch (stmt->kind) {
        case STMT_LOCAL: {
            const Name *name = &stmt->as.local.name;
            checkTemplate(an, unit, &stmt->as.local.value, &locals);
            const char *variable = protectedVariable(name->text);
            if (variable) {
                diagError(an->diag, name->line, name->column,
                          "variable `%s` would replace the environment "
                          "variable %s in shell blocks; choose another name",
                          name->text, variable);
            }
            if (nameSetHas(&locals, name->text)) {
                diagError(an->diag, name->line, name->column,
                          "variable `%s` is already bound on line %d",
                          name->text, localLine(unit, name->text));
            } else {
                nameSetAdd(an->arena, &locals, name->text);
            }
            break;
        }

        case STMT_CHECK:
        case STMT_CREATE: {
            const Name *variable = &stmt->as.predicate.variable;
            if (!nameSetHas(&locals, variable->text)) {
                reportUnbound(an, unit, variable->text, variable->line,
                              variable->column);
            } else if (stmt->kind == STMT_CHECK) {
                nameSetAdd(an->arena, &checked, variable->text);
            }
            break;
        }

        case STMT_APPLY_IF:
        case STMT_APPLY_UNLESS: {
            const Name *variable = &stmt->as.apply.variable;
            if (!nameSetHas(&checked, variable->text)) {
                diagError(an->diag, variable->line, variable->column,
                          "`%s %s` requires a preceding `check ... %s` in "
                          "unit `%s`",
                          stmt->kind == STMT_APPLY_IF ? "apply_if"
                                                      : "apply_unless",
                          variable->text, variable->text, unit->name.text);
            }
            break;
        }

        case STMT_APPLY:
        case STMT_NEEDS:
        case STMT_CHILD_PKG:
            break;
        }
    }
}

static void resolveDependencies(Analyzer *an, Unit *unit)
{
    size_t capacity = 0;
    for (size_t i = 0; i < unit->stmtCount; i++) {
        const Stmt *stmt = &unit->stmts[i];
        if (stmt->kind != STMT_NEEDS) {
            continue;
        }
        for (size_t j = 0; j < stmt->as.needs.count; j++) {
            const Name *name = &stmt->as.needs.names[j];
            Unit *target = findUnit(an->manifest, name->text);
            const Unit *elsewhere =
                target ? NULL : findUnit(an->full, name->text);
            if (elsewhere) {
                diagError(an->diag, name->line, name->column,
                          "unit `%s` is not declared on this system; its "
                          "declaration on line %d is inside an `on` block",
                          name->text, elsewhere->name.line);
                continue;
            }
            if (!target) {
                const char *quote = isIdentifier(name->text) ? "" : "\"";
                diagError(an->diag, name->line, name->column,
                          "unit `%s` is not declared; declare it at top "
                          "level, for example `pkg %s%s%s`",
                          name->text, quote, name->text, quote);
                continue;
            }
            bool duplicate = false;
            for (size_t k = 0; k < unit->depCount; k++) {
                if (unit->deps[k] == target) {
                    duplicate = true;
                }
            }
            if (!duplicate) {
                unit->deps = arenaGrow(an->arena, unit->deps, unit->depCount,
                                       &capacity, sizeof *unit->deps);
                unit->deps[unit->depCount++] = target;
            }
        }
    }
}

/*
 * Depth-first search for a dependency cycle. On finding one, reports it and
 * returns true. path holds the units on the current search path.
 */
static bool findCycle(Analyzer *an, Unit *unit, VisitState *states,
                      Unit **path, size_t depth)
{
    size_t index = (size_t)(unit - an->manifest->units);
    states[index] = VISIT_ACTIVE;
    path[depth] = unit;

    for (size_t i = 0; i < unit->depCount; i++) {
        Unit *dep = unit->deps[i];
        size_t depIndex = (size_t)(dep - an->manifest->units);
        if (states[depIndex] == VISIT_ACTIVE) {
            size_t start = 0;
            while (path[start] != dep) {
                start++;
            }
            StrBuf cycle;
            strBufInit(&cycle);
            for (size_t k = start; k <= depth; k++) {
                strBufAppendStr(&cycle, path[k]->name.text);
                strBufAppendStr(&cycle, " -> ");
            }
            strBufAppendStr(&cycle, dep->name.text);
            diagError(an->diag, dep->name.line, dep->name.column,
                      "dependency cycle: %s", cycle.data);
            strBufFree(&cycle);
            return true;
        }
        if (states[depIndex] == VISIT_NEW &&
            findCycle(an, dep, states, path, depth + 1)) {
            return true;
        }
    }

    states[index] = VISIT_DONE;
    return false;
}

/* Applies the version 1 rules to a manifest without `on` blocks. */
static bool analyzeView(Arena *arena, Manifest *manifest,
                        const Manifest *full, Diag *diag)
{
    Analyzer an = {arena, diag, manifest, full};
    int errorsBefore = diag->errorCount;

    for (size_t i = 0; i < manifest->count; i++) {
        const Unit *unit = &manifest->units[i];
        for (size_t j = 0; j < i; j++) {
            const Unit *earlier = &manifest->units[j];
            if (strcmp(earlier->name.text, unit->name.text) == 0) {
                diagError(diag, unit->name.line, unit->name.column,
                          "duplicate unit `%s`; first declared on line %d",
                          unit->name.text, earlier->name.line);
                break;
            }
        }
    }

    for (size_t i = 0; i < manifest->count; i++) {
        analyzeUnitBody(&an, &manifest->units[i]);
        resolveDependencies(&an, &manifest->units[i]);
    }

    if (manifest->count > 0) {
        VisitState *states =
            arenaAlloc(arena, manifest->count * sizeof *states);
        Unit **path = arenaAlloc(arena, manifest->count * sizeof *path);
        for (size_t i = 0; i < manifest->count; i++) {
            if (states[i] == VISIT_NEW &&
                findCycle(&an, &manifest->units[i], states, path, 0)) {
                break;
            }
        }
    }

    return diag->errorCount == errorsBefore;
}

static bool appliesOn(SystemSet item, SystemSet systems)
{
    return (item & systems) == systems;
}

/*
 * Builds the view of manifest on systems: copies of the units, statements,
 * and actions that apply on every one of systems. Names, templates, and
 * other contents are shared with manifest.
 */
static void buildView(Arena *arena, const Manifest *manifest,
                      SystemSet systems, Manifest *view)
{
    memset(view, 0, sizeof *view);
    view->units = arenaAlloc(arena, (manifest->count + 1) * sizeof *view->units);

    for (size_t i = 0; i < manifest->count; i++) {
        const Unit *unit = &manifest->units[i];
        if (!appliesOn(unit->systems, systems)) {
            continue;
        }
        Unit *copy = &view->units[view->count++];
        *copy = *unit;
        copy->deps = NULL;
        copy->depCount = 0;
        copy->stmts = arenaAlloc(arena, (unit->stmtCount + 1) *
                                            sizeof *copy->stmts);
        copy->stmtCount = 0;

        for (size_t j = 0; j < unit->stmtCount; j++) {
            const Stmt *stmt = &unit->stmts[j];
            if (!appliesOn(stmt->systems, systems)) {
                continue;
            }
            Stmt *stmtCopy = &copy->stmts[copy->stmtCount++];
            *stmtCopy = *stmt;
            if (stmt->kind != STMT_APPLY && stmt->kind != STMT_APPLY_IF &&
                stmt->kind != STMT_APPLY_UNLESS) {
                continue;
            }
            ActionList *lists[] = {&stmtCopy->as.apply.thenActions,
                                   &stmtCopy->as.apply.elseActions};
            for (size_t k = 0; k < 2; k++) {
                const ActionList original = *lists[k];
                lists[k]->items = arenaAlloc(arena, (original.count + 1) *
                                                        sizeof *original.items);
                lists[k]->count = 0;
                for (size_t a = 0; a < original.count; a++) {
                    if (appliesOn(original.items[a].systems, systems)) {
                        lists[k]->items[lists[k]->count++] = original.items[a];
                    }
                }
            }
        }
    }
}

/* A diagnostic of one view, and the systems on which it occurs. */
typedef struct {
    int line;
    int column;
    char *message;
    SystemSet systems;
    size_t order;
} SystemDiagnostic;

typedef struct {
    Arena *arena;
    SystemDiagnostic *items;
    size_t count;
    size_t capacity;
    SystemSet current;
} DiagnosticCollector;

static void collectDiagnostic(void *context, int line, int column,
                              const char *message)
{
    DiagnosticCollector *collector = context;
    for (size_t i = 0; i < collector->count; i++) {
        SystemDiagnostic *item = &collector->items[i];
        if (item->line == line && item->column == column &&
            strcmp(item->message, message) == 0) {
            item->systems |= collector->current;
            return;
        }
    }
    collector->items =
        arenaGrow(collector->arena, collector->items, collector->count,
                  &collector->capacity, sizeof *collector->items);
    collector->items[collector->count] = (SystemDiagnostic){
        line, column, arenaStrdup(collector->arena, message),
        collector->current, collector->count,
    };
    collector->count++;
}

static int compareDiagnostics(const void *a, const void *b)
{
    const SystemDiagnostic *x = a;
    const SystemDiagnostic *y = b;
    if (x->line != y->line) {
        return x->line < y->line ? -1 : 1;
    }
    if (x->column != y->column) {
        return x->column < y->column ? -1 : 1;
    }
    return x->order < y->order ? -1 : x->order > y->order;
}

static const SystemSet everySystem[] = {
    SYSTEM_PACMAN, SYSTEM_APT, SYSTEM_DNF, SYSTEM_BREW, SYSTEM_WINGET,
};

bool analyzeManifest(Arena *arena, Manifest *manifest, Diag *diag)
{
    if (manifest->onCount == 0) {
        return analyzeView(arena, manifest, manifest, diag);
    }

    /* Every system sees its own view; each error is reported once. */
    DiagnosticCollector collector = {arena, NULL, 0, 0, 0};
    for (size_t i = 0; i < sizeof everySystem / sizeof everySystem[0]; i++) {
        Manifest view;
        buildView(arena, manifest, everySystem[i], &view);
        Diag viewDiag;
        diagInitSink(&viewDiag, collectDiagnostic, &collector);
        collector.current = everySystem[i];
        analyzeView(arena, &view, manifest, &viewDiag);
    }

    if (collector.count > 0) {
        qsort(collector.items, collector.count, sizeof *collector.items,
              compareDiagnostics);
    }
    for (size_t i = 0; i < collector.count; i++) {
        const SystemDiagnostic *item = &collector.items[i];
        if (item->systems == SYSTEM_ALL) {
            diagError(diag, item->line, item->column, "%s", item->message);
        } else {
            char systems[96];
            describeSystems(item->systems, systems, sizeof systems);
            diagError(diag, item->line, item->column, "%s (on %s)",
                      item->message, systems);
        }
    }
    return collector.count == 0;
}

bool viewsAgree(const Manifest *manifest, SystemSet systems)
{
    for (size_t i = 0; i < manifest->onCount; i++) {
        SystemSet shared = manifest->onBlocks[i].systems & systems;
        if (shared != 0 && shared != systems) {
            return false;
        }
    }
    return true;
}

static void discardDiagnostic(void *context, int line, int column,
                              const char *message)
{
    (void)context;
    (void)line;
    (void)column;
    (void)message;
}

bool selectManifest(Arena *arena, const Manifest *manifest,
                    SystemSet systems, Manifest *view)
{
    buildView(arena, manifest, systems, view);
    Diag quiet;
    diagInitSink(&quiet, discardDiagnostic, NULL);
    return analyzeView(arena, view, manifest, &quiet);
}
