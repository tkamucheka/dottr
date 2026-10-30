/*
 * Language server tests. Each test runs lspRun on a scripted session through
 * in-memory streams and checks the messages it writes.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/json.h"
#include "dottr/lsp.h"
#include "dottr/util.h"
#include "platform.h"
#include "test.h"

#define URI "file:///work/packages.dotfile"

typedef struct {
    StrBuf input;
    Arena arena;
    JsonValue **messages;
    size_t count;
    int status;
} Session;

static void sessionInit(Session *session)
{
    strBufInit(&session->input);
    arenaInit(&session->arena);
    session->messages = NULL;
    session->count = 0;
    session->status = -1;
}

static void sessionFree(Session *session)
{
    strBufFree(&session->input);
    arenaFree(&session->arena);
    free(session->messages);
}

/* Appends one framed message, given as printf-style JSON. */
static void sendf(Session *session, const char *format, ...)
    DOTTR_PRINTF(2, 3);

static void sendf(Session *session, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    char *body = xmalloc((size_t)length + 1);
    vsnprintf(body, (size_t)length + 1, format, args);
    va_end(args);

    char header[64];
    snprintf(header, sizeof header, "Content-Length: %d\r\n\r\n", length);
    strBufAppendStr(&session->input, header);
    strBufAppend(&session->input, body, (size_t)length);
    free(body);
}

static void sendRaw(Session *session, const char *text)
{
    strBufAppendStr(&session->input, text);
}

/* Sends initialize with the given position encodings (JSON array text). */
static void sendInitialize(Session *session, const char *encodings)
{
    sendf(session,
          "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":"
          "{\"capabilities\":{\"general\":{\"positionEncodings\":%s},"
          "\"textDocument\":{\"hover\":{\"contentFormat\":[\"markdown\"]},"
          "\"completion\":{\"completionItem\":{\"snippetSupport\":false}}}}}}",
          encodings);
    sendf(session, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\","
                   "\"params\":{}}");
}

static void sendOpen(Session *session, const char *text)
{
    StrBuf escaped;
    strBufInit(&escaped);
    jsonWriteString(&escaped, text, strlen(text));
    sendf(session,
          "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
          "{\"textDocument\":{\"uri\":\"" URI "\",\"languageId\":\"dottr\","
          "\"version\":1,\"text\":%s}}}",
          escaped.data);
    strBufFree(&escaped);
}

/* Sends a request with textDocument and position parameters. */
static void sendAt(Session *session, int id, const char *method, int line,
                   int character, const char *extra)
{
    sendf(session,
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":"
          "{\"textDocument\":{\"uri\":\"" URI "\"},\"position\":"
          "{\"line\":%d,\"character\":%d}%s}}",
          id, method, line, character, extra ? extra : "");
}

