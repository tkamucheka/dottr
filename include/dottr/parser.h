#ifndef DOTTR_PARSER_H
#define DOTTR_PARSER_H

#include <stdbool.h>

#include "dottr/ast.h"
#include "dottr/diag.h"
#include "dottr/lexer.h"
#include "dottr/util.h"

/*
 * Builds the syntax tree of a manifest from its tokens. All nodes are
 * allocated in arena. Returns false after reporting the first syntax error
 * to diag.
 */
bool parseManifest(Arena *arena, const TokenList *tokens, Manifest *manifest,
                   Diag *diag);

#endif
