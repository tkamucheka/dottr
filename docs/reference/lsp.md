# dottr-lsp and dottr lsp

`dottr-lsp` is a Language Server Protocol server for dottr manifests, also
available as the subcommand `dottr lsp`. Both run the same server, which
communicates over standard input and output and takes no arguments.

## Features

| Feature | Behavior |
|---------|----------|
| Diagnostics | Lexical, syntax, and semantic errors, as reported by `dottr check`, updated on every change. Lexical and syntax errors stop at the first error; semantic errors are all reported. |
| Formatting | Formats the document as `dottr fmt` does. A document that does not parse is left unchanged. |
| Document symbols | Units, with their locals and child packages. |
| Workspace symbols | Units and locals of the open documents. |
| Go to definition | From a `needs` target to its unit; from a variable use to its `local`. |
| References | All uses of a unit or a variable. |
| Rename | Units and variables, with all their uses. A unit name that is not an identifier is written as a string. A variable name must be an identifier that is not a keyword, not an environment variable protected by the language, and not already bound in the unit. |
| Hover | Units (kind, line, and dependencies), child packages, variables (declaration, current value, and `check` statements), `@{dottr.env.home}` (with its value), and keywords. |
| Completion | Statement keywords valid in the enclosing block; unit names after `needs`; predicate kinds after `check`; variables after `check KIND` and `create directory`; checked variables after `apply_if` and `apply_unless`; variables and `dottr.env.home` after `@{`. Block snippets are offered to clients that support snippets. |

A variable use is a `local` name in `check`, `create`, `apply_if`, or
`apply_unless`; `@{NAME}` in a string; or `$NAME` or `${NAME...}` in a shell
block of the unit after the `local` that binds it, since shell blocks receive
the unit's variables as environment variables. Shell text is not parsed:
uses in single quotes and escaped dollar signs (`\$NAME`) are excluded, and
a shell variable assigned in the script with the name of a local is treated
as a use of the local.

Completion uses a line-based scan of the document rather than the syntax
tree, so it works while the document has syntax errors.

## Positions

The server uses UTF-8 positions if the client offers them in
`general.positionEncodings`, and UTF-16 otherwise.

## Neovim

With Neovim 0.11 or later and the `dottr` filetype for `*.dotfile`:

```lua
vim.filetype.add({ extension = { dotfile = "dottr" } })

vim.lsp.config("dottr", {
  cmd = { "dottr-lsp" },
  filetypes = { "dottr" },
  root_markers = { "packages.dotfile", ".git" },
})
vim.lsp.enable("dottr")
```