static void sendShutdown(Session *session)
{
    sendf(session, "{\"jsonrpc\":\"2.0\",\"id\":999,\"method\":\"shutdown\"}");
    sendf(session, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
}

/* Runs the server on the session input and parses its output. */
static void run(Session *session)
{
    FILE *in = testInputStream(session->input.data, session->input.length);
    FILE *out = testOutputStream();
    session->status = lspRun(in, out);
    fclose(in);
    size_t outputLength;
    char *output = testCloseOutput(out, &outputLength);

    const char *cursor = output;
    const char *end = output + outputLength;
    while (cursor < end) {
        const char *body = strstr(cursor, "\r\n\r\n");
        if (!body) {
            break;
        }
        size_t length = strtoul(cursor + strlen("Content-Length: "), NULL, 10);
        body += 4;
        JsonValue *message = jsonParse(&session->arena, body, length);
        session->messages = xrealloc(session->messages, (session->count + 1) *
                                                            sizeof *session->messages);
        session->messages[session->count++] = message;
        cursor = body + length;
    }
    free(output);
}

/* Looks up a path such as "result.contents.value" or "result[1].name". */
static const JsonValue *at(const JsonValue *value, const char *path)
{
    char key[128];
    while (value && *path) {
        if (*path == '.') {
            path++;
        }
        if (*path == '[') {
            size_t index = strtoul(path + 1, (char **)&path, 10);
            path++;
            if (value->kind != JSON_ARRAY || index >= value->count) {
                return NULL;
            }
            value = value->items[index];
            continue;
        }
        size_t length = strcspn(path, ".[");
        if (length >= sizeof key) {
            return NULL;
        }
        memcpy(key, path, length);
        key[length] = '\0';
        value = jsonGet(value, key);
        path += length;
    }
    return value;
}

/* The response to a request id, or NULL. */
static const JsonValue *response(const Session *session, int id)
{
    for (size_t i = 0; i < session->count; i++) {
        if (jsonInt(jsonGet(session->messages[i], "id"), -1) == id) {
            return session->messages[i];
        }
    }
    return NULL;
}

/* The last publishDiagnostics notification, or NULL. */
static const JsonValue *lastDiagnostics(const Session *session)
{
    const JsonValue *found = NULL;
    for (size_t i = 0; i < session->count; i++) {
        const char *method = jsonString(jsonGet(session->messages[i], "method"));
        if (method && strcmp(method, "textDocument/publishDiagnostics") == 0) {
            found = session->messages[i];
        }
    }
    return found;
}

static void expectString(const char *name, const JsonValue *root,
                         const char *path, const char *expected)
{
    testCount++;
    const char *actual = jsonString(at(root, path));
    if (!actual || strcmp(actual, expected) != 0) {
        TEST_FAIL("lsp %s: %s is %s%s%s, expected \"%s\"", name, path,
                  actual ? "\"" : "", actual ? actual : "missing",
                  actual ? "\"" : "", expected);
    }
}

static void expectContains(const char *name, const JsonValue *root,
                           const char *path, const char *expected)
{
    testCount++;
    const char *actual = jsonString(at(root, path));
    if (!actual || !strstr(actual, expected)) {
        TEST_FAIL("lsp %s: %s is \"%s\", expected it to contain \"%s\"", name,
                  path, actual ? actual : "(missing)", expected);
    }
}

static void expectInt(const char *name, const JsonValue *root,
                      const char *path, int expected)
{
    testCount++;
    int actual = jsonInt(at(root, path), -12345);
    if (actual != expected) {
        TEST_FAIL("lsp %s: %s is %d, expected %d", name, path, actual,
                  expected);
    }
}

static void expectCount(const char *name, const JsonValue *root,
                        const char *path, size_t expected)
{
    testCount++;
    const JsonValue *value = at(root, path);
    if (!value || value->kind != JSON_ARRAY || value->count != expected) {
        TEST_FAIL("lsp %s: %s has %zu items, expected %zu", name, path,
                  value && value->kind == JSON_ARRAY ? value->count : 0,
                  expected);
    }
}

static void expectNull(const char *name, const JsonValue *root,
                       const char *path)
{
    testCount++;
    const JsonValue *value = at(root, path);
    if (!value || value->kind != JSON_NULL) {
        TEST_FAIL("lsp %s: %s is not null", name, path);
    }
}

/* Whether an array of completion items or symbols has an item labelled so. */
static bool hasLabel(const JsonValue *items, const char *field,
                     const char *label)
{
    for (size_t i = 0; items && items->kind == JSON_ARRAY && i < items->count;
         i++) {
        const char *text = jsonString(jsonGet(items->items[i], field));
        if (text && strcmp(text, label) == 0) {
            return true;
        }
    }
    return false;
}

static void expectLabels(const char *name, const JsonValue *items,
                         const char *const *present, const char *const *absent)
{
    for (size_t i = 0; present && present[i]; i++) {
        testCount++;
        if (!hasLabel(items, "label", present[i])) {
            TEST_FAIL("lsp %s: no completion \"%s\"", name, present[i]);
        }
    }
    for (size_t i = 0; absent && absent[i]; i++) {
        testCount++;
        if (hasLabel(items, "label", absent[i])) {
            TEST_FAIL("lsp %s: unexpected completion \"%s\"", name, absent[i]);
        }
    }
}

static const char manifest[] =
    "pkg git\n"                                          /* 0 */
    "pkg \"tree-sitter-cli\"\n"                          /* 1 */
    "\n"                                                 /* 2 */
    "pkg tmux do\n"                                      /* 3 */
    "  local TPM = \"@{dottr.env.home}/tpm\"\n"          /* 4 */
    "  local LOG = \"@{TPM}/log\"\n"                     /* 5 */
    "  needs git, \"tree-sitter-cli\"\n"                 /* 6 */
    "  pkg tmux\n"                                       /* 7 */
    "  check directory TPM\n"                            /* 8 */
    "  apply_if TPM do\n"                                /* 9 */
    "    shell do\n"                                     /* 10 */
    "      ```\n"                                        /* 11 */
    "      git -C \"$TPM\" pull\n"                       /* 12 */
    "      ```\n"                                        /* 13 */
    "    end\n"                                          /* 14 */
    "  end\n"                                            /* 15 */
    "end\n";                                             /* 16 */

static void testLifecycle(void)
{
    Session s;

    sessionInit(&s);
    sendInitialize(&s, "[\"utf-16\",\"utf-8\"]");
    sendShutdown(&s);
    run(&s);
    expectString("initialize", response(&s, 0),
                 "result.capabilities.positionEncoding", "utf-8");
    expectString("initialize", response(&s, 0), "result.serverInfo.name",
                 "dottr");
    expectInt("initialize", response(&s, 0),
              "result.capabilities.textDocumentSync.change", 1);
    expectNull("shutdown", response(&s, 999), "result");
    testCount++;
    if (s.status != 0) {
        TEST_FAIL("lsp exit after shutdown: status %d", s.status);
    }
    sessionFree(&s);

    sessionInit(&s);
    sendInitialize(&s, "[\"utf-16\"]");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    run(&s);
    expectString("utf-16 only", response(&s, 0),
                 "result.capabilities.positionEncoding", "utf-16");
    testCount++;
    if (s.status != 1) {
        TEST_FAIL("lsp exit without shutdown: status %d", s.status);
    }
    sessionFree(&s);

    sessionInit(&s);
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/hover\","
              "\"params\":{}}");
    sendInitialize(&s, "[]");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"no/such\"}");
    sendRaw(&s, "Content-Length: 5\r\n\r\n{nope");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":\"s\",\"method\":\"shutdown\"}");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\","
              "\"params\":{}}");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    run(&s);
    expectInt("before initialize", response(&s, 1), "error.code", -32002);
    expectInt("unknown method", response(&s, 2), "error.code", -32601);
    testCount++;
    bool parseError = false;
    for (size_t i = 0; i < s.count; i++) {
        parseError = parseError ||
                     jsonInt(at(s.messages[i], "error.code"), 0) == -32700;
    }
    if (!parseError) {
        TEST_FAIL("lsp invalid JSON: no parse error response");
    }
    expectInt("after shutdown", response(&s, 3), "error.code", -32600);
    sessionFree(&s);
}

