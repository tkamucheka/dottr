#include "dottr/parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    Arena *arena;
    Diag *diag;
    const Token *tokens;
    size_t count;
    size_t pos;
    char described[160];
    Manifest *manifest;
    size_t unitCapacity;
    size_t onCapacity;
    /* Systems of the enclosing `on` blocks; SYSTEM_ALL outside them. */
    SystemSet systems;
} Parser;

static const Token *peek(const Parser *p)
{
    return &p->tokens[p->pos];
}

static bool at(const Parser *p, TokenKind kind)
{
    return peek(p)->kind == kind;
}

static const Token *advance(Parser *p)
{
    const Token *token = &p->tokens[p->pos];
    if (token->kind != TOK_EOF) {
        p->pos++;
    }
    return token;
}

static void skipNewlines(Parser *p)
{
    while (at(p, TOK_NEWLINE)) {
        advance(p);
    }
}

static bool isKeyword(TokenKind kind)
{
    return kind >= TOK_PKG;
}

/* Describes a token for use after "found". */
static const char *describe(Parser *p, const Token *token)
{
    if (token->kind == TOK_IDENT) {
        snprintf(p->described, sizeof p->described, "identifier `%.*s`",
                 (int)token->length, token->text);
    } else {
        snprintf(p->described, sizeof p->described, "%s",
                 tokenKindName(token->kind));
    }
    return p->described;
}

static void errorAt(Parser *p, const Token *token, const char *format, ...)
    DOTTR_PRINTF(3, 4);

static void errorAt(Parser *p, const Token *token, const char *format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    diagError(p->diag, token->line, token->column, "%s", message);
}

static bool expect(Parser *p, TokenKind kind, const char *context)
{
    if (at(p, kind)) {
        advance(p);
        return true;
    }
    errorAt(p, peek(p), "expected %s %s, found %s", tokenKindName(kind),
            context, describe(p, peek(p)));
    return false;
}

static Name nameFrom(Parser *p, const Token *token)
{
    Name name = {
        arenaStrndup(p->arena, token->text, token->length),
        token->line,
        token->column,
    };
    return name;
}

static bool parseUnitName(Parser *p, Name *name, const char *context)
{
    const Token *token = peek(p);

    if (token->kind == TOK_IDENT) {
        advance(p);
        *name = nameFrom(p, token);
        return true;
    }

    if (token->kind == TOK_STRING) {
        const Template *value = &token->value;
        for (size_t i = 0; i < value->count; i++) {
            if (value->parts[i].kind == TEMPLATE_REF) {
                diagError(p->diag, value->parts[i].line, value->parts[i].column,
                          "a unit name must not contain interpolation "
                          "references");
                return false;
            }
        }
        if (value->count == 0) {
            errorAt(p, token, "a unit name must not be empty");
            return false;
        }
        advance(p);
        name->text = value->parts[0].text;
        name->line = token->line;
        name->column = token->column;
        return true;
    }

    if (isKeyword(token->kind)) {
        errorAt(p, token,
                "expected unit name %s, found reserved word %s; write a unit "
                "with this name as a string, \"%.*s\"",
                context, tokenKindName(token->kind), (int)token->length,
                token->text);
        return false;
    }

    errorAt(p, token, "expected unit name %s, found %s", context,
            describe(p, token));
    return false;
}

static bool parseVariable(Parser *p, Name *name, const char *context)
{
    const Token *token = peek(p);
    if (token->kind == TOK_IDENT) {
        advance(p);
        *name = nameFrom(p, token);
        return true;
    }
    if (isKeyword(token->kind)) {
        errorAt(p, token,
                "expected variable name %s, found reserved word %s", context,
                tokenKindName(token->kind));
        return false;
    }
    errorAt(p, token, "expected variable name %s, found %s", context,
            describe(p, token));
    return false;
}

