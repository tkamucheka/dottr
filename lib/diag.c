#include "dottr/diag.h"

#include <stdarg.h>

void diagInit(Diag *diag, const char *path, FILE *stream)
{
    diag->path = path;
    diag->stream = stream;
    diag->errorCount = 0;
}

void diagError(Diag *diag, int line, int column, const char *format, ...)
{
    diag->errorCount++;
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
