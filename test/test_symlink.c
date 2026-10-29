#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "dottr/host.h"
#include "dottr/symlink.h"
#include "dottr/util.h"
#include "platform.h"
#include "test.h"

typedef struct {
    Arena arena;
    const char *root;
} Filesystem;

static const char *path(Filesystem *fs, const char *relative)
{
    size_t length = strlen(fs->root) + strlen(relative) + 2;
    char *result = arenaAlloc(&fs->arena, length);
    snprintf(result, length, "%s/%s", fs->root, relative);
    return result;
}

static bool directory(Filesystem *fs, const char *relative)
{
    char message[512];
    if (!hostCreateDirectory(path(fs, relative), message, sizeof message)) {
        TEST_FAIL("symlink setup: %s", message);
        return false;
    }
    return true;
}

static bool writeFile(Filesystem *fs, const char *relative, const char *text)
{
    FILE *file = fopen(path(fs, relative), "wb");
    if (!file) {
        TEST_FAIL("symlink setup: cannot write %s", relative);
        return false;
    }
    bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        TEST_FAIL("symlink setup: cannot finish writing %s", relative);
    }
    return ok;
}

static void expectContents(Filesystem *fs, const char *relative,
                           const char *text)
{
    testCount++;
    FILE *file = fopen(path(fs, relative), "rb");
    char buffer[256];
    size_t length = file ? fread(buffer, 1, sizeof buffer - 1, file) : 0;
    buffer[length] = '\0';
    if (file) {
        fclose(file);
    }
    if (!file || strcmp(buffer, text) != 0) {
        TEST_FAIL("symlink %s: expected contents %s, got %s", relative, text,
                  buffer);
    }
}

static void expectKind(Filesystem *fs, const char *relative,
                       HostPathKind expected)
{
    testCount++;
    HostPathKind kind;
    char message[512];
    if (!hostPathKind(path(fs, relative), &kind, message, sizeof message)) {
        TEST_FAIL("symlink inspect %s: %s", relative, message);
    } else if (kind != expected) {
        TEST_FAIL("symlink %s: expected kind %d, got %d", relative, expected,
                  kind);
    }
}

static void expectDeployment(Filesystem *fs, const char *source,
                             const char *target, bool tree, bool plan,
                             bool success, const char *diagnostic)
{
    testCount++;
    char message[512] = "";
    bool result = deploySymlink(path(fs, source), path(fs, target), tree, plan,
                                message, sizeof message);
    if (result != success || (diagnostic && !strstr(message, diagnostic))) {
        TEST_FAIL("symlink %s -> %s: expected %s (%s), got %s (%s)", source,
                  target, success ? "success" : "failure",
                  diagnostic ? diagnostic : "", result ? "success" : "failure",
                  message);
    }
}

static bool rawLink(Filesystem *fs, const char *source, const char *target)
{
    char message[512];
    if (!hostCreateSymlink(path(fs, source), path(fs, target), message,
                           sizeof message)) {
        TEST_FAIL("symlink setup %s: %s", target, message);
        return false;
    }
    return true;
}

static void expectMatches(Filesystem *fs, const char *source,
                          const char *target)
{
    testCount++;
    bool matches = false;
    char message[512];
    if (!hostSymlinkMatches(path(fs, source), path(fs, target), &matches,
                            message, sizeof message) ||
        !matches) {
        TEST_FAIL("symlink %s does not name %s", target, source);
    }
}

