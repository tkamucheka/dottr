#include "dottr/format.h"

#include <string.h>

#include "dottr/lexer.h"
#include "dottr/parser.h"

#define INDENT_WIDTH 2

typedef enum {
    LINE_CODE,
    LINE_RAW,
    LINE_COMMENT,
} LineKind;

/* One output line, or several for a raw block. */
typedef struct {
    LineKind kind;
    /* Source lines spanned. */
    int firstLine;
    int lastLine;
    /* Tokens [first, end) of a code line or raw block. */
    size_t first;
    size_t end;
    /* The comment of a comment line, or the trailing comment of a code line. */
    const Comment *comment;
} Line;

typedef struct {
    Arena *arena;
    Line *items;
    size_t count;
    size_t capacity;
} LineList;

static void addLine(LineList *lines, Line line)
{
    lines->items = arenaGrow(lines->arena, lines->items, lines->count,
                             &lines->capacity, sizeof *lines->items);
    lines->items[lines->count++] = line;
}

/*
 * Length of a comment without trailing whitespace. A carriage return counts
 * as whitespace: once printed before a line feed, it would be read as part of
 * the line end.
 */
static size_t commentLength(const Comment *comment)
{
    size_t length = comment->length;
    while (length > 0 && (comment->text[length - 1] == ' ' ||
                          comment->text[length - 1] == '\t' ||
                          comment->text[length - 1] == '\r')) {
        length--;
    }
    return length;
}

/* Groups tokens into code lines and raw blocks. */
static void collectCodeLines(const TokenList *tokens, LineList *lines)
{
    size_t i = 0;
    while (i < tokens->count) {
        const Token *token = &tokens->items[i];
        if (token->kind == TOK_NEWLINE || token->kind == TOK_EOF) {
            i++;
            continue;
        }
        if (token->kind == TOK_RAW_BLOCK) {
            /* The NEWLINE after a raw block is on the closing fence line. */
            int lastLine = tokens->items[i + 1].line;
            addLine(lines, (Line){LINE_RAW, token->line, lastLine, i, i + 1,
                                  NULL});
            i++;
            continue;
        }
        size_t first = i;
        while (i < tokens->count && tokens->items[i].kind != TOK_NEWLINE &&
               tokens->items[i].kind != TOK_EOF) {
            i++;
        }
        addLine(lines, (Line){LINE_CODE, token->line, token->line, first, i,
                              NULL});
    }
}

/*
 * Merges comments into the code lines by source line: a comment on the line
 * of a code line becomes its trailing comment; any other comment becomes a
 * line of its own.
 */
static void mergeComments(const TokenList *tokens, const LineList *code,
                          LineList *lines)
{
    size_t c = 0;
    for (size_t i = 0; i < code->count; i++) {
        Line line = code->items[i];
        while (c < tokens->commentCount &&
               tokens->comments[c].line < line.firstLine) {
            const Comment *comment = &tokens->comments[c++];
            addLine(lines, (Line){LINE_COMMENT, comment->line, comment->line,
                                  0, 0, comment});
        }
        if (line.kind == LINE_CODE && c < tokens->commentCount &&
            tokens->comments[c].line == line.firstLine) {
            line.comment = &tokens->comments[c++];
        }
        addLine(lines, line);
    }
    while (c < tokens->commentCount) {
        const Comment *comment = &tokens->comments[c++];
        addLine(lines, (Line){LINE_COMMENT, comment->line, comment->line, 0, 0,
                              comment});
    }
}

static void appendIndent(StrBuf *out, int level)
{
    for (int i = 0; i < level * INDENT_WIDTH; i++) {
        strBufAppendChar(out, ' ');
    }
}

static void appendCodeLine(StrBuf *out, const TokenList *tokens,
                           const Line *line)
{
    for (size_t i = line->first; i < line->end; i++) {
        const Token *token = &tokens->items[i];
        if (i > line->first && token->kind != TOK_COMMA) {
            strBufAppendChar(out, ' ');
        }
        strBufAppend(out, token->text, token->length);
    }
    if (line->comment) {
        strBufAppendChar(out, ' ');
        strBufAppend(out, line->comment->text, commentLength(line->comment));
    }
    strBufAppendChar(out, '\n');
}

