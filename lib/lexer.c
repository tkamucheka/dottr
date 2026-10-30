#include "dottr/lexer.h"

#include <string.h>

typedef struct {
    Arena *arena;
    Diag *diag;
    const char *src;
    size_t length;
    size_t pos;
    size_t lineStart;
    int line;
    Token *tokens;
    size_t count;
    size_t capacity;
    Comment *comments;
    size_t commentCount;
    size_t commentCapacity;
} Lexer;

/* Accumulates the parts of a string literal or raw block. */
typedef struct {
    Arena *arena;
    TemplatePart *parts;
    size_t count;
    size_t capacity;
    StrBuf text;
    int textLine;
    int textColumn;
} TemplateBuilder;

static const struct {
    const char *word;
    TokenKind kind;
} keywords[] = {
    {"pkg", TOK_PKG},
    {"unit", TOK_UNIT},
    {"systems", TOK_SYSTEMS},
    {"do", TOK_DO},
    {"end", TOK_END},
    {"local", TOK_LOCAL},
    {"needs", TOK_NEEDS},
    {"check", TOK_CHECK},
    {"create", TOK_CREATE},
    {"apply", TOK_APPLY},
    {"apply_if", TOK_APPLY_IF},
    {"apply_unless", TOK_APPLY_UNLESS},
    {"on", TOK_ON},
    {"else", TOK_ELSE},
    {"shell", TOK_SHELL},
    {"command", TOK_COMMAND},
    {"file", TOK_FILE},
    {"directory", TOK_DIRECTORY},
    {"link", TOK_SYMLINK},
};

static const char malformedRefMessage[] =
    "malformed interpolation reference; expected @{NAME} or @{NAME.NAME...}";

static bool isBlank(char c)
{
    return c == ' ' || c == '\t';
}