static bool parseSelectors(Parser *p, const char *keyword, Name **names,
                           size_t *count, SystemSet *selected)
{
    size_t capacity = 0;
    for (;;) {
        const Token *token = peek(p);
        if (token->kind != TOK_IDENT) {
            errorAt(p, token,
                    "expected a system after `%s` (%s), found %s",
                    keyword, SYSTEM_SELECTORS, describe(p, token));
            return false;
        }
        Name selector = nameFrom(p, token);
        SystemSet systems = systemSelector(selector.text);
        if (systems == 0) {
            const char *replacement = systemSelectorReplacement(selector.text);
            if (replacement) {
                errorAt(p, token,
                        "`%s %s` selects by package manager; use `%s %s`",
                        keyword, selector.text, keyword, replacement);
                return false;
            }
            errorAt(p, token, "unknown system `%s`; expected one of %s",
                    selector.text, SYSTEM_SELECTORS);
            return false;
        }
        advance(p);
        *names = arenaGrow(p->arena, *names, *count,
                           &capacity, sizeof **names);
        (*names)[(*count)++] = selector;
        *selected |= systems;
        if (!at(p, TOK_COMMA)) {
            break;
        }
        advance(p);
    }

    return true;
}

/*
 * Parses `on SELECTOR, ... do NEWLINE`, records the block, and narrows the
 * systems of the parser to those of the block. Stores the index of the block
 * in *index and the systems to restore at its end in *outer.
 */
static bool beginOn(Parser *p, size_t *index, SystemSet *outer)
{
    const Token *on = advance(p);
    OnBlock block = {on->line, on->column, 0, 0, NULL, 0, 0};
    SystemSet selected = 0;

    if (!parseSelectors(p, "on", &block.selectors, &block.selectorCount,
                        &selected)) {
        return false;
    }

    if (!expect(p, TOK_DO, "after the systems of `on`") ||
        !expect(p, TOK_NEWLINE, "after `do`")) {
        return false;
    }

    block.systems = p->systems & selected;
    if (block.systems == 0) {
        char enclosing[96];
        describeSystems(p->systems, enclosing, sizeof enclosing);
        if (p->manifest->selectorCount && p->systems == p->manifest->systems) {
            StrBuf requested;
            strBufInit(&requested);
            for (size_t i = 0; i < block.selectorCount; i++) {
                if (i > 0) {
                    strBufAppendStr(&requested, ", ");
                }
                strBufAppendStr(&requested, block.selectors[i].text);
            }
            errorAt(p, on, "`on %s`: the manifest is for %s",
                    requested.data, enclosing);
            strBufFree(&requested);
        } else {
            errorAt(p, on,
                    "this `on` block applies on no system: it is inside a block "
                    "for %s", enclosing);
        }
        return false;
    }

    Manifest *manifest = p->manifest;
    manifest->onBlocks = arenaGrow(p->arena, manifest->onBlocks,
                                   manifest->onCount, &p->onCapacity,
                                   sizeof *manifest->onBlocks);
    *index = manifest->onCount;
    manifest->onBlocks[manifest->onCount++] = block;
    *outer = p->systems;
    p->systems = block.systems;
    return true;
}

/* Consumes the `end` of an `on` block and restores the outer systems. */
static bool endOn(Parser *p, size_t index, SystemSet outer)
{
    const Token *end = advance(p);
    OnBlock *block = &p->manifest->onBlocks[index];
    block->endLine = end->line;
    block->endColumn = end->column;
    p->systems = outer;
    return expect(p, TOK_NEWLINE, "after `end`");
}

/* Reports `else` in an `on` block, which the language does not provide. */
static void errorElseInOn(Parser *p, const Token *token)
{
    errorAt(p, token, "`else` is not supported in `on` blocks");
}

