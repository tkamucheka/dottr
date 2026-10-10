#ifndef DOTTR_FORMAT_H
#define DOTTR_FORMAT_H

#include <stdbool.h>
#include <stddef.h>

#include "dottr/diag.h"
#include "dottr/util.h"

/*
 * Formats a manifest and appends the
 * result to out. Returns false after reporting the first lexical or syntax
 * error to diag; out is then unchanged.
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

#endif
