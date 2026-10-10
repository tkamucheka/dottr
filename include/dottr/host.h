#ifndef DOTTR_HOST_H
#define DOTTR_HOST_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Home directory of the invoking user: $HOME, or the password database entry
 * if $HOME is unset or empty. Returns NULL if neither is available.
 */
const char *hostHomeDirectory(void);

/*
 * Predicates of `check`. Symbolic links are
 * followed.
 */
bool hostCommandExists(const char *command);
bool hostIsFile(const char *path);
bool hostIsDirectory(const char *path);

/*
 * Creates a directory and any missing parents. On failure writes a
 * description of the error to message and returns false.
 */
bool hostCreateDirectory(const char *path, char *message, size_t messageSize);

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

#endif