static bool isIdentStart(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool isIdentChar(char c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

static bool startsRef(const char *src, size_t at, size_t end)
{
    return at + 1 < end && src[at] == '@' && src[at + 1] == '{';
}

/*
 * Scans a reference whose '@' is at src[at] and whose '{' follows it. On
 * success stores the offset of the closing '}' in *closeAt.
 */
static bool scanRef(const char *src, size_t at, size_t end, size_t *closeAt)
{
    size_t p = at + 2;
    for (;;) {
        if (p >= end || !isIdentStart(src[p])) {
            return false;
        }
        while (p < end && isIdentChar(src[p])) {
            p++;
        }
        if (p < end && src[p] == '.') {
            p++;
            continue;
        }
        break;
    }
    if (p >= end || src[p] != '}') {
        return false;
    }
    *closeAt = p;
    return true;
}

static void builderInit(TemplateBuilder *builder, Arena *arena)
{
    builder->arena = arena;
    builder->parts = NULL;
    builder->count = 0;
    builder->capacity = 0;
    builder->textLine = 0;
    builder->textColumn = 0;
    strBufInit(&builder->text);
}

static void builderAddPart(TemplateBuilder *builder, TemplatePartKind kind,
                           const char *text, size_t length, int line,
                           int column)
{
    builder->parts = arenaGrow(builder->arena, builder->parts, builder->count,
                               &builder->capacity, sizeof *builder->parts);
    TemplatePart *part = &builder->parts[builder->count++];
    part->kind = kind;
    part->text = arenaStrndup(builder->arena, text, length);
    part->length = length;
    part->line = line;
    part->column = column;
}

static void builderText(TemplateBuilder *builder, const char *text,
                        size_t length, int line, int column)
{
    if (builder->text.length == 0) {
        builder->textLine = line;
        builder->textColumn = column;
    }
    strBufAppend(&builder->text, text, length);
}

static void builderFlush(TemplateBuilder *builder)
{
    if (builder->text.length > 0) {
        builderAddPart(builder, TEMPLATE_TEXT, builder->text.data,
                       builder->text.length, builder->textLine,
                       builder->textColumn);
        strBufClear(&builder->text);
    }
}

static void builderRef(TemplateBuilder *builder, const char *path,
                       size_t length, int line, int column)
{
    builderFlush(builder);
    builderAddPart(builder, TEMPLATE_REF, path, length, line, column);
}

static Template builderFinish(TemplateBuilder *builder)
{
    builderFlush(builder);
    strBufFree(&builder->text);
    Template result = {builder->parts, builder->count};
    return result;
}

static void builderDiscard(TemplateBuilder *builder)
{
    strBufFree(&builder->text);
}

static int columnAt(const Lexer *lx, size_t offset)
{
    return (int)(offset - lx->lineStart) + 1;
}

static Token *emit(Lexer *lx, TokenKind kind, int line, int column,
                   const char *text, size_t length)
{
    lx->tokens = arenaGrow(lx->arena, lx->tokens, lx->count, &lx->capacity,
                           sizeof *lx->tokens);
    Token *token = &lx->tokens[lx->count++];
    token->kind = kind;
    token->line = line;
    token->column = column;
    token->text = text;
    token->length = length;
    return token;
}

static void emitNewline(Lexer *lx, int line, int column)
{
    if (lx->count > 0 && lx->tokens[lx->count - 1].kind != TOK_NEWLINE &&
        lx->tokens[lx->count - 1].kind != TOK_COMMA) {
        emit(lx, TOK_NEWLINE, line, column, "", 0);
    }
}

static bool lexString(Lexer *lx)
{
    const char *src = lx->src;
    size_t start = lx->pos;
    int line = lx->line;
    int column = columnAt(lx, start);
    size_t pos = start + 1;

    TemplateBuilder builder;
    builderInit(&builder, lx->arena);

    for (;;) {
        bool atLineEnd = pos >= lx->length || src[pos] == '\n' ||
                         (src[pos] == '\r' && pos + 1 < lx->length &&
                          src[pos + 1] == '\n');
        if (atLineEnd) {
            diagError(lx->diag, line, column, "unterminated string literal");
            builderDiscard(&builder);
            return false;
        }

        char c = src[pos];
        if (c == '"') {
            pos++;
            break;
        }
        if (c == '\\') {
            if (pos + 1 < lx->length &&
                (src[pos + 1] == '"' || src[pos + 1] == '\\')) {
                builderText(&builder, &src[pos + 1], 1, line,
                            columnAt(lx, pos));
                pos += 2;
                continue;
            }
            diagError(lx->diag, line, columnAt(lx, pos),
                      "unknown escape sequence; only \\\" and \\\\ are "
                      "supported");
            builderDiscard(&builder);
            return false;
        }
        if (startsRef(src, pos, lx->length)) {
            size_t closeAt;
            if (!scanRef(src, pos, lx->length, &closeAt)) {
                diagError(lx->diag, line, columnAt(lx, pos), "%s",
                          malformedRefMessage);
                builderDiscard(&builder);
                return false;
            }
            builderRef(&builder, &src[pos + 2], closeAt - (pos + 2), line,
                       columnAt(lx, pos));
            pos = closeAt + 1;
            continue;
        }
        if (c == '\0') {
            diagError(lx->diag, line, columnAt(lx, pos),
                      "NUL byte in string literal");
            builderDiscard(&builder);
            return false;
        }
        builderText(&builder, &src[pos], 1, line, columnAt(lx, pos));
        pos++;
    }

    Token *token = emit(lx, TOK_STRING, line, column, &src[start], pos - start);
    token->value = builderFinish(&builder);
    lx->pos = pos;
    return true;
}

static bool parseInfoString(Lexer *lx, size_t start, size_t end,
                            ShellDialect *dialect)
{
    const char *info = &lx->src[start];
    size_t length = end - start;

    if (length == 0) {
        *dialect = SHELL_DEFAULT;
        return true;
    }
    if (length == 2 && memcmp(info, "sh", 2) == 0) {
        *dialect = SHELL_SH;
        return true;
    }
    if (length == 4 && memcmp(info, "bash", 4) == 0) {
        *dialect = SHELL_BASH;
        return true;
    }
    if (length == 3 && memcmp(info, "zsh", 3) == 0) {
        *dialect = SHELL_ZSH;
        return true;
    }
    if (length == 4 && memcmp(info, "pwsh", 4) == 0) {
        *dialect = SHELL_PWSH;
        return true;
    }
    if (length == 10 && memcmp(info, "powershell", 10) == 0) {
        *dialect = SHELL_POWERSHELL;
        return true;
    }

    int column = columnAt(lx, start);
    if (info[0] == '`') {
        diagError(lx->diag, lx->line, column - 3,
                  "fences longer than three backticks are not supported");
    } else if (isBlank(info[0])) {
        diagError(lx->diag, lx->line, column,
                  "the info string must immediately follow the fence");
    } else {
        diagError(lx->diag, lx->line, column,
                  "unknown info string '%.*s'; expected sh, bash, zsh, "
                  "pwsh, or powershell",
                  (int)length, info);
    }
    return false;
}

/*
 * Captures a raw block whose opening fence starts at fenceAt on the current
 * line.
 */
static bool lexRawBlock(Lexer *lx, size_t fenceAt)
{
    const char *src = lx->src;
    size_t length = lx->length;
    int fenceLine = lx->line;
    int fenceColumn = columnAt(lx, fenceAt);
    const char *margin = &src[lx->lineStart];
    size_t marginLength = fenceAt - lx->lineStart;

    size_t fenceEnd = fenceAt + 3;
    while (fenceEnd < length && src[fenceEnd] != '\n') {
        fenceEnd++;
    }
    size_t infoEnd = fenceEnd;
    while (infoEnd > fenceAt + 3 &&
           (isBlank(src[infoEnd - 1]) || src[infoEnd - 1] == '\r')) {
        infoEnd--;
    }
    ShellDialect dialect;
    if (!parseInfoString(lx, fenceAt + 3, infoEnd, &dialect)) {
        return false;
    }

    TemplateBuilder builder;
    builderInit(&builder, lx->arena);

    size_t pos = fenceEnd < length ? fenceEnd + 1 : length;
    int line = fenceLine + 1;
    bool closed = false;
    int closingLine = 0;
    int closingColumn = 0;

    while (pos < length) {
        size_t lineStart = pos;
        size_t lineEnd = lineStart;
        while (lineEnd < length && src[lineEnd] != '\n') {
            lineEnd++;
        }
        size_t next = lineEnd < length ? lineEnd + 1 : length;
        size_t contentEnd = lineEnd;
        if (contentEnd > lineStart && src[contentEnd - 1] == '\r') {
            contentEnd--;
        }

        size_t trimStart = lineStart;
        while (trimStart < contentEnd && isBlank(src[trimStart])) {
            trimStart++;
        }
        size_t trimEnd = contentEnd;
        while (trimEnd > trimStart && isBlank(src[trimEnd - 1])) {
            trimEnd--;
        }

        if (trimEnd - trimStart == 3 && memcmp(&src[trimStart], "```", 3) == 0) {
            closed = true;
            closingLine = line;
            closingColumn = (int)(trimStart - lineStart) + 1;
            pos = next;
            break;
        }

        if (trimStart == trimEnd) {
            builderText(&builder, "\n", 1, line, 1);
        } else if (contentEnd - lineStart >= marginLength &&
                   memcmp(&src[lineStart], margin, marginLength) == 0) {
            /* Raw block contents are not interpolated. */
            size_t q = lineStart + marginLength;
            const char *nul = memchr(&src[q], '\0', contentEnd - q);
            if (nul) {
                diagError(lx->diag, line, (int)(nul - &src[lineStart]) + 1,
                          "NUL byte in raw block");
                builderDiscard(&builder);
                return false;
            }
            builderText(&builder, &src[q], contentEnd - q, line,
                        (int)(q - lineStart) + 1);
            builderText(&builder, "\n", 1, line, (int)(contentEnd - lineStart) + 1);
        } else {
            diagError(lx->diag, line, 1,
                      "line does not begin with the indentation of the "
                      "opening fence on line %d (is the closing ``` "
                      "missing?)",
                      fenceLine);
            builderDiscard(&builder);
            return false;
        }

        line++;
        pos = next;
    }

    if (!closed) {
        diagError(lx->diag, fenceLine, fenceColumn,
                  "unterminated raw block; expected a closing ``` line");
        builderDiscard(&builder);
        return false;
    }

    Token *token = emit(lx, TOK_RAW_BLOCK, fenceLine, fenceColumn,
                        &src[fenceAt], infoEnd - fenceAt);
    token->value = builderFinish(&builder);
    token->dialect = dialect;
    emit(lx, TOK_NEWLINE, closingLine, closingColumn + 3, "", 0);

    lx->pos = pos;
    lx->line = closingLine + 1;
    lx->lineStart = pos;
    return true;
}

static void lexWord(Lexer *lx)
{
    size_t start = lx->pos;
    while (lx->pos < lx->length && isIdentChar(lx->src[lx->pos])) {
        lx->pos++;
    }
    size_t length = lx->pos - start;

    TokenKind kind = TOK_IDENT;
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strlen(keywords[i].word) == length &&
            memcmp(keywords[i].word, &lx->src[start], length) == 0) {
            kind = keywords[i].kind;
            break;
        }
    }
    emit(lx, kind, lx->line, columnAt(lx, start), &lx->src[start], length);
}

