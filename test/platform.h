#ifndef DOTTR_TEST_PLATFORM_H
#define DOTTR_TEST_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Operating system services of the test program that dottr itself does not
 * need. Programs under test are run with the host layer (dottr/host.h).
 */

/* Creates a new private temporary directory and stores its path in buffer. */
bool testMakeTempDir(char *buffer, size_t size);

/* Removes a directory and everything in it. */
void testRemoveTree(const char *path);

/*
 * The names of the files in directory that end in suffix, sorted, as a
 * heap-allocated array of heap-allocated strings. Returns NULL with count 0
 * if the directory cannot be read.
 */
char **testListFiles(const char *directory, const char *suffix,
                     size_t *count);

/* Sets an environment variable, or removes it if value is NULL. */
void testSetVariable(const char *name, const char *value);

/* A stream that reads the length bytes of data. */
FILE *testInputStream(const char *data, size_t length);

/* A stream that collects what is written to it, for testCloseOutput. */
FILE *testOutputStream(void);

/*
 * Closes a stream of testOutputStream and returns what was written to it, as
 * a heap-allocated NUL-terminated string of length bytes.
 */
char *testCloseOutput(FILE *stream, size_t *length);

/* Standard streams saved by testRedirect. */
typedef struct {
    int input;
    int output;
    int error;
} SavedStreams;

/*
 * Redirects standard input to read inputPath, and standard output and
 * standard error to write outputPath together, so that a program run with
 * hostRun inherits them. Returns false if a file cannot be opened.
 */
bool testRedirect(const char *inputPath, const char *outputPath,
                  SavedStreams *saved);

/* Restores the standard streams saved by testRedirect. */
void testRestore(SavedStreams *saved);

#endif
