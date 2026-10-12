#define _POSIX_C_SOURCE 200809L

/*
 * Test program for dottr. Runs the lexer tests in test_lexer.c, then the
 * command-line tests: the fixtures in test/cases and the cases in
 * cliCases below. Run from the repository root after building build/dottr.
 *
 *     build/dottr-test           run all tests
 *     build/dottr-test --update  rewrite the expected output of fixtures
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dottr/util.h"
#include "test.h"

#define DOTTR_BINARY "build/dottr"
#define FORMAT_BINARY "build/dottr-format"
#define LSP_BINARY "build/dottr-lsp"
#define CASES_DIR "test/cases"
#define FIXTURE_SUFFIX ".dotfile"
#define MAX_ARGS 32

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
     {"apply", "--package-manager", "winget", "-f",
      "test/cases/apply-empty.dotfile", NULL},
     2, "dottr: error: package manager 'winget' is for windows", NULL, NULL},
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

static bool makeTempDir(char *buffer, size_t size)
{
    const char *base = getenv("TMPDIR");
    if (!base || !*base) {
        base = "/tmp";
    }
    snprintf(buffer, size, "%s/dottr-test-XXXXXX", base);
    return mkdtemp(buffer) != NULL;
}

static void removeTree(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        execlp("rm", "rm", "-rf", "--", path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

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
 * Runs a program with the given arguments, HOME set to a fresh temporary
 * directory, standard input read from input (or /dev/null if it is NULL),
 * and standard output and error captured together. Occurrences of the
 * temporary directory in the output are replaced by "$HOME".
 */
static bool runProgram(const char *program, const char *const *args,
                       const char *input, RunResult *result)
{
    char home[4096];
    if (!makeTempDir(home, sizeof home)) {
        fprintf(stderr, "cannot create temporary directory: %s\n",
                strerror(errno));
        return false;
    }
    /* The input file is outside HOME, so the program cannot see it there. */
    char inputPath[4200];
    snprintf(inputPath, sizeof inputPath, "%s.input", home);
    if (!writeFile(inputPath, input ? input : "", input ? strlen(input) : 0)) {
        removeTree(home);
        return false;
    }

    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        removeTree(home);
        return false;
    }

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        removeTree(home);
        return false;
    }
    if (pid == 0) {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[1]);
        int inputFd = open(input ? inputPath : "/dev/null", O_RDONLY);
        if (inputFd >= 0) {
            dup2(inputFd, STDIN_FILENO);
            close(inputFd);
        }
        setenv("HOME", home, 1);
        setenv("LC_ALL", "C", 1);

        const char *argv[MAX_ARGS + 2];
        size_t n = 0;
        argv[n++] = program;
        for (size_t i = 0; args[i] && n < MAX_ARGS; i++) {
            argv[n++] = args[i];
        }
        argv[n] = NULL;
        execv(program, (char *const *)argv);
        fprintf(stderr, "cannot execute %s: %s\n", program, strerror(errno));
        _exit(127);
    }

    close(pipeFds[1]);
    StrBuf captured;
    strBufInit(&captured);
    char chunk[4096];
    ssize_t count;
    while ((count = read(pipeFds[0], chunk, sizeof chunk)) != 0) {
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        strBufAppend(&captured, chunk, (size_t)count);
    }
    close(pipeFds[0]);

    int status;
    waitpid(pid, &status, 0);
    result->status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result->output = replaceAll(captured.data, home, "$HOME");
    result->length = strlen(result->output);
    strBufFree(&captured);
    removeTree(home);
    unlink(inputPath);
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
    if (!makeTempDir(dir, sizeof dir)) {
        TEST_FAIL("fmt -w: cannot create temporary directory");
        return;
    }
    char path[4200];
    snprintf(path, sizeof path, "%s/manifest.dotfile", dir);
    if (!writeFile(path, input, sizeof input - 1)) {
        TEST_FAIL("fmt -w: cannot write %s", path);
        removeTree(dir);
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
    removeTree(dir);
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

static int compareNames(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
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
    if (geteuid() == 0 && strstr(name, "brew")) {
        /* Homebrew does not run as root, so its plan is an error. */
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
            if (geteuid() == 0) {
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
    DIR *dir = opendir(CASES_DIR);
    if (!dir) {
        TEST_FAIL("cannot open %s: %s", CASES_DIR, strerror(errno));
        return;
    }

    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        size_t suffix = strlen(FIXTURE_SUFFIX);
        if (length > suffix &&
            strcmp(entry->d_name + length - suffix, FIXTURE_SUFFIX) == 0) {
            names = xrealloc(names, (count + 1) * sizeof *names);
            names[count] = xmalloc(length - suffix + 1);
            memcpy(names[count], entry->d_name, length - suffix);
            names[count][length - suffix] = '\0';
            count++;
        }
    }
    closedir(dir);

    qsort(names, count, sizeof *names, compareNames);
    for (size_t i = 0; i < count; i++) {
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
        if (access(programs[i], X_OK) != 0) {
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
