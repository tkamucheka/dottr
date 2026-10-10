#define _POSIX_C_SOURCE 200809L

#include "dottr/host.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dottr/util.h"

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

bool hostIsRoot(void)
{
    return geteuid() == 0;
}

int hostRun(char *const argv[], const HostVariable *variables,
            size_t variableCount)
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