static void testDiagnostics(void)
{
    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, "pkg a do\n  needs missing\n  check file NOPE\nend\n");
    run(&s);
    const JsonValue *d = lastDiagnostics(&s);
    expectCount("semantic diagnostics", d, "params.diagnostics", 2);
    expectContains("semantic diagnostics", d, "params.diagnostics[0].message",
                   "undefined variable `NOPE`");
    expectInt("semantic diagnostics", d,
              "params.diagnostics[0].range.start.line", 2);
    expectInt("semantic diagnostics", d,
              "params.diagnostics[0].range.start.character", 13);
    expectInt("semantic diagnostics", d,
              "params.diagnostics[0].range.end.character", 17);
    expectContains("semantic diagnostics", d, "params.diagnostics[1].message",
                   "unit `missing` is not declared");
    expectString("semantic diagnostics", d, "params.diagnostics[1].source",
                 "dottr");
    sessionFree(&s);

    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, "pkg tree-sitter\n");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
              "\"params\":{\"textDocument\":{\"uri\":\"" URI "\",\"version\":2},"
              "\"contentChanges\":[{\"text\":\"pkg \\\"tree-sitter\\\"\\n\"}]}}");
    run(&s);
    expectCount("fixed by change", lastDiagnostics(&s), "params.diagnostics",
                0);
    expectContains("lexical diagnostic", s.messages[1],
                   "params.diagnostics[0].message", "must be written as a string");
    sessionFree(&s);

    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, "pkg a do\n  needs b\nend\npkg b\n");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
              "\"params\":{\"textDocument\":{\"uri\":\"" URI "\",\"version\":2},"
              "\"contentChanges\":[{\"range\":{\"start\":{\"line\":1,"
              "\"character\":8},\"end\":{\"line\":1,\"character\":9}},"
              "\"text\":\"c\"}]}}");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
              "\"params\":{\"textDocument\":{\"uri\":\"" URI "\"}}}");
    run(&s);
    expectContains("ranged change", s.messages[s.count - 2],
                   "params.diagnostics[0].message", "unit `c` is not declared");
    expectCount("close clears", lastDiagnostics(&s), "params.diagnostics", 0);
    sessionFree(&s);
}

