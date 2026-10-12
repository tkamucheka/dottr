/*
 * dottr-format: formats dottr manifests (docs/reference/formatting.md). The
 * formatting is that of `dottr fmt`; this program takes files as arguments,
 * as standalone formatters do.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dottr/format.h"
#include "dottr/host.h"

#define EXIT_USAGE 2

static void printUsage(FILE *stream)
{
    fputs("usage: dottr-format [-w | --check] [FILE...]\n"
          "\n"
          "Formats dottr manifests. Without FILE, or with FILE -, reads\n"
          "standard input and writes the formatted manifest to standard\n"
          "output.\n"
          "\n"
          "options:\n"
          "  -w          rewrite each FILE instead of printing it\n"
          "  --check     print nothing; exit 1 if any FILE is not formatted\n"
          "  -h, --help  show this help\n",
          stream);
}

static int usageError(const char *message, const char *argument)
{
    fputs("dottr-format: error: ", stderr);
    fprintf(stderr, message, argument);
    fputs("\n", stderr);
    printUsage(stderr);
    return EXIT_USAGE;
}

int main(int argc, char **argv)
{
    hostInitialize();
    bool write = false;
    bool checkOnly = false;
    bool optionsEnded = false;
    int firstFile = argc;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (optionsEnded || arg[0] != '-' || strcmp(arg, "-") == 0) {
            if (firstFile == argc) {
                firstFile = i;
            }
            continue;
        }
        if (firstFile != argc) {
            return usageError("option '%s' must precede the files", arg);
        }
        if (strcmp(arg, "--") == 0) {
            optionsEnded = true;
        } else if (strcmp(arg, "-w") == 0) {
            write = true;
        } else if (strcmp(arg, "--check") == 0) {
            checkOnly = true;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            printUsage(stdout);
            return 0;
        } else {
            return usageError("unknown option '%s'", arg);
        }
    }

    if (write && checkOnly) {
        return usageError("%s",
                          "options '-w' and '--check' cannot be combined");
    }
    FormatMode mode = write ? FORMAT_WRITE
                      : checkOnly ? FORMAT_CHECK
                                  : FORMAT_PRINT;

    if (firstFile == argc) {
        if (write) {
            return usageError("%s", "option '-w' requires files");
        }
        return formatPath("dottr-format", "-", mode);
    }

    int status = 0;
    for (int i = firstFile; i < argc; i++) {
        if (write && strcmp(argv[i], "-") == 0) {
            return usageError("%s",
                              "option '-w' cannot be used with standard input");
        }
    }
    for (int i = firstFile; i < argc; i++) {
        int result = formatPath("dottr-format", argv[i], mode);
        status = result > status ? result : status;
    }
    return status;
}