static void reportUnexpected(Lexer *lx)
{
    size_t pos = lx->pos;
    char c = lx->src[pos];
    int column = columnAt(lx, pos);
    const Token *previous = lx->count > 0 ? &lx->tokens[lx->count - 1] : NULL;

    bool continuesWord = previous && previous->kind != TOK_NEWLINE &&
                         previous->kind != TOK_STRING &&
                         previous->kind != TOK_EQUALS &&
                         previous->kind != TOK_COMMA &&
                         previous->text + previous->length == &lx->src[pos];
    if (continuesWord && (c == '-' || c == '+' || c == '.')) {
        const char *word = previous->text;
        size_t length = 0;
        while (&word[length] < lx->src + lx->length) {
            char w = word[length];
            if (isBlank(w) || w == '\n' || w == '\r' || w == ',' || w == '#' ||
                w == '"') {
                break;
            }
            length++;
        }
        diagError(lx->diag, lx->line, column,
                  "unexpected character '%c'; a unit name containing "
                  "characters other than letters, digits, and '_' must be "
                  "written as a string, for example \"%.*s\"",
                  c, (int)length, word);
        return;
    }
    if (c == '`') {
        diagError(lx->diag, lx->line, column,
                  "unexpected character '`'; a fence must be on a line of "
                  "its own");
        return;
    }
    if (c >= 0x21 && c <= 0x7e) {
        diagError(lx->diag, lx->line, column, "unexpected character '%c'", c);
    } else {
        diagError(lx->diag, lx->line, column, "unexpected byte 0x%02X",
                  (unsigned)(unsigned char)c);
    }
}