static bool parseShell(Parser *p, ActionList *actions, size_t *capacity)
{
    const Token *shellToken = advance(p);
    if (!expect(p, TOK_DO, "after `shell`") ||
        !expect(p, TOK_NEWLINE, "after `shell do`")) {
        return false;
    }

    const Token *raw = peek(p);
    if (raw->kind != TOK_RAW_BLOCK) {
        errorAt(p, raw,
                "expected a fenced raw block (```) after `shell do`, found %s",
                describe(p, raw));
        return false;
    }
    advance(p);
    if (!expect(p, TOK_NEWLINE, "after raw block")) {
        return false;
    }

    if (at(p, TOK_RAW_BLOCK)) {
        errorAt(p, peek(p), "a `shell` block contains exactly one raw block");
        return false;
    }
    char context[96];
    snprintf(context, sizeof context, "to close `shell` block opened on line %d",
             shellToken->line);
    if (!expect(p, TOK_END, context) ||
        !expect(p, TOK_NEWLINE, "after `end`")) {
        return false;
    }

    actions->items = arenaGrow(p->arena, actions->items, actions->count,
                               capacity, sizeof *actions->items);
    Action *action = &actions->items[actions->count++];
    action->kind = ACTION_SHELL;
    action->systems = p->systems;
    action->line = shellToken->line;
    action->column = shellToken->column;
    action->fenceLine = raw->line;
    action->fenceColumn = raw->column;
    action->dialect = raw->dialect;
    action->script = raw->value;
    return true;
}

/*
 * Parses action statements into actions up to `end`, or up to `else` when
 * allowElse is set. The terminator is not consumed. The actions of nested
 * `on` blocks are added to the same list.
 */
static bool parseActionsInto(Parser *p, ActionList *actions, size_t *capacity,
                             bool allowElse, const Token *opener)
{
    for (;;) {
        skipNewlines(p);
        const Token *token = peek(p);

        switch (token->kind) {
        case TOK_END:
            return true;
        case TOK_ON: {
            size_t index;
            SystemSet outer;
            if (!beginOn(p, &index, &outer) ||
                !parseActionsInto(p, actions, capacity, false, token) ||
                !endOn(p, index, outer)) {
                return false;
            }
            break;
        }
        case TOK_ELSE:
            if (allowElse) {
                return true;
            }
            if (opener->kind == TOK_ON) {
                errorElseInOn(p, token);
            } else if (opener->kind == TOK_APPLY_IF) {
                errorAt(p, token, "`apply_if` takes at most one `else` branch");
            } else {
                errorAt(p, token,
                        "`else` is allowed only in `apply_if`, not in %s",
                        tokenKindName(opener->kind));
            }
            return false;
        case TOK_SHELL:
            if (!parseShell(p, actions, capacity)) {
                return false;
            }
            break;
        case TOK_EOF:
            errorAt(p, token,
                    "expected `end` to close %s opened on line %d, found end "
                    "of file",
                    tokenKindName(opener->kind), opener->line);
            return false;
        case TOK_RAW_BLOCK:
            errorAt(p, token,
                    "a raw block must be the body of a `shell` block "
                    "(shell do ... end)");
            return false;
        default:
            errorAt(p, token,
                    "expected `shell` block or `end` in %s body, found %s",
                    tokenKindName(opener->kind), describe(p, token));
            return false;
        }
    }
}

static bool parseActions(Parser *p, ActionList *actions, bool allowElse,
                         const Token *opener)
{
    size_t capacity = 0;
    actions->items = NULL;
    actions->count = 0;
    return parseActionsInto(p, actions, &capacity, allowElse, opener);
}

static bool parseApply(Parser *p, Stmt *stmt)
{
    const Token *opener = advance(p);
    stmt->kind = opener->kind == TOK_APPLY      ? STMT_APPLY
                 : opener->kind == TOK_APPLY_IF ? STMT_APPLY_IF
                                                : STMT_APPLY_UNLESS;

    if (opener->kind != TOK_APPLY) {
        char context[64];
        snprintf(context, sizeof context, "after %s",
                 tokenKindName(opener->kind));
        if (!parseVariable(p, &stmt->as.apply.variable, context)) {
            return false;
        }
    }

    char context[64];
    snprintf(context, sizeof context, "to begin %s body",
             tokenKindName(opener->kind));
    if (!expect(p, TOK_DO, context) ||
        !expect(p, TOK_NEWLINE, "after `do`")) {
        return false;
    }

    bool allowElse = opener->kind == TOK_APPLY_IF;
    if (!parseActions(p, &stmt->as.apply.thenActions, allowElse, opener)) {
        return false;
    }
    if (allowElse && at(p, TOK_ELSE)) {
        advance(p);
        if (!expect(p, TOK_NEWLINE, "after `else`")) {
            return false;
        }
        stmt->as.apply.hasElse = true;
        if (!parseActions(p, &stmt->as.apply.elseActions, false, opener)) {
            return false;
        }
    }

    advance(p); /* `end` */
    return expect(p, TOK_NEWLINE, "after `end`");
}

