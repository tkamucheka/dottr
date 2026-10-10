#ifndef DOTTR_LSP_H
#define DOTTR_LSP_H

#include <stdio.h>

/*
 * Runs the language server on the given streams until
 * the client sends `exit` or input ends. Returns 0 if `exit` followed
 * `shutdown`, and 1 otherwise.
 */
int lspRun(FILE *in, FILE *out);

#endif