static void testNavigation(void)
{
    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, manifest);
    sendAt(&s, 1, "textDocument/definition", 6, 9, NULL);
    sendAt(&s, 2, "textDocument/definition", 5, 18, NULL);
    sendAt(&s, 3, "textDocument/definition", 9, 12, NULL);
    sendAt(&s, 4, "textDocument/definition", 4, 18, NULL);
    sendAt(&s, 5, "textDocument/definition", 6, 16, NULL);
    sendAt(&s, 6, "textDocument/references", 4, 9,
           ",\"context\":{\"includeDeclaration\":true}");
    sendAt(&s, 7, "textDocument/references", 0, 5,
           ",\"context\":{\"includeDeclaration\":false}");
    sendAt(&s, 8, "textDocument/references", 7, 7,
           ",\"context\":{\"includeDeclaration\":true}");
    run(&s);

    expectInt("definition of needs target", response(&s, 1),
              "result.range.start.line", 0);
    expectInt("definition of needs target", response(&s, 1),
              "result.range.start.character", 4);
    expectInt("definition of reference in string", response(&s, 2),
              "result.range.start.line", 4);
    expectInt("definition of reference in string", response(&s, 2),
              "result.range.start.character", 8);
    expectInt("definition of apply_if variable", response(&s, 3),
              "result.range.start.line", 4);
    expectNull("definition of built-in", response(&s, 4), "result");
    expectInt("definition of string unit name", response(&s, 5),
              "result.range.start.line", 1);
    expectInt("definition of string unit name", response(&s, 5),
              "result.range.end.character", 21);
    /* TPM: local, @{TPM} in LOG, check, apply_if, and "$TPM" in the shell. */
    expectCount("references of variable", response(&s, 6), "result", 5);
    expectCount("references of unit", response(&s, 7), "result", 1);
    expectInt("references of unit", response(&s, 7), "result[0].range.start.line",
              6);
    expectCount("child package has no references", response(&s, 8), "result",
                0);
    sessionFree(&s);
}

static void testRename(void)
{
    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, manifest);
    sendAt(&s, 1, "textDocument/rename", 8, 19, ",\"newName\":\"ROOT\"");
    sendAt(&s, 2, "textDocument/rename", 0, 5, ",\"newName\":\"git-core\"");
    sendAt(&s, 3, "textDocument/rename", 4, 9, ",\"newName\":\"PATH\"");
    sendAt(&s, 4, "textDocument/rename", 4, 9, ",\"newName\":\"LOG\"");
    sendAt(&s, 5, "textDocument/rename", 4, 9, ",\"newName\":\"file\"");
    sendAt(&s, 6, "textDocument/rename", 0, 5, ",\"newName\":\"tmux\"");
    sendAt(&s, 7, "textDocument/prepareRename", 5, 18, NULL);
    sendAt(&s, 8, "textDocument/prepareRename", 4, 18, NULL);
    sendAt(&s, 9, "textDocument/rename", 12, 8, ",\"newName\":\"X\"");
    run(&s);

    /* The URI contains '.', so it is looked up directly. */
    const JsonValue *edits = jsonGet(at(response(&s, 1), "result.changes"), URI);
    testCount++;
    if (!edits || edits->kind != JSON_ARRAY || edits->count != 5) {
        TEST_FAIL("lsp rename variable: expected 5 edits");
    } else {
        expectString("rename variable", edits, "[1].newText", "ROOT");
        /* The edit inside "@{TPM}/log" covers the name only. */
        expectInt("rename variable", edits, "[1].range.start.line", 5);
        expectInt("rename variable", edits, "[1].range.start.character", 17);
        expectInt("rename variable", edits, "[1].range.end.character", 20);
        /* The edit of "$TPM" in the shell block covers the name only. */
        expectInt("rename variable in shell", edits, "[4].range.start.line",
                  12);
        expectInt("rename variable in shell", edits,
                  "[4].range.start.character", 15);
        expectInt("rename variable in shell", edits,
                  "[4].range.end.character", 18);
    }
    const JsonValue *unitEdits =
        jsonGet(at(response(&s, 2), "result.changes"), URI);
    testCount++;
    if (!unitEdits || unitEdits->kind != JSON_ARRAY || unitEdits->count != 2) {
        TEST_FAIL("lsp rename unit: expected 2 edits");
    } else {
        expectString("rename unit to string form", unitEdits, "[0].newText",
                     "\"git-core\"");
    }
    expectInt("rename to protected name", response(&s, 3), "error.code",
              -32803);
    expectInt("rename to existing variable", response(&s, 4), "error.code",
              -32803);
    expectInt("rename to keyword", response(&s, 5), "error.code", -32803);
    expectInt("rename to existing unit", response(&s, 6), "error.code",
              -32803);
    expectString("prepareRename", response(&s, 7), "result.placeholder", "TPM");
    expectInt("prepareRename", response(&s, 7), "result.range.start.character",
              17);
    expectNull("prepareRename built-in", response(&s, 8), "result");
    expectInt("rename in shell text", response(&s, 9), "error.code", -32803);
    sessionFree(&s);
}

