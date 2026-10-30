# Language Server

`dottr-lsp` is a Language Server Protocol server for dottr manifests, also
available as `dottr lsp`. It communicates over standard input and output and
takes no arguments. Editors start it for files of the `dottr` language; the
setup of supported editors is described in [Editors](editors.md).

## Features

| Feature | Behavior |
|---------|----------|
| Diagnostics | Lexical, syntax, and semantic errors, as reported by `dottr check`, updated on every change. Lexical and syntax errors stop at the first error; semantic errors are all reported, for the declared systems, with a system annotation when an error occurs on a proper subset. |
| Formatting | Formats the document as `dottr fmt` does. A document that does not parse is left unchanged. |
| Document symbols | One symbol per top-level package name or block unit, with symbols for locals and installed packages. |
| Workspace symbols | Units and locals of the open documents. |
| Go to definition | From a `needs` target to its unit; from a variable use to its `local`. |
| References | All uses of a unit or a variable. |
| Rename | Units and variables, with all their uses. A unit name that is not an identifier is written as a string. A variable name must be an identifier that is not a keyword, not an environment variable protected by the language, and not already bound in the unit. |
| Hover | Units (kind, line, and dependencies), package statements, variables (declaration, current value, and `check` statements), `@{dottr.env.home}` (with its value), the selectors of `on` blocks and `systems` declarations, and keywords. Units declared for some systems only list them. |
| Completion | Statement keywords valid in the enclosing block; unit names after `needs`, including continuation lines; predicate kinds after `check`; variables after `check KIND`, `create directory`, in link operands, and after `ignore`; contextual `tree` and `ignore` link modifiers; checked variables after `apply_if` and `apply_unless`; variables and `dottr.env.home` after `@{`; family selectors after `on` and `systems`, including continuation lines; `on` completion is restricted to families overlapping the declaration. `systems` is offered before the first declaration or unit. `unit` block snippets are offered to clients that support snippets. |

A unit or variable declared once per system, in `on` blocks, is one symbol
for references and rename; go to definition leads to the declaration for the
systems of the use.

A variable use is a `local` name in `check`, `create`, `apply_if`, or
`apply_unless`; `@{NAME}` in a string; or `$NAME` or `${NAME...}` in a shell
block of the unit after the `local` that binds it, since shell blocks receive
the unit's variables as environment variables. Shell text is not parsed:
uses in single quotes and escaped dollar signs (`\$NAME`) are excluded, and
a shell variable assigned in the script with the name of a local is treated
as a use of the local.

Completion remains available while the document contains syntax errors.

## Positions

The server uses UTF-8 positions if the client offers them in
`general.positionEncodings`, and UTF-16 otherwise.
