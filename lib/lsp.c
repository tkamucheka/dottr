#include "dottr/lsp.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/ast.h"
#include "dottr/diag.h"
#include "dottr/format.h"
#include "dottr/host.h"
#include "dottr/json.h"
#include "dottr/lexer.h"
#include "dottr/parser.h"
#include "dottr/sema.h"
#include "dottr/util.h"

/* Largest message accepted, to bound allocation. */
#define MAX_MESSAGE (64u * 1024u * 1024u)

/* JSON-RPC and LSP error codes. */
#define ERROR_PARSE (-32700)
#define ERROR_INVALID_REQUEST (-32600)
#define ERROR_METHOD_NOT_FOUND (-32601)
#define ERROR_INVALID_PARAMS (-32602)
#define ERROR_NOT_INITIALIZED (-32002)
#define ERROR_REQUEST_FAILED (-32803)

/* LSP enumerations. */
#define SYMBOL_MODULE 2
#define SYMBOL_PACKAGE 4
#define SYMBOL_VARIABLE 13
#define COMPLETION_VARIABLE 6
#define COMPLETION_MODULE 9
#define COMPLETION_KEYWORD 14
#define COMPLETION_SNIPPET 15
#define COMPLETION_ENUM_MEMBER 20
#define COMPLETION_CONSTANT 21

/* ---------------------------------------------------------------------- */
/* Documents                                                              */
/* ---------------------------------------------------------------------- */

typedef struct {
    int line;
    int column;
    const char *message;
} Problem;

typedef enum {
    OCC_UNIT_DECL,
    OCC_UNIT_REF,
    OCC_VAR_DECL,
    OCC_VAR_REF,
    /* $NAME or ${NAME} in a shell block, naming a local of the unit. */
    OCC_VAR_SHELL,
    OCC_BUILTIN,
    OCC_PACKAGE,
} OccurrenceKind;

/* A declaration or use of a name, located in the document text. */
typedef struct {
    OccurrenceKind kind;
    /* Bytes [start, end): the whole occurrence, including quotes or @{ }. */
    size_t start;
    size_t end;
    /* Bytes [nameStart, nameEnd): the name alone. */
    size_t nameStart;
    size_t nameEnd;
    const char *name;
    /*
     * OCC_UNIT_DECL and OCC_UNIT_REF: the unit named, or NULL if it is not
     * declared. Otherwise: the enclosing unit.
     */
    Unit *unit;
    /* OCC_VAR_DECL and OCC_VAR_REF: the declaring local, or NULL. */
    const Stmt *local;
    /* The statement containing the occurrence; NULL for unit declarations. */
    const Stmt *stmt;
} Occurrence;

typedef struct {
    char *uri;
    char *text;
    size_t length;
    size_t *lineStarts;
    size_t lineCount;

    /* Analysis of text, allocated in arena. */
    Arena arena;
    bool lexed;
    bool parsed;
    TokenList tokens;
    Manifest manifest;
    Problem *problems;
    size_t problemCount;
    size_t problemCapacity;
    Occurrence *occurrences;
    size_t occurrenceCount;
    size_t occurrenceCapacity;
} Document;

typedef struct {
    FILE *in;
    FILE *out;
    bool initialized;
    bool shutdown;
    /* Position encoding: UTF-8 if true, otherwise UTF-16. */
    bool utf8;
    bool snippets;
    bool markdown;
    Document **documents;
    size_t documentCount;
} Server;

