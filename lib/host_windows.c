/* Host layer for Windows. Strings are UTF-8, converted at the API boundary. */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dottr/host.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "dottr/util.h"

/* Converts UTF-8 to a newly allocated UTF-16 string; NULL on failure. */
static wchar_t *toWide(const char *text)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }
    wchar_t *wide = xmalloc((size_t)length * sizeof *wide);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
    return wide;
}

/* Converts UTF-16 to a newly allocated UTF-8 string; NULL on failure. */
static char *fromWide(const wchar_t *wide)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *text = xmalloc((size_t)length);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, length, NULL, NULL);
    return text;
}

/* The value of an environment variable as UTF-8, or NULL if it is unset. */
static char *environmentValue(const wchar_t *name)
{
    DWORD size = GetEnvironmentVariableW(name, NULL, 0);
    if (size == 0) {
        return NULL;
    }
    wchar_t *value = xmalloc(size * sizeof *value);
    DWORD length = GetEnvironmentVariableW(name, value, size);
    char *text = length > 0 && length < size ? fromWide(value) : NULL;
    free(value);
    return text;
}

/* Describes the error code of the last failed call. */
static void describeError(DWORD error, char *message, size_t messageSize)
{
    wchar_t buffer[512];
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        error, 0, buffer, sizeof buffer / sizeof buffer[0], NULL);
    while (length > 0 &&
           (buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n' ||
            buffer[length - 1] == L'.')) {
        buffer[--length] = L'\0';
    }
    char *text = length > 0 ? fromWide(buffer) : NULL;
    snprintf(message, messageSize, "%s", text ? text : "unknown error");
    free(text);
}

void hostInitialize(void)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
}

const char *hostHomeDirectory(void)
{
    /* Read on every call, as the POSIX layer reads HOME. */
    static char *home;
    free(home);
    home = environmentValue(L"USERPROFILE");
    if (home && !*home) {
        free(home);
        home = NULL;
    }
    return home;
}

/*
 * Reads the attributes of path, following symbolic links and junctions as
 * the predicates of `check` require.
 */
static bool attributes(const char *path, DWORD *result)
{
    wchar_t *wide = toWide(path);
    if (!wide) {
        return false;
    }
    HANDLE file = CreateFileW(
        wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileInformationByHandle(file, &info) != 0;
    CloseHandle(file);
    if (ok) {
        *result = info.dwFileAttributes;
    }
    return ok;
}

bool hostIsFile(const char *path)
{
    DWORD value;
    return attributes(path, &value) && !(value & FILE_ATTRIBUTE_DIRECTORY);
}

bool hostIsDirectory(const char *path)
{
    DWORD value;
    return attributes(path, &value) && (value & FILE_ATTRIBUTE_DIRECTORY);
}

static bool isSeparator(char c)
{
    return c == '/' || c == '\\';
}

/* Whether the last component of path contains a '.'. */
static bool hasExtension(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (isSeparator(*p)) {
            name = p + 1;
        }
    }
    return strchr(name, '.') != NULL;
}

/*
 * Whether candidate names an executable: the file itself if its name has an
 * extension, or the file with one of the extensions of PATHEXT appended.
 */
static bool findExecutable(StrBuf *candidate, const char *extensions)
{
    if (hasExtension(candidate->data) && hostIsFile(candidate->data)) {
        return true;
    }
    size_t base = candidate->length;
    const char *start = extensions;
    for (;;) {
        const char *end = strchr(start, ';');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length > 0) {
            candidate->length = base;
            candidate->data[base] = '\0';
            strBufAppend(candidate, start, length);
            if (hostIsFile(candidate->data)) {
                return true;
            }
        }
        if (!end) {
            break;
        }
        start = end + 1;
    }
    candidate->length = base;
    candidate->data[base] = '\0';
    return false;
}

/*
 * Finds the executable that command names, as `check command` and hostRun
 * do: a path if command contains a separator or drive, and otherwise the
 * first match in the directories of PATH, with the extensions of PATHEXT.
 * Stores the path of the executable in result.
 */
static bool findCommand(const char *command, StrBuf *result)
{
    if (*command == '\0') {
        return false;
    }

    char *extensions = environmentValue(L"PATHEXT");
    const char *pathext = extensions ? extensions : ".COM;.EXE;.BAT;.CMD";
    bool found = false;

    if (strchr(command, '/') || strchr(command, '\\') ||
        strchr(command, ':')) {
        strBufClear(result);
        strBufAppendStr(result, command);
        found = findExecutable(result, pathext);
    } else {
        char *path = environmentValue(L"PATH");
        const char *start = path ? path : "";
        for (;;) {
            const char *end = strchr(start, ';');
            size_t length = end ? (size_t)(end - start) : strlen(start);
            if (length > 0) {
                strBufClear(result);
                strBufAppend(result, start, length);
                if (!isSeparator(result->data[result->length - 1])) {
                    strBufAppendChar(result, '\\');
                }
                strBufAppendStr(result, command);
                if (findExecutable(result, pathext)) {
                    found = true;
                    break;
                }
            }
            if (!end) {
                break;
            }
            start = end + 1;
        }
        free(path);
    }

    free(extensions);
    return found;
}