static bool parsePredicateKind(Parser *p, PredicateKind *kind,
                               bool directoryOnly)
{
    const Token *token = peek(p);
    switch (token->kind) {
    case TOK_DIRECTORY:
        *kind = PREDICATE_DIRECTORY;
        break;
    case TOK_COMMAND:
    case TOK_FILE:
        if (directoryOnly) {
            errorAt(p, token, "`create` supports only `directory`");
            return false;
        }
        *kind = token->kind == TOK_COMMAND ? PREDICATE_COMMAND : PREDICATE_FILE;
        break;
    default:
        if (directoryOnly) {
            errorAt(p, token, "expected `directory` after `create`, found %s",
                    describe(p, token));
        } else {
            errorAt(p, token,
                    "expected `command`, `file`, or `directory` after "
                    "`check`, found %s",
                    describe(p, token));
        }
        return false;
    }
    advance(p);
    return true;
}

static bool parsePackageNames(Parser *p, Name **names, size_t *count)
{
    advance(p);
    size_t capacity = 0;
    for (;;) {
        Name name;
        if (!parseUnitName(p, &name, "in `pkg`")) {
            return false;
        }
        for (size_t i = 0; i < *count; i++) {
            if (strcmp((*names)[i].text, name.text) == 0) {
                diagError(p->diag, name.line, name.column,
                          "duplicate package `%s` in list", name.text);
                return false;
            }
        }
        *names = arenaGrow(p->arena, *names, *count, &capacity, sizeof **names);
        (*names)[(*count)++] = name;
        if (!at(p, TOK_COMMA)) {
            break;
        }
        advance(p);
    }
    if (at(p, TOK_DO)) {
        errorAt(p, peek(p), "use `unit NAME do` to declare a block unit");
        return false;
    }
    return expect(p, TOK_NEWLINE, "after package list");
}

