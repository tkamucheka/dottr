/*
 * Test program for dottr. Runs the lexer tests in test_lexer.c, then the
 * command-line tests: the fixtures in test/cases and the cases in
 * cliCases below. Run from the repository root after building build/dottr.
 *
 *     build/dottr-test           run all tests
 *     build/dottr-test --update  rewrite the expected output of fixtures
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/host.h"
#include "dottr/package.h"
#include "dottr/util.h"
#include "platform.h"
#include "test.h"

#define DOTTR_BINARY "build/dottr"
#define FORMAT_BINARY "build/dottr-format"
#define LSP_BINARY "build/dottr-lsp"
#define CASES_DIR "test/cases"
#define FIXTURE_SUFFIX ".dotfile"
#define MAX_ARGS 32

/* A package manager of another operating system than the host's. */
#ifdef _WIN32
#define FOREIGN_MANAGER "pacman"
#define FOREIGN_SYSTEM "linux"
#else
#define FOREIGN_MANAGER "winget"
#define FOREIGN_SYSTEM "windows"
#endif

int testFailures = 0;
int testCount = 0;

void testFail(const char *file, int line, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%s:%d: ", file, line);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    testFailures++;
}

typedef struct {
    const char *name;
    const char *argv[MAX_ARGS];
    int status;
    /* Text the combined output must contain; NULL for no requirement. */
    const char *contains;
    /* Program to run; NULL for build/dottr. */
    const char *program;
    /* Standard input; NULL for /dev/null. */
    const char *input;
} CliCase;

/* initialize, shutdown, and exit, framed for the language server. */
#define LSP_SESSION                                                            \
    "Content-Length: 75\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"         \
    "\"initialize\",\"params\":{\"capabilities\":{}}}"                            \
    "Content-Length: 44\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":"         \
    "\"shutdown\"}"                                                             \
    "Content-Length: 33\r\n\r\n{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"