/* Prints a raw block with its fences and lines at the given level. */
static void appendRawBlock(StrBuf *out, const Token *raw, int level)
{
    appendIndent(out, level);
    strBufAppend(out, raw->text, raw->length);
    strBufAppendChar(out, '\n');

    StrBuf text;
    strBufInit(&text);
    for (size_t i = 0; i < raw->value.count; i++) {
        strBufAppend(&text, raw->value.parts[i].text,
                     raw->value.parts[i].length);
    }
    const char *cursor = text.data;
    const char *limit = text.data + text.length;
    while (cursor < limit) {
        const char *newline = memchr(cursor, '\n', (size_t)(limit - cursor));
        size_t length = newline ? (size_t)(newline - cursor)
                                : (size_t)(limit - cursor);
        if (length > 0) {
            appendIndent(out, level);
            strBufAppend(out, cursor, length);
        }
        strBufAppendChar(out, '\n');
        cursor += length + 1;
    }
    strBufFree(&text);

    appendIndent(out, level);
    strBufAppendStr(out, "```\n");
}

static void printLines(StrBuf *out, const TokenList *tokens,
                       const LineList *lines)
{
    int depth = 0;
    bool previousOpens = false;
    const Line *previous = NULL;

    for (size_t i = 0; i < lines->count; i++) {
        const Line *line = &lines->items[i];
        TokenKind firstKind = TOK_EOF;
        TokenKind lastKind = TOK_EOF;
        if (line->kind == LINE_CODE) {
            firstKind = tokens->items[line->first].kind;
            lastKind = tokens->items[line->end - 1].kind;
        }
        bool closes = firstKind == TOK_END || firstKind == TOK_ELSE;

        if (firstKind == TOK_END) {
            depth--;
        }
        if (previous && line->firstLine > previous->lastLine + 1 &&
            !previousOpens && !closes) {
            strBufAppendChar(out, '\n');
        }

        if (line->kind == LINE_RAW) {
            appendRawBlock(out, &tokens->items[line->first], depth);
        } else if (line->kind == LINE_COMMENT) {
            appendIndent(out, depth);
            strBufAppend(out, line->comment->text,
                         commentLength(line->comment));
            strBufAppendChar(out, '\n');
        } else {
            appendIndent(out, firstKind == TOK_ELSE ? depth - 1 : depth);
            appendCodeLine(out, tokens, line);
        }

        if (lastKind == TOK_DO) {
            depth++;
        }
        previousOpens = lastKind == TOK_DO || firstKind == TOK_ELSE;
        previous = line;
    }
}

bool formatManifest(const char *source, size_t length, StrBuf *out,
                    Diag *diag)
{
    Arena arena;
    arenaInit(&arena);

    TokenList tokens;
    Manifest manifest;
    bool ok = lexSource(&arena, source, length, &tokens, diag) &&
              parseManifest(&arena, &tokens, &manifest, diag);
    if (ok) {
        LineList code = {&arena, NULL, 0, 0};
        LineList lines = {&arena, NULL, 0, 0};
        collectCodeLines(&tokens, &code);
        mergeComments(&tokens, &code, &lines);
        printLines(out, &tokens, &lines);
    }

    arenaFree(&arena);
    return ok;
}

static bool sameRawBlock(const Token *a, const Token *b)
{
    if (a->dialect != b->dialect) {
        return false;
    }
    StrBuf textA;
    StrBuf textB;
    strBufInit(&textA);
    strBufInit(&textB);
    for (size_t i = 0; i < a->value.count; i++) {
        strBufAppend(&textA, a->value.parts[i].text, a->value.parts[i].length);
    }
    for (size_t i = 0; i < b->value.count; i++) {
        strBufAppend(&textB, b->value.parts[i].text, b->value.parts[i].length);
    }
    bool same = textA.length == textB.length &&
                memcmp(textA.data, textB.data, textA.length) == 0;
    strBufFree(&textA);
    strBufFree(&textB);
    return same;
}

bool formatEquivalent(const char *a, size_t aLength, const char *b,
                      size_t bLength)
{
    Arena arena;
    arenaInit(&arena);
    Diag diag;
    diagInit(&diag, "", NULL);

    TokenList x;
    TokenList y;
    bool same = lexSource(&arena, a, aLength, &x, &diag) &&
                lexSource(&arena, b, bLength, &y, &diag) &&
                x.count == y.count && x.commentCount == y.commentCount;

    for (size_t i = 0; same && i < x.count; i++) {
        const Token *s = &x.items[i];
        const Token *t = &y.items[i];
        if (s->kind != t->kind) {
            same = false;
        } else if (s->kind == TOK_RAW_BLOCK) {
            same = sameRawBlock(s, t);
        } else {
            same = s->length == t->length &&
                   memcmp(s->text, t->text, s->length) == 0;
        }
    }
    for (size_t i = 0; same && i < x.commentCount; i++) {
        size_t length = commentLength(&x.comments[i]);
        same = length == commentLength(&y.comments[i]) &&
               memcmp(x.comments[i].text, y.comments[i].text, length) == 0;
    }

    arenaFree(&arena);
    return same;
}
