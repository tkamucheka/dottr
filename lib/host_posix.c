/* Host layer for POSIX systems: Linux and macOS. */
#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
/* macOS declares mkdtemp only with its own extensions. */
#define _DARWIN_C_SOURCE

#include "dottr/host.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dottr/util.h"

void hostInitialize(void)
{
}

const char *hostHomeDirectory(void)
{
    const char *home = getenv("HOME");
    if (home && *home) {
        return home;
    }
    struct passwd *entry = getpwuid(getuid());
    if (entry && entry->pw_dir && *entry->pw_dir) {
        return entry->pw_dir;
    }
    return NULL;
}

static bool isExecutableFile(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) &&
           access(path, X_OK) == 0;
}

bool hostCommandExists(const char *command)
{
    if (*command == '\0') {
        return false;
    }
    if (strchr(command, '/')) {
        return isExecutableFile(command);
    }

    const char *path = getenv("PATH");
    if (!path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }

    StrBuf candidate;
    strBufInit(&candidate);
    bool found = false;
    const char *start = path;
    for (;;) {
        const char *end = strchr(start, ':');
        size_t length = end ? (size_t)(end - start) : strlen(start);

        strBufClear(&candidate);
        if (length == 0) {
            strBufAppendStr(&candidate, ".");
        } else {
            strBufAppend(&candidate, start, length);
        }
        strBufAppendChar(&candidate, '/');
        strBufAppendStr(&candidate, command);
        if (isExecutableFile(candidate.data)) {
            found = true;
            break;
        }

        if (!end) {
            break;
        }
        start = end + 1;
    }
    strBufFree(&candidate);
    return found;
}

bool hostIsFile(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bool hostIsDirectory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool makeOne(const char *path, char *message, size_t messageSize)
{
    if (mkdir(path, 0777) == 0) {
        return true;
    }
    int error = errno;
    if (error == EEXIST) {
        if (hostIsDirectory(path)) {
            return true;
        }
        snprintf(message, messageSize, "%s exists and is not a directory",
                 path);
        return false;
    }
    snprintf(message, messageSize, "cannot create %s: %s", path,
             strerror(error));
    return false;
}

bool hostCreateDirectory(const char *path, char *message, size_t messageSize)
{
    if (*path == '\0') {
        snprintf(message, messageSize, "cannot create a directory with an "
                                       "empty path");
        return false;
    }

    size_t length = strlen(path);
    char *copy = xmalloc(length + 1);
    memcpy(copy, path, length + 1);

    bool ok = true;
    for (size_t i = 1; i < length && ok; i++) {
        if (copy[i] == '/' && copy[i - 1] != '/') {
            copy[i] = '\0';
            ok = makeOne(copy, message, messageSize);
            copy[i] = '/';
        }
    }
    if (ok) {
        ok = makeOne(copy, message, messageSize);
    }

    free(copy);
    return ok;
}

bool hostPathKind(const char *path, HostPathKind *kind, char *message,
                  size_t messageSize)
{
    struct stat info;
    if (lstat(path, &info) == 0) {
        *kind = S_ISLNK(info.st_mode) ? HOST_PATH_SYMLINK
                : S_ISDIR(info.st_mode) ? HOST_PATH_DIRECTORY
                : S_ISREG(info.st_mode) ? HOST_PATH_FILE
                                       : HOST_PATH_OTHER;
        return true;
    }
    if (errno == ENOENT) {
        *kind = HOST_PATH_MISSING;
        return true;
    }
    snprintf(message, messageSize, "cannot inspect %s: %s", path,
             strerror(errno));
    return false;
}

bool hostPathSeparator(char c)
{
    return c == '/';
}

size_t hostPathRootLength(const char *path)
{
    return path[0] == '/' ? 1 : 0;
}

bool hostPathsEqual(const char *left, const char *right)
{
    return strcmp(left, right) == 0;
}

char *hostAbsolutePath(const char *path, char *message, size_t messageSize)
{
    if (!*path) {
        snprintf(message, messageSize, "a symlink path must not be empty");
        return NULL;
    }
    StrBuf absolute;
    strBufInit(&absolute);
    if (*path != '/') {
        size_t size = 128;
        char *cwd = xmalloc(size);
        while (!getcwd(cwd, size)) {
            if (errno != ERANGE) {
                snprintf(message, messageSize, "cannot read working directory: %s",
                         strerror(errno));
                free(cwd);
                return NULL;
            }
            size *= 2;
            cwd = xrealloc(cwd, size);
        }
        strBufAppendStr(&absolute, cwd);
        strBufAppendChar(&absolute, '/');
        free(cwd);
    }
    strBufAppendStr(&absolute, path);
    char *normalized = xmalloc(absolute.length + 1);
    size_t length = 1;
    normalized[0] = '/';
    for (const char *p = absolute.data; *p;) {
        if (*p == '/') {
            p++;
            continue;
        }
        const char *end = strchr(p, '/');
        size_t count = end ? (size_t)(end - p) : strlen(p);
        if (count == 2 && p[0] == '.' && p[1] == '.') {
            while (length > 1 && normalized[length - 1] != '/') {
                length--;
            }
            if (length > 1) {
                length--;
            }
        } else if (!(count == 1 && *p == '.')) {
            if (length > 1) {
                normalized[length++] = '/';
            }
            memcpy(normalized + length, p, count);
            length += count;
        }
        p += count;
    }
    normalized[length] = '\0';
    strBufFree(&absolute);
    return normalized;
}

char *hostCanonicalPath(const char *path, char *message, size_t messageSize)
{
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        snprintf(message, messageSize, "cannot resolve %s: %s", path,
                 strerror(errno));
    }
    return resolved;
}