static bool parseStmt(Parser *p, Stmt *stmt)
{
    const Token *token = peek(p);
    stmt->line = token->line;
    stmt->column = token->column;

    switch (token->kind) {
    case TOK_LOCAL: {
        advance(p);
        stmt->kind = STMT_LOCAL;
        if (!parseVariable(p, &stmt->as.local.name, "after `local`") ||
            !expect(p, TOK_EQUALS, "after variable name")) {
            return false;
        }
        const Token *value = peek(p);
        if (value->kind != TOK_STRING) {
            errorAt(p, value, "expected string after '=', found %s",
                    describe(p, value));
            return false;
        }
        advance(p);
        stmt->as.local.value = value->value;
        return expect(p, TOK_NEWLINE, "after variable value");
    }

    case TOK_NEEDS: {
        advance(p);
        stmt->kind = STMT_NEEDS;
        size_t capacity = 0;
        for (;;) {
            Name name;
            if (!parseUnitName(p, &name, "in `needs`")) {
                return false;
            }
            stmt->as.needs.names =
                arenaGrow(p->arena, stmt->as.needs.names, stmt->as.needs.count,
                          &capacity, sizeof *stmt->as.needs.names);
            stmt->as.needs.names[stmt->as.needs.count++] = name;
            if (!at(p, TOK_COMMA)) {
                break;
            }
            advance(p);
        }
        return expect(p, TOK_NEWLINE, "after dependency list");
    }

    case TOK_PKG: {
        stmt->kind = STMT_PKG;
        return parsePackageNames(p, &stmt->as.pkg.names, &stmt->as.pkg.count);
    }

    case TOK_UNIT:
        errorAt(p, peek(p), "units cannot be nested");
        return false;

    case TOK_SYMLINK:
        advance(p);
        stmt->kind = STMT_SYMLINK;
        /* `tree` is contextual: ordinary packages and locals may use it. */
        if (at(p, TOK_IDENT) && peek(p)->length == 4 &&
            memcmp(peek(p)->text, "tree", 4) == 0 &&
            p->pos + 2 < p->count && p->tokens[p->pos + 2].kind == TOK_IDENT) {
            stmt->as.symlink.tree = true;
            advance(p);
        }
        if (!parseVariable(p, &stmt->as.symlink.source, "after `link`") ||
            !parseVariable(p, &stmt->as.symlink.target, "after source variable")) {
            return false;
        }
        if (at(p, TOK_IDENT) && peek(p)->length == 6 &&
            memcmp(peek(p)->text, "ignore", 6) == 0) {
            const Token *ignore = advance(p);
            if (!stmt->as.symlink.tree) {
                errorAt(p, ignore, "`ignore` requires `link tree`");
                return false;
            }
            if (!parseVariable(p, &stmt->as.symlink.ignore,
                                "after `ignore`")) {
                return false;
            }
        }
        return expect(p, TOK_NEWLINE, "after target variable");

    case TOK_CHECK:
    case TOK_CREATE: {
        bool isCreate = token->kind == TOK_CREATE;
        advance(p);
        stmt->kind = isCreate ? STMT_CREATE : STMT_CHECK;
        if (!parsePredicateKind(p, &stmt->as.predicate.kind, isCreate)) {
            return false;
        }
        const char *context = isCreate ? "after `create directory`"
                                       : "after predicate kind";
        if (!parseVariable(p, &stmt->as.predicate.variable, context)) {
            return false;
        }
        return expect(p, TOK_NEWLINE, "after variable name");
    }

    case TOK_APPLY:
    case TOK_APPLY_IF:
    case TOK_APPLY_UNLESS:
        return parseApply(p, stmt);

    case TOK_SHELL:
        errorAt(p, token,
                "`shell` blocks may appear only inside `apply`, `apply_if`, "
                "or `apply_unless`");
        return false;

    case TOK_RAW_BLOCK:
        errorAt(p, token,
                "a raw block must be the body of a `shell` block "
                "(shell do ... end)");
        return false;

    case TOK_ELSE:
        errorAt(p, token, "`else` without a matching `apply_if`");
        return false;

    default:
        errorAt(p, token, "expected a statement, found %s", describe(p, token));
        return false;
    }
}

/*
 * Parses the statements of a unit up to `end`, which is not consumed. on is
 * the `on` token of the enclosing block within the unit, or NULL for the
 * body of the unit itself. The statements of nested `on` blocks are added to
 * the unit.
 */
static bool parseUnitBody(Parser *p, Unit *unit, size_t *capacity,
                          const Token *pkg, const Token *on)
{
    for (;;) {
        skipNewlines(p);
        const Token *token = peek(p);
        if (token->kind == TOK_END) {
            return true;
        }
        if (token->kind == TOK_EOF) {
            if (on) {
                errorAt(p, token,
                        "expected `end` to close `on` opened on line %d, "
                        "found end of file",
                        on->line);
            } else {
                errorAt(p, token,
                        "expected `end` to close unit `%s` opened on line "
                        "%d, found end of file",
                        unit->name.text, pkg->line);
            }
            return false;
        }
        if (token->kind == TOK_ELSE && on) {
            errorElseInOn(p, token);
            return false;
        }
        if (token->kind == TOK_ON) {
            size_t index;
            SystemSet outer;
            if (!beginOn(p, &index, &outer) ||
                !parseUnitBody(p, unit, capacity, pkg, token) ||
                !endOn(p, index, outer)) {
                return false;
            }
            continue;
        }

        unit->stmts = arenaGrow(p->arena, unit->stmts, unit->stmtCount,
                                capacity, sizeof *unit->stmts);
        Stmt *stmt = &unit->stmts[unit->stmtCount];
        memset(stmt, 0, sizeof *stmt);
        if (!parseStmt(p, stmt)) {
            return false;
        }
        stmt->systems = p->systems;
        unit->stmtCount++;
    }
}