static void testHover(void)
{
    const char *savedHome = getenv("HOME");
    char *home = savedHome ? strcpy(xmalloc(strlen(savedHome) + 1), savedHome)
                           : NULL;
    const char *savedProfile = getenv("USERPROFILE");
    char *profile = savedProfile ? strcpy(xmalloc(strlen(savedProfile) + 1),
                                          savedProfile)
                                 : NULL;
    testSetVariable("HOME", "/home/test");
    testSetVariable("USERPROFILE", "/home/test");

    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, manifest);
    sendAt(&s, 1, "textDocument/hover", 5, 9, NULL);
    sendAt(&s, 2, "textDocument/hover", 3, 5, NULL);
    sendAt(&s, 3, "textDocument/hover", 4, 20, NULL);
    sendAt(&s, 4, "textDocument/hover", 7, 7, NULL);
    sendAt(&s, 5, "textDocument/hover", 8, 4, NULL);
    sendAt(&s, 6, "textDocument/hover", 0, 5, NULL);
    sendAt(&s, 7, "textDocument/hover", 2, 0, NULL);
    run(&s);

    expectContains("hover variable", response(&s, 1), "result.contents.value",
                   "local LOG = \"@{TPM}/log\"");
    expectContains("hover variable value", response(&s, 1),
                   "result.contents.value", "`/home/test/tpm/log`");
    expectString("hover markdown", response(&s, 1), "result.contents.kind",
                 "markdown");
    expectContains("hover block unit", response(&s, 2), "result.contents.value",
                   "Needs `git`, `tree-sitter-cli`");
    expectContains("hover built-in", response(&s, 3), "result.contents.value",
                   "`/home/test`");
    expectContains("hover child package", response(&s, 4),
                   "result.contents.value", "`tmux-child-tmux`");
    expectContains("hover keyword", response(&s, 5), "result.contents.value",
                   "records the result as a fact");
    expectContains("hover bare unit", response(&s, 6), "result.contents.value",
                   "Installs the package `git`");
    expectNull("hover nothing", response(&s, 7), "result");
    sessionFree(&s);

    testSetVariable("HOME", home);
    testSetVariable("USERPROFILE", profile);
    free(home);
    free(profile);
}

static void testSymbols(void)
{
    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, manifest);
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
              "\"textDocument/documentSymbol\",\"params\":{\"textDocument\":"
              "{\"uri\":\"" URI "\"}}}");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"workspace/symbol\","
              "\"params\":{\"query\":\"tp\"}}");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":"
              "\"textDocument/formatting\",\"params\":{\"textDocument\":"
              "{\"uri\":\"" URI "\"},\"options\":{\"tabSize\":2,"
              "\"insertSpaces\":true}}}");
    run(&s);

    const JsonValue *symbols = response(&s, 1);
    expectCount("document symbols", symbols, "result", 3);
    expectString("document symbols", symbols, "result[2].name", "tmux");
    expectInt("document symbols", symbols, "result[2].kind", 2);
    expectInt("unit symbol spans to end", symbols, "result[2].range.end.line",
              16);
    expectCount("unit children", symbols, "result[2].children", 3);
    expectString("unit children", symbols, "result[2].children[2].detail",
                 "tmux-child-tmux");
    expectCount("workspace symbols", response(&s, 2), "result", 1);
    expectString("workspace symbols", response(&s, 2), "result[0].containerName",
                 "tmux");
    expectCount("formatting a formatted document", response(&s, 3), "result",
                0);
    sessionFree(&s);

    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, "pkg   a do\nneeds b\nend\npkg b");
    sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
              "\"textDocument/formatting\",\"params\":{\"textDocument\":"
              "{\"uri\":\"" URI "\"}}}");
    run(&s);
    expectString("formatting", response(&s, 1), "result[0].newText",
                 "pkg a do\n  needs b\nend\npkg b\n");
    expectInt("formatting", response(&s, 1), "result[0].range.end.line", 3);
    sessionFree(&s);
}

