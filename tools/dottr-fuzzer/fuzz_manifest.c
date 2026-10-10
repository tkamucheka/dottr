/*
 * libFuzzer harness for the dottr front end. Each input is treated as a
 * manifest and passed through lexing, parsing, and semantic analysis, the
 * phases that run before any host state is observed. Diagnostics are
 * formatted into /dev/null so that the formatting of every message is
 * exercised. Build and run instructions are in AGENTS.md.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/diag.h"
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
    if (lexSource(&arena, source, size, &tokens, &diag) &&
        parseManifest(&arena, &tokens, &manifest, &diag)) {
        analyzeManifest(&arena, &manifest, &diag);
    }

    arenaFree(&arena);
    free(source);
    return 0;
}
