#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/lexer.h"
#include "platform.h"
#include "test.h"

typedef struct {
    Arena arena;
    TokenList tokens;
    bool ok;
    char *diagnostics;
    size_t diagnosticsLength;
} LexResult;

static void lexBytes(const char *source, size_t length, LexResult *result)
{
    arenaInit(&result->arena);
    result->diagnostics = NULL;
    result->diagnosticsLength = 0;
    FILE *stream = testOutputStream();
    Diag diag;
    diagInit(&diag, "t.dotfile", stream);
    result->ok =
        lexSource(&result->arena, source, length, &result->tokens, &diag);
    result->diagnostics =
        testCloseOutput(stream, &result->diagnosticsLength);
}

static void lexText(const char *source, LexResult *result)
{
    lexBytes(source, strlen(source), result);
}

static void lexFree(LexResult *result)
{
    arenaFree(&result->arena);
    free(result->diagnostics);
}

/*
 * Renders a template as T(text) and R(path line:column) parts, so that tests
 * can assert on structure and reference positions with one comparison.
 */
static char *describeTemplate(const Template *template)
{
    StrBuf buf;
    strBufInit(&buf);
    for (size_t i = 0; i < template->count; i++) {
        const TemplatePart *part = &template->parts[i];
        if (part->kind == TEMPLATE_TEXT) {
            strBufAppendStr(&buf, "T(");
            strBufAppend(&buf, part->text, part->length);
            strBufAppendStr(&buf, ")");
        } else {
            char position[64];
            snprintf(position, sizeof position, " %d:%d)", part->line,
                     part->column);
            strBufAppendStr(&buf, "R(");
            strBufAppend(&buf, part->text, part->length);
            strBufAppendStr(&buf, position);
        }
    }
    return buf.data;
}

static void expectKinds(const char *name, const LexResult *result,
                        const TokenKind *kinds, size_t count)
{
    testCount++;
    if (!result->ok) {
        TEST_FAIL("lexer %s: unexpected error: %s", name, result->diagnostics);
        return;
    }
    if (result->tokens.count != count) {
        TEST_FAIL("lexer %s: expected %zu tokens, got %zu", name, count,
                  result->tokens.count);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (result->tokens.items[i].kind != kinds[i]) {
            TEST_FAIL("lexer %s: token %zu: expected %s, got %s", name, i,
                      tokenKindName(kinds[i]),
                      tokenKindName(result->tokens.items[i].kind));
            return;
        }
    }
}

#define EXPECT_KINDS(name, result, ...)                                       \
    expectKinds(name, result, (const TokenKind[]){__VA_ARGS__},               \
                sizeof((const TokenKind[]){__VA_ARGS__}) / sizeof(TokenKind))

static void expectTemplate(const char *name, const LexResult *result,
                           size_t index, const char *expected)
{
    testCount++;
    if (!result->ok || index >= result->tokens.count) {
        TEST_FAIL("lexer %s: no token %zu (%s)", name, index,
                  result->ok ? "too few tokens" : result->diagnostics);
        return;
    }
    char *actual = describeTemplate(&result->tokens.items[index].value);
    if (strcmp(actual, expected) != 0) {
        TEST_FAIL("lexer %s: template\n  expected: %s\n  actual:   %s", name,
                  expected, actual);
    }
    free(actual);
}

static void expectError(const char *name, const char *source,
                        const char *expected)
{
    LexResult result;
    lexText(source, &result);
    testCount++;
    if (result.ok) {
        TEST_FAIL("lexer %s: expected an error", name);
    } else if (strcmp(result.diagnostics, expected) != 0) {
        TEST_FAIL("lexer %s: diagnostic\n  expected: %s  actual:   %s", name,
                  expected, result.diagnostics);
    }
    lexFree(&result);
}

