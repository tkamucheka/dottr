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
} CliCase;

static const CliCase cliCases[] = {
    {"no arguments", {NULL}, 2, "usage: dottr check"},
    {"help", {"--help", NULL}, 0, "usage: dottr check"},
    {"unknown command", {"frobnicate", NULL}, 2,
     "dottr: error: unknown command 'frobnicate'"},
    {"unknown option", {"plan", "-x", NULL}, 2,
     "dottr: error: unknown option '-x'"},
    {"missing file argument", {"plan", "-f", NULL}, 2,
     "dottr: error: option '-f' requires a file name"},
    {"check with units", {"check", "tmux", NULL}, 2,
     "dottr: error: check does not take unit names (found 'tmux')"},
    {"unreadable manifest", {"check", "-f", "test/cases/does-not-exist.dotfile", NULL},
     2, "dottr: error: cannot read test/cases/does-not-exist.dotfile: No such file"},
    {"repository manifest", {"check", "-f", "packages.dotfile", NULL}, 0, NULL},
    {"repository manifest plan", {"plan", "-f", "packages.dotfile", NULL}, 0,
     "install package tmux (child unit tmux-child-tmux)"},
    {"repository manifest is formatted",
     {"fmt", "--check", "-f", "packages.dotfile", NULL}, 0, NULL},
    {"fmt with units", {"fmt", "tmux", NULL}, 2,
     "dottr: error: fmt does not take unit names (found 'tmux')"},
    {"fmt -w and --check", {"fmt", "-w", "--check", NULL}, 2,
     "dottr: error: options '-w' and '--check' cannot be combined"},
    {"fmt -w with standard input", {"fmt", "-w", "-f", "-", NULL}, 2,
     "dottr: error: option '-w' cannot be used with standard input"},
    {"-w outside fmt", {"check", "-w", NULL}, 2,
     "dottr: error: unknown option '-w'"},
    {"empty standard input", {"fmt", "-f", "-", NULL}, 0, NULL},
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

/*
 * Runs build/dottr with the given arguments, HOME set to a fresh temporary
 * directory, and standard output and error captured together. Occurrences of
 * the temporary directory in the output are replaced by "$HOME".
 */
static bool runDottr(const char *const *args, RunResult *result)
{
    char home[4096];
    if (!makeTempDir(home, sizeof home)) {
        fprintf(stderr, "cannot create temporary directory: %s\n",
                strerror(errno));
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
        int devNull = open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            close(devNull);
        }
        setenv("HOME", home, 1);
        setenv("LC_ALL", "C", 1);

        const char *argv[MAX_ARGS + 2];
        size_t n = 0;
        argv[n++] = DOTTR_BINARY;
        for (size_t i = 0; args[i] && n < MAX_ARGS; i++) {
            argv[n++] = args[i];
        }
        argv[n] = NULL;
        execv(DOTTR_BINARY, (char *const *)argv);
        fprintf(stderr, "cannot execute %s: %s\n", DOTTR_BINARY,
                strerror(errno));
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
    return true;
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
 * Runs `dottr fmt -w` on a copy of an unformatted manifest in a temporary
 * directory and checks the rewritten file.
 */
static void runFormatWriteTest(void)
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

    const char *args[] = {"fmt", "-w", "-f", path, NULL};
    RunResult result;
    if (!runDottr(args, &result)) {
        TEST_FAIL("fmt -w: could not run %s", DOTTR_BINARY);
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
        if (!runDottr(c->argv, &result)) {
            TEST_FAIL("cli '%s': could not run %s", c->name, DOTTR_BINARY);
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
                char *unprivileged = replaceAll(expected, "sudo pacman",
                                                "pacman");
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

    if (access(DOTTR_BINARY, X_OK) != 0) {
        fprintf(stderr,
                "%s not found; build it first (see AGENTS.md) and run the "
                "tests from the repository root\n",
                DOTTR_BINARY);
        return 2;
    }

    runLexerTests();
    runFormatTests();
    runCliCases();
    runFormatWriteTest();
    runFixtures(update);

    if (testFailures > 0) {
        fprintf(stderr, "%d of %d tests failed\n", testFailures, testCount);
        return 1;
    }
    printf("all %d tests passed\n", testCount);
    return 0;
}