static int compareNames(const void *left, const void *right)
{
    return strcmp(*(char *const *)left, *(char *const *)right);
}

bool hostDirectoryEntries(const char *path, char ***names, size_t *count,
                          char *message, size_t messageSize)
{
    *names = NULL;
    *count = 0;
    DIR *directory = opendir(path);
    if (!directory) {
        snprintf(message, messageSize, "cannot read %s: %s", path,
                 strerror(errno));
        return false;
    }
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        *names = xrealloc(*names, (*count + 1) * sizeof **names);
        size_t length = strlen(entry->d_name);
        (*names)[*count] = xmalloc(length + 1);
        memcpy((*names)[(*count)++], entry->d_name, length + 1);
    }
    int error = errno;
    if (closedir(directory) != 0 && error == 0) {
        error = errno;
    }
    if (error != 0) {
        snprintf(message, messageSize, "cannot read %s: %s", path,
                 strerror(error));
        for (size_t i = 0; i < *count; i++) {
            free((*names)[i]);
        }
        free(*names);
        *names = NULL;
        *count = 0;
        return false;
    }
    if (*count > 1) {
        qsort(*names, *count, sizeof **names, compareNames);
    }
    return true;
}

bool hostSymlinkMatches(const char *source, const char *target, bool *matches,
                        char *message, size_t messageSize)
{
    *matches = false;
    size_t size = 256;
    char *contents = xmalloc(size + 1);
    ssize_t length;
    while ((length = readlink(target, contents, size)) == (ssize_t)size) {
        size *= 2;
        contents = xrealloc(contents, size + 1);
    }
    if (length < 0) {
        snprintf(message, messageSize, "cannot read symlink %s: %s", target,
                 strerror(errno));
        free(contents);
        return false;
    }
    contents[length] = '\0';
    StrBuf path;
    strBufInit(&path);
    if (contents[0] != '/') {
        const char *slash = strrchr(target, '/');
        strBufAppend(&path, target, (size_t)(slash - target) + 1);
    }
    strBufAppendStr(&path, contents);
    char *absolute = hostAbsolutePath(path.data, message, messageSize);
    if (absolute) {
        *matches = hostPathsEqual(source, absolute);
    }
    bool ok = absolute != NULL;
    free(contents);
    free(absolute);
    strBufFree(&path);
    return ok;
}

bool hostCreateSymlink(const char *source, const char *target, char *message,
                       size_t messageSize)
{
    if (symlink(source, target) == 0) {
        return true;
    }
    snprintf(message, messageSize, "cannot create symlink %s: %s", target,
             strerror(errno));
    return false;
}

bool hostIsRoot(void)
{
    return geteuid() == 0;
}

/*
 * Runs argv as hostRun does; if silent, the child's standard output and
 * standard error are discarded.
 */
static int run(char *const argv[], const HostVariable *variables,
               size_t variableCount, bool silent)
{
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "dottr: error: cannot start %s: %s\n", argv[0],
                strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (silent) {
            int null = open("/dev/null", O_WRONLY);
            if (null >= 0) {
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
                close(null);
            }
        }
        for (size_t i = 0; i < variableCount; i++) {
            if (setenv(variables[i].name, variables[i].value, 1) != 0) {
                fprintf(stderr, "dottr: error: cannot set %s: %s\n",
                        variables[i].name, strerror(errno));
                _exit(127);
            }
        }
        execvp(argv[0], argv);
        fprintf(stderr, "dottr: error: cannot execute %s: %s\n", argv[0],
                strerror(errno));
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "dottr: error: cannot wait for %s: %s\n", argv[0],
                    strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

int hostRun(char *const argv[], const HostVariable *variables,
            size_t variableCount)
{
    return run(argv, variables, variableCount, false);
}

int hostRunSilently(char *const argv[])
{
    return run(argv, NULL, 0, true);
}

bool hostWriteTemporary(const char *name, const char *contents, size_t length,
                        char *path, size_t pathSize, char *message,
                        size_t messageSize)
{
    const char *base = getenv("TMPDIR");
    if (!base || !*base) {
        base = "/tmp";
    }
    char directory[4096];
    int written = snprintf(directory, sizeof directory, "%s/dottr-XXXXXX",
                           base);
    if (written < 0 || (size_t)written >= sizeof directory) {
        snprintf(message, messageSize, "the temporary directory %s is too "
                                       "long", base);
        return false;
    }
    if (!mkdtemp(directory)) {
        snprintf(message, messageSize, "cannot create a directory in %s: %s",
                 base, strerror(errno));
        return false;
    }
    written = snprintf(path, pathSize, "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= pathSize) {
        rmdir(directory);
        snprintf(message, messageSize, "the temporary directory %s is too "
                                       "long", base);
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        snprintf(message, messageSize, "cannot create %s: %s", path,
                 strerror(errno));
        rmdir(directory);
        return false;
    }
    bool ok = fwrite(contents, 1, length, file) == length;
    int error = errno;
    if (fclose(file) != 0 && ok) {
        ok = false;
        error = errno;
    }
    if (!ok) {
        snprintf(message, messageSize, "cannot write %s: %s", path,
                 strerror(error));
        hostRemoveTemporary(path);
    }
    return ok;
}

void hostRemoveTemporary(const char *path)
{
    remove(path);
    const char *slash = strrchr(path, '/');
    if (slash) {
        char *directory = xmalloc((size_t)(slash - path) + 1);
        memcpy(directory, path, (size_t)(slash - path));
        directory[slash - path] = '\0';
        rmdir(directory);
        free(directory);
    }
}

#endif
