/*
 * libFuzzer harness for the dottr front end. Each input is treated as a
 * manifest and passed through lexing, parsing, and semantic analysis, the
 * phases that run before any host state is observed. Diagnostics are
 * formatted into /dev/null so that the formatting of every message is
 * exercised.
 *
 * Every input that lexes and parses is also formatted, and the harness aborts
 * if the formatted manifest is not equivalent to the input
 * (formatEquivalent) or if formatting it again changes it. Build and run
 * instructions are in AGENTS.md.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/diag.h"
#include "dottr/format.h"
#include "dottr/lexer.h"
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
    free(source);
    return 0;
}