static void testCompletion(void)
{
    static const char text[] =
        "pkg git\n"                       /* 0 */
        "pkg \"tree-sitter-cli\"\n"       /* 1 */
        "pkg app do\n"                    /* 2 */
        "  local ROOT = \"x\"\n"          /* 3 */
        "  local CONF = \"x\"\n"          /* 4 */
        "  check directory ROOT\n"        /* 5 */
        "  needs git, \n"                 /* 6 */
        "  apply_if \n"                   /* 7 */
        "  local B = \"@{\"\n"            /* 8 */
        "  check \n"                      /* 9 */
        "  check file \n"                 /* 10 */
        "  apply_if ROOT do\n"            /* 11 */
        "    \n"                          /* 12 */
        "    shell do\n"                  /* 13 */
        "      ```\n"                     /* 14 */
        "      ech\n"                     /* 15 */
        "      ```\n"                     /* 16 */
        "    end\n"                       /* 17 */
        "  end\n"                         /* 18 */
        "  lo\n"                          /* 19 */
        "end\n"                           /* 20 */
        "p\n";                            /* 21 */

    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, text);
    sendAt(&s, 1, "textDocument/completion", 6, 13, NULL);
    sendAt(&s, 2, "textDocument/completion", 7, 11, NULL);
    sendAt(&s, 3, "textDocument/completion", 8, 15, NULL);
    sendAt(&s, 4, "textDocument/completion", 9, 8, NULL);
    sendAt(&s, 5, "textDocument/completion", 10, 13, NULL);
    sendAt(&s, 6, "textDocument/completion", 12, 4, NULL);
    sendAt(&s, 7, "textDocument/completion", 15, 9, NULL);
    sendAt(&s, 8, "textDocument/completion", 19, 4, NULL);
    sendAt(&s, 9, "textDocument/completion", 21, 1, NULL);
    run(&s);

    const char *const unitsPresent[] = {"tree-sitter-cli", NULL};
    const char *const unitsAbsent[] = {"git", "app", NULL};
    expectLabels("needs", at(response(&s, 1), "result"), unitsPresent,
                 unitsAbsent);
    for (size_t i = 0; i < at(response(&s, 1), "result")->count; i++) {
        const JsonValue *item = at(response(&s, 1), "result")->items[i];
        if (strcmp(jsonString(jsonGet(item, "label")), "tree-sitter-cli") == 0) {
            expectString("needs quotes", item, "insertText",
                         "\"tree-sitter-cli\"");
        }
    }
    const char *const checked[] = {"ROOT", NULL};
    const char *const unchecked[] = {"CONF", NULL};
    expectLabels("apply_if", at(response(&s, 2), "result"), checked,
                 unchecked);
    const char *const references[] = {"ROOT", "CONF", "dottr.env.home", NULL};
    expectLabels("reference", at(response(&s, 3), "result"), references, NULL);
    expectString("reference edit", response(&s, 3), "result[0].textEdit.newText",
                 "ROOT}");
    const char *const kinds[] = {"command", "file", "directory", NULL};
    expectLabels("check", at(response(&s, 4), "result"), kinds, NULL);
    const char *const variables[] = {"ROOT", "CONF", NULL};
    expectLabels("check file", at(response(&s, 5), "result"), variables, NULL);
    const char *const applyIfBody[] = {"shell", "else", "end", NULL};
    const char *const notInApply[] = {"local", "pkg", NULL};
    expectLabels("apply_if body", at(response(&s, 6), "result"), applyIfBody,
                 notInApply);
    expectCount("raw block", response(&s, 7), "result", 0);
    const char *const unitBody[] = {"local", "needs", "check", "apply", NULL};
    const char *const notInUnit[] = {"shell", "else", NULL};
    expectLabels("unit body", at(response(&s, 8), "result"), unitBody,
                 notInUnit);
    const char *const topLevel[] = {"pkg", NULL};
    const char *const notTopLevel[] = {"local", NULL};
    expectLabels("top level", at(response(&s, 9), "result"), topLevel,
                 notTopLevel);
    sessionFree(&s);
}

static void testShellUses(void)
{
    static const char text[] =
        "pkg a do\n"                                            /* 0 */
        "  local V = \"x\"\n"                                   /* 1 */
        "  apply do\n"                                          /* 2 */
        "    shell do\n"                                        /* 3 */
        "      ```\n"                                           /* 4 */
        "      echo \"$V\" ${V} ${V:-d} '$V' \\$V $V2 $LATE\n"   /* 5 */
        "      ```\n"                                           /* 6 */
        "    end\n"                                             /* 7 */
        "  end\n"                                               /* 8 */
        "  local LATE = \"y\"\n"                                /* 9 */
        "end\n";                                                /* 10 */
    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, text);
    sendAt(&s, 1, "textDocument/references", 1, 8,
           ",\"context\":{\"includeDeclaration\":true}");
    sendAt(&s, 2, "textDocument/references", 9, 9,
           ",\"context\":{\"includeDeclaration\":true}");
    sendAt(&s, 3, "textDocument/hover", 5, 19, NULL);
    sendAt(&s, 4, "textDocument/definition", 5, 25, NULL);
    run(&s);
    /* The local, "$V", ${V}, and ${V:-d}; not '$V', \$V, or $V2. */
    expectCount("shell uses", response(&s, 1), "result", 4);
    expectInt("shell use range", response(&s, 1), "result[1].range.start.character",
              12);
    expectInt("shell use range", response(&s, 1), "result[2].range.end.character",
              20);
    /* $LATE is used before LATE is bound. */
    expectCount("shell use before binding", response(&s, 2), "result", 1);
    expectContains("hover shell use", response(&s, 3), "result.contents.value",
                   "local V = \"x\"");
    expectInt("definition from shell use", response(&s, 4),
              "result.range.start.line", 1);
    sessionFree(&s);
}