bool hostCommandExists(const char *command)
{
    StrBuf path;
    strBufInit(&path);
    bool found = findCommand(command, &path);
    strBufFree(&path);
    return found;
}

static bool makeOne(const char *path, char *message, size_t messageSize)
{
    wchar_t *wide = toWide(path);
    if (!wide) {
        snprintf(message, messageSize, "cannot create %s: invalid UTF-8",
                 path);
        return false;
    }
    BOOL created = CreateDirectoryW(wide, NULL);
    DWORD error = GetLastError();
    free(wide);
    if (created) {
        return true;
    }
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_ACCESS_DENIED) {
        /* A drive root reports ERROR_ACCESS_DENIED. */
        if (hostIsDirectory(path)) {
            return true;
        }
        if (error == ERROR_ALREADY_EXISTS) {
            snprintf(message, messageSize, "%s exists and is not a directory",
                     path);
            return false;
        }
    }
    char reason[256];
    describeError(error, reason, sizeof reason);
    snprintf(message, messageSize, "cannot create %s: %s", path, reason);
    return false;
}

/*
 * The length of the root of path, which is not created: "C:\", "\", or
 * "\\server\share\".
 */
static size_t rootLength(const char *path)
{
    if (isSeparator(path[0]) && isSeparator(path[1])) {
        size_t i = 2;
        for (int part = 0; part < 2; part++) {
            while (path[i] && !isSeparator(path[i])) {
                i++;
            }
            if (path[i]) {
                i++;
            }
        }
        return i;
    }
    if (path[0] && path[1] == ':') {
        return isSeparator(path[2]) ? 3 : 2;
    }
    return isSeparator(path[0]) ? 1 : 0;
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
    for (size_t i = rootLength(path); i < length && ok; i++) {
        if (isSeparator(copy[i]) && i > 0 && !isSeparator(copy[i - 1])) {
            char separator = copy[i];
            copy[i] = '\0';
            ok = makeOne(copy, message, messageSize);
            copy[i] = separator;
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
    return false;
}

/*
 * Appends an argument to a command line, quoted as CommandLineToArgvW and
 * the C runtime parse it: backslashes are literal except before a quote.
 */
static void appendArgument(StrBuf *line, const char *argument)
{
    if (line->length > 0) {
        strBufAppendChar(line, ' ');
    }
    if (*argument && !strpbrk(argument, " \t\n\v\"")) {
        strBufAppendStr(line, argument);
        return;
    }
    strBufAppendChar(line, '"');
    for (const char *p = argument;; p++) {
        size_t backslashes = 0;
        while (*p == '\\') {
            backslashes++;
            p++;
        }
        if (*p == '\0') {
            for (size_t i = 0; i < 2 * backslashes; i++) {
                strBufAppendChar(line, '\\');
            }
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < 2 * backslashes + 1; i++) {
                strBufAppendChar(line, '\\');
            }
        } else {
            for (size_t i = 0; i < backslashes; i++) {
                strBufAppendChar(line, '\\');
            }
        }
        strBufAppendChar(line, *p);
    }
    strBufAppendChar(line, '"');
}

typedef struct {
    wchar_t *name;
    /* The previous value, or NULL if the variable was unset. */
    wchar_t *value;
} SavedVariable;

static wchar_t *savedValue(const wchar_t *name)
{
    DWORD size = GetEnvironmentVariableW(name, NULL, 0);
    if (size == 0) {
        return NULL;
    }
    wchar_t *value = xmalloc(size * sizeof *value);
    if (GetEnvironmentVariableW(name, value, size) >= size) {
        free(value);
        return NULL;
    }
    return value;
}

/*
 * Runs argv as hostRun does. The child inherits dottr's environment, so the
 * variables are set in dottr's own environment while the child is created
 * and restored afterwards. If silent, the child's standard output and
 * standard error go to NUL.
 */
static int run(char *const argv[], const HostVariable *variables,
               size_t variableCount, bool silent)
{
    fflush(stdout);
    fflush(stderr);

    StrBuf line;
    strBufInit(&line);
    for (size_t i = 0; argv[i]; i++) {
        appendArgument(&line, argv[i]);
    }
    wchar_t *commandLine = toWide(line.data);
    strBufFree(&line);
    if (!commandLine) {
        fprintf(stderr, "dottr: error: cannot start %s: invalid UTF-8\n",
                argv[0]);
        return -1;
    }

    SavedVariable *saved = xmalloc((variableCount + 1) * sizeof *saved);
    size_t savedCount = 0;
    bool ok = true;
    for (size_t i = 0; i < variableCount && ok; i++) {
        wchar_t *name = toWide(variables[i].name);
        wchar_t *value = toWide(variables[i].value);
        if (!name || !value) {
            free(name);
            free(value);
            fprintf(stderr, "dottr: error: cannot set %s: invalid UTF-8\n",
                    variables[i].name);
            ok = false;
            break;
        }
        saved[savedCount].name = name;
        saved[savedCount].value = savedValue(name);
        savedCount++;
        if (!SetEnvironmentVariableW(name, value)) {
            char reason[256];
            describeError(GetLastError(), reason, sizeof reason);
            fprintf(stderr, "dottr: error: cannot set %s: %s\n",
                    variables[i].name, reason);
            ok = false;
        }
        free(value);
    }

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    HANDLE null = INVALID_HANDLE_VALUE;
    if (ok && silent) {
        SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
        null = CreateFileW(L"NUL", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                           OPEN_EXISTING, 0, NULL);
        if (null != INVALID_HANDLE_VALUE) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = null;
            startup.hStdError = null;
        }
    }

    /*
     * CreateProcessW finds a program only with the extension .exe and
     * searches other directories before PATH, so a program found as
     * hostCommandExists finds it is named directly. Batch files are left to
     * CreateProcessW, which runs them with cmd.exe.
     */
    StrBuf found;
    strBufInit(&found);
    wchar_t *application = NULL;
    if (findCommand(argv[0], &found) && found.length > 4 &&
        (_stricmp(found.data + found.length - 4, ".exe") == 0 ||
         _stricmp(found.data + found.length - 4, ".com") == 0)) {
        application = toWide(found.data);
    }
    strBufFree(&found);

    PROCESS_INFORMATION process;
    int status = -1;
    if (ok) {
        if (CreateProcessW(application, commandLine, NULL, NULL, TRUE, 0, NULL,
                           NULL, &startup, &process)) {
            CloseHandle(process.hThread);
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD code;
            if (GetExitCodeProcess(process.hProcess, &code)) {
                status = (int)code;
            }
            CloseHandle(process.hProcess);
        } else {
            DWORD error = GetLastError();
            char reason[256];
            describeError(error, reason, sizeof reason);
            fprintf(stderr, "dottr: error: cannot execute %s: %s\n", argv[0],
                    reason);
            /* As on POSIX systems, a missing program is status 127. */
            if (error == ERROR_FILE_NOT_FOUND ||
                error == ERROR_PATH_NOT_FOUND) {
                status = 127;
            }
        }
    }

    if (null != INVALID_HANDLE_VALUE) {
        CloseHandle(null);
    }
    for (size_t i = savedCount; i-- > 0;) {
        SetEnvironmentVariableW(saved[i].name, saved[i].value);
        free(saved[i].name);
        free(saved[i].value);
    }
    free(saved);
    free(application);
    free(commandLine);
    return status;
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
    wchar_t base[MAX_PATH + 1];
    DWORD baseLength = GetTempPathW(sizeof base / sizeof base[0], base);
    if (baseLength == 0 || baseLength > MAX_PATH) {
        snprintf(message, messageSize, "cannot find the temporary directory");
        return false;
    }

    /* A new directory, named after the process and a counter. */
    static unsigned counter;
    wchar_t directory[MAX_PATH + 64];
    bool created = false;
    for (int attempt = 0; attempt < 100 && !created; attempt++) {
        swprintf(directory, sizeof directory / sizeof directory[0],
                 L"%lsdottr-%lu-%u", base, (unsigned long)GetCurrentProcessId(),
                 counter++);
        if (CreateDirectoryW(directory, NULL)) {
            created = true;
        } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (!created) {
        char reason[256];
        describeError(GetLastError(), reason, sizeof reason);
        snprintf(message, messageSize,
                 "cannot create a temporary directory: %s", reason);
        return false;
    }

    char *directoryText = fromWide(directory);
    int written = snprintf(path, pathSize, "%s\\%s",
                           directoryText ? directoryText : "", name);
    free(directoryText);
    if (written < 0 || (size_t)written >= pathSize) {
        RemoveDirectoryW(directory);
        snprintf(message, messageSize, "the temporary directory is too long");
        return false;
    }

    wchar_t *widePath = toWide(path);
    FILE *file = widePath ? _wfopen(widePath, L"wb") : NULL;
    free(widePath);
    if (!file) {
        snprintf(message, messageSize, "cannot create %s", path);
        RemoveDirectoryW(directory);
        return false;
    }
    bool ok = fwrite(contents, 1, length, file) == length;
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        snprintf(message, messageSize, "cannot write %s", path);
        hostRemoveTemporary(path);
    }
    return ok;
}

void hostRemoveTemporary(const char *path)
{
    wchar_t *wide = toWide(path);
    if (!wide) {
        return;
    }
    DeleteFileW(wide);
    wchar_t *separator = wcsrchr(wide, L'\\');
    if (separator) {
        *separator = L'\0';
        RemoveDirectoryW(wide);
    }
    free(wide);
}

#endif
