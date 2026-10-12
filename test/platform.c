#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
/* macOS declares mkdtemp only with its own extensions. */
#define _DARWIN_C_SOURCE
#endif

#include "platform.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int compareNames(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool hasSuffix(const char *name, const char *suffix)
{
    size_t length = strlen(name);
    size_t suffixLength = strlen(suffix);
    return length > suffixLength &&
           strcmp(name + length - suffixLength, suffix) == 0;
}

static void addName(char ***names, size_t *count, const char *name)
{
    *names = xrealloc(*names, (*count + 1) * sizeof **names);
    size_t length = strlen(name);
    (*names)[*count] = xmalloc(length + 1);
    memcpy((*names)[*count], name, length + 1);
    (*count)++;
}

/* A new temporary file, opened for update and removed when it is closed. */
static FILE *temporaryFile(void)
{
#ifdef _WIN32
    /* tmpfile of the Microsoft C runtime may write to the root directory. */
    char directory[MAX_PATH + 1];
    char path[MAX_PATH + 1];
    DWORD length = GetTempPathA(sizeof directory, directory);
    if (length == 0 || length > MAX_PATH ||
        GetTempFileNameA(directory, "dtr", 0, path) == 0) {
        return NULL;
    }
    /* D: the file is deleted when it is closed. */
    return fopen(path, "w+bD");
#else
    return tmpfile();
#endif
}

FILE *testInputStream(const char *data, size_t length)
{
    FILE *stream = temporaryFile();
    if (stream) {
        fwrite(data, 1, length, stream);
        rewind(stream);
    }
    return stream;
}

FILE *testOutputStream(void)
{
    return temporaryFile();
}

char *testCloseOutput(FILE *stream, size_t *length)
{
    char *data = NULL;
    rewind(stream);
    if (!readStream(stream, &data, length)) {
        data = xmalloc(1);
        data[0] = '\0';
        *length = 0;
    }
    fclose(stream);
    return data;
}

#ifdef _WIN32

bool testMakeTempDir(char *buffer, size_t size)
{
    char base[MAX_PATH + 1];
    DWORD length = GetTempPathA(sizeof base, base);
    if (length == 0 || length > MAX_PATH) {
        return false;
    }
    static unsigned counter;
    for (int attempt = 0; attempt < 100; attempt++) {
        snprintf(buffer, size, "%sdottr-test-%lu-%u", base,
                 (unsigned long)GetCurrentProcessId(), counter++);
        if (CreateDirectoryA(buffer, NULL)) {
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return false;
}

void testRemoveTree(const char *path)
{
    char pattern[4200];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(entry.cFileName, ".") == 0 ||
                strcmp(entry.cFileName, "..") == 0) {
                continue;
            }
            char child[4200];
            snprintf(child, sizeof child, "%s\\%s", path, entry.cFileName);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                testRemoveTree(child);
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryA(path);
}

char **testListFiles(const char *directory, const char *suffix, size_t *count)
{
    *count = 0;
    char pattern[4200];
    snprintf(pattern, sizeof pattern, "%s\\*", directory);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    char **names = NULL;
    do {
        if (hasSuffix(entry.cFileName, suffix)) {
            addName(&names, count, entry.cFileName);
        }
    } while (FindNextFileA(find, &entry));
    FindClose(find);
    if (*count > 0) {
        qsort(names, *count, sizeof *names, compareNames);
    }
    return names;
}

void testSetVariable(const char *name, const char *value)
{
    /* An empty value removes the variable. */
    _putenv_s(name, value ? value : "");
}

bool testRedirect(const char *inputPath, const char *outputPath,
                  SavedStreams *saved)
{
    int input = _open(inputPath, _O_RDONLY | _O_BINARY);
    int output = _open(outputPath, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                       0600);
    if (input < 0 || output < 0) {
        if (input >= 0) {
            _close(input);
        }
        if (output >= 0) {
            _close(output);
        }
        return false;
    }
    fflush(stdout);
    fflush(stderr);
    saved->input = _dup(0);
    saved->output = _dup(1);
    saved->error = _dup(2);
    _dup2(input, 0);
    _dup2(output, 1);
    _dup2(output, 2);
    _close(input);
    _close(output);
    return true;
}

void testRestore(SavedStreams *saved)
{
    fflush(stdout);
    fflush(stderr);
    _dup2(saved->input, 0);
    _dup2(saved->output, 1);
    _dup2(saved->error, 2);
    _close(saved->input);
    _close(saved->output);
    _close(saved->error);
}

#else

bool testMakeTempDir(char *buffer, size_t size)
{
    const char *base = getenv("TMPDIR");
    if (!base || !*base) {
        base = "/tmp";
    }
    snprintf(buffer, size, "%s/dottr-test-XXXXXX", base);
    return mkdtemp(buffer) != NULL;
}

void testRemoveTree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) {
        return;
    }
    if (S_ISDIR(info.st_mode)) {
        /* Make the directory writable and searchable, as tests may not. */
        chmod(path, 0700);
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[4200];
                snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
                testRemoveTree(child);
            }
            closedir(dir);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

char **testListFiles(const char *directory, const char *suffix, size_t *count)
{
    *count = 0;
    DIR *dir = opendir(directory);
    if (!dir) {
        return NULL;
    }
    char **names = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (hasSuffix(entry->d_name, suffix)) {
            addName(&names, count, entry->d_name);
        }
    }
    closedir(dir);
    if (*count > 0) {
        qsort(names, *count, sizeof *names, compareNames);
    }
    return names;
}

void testSetVariable(const char *name, const char *value)
{
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

bool testRedirect(const char *inputPath, const char *outputPath,
                  SavedStreams *saved)
{
    int input = open(inputPath, O_RDONLY);
    int output = open(outputPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (input < 0 || output < 0) {
        if (input >= 0) {
            close(input);
        }
        if (output >= 0) {
            close(output);
        }
        return false;
    }
    fflush(stdout);
    fflush(stderr);
    saved->input = dup(STDIN_FILENO);
    saved->output = dup(STDOUT_FILENO);
    saved->error = dup(STDERR_FILENO);
    dup2(input, STDIN_FILENO);
    dup2(output, STDOUT_FILENO);
    dup2(output, STDERR_FILENO);
    close(input);
    close(output);
    return true;
}

void testRestore(SavedStreams *saved)
{
    fflush(stdout);
    fflush(stderr);
    dup2(saved->input, STDIN_FILENO);
    dup2(saved->output, STDOUT_FILENO);
    dup2(saved->error, STDERR_FILENO);
    close(saved->input);
    close(saved->output);
    close(saved->error);
}

#endif