static bool parseUnit(Parser *p, Unit *unit)
{
    const Token *pkg = advance(p);
    if (!parseUnitName(p, &unit->name, "after unit keyword")) {
        return false;
    }

    if (!expect(p, TOK_DO, "after unit name") ||
        !expect(p, TOK_NEWLINE, "after `do`")) {
        return false;
    }
    unit->isBlock = true;

    size_t capacity = 0;
    if (!parseUnitBody(p, unit, &capacity, pkg, NULL)) {
        return false;
    }

    const Token *end = advance(p);
    unit->endLine = end->line;
    unit->endColumn = end->column;
    return expect(p, TOK_NEWLINE, "after `end`");
}

/*
 * Parses units up to the end of the file, or, within the `on` block opened
 * by on, up to its `end`, which is not consumed.
 */
static bool parseTopLevel(Parser *p, const Token *on)
{
    Manifest *manifest = p->manifest;
    for (;;) {
        skipNewlines(p);
        const Token *token = peek(p);
        switch (token->kind) {
        case TOK_EOF:
            if (on) {
                errorAt(p, token,
                        "expected `end` to close `on` opened on line %d, "
                        "found end of file",
                        on->line);
                return false;
            }
            return true;
        case TOK_END:
            if (on) {
                return true;
            }
            break;
        case TOK_ELSE:
            if (on) {
                errorElseInOn(p, token);
                return false;
            }
            break;
        case TOK_SYSTEMS: {
            if (on || manifest->selectorCount || manifest->count || manifest->onCount) {
                errorAt(p, token, "`systems` must precede the first unit");
                return false;
            }
            advance(p);
            SystemSet selected = 0;
            if (!parseSelectors(p, "systems", &manifest->selectors,
                                &manifest->selectorCount, &selected) ||
                !expect(p, TOK_NEWLINE, "after the systems declaration")) {
                return false;
            }
            manifest->systemsLine = token->line;
            manifest->systemsColumn = token->column;
            manifest->systems = selected;
            p->systems = selected;
            continue;
        }
        case TOK_ON: {
            size_t index;
            SystemSet outer;
            if (!beginOn(p, &index, &outer) || !parseTopLevel(p, token) ||
                !endOn(p, index, outer)) {
                return false;
            }
            continue;
        }
        case TOK_PKG: {
            Name *names = NULL;
            size_t count = 0;
            if (!parsePackageNames(p, &names, &count)) {
                return false;
            }
            for (size_t i = 0; i < count; i++) {
                manifest->units = arenaGrow(p->arena, manifest->units,
                        manifest->count, &p->unitCapacity,
                        sizeof *manifest->units);
                Unit *unit = &manifest->units[manifest->count++];
                memset(unit, 0, sizeof *unit);
                unit->name = names[i];
                unit->systems = p->systems;
            }
            continue;
        }
        case TOK_UNIT: {
            manifest->units =
                arenaGrow(p->arena, manifest->units, manifest->count,
                          &p->unitCapacity, sizeof *manifest->units);
            Unit *unit = &manifest->units[manifest->count];
            memset(unit, 0, sizeof *unit);
            unit->systems = p->systems;
            if (!parseUnit(p, unit)) {
                return false;
            }
            manifest->count++;
            continue;
        }
        default:
            break;
        }
        errorAt(p, token, "expected `systems`, `pkg`, `unit`, or `on` at top level, found %s",
                describe(p, token));
        return false;
    }
}

bool parseManifest(Arena *arena, const TokenList *tokens, Manifest *manifest,
                   Diag *diag)
{
    memset(manifest, 0, sizeof *manifest);
    manifest->systems = SYSTEM_ALL;
    Parser p = {
        .arena = arena,
        .diag = diag,
        .tokens = tokens->items,
        .count = tokens->count,
        .pos = 0,
        .manifest = manifest,
        .systems = SYSTEM_ALL,
    };
    return parseTopLevel(&p, NULL);
}
