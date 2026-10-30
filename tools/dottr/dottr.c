#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/diag.h"
#include "dottr/eval.h"
#include "dottr/format.h"
#include "dottr/host.h"
#include "dottr/lexer.h"
#include "dottr/lsp.h"
#include "dottr/package.h"
#include "dottr/parser.h"
#include "dottr/sema.h"
#include "dottr/util.h"

#define EXIT_USAGE 2
#define DEFAULT_MANIFEST "packages.dotfile"

typedef enum {
    COMMAND_CHECK,
    COMMAND_PLAN,
    COMMAND_APPLY,
    COMMAND_FMT,
} Command;

static void printUsage(FILE *stream)
{
    fputs("usage: dottr check [-f FILE]\n"
          "       dottr plan  [-f FILE] [--package-manager NAME] [UNIT...]\n"
          "       dottr apply [-f FILE] [--package-manager NAME] [UNIT...]\n"
          "       dottr fmt   [-f FILE] [-w | --check]\n"
          "       dottr lsp\n"
          "\n"
          "commands:\n"
          "  check   validate the manifest\n"
          "  plan    print the actions apply would perform, without "
          "performing them\n"
          "  apply   bring the selected units, and the units they need, to "
          "their\n"
          "          declared state; all units if none are named\n"
          "  fmt     print the manifest in canonical layout\n"
          "  lsp     run the language server on standard input and output\n"
          "\n"
          "options:\n"
          "  -f FILE     manifest to read, or - for standard input\n"
          "              (default: " DEFAULT_MANIFEST ")\n"
          "  --package-manager NAME\n"
          "              plan, apply: install packages with NAME, one of "
          "pacman, apt,\n"
          "              dnf, brew, or winget (default: detected)\n"
          "  -w          fmt: rewrite the file instead of printing it\n"
          "  --check     fmt: print nothing; exit 1 if the file is not "
          "formatted\n"
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
    hostInitialize();
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
    } else if (strcmp(commandName, "fmt") == 0) {
        command = COMMAND_FMT;
    } else if (strcmp(commandName, "lsp") == 0) {
        if (argc > 2) {
            return usageError("lsp takes no arguments (found '%s')", argv[2]);
        }
        return lspRun(stdin, stdout);
    } else {
        return usageError("unknown command '%s'", commandName);
    }

    const char *path = DEFAULT_MANIFEST;
    const char *managerName = NULL;
    bool write = false;
    bool checkOnly = false;
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
                   (command == COMMAND_PLAN || command == COMMAND_APPLY) &&
                   strcmp(arg, "--package-manager") == 0) {
            if (i + 1 >= argc) {
                free(unitNames);
                return usageError("option '%s' requires a package manager",
                                  arg);
            }
            managerName = argv[++i];
        } else if (!optionsEnded && command == COMMAND_FMT &&
                   strcmp(arg, "-w") == 0) {
            write = true;
        } else if (!optionsEnded && command == COMMAND_FMT &&
                   strcmp(arg, "--check") == 0) {
            checkOnly = true;
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

    if ((command == COMMAND_CHECK || command == COMMAND_FMT) &&
        unitCount > 0) {
        int result = usageError(command == COMMAND_CHECK
                                    ? "check does not take unit names "
                                      "(found '%s')"
                                    : "fmt does not take unit names "
                                      "(found '%s')",
                                unitNames[0]);
        free(unitNames);
        return result;
    }
    bool fromStdin = strcmp(path, "-") == 0;
    if (write && checkOnly) {
        free(unitNames);
        return usageError("%s",
                          "options '-w' and '--check' cannot be combined");
    }
    if (write && fromStdin) {
        free(unitNames);
        return usageError("%s",
                          "option '-w' cannot be used with standard input");
    }

    PackageManager manager = PACKAGE_MANAGER_NONE;
    if (managerName) {
        manager = packageManagerFromName(managerName);
        if (manager == PACKAGE_MANAGER_NONE) {
            free(unitNames);
            return usageError("unknown package manager '%s'; expected "
                              "pacman, apt, dnf, brew, or winget",
                              managerName);
        }
        OperatingSystem system = packageManagerSystem(manager);
        if (command == COMMAND_APPLY && system != hostOperatingSystem()) {
            fprintf(stderr,
                    "dottr: error: package manager '%s' is for %s; apply "
                    "runs on %s\n",
                    managerName, operatingSystemName(system),
                    operatingSystemName(hostOperatingSystem()));
            free(unitNames);
            return EXIT_USAGE;
        }
    }

    if (command == COMMAND_FMT) {
        free(unitNames);
        return formatPath("dottr", path,
                          write       ? FORMAT_WRITE
                          : checkOnly ? FORMAT_CHECK
                                      : FORMAT_PRINT);
    }

    char *source;
    size_t length;
    if (!(fromStdin ? readStream(stdin, &source, &length)
                    : readFile(path, &source, &length))) {
        fprintf(stderr, "dottr: error: cannot read %s: %s\n",
                fromStdin ? "standard input" : path, strerror(errno));
        free(unitNames);
        return EXIT_USAGE;
    }
    if (fromStdin) {
        path = "<stdin>";
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

    if (!managerName) {
        manager = detectPackageManager();
    }
    char systemName[96];
    SystemSet systems = packageManagerSystems(manager);
    if (systems && !(systems & manifest.systems)) {
        char declared[96];
        describeSystems(manifest.systems, declared, sizeof declared);
        describeSystems(systems, systemName, sizeof systemName);
        fprintf(stderr, "dottr: error: %s is for %s; this host uses %s (%s)\n",
                path, declared, packageManagerName(manager), systemName);
        goto done;
    }
    if (systems == 0) {
        /*
         * Without a package manager, the operating system selects the view,
         * which requires that its package managers see the same manifest.
         */
        switch (hostOperatingSystem()) {
        case OS_LINUX: systems = SYSTEM_LINUX; break;
        case OS_MACOS: systems = SYSTEM_MACOS; break;
        case OS_WINDOWS: systems = SYSTEM_WINDOWS; break;
        case OS_OTHER: systems = SYSTEM_ALL; break;
        }
        systems &= manifest.systems;
        if (!systems) {
            char declared[96];
            describeSystems(manifest.systems, declared, sizeof declared);
            fprintf(stderr, "dottr: error: %s is for %s; this host is %s\n",
                    path, declared, operatingSystemName(hostOperatingSystem()));
            goto done;
        }
        if (!viewsAgree(&manifest, systems)) {
            fprintf(stderr,
                    "dottr: error: no supported package manager was found, "
                    "and %s selects statements by system family; name one "
                    "with --package-manager\n",
                    path);
            goto done;
        }
    }
    Manifest view;
    if (!selectManifest(&arena, &manifest, systems, &view)) {
        fprintf(stderr, "dottr: error: internal error: invalid view\n");
        goto done;
    }
    describeSystems(systems, systemName, sizeof systemName);

    selected = xmalloc((unitCount + 1) * sizeof *selected);
    for (size_t i = 0; i < unitCount; i++) {
        selected[i] = findUnit(&view, unitNames[i]);
        if (!selected[i] && findUnit(&manifest, unitNames[i])) {
            fprintf(stderr,
                    "dottr: error: unit '%s' of %s does not exist on %s\n",
                    unitNames[i], path, systemName);
            goto done;
        }
        if (!selected[i]) {
            fprintf(stderr, "dottr: error: %s declares no unit named '%s'\n",
                    path, unitNames[i]);
            goto done;
        }
    }

    status = evaluateManifest(&view,
                              command == COMMAND_PLAN ? EVAL_PLAN : EVAL_APPLY,
                              manager, selected, unitCount, &diag);

done:
    free(selected);
    arenaFree(&arena);
    free(source);
    free(unitNames);
    return status;
}
