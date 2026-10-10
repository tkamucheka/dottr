#include "dottr/diag.h"

#include <stdarg.h>
#include <stdlib.h>

void diagInit(Diag *diag, const char *path, FILE *stream)
{
    diag->path = path;
    diag->stream = stream;
    diag->errorCount = 0;
    diag->sink = NULL;
    diag->sinkContext = NULL;
}

void diagInitSink(Diag *diag, DiagSink sink, void *context)
{
    diagInit(diag, "", NULL);
    diag->sink = sink;
    diag->sinkContext = context;
}

void diagError(Diag *diag, int line, int column, const char *format, ...)
{
    diag->errorCount++;
    if (diag->sink) {
        va_list args;
        va_start(args, format);
        va_list copy;
        va_copy(copy, args);
        int length = vsnprintf(NULL, 0, format, copy);
        va_end(copy);
        char *message = malloc(length > 0 ? (size_t)length + 1 : 1);
        if (message) {
            vsnprintf(message, (size_t)length + 1, format, args);
            diag->sink(diag->sinkContext, line, column, message);
            free(message);
        }
        va_end(args);
        return;
    }
    if (!diag->stream) {
        return;
    }

    fflush(stdout);
    if (line > 0 && column > 0) {
        fprintf(diag->stream, "%s:%d:%d: error: ", diag->path, line, column);
    } else if (line > 0) {
        fprintf(diag->stream, "%s:%d: error: ", diag->path, line);
    } else {
        fprintf(diag->stream, "%s: error: ", diag->path);
    }

    va_list args;
    va_start(args, format);
    vfprintf(diag->stream, format, args);
    va_end(args);
    fputc('\n', diag->stream);
    fflush(diag->stream);
}