bool lexSource(Arena *arena, const char *source, size_t length,
               TokenList *tokens, Diag *diag)
{
    Lexer lx = {
        .arena = arena,
        .diag = diag,
        .src = source,
        .length = length,
        .pos = 0,
        .lineStart = 0,
        .line = 1,
        .tokens = NULL,
        .count = 0,
        .capacity = 0,
        .comments = NULL,
        .commentCount = 0,
        .commentCapacity = 0,
    };
    bool atLineStart = true;

    while (lx.pos < lx.length) {
        if (atLineStart) {
            atLineStart = false;
            size_t p = lx.pos;
            while (p < lx.length && isBlank(source[p])) {
                p++;
            }
            if (lx.length - p >= 3 && memcmp(&source[p], "```", 3) == 0) {
                if (!lexRawBlock(&lx, p)) {
                    return false;
                }
                atLineStart = true;
                continue;
            }
        }

        char c = source[lx.pos];
        if (isBlank(c)) {
            lx.pos++;
        } else if (c == '\r' && lx.pos + 1 < lx.length &&
                   source[lx.pos + 1] == '\n') {
            lx.pos++;
        } else if (c == '\n') {
            emitNewline(&lx, lx.line, columnAt(&lx, lx.pos));
            lx.pos++;
            lx.line++;
            lx.lineStart = lx.pos;
            atLineStart = true;
        } else if (c == '#') {
            size_t start = lx.pos;
            while (lx.pos < lx.length && source[lx.pos] != '\n') {
                lx.pos++;
            }
            size_t end = lx.pos;
            if (end > start && source[end - 1] == '\r') {
                end--;
            }
            lx.comments = arenaGrow(lx.arena, lx.comments, lx.commentCount,
                                    &lx.commentCapacity, sizeof *lx.comments);
            lx.comments[lx.commentCount++] = (Comment){
                lx.line, columnAt(&lx, start), &source[start], end - start};
        } else if (c == '"') {
            if (!lexString(&lx)) {
                return false;
            }
        } else if (isIdentStart(c)) {
            lexWord(&lx);
        } else if (c == '=') {
            emit(&lx, TOK_EQUALS, lx.line, columnAt(&lx, lx.pos),
                 &source[lx.pos], 1);
            lx.pos++;
        } else if (c == ',') {
            emit(&lx, TOK_COMMA, lx.line, columnAt(&lx, lx.pos),
                 &source[lx.pos], 1);
            lx.pos++;
        } else {
            reportUnexpected(&lx);
            return false;
        }
    }

    emitNewline(&lx, lx.line, columnAt(&lx, lx.pos));
    emit(&lx, TOK_EOF, lx.line, columnAt(&lx, lx.pos), "", 0);
    tokens->items = lx.tokens;
    tokens->count = lx.count;
    tokens->comments = lx.comments;
    tokens->commentCount = lx.commentCount;
    return true;
}