static void testStatements(void)
{
    LexResult r;
    lexText("unit \"tmux\" do\n"
            "  local A = \"v\"\n"
            "  needs git, npm\n"
            "  check directory A\n"
            "end\n",
            &r);
    EXPECT_KINDS("statements", &r, TOK_UNIT, TOK_STRING, TOK_DO, TOK_NEWLINE,
                 TOK_LOCAL, TOK_IDENT, TOK_EQUALS, TOK_STRING, TOK_NEWLINE,
                 TOK_NEEDS, TOK_IDENT, TOK_COMMA, TOK_IDENT, TOK_NEWLINE,
                 TOK_CHECK, TOK_DIRECTORY, TOK_IDENT, TOK_NEWLINE, TOK_END,
                 TOK_NEWLINE, TOK_EOF);
    if (r.ok && r.tokens.count > 5) {
        testCount++;
        const Token *a = &r.tokens.items[5];
        if (a->line != 2 || a->column != 9 || a->length != 1) {
            TEST_FAIL("lexer statements: identifier at %d:%d", a->line,
                      a->column);
        }
    }
    lexFree(&r);

    lexText("apply apply_if apply_unless else shell command file create",
            &r);
    EXPECT_KINDS("keywords", &r, TOK_APPLY, TOK_APPLY_IF, TOK_APPLY_UNLESS,
                 TOK_ELSE, TOK_SHELL, TOK_COMMAND, TOK_FILE, TOK_CREATE,
                 TOK_NEWLINE, TOK_EOF);
    lexFree(&r);

    lexText("link tree SOURCE TARGET\n", &r);
    EXPECT_KINDS("symlink with contextual tree", &r, TOK_SYMLINK, TOK_IDENT,
                 TOK_IDENT, TOK_IDENT, TOK_NEWLINE, TOK_EOF);
    lexFree(&r);

    lexText("applyx _a1 Pkg", &r);
    EXPECT_KINDS("identifiers", &r, TOK_IDENT, TOK_IDENT, TOK_IDENT,
                 TOK_NEWLINE, TOK_EOF);
    lexFree(&r);
}

static void testNewlines(void)
{
    LexResult r;
    lexText("systems arch,\n # other\n debian\n", &r);
    EXPECT_KINDS("systems declaration", &r, TOK_SYSTEMS, TOK_IDENT,
                 TOK_COMMA, TOK_IDENT, TOK_NEWLINE, TOK_EOF);
    lexFree(&r);
    lexText("pkg a, # trailing\r\n\n# between\n b,\n c\n", &r);
    EXPECT_KINDS("comma continuation", &r, TOK_PKG, TOK_IDENT, TOK_COMMA,
                 TOK_IDENT, TOK_COMMA, TOK_IDENT, TOK_NEWLINE, TOK_EOF);
    lexFree(&r);
    lexText("\n\n# comment\n   \npkg a # trailing\n\n# more\n\n", &r);
    EXPECT_KINDS("blank and comment lines", &r, TOK_PKG, TOK_IDENT,
                 TOK_NEWLINE, TOK_EOF);
    lexFree(&r);

    lexText("pkg a", &r);
    EXPECT_KINDS("no final newline", &r, TOK_PKG, TOK_IDENT, TOK_NEWLINE,
                 TOK_EOF);
    lexFree(&r);

    lexText("", &r);
    EXPECT_KINDS("empty source", &r, TOK_EOF);
    lexFree(&r);

    lexText("pkg a\r\npkg b\r\n", &r);
    EXPECT_KINDS("carriage returns", &r, TOK_PKG, TOK_IDENT, TOK_NEWLINE,
                 TOK_PKG, TOK_IDENT, TOK_NEWLINE, TOK_EOF);
    lexFree(&r);
}

