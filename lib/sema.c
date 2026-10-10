#include "dottr/sema.h"

#include <string.h>

static const char *const builtinReferences[] = {
    "dottr.env.home",
};

/*
 * Environment variables that a local may not replace, because they control
 * how a shell starts or finds commands.
 */
static const char *const protectedVariables[] = {
    "BASHOPTS", "BASH_ENV", "CDPATH", "ENV", "HOME", "IFS",
    "OLDPWD", "PATH", "PWD", "SHELL", "SHELLOPTS", "ZDOTDIR",
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

bool isProtectedVariable(const char *name)
{
    for (size_t i = 0;
         i < sizeof protectedVariables / sizeof protectedVariables[0]; i++) {
        if (strcmp(protectedVariables[i], name) == 0) {
            return true;
        }
    }
    return false;
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
            if (isProtectedVariable(name->text)) {
                diagError(an->diag, name->line, name->column,
                          "variable `%s` would replace the environment "
                          "variable %s in shell blocks; choose another name",
                          name->text, name->text);
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

bool analyzeManifest(Arena *arena, Manifest *manifest, Diag *diag)
{
    Analyzer an = {arena, diag, manifest};
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
