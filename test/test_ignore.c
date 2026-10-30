#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/ignore.h"
#include "dottr/util.h"
#include "platform.h"
#include "test.h"

static void expectIgnore(const IgnoreList *list, const char *path,
                         bool expected)
{
    testCount++;
    if (ignoresPath(list, path) != expected) {
        TEST_FAIL("ignore %s: expected %s", path,
                  expected ? "ignored" : "retained");
    }
}

void runIgnoreTests(void)
{
    char temporary[4096];
    if (!testMakeTempDir(temporary, sizeof temporary)) {
        TEST_FAIL("ignore: cannot create temporary directory");
        return;
    }
    StrBuf path;
    strBufInit(&path);
    strBufAppendStr(&path, temporary);
    strBufAppendStr(&path, "/rules");
    static const char rules[] =
        "# Git stuff\n\\.git\n\\.gitignore\n\\.gitmodules\n^/\\.github\n"
        "\n# Environment setup\n^/packages.*\ninstall.sh\n"
        "\n# Repository docs and non-home "
        "content\n^/README.*\n^/AGENTS\\.md\n^/docker\n";
    FILE *file = fopen(path.data, "wb");
    if (!file) {
        TEST_FAIL("ignore: cannot write rules");
        goto done;
    }
    fwrite(rules, 1, strlen(rules), file);
    fclose(file);
    Arena arena;
    arenaInit(&arena);
    IgnoreList list = {0};
    char message[512];
    testCount++;
    if (!loadIgnoreList(&arena, path.data, &list, message, sizeof message)) {
        TEST_FAIL("ignore rules: %s", message);
    } else {
        if (list.count != 9) {
            TEST_FAIL("ignore: expected 9 rules, got %zu", list.count);
        }
        expectIgnore(&list, ".git", true);
        expectIgnore(&list, ".config/theme/.git", true);
        expectIgnore(&list, ".config/.gitignore", true);
        expectIgnore(&list, ".gitmodules", true);
        expectIgnore(&list, ".github", true);
        expectIgnore(&list, ".github/workflows/build.yml", true);
        expectIgnore(&list, "packages.dotfile", true);
        expectIgnore(&list, "packages.vm.dotfile", true);
        expectIgnore(&list, "packages.d", true);
        expectIgnore(&list, "install.sh", true);
        expectIgnore(&list, "installXsh", true);
        expectIgnore(&list, "README.md", true);
        expectIgnore(&list, "AGENTS.md", true);
        expectIgnore(&list, "docker", true);
        expectIgnore(&list, "my.git", false);
        expectIgnore(&list, "dockerfile", false);
        expectIgnore(&list, "fooinstall.sh", false);
        expectIgnore(&list, ".config/README.md", false);
        expectIgnore(&list, ".config/nvim/init.lua", false);
        expectIgnore(&list, ".gitconfig", false);
    }
    arenaFree(&arena);

    static const char extra[] = "  # comment\r\n\\#.*\\# # inline comment\r\n"
                                "bar/.*x\nfoo+\ncolou?r\n";
    file = fopen(path.data, "wb");
    if (file) {
        fwrite(extra, 1, strlen(extra), file);
        fclose(file);
        arenaInit(&arena);
        list = (IgnoreList){0};
        testCount++;
        if (!loadIgnoreList(&arena, path.data, &list, message,
                            sizeof message)) {
            TEST_FAIL("ignore escaped comments: %s", message);
        } else {
            expectIgnore(&list, "#backup#", true);
            expectIgnore(&list, "foo/bar/bazqux", true);
            expectIgnore(&list, "barbazqux", false);
            expectIgnore(&list, "foooo", true);
            expectIgnore(&list, "fo", false);
            expectIgnore(&list, "color", true);
            expectIgnore(&list, "colour", true);
            expectIgnore(&list, "colouur", false);
        }
        arenaFree(&arena);
    }

    file = fopen(path.data, "wb");
    if (file) {
        fputs("# line one\nunsupported[group]\n", file);
        fclose(file);
        arenaInit(&arena);
        list = (IgnoreList){0};
        testCount++;
        if (loadIgnoreList(&arena, path.data, &list, message, sizeof message) ||
            !strstr(message, ":2: invalid ignore rule")) {
            TEST_FAIL("ignore unsupported expression: %s", message);
        }
        arenaFree(&arena);
    }
done:
    testRemoveTree(temporary);
    strBufFree(&path);
}