static const CliCase cliCases[] = {
    {"no arguments", {NULL}, 2, "usage: dottr check", NULL, NULL},
    {"help", {"--help", NULL}, 0, "usage: dottr check", NULL, NULL},
    {"unknown command", {"frobnicate", NULL}, 2,
     "dottr: error: unknown command 'frobnicate'", NULL, NULL},
    {"unknown option", {"plan", "-x", NULL}, 2,
     "dottr: error: unknown option '-x'", NULL, NULL},
    {"missing file argument", {"plan", "-f", NULL}, 2,
     "dottr: error: option '-f' requires a file name", NULL, NULL},
    {"check with units", {"check", "tmux", NULL}, 2,
     "dottr: error: check does not take unit names (found 'tmux')", NULL,
     NULL},
    {"unreadable manifest",
     {"check", "-f", "test/cases/does-not-exist.dotfile", NULL}, 2,
     "dottr: error: cannot read test/cases/does-not-exist.dotfile: No such "
     "file",
     NULL, NULL},
    {"repository manifest", {"check", "-f", "manifest.dotfile", NULL}, 0, NULL,
     NULL, NULL},
    {"repository manifest plan",
     {"plan", "--package-manager", "pacman", "-f", "manifest.dotfile", NULL},
     0, "install package tmux (child unit tmux-child-tmux)", NULL, NULL},
    {"unknown package manager", {"plan", "--package-manager", "yum", NULL}, 2,
     "dottr: error: unknown package manager 'yum'", NULL, NULL},
    {"missing package manager", {"plan", "--package-manager", NULL}, 2,
     "dottr: error: option '--package-manager' requires a package manager",
     NULL, NULL},
    {"package manager outside plan and apply",
     {"check", "--package-manager", "apt", NULL}, 2,
     "dottr: error: unknown option '--package-manager'", NULL, NULL},
    {"unit of another system",
     {"plan", "--package-manager", "apt", "-f",
      "test/cases/on-plan-apt.dotfile", "rectangle", NULL},
     2, "dottr: error: unit 'rectangle' of test/cases/on-plan-apt.dotfile "
        "does not exist on apt",
     NULL, NULL},
    {"apply with the package manager of another system",
     {"apply", "--package-manager", FOREIGN_MANAGER, "-f",
      "test/cases/apply-empty.dotfile", NULL},
     2, "dottr: error: package manager '" FOREIGN_MANAGER "' is for "
        FOREIGN_SYSTEM,
     NULL, NULL},
    {"repository manifest is formatted",
     {"fmt", "--check", "-f", "manifest.dotfile", NULL}, 0, NULL, NULL, NULL},
    {"fmt with units", {"fmt", "tmux", NULL}, 2,
     "dottr: error: fmt does not take unit names (found 'tmux')", NULL, NULL},
    {"fmt -w and --check", {"fmt", "-w", "--check", NULL}, 2,
     "dottr: error: options '-w' and '--check' cannot be combined", NULL, NULL},
    {"fmt -w with standard input", {"fmt", "-w", "-f", "-", NULL}, 2,
     "dottr: error: option '-w' cannot be used with standard input", NULL, NULL},
    {"-w outside fmt", {"check", "-w", NULL}, 2,
     "dottr: error: unknown option '-w'", NULL, NULL},
    {"empty standard input", {"fmt", "-f", "-", NULL}, 0, NULL, NULL, NULL},
    {"fmt from standard input", {"fmt", "-f", "-", NULL}, 0, "pkg a\n", NULL,
     "pkg   a\n"},
    {"lsp session", {"lsp", NULL}, 0, "\"serverInfo\":{\"name\":\"dottr\"}",
     NULL, LSP_SESSION},

    {"dottr-format help", {"--help", NULL}, 0, "usage: dottr-format",
     FORMAT_BINARY, NULL},
    {"dottr-format standard input", {NULL}, 0, "pkg a\n", FORMAT_BINARY,
     "pkg   a\n"},
    {"dottr-format -w without files", {"-w", NULL}, 2,
     "dottr-format: error: option '-w' requires files", FORMAT_BINARY, NULL},
    {"dottr-format option after files", {"manifest.dotfile", "-w", NULL}, 2,
     "dottr-format: error: option '-w' must precede the files", FORMAT_BINARY,
     NULL},
    {"dottr-format -w and --check", {"-w", "--check", "manifest.dotfile", NULL},
     2, "options '-w' and '--check' cannot be combined", FORMAT_BINARY, NULL},
    {"dottr-format unknown option", {"-x", NULL}, 2,
     "dottr-format: error: unknown option '-x'", FORMAT_BINARY, NULL},
    {"dottr-format formatted file", {"--check", "manifest.dotfile", NULL}, 0,
     NULL, FORMAT_BINARY, NULL},
    {"dottr-format several files",
     {"--check", "manifest.dotfile", "test/cases/fmt-check-unformatted.dotfile",
      NULL},
     1, "dottr-format: test/cases/fmt-check-unformatted.dotfile is not "
        "formatted",
     FORMAT_BINARY, NULL},
    {"dottr-format worst status",
     {"--check", "test/cases/fmt-check-unformatted.dotfile",
      "test/cases/does-not-exist.dotfile", NULL},
     2, "dottr-format: error: cannot read test/cases/does-not-exist.dotfile",
     FORMAT_BINARY, NULL},
    {"dottr-format syntax error", {"test/cases/fmt-syntax-error.dotfile", NULL},
     2, "fmt-syntax-error.dotfile:4:11: error:", FORMAT_BINARY, NULL},

    {"dottr-lsp help", {"--help", NULL}, 0, "usage: dottr-lsp", LSP_BINARY,
     NULL},
    {"dottr-lsp argument", {"x", NULL}, 2,
     "dottr-lsp: error: unexpected argument 'x'", LSP_BINARY, NULL},
    {"dottr-lsp session", {NULL}, 0, "\"serverInfo\":{\"name\":\"dottr\"}",
     LSP_BINARY, LSP_SESSION},
    {"dottr-lsp end of input without exit", {NULL}, 1, NULL, LSP_BINARY, NULL},
};