static void testUtf16(void)
{
    /*
     * U+00E9 is one UTF-16 unit and two UTF-8 bytes; U+1F600, outside the
     * Basic Multilingual Plane, is two units and four bytes.
     */
    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-16\"]");
    sendOpen(&s, "pkg a do\n  local A = \"x\"\n  local B = \"\xC3\xA9\xF0\x9F"
                 "\x98\x80@{A}\"\n  check file MISSING\nend\n");
    sendAt(&s, 1, "textDocument/definition", 2, 18, NULL);
    sendAt(&s, 2, "textDocument/references", 1, 8,
           ",\"context\":{\"includeDeclaration\":false}");
    run(&s);
    expectInt("utf-16 position in", response(&s, 1), "result.range.start.line",
              1);
    /* `  local B = "` is 13 units, U+00E9 1, and U+1F600 2: @{A} is at 16. */
    expectInt("utf-16 position out", response(&s, 2),
              "result[0].range.start.character", 16);
    expectInt("utf-16 position out", response(&s, 2),
              "result[0].range.end.character", 20);
    sessionFree(&s);
}

/* `on` blocks: symbols per system, hover, diagnostics, and completion. */
static void testOnBlocks(void)
{
    static const char text[] =
        "pkg git\n"                          /* 0 */
        "on macos do\n"                      /* 1 */
        "  pkg rectangle\n"                  /* 2 */
        "end\n"                              /* 3 */
        "pkg app do\n"                       /* 4 */
        "  on linux, macos do\n"             /* 5 */
        "    local DIR = \"a\"\n"            /* 6 */
        "  end\n"                            /* 7 */
        "  on windows do\n"                  /* 8 */
        "    local DIR = \"b\"\n"            /* 9 */
        "  end\n"                            /* 10 */
        "  check directory DIR\n"            /* 11 */
        "  apply_if DIR do\n"                /* 12 */
        "  end\n"                            /* 13 */
        "  needs rectangle\n"                /* 14 */
        "end\n";                             /* 15 */

    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, text);
    sendAt(&s, 1, "textDocument/rename", 9, 11, ",\"newName\":\"ROOT\"");
    sendAt(&s, 2, "textDocument/hover", 5, 6, NULL);
    sendAt(&s, 3, "textDocument/hover", 5, 2, NULL);
    sendAt(&s, 4, "textDocument/hover", 2, 6, NULL);
    sendAt(&s, 5, "textDocument/definition", 11, 19, NULL);
    run(&s);

    const JsonValue *edits = jsonGet(at(response(&s, 1), "result.changes"), URI);
    testCount++;
    if (!edits || edits->kind != JSON_ARRAY || edits->count != 4) {
        TEST_FAIL("lsp on: rename a variable bound per system: expected 4 "
                  "edits");
    }
    expectContains("lsp on: hover selector", response(&s, 2),
                   "result.contents.value", "Linux, with pacman");
    expectContains("lsp on: hover keyword", response(&s, 3),
                   "result.contents.value", "selects its contents");
    expectContains("lsp on: hover unit of one system", response(&s, 4),
                   "result.contents.value", "Exists on macos only");
    expectInt("lsp on: definition", response(&s, 5),
              "result.range.start.line", 6);
    const JsonValue *d = lastDiagnostics(&s);
    expectCount("lsp on: diagnostics", d, "params.diagnostics", 1);
    expectContains("lsp on: diagnostics", d, "params.diagnostics[0].message",
                   "unit `rectangle` is not declared on this system");
    expectContains("lsp on: diagnostics", d, "params.diagnostics[0].message",
                   "(on linux, windows)");
    sessionFree(&s);

    static const char partial[] =
        "on \n"                              /* 0 */
        "on linux, \n"                       /* 1 */
        "pkg a do\n"                         /* 2 */
        "  on linux do\n"                    /* 3 */
        "    \n"                             /* 4 */
        "  end\n"                            /* 5 */
        "  apply_if X do\n"                  /* 6 */
        "    on linux do\n"                  /* 7 */
        "      \n"                           /* 8 */
        "    end\n"                          /* 9 */
        "  end\n"                            /* 10 */
        "end\n";                             /* 11 */

    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, partial);
    sendAt(&s, 1, "textDocument/completion", 0, 3, NULL);
    sendAt(&s, 2, "textDocument/completion", 1, 10, NULL);
    sendAt(&s, 3, "textDocument/completion", 4, 4, NULL);
    sendAt(&s, 4, "textDocument/completion", 8, 6, NULL);
    run(&s);

    const char *const selectors[] = {"linux", "macos", "windows", "pacman",
                                     "apt", "winget", NULL};
    expectLabels("lsp on: selectors", at(response(&s, 1), "result"),
                 selectors, NULL);
    const char *const remaining[] = {"macos", "apt", NULL};
    const char *const listed[] = {"linux", NULL};
    expectLabels("lsp on: remaining selectors", at(response(&s, 2), "result"),
                 remaining, listed);
    const char *const unitBody[] = {"local", "needs", "on", "end", NULL};
    const char *const notInUnit[] = {"shell", NULL};
    expectLabels("lsp on: unit body", at(response(&s, 3), "result"), unitBody,
                 notInUnit);
    const char *const actions[] = {"shell", "on", "end", NULL};
    const char *const noElse[] = {"else", "local", NULL};
    expectLabels("lsp on: apply_if body", at(response(&s, 4), "result"),
                 actions, noElse);
    sessionFree(&s);
}

