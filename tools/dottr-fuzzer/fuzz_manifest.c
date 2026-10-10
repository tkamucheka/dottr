/*
 * libFuzzer harness for the dottr front end. Each input is treated as a
 * manifest and passed through lexing, parsing, and semantic analysis, the
 * phases that run before any host state is observed. Diagnostics are
 * formatted into /dev/null so that the formatting of every message is
 * exercised.
 *
 * Every input that lexes and parses is also formatted, and the harness aborts
 * if the formatted manifest is not equivalent to the input
 * (formatEquivalent) or if formatting it again changes it.
 *
 * Every input is also parsed as JSON, and a value that parses must serialize
 * and parse back to the same serialization. Finally, the input is opened as
 * a document in a language server session that requests every feature at a
 * set of positions. Build and run instructions are in AGENTS.md.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/diag.h"
#include "dottr/format.h"
#include "dottr/json.h"
#include "dottr/lexer.h"
#include "dottr/lsp.h"
#include "dottr/parser.h"
#include "dottr/sema.h"
#include "dottr/util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static FILE *nullStream(void)
{
    static FILE *stream;
    if (!stream) {
        stream = fopen("/dev/null", "w");
        if (!stream) {
            perror("fuzz: cannot open /dev/null");
            abort();
        }
    }
    return stream;
}

static void checkFormat(const char *source, size_t size)
{
    Diag diag;
    diagInit(&diag, "fuzz.dotfile", nullStream());
    StrBuf once;
    StrBuf twice;
    strBufInit(&once);
    strBufInit(&twice);

    if (!formatManifest(source, size, &once, &diag)) {
        fputs("fuzz: a manifest that parses was not formatted\n", stderr);
        abort();
    }
    if (!formatEquivalent(source, size, once.data, once.length)) {
        fputs("fuzz: formatted manifest is not equivalent\n", stderr);
        abort();
    }
    if (!formatManifest(once.data, once.length, &twice, &diag) ||
        twice.length != once.length ||
        memcmp(twice.data, once.data, once.length) != 0) {
        fputs("fuzz: formatting is not idempotent\n", stderr);
        abort();
    }

    strBufFree(&once);
    strBufFree(&twice);
}

static void checkJson(const char *source, size_t size)
{
    Arena arena;
    arenaInit(&arena);
    JsonValue *value = jsonParse(&arena, source, size);
    if (value) {
        StrBuf once;
        StrBuf twice;
        strBufInit(&once);
        strBufInit(&twice);
        jsonWriteValue(&once, value);
        JsonValue *again = jsonParse(&arena, once.data, once.length);
        if (!again) {
            fputs("fuzz: serialized JSON does not parse\n", stderr);
            abort();
        }
        jsonWriteValue(&twice, again);
        if (twice.length != once.length ||
            memcmp(twice.data, once.data, once.length) != 0) {
            fputs("fuzz: JSON serialization is not stable\n", stderr);
            abort();
        }
        strBufFree(&once);
        strBufFree(&twice);
    }
    arenaFree(&arena);
}

static void frame(StrBuf *session, const char *body, size_t length)
{
    char header[64];
    snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", length);
    strBufAppendStr(session, header);
    strBufAppend(session, body, length);
}

static void frameString(StrBuf *session, const char *body)
{
    frame(session, body, strlen(body));
}

/* Runs a language server session on the input as a document. */
static void checkServer(const char *source, size_t size)
{
    static const char *const methods[] = {
        "textDocument/hover", "textDocument/completion",
        "textDocument/definition", "textDocument/prepareRename",
        "textDocument/references",
    };
    static const int positions[][2] = {
        {0, 0}, {0, 5}, {1, 2}, {2, 10}, {3, 4}, {5, 1}, {10, 3}, {100, 100},
    };

    StrBuf session;
    StrBuf body;
    strBufInit(&session);
    strBufInit(&body);
    frameString(&session,
                "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\","
                "\"params\":{\"capabilities\":{}}}");
    strBufAppendStr(&body, "{\"jsonrpc\":\"2.0\",\"method\":"
                           "\"textDocument/didOpen\",\"params\":"
                           "{\"textDocument\":{\"uri\":\"file:///f\","
                           "\"version\":1,\"text\":");
    jsonWriteString(&body, source, size);
    strBufAppendStr(&body, "}}}");
    frame(&session, body.data, body.length);

    int id = 1;
    char request[512];
    for (size_t m = 0; m < sizeof methods / sizeof methods[0]; m++) {
        for (size_t p = 0; p < sizeof positions / sizeof positions[0]; p++) {
            snprintf(request, sizeof request,
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\","
                     "\"params\":{\"textDocument\":{\"uri\":\"file:///f\"},"
                     "\"position\":{\"line\":%d,\"character\":%d},"
                     "\"newName\":\"N\",\"context\":"
                     "{\"includeDeclaration\":true}}}",
                     id++, methods[m], positions[p][0], positions[p][1]);
            frameString(&session, request);
        }
    }
    snprintf(request, sizeof request,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":"
             "\"textDocument/rename\",\"params\":{\"textDocument\":"
             "{\"uri\":\"file:///f\"},\"position\":{\"line\":0,"
             "\"character\":4},\"newName\":\"renamed\"}}",
             id++);
    frameString(&session, request);
    frameString(&session, "{\"jsonrpc\":\"2.0\",\"id\":900,\"method\":"
                          "\"textDocument/documentSymbol\",\"params\":"
                          "{\"textDocument\":{\"uri\":\"file:///f\"}}}");
    frameString(&session, "{\"jsonrpc\":\"2.0\",\"id\":901,\"method\":"
                          "\"textDocument/formatting\",\"params\":"
                          "{\"textDocument\":{\"uri\":\"file:///f\"}}}");
    frameString(&session, "{\"jsonrpc\":\"2.0\",\"id\":902,\"method\":"
                          "\"workspace/symbol\",\"params\":{\"query\":\"\"}}");
    frameString(&session, "{\"jsonrpc\":\"2.0\",\"id\":903,\"method\":"
                          "\"shutdown\"}");
    frameString(&session, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    FILE *in = fmemopen(session.data, session.length, "r");
    if (lspRun(in, nullStream()) != 0) {
        fputs("fuzz: language server session did not end cleanly\n", stderr);
        abort();
    }
    fclose(in);
    strBufFree(&session);
    strBufFree(&body);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /*
     * An exact-size copy without a terminating NUL, so that any read past
     * the given length is reported by AddressSanitizer.
     */
    char *source = xmalloc(size > 0 ? size : 1);
    if (size > 0) {
        memcpy(source, data, size);
    }

    Arena arena;
    arenaInit(&arena);
    Diag diag;
    diagInit(&diag, "fuzz.dotfile", nullStream());

    TokenList tokens;
    Manifest manifest;
    bool parsed = lexSource(&arena, source, size, &tokens, &diag) &&
                  parseManifest(&arena, &tokens, &manifest, &diag);
    if (parsed) {
        analyzeManifest(&arena, &manifest, &diag);
        checkFormat(source, size);
    }

    arenaFree(&arena);
    checkJson(source, size);
    checkServer(source, size);
    free(source);
    return 0;
}
