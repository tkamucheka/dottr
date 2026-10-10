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
    action->line = shellToken->line;
    action->column = shellToken->column;
    action->dialect = raw->dialect;
    action->script = raw->value;
    return true;
}

/*
 * Parses action statements up to `end`, or up to `else` when allowElse is
 * set. The terminator is not consumed.
 */
static bool parseActions(Parser *p, ActionList *actions, bool allowElse,
                         const Token *opener)
{
    size_t capacity = 0;
    actions->items = NULL;
    actions->count = 0;

    for (;;) {
        skipNewlines(p);
        const Token *token = peek(p);

        switch (token->kind) {
        case TOK_END:
            return true;
        case TOK_ELSE:
            if (allowElse) {
                return true;
            }
            if (opener->kind == TOK_APPLY_IF) {
                errorAt(p, token, "`apply_if` takes at most one `else` branch");
            } else {
                errorAt(p, token,
                        "`else` is allowed only in `apply_if`, not in %s",
                        tokenKindName(opener->kind));
            }
            return false;
        case TOK_SHELL:
            if (!parseShell(p, actions, &capacity)) {
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

static bool parseStmt(Parser *p, const Unit *unit, Stmt *stmt)
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
        advance(p);
        stmt->kind = STMT_CHILD_PKG;
        Name *package = &stmt->as.childPkg.package;
        if (!parseUnitName(p, package, "after `pkg`")) {
            return false;
        }
        if (at(p, TOK_DO)) {
            errorAt(p, peek(p),
                    "block-form units cannot be nested; only `pkg NAME` is "
                    "allowed inside a unit");
            return false;
        }
        size_t length = strlen(unit->name.text) + strlen("-child-") +
                        strlen(package->text);
        char *childName = arenaAlloc(p->arena, length + 1);
        snprintf(childName, length + 1, "%s-child-%s", unit->name.text,
                 package->text);
        stmt->as.childPkg.childName = childName;
        return expect(p, TOK_NEWLINE, "after package name");
    }

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

static bool parseUnit(Parser *p, Unit *unit)
{
    const Token *pkg = advance(p);
    if (!parseUnitName(p, &unit->name, "after `pkg`")) {
        return false;
    }

    if (!at(p, TOK_DO)) {
        unit->isBlock = false;
        return expect(p, TOK_NEWLINE, "after unit name");
    }

    advance(p);
    if (!expect(p, TOK_NEWLINE, "after `do`")) {
        return false;
    }
    unit->isBlock = true;

    size_t capacity = 0;
    for (;;) {
        skipNewlines(p);
        if (at(p, TOK_END)) {
            break;
        }
        if (at(p, TOK_EOF)) {
            errorAt(p, peek(p),
                    "expected `end` to close unit `%s` opened on line %d, "
                    "found end of file",
                    unit->name.text, pkg->line);
            return false;
        }
        unit->stmts = arenaGrow(p->arena, unit->stmts, unit->stmtCount,
                                &capacity, sizeof *unit->stmts);
        Stmt *stmt = &unit->stmts[unit->stmtCount];
        memset(stmt, 0, sizeof *stmt);
        if (!parseStmt(p, unit, stmt)) {
            return false;
        }
        unit->stmtCount++;
    }

    advance(p); /* `end` */
    return expect(p, TOK_NEWLINE, "after `end`");
}

bool parseManifest(Arena *arena, const TokenList *tokens, Manifest *manifest,
                   Diag *diag)
{
    Parser p = {
        .arena = arena,
        .diag = diag,
        .tokens = tokens->items,
        .count = tokens->count,
        .pos = 0,
    };
    manifest->units = NULL;
    manifest->count = 0;
    size_t capacity = 0;

    for (;;) {
        skipNewlines(&p);
        const Token *token = peek(&p);
        if (token->kind == TOK_EOF) {
            return true;
        }
        if (token->kind != TOK_PKG) {
            errorAt(&p, token, "expected `pkg` at top level, found %s",
                    describe(&p, token));
            return false;
        }

        manifest->units = arenaGrow(arena, manifest->units, manifest->count,
                                    &capacity, sizeof *manifest->units);
        Unit *unit = &manifest->units[manifest->count];
        memset(unit, 0, sizeof *unit);
        if (!parseUnit(&p, unit)) {
            return false;
        }
        manifest->count++;
    }
}