static void testSymlink(void)
{
    static const char text[] =
        "pkg config do\n"
        "  local SOURCE = \"source\"\n"
        "  local TARGET = \"target\"\n"
        "  local RULES = \"rules\"\n"
        "  link SOURCE TARGET\n"
        "  link tree SOURCE TARGET ignore RULES\n"
        "end\n";
    Session s;
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, text);
    sendAt(&s, 1, "textDocument/definition", 4, 12, NULL);
    sendAt(&s, 2, "textDocument/definition", 5, 24, NULL);
    sendAt(&s, 3, "textDocument/hover", 4, 4, NULL);
    sendAt(&s, 4, "textDocument/references", 1, 9,
           ",\"context\":{\"includeDeclaration\":false}");
    sendAt(&s, 5, "textDocument/rename", 1, 9, ",\"newName\":\"REPO\"");
    sendAt(&s, 6, "textDocument/hover", 5, 8, NULL);
    sendAt(&s, 7, "textDocument/definition", 5, 34, NULL);
    sendAt(&s, 8, "textDocument/hover", 5, 28, NULL);
    run(&s);
    expectCount("symlink diagnostics", lastDiagnostics(&s), "params.diagnostics", 0);
    expectInt("symlink source definition", response(&s, 1), "result.range.start.line", 1);
    expectInt("symlink target definition", response(&s, 2), "result.range.start.line", 2);
    expectContains("symlink hover", response(&s, 3), "result.contents.value", "symbolic link");
    expectContains("symlink tree hover", response(&s, 6), "result.contents.value", "recursively");
    expectInt("link ignore definition", response(&s, 7), "result.range.start.line", 3);
    expectContains("link ignore hover", response(&s, 8), "result.contents.value", "ignore file");
    expectCount("symlink references", response(&s, 4), "result", 2);
    const JsonValue *edits = jsonGet(at(response(&s, 5), "result.changes"), URI);
    testCount++;
    if (!edits || edits->kind != JSON_ARRAY || edits->count != 3) {
        TEST_FAIL("lsp symlink rename: expected 3 edits");
    }
    sessionFree(&s);

    static const char partial[] =
        "pkg config do\n"
        "  local SOURCE = \"source\"\n"
        "  local TARGET = \"target\"\n"
        "  local RULES = \"rules\"\n"
        "  link \n"
        "  link tree \n"
        "  link SOURCE \n"
        "  link tree SOURCE \n"
        "  link tree SOURCE TARGET \n"
        "  link tree SOURCE TARGET ignore \n"
        "end\n";
    sessionInit(&s);
    sendInitialize(&s, "[\"utf-8\"]");
    sendOpen(&s, partial);
    sendAt(&s, 1, "textDocument/completion", 4, 7, NULL);
    sendAt(&s, 2, "textDocument/completion", 5, 12, NULL);
    sendAt(&s, 3, "textDocument/completion", 6, 14, NULL);
    sendAt(&s, 4, "textDocument/completion", 7, 19, NULL);
    sendAt(&s, 5, "textDocument/completion", 8, 26, NULL);
    sendAt(&s, 6, "textDocument/completion", 9, 33, NULL);
    run(&s);
    const char *const source[] = {"SOURCE", "TARGET", "RULES", "tree", NULL};
    const char *const variables[] = {"SOURCE", "TARGET", "RULES", NULL};
    expectLabels("symlink source completion", at(response(&s, 1), "result"), source, NULL);
    for (int i = 2; i <= 4; i++) {
        expectLabels("symlink operand completion", at(response(&s, i), "result"), variables, NULL);
    }
    const char *const modifier[] = {"ignore", NULL};
    expectLabels("link ignore modifier", at(response(&s, 5), "result"), modifier, NULL);
    expectLabels("link ignore operand", at(response(&s, 6), "result"), variables, NULL);
    sessionFree(&s);
}

void runLspTests(void)
{
    testLifecycle();
    testDiagnostics();
    testNavigation();
    testRename();
    testHover();
    testSymbols();
    testCompletion();
    testShellUses();
    testUtf16();
    testOnBlocks();
    testSymlink();
}