static void testStrings(void)
{
    LexResult r;
    lexText("\"a\\\"b\\\\c\"", &r);
    expectTemplate("escapes", &r, 0, "T(a\"b\\c)");
    lexFree(&r);

    lexText("\"x@{A}y@{dottr.env.home}\"", &r);
    expectTemplate("references in string", &r, 0,
                   "T(x)R(A 1:3)T(y)R(dottr.env.home 1:8)");
    lexFree(&r);

    lexText("\"user@host @ {x} #notcomment\"", &r);
    expectTemplate("literal at sign", &r, 0,
                   "T(user@host @ {x} #notcomment)");
    lexFree(&r);

    lexText("\"\"", &r);
    expectTemplate("empty string", &r, 0, "");
    lexFree(&r);
}

static void testRawBlocks(void)
{
    LexResult r;
    lexText("    shell do\n"
            "    ```\n"
            "    a\n"
            "      b\n"
            "\n"
            "   \n"
            "    c   \n"
            "      ```\n"
            "    end\n",
            &r);
    EXPECT_KINDS("raw block", &r, TOK_SHELL, TOK_DO, TOK_NEWLINE,
                 TOK_RAW_BLOCK, TOK_NEWLINE, TOK_END, TOK_NEWLINE, TOK_EOF);
    expectTemplate("raw block margin", &r, 3, "T(a\n  b\n\n\nc   \n)");
    if (r.ok && r.tokens.count > 3) {
        testCount++;
        const Token *raw = &r.tokens.items[3];
        if (raw->line != 2 || raw->column != 5 ||
            raw->dialect != SHELL_DEFAULT) {
            TEST_FAIL("lexer raw block: position %d:%d dialect %d", raw->line,
                      raw->column, (int)raw->dialect);
        }
        const Token *end = &r.tokens.items[5];
        if (end->line != 9) {
            TEST_FAIL("lexer raw block: `end` on line %d, expected 9",
                      end->line);
        }
    }
    lexFree(&r);

    lexText("```\n"
            "if true; then\n"
            "else\n"
            "end\n"
            "# comment\n"
            "```sh\n"
            " ``` x\n"
            "fi\n"
            "```\n",
            &r);
    expectTemplate("raw block keeps keywords and fence-like lines", &r, 0,
                   "T(if true; then\nelse\nend\n# comment\n```sh\n ``` x\n"
                   "fi\n)");
    lexFree(&r);

    lexText("\t```bash\n\tcat <<EOF\n\t\tkey: @{V}\n\tEOF\n\t```\n", &r);
    expectTemplate("raw block tabs", &r, 0,
                   "T(cat <<EOF\n\tkey: @{V}\nEOF\n)");
    if (r.ok) {
        testCount++;
        if (r.tokens.items[0].dialect != SHELL_BASH) {
            TEST_FAIL("lexer raw block: expected bash dialect");
        }
    }
    lexFree(&r);

    lexText("```\ngit reset @{u} HEAD@{1} @{-1} @{\n\"$DIR\"\n```\n", &r);
    expectTemplate("raw block is not interpolated", &r, 0,
                   "T(git reset @{u} HEAD@{1} @{-1} @{\n\"$DIR\"\n)");
    lexFree(&r);

    lexText("```zsh\n```\n```sh\r\necho hi\r\n```\r\n", &r);
    EXPECT_KINDS("adjacent raw blocks", &r, TOK_RAW_BLOCK, TOK_NEWLINE,
                 TOK_RAW_BLOCK, TOK_NEWLINE, TOK_EOF);
    expectTemplate("empty raw block", &r, 0, "");
    expectTemplate("raw block carriage returns", &r, 2, "T(echo hi\n)");
    if (r.ok) {
        testCount++;
        if (r.tokens.items[0].dialect != SHELL_ZSH ||
            r.tokens.items[2].dialect != SHELL_SH) {
            TEST_FAIL("lexer raw block: expected zsh and sh dialects");
        }
    }
    lexFree(&r);

    lexText("```pwsh\nWrite-Output $env:A\n```\n```powershell\n```\n", &r);
    expectTemplate("powershell raw block", &r, 0, "T(Write-Output $env:A\n)");
    if (r.ok) {
        testCount++;
        if (r.tokens.items[0].dialect != SHELL_PWSH ||
            r.tokens.items[2].dialect != SHELL_POWERSHELL) {
            TEST_FAIL("lexer raw block: expected pwsh and powershell "
                      "dialects");
        }
    }
    lexFree(&r);

    lexText("```\ngit rev-parse @{u}\n```", &r);
    expectTemplate("raw block closed at end of input", &r, 0,
                   "T(git rev-parse @{u}\n)");
    lexFree(&r);
}

