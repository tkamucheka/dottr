/*
 * dottr-lsp: the dottr language server (docs/reference/lsp.md), on standard
 * input and output. The server is that of `dottr lsp`.
 */

#include <stdio.h>
#include <string.h>

#include "dottr/host.h"
#include "dottr/lsp.h"

#define EXIT_USAGE 2

static void printUsage(FILE *stream)
{
    fputs("usage: dottr-lsp\n"
          "\n"
          "Runs the dottr language server on standard input and output.\n",
          stream);
}

int main(int argc, char **argv)
{
    hostInitialize();
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printUsage(stdout);
            return 0;
        }
        fprintf(stderr, "dottr-lsp: error: unexpected argument '%s'\n",
                argv[1]);
        printUsage(stderr);
        return EXIT_USAGE;
    }
    return lspRun(stdin, stdout);
}