typedef struct {
    char *output;
    size_t length;
    int status;
} RunResult;

/* Replaces every occurrence of from in text with to. */
static char *replaceAll(const char *text, const char *from, const char *to)
{
    StrBuf buf;
    strBufInit(&buf);
    size_t fromLength = strlen(from);
    const char *cursor = text;
    const char *match;
    while (fromLength > 0 && (match = strstr(cursor, from)) != NULL) {
        strBufAppend(&buf, cursor, (size_t)(match - cursor));
        strBufAppendStr(&buf, to);
        cursor = match + fromLength;
    }
    strBufAppendStr(&buf, cursor);
    return buf.data;
}

static bool writeFile(const char *path, const char *data, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    bool ok = fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

/*
 * Runs a program with the given arguments, its home directory (HOME and
 * USERPROFILE) set to a fresh temporary directory, standard input read from
 * input (or empty if it is NULL), and standard output and error captured
 * together. Occurrences of the temporary directory in the output are replaced
 * by "$HOME".
 */
static bool runProgram(const char *program, const char *const *args,
                       const char *input, RunResult *result)
{
    char home[4096];
    if (!testMakeTempDir(home, sizeof home)) {
        fprintf(stderr, "cannot create a temporary directory\n");
        return false;
    }
    /* The files are outside HOME, so the program cannot see them there. */
    char inputPath[4200];
    char outputPath[4200];
    snprintf(inputPath, sizeof inputPath, "%s.input", home);
    snprintf(outputPath, sizeof outputPath, "%s.output", home);
    if (!writeFile(inputPath, input ? input : "", input ? strlen(input) : 0)) {
        testRemoveTree(home);
        return false;
    }

    const char *argv[MAX_ARGS + 2];
    size_t n = 0;
    argv[n++] = program;
    for (size_t i = 0; args[i] && n < MAX_ARGS; i++) {
        argv[n++] = args[i];
    }
    argv[n] = NULL;
    const HostVariable variables[] = {
        {"HOME", home},
        {"USERPROFILE", home},
        {"LC_ALL", "C"},
    };

    SavedStreams saved;
    if (!testRedirect(inputPath, outputPath, &saved)) {
        remove(inputPath);
        testRemoveTree(home);
        return false;
    }
    int status = hostRun((char *const *)argv, variables,
                         sizeof variables / sizeof variables[0]);
    testRestore(&saved);

    char *captured;
    size_t length;
    if (!readFile(outputPath, &captured, &length)) {
        captured = xmalloc(1);
        captured[0] = '\0';
    }
    result->status = status;
    result->output = replaceAll(captured, home, "$HOME");
    result->length = strlen(result->output);
    free(captured);
    remove(inputPath);
    remove(outputPath);
    testRemoveTree(home);
    return true;
}

static bool runDottr(const char *const *args, RunResult *result)
{
    return runProgram(DOTTR_BINARY, args, NULL, result);
}

/*
 * Runs `dottr fmt -w` or `dottr-format -w` on a copy of an unformatted
 * manifest in a temporary directory and checks the rewritten file.
 */
static void runFormatWriteTest(bool standalone)
{
    static const char input[] = "pkg   a do\nneeds b\n\n\nend\npkg b";
    static const char expected[] = "pkg a do\n  needs b\nend\npkg b\n";
    testCount++;

    char dir[4096];
    if (!testMakeTempDir(dir, sizeof dir)) {
        TEST_FAIL("fmt -w: cannot create temporary directory");
        return;
    }
    char path[4200];
    snprintf(path, sizeof path, "%s/manifest.dotfile", dir);
    if (!writeFile(path, input, sizeof input - 1)) {
        TEST_FAIL("fmt -w: cannot write %s", path);
        testRemoveTree(dir);
        return;
    }

    const char *subcommand[] = {"fmt", "-w", "-f", path, NULL};
    const char *program[] = {"-w", path, NULL};
    RunResult result;
    if (!runProgram(standalone ? FORMAT_BINARY : DOTTR_BINARY,
                    standalone ? program : subcommand, NULL, &result)) {
        TEST_FAIL("fmt -w: could not run the formatter");
    } else {
        char *data;
        size_t length;
        if (result.status != 0 || result.length != 0) {
            TEST_FAIL("fmt -w: status %d, output:\n%s", result.status,
                      result.output);
        } else if (!readFile(path, &data, &length)) {
            TEST_FAIL("fmt -w: cannot read %s", path);
        } else {
            if (strcmp(data, expected) != 0) {
                TEST_FAIL("fmt -w: file contains\n%s", data);
            }
            free(data);
        }
        free(result.output);
    }
    testRemoveTree(dir);
}

static void runCliCases(void)
{
    for (size_t i = 0; i < sizeof cliCases / sizeof cliCases[0]; i++) {
        const CliCase *c = &cliCases[i];
        RunResult result;
        testCount++;
        const char *program = c->program ? c->program : DOTTR_BINARY;
        if (!runProgram(program, c->argv, c->input, &result)) {
            TEST_FAIL("cli '%s': could not run %s", c->name, program);
            continue;
        }
        if (result.status != c->status) {
            TEST_FAIL("cli '%s': expected status %d, got %d; output:\n%s",
                      c->name, c->status, result.status, result.output);
        } else if (c->contains && !strstr(result.output, c->contains)) {
            TEST_FAIL("cli '%s': output does not contain \"%s\"; output:\n%s",
                      c->name, c->contains, result.output);
        } else if (!c->contains && result.length > 0) {
            TEST_FAIL("cli '%s': expected no output; output:\n%s", c->name,
                      result.output);
        }
        free(result.output);
    }
}

/*
 * Reads the "# args:" and "# status:" header of a fixture. Arguments are
 * separated by spaces and stored in storage.
 */
static bool readHeader(const char *source, char *storage, size_t storageSize,
                       const char **args, size_t maxArgs, int *status)
{
    const char *argsLine = strstr(source, "# args:");
    const char *statusLine = strstr(source, "# status:");
    if (!argsLine || !statusLine) {
        return false;
    }

    argsLine += strlen("# args:");
    size_t length = strcspn(argsLine, "\n");
    if (length >= storageSize) {
        return false;
    }
    memcpy(storage, argsLine, length);
    storage[length] = '\0';

    size_t count = 0;
    for (char *token = strtok(storage, " \t"); token && count + 1 < maxArgs;
         token = strtok(NULL, " \t")) {
        args[count++] = token;
    }
    args[count] = NULL;

    *status = atoi(statusLine + strlen("# status:"));
    return count > 0;
}

/*
 * Reads the optional "# requires:" header of a fixture, the command the
 * fixture needs, into command. Returns false if there is none.
 */
static bool readRequirement(const char *source, char *command, size_t size)
{
    const char *line = strstr(source, "# requires:");
    if (!line) {
        return false;
    }
    line += strlen("# requires:");
    line += strspn(line, " \t");
    size_t length = strcspn(line, " \t\r\n");
    if (length >= size) {
        length = size - 1;
    }
    memcpy(command, line, length);
    command[length] = '\0';
    return length > 0;
}

/*
 * Whether a fixture runs on this system: true unless it has an "# os:"
 * header that does not name the operating system of the host.
 */
static bool runsOnHost(const char *source)
{
    const char *line = strstr(source, "# os:");
    if (!line) {
        return true;
    }
    line += strlen("# os:");
    size_t length = strcspn(line, "\n");
    const char *host = operatingSystemName(hostOperatingSystem());
    size_t hostLength = strlen(host);
    for (const char *p = line; p + hostLength <= line + length; p++) {
        if (strncmp(p, host, hostLength) == 0) {
            return true;
        }
    }
    return false;
}

static void runFixture(const char *name, bool update)
{
    char sourcePath[1024];
    char outPath[1024];
    snprintf(sourcePath, sizeof sourcePath, "%s/%s" FIXTURE_SUFFIX, CASES_DIR,
             name);
    snprintf(outPath, sizeof outPath, "%s/%s.out", CASES_DIR, name);
    testCount++;

    char *source;
    size_t sourceLength;
    if (!readFile(sourcePath, &source, &sourceLength)) {
        TEST_FAIL("fixture %s: cannot read %s", name, sourcePath);
        return;
    }

    char storage[1024];
    const char *args[MAX_ARGS];
    int expectedStatus;
    if (!readHeader(source, storage, sizeof storage, args, MAX_ARGS - 2,
                    &expectedStatus)) {
        TEST_FAIL("fixture %s: missing '# args:' or '# status:' header", name);
        free(source);
        return;
    }
    if (hostIsRoot() && strstr(name, "brew")) {
        /* Homebrew does not run as root, so its plan is an error. */
        free(source);
        return;
    }
    if (!runsOnHost(source)) {
        printf("skipped fixture %s: not for %s\n", name,
               operatingSystemName(hostOperatingSystem()));
        free(source);
        return;
    }
    char command[64];
    if (readRequirement(source, command, sizeof command) &&
        !hostCommandExists(command)) {
        printf("skipped fixture %s: %s is not installed\n", name, command);
        free(source);
        return;
    }
    size_t argCount = 0;
    while (args[argCount]) {
        argCount++;
    }
    args[argCount++] = "-f";
    args[argCount++] = sourcePath;
    args[argCount] = NULL;

    RunResult result;
    if (!runDottr(args, &result)) {
        TEST_FAIL("fixture %s: could not run %s", name, DOTTR_BINARY);
        free(source);
        return;
    }

    if (result.status != expectedStatus) {
        TEST_FAIL("fixture %s: expected status %d, got %d; output:\n%s", name,
                  expectedStatus, result.status, result.output);
    }

    if (update) {
        if (!writeFile(outPath, result.output, result.length)) {
            TEST_FAIL("fixture %s: cannot write %s", name, outPath);
        }
    } else {
        char *expected;
        size_t expectedLength;
        if (!readFile(outPath, &expected, &expectedLength)) {
            TEST_FAIL("fixture %s: cannot read %s", name, outPath);
        } else {
            if (hostIsRoot()) {
                /* Expected outputs are recorded without root privileges. */
                char *unprivileged = replaceAll(expected, ": sudo ", ": ");
                free(expected);
                expected = unprivileged;
                expectedLength = strlen(expected);
            }
            if (expectedLength != result.length ||
                memcmp(expected, result.output, result.length) != 0) {
                TEST_FAIL("fixture %s: output differs\n"
                          "--- expected\n%s--- actual\n%s---",
                          name, expected, result.output);
            }
            free(expected);
        }
    }

    free(result.output);
    free(source);
}

static void runFixtures(bool update)
{
    size_t count;
    char **names = testListFiles(CASES_DIR, FIXTURE_SUFFIX, &count);
    if (!names) {
        TEST_FAIL("cannot read %s", CASES_DIR);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        /* The name of a fixture is its file name without the suffix. */
        names[i][strlen(names[i]) - strlen(FIXTURE_SUFFIX)] = '\0';
        runFixture(names[i], update);
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv)
{
    bool update = argc > 1 && strcmp(argv[1], "--update") == 0;

    static const char *const programs[] = {DOTTR_BINARY, FORMAT_BINARY,
                                           LSP_BINARY};
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        if (!hostCommandExists(programs[i])) {
            fprintf(stderr,
                    "%s not found; build the programs first (see AGENTS.md) "
                    "and run the tests from the repository root\n",
                    programs[i]);
            return 2;
        }
    }

    runLexerTests();
    runFormatTests();
    runLspTests();
    runPackageTests();
    runCliCases();
    runFormatWriteTest(false);
    runFormatWriteTest(true);
    runFixtures(update);

    if (testFailures > 0) {
        fprintf(stderr, "%d of %d tests failed\n", testFailures, testCount);
        return 1;
    }
    printf("all %d tests passed\n", testCount);
    return 0;
}