static bool isWordStart(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool isWordChar(char c)
{
    return isWordStart(c) || (c >= '0' && c <= '9');
}

static bool isBlank(char c)
{
    return c == ' ' || c == '\t';
}

static void indexLines(Document *doc)
{
    size_t count = 1;
    for (size_t i = 0; i < doc->length; i++) {
        if (doc->text[i] == '\n') {
            count++;
        }
    }
    free(doc->lineStarts);
    doc->lineStarts = xmalloc(count * sizeof *doc->lineStarts);
    doc->lineCount = 0;
    doc->lineStarts[doc->lineCount++] = 0;
    for (size_t i = 0; i < doc->length; i++) {
        if (doc->text[i] == '\n') {
            doc->lineStarts[doc->lineCount++] = i + 1;
        }
    }
}

/* Line (0-based) containing a byte offset. */
static size_t lineOf(const Document *doc, size_t offset)
{
    size_t low = 0;
    size_t high = doc->lineCount;
    while (high - low > 1) {
        size_t mid = low + (high - low) / 2;
        if (doc->lineStarts[mid] <= offset) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

/* Offset of the end of a line (0-based), before its line terminator. */
static size_t lineEnd(const Document *doc, size_t line)
{
    size_t end = line + 1 < doc->lineCount ? doc->lineStarts[line + 1] - 1
                                           : doc->length;
    if (end > doc->lineStarts[line] && doc->text[end - 1] == '\r') {
        end--;
    }
    return end;
}

/* Offset of a dottr position (1-based line and byte column). */
static size_t offsetAt(const Document *doc, int line, int column)
{
    if (line < 1) {
        return 0;
    }
    if ((size_t)line > doc->lineCount) {
        return doc->length;
    }
    size_t start = doc->lineStarts[line - 1];
    size_t offset = start + (size_t)(column > 0 ? column - 1 : 0);
    size_t end = lineEnd(doc, (size_t)line - 1);
    return offset < end ? offset : end;
}

/* Offset of the first non-blank character of the line containing offset. */
static size_t lineIndentEnd(const Document *doc, size_t offset)
{
    size_t line = lineOf(doc, offset);
    size_t i = doc->lineStarts[line];
    size_t end = lineEnd(doc, line);
    while (i < end && isBlank(doc->text[i])) {
        i++;
    }
    return i;
}

/*
 * End of the lexeme starting at offset, within its line: a string literal,
 * an interpolation reference, a word, a fence, or one character.
 */
static size_t lexemeEnd(const Document *doc, size_t start)
{
    const char *text = doc->text;
    if (start >= doc->length) {
        return doc->length;
    }
    size_t end = lineEnd(doc, lineOf(doc, start));
    if (start >= end) {
        return start;
    }
    size_t i = start;
    if (text[i] == '"') {
        for (i++; i < end; i++) {
            if (text[i] == '\\' && i + 1 < end) {
                i++;
            } else if (text[i] == '"') {
                return i + 1;
            }
        }
        return end;
    }
    if (text[i] == '@' && i + 1 < end && text[i + 1] == '{') {
        while (i < end && text[i] != '}') {
            i++;
        }
        return i < end ? i + 1 : end;
    }
    if (isWordChar(text[i])) {
        while (i < end && isWordChar(text[i])) {
            i++;
        }
        return i;
    }
    if (text[i] == '`') {
        while (i < end && text[i] == '`') {
            i++;
        }
        return i;
    }
    return i + 1;
}

/* ---------------------------------------------------------------------- */
/* Positions                                                              */
/* ---------------------------------------------------------------------- */

/* Width of the character starting with byte c, in the position encoding. */
static int unitWidth(const Server *s, unsigned char c)
{
    if (s->utf8) {
        return 1;
    }
    if ((c & 0xC0) == 0x80) {
        return 0;
    }
    return c >= 0xF0 ? 2 : 1;
}

static void writePosition(StrBuf *out, const Server *s, const Document *doc,
                          size_t offset)
{
    size_t line = lineOf(doc, offset);
    int character = 0;
    for (size_t i = doc->lineStarts[line]; i < offset; i++) {
        character += unitWidth(s, (unsigned char)doc->text[i]);
    }
    char buf[64];
    snprintf(buf, sizeof buf, "{\"line\":%zu,\"character\":%d}", line,
             character);
    strBufAppendStr(out, buf);
}

static void writeRange(StrBuf *out, const Server *s, const Document *doc,
                       size_t start, size_t end)
{
    strBufAppendStr(out, "{\"start\":");
    writePosition(out, s, doc, start);
    strBufAppendStr(out, ",\"end\":");
    writePosition(out, s, doc, end);
    strBufAppendChar(out, '}');
}

static void writeLocation(StrBuf *out, const Server *s, const Document *doc,
                          size_t start, size_t end)
{
    strBufAppendStr(out, "{\"uri\":");
    jsonWriteString(out, doc->uri, strlen(doc->uri));
    strBufAppendStr(out, ",\"range\":");
    writeRange(out, s, doc, start, end);
    strBufAppendChar(out, '}');
}

/* Offset of an LSP Position object, clamped to the document. */
static size_t offsetOfPosition(const Server *s, const Document *doc,
                               const JsonValue *position)
{
    int line = jsonInt(jsonGet(position, "line"), 0);
    int character = jsonInt(jsonGet(position, "character"), 0);
    if (line < 0) {
        return 0;
    }
    if ((size_t)line >= doc->lineCount) {
        return doc->length;
    }
    size_t i = doc->lineStarts[line];
    size_t end = lineEnd(doc, (size_t)line);
    int units = 0;
    while (i < end && units < character) {
        units += unitWidth(s, (unsigned char)doc->text[i]);
        i++;
        while (!s->utf8 && i < end &&
               ((unsigned char)doc->text[i] & 0xC0) == 0x80) {
            i++;
        }
    }
    return i;
}

/* ---------------------------------------------------------------------- */
/* Analysis                                                               */
/* ---------------------------------------------------------------------- */

static void collectProblem(void *context, int line, int column,
                           const char *message)
{
    Document *doc = context;
    doc->problems = arenaGrow(&doc->arena, doc->problems, doc->problemCount,
                              &doc->problemCapacity, sizeof *doc->problems);
    doc->problems[doc->problemCount++] =
        (Problem){line, column, arenaStrdup(&doc->arena, message)};
}

static void addOccurrence(Document *doc, Occurrence occurrence)
{
    doc->occurrences =
        arenaGrow(&doc->arena, doc->occurrences, doc->occurrenceCount,
                  &doc->occurrenceCapacity, sizeof *doc->occurrences);
    doc->occurrences[doc->occurrenceCount++] = occurrence;
}

/* The first local of a unit binding name, or NULL. */
static const Stmt *findLocal(const Unit *unit, const char *name)
{
    for (size_t i = 0; i < unit->stmtCount; i++) {
        const Stmt *stmt = &unit->stmts[i];
        if (stmt->kind == STMT_LOCAL &&
            strcmp(stmt->as.local.name.text, name) == 0) {
            return stmt;
        }
    }
    return NULL;
}

/* Byte range of a name as written: an identifier or a string literal. */
static void nameRange(const Document *doc, const Name *name, size_t *start,
                      size_t *end, size_t *nameStart, size_t *nameEnd)
{
    *start = offsetAt(doc, name->line, name->column);
    *end = lexemeEnd(doc, *start);
    *nameStart = *start;
    *nameEnd = *end;
    if (*start < doc->length && doc->text[*start] == '"') {
        *nameStart = *start + 1;
        *nameEnd = *end > *nameStart && doc->text[*end - 1] == '"' ? *end - 1
                                                                   : *end;
    }
}

static void addName(Document *doc, OccurrenceKind kind, const Name *name,
                    Unit *unit, const Stmt *local, const Stmt *stmt)
{
    Occurrence occurrence = {kind, 0, 0, 0, 0, name->text, unit, local, stmt};
    nameRange(doc, name, &occurrence.start, &occurrence.end,
              &occurrence.nameStart, &occurrence.nameEnd);
    addOccurrence(doc, occurrence);
}

static void addReferences(Document *doc, const Template *value, Unit *unit,
                          const Stmt *stmt)
{
    for (size_t i = 0; i < value->count; i++) {
        const TemplatePart *part = &value->parts[i];
        if (part->kind != TEMPLATE_REF) {
            continue;
        }
        size_t start = offsetAt(doc, part->line, part->column);
        bool builtin = strchr(part->text, '.') != NULL;
        Occurrence occurrence = {
            builtin ? OCC_BUILTIN : OCC_VAR_REF,
            start,
            start + part->length + 3,
            start + 2,
            start + 2 + part->length,
            part->text,
            unit,
            builtin ? NULL : findLocal(unit, part->text),
            stmt,
        };
        addOccurrence(doc, occurrence);
    }
}

/*
 * Adds the uses of the unit's locals in the text of a shell block: $NAME and
 * ${NAME...}, where NAME is a local bound before the block. Text in single
 * quotes and escaped dollar signs are skipped, as the shell does not expand
 * them; the scan is per line and does not otherwise parse the shell text.
 */
static void addShellUses(Document *doc, Unit *unit, const Stmt *stmt,
                         const Action *action)
{
    StrBuf text;
    strBufInit(&text);
    for (size_t i = 0; i < action->script.count; i++) {
        strBufAppend(&text, action->script.parts[i].text,
                     action->script.parts[i].length);
    }
    size_t margin = (size_t)(action->fenceColumn - 1);
    int sourceLine = action->fenceLine + 1;
    const char *line = text.data;
    const char *limit = text.data + text.length;

    while (line && line < limit) {
        const char *newline = memchr(line, '\n', (size_t)(limit - line));
        size_t length = newline ? (size_t)(newline - line)
                                : (size_t)(limit - line);
        size_t base = offsetAt(doc, sourceLine, 1) + margin;
        bool single = false;
        bool dbl = false;
        for (size_t i = 0; i < length; i++) {
            char c = line[i];
            if (c == '\\' && !single) {
                i++;
            } else if (c == '\'' && !dbl) {
                single = !single;
            } else if (c == '"' && !single) {
                dbl = !dbl;
            } else if (c == '$' && !single) {
                size_t j = i + 1;
                bool brace = j < length && line[j] == '{';
                if (brace) {
                    j++;
                }
                size_t nameStart = j;
                if (j >= length || !isWordStart(line[j])) {
                    continue;
                }
                while (j < length && isWordChar(line[j])) {
                    j++;
                }
                char *name = arenaStrndup(&doc->arena, &line[nameStart],
                                          j - nameStart);
                const Stmt *local = findLocal(unit, name);
                if (!local || local->line >= action->line) {
                    continue;
                }
                size_t end = j;
                if (brace) {
                    const char *close = memchr(&line[j], '}', length - j);
                    end = close ? (size_t)(close - line) + 1 : j;
                }
                Occurrence occurrence = {
                    OCC_VAR_SHELL, base + i,         base + end,
                    base + nameStart, base + j,       name,
                    unit,           local,            stmt,
                };
                addOccurrence(doc, occurrence);
                i = j - 1;
            }
        }
        line = newline ? newline + 1 : NULL;
        sourceLine++;
    }
    strBufFree(&text);
}

static void addShellUsesOfActions(Document *doc, Unit *unit, const Stmt *stmt,
                                  const ActionList *actions)
{
    for (size_t i = 0; i < actions->count; i++) {
        addShellUses(doc, unit, stmt, &actions->items[i]);
    }
}

static void buildIndex(Document *doc)
{
    Manifest *manifest = &doc->manifest;
    for (size_t u = 0; u < manifest->count; u++) {
        Unit *unit = &manifest->units[u];
        addName(doc, OCC_UNIT_DECL, &unit->name, unit, NULL, NULL);
        for (size_t i = 0; i < unit->stmtCount; i++) {
            const Stmt *stmt = &unit->stmts[i];
            switch (stmt->kind) {
            case STMT_LOCAL:
                addName(doc, OCC_VAR_DECL, &stmt->as.local.name, unit, stmt,
                        stmt);
                addReferences(doc, &stmt->as.local.value, unit, stmt);
                break;
            case STMT_NEEDS:
                for (size_t j = 0; j < stmt->as.needs.count; j++) {
                    const Name *name = &stmt->as.needs.names[j];
                    addName(doc, OCC_UNIT_REF, name,
                            findUnit(manifest, name->text), NULL, stmt);
                }
                break;
            case STMT_CHILD_PKG:
                addName(doc, OCC_PACKAGE, &stmt->as.childPkg.package, unit,
                        NULL, stmt);
                break;
            case STMT_CHECK:
            case STMT_CREATE:
                addName(doc, OCC_VAR_REF, &stmt->as.predicate.variable, unit,
                        findLocal(unit, stmt->as.predicate.variable.text),
                        stmt);
                break;
            case STMT_APPLY_IF:
            case STMT_APPLY_UNLESS:
                addName(doc, OCC_VAR_REF, &stmt->as.apply.variable, unit,
                        findLocal(unit, stmt->as.apply.variable.text), stmt);
                addShellUsesOfActions(doc, unit, stmt,
                                      &stmt->as.apply.thenActions);
                addShellUsesOfActions(doc, unit, stmt,
                                      &stmt->as.apply.elseActions);
                break;
            case STMT_APPLY:
                addShellUsesOfActions(doc, unit, stmt,
                                      &stmt->as.apply.thenActions);
                break;
            }
        }
    }
}

static void analyze(Document *doc)
{
    arenaFree(&doc->arena);
    arenaInit(&doc->arena);
    doc->problems = NULL;
    doc->problemCount = 0;
    doc->problemCapacity = 0;
    doc->occurrences = NULL;
    doc->occurrenceCount = 0;
    doc->occurrenceCapacity = 0;

    Diag diag;
    diagInitSink(&diag, collectProblem, doc);
    doc->lexed =
        lexSource(&doc->arena, doc->text, doc->length, &doc->tokens, &diag);
    doc->parsed = doc->lexed && parseManifest(&doc->arena, &doc->tokens,
                                              &doc->manifest, &diag);
    if (doc->parsed) {
        analyzeManifest(&doc->arena, &doc->manifest, &diag);
        buildIndex(doc);
    }
}

static void setText(Document *doc, const char *text, size_t length)
{
    char *copy = xmalloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    free(doc->text);
    doc->text = copy;
    doc->length = length;
    indexLines(doc);
    analyze(doc);
}

static Document *findDocument(Server *s, const char *uri)
{
    for (size_t i = 0; uri && i < s->documentCount; i++) {
        if (strcmp(s->documents[i]->uri, uri) == 0) {
            return s->documents[i];
        }
    }
    return NULL;
}

static void freeDocument(Document *doc)
{
    arenaFree(&doc->arena);
    free(doc->uri);
    free(doc->text);
    free(doc->lineStarts);
    free(doc);
}

static Document *openDocument(Server *s, const char *uri)
{
    Document *doc = findDocument(s, uri);
    if (doc) {
        return doc;
    }
    doc = xmalloc(sizeof *doc);
    memset(doc, 0, sizeof *doc);
    size_t length = strlen(uri);
    doc->uri = xmalloc(length + 1);
    memcpy(doc->uri, uri, length + 1);
    arenaInit(&doc->arena);
    s->documents = xrealloc(s->documents, (s->documentCount + 1) *
                                              sizeof *s->documents);
    s->documents[s->documentCount++] = doc;
    return doc;
}

static void closeDocument(Server *s, const char *uri)
{
    for (size_t i = 0; uri && i < s->documentCount; i++) {
        if (strcmp(s->documents[i]->uri, uri) == 0) {
            freeDocument(s->documents[i]);
            s->documents[i] = s->documents[--s->documentCount];
            return;
        }
    }
}

/* The occurrence at an offset, preferring one that contains it. */
static const Occurrence *occurrenceAt(const Document *doc, size_t offset)
{
    for (size_t i = 0; i < doc->occurrenceCount; i++) {
        const Occurrence *o = &doc->occurrences[i];
        if (o->start <= offset && offset < o->end) {
            return o;
        }
    }
    for (size_t i = 0; i < doc->occurrenceCount; i++) {
        const Occurrence *o = &doc->occurrences[i];
        if (offset == o->end) {
            return o;
        }
    }
    return NULL;
}

static bool isUnitOccurrence(const Occurrence *o)
{
    return o->kind == OCC_UNIT_DECL || o->kind == OCC_UNIT_REF;
}

static bool isVariableOccurrence(const Occurrence *o)
{
    return o->kind == OCC_VAR_DECL || o->kind == OCC_VAR_REF ||
           o->kind == OCC_VAR_SHELL;
}

static bool sameSymbol(const Occurrence *a, const Occurrence *b)
{
    if (isUnitOccurrence(a) && isUnitOccurrence(b)) {
        return a->unit && a->unit == b->unit;
    }
    if (isVariableOccurrence(a) && isVariableOccurrence(b)) {
        return a->local && a->local == b->local;
    }
    return false;
}

/* The declaration of the symbol of an occurrence, or NULL. */
static const Occurrence *declarationOf(const Document *doc,
                                       const Occurrence *o)
{
    for (size_t i = 0; i < doc->occurrenceCount; i++) {
        const Occurrence *d = &doc->occurrences[i];
        if ((d->kind == OCC_UNIT_DECL || d->kind == OCC_VAR_DECL) &&
            sameSymbol(o, d)) {
            return d;
        }
    }
    return NULL;
}

/* Whether text is an identifier that is not a keyword. */
static bool isPlainIdentifier(const char *text)
{
    size_t length = strlen(text);
    Arena arena;
    arenaInit(&arena);
    Diag diag;
    diagInit(&diag, "", NULL);
    TokenList tokens;
    bool plain = lexSource(&arena, text, length, &tokens, &diag) &&
                 tokens.count == 3 && tokens.items[0].kind == TOK_IDENT &&
                 tokens.items[0].length == length;
    arenaFree(&arena);
    return plain;
}

/* Appends a unit name as written in a manifest: bare or as a string. */
static void appendUnitName(StrBuf *out, const char *name)
{
    if (isPlainIdentifier(name)) {
        strBufAppendStr(out, name);
        return;
    }
    strBufAppendChar(out, '"');
    for (const char *c = name; *c; c++) {
        if (*c == '"' || *c == '\\') {
            strBufAppendChar(out, '\\');
        }
        strBufAppendChar(out, *c);
    }
    strBufAppendChar(out, '"');
}

/* ---------------------------------------------------------------------- */
/* Messages                                                               */
/* ---------------------------------------------------------------------- */

static bool startsWithIgnoringCase(const char *text, const char *prefix)
{
    for (; *prefix; text++, prefix++) {
        char a = *text;
        char b = *prefix;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/*
 * Reads one message. Returns false at the end of input or on a framing
 * error; otherwise *body is the message, or NULL if it had no length.
 */
static bool readMessage(FILE *in, char **body, size_t *length)
{
    char line[1024];
    bool haveLength = false;
    unsigned long contentLength = 0;
    *body = NULL;

    for (;;) {
        if (!fgets(line, sizeof line, in)) {
            return false;
        }
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;
        }
        if (startsWithIgnoringCase(line, "Content-Length:")) {
            char *end;
            contentLength = strtoul(line + 15, &end, 10);
            haveLength = end != line + 15;
        }
    }
    if (!haveLength) {
        return true;
    }
    if (contentLength > MAX_MESSAGE) {
        return false;
    }
    *body = xmalloc(contentLength + 1);
    if (fread(*body, 1, contentLength, in) != contentLength) {
        free(*body);
        *body = NULL;
        return false;
    }
    (*body)[contentLength] = '\0';
    *length = contentLength;
    return true;
}

static void sendMessage(Server *s, const StrBuf *message)
{
    fprintf(s->out, "Content-Length: %zu\r\n\r\n", message->length);
    fwrite(message->data, 1, message->length, s->out);
    fflush(s->out);
}

static void sendResult(Server *s, const JsonValue *id, const StrBuf *result)
{
    StrBuf message;
    strBufInit(&message);
    strBufAppendStr(&message, "{\"jsonrpc\":\"2.0\",\"id\":");
    jsonWriteValue(&message, id);
    strBufAppendStr(&message, ",\"result\":");
    if (result->length > 0) {
        strBufAppend(&message, result->data, result->length);
    } else {
        strBufAppendStr(&message, "null");
    }
    strBufAppendChar(&message, '}');
    sendMessage(s, &message);
    strBufFree(&message);
}

static void sendError(Server *s, const JsonValue *id, int code,
                      const char *text)
{
    StrBuf message;
    strBufInit(&message);
    strBufAppendStr(&message, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id) {
        jsonWriteValue(&message, id);
    } else {
        strBufAppendStr(&message, "null");
    }
    char buf[64];
    snprintf(buf, sizeof buf, ",\"error\":{\"code\":%d,\"message\":", code);
    strBufAppendStr(&message, buf);
    jsonWriteString(&message, text, strlen(text));
    strBufAppendStr(&message, "}}");
    sendMessage(s, &message);
    strBufFree(&message);
}

static void publishDiagnostics(Server *s, const Document *doc, bool clear)
{
    StrBuf message;
    strBufInit(&message);
    strBufAppendStr(&message, "{\"jsonrpc\":\"2.0\",\"method\":"
                              "\"textDocument/publishDiagnostics\","
                              "\"params\":{\"uri\":");
    jsonWriteString(&message, doc->uri, strlen(doc->uri));
    strBufAppendStr(&message, ",\"diagnostics\":[");
    for (size_t i = 0; !clear && i < doc->problemCount; i++) {
        const Problem *problem = &doc->problems[i];
        size_t start = 0;
        size_t end = 0;
        if (problem->line > 0) {
            start = offsetAt(doc, problem->line,
                             problem->column > 0 ? problem->column : 1);
            end = problem->column > 0
                      ? lexemeEnd(doc, start)
                      : lineEnd(doc, (size_t)problem->line - 1);
        }
        if (i > 0) {
            strBufAppendChar(&message, ',');
        }
        strBufAppendStr(&message, "{\"range\":");
        writeRange(&message, s, doc, start, end);
        strBufAppendStr(&message,
                        ",\"severity\":1,\"source\":\"dottr\",\"message\":");
        jsonWriteString(&message, problem->message, strlen(problem->message));
        strBufAppendChar(&message, '}');
    }
    strBufAppendStr(&message, "]}}");
    sendMessage(s, &message);
    strBufFree(&message);
}

/* ---------------------------------------------------------------------- */
/* Hover                                                                  */
/* ---------------------------------------------------------------------- */

static const struct {
    const char *word;
    const char *text;
} keywordDocs[] = {
    {"pkg", "`pkg NAME` declares a unit that installs the package NAME with "
            "pacman. `pkg NAME do ... end` declares a unit satisfied by "
            "evaluating its body. Inside a unit, `pkg NAME` installs a "
            "package as a child unit."},
    {"do", "Opens a block, closed by `end`."},
    {"end", "Closes the innermost block."},
    {"local", "`local NAME = \"STRING\"` binds a variable in the enclosing "
              "unit. Strings may contain `@{NAME}` and `@{dottr.env.home}`. "
              "Shell blocks receive the unit's variables as environment "
              "variables."},
    {"needs", "`needs A, B` requires the units A and B to be satisfied "
              "before this unit. Each must be declared at top level."},
    {"check", "`check command|file|directory NAME` tests the value of a "
              "variable and records the result as a fact for `apply_if` and "
              "`apply_unless`."},
    {"create", "`create directory NAME` creates the directory named by a "
               "variable, with any missing parents."},
    {"apply", "`apply do ... end` runs its shell blocks."},
    {"apply_if", "`apply_if NAME do ... [else ...] end` runs the first "
                 "branch if the fact recorded by `check ... NAME` is true, "
                 "and the `else` branch otherwise."},
    {"apply_unless", "`apply_unless NAME do ... end` runs its body if the "
                     "fact recorded by `check ... NAME` is false."},
    {"else", "Begins the branch of `apply_if` that runs when the fact is "
             "false."},
    {"shell", "`shell do`, a fenced script (```, ```sh, ```bash, or ```zsh), "
              "and `end`. The script runs as `INTERPRETER -e -c SCRIPT` in a "
              "process of its own, with the unit's variables in its "
              "environment."},
    {"command", "Predicate: true if the value names an executable regular "
                "file, directly if it contains `/`, otherwise through "
                "`PATH`."},
    {"file", "Predicate: true if the value names an existing regular file."},
    {"directory", "Predicate: true if the value names an existing "
                  "directory."},
};

static const char *keywordDoc(const char *word, size_t length)
{
    for (size_t i = 0; i < sizeof keywordDocs / sizeof keywordDocs[0]; i++) {
        if (strlen(keywordDocs[i].word) == length &&
            memcmp(keywordDocs[i].word, word, length) == 0) {
            return keywordDocs[i].text;
        }
    }
    return NULL;
}

/* Appends the value of a local, interpolating earlier locals. */
static void appendLocalValue(StrBuf *out, const Unit *unit, const Stmt *local,
                             int depth)
{
    const Template *value = &local->as.local.value;
    for (size_t i = 0; i < value->count; i++) {
        const TemplatePart *part = &value->parts[i];
        if (part->kind == TEMPLATE_TEXT) {
            strBufAppend(out, part->text, part->length);
            continue;
        }
        if (strcmp(part->text, "dottr.env.home") == 0 &&
            hostHomeDirectory()) {
            strBufAppendStr(out, hostHomeDirectory());
            continue;
        }
        const Stmt *target = NULL;
        for (size_t j = 0; j < unit->stmtCount && &unit->stmts[j] < local;
             j++) {
            const Stmt *stmt = &unit->stmts[j];
            if (stmt->kind == STMT_LOCAL &&
                strcmp(stmt->as.local.name.text, part->text) == 0) {
                target = stmt;
                break;
            }
        }
        if (target && depth < 32) {
            appendLocalValue(out, unit, target, depth + 1);
        } else {
            strBufAppendStr(out, "@{");
            strBufAppendStr(out, part->text);
            strBufAppendChar(out, '}');
        }
    }
}

/*
 * Appends text as Markdown inline code, delimited by more backticks than any
 * run in the text, and padded with spaces only if the text begins or ends
 * with a backtick.
 */
static void appendCode(StrBuf *out, const char *text, size_t length)
{
    size_t longest = 0;
    size_t run = 0;
    for (size_t i = 0; i < length; i++) {
        run = text[i] == '`' ? run + 1 : 0;
        longest = run > longest ? run : longest;
    }
    bool pad = length > 0 && (text[0] == '`' || text[length - 1] == '`');
    for (size_t i = 0; i <= longest; i++) {
        strBufAppendChar(out, '`');
    }
    if (pad) {
        strBufAppendChar(out, ' ');
    }
    strBufAppend(out, text, length);
    if (pad) {
        strBufAppendChar(out, ' ');
    }
    for (size_t i = 0; i <= longest; i++) {
        strBufAppendChar(out, '`');
    }
}

static void appendLineNumber(StrBuf *out, const char *prefix, int line)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%s%d", prefix, line);
    strBufAppendStr(out, buf);
}

static void hoverUnit(StrBuf *text, const Document *doc, const Occurrence *o)
{
    const Unit *unit = o->unit;
    if (!unit) {
        strBufAppendStr(text, "**unit** ");
        appendCode(text, o->name, strlen(o->name));
        strBufAppendStr(text, "\n\nNot declared. Declare it at top level, "
                              "for example `pkg ");
        appendUnitName(text, o->name);
        strBufAppendStr(text, "`.");
        return;
    }
    strBufAppendStr(text, "**unit** ");
    appendCode(text, unit->name.text, strlen(unit->name.text));
    strBufAppendStr(text, "\n\n");
    if (unit->isBlock) {
        appendLineNumber(text, "Block unit, declared on line ",
                         unit->name.line);
        strBufAppendChar(text, '.');
        bool first = true;
        for (size_t i = 0; i < unit->stmtCount; i++) {
            const Stmt *stmt = &unit->stmts[i];
            if (stmt->kind != STMT_NEEDS) {
                continue;
            }
            for (size_t j = 0; j < stmt->as.needs.count; j++) {
                strBufAppendStr(text, first ? "\n\nNeeds " : ", ");
                const char *name = stmt->as.needs.names[j].text;
                appendCode(text, name, strlen(name));
                first = false;
            }
        }
        if (!first) {
            strBufAppendChar(text, '.');
        }
    } else {
        strBufAppendStr(text, "Installs the package ");
        appendCode(text, unit->name.text, strlen(unit->name.text));
        appendLineNumber(text, " with pacman. Declared on line ",
                         unit->name.line);
        strBufAppendChar(text, '.');
    }
    (void)doc;
}

static void hoverVariable(StrBuf *text, const Document *doc,
                          const Occurrence *o)
{
    const Stmt *local = o->local;
    if (!local) {
        strBufAppendStr(text, "**variable** ");
        appendCode(text, o->name, strlen(o->name));
        strBufAppendStr(text, "\n\nNot bound in this unit.");
        return;
    }
    size_t start = lineIndentEnd(doc, offsetAt(doc, local->line, 1));
    size_t end = lineEnd(doc, (size_t)local->line - 1);
    strBufAppendStr(text, "```dottr\n");
    strBufAppend(text, &doc->text[start], end - start);
    strBufAppendStr(text, "\n```\n\nValue: ");
    StrBuf value;
    strBufInit(&value);
    appendLocalValue(&value, o->unit, local, 0);
    appendCode(text, value.data ? value.data : "", value.length);
    strBufFree(&value);

    for (size_t i = 0; i < o->unit->stmtCount; i++) {
        const Stmt *stmt = &o->unit->stmts[i];
        if (stmt->kind == STMT_CHECK &&
            strcmp(stmt->as.predicate.variable.text, o->name) == 0) {
            strBufAppendStr(text, "\n\nChecked as ");
            strBufAppendStr(text, predicateKindName(stmt->as.predicate.kind));
            appendLineNumber(text, " on line ", stmt->line);
            strBufAppendChar(text, '.');
        }
    }
}

static void hoverBuiltin(StrBuf *text, const Occurrence *o)
{
    strBufAppendStr(text, "**built-in** ");
    appendCode(text, o->name, strlen(o->name));
    if (strcmp(o->name, "dottr.env.home") != 0) {
        strBufAppendStr(text, "\n\nUnknown built-in value. Version 1 defines "
                              "`dottr.env.home`.");
        return;
    }
    strBufAppendStr(text, "\n\nThe home directory: `$HOME`, or the entry of "
                          "the password database if `$HOME` is unset.");
    const char *home = hostHomeDirectory();
    if (home) {
        strBufAppendStr(text, "\n\nValue: ");
        appendCode(text, home, strlen(home));
    }
}

static void hoverPackage(StrBuf *text, const Occurrence *o)
{
    strBufAppendStr(text, "**child unit** ");
    const char *child = o->stmt->as.childPkg.childName;
    appendCode(text, child, strlen(child));
    strBufAppendStr(text, "\n\nInstalls the package ");
    appendCode(text, o->name, strlen(o->name));
    strBufAppendStr(text, " with pacman. Child units cannot be named in "
                          "`needs`.");
}

/* The keyword token at an offset, or NULL. */
static const Token *keywordAt(const Document *doc, size_t offset)
{
    if (!doc->lexed) {
        return NULL;
    }
    for (size_t i = 0; i < doc->tokens.count; i++) {
        const Token *token = &doc->tokens.items[i];
        if (token->kind < TOK_PKG) {
            continue;
        }
        size_t start = offsetAt(doc, token->line, token->column);
        if (start <= offset && offset <= start + token->length) {
            return token;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Requests                                                               */
/* ---------------------------------------------------------------------- */

typedef struct {
    StrBuf result;
    int errorCode;
    const char *errorMessage;
} Reply;

static Document *paramsDocument(Server *s, const JsonValue *params)
{
    return findDocument(
        s, jsonString(jsonGet(jsonGet(params, "textDocument"), "uri")));
}

static bool paramsOffset(Server *s, const JsonValue *params, Document **doc,
                         size_t *offset)
{
    *doc = paramsDocument(s, params);
    if (!*doc) {
        return false;
    }
    *offset = offsetOfPosition(s, *doc, jsonGet(params, "position"));
    return true;
}

static bool arrayContainsString(const JsonValue *array, const char *text)
{
    if (!array || array->kind != JSON_ARRAY) {
        return false;
    }
    for (size_t i = 0; i < array->count; i++) {
        const char *item = jsonString(array->items[i]);
        if (item && strcmp(item, text) == 0) {
            return true;
        }
    }
    return false;
}

static void handleInitialize(Server *s, const JsonValue *params, Reply *reply)
{
    const JsonValue *capabilities = jsonGet(params, "capabilities");
    const JsonValue *textDocument = jsonGet(capabilities, "textDocument");
    s->utf8 = arrayContainsString(
        jsonGet(jsonGet(capabilities, "general"), "positionEncodings"),
        "utf-8");
    s->snippets = jsonBool(
        jsonGet(jsonGet(jsonGet(textDocument, "completion"), "completionItem"),
                "snippetSupport"),
        false);
    s->markdown = arrayContainsString(
        jsonGet(jsonGet(textDocument, "hover"), "contentFormat"), "markdown");
    s->initialized = true;

    strBufAppendStr(&reply->result, "{\"capabilities\":{\"positionEncoding\":");
    strBufAppendStr(&reply->result, s->utf8 ? "\"utf-8\"" : "\"utf-16\"");
    strBufAppendStr(&reply->result,
                    ",\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                    "\"documentFormattingProvider\":true,"
                    "\"documentSymbolProvider\":true,"
                    "\"workspaceSymbolProvider\":true,"
                    "\"definitionProvider\":true,"
                    "\"referencesProvider\":true,"
                    "\"hoverProvider\":true,"
                    "\"renameProvider\":{\"prepareProvider\":true},"
                    "\"completionProvider\":{\"triggerCharacters\":"
                    "[\"{\",\".\"]}},"
                    "\"serverInfo\":{\"name\":\"dottr\"}}");
}

static void handleShutdown(Server *s, const JsonValue *params, Reply *reply)
{
    (void)params;
    (void)reply;
    s->shutdown = true;
}

static void handleFormatting(Server *s, const JsonValue *params, Reply *reply)
{
    Document *doc = paramsDocument(s, params);
    if (!doc) {
        return;
    }
    Diag diag;
    diagInit(&diag, "", NULL);
    StrBuf formatted;
    strBufInit(&formatted);
    if (formatManifest(doc->text, doc->length, &formatted, &diag)) {
        if (formatted.length == doc->length &&
            memcmp(formatted.data ? formatted.data : "", doc->text,
                   doc->length) == 0) {
            strBufAppendStr(&reply->result, "[]");
        } else {
            strBufAppendStr(&reply->result, "[{\"range\":");
            writeRange(&reply->result, s, doc, 0, doc->length);
            strBufAppendStr(&reply->result, ",\"newText\":");
            jsonWriteString(&reply->result, formatted.data ? formatted.data : "",
                            formatted.length);
            strBufAppendStr(&reply->result, "}]");
        }
    }
    strBufFree(&formatted);
}

static void writeSymbol(StrBuf *out, const Server *s, const Document *doc,
                        const char *name, const char *detail, int kind,
                        size_t start, size_t end, size_t selectionStart,
                        size_t selectionEnd)
{
    char buf[32];
    strBufAppendStr(out, "{\"name\":");
    jsonWriteString(out, name, strlen(name));
    if (detail) {
        strBufAppendStr(out, ",\"detail\":");
        jsonWriteString(out, detail, strlen(detail));
    }
    snprintf(buf, sizeof buf, ",\"kind\":%d", kind);
    strBufAppendStr(out, buf);
    strBufAppendStr(out, ",\"range\":");
    writeRange(out, s, doc, start, end);
    strBufAppendStr(out, ",\"selectionRange\":");
    writeRange(out, s, doc, selectionStart, selectionEnd);
}

static void unitSpan(const Document *doc, const Unit *unit, size_t *start,
                     size_t *end)
{
    size_t nameStart;
    size_t nameEnd;
    size_t unused;
    nameRange(doc, &unit->name, &nameStart, &nameEnd, &unused, &unused);
    *start = lineIndentEnd(doc, nameStart);
    *end = unit->isBlock && unit->endLine > 0
               ? offsetAt(doc, unit->endLine, unit->endColumn) + 3
               : nameEnd;
    if (*end > doc->length) {
        *end = doc->length;
    }
}

static void handleDocumentSymbol(Server *s, const JsonValue *params,
                                 Reply *reply)
{
    Document *doc = paramsDocument(s, params);
    if (!doc || !doc->parsed) {
        return;
    }
    StrBuf *out = &reply->result;
    strBufAppendChar(out, '[');
    for (size_t u = 0; u < doc->manifest.count; u++) {
        const Unit *unit = &doc->manifest.units[u];
        size_t start;
        size_t end;
        size_t nameStart;
        size_t nameEnd;
        size_t unused;
        unitSpan(doc, unit, &start, &end);
        nameRange(doc, &unit->name, &nameStart, &nameEnd, &unused, &unused);
        if (u > 0) {
            strBufAppendChar(out, ',');
        }
        writeSymbol(out, s, doc, unit->name.text,
                    unit->isBlock ? "unit" : "package",
                    unit->isBlock ? SYMBOL_MODULE : SYMBOL_PACKAGE, start, end,
                    nameStart, nameEnd);
        strBufAppendStr(out, ",\"children\":[");
        bool first = true;
        for (size_t i = 0; i < unit->stmtCount; i++) {
            const Stmt *stmt = &unit->stmts[i];
            const Name *name;
            const char *detail;
            int kind;
            if (stmt->kind == STMT_LOCAL) {
                name = &stmt->as.local.name;
                detail = "local";
                kind = SYMBOL_VARIABLE;
            } else if (stmt->kind == STMT_CHILD_PKG) {
                name = &stmt->as.childPkg.package;
                detail = stmt->as.childPkg.childName;
                kind = SYMBOL_PACKAGE;
            } else {
                continue;
            }
            size_t lineStart = lineIndentEnd(doc, offsetAt(doc, stmt->line, 1));
            size_t lineStop = lineEnd(doc, (size_t)stmt->line - 1);
            nameRange(doc, name, &nameStart, &nameEnd, &unused, &unused);
            if (!first) {
                strBufAppendChar(out, ',');
            }
            writeSymbol(out, s, doc, name->text, detail, kind, lineStart,
                        lineStop, nameStart, nameEnd);
            strBufAppendChar(out, '}');
            first = false;
        }
        strBufAppendStr(out, "]}");
    }
    strBufAppendChar(out, ']');
}

static bool containsIgnoringCase(const char *text, const char *query)
{
    size_t queryLength = strlen(query);
    for (const char *t = text; *t; t++) {
        if (strlen(t) < queryLength) {
            return false;
        }
        if (startsWithIgnoringCase(t, query)) {
            return true;
        }
    }
    return queryLength == 0;
}

static void writeSymbolInformation(StrBuf *out, const Server *s,
                                   const Document *doc, const char *name,
                                   int kind, size_t start, size_t end,
                                   const char *container, bool *first)
{
    char buf[32];
    if (!*first) {
        strBufAppendChar(out, ',');
    }
    *first = false;
    strBufAppendStr(out, "{\"name\":");
    jsonWriteString(out, name, strlen(name));
    snprintf(buf, sizeof buf, ",\"kind\":%d", kind);
    strBufAppendStr(out, buf);
    strBufAppendStr(out, ",\"location\":");
    writeLocation(out, s, doc, start, end);
    if (container) {
        strBufAppendStr(out, ",\"containerName\":");
        jsonWriteString(out, container, strlen(container));
    }
    strBufAppendChar(out, '}');
}

static void handleWorkspaceSymbol(Server *s, const JsonValue *params,
                                  Reply *reply)
{
    const char *query = jsonString(jsonGet(params, "query"));
    if (!query) {
        query = "";
    }
    StrBuf *out = &reply->result;
    bool first = true;
    strBufAppendChar(out, '[');
    for (size_t d = 0; d < s->documentCount; d++) {
        const Document *doc = s->documents[d];
        for (size_t i = 0; i < doc->occurrenceCount; i++) {
            const Occurrence *o = &doc->occurrences[i];
            if ((o->kind != OCC_UNIT_DECL && o->kind != OCC_VAR_DECL) ||
                !containsIgnoringCase(o->name, query)) {
                continue;
            }
            if (o->kind == OCC_UNIT_DECL) {
                writeSymbolInformation(
                    out, s, doc, o->name,
                    o->unit->isBlock ? SYMBOL_MODULE : SYMBOL_PACKAGE,
                    o->start, o->end, NULL, &first);
            } else {
                writeSymbolInformation(out, s, doc, o->name, SYMBOL_VARIABLE,
                                       o->start, o->end, o->unit->name.text,
                                       &first);
            }
        }
    }
    strBufAppendChar(out, ']');
}

static void handleDefinition(Server *s, const JsonValue *params, Reply *reply)
{
    Document *doc;
    size_t offset;
    if (!paramsOffset(s, params, &doc, &offset)) {
        return;
    }
    const Occurrence *o = occurrenceAt(doc, offset);
    const Occurrence *declaration = o ? declarationOf(doc, o) : NULL;
    if (declaration) {
        writeLocation(&reply->result, s, doc, declaration->start,
                      declaration->end);
    }
}

static void handleReferences(Server *s, const JsonValue *params, Reply *reply)
{
    Document *doc;
    size_t offset;
    if (!paramsOffset(s, params, &doc, &offset)) {
        return;
    }
    const Occurrence *o = occurrenceAt(doc, offset);
    if (!o) {
        return;
    }
    bool includeDeclaration = jsonBool(
        jsonGet(jsonGet(params, "context"), "includeDeclaration"), true);
    StrBuf *out = &reply->result;
    bool first = true;
    strBufAppendChar(out, '[');
    for (size_t i = 0; i < doc->occurrenceCount; i++) {
        const Occurrence *r = &doc->occurrences[i];
        bool declaration = r->kind == OCC_UNIT_DECL || r->kind == OCC_VAR_DECL;
        if (!sameSymbol(o, r) || (declaration && !includeDeclaration)) {
            continue;
        }
        if (!first) {
            strBufAppendChar(out, ',');
        }
        first = false;
        writeLocation(out, s, doc, r->start, r->end);
    }
    strBufAppendChar(out, ']');
}

static bool renameable(const Occurrence *o)
{
    return (isUnitOccurrence(o) && o->unit) ||
           (isVariableOccurrence(o) && o->local);
}

static void handlePrepareRename(Server *s, const JsonValue *params,
                                Reply *reply)
{
    Document *doc;
    size_t offset;
    if (!paramsOffset(s, params, &doc, &offset)) {
        return;
    }
    const Occurrence *o = occurrenceAt(doc, offset);
    if (!o || !renameable(o)) {
        return;
    }
    size_t start = isUnitOccurrence(o) ? o->start : o->nameStart;
    size_t end = isUnitOccurrence(o) ? o->end : o->nameEnd;
    strBufAppendStr(&reply->result, "{\"range\":");
    writeRange(&reply->result, s, doc, start, end);
    strBufAppendStr(&reply->result, ",\"placeholder\":");
    jsonWriteString(&reply->result, o->name, strlen(o->name));
    strBufAppendChar(&reply->result, '}');
}

static void handleRename(Server *s, const JsonValue *params, Reply *reply)
{
    Document *doc;
    size_t offset;
    if (!paramsOffset(s, params, &doc, &offset)) {
        return;
    }
    const char *newName = jsonString(jsonGet(params, "newName"));
    const Occurrence *o = occurrenceAt(doc, offset);
    if (!newName || !o || !renameable(o)) {
        reply->errorCode = ERROR_REQUEST_FAILED;
        reply->errorMessage = "there is no unit or variable to rename here";
        return;
    }

    StrBuf newText;
    strBufInit(&newText);
    if (isUnitOccurrence(o)) {
        if (*newName == '\0' || strchr(newName, '\n') ||
            strstr(newName, "@{")) {
            reply->errorMessage = "a unit name must be non-empty and contain "
                                  "no line break or @{";
        } else {
            const Unit *existing = findUnit(&doc->manifest, newName);
            if (existing && existing != o->unit) {
                reply->errorMessage = "a unit of that name already exists";
            }
        }
        appendUnitName(&newText, newName);
    } else {
        if (!isPlainIdentifier(newName)) {
            reply->errorMessage = "a variable name must be an identifier "
                                  "that is not a keyword";
        } else if (isProtectedVariable(newName)) {
            reply->errorMessage = "that name would replace an environment "
                                  "variable that shells depend on";
        } else {
            const Stmt *existing = findLocal(o->unit, newName);
            if (existing && existing != o->local) {
                reply->errorMessage = "the unit already has a variable of "
                                      "that name";
            }
        }
        strBufAppendStr(&newText, newName);
    }
    if (reply->errorMessage) {
        reply->errorCode = ERROR_REQUEST_FAILED;
        strBufFree(&newText);
        return;
    }

    StrBuf *out = &reply->result;
    strBufAppendStr(out, "{\"changes\":{");
    jsonWriteString(out, doc->uri, strlen(doc->uri));
    strBufAppendStr(out, ":[");
    bool first = true;
    for (size_t i = 0; i < doc->occurrenceCount; i++) {
        const Occurrence *r = &doc->occurrences[i];
        if (!sameSymbol(o, r)) {
            continue;
        }
        size_t start = isUnitOccurrence(r) ? r->start : r->nameStart;
        size_t end = isUnitOccurrence(r) ? r->end : r->nameEnd;
        if (!first) {
            strBufAppendChar(out, ',');
        }
        first = false;
        strBufAppendStr(out, "{\"range\":");
        writeRange(out, s, doc, start, end);
        strBufAppendStr(out, ",\"newText\":");
        jsonWriteString(out, newText.data, newText.length);
        strBufAppendChar(out, '}');
    }
    strBufAppendStr(out, "]}}");
    strBufFree(&newText);
}

static void handleHover(Server *s, const JsonValue *params, Reply *reply)
{
    Document *doc;
    size_t offset;
    if (!paramsOffset(s, params, &doc, &offset)) {
        return;
    }
    StrBuf text;
    strBufInit(&text);
    size_t start = offset;
    size_t end = offset;

    const Occurrence *o = occurrenceAt(doc, offset);
    if (o) {
        start = o->start;
        end = o->end;
        switch (o->kind) {
        case OCC_UNIT_DECL:
        case OCC_UNIT_REF:
            hoverUnit(&text, doc, o);
            break;
        case OCC_VAR_DECL:
        case OCC_VAR_REF:
        case OCC_VAR_SHELL:
            hoverVariable(&text, doc, o);
            break;
        case OCC_BUILTIN:
            hoverBuiltin(&text, o);
            break;
        case OCC_PACKAGE:
            hoverPackage(&text, o);
            break;
        }
    } else {
        const Token *keyword = keywordAt(doc, offset);
        const char *docText =
            keyword ? keywordDoc(keyword->text, keyword->length) : NULL;
        if (docText) {
            start = offsetAt(doc, keyword->line, keyword->column);
            end = start + keyword->length;
            strBufAppendStr(&text, docText);
        }
    }

    if (text.length > 0) {
        strBufAppendStr(&reply->result, "{\"contents\":{\"kind\":");
        strBufAppendStr(&reply->result,
                        s->markdown ? "\"markdown\"" : "\"plaintext\"");
        strBufAppendStr(&reply->result, ",\"value\":");
        jsonWriteString(&reply->result, text.data, text.length);
        strBufAppendStr(&reply->result, "},\"range\":");
        writeRange(&reply->result, s, doc, start, end);
        strBufAppendChar(&reply->result, '}');
    }
    strBufFree(&text);
}

/* ---------------------------------------------------------------------- */
/* Completion                                                             */
/* ---------------------------------------------------------------------- */

typedef enum {
    BLOCK_UNIT,
    BLOCK_APPLY,
    BLOCK_APPLY_IF,
    BLOCK_SHELL,
} BlockKind;

#define MAX_BLOCKS 64

/* A name in the document text. */
typedef struct {
    const char *text;
    size_t length;
} Span;

typedef struct {
    Span *items;
    size_t count;
} SpanList;

static void spanListAdd(SpanList *list, Span span)
{
    list->items = xrealloc(list->items, (list->count + 1) * sizeof *list->items);
    list->items[list->count++] = span;
}

/*
 * Line-based scan of a document, which does
 * not require the document to parse.
 */
typedef struct {
    bool inRaw;
    BlockKind blocks[MAX_BLOCKS];
    int depth;
    Span unitName;
    SpanList locals;
    SpanList checked;
    /* Units declared at top level, collected over the whole document. */
    SpanList units;
} Scan;

/* Splits a line into words, string literals (as one word), and commas. */
typedef struct {
    Span words[16];
    size_t count;
    /* The line ends inside a string literal or a comment. */
    bool unfinished;
} Words;

static void splitWords(const char *text, size_t length, Words *words)
{
    words->count = 0;
    words->unfinished = false;
    size_t i = 0;
    while (i < length) {
        char c = text[i];
        size_t start = i;
        if (isBlank(c) || c == '\r') {
            i++;
            continue;
        }
        if (c == '#') {
            words->unfinished = true;
            return;
        }
        if (c == '"') {
            for (i++; i < length && text[i] != '"'; i++) {
                if (text[i] == '\\') {
                    i++;
                }
            }
            if (i >= length) {
                words->unfinished = true;
                return;
            }
            i++;
        } else if (isWordChar(c)) {
            while (i < length && isWordChar(text[i])) {
                i++;
            }
        } else {
            i++;
        }
        if (words->count < sizeof words->words / sizeof words->words[0]) {
            words->words[words->count++] = (Span){&text[start], i - start};
        }
    }
}

static bool spanIs(Span span, const char *word)
{
    return span.length == strlen(word) &&
           memcmp(span.text, word, span.length) == 0;
}

/* The name of a word: a string literal without its quotes. */
static Span unquote(Span span)
{
    if (span.length >= 2 && span.text[0] == '"') {
        return (Span){span.text + 1, span.length - 2};
    }
    return span;
}

static void scanLine(Scan *scan, const char *text, size_t length)
{
    size_t start = 0;
    while (start < length && isBlank(text[start])) {
        start++;
    }
    size_t end = length;
    while (end > start && (isBlank(text[end - 1]) || text[end - 1] == '\r')) {
        end--;
    }
    bool fence = end - start >= 3 && memcmp(&text[start], "```", 3) == 0;
    if (scan->inRaw) {
        scan->inRaw = !(fence && end - start == 3);
        return;
    }
    if (fence) {
        scan->inRaw = true;
        return;
    }

    Words words;
    splitWords(text, length, &words);
    if (words.count == 0) {
        return;
    }
    Span first = words.words[0];
    Span last = words.words[words.count - 1];

    if (spanIs(first, "end")) {
        if (scan->depth > 0 && --scan->depth == 0) {
            scan->unitName = (Span){NULL, 0};
            scan->locals.count = 0;
            scan->checked.count = 0;
        }
        return;
    }
    if (spanIs(first, "pkg") && words.count >= 2 && scan->depth == 0) {
        spanListAdd(&scan->units, unquote(words.words[1]));
    }
    if (scan->depth > 0 && spanIs(first, "local") && words.count >= 2) {
        spanListAdd(&scan->locals, words.words[1]);
    }
    if (scan->depth > 0 && spanIs(first, "check") && words.count >= 3) {
        spanListAdd(&scan->checked, words.words[2]);
    }
    if (spanIs(last, "do") && scan->depth < MAX_BLOCKS) {
        BlockKind kind = BLOCK_APPLY;
        if (spanIs(first, "pkg")) {
            kind = BLOCK_UNIT;
            if (scan->depth == 0 && words.count >= 2) {
                scan->unitName = unquote(words.words[1]);
                scan->locals.count = 0;
                scan->checked.count = 0;
            }
        } else if (spanIs(first, "apply_if")) {
            kind = BLOCK_APPLY_IF;
        } else if (spanIs(first, "shell")) {
            kind = BLOCK_SHELL;
        }
        scan->blocks[scan->depth++] = kind;
    }
}

static void scanFree(Scan *scan)
{
    free(scan->locals.items);
    free(scan->checked.items);
    free(scan->units.items);
}

typedef struct {
    StrBuf *out;
    bool first;
} ItemWriter;

static void beginItem(ItemWriter *w, const char *label, size_t length,
                      int kind, const char *detail)
{
    char buf[32];
    if (!w->first) {
        strBufAppendChar(w->out, ',');
    }
    w->first = false;
    strBufAppendStr(w->out, "{\"label\":");
    jsonWriteString(w->out, label, length);
    snprintf(buf, sizeof buf, ",\"kind\":%d", kind);
    strBufAppendStr(w->out, buf);
    if (detail) {
        strBufAppendStr(w->out, ",\"detail\":");
        jsonWriteString(w->out, detail, strlen(detail));
    }
}

static void addItem(ItemWriter *w, const char *label, size_t length, int kind,
                    const char *detail)
{
    beginItem(w, label, length, kind, detail);
    strBufAppendChar(w->out, '}');
}

static const struct {
    const char *label;
    const char *body;
} snippets[] = {
    {"pkg", "pkg ${1:name} do\n\t$0\nend"},
    {"apply", "apply do\n\tshell do\n\t\t```\n\t\t$0\n\t\t```\n\tend\nend"},
    {"apply_if", "apply_if ${1:NAME} do\n\t$0\nend"},
    {"apply_unless", "apply_unless ${1:NAME} do\n\t$0\nend"},
    {"shell", "shell do\n\t```${1}\n\t$0\n\t```\nend"},
};

static void addKeywords(ItemWriter *w, const Server *s, int depth,
                        BlockKind block)
{
    static const char *const topLevel[] = {"pkg", NULL};
    static const char *const unit[] = {
        "local", "needs", "pkg", "check", "create",
        "apply", "apply_if", "apply_unless", "end", NULL};
    static const char *const apply[] = {"shell", "end", NULL};
    static const char *const applyIf[] = {"shell", "else", "end", NULL};
    static const char *const shell[] = {"end", NULL};

    const char *const *words = topLevel;
    if (depth > 0) {
        words = block == BLOCK_UNIT       ? unit
                : block == BLOCK_APPLY_IF ? applyIf
                : block == BLOCK_SHELL    ? shell
                                          : apply;
    }
    for (size_t i = 0; words[i]; i++) {
        addItem(w, words[i], strlen(words[i]), COMPLETION_KEYWORD, NULL);
        for (size_t j = 0; s->snippets && j < sizeof snippets / sizeof snippets[0];
             j++) {
            if (strcmp(snippets[j].label, words[i]) == 0) {
                beginItem(w, words[i], strlen(words[i]), COMPLETION_SNIPPET,
                          "block");
                strBufAppendStr(w->out, ",\"insertTextFormat\":2,"
                                        "\"insertText\":");
                jsonWriteString(w->out, snippets[j].body,
                                strlen(snippets[j].body));
                strBufAppendChar(w->out, '}');
            }
        }
    }
}

static void addVariables(ItemWriter *w, const SpanList *names)
{
    for (size_t i = 0; i < names->count; i++) {
        addItem(w, names->items[i].text, names->items[i].length,
                COMPLETION_VARIABLE, "local");
    }
}

/* Completion after `@{`: replaces the reference text typed so far. */
static void completeReference(ItemWriter *w, const Server *s,
                              const Document *doc, const Scan *scan,
                              size_t replaceStart, size_t cursor)
{
    bool closed = cursor < doc->length && doc->text[cursor] == '}';
    for (size_t i = 0; i <= scan->locals.count; i++) {
        Span name = i < scan->locals.count
                        ? scan->locals.items[i]
                        : (Span){"dottr.env.home", strlen("dottr.env.home")};
        beginItem(w, name.text, name.length,
                  i < scan->locals.count ? COMPLETION_VARIABLE
                                         : COMPLETION_CONSTANT,
                  i < scan->locals.count ? "local" : "home directory");
        strBufAppendStr(w->out, ",\"textEdit\":{\"range\":");
        writeRange(w->out, s, doc, replaceStart, cursor);
        strBufAppendStr(w->out, ",\"newText\":");
        StrBuf text;
        strBufInit(&text);
        strBufAppend(&text, name.text, name.length);
        if (!closed) {
            strBufAppendChar(&text, '}');
        }
        jsonWriteString(w->out, text.data, text.length);
        strBufFree(&text);
        strBufAppendStr(w->out, "}}");
    }
}

static void handleCompletion(Server *s, const JsonValue *params, Reply *reply)
{
    Document *doc;
    size_t cursor;
    if (!paramsOffset(s, params, &doc, &cursor)) {
        return;
    }
    size_t cursorLine = lineOf(doc, cursor);

    /* Units over the whole document; context up to the cursor line. */
    Scan whole = {0};
    for (size_t line = 0; line < doc->lineCount; line++) {
        size_t start = doc->lineStarts[line];
        scanLine(&whole, &doc->text[start], lineEnd(doc, line) - start);
    }
    Scan scan = {0};
    for (size_t line = 0; line < cursorLine; line++) {
        size_t start = doc->lineStarts[line];
        scanLine(&scan, &doc->text[start], lineEnd(doc, line) - start);
    }

    ItemWriter w = {&reply->result, true};
    strBufAppendChar(&reply->result, '[');
    if (scan.inRaw) {
        goto done;
    }

    const char *prefix = &doc->text[doc->lineStarts[cursorLine]];
    size_t prefixLength = cursor - doc->lineStarts[cursorLine];

    /* Inside `@{`, with only reference characters typed since. */
    for (size_t i = prefixLength; i >= 2; i--) {
        char c = prefix[i - 1];
        if (c == '{' && prefix[i - 2] == '@') {
            completeReference(&w, s, doc, &scan,
                              doc->lineStarts[cursorLine] + i, cursor);
            goto done;
        }
        if (!isWordChar(c) && c != '.') {
            break;
        }
    }

    Words words;
    splitWords(prefix, prefixLength, &words);
    if (words.unfinished) {
        goto done;
    }
    /* A word under the cursor is being typed; it is not context. */
    if (prefixLength > 0 && isWordChar(prefix[prefixLength - 1]) &&
        words.count > 0) {
        words.count--;
    }
    Span context[16];
    size_t count = 0;
    for (size_t i = 0; i < words.count; i++) {
        if (!spanIs(words.words[i], ",")) {
            context[count++] = words.words[i];
        }
    }

    if (count == 0) {
        addKeywords(&w, s, scan.depth,
                    scan.depth > 0 ? scan.blocks[scan.depth - 1] : BLOCK_UNIT);
    } else if (spanIs(context[0], "needs")) {
        for (size_t i = 0; i < whole.units.count; i++) {
            Span unit = whole.units.items[i];
            bool listed = false;
            for (size_t j = 1; j < count; j++) {
                Span given = unquote(context[j]);
                listed = listed || (given.length == unit.length &&
                                    memcmp(given.text, unit.text,
                                           unit.length) == 0);
            }
            bool self = scan.unitName.text &&
                        scan.unitName.length == unit.length &&
                        memcmp(scan.unitName.text, unit.text, unit.length) == 0;
            if (listed || self) {
                continue;
            }
            char *name = xmalloc(unit.length + 1);
            memcpy(name, unit.text, unit.length);
            name[unit.length] = '\0';
            beginItem(&w, name, unit.length, COMPLETION_MODULE, "unit");
            if (!isPlainIdentifier(name)) {
                StrBuf quoted;
                strBufInit(&quoted);
                appendUnitName(&quoted, name);
                strBufAppendStr(w.out, ",\"insertText\":");
                jsonWriteString(w.out, quoted.data, quoted.length);
                strBufFree(&quoted);
            }
            strBufAppendChar(w.out, '}');
            free(name);
        }
    } else if (count == 1 && spanIs(context[0], "check")) {
        addItem(&w, "command", 7, COMPLETION_ENUM_MEMBER, "predicate");
        addItem(&w, "file", 4, COMPLETION_ENUM_MEMBER, "predicate");
        addItem(&w, "directory", 9, COMPLETION_ENUM_MEMBER, "predicate");
    } else if (count == 1 && spanIs(context[0], "create")) {
        addItem(&w, "directory", 9, COMPLETION_ENUM_MEMBER, "kind");
    } else if (count == 2 && (spanIs(context[0], "check") ||
                              spanIs(context[0], "create"))) {
        addVariables(&w, &scan.locals);
    } else if (count == 1 && (spanIs(context[0], "apply_if") ||
                              spanIs(context[0], "apply_unless"))) {
        addVariables(&w, &scan.checked);
    }

done:
    strBufAppendChar(&reply->result, ']');
    scanFree(&scan);
    scanFree(&whole);
}

/* ---------------------------------------------------------------------- */
/* Notifications                                                          */
/* ---------------------------------------------------------------------- */

static void handleDidOpen(Server *s, const JsonValue *params)
{
    const JsonValue *item = jsonGet(params, "textDocument");
    const char *uri = jsonString(jsonGet(item, "uri"));
    const JsonValue *text = jsonGet(item, "text");
    if (!uri || !text || text->kind != JSON_STRING) {
        return;
    }
    Document *doc = openDocument(s, uri);
    setText(doc, text->string, text->length);
    publishDiagnostics(s, doc, false);
}

/* Applies one content change, ranged or full. */
static void applyChange(Server *s, Document *doc, const JsonValue *change)
{
    const JsonValue *text = jsonGet(change, "text");
    if (!text || text->kind != JSON_STRING) {
        return;
    }
    const JsonValue *range = jsonGet(change, "range");
    if (!range) {
        setText(doc, text->string, text->length);
        return;
    }
    size_t start = offsetOfPosition(s, doc, jsonGet(range, "start"));
    size_t end = offsetOfPosition(s, doc, jsonGet(range, "end"));
    if (end < start) {
        end = start;
    }
    StrBuf updated;
    strBufInit(&updated);
    strBufAppend(&updated, doc->text, start);
    strBufAppend(&updated, text->string, text->length);
    strBufAppend(&updated, doc->text + end, doc->length - end);
    setText(doc, updated.data ? updated.data : "", updated.length);
    strBufFree(&updated);
}

static void handleDidChange(Server *s, const JsonValue *params)
{
    Document *doc = paramsDocument(s, params);
    const JsonValue *changes = jsonGet(params, "contentChanges");
    if (!doc || !changes || changes->kind != JSON_ARRAY) {
        return;
    }
    for (size_t i = 0; i < changes->count; i++) {
        applyChange(s, doc, changes->items[i]);
    }
    publishDiagnostics(s, doc, false);
}

static void handleDidClose(Server *s, const JsonValue *params)
{
    Document *doc = paramsDocument(s, params);
    if (doc) {
        publishDiagnostics(s, doc, true);
        closeDocument(s, doc->uri);
    }
}

/* ---------------------------------------------------------------------- */
/* Dispatch                                                               */
/* ---------------------------------------------------------------------- */

static const struct {
    const char *method;
    void (*handler)(Server *, const JsonValue *, Reply *);
} requests[] = {
    {"initialize", handleInitialize},
    {"shutdown", handleShutdown},
    {"textDocument/formatting", handleFormatting},
    {"textDocument/documentSymbol", handleDocumentSymbol},
    {"workspace/symbol", handleWorkspaceSymbol},
    {"textDocument/definition", handleDefinition},
    {"textDocument/references", handleReferences},
    {"textDocument/prepareRename", handlePrepareRename},
    {"textDocument/rename", handleRename},
    {"textDocument/hover", handleHover},
    {"textDocument/completion", handleCompletion},
};

static void handleRequest(Server *s, const char *method, const JsonValue *id,
                          const JsonValue *params)
{
    if (!s->initialized && strcmp(method, "initialize") != 0) {
        sendError(s, id, ERROR_NOT_INITIALIZED, "server not initialized");
        return;
    }
    if (s->shutdown) {
        sendError(s, id, ERROR_INVALID_REQUEST, "server is shutting down");
        return;
    }
    for (size_t i = 0; i < sizeof requests / sizeof requests[0]; i++) {
        if (strcmp(requests[i].method, method) != 0) {
            continue;
        }
        if (params && params->kind != JSON_OBJECT &&
            params->kind != JSON_ARRAY) {
            sendError(s, id, ERROR_INVALID_PARAMS, "invalid params");
            return;
        }
        Reply reply;
        strBufInit(&reply.result);
        reply.errorCode = 0;
        reply.errorMessage = NULL;
        requests[i].handler(s, params, &reply);
        if (reply.errorCode) {
            sendError(s, id, reply.errorCode, reply.errorMessage);
        } else {
            sendResult(s, id, &reply.result);
        }
        strBufFree(&reply.result);
        return;
    }
    sendError(s, id, ERROR_METHOD_NOT_FOUND, "method not found");
}

static void handleNotification(Server *s, const char *method,
                               const JsonValue *params)
{
    if (!s->initialized || s->shutdown) {
        return;
    }
    if (strcmp(method, "textDocument/didOpen") == 0) {
        handleDidOpen(s, params);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        handleDidChange(s, params);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        handleDidClose(s, params);
    }
}

int lspRun(FILE *in, FILE *out)
{
    Server s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.out = out;
    int status = 1;

    for (;;) {
        char *body;
        size_t length = 0;
        if (!readMessage(in, &body, &length)) {
            break;
        }
        if (!body) {
            continue;
        }
        Arena arena;
        arenaInit(&arena);
        JsonValue *message = jsonParse(&arena, body, length);
        const char *method = jsonString(jsonGet(message, "method"));
        const JsonValue *id = jsonGet(message, "id");
        bool exit = false;

        if (!message || message->kind != JSON_OBJECT) {
            sendError(&s, NULL, ERROR_PARSE, "invalid JSON");
        } else if (!method) {
            /* A response to a server request; none are sent. */
        } else if (strcmp(method, "exit") == 0) {
            status = s.shutdown ? 0 : 1;
            exit = true;
        } else if (id) {
            handleRequest(&s, method, id, jsonGet(message, "params"));
        } else {
            handleNotification(&s, method, jsonGet(message, "params"));
        }

        arenaFree(&arena);
        free(body);
        if (exit) {
            break;
        }
    }

    for (size_t i = 0; i < s.documentCount; i++) {
        freeDocument(s.documents[i]);
    }
    free(s.documents);
    return status;
}
