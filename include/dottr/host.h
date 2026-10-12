#ifndef DOTTR_HOST_H
#define DOTTR_HOST_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The host layer: the operating system services dottr uses. It is
 * implemented by host_posix.c on Linux and macOS and by host_windows.c on
 * Windows. Strings are UTF-8.
 */

/*
 * Prepares the process: on Windows, puts the standard streams in binary mode,
 * so that output has the same line endings on every system, and sets the
 * console output code page to UTF-8. Called first by every program.
 */
void hostInitialize(void);

/*
 * Home directory of the invoking user. On POSIX systems: $HOME, or the
 * password database entry if $HOME is unset or empty. On Windows:
 * %USERPROFILE%. Returns NULL if none is available.
 */
const char *hostHomeDirectory(void);

/* Why hostHomeDirectory returns NULL, for a diagnostic. */
#ifdef _WIN32
#define HOST_HOME_MISSING "USERPROFILE is unset"
#else
#define HOST_HOME_MISSING                                                      \
    "HOME is unset and the password database has no entry"
#endif

/* Predicates of `check`. Symbolic links are followed. */
bool hostCommandExists(const char *command);
bool hostIsFile(const char *path);
bool hostIsDirectory(const char *path);

/*
 * Creates a directory and any missing parents. On failure writes a
 * description of the error to message and returns false.
 */
bool hostCreateDirectory(const char *path, char *message, size_t messageSize);

/* Whether dottr runs as root. Always false on Windows. */
bool hostIsRoot(void);

/* An environment variable to set in a child process. */
typedef struct {
    const char *name;
    const char *value;
} HostVariable;

/*
 * Runs argv[0] through PATH with inherited standard streams and waits for it.
 * The child inherits dottr's environment with the given variables added or
 * replaced. Returns the exit status, 128 + N if the process was terminated by
 * signal N, or -1 if no process could be started. If the program cannot be
 * executed, the child reports the error and exits with status 127.
 */
int hostRun(char *const argv[], const HostVariable *variables,
            size_t variableCount);

/*
 * Runs argv as hostRun does, without added variables, and discards its
 * standard output and standard error.
 */
int hostRunSilently(char *const argv[]);

/*
 * Writes length bytes of contents to a new file named name in a new private
 * temporary directory and stores its path in path. Returns false and writes
 * a description to message on failure. hostRemoveTemporary removes the file
 * and its directory.
 */
bool hostWriteTemporary(const char *name, const char *contents, size_t length,
                        char *path, size_t pathSize, char *message,
                        size_t messageSize);
void hostRemoveTemporary(const char *path);

#endif
