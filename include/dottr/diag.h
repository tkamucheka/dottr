#ifndef DOTTR_DIAG_H
#define DOTTR_DIAG_H

#include <stdio.h>

#if defined(__GNUC__)
#define DOTTR_PRINTF(formatIndex, firstArg) \
    __attribute__((format(printf, formatIndex, firstArg)))
#else
#define DOTTR_PRINTF(formatIndex, firstArg)
#endif

/*
 * Receives a formatted diagnostic instead of the stream. line and column are
 * as for diagError.
 */
typedef void (*DiagSink)(void *context, int line, int column,
                         const char *message);

/* Diagnostic sink for one manifest. */
typedef struct {
    const char *path;
    FILE *stream;
    int errorCount;
    /* If set, diagnostics go to sink rather than to stream. */
    DiagSink sink;
    void *sinkContext;
} Diag;

void diagInit(Diag *diag, const char *path, FILE *stream);

/* Initializes diag to deliver diagnostics to sink. */
void diagInitSink(Diag *diag, DiagSink sink, void *context);

/*
 * Reports an error as PATH:LINE:COLUMN: error: MESSAGE, or passes it to the
 * sink. A line of 0 omits the position; a column of 0 omits the column.
 */
void diagError(Diag *diag, int line, int column, const char *format, ...)
    DOTTR_PRINTF(4, 5);

#endif
