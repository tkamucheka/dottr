#ifndef DOTTR_DIAG_H
#define DOTTR_DIAG_H

#include <stdio.h>

#if defined(__GNUC__)
#define DOTTR_PRINTF(formatIndex, firstArg) \
    __attribute__((format(printf, formatIndex, firstArg)))
#else
#define DOTTR_PRINTF(formatIndex, firstArg)
#endif

/* Diagnostic sink for one manifest. */
typedef struct {
    const char *path;
    FILE *stream;
    int errorCount;
} Diag;

void diagInit(Diag *diag, const char *path, FILE *stream);

/*
 * Reports an error as PATH:LINE:COLUMN: error: MESSAGE. A line of 0 omits the
 * position; a column of 0 omits the column.
 */
void diagError(Diag *diag, int line, int column, const char *format, ...)
    DOTTR_PRINTF(4, 5);

#endif
