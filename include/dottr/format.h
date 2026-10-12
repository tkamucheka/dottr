#ifndef DOTTR_FORMAT_H
#define DOTTR_FORMAT_H

#include <stdbool.h>
#include <stddef.h>

#include "dottr/diag.h"
#include "dottr/util.h"

/*
 * Formats a manifest and appends the result to out. Returns false after
 * reporting the first lexical or syntax error to diag; out is then unchanged.
 */
bool formatManifest(const char *source, size_t length, StrBuf *out,
                    Diag *diag);

/*
 * Returns true if two manifests have the same token sequence, the same raw
 * block contents and dialects, and the same comment texts, ignoring trailing
 * spaces, tabs, and carriage returns in comments. Returns false if either
 * does not lex. Used to check that formatting preserves a manifest.
 */
bool formatEquivalent(const char *a, size_t aLength, const char *b,
                      size_t bLength);

typedef enum {
    /* Write the formatted manifest to standard output. */
    FORMAT_PRINT,
    /* Rewrite the file if its formatting changes. */
    FORMAT_WRITE,
    /* Write nothing; report whether the file is formatted. */
    FORMAT_CHECK,
} FormatMode;

/*
 * Formats the manifest at path, or standard input if path is "-", for the
 * command-line programs (docs/reference/formatting.md). Messages are written
 * to standard error, prefixed with program. Returns 0 on success, 1 if
 * FORMAT_CHECK finds the file unformatted, and 2 if the file cannot be read
 * or written or does not lex and parse. FORMAT_WRITE requires a file path.
 */
int formatPath(const char *program, const char *path, FormatMode mode);

#endif