static void testErrors(void)
{
    expectError("unterminated string", "local A = \"abc\n",
                "t.dotfile:1:11: error: unterminated string literal\n");
    expectError("unknown escape", "local A = \"a\\n\"\n",
                "t.dotfile:1:13: error: unknown escape sequence; only \\\" and "
                "\\\\ are supported\n");
    expectError("malformed reference in string", "local A = \"@{1x}\"\n",
                "t.dotfile:1:12: error: malformed interpolation reference; "
                "expected @{NAME} or @{NAME.NAME...}\n");
    expectError("unclosed reference in string", "local A = \"@{A\"\n",
                "t.dotfile:1:12: error: malformed interpolation reference; "
                "expected @{NAME} or @{NAME.NAME...}\n");
    expectError("unterminated raw block", "pkg a\n  ```\n  echo\n",
                "t.dotfile:2:3: error: unterminated raw block; expected a "
                "closing ``` line\n");
    expectError("unknown info string", "```python\n```\n",
                "t.dotfile:1:4: error: unknown info string 'python'; expected "
                "sh, bash, zsh, pwsh, or powershell\n");
    expectError("info string case", "```PowerShell\n```\n",
                "t.dotfile:1:4: error: unknown info string 'PowerShell'; "
                "expected sh, bash, zsh, pwsh, or powershell\n");
    expectError("info string after space", "``` sh\n```\n",
                "t.dotfile:1:4: error: the info string must immediately follow "
                "the fence\n");
    expectError("long fence", "  ````\n  ````\n",
                "t.dotfile:1:3: error: fences longer than three backticks are not "
                "supported\n");
    expectError("under-indented line", "    ```\n    a\n  b\n    ```\n",
                "t.dotfile:3:1: error: line does not begin with the indentation "
                "of the opening fence on line 1 (is the closing ``` "
                "missing?)\n");
    expectError("mixed indentation", "\t```\n    a\n\t```\n",
                "t.dotfile:2:1: error: line does not begin with the indentation "
                "of the opening fence on line 1 (is the closing ``` "
                "missing?)\n");
    expectError("hyphenated unit name", "  needs git, tree-sitter-cli\n",
                "t.dotfile:1:18: error: unexpected character '-'; a unit name "
                "containing characters other than letters, digits, and '_' "
                "must be written as a string, for example "
                "\"tree-sitter-cli\"\n");
    expectError("keyword-prefixed unit name", "pkg file-roller\n",
                "t.dotfile:1:9: error: unexpected character '-'; a unit name "
                "containing characters other than letters, digits, and '_' "
                "must be written as a string, for example \"file-roller\"\n");
    expectError("inline fence", "shell do ```\n",
                "t.dotfile:1:10: error: unexpected character '`'; a fence must "
                "be on a line of its own\n");
    expectError("unexpected character", "local A = 'x'\n",
                "t.dotfile:1:11: error: unexpected character '''\n");

    LexResult r;
    lexBytes("pkg a\0b\n", 8, &r);
    testCount++;
    if (r.ok || strcmp(r.diagnostics,
                       "t.dotfile:1:6: error: unexpected byte 0x00\n") != 0) {
        TEST_FAIL("lexer NUL byte: got %s",
                  r.ok ? "no error" : r.diagnostics);
    }
    lexFree(&r);
}

void runLexerTests(void)
{
    testStatements();
    testNewlines();
    testStrings();
    testRawBlocks();
    testErrors();
}
