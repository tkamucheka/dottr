#define _POSIX_C_SOURCE 200809L

/*
 * Formatter tests. Every manifest in test/cases that lexes and parses, and the
 * repository manifest, must format to an equivalent manifest (formatEquivalent)
 * and be unchanged by a second formatting pass. formatEquivalent itself is
 * tested on pairs that differ.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/format.h"
#include "dottr/lexer.h"
#include "dottr/parser.h"
#include "dottr/util.h"
#include "test.h"

static bool parses(const char *source, size_t length)
{
    Arena arena;
    arenaInit(&arena);
    Diag diag;
    diagInit(&diag, "", NULL);
    TokenList tokens;
    Manifest manifest;
    bool ok = lexSource(&arena, source, length, &tokens, &diag) &&
              parseManifest(&arena, &tokens, &manifest, &diag);
    arenaFree(&arena);
    return ok;
}

/* Checks the formatter properties for one manifest that parses. */
static void checkProperties(const char *name, const char *source,
                            size_t length)
{
    Diag diag;
    diagInit(&diag, name, NULL);
    StrBuf once;
    StrBuf twice;
    strBufInit(&once);
    strBufInit(&twice);
    testCount++;

    if (!formatManifest(source, length, &once, &diag)) {
        TEST_FAIL("format %s: the manifest parses but was not formatted",
                  name);
    } else if (!parses(once.data, once.length)) {
        TEST_FAIL("format %s: formatted output does not parse:\n%s", name,
                  once.data);
    } else if (!formatEquivalent(source, length, once.data, once.length)) {
        TEST_FAIL("format %s: formatted output is not equivalent:\n%s", name,
                  once.data);
    } else if (!formatManifest(once.data, once.length, &twice, &diag) ||
               twice.length != once.length ||
               memcmp(twice.data, once.data, once.length) != 0) {
        TEST_FAIL("format %s: formatting is not idempotent\n--- once\n%s"
                  "--- twice\n%s---",
                  name, once.data, twice.data ? twice.data : "");
    }

    strBufFree(&once);
    strBufFree(&twice);
}

static void checkFile(const char *path)
{
    char *source;
    size_t length;
    if (!readFile(path, &source, &length)) {
        testCount++;
        TEST_FAIL("format: cannot read %s", path);
        return;
    }
    if (parses(source, length)) {
        checkProperties(path, source, length);
    }
    free(source);
}

static void testFixtures(void)
{
    checkFile("manifest.dotfile");

    DIR *dir = opendir("test/cases");
    if (!dir) {
        testCount++;
        TEST_FAIL("format: cannot open test/cases");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length > 8 && strcmp(entry->d_name + length - 8, ".dotfile") == 0) {
            char path[1024];
            snprintf(path, sizeof path, "test/cases/%s", entry->d_name);
            checkFile(path);
        }
    }
    closedir(dir);
}

static void expectFormat(const char *name, const char *source,
                         const char *expected)
{
    Diag diag;
    diagInit(&diag, name, NULL);
    StrBuf out;
    strBufInit(&out);
    testCount++;
    if (!formatManifest(source, strlen(source), &out, &diag)) {
        TEST_FAIL("format %s: not formatted", name);
    } else if (strcmp(out.data ? out.data : "", expected) != 0) {
        TEST_FAIL("format %s:\n--- expected\n%s--- actual\n%s---", name,
                  expected, out.data ? out.data : "");
    }
    strBufFree(&out);
    checkProperties(name, source, strlen(source));
}

static void testCases(void)
{
    expectFormat("empty", "", "");
    expectFormat("blank lines only", "\n\n  \n", "");
    expectFormat("comments only", "  # a  \n\n\n# b", "# a\n\n# b\n");
    expectFormat("carriage returns", "pkg a do\r\n  needs b\r\nend\r\n",
                 "pkg a do\n  needs b\nend\n");
    expectFormat("no final line end", "pkg a", "pkg a\n");
    expectFormat("carriage return inside comment", "# a\t\r\t\npkg a\n",
                 "# a\npkg a\n");
    expectFormat("raw block at end of input",
                 "pkg a do\napply do\nshell do\n```\necho\n```\nend\nend\nend",
                 "pkg a do\n  apply do\n    shell do\n      ```\n      echo\n"
                 "      ```\n    end\n  end\nend\n");
    expectFormat("empty raw block",
                 "pkg a do\n apply do\n  shell do\n   ```zsh\n   ```\n  end\n"
                 " end\nend\n",
                 "pkg a do\n  apply do\n    shell do\n      ```zsh\n"
                 "      ```\n    end\n  end\nend\n");
    expectFormat("tab margin",
                 "pkg a do\n\tapply do\n\t\tshell do\n\t\t\t```\n\t\t\tif x; "
                 "then\n\t\t\t\techo\n\t\t\tfi\n\t\t\t```\n\t\tend\n\tend\n"
                 "end\n",
                 "pkg a do\n  apply do\n    shell do\n      ```\n      if x; "
                 "then\n      \techo\n      fi\n      ```\n    end\n  end\n"
                 "end\n");
    expectFormat("apply_unless and empty branches",
                 "pkg a do\napply_if A do\nelse\nend\napply_unless A do\nend\n"
                 "end\n",
                 "pkg a do\n  apply_if A do\n  else\n  end\n"
                 "  apply_unless A do\n  end\nend\n");
}

static void expectEquivalent(const char *name, const char *a, const char *b,
                             bool expected)
{
    testCount++;
    if (formatEquivalent(a, strlen(a), b, strlen(b)) != expected) {
        TEST_FAIL("formatEquivalent %s: expected %s", name,
                  expected ? "equivalent" : "different");
    }
}

static void testEquivalence(void)
{
    const char *raw = "pkg a do\n apply do\n  shell do\n   ```\n   %s\n   ```"
                      "\n  end\n end\nend\n";
    char x[256];
    char y[256];
    snprintf(x, sizeof x, raw, "echo a");
    snprintf(y, sizeof y, raw, "echo b");

    expectEquivalent("whitespace", "pkg   a\n\n", "pkg a", true);
    expectEquivalent("token text", "pkg a\n", "pkg b\n", false);
    expectEquivalent("string lexeme", "pkg \"a\"\n", "pkg a\n", false);
    expectEquivalent("raw contents", x, y, false);
    expectEquivalent("raw dialect",
                     "pkg a do\napply do\nshell do\n```sh\n```\nend\nend\nend\n",
                     "pkg a do\napply do\nshell do\n```\n```\nend\nend\nend\n",
                     false);
    expectEquivalent("comment text", "# a\n", "# b\n", false);
    expectEquivalent("comment count", "pkg a\n", "pkg a # c\n", false);
    expectEquivalent("comment trailing blanks", "# a  \n", "# a\n", true);
    expectEquivalent("comment trailing carriage return", "# a\t\r\t\n",
                     "# a\n", true);
    expectEquivalent("does not lex", "pkg a-b\n", "pkg a-b\n", false);
}

void runFormatTests(void)
{
    testCases();
    testEquivalence();
    testFixtures();
}
