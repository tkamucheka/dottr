#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/diag.h"
#include "dottr/eval.h"
#include "dottr/lexer.h"
#include "dottr/parser.h"
#include "dottr/sema.h"
#include "dottr/util.h"

#define EXIT_USAGE 2
#define DEFAULT_MANIFEST "packages.dotfile"

typedef enum {
    COMMAND_CHECK,
    COMMAND_PLAN,
    COMMAND_APPLY,
} Command;

static void printUsage(FILE *stream)
{
    fputs("usage: dottr check [-f FILE]\n"
          "       dottr plan  [-f FILE] [UNIT...]\n"
          "       dottr apply [-f FILE] [UNIT...]\n"
          "\n"
          "commands:\n"
          "  check   validate the manifest\n"
          "  plan    print the actions apply would perform, without "
          "performing them\n"
          "  apply   bring the selected units, and the units they need, to "
          "their\n"
          "          declared state; all units if none are named\n"
          "\n"
          "options:\n"
          "  -f FILE     manifest to read (default: " DEFAULT_MANIFEST ")\n"
          "  -h, --help  show this help\n",
          stream);
}

static int usageError(const char *format, const char *argument)
{
    fputs("dottr: error: ", stderr);
    fprintf(stderr, format, argument);
    fputs("\n", stderr);
    printUsage(stderr);
    return EXIT_USAGE;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printUsage(stderr);
        return EXIT_USAGE;
    }

    const char *commandName = argv[1];
    Command command;
    if (strcmp(commandName, "-h") == 0 || strcmp(commandName, "--help") == 0 ||
        strcmp(commandName, "help") == 0) {
        printUsage(stdout);
        return 0;
    } else if (strcmp(commandName, "check") == 0) {
        command = COMMAND_CHECK;
    } else if (strcmp(commandName, "plan") == 0) {
        command = COMMAND_PLAN;
    } else if (strcmp(commandName, "apply") == 0) {
        command = COMMAND_APPLY;
    } else {
        return usageError("unknown command '%s'", commandName);
    }

    const char *path = DEFAULT_MANIFEST;
    const char **unitNames = xmalloc((size_t)argc * sizeof *unitNames);
    size_t unitCount = 0;
    bool optionsEnded = false;

    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (!optionsEnded && strcmp(arg, "--") == 0) {
            optionsEnded = true;
        } else if (!optionsEnded && strcmp(arg, "-f") == 0) {
            if (i + 1 >= argc) {
                free(unitNames);
                return usageError("option '%s' requires a file name", arg);
            }
            path = argv[++i];
        } else if (!optionsEnded &&
                   (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)) {
            free(unitNames);
            printUsage(stdout);
            return 0;
        } else if (!optionsEnded && arg[0] == '-' && arg[1] != '\0') {
            free(unitNames);
            return usageError("unknown option '%s'", arg);
        } else {
            unitNames[unitCount++] = arg;
        }
    }

    if (command == COMMAND_CHECK && unitCount > 0) {
        int result = usageError("check does not take unit names (found '%s')",
                                unitNames[0]);
        free(unitNames);
        return result;
    }

    char *source;
    size_t length;
    if (!readFile(path, &source, &length)) {
        fprintf(stderr, "dottr: error: cannot read %s: %s\n", path,
                strerror(errno));
        free(unitNames);
        return EXIT_USAGE;
    }

    Arena arena;
    arenaInit(&arena);
    Diag diag;
    diagInit(&diag, path, stderr);

    int status = EXIT_USAGE;
    TokenList tokens;
    Manifest manifest;
    Unit **selected = NULL;

    if (!lexSource(&arena, source, length, &tokens, &diag) ||
        !parseManifest(&arena, &tokens, &manifest, &diag) ||
        !analyzeManifest(&arena, &manifest, &diag)) {
        goto done;
    }

    if (command == COMMAND_CHECK) {
        status = 0;
        goto done;
    }

    selected = xmalloc((unitCount + 1) * sizeof *selected);
    for (size_t i = 0; i < unitCount; i++) {
        selected[i] = findUnit(&manifest, unitNames[i]);
        if (!selected[i]) {
            fprintf(stderr, "dottr: error: %s declares no unit named '%s'\n",
                    path, unitNames[i]);
            goto done;
        }
    }

    status = evaluateManifest(&manifest,
                              command == COMMAND_PLAN ? EVAL_PLAN : EVAL_APPLY,
                              selected, unitCount, &diag);

done:
    free(selected);
    arenaFree(&arena);
    free(source);
    free(unitNames);
    return status;
}