const char *tokenKindName(TokenKind kind)
{
    switch (kind) {
    case TOK_EOF: return "end of file";
    case TOK_NEWLINE: return "end of line";
    case TOK_IDENT: return "identifier";
    case TOK_STRING: return "string";
    case TOK_RAW_BLOCK: return "raw block";
    case TOK_EQUALS: return "'='";
    case TOK_COMMA: return "','";
    case TOK_PKG: return "`pkg`";
    case TOK_UNIT: return "`unit`";
    case TOK_SYSTEMS: return "`systems`";
    case TOK_DO: return "`do`";
    case TOK_END: return "`end`";
    case TOK_LOCAL: return "`local`";
    case TOK_NEEDS: return "`needs`";
    case TOK_CHECK: return "`check`";
    case TOK_CREATE: return "`create`";
    case TOK_APPLY: return "`apply`";
    case TOK_APPLY_IF: return "`apply_if`";
    case TOK_APPLY_UNLESS: return "`apply_unless`";
    case TOK_ON: return "`on`";
    case TOK_ELSE: return "`else`";
    case TOK_SHELL: return "`shell`";
    case TOK_COMMAND: return "`command`";
    case TOK_FILE: return "`file`";
    case TOK_DIRECTORY: return "`directory`";
    case TOK_SYMLINK: return "`link`";
    }
    return "token";
}

const char *shellDialectInterpreter(ShellDialect dialect, bool windows)
{
    switch (dialect) {
    case SHELL_DEFAULT: return windows ? "powershell" : "sh";
    case SHELL_SH: return "sh";
    case SHELL_BASH: return "bash";
    case SHELL_ZSH: return "zsh";
    case SHELL_PWSH: return "pwsh";
    case SHELL_POWERSHELL: return "powershell";
    }
    return "sh";
}

bool shellDialectIsPowerShell(ShellDialect dialect, bool windows)
{
    return dialect == SHELL_PWSH || dialect == SHELL_POWERSHELL ||
           (dialect == SHELL_DEFAULT && windows);
}