void runSymlinkTests(void)
{
    Filesystem fs = {0};
    arenaInit(&fs.arena);
    char temporary[4096];
    char message[512];
    if (!testMakeTempDir(temporary, sizeof temporary)) {
        TEST_FAIL("symlink: cannot create test directory");
        return;
    }
    /* Use the canonical temporary root, including on systems with /tmp links.
     */
    char *root = hostCanonicalPath(temporary, message, sizeof message);
    if (!root) {
        TEST_FAIL("symlink test directory: %s", message);
        testRemoveTree(temporary);
        return;
    }
    fs.root = root;
    if (!directory(&fs, "source/sub") ||
        !writeFile(&fs, "source/a", "source") ||
        !writeFile(&fs, "source/sub/b", "nested")) {
        goto done;
    }
    /* No files should be changed by planning, including missing ancestors. */
    expectDeployment(&fs, "source", "new/target", true, true, true, NULL);
    expectKind(&fs, "new", HOST_PATH_MISSING);
    expectDeployment(&fs, "missing", "missing-plan", false, true, true, NULL);
    expectKind(&fs, "missing-plan", HOST_PATH_MISSING);
    expectDeployment(&fs, "missing", "missing-apply", false, false, false,
                     "source");
    expectDeployment(&fs, "source/a", "absent/file", false, false, false,
                     "parent");
    expectDeployment(&fs, "source/a", "planned-parent/file", false, true, true,
                     NULL);
    expectKind(&fs, "planned-parent", HOST_PATH_MISSING);

    /* Some Windows accounts lack both Developer Mode and symlink privileges. */
    if (!hostCreateSymlink(path(&fs, "source/a"), path(&fs, "capability-probe"),
                           message, sizeof message)) {
#ifdef _WIN32
        if (strstr(message, "Developer Mode")) {
            printf("skipped symlink filesystem tests: %s\n", message);
            goto done;
        }
#endif
        TEST_FAIL("symlink creation: %s", message);
        goto done;
    }
    expectDeployment(&fs, "source/a", "file-link", false, false, true, NULL);
    expectDeployment(&fs, "source/a", "file-link", false, false, true, NULL);
    expectMatches(&fs, "source/a", "file-link");
    testCount++;
    if (!hostCreateSymlink("source/a", path(&fs, "relative-link"), message,
                           sizeof message)) {
        TEST_FAIL("relative link setup: %s", message);
    } else {
        expectDeployment(&fs, "source/a", "relative-link", false, false, true,
                         NULL);
    }
    writeFile(&fs, "source/a", "updated");
    expectContents(&fs, "file-link", "updated");

    writeFile(&fs, "existing", "local");
    expectDeployment(&fs, "source/a", "existing", false, false, false,
                     "conflicts");
    expectContents(&fs, "existing", "local");
    rawLink(&fs, "source/sub/b", "different-link");
    expectDeployment(&fs, "source/a", "different-link", false, false, false,
                     "conflicts");
    expectMatches(&fs, "source/sub/b", "different-link");
    rawLink(&fs, "missing", "dangling");
    expectDeployment(&fs, "source/a", "dangling", false, false, false,
                     "conflicts");
    expectKind(&fs, "dangling", HOST_PATH_SYMLINK);
    expectDeployment(&fs, "source/a", "dangling/injected", false, false, false,
                     "parent");
    expectDeployment(&fs, "dangling", "dangling-source-link", false, false,
                     true, NULL);
    expectMatches(&fs, "dangling", "dangling-source-link");

    expectDeployment(&fs, "source", "directory-link", false, false, true, NULL);
    expectDeployment(&fs, "source", "directory-link", false, false, true, NULL);
    expectKind(&fs, "directory-link", HOST_PATH_SYMLINK);
    expectDeployment(&fs, "source", "directory-link", true, false, false,
                     "conflicts");
    expectDeployment(&fs, "source/a", "directory-link/injected", false, false,
                     false, "parent");
    expectDeployment(&fs, "source", "directory-link/injected", true, false,
                     false, "parent");
    expectKind(&fs, "source/injected", HOST_PATH_MISSING);

    /* Source directory links, even cycles, must be leaves in tree mode. */
    rawLink(&fs, "source", "source/cycle");
    expectDeployment(&fs, "source", "tree", true, false, true, NULL);
    expectKind(&fs, "tree", HOST_PATH_DIRECTORY);
    expectKind(&fs, "tree/sub", HOST_PATH_DIRECTORY);
    expectKind(&fs, "tree/a", HOST_PATH_SYMLINK);
    expectKind(&fs, "tree/cycle", HOST_PATH_SYMLINK);
    expectMatches(&fs, "source/cycle", "tree/cycle");
    writeFile(&fs, "tree/unrelated", "retain");
    expectDeployment(&fs, "source", "tree", true, false, true, NULL);
    expectContents(&fs, "tree/unrelated", "retain");
    expectDeployment(&fs, "source", "deep/parent/tree", true, false, true,
                     NULL);
    expectContents(&fs, "deep/parent/tree/sub/b", "nested");

    directory(&fs, "conflict");
    writeFile(&fs, "conflict/cycle", "preserved");
    expectDeployment(&fs, "source", "conflict", true, false, false,
                     "conflicts");
    expectKind(&fs, "conflict/a", HOST_PATH_MISSING);
    expectContents(&fs, "conflict/cycle", "preserved");
    expectDeployment(&fs, "source", "source/inside", true, false, false,
                     "overlap");
    expectDeployment(&fs, "source/sub", "source", true, false, false,
                     "overlap");
    expectDeployment(&fs, "source", "source", true, false, false, "same path");
    expectDeployment(&fs, "source/a", "source/a", false, false, false,
                     "same path");

#ifndef _WIN32
    testCount++;
    if (mkfifo(path(&fs, "source/special"), 0600) != 0) {
        TEST_FAIL("symlink setup: cannot create FIFO");
    } else {
        expectDeployment(&fs, "source/special", "special-link", false, false,
                         false, "source");
        expectDeployment(&fs, "source", "special-tree", true, false, false,
                         "source");
        expectKind(&fs, "special-tree", HOST_PATH_MISSING);
        remove(path(&fs, "source/special"));
    }
    if (!hostIsRoot()) {
        chmod(path(&fs, "source/sub"), 0000);
        expectDeployment(&fs, "source", "unreadable-tree", true, false, false,
                         "cannot read");
        expectKind(&fs, "unreadable-tree", HOST_PATH_MISSING);
        chmod(path(&fs, "source/sub"), 0700);
    }
#endif

    testCount++;
    const char *relative = "./test/cases/apply-empty.dotfile";
    const char *destination = path(&fs, "relative-source");
    if (!deploySymlink(relative, destination, false, false, message,
                       sizeof message)) {
        TEST_FAIL("symlink relative source: %s", message);
    } else {
        char *absolute = hostAbsolutePath(relative, message, sizeof message);
        bool matches = false;
        if (!absolute ||
            !hostSymlinkMatches(absolute, destination, &matches, message,
                                sizeof message) ||
            !matches) {
            TEST_FAIL("symlink relative source was not made absolute");
        }
        free(absolute);
    }

done:
    testRemoveTree(root);
    free(root);
    arenaFree(&fs.arena);
}
