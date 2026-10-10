# Formatting

`dottr-format` and `dottr fmt` rewrite a manifest in a canonical layout.
Formatting changes only whitespace: the statements of the manifest, the
scripts of its shell blocks, and the text of its comments are preserved.

## 1. Command-Line Interface

The formatter is available as a program of its own and as a subcommand of
`dottr`. Both format identically.

```
dottr-format [-w | --check] [FILE...]
```

| Invocation | Effect |
|------------|--------|
| `dottr-format FILE...` | Writes each formatted manifest to standard output. |
| `dottr-format -w FILE...` | Rewrites each file in place if its formatting changes. |
| `dottr-format --check FILE...` | Writes nothing; reports each file that is not formatted. |

Without FILE, or with FILE `-`, `dottr-format` reads standard input and
writes standard output, for use by editors; `-w` requires files. Options
precede the files. The exit status is the highest of the files: 0 on
success, 1 if `--check` finds an unformatted file, and 2 for a usage error,
an unreadable file, or a syntax error.

```
dottr fmt [-f FILE] [-w | --check]
```

| Invocation | Effect |
|------------|--------|
| `dottr fmt` | Writes the formatted manifest to standard output. |
| `dottr fmt -w` | Rewrites the file in place if its formatting changes. |
| `dottr fmt --check` | Writes nothing; exits with status 1 and a message if the file is not formatted. |

`dottr fmt` formats one manifest, `packages.dotfile` unless `-f` names
another; `-f -` reads standard input and cannot be combined with `-w`. Its
exit status is as for `dottr-format`. Semantic errors prevent formatting in
neither form.

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
5. **Shell blocks.** The fences are indented to the body level of the
   `shell` block. Each line of the script is re-indented by replacing the
   indentation of the original opening fence with that of the new one;
   indentation relative to the fence is preserved
   ([The Manifest Language](language.md), Section 6.1). Blank and
   whitespace-only lines become empty lines. Other text, including trailing
   whitespace, is preserved, since it is shell text.
6. **Line endings.** Lines end with a line feed; the file ends with exactly
   one.

Rules 4 and 5 do not change the meaning of a manifest: blank lines are not
significant outside shell blocks, and the indentation of the fence and
whitespace-only lines are not part of a script.
