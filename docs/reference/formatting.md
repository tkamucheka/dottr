# dottr fmt

`dottr fmt` rewrites a manifest in a canonical layout. It changes only
whitespace: the token sequence of the manifest, the contents of its raw
blocks, and the text of its comments are preserved.

## 1. Command-Line Interface

```
dottr fmt [-f FILE] [-w | --check]
```

| Invocation | Effect |
|------------|--------|
| `dottr fmt` | Writes the formatted manifest to standard output. |
| `dottr fmt -w` | Rewrites the file in place if its formatting changes. |
| `dottr fmt --check` | Writes nothing; exits with status 1 and a message if the file is not formatted. |

`-f -` reads the manifest from standard input, for use by editors; it cannot
be combined with `-w`. The exit status is 0 on success, 1 if `--check` finds
an unformatted file, and 2 for a usage error, an unreadable file, or a
lexical or syntax error. Semantic errors do not prevent formatting.

## 2. Layout Rules

1. **Indentation.** Two spaces per level. The body of a unit, of an `apply`,
   `apply_if`, or `apply_unless`, of an `else` branch, and of a `shell` block
   is one level deeper than the line that opens it. `else` and `end` are at
   the level of the line that opens their construct.
2. **Spacing within a line.** Tokens are separated by one space, except that
   no space precedes `,`. String literals are reproduced exactly.
3. **Comments.** A comment on a line of its own is indented to the level of
   the surrounding statements. A comment following tokens is separated from
   them by one space. Trailing whitespace, including carriage returns, is
   removed from comments.
4. **Blank lines.** A run of blank lines is reduced to one. Blank lines are
   removed at the start and end of the file, after a line that opens a block
   (`... do`, `else`), and before a line that closes one (`end`, `else`).
5. **Raw blocks.** The fences are indented to the body level of the `shell`
   block. Each line of the block is re-indented by replacing the margin of the
   original block with the
   new margin; indentation relative to the margin is preserved. Blank and
   whitespace-only lines become empty lines. Other text, including trailing
   whitespace, is preserved, since it is shell text.
6. **Line endings.** Lines end with a line feed; the file ends with exactly
   one.

Rules 4 and 5 rely on properties of the language: blank lines are not
significant outside raw blocks, and whitespace-only lines and the margin of a
raw block are not part of its contents.
