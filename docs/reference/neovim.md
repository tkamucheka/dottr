# Neovim

This document describes the Neovim setup for dottr manifests. It has two
independent parts:

* The tree-sitter grammar in `utils/tree-sitter-dottr` provides highlighting,
  shell highlighting inside `shell` blocks, folds, indentation, and text
  objects.
* The language server `dottr-lsp` provides diagnostics, navigation, rename,
  hover, completion, and formatting (`docs/reference/lsp.md`).

The snippets below are written for `init.lua` and `ftplugin/`; they can be
moved into a plugin manager's configuration unchanged.

## Requirements

| Requirement | Used by |
|-------------|---------|
| Neovim 0.11 or later | `vim.lsp.config` and `vim.lsp.enable` |
| nvim-treesitter, `main` branch | Installing the grammar; indentation |
| The tree-sitter CLI and a C compiler | Building the grammar (`tree-sitter build`) |
| The `bash` and `zsh` tree-sitter parsers | Shell highlighting: `:TSInstall bash zsh` |
| `dottr-lsp` on `PATH` | The language server (`docs/reference/installation.md`) |
| A dottr checkout with its submodule | The grammar source: `git submodule update --init` |

## 1. Filetype

Manifests use the extension `.dotfile`. Map it to the filetype `dottr`, which
is also the name of the tree-sitter language:

```lua
vim.filetype.add({ extension = { dotfile = "dottr" } })
```

Settings for the filetype go in `ftplugin/dottr.lua`:

```lua
-- Comments, for gc and gcc.
vim.bo.commentstring = "# %s"

-- Tree-sitter highlighting, folds, and indentation (Section 2).
if pcall(vim.treesitter.start) then
  vim.wo[0][0].foldexpr = "v:lua.vim.treesitter.foldexpr()"
  vim.wo[0][0].foldmethod = "expr"
  vim.bo.indentexpr = "v:lua.require'nvim-treesitter'.indentexpr()"
end
```

`vim.treesitter.start` fails until the grammar is installed; the `pcall`
leaves the buffer without tree-sitter features in that case.

## 2. Tree-sitter Grammar

Register the grammar with nvim-treesitter in a `User TSUpdate` autocommand,
pointing at the checkout:

```lua
vim.api.nvim_create_autocmd("User", {
  pattern = "TSUpdate",
  callback = function()
    require("nvim-treesitter.parsers").dottr = {
      install_info = {
        path = "~/projects/dottr/utils/tree-sitter-dottr",
        queries = "queries",
      },
    }
  end,
})
```

Then install it, with the shell parsers:

```
:TSInstall dottr bash zsh
```

The grammar repository contains the generated parser, so no generation step
is needed. The queries are linked from the checkout rather than copied:
changes to them apply when a buffer is reopened. Changes to the grammar
itself require a rebuild with `:TSInstall! dottr`.

The grammar provides the following queries:

| Query | Effect |
|-------|--------|
| `highlights.scm` | Highlighting with Neovim's standard capture names. |
| `injections.scm` | The text of a `shell` block is highlighted as `zsh` for ` ```zsh `, and as `bash` otherwise. The indentation margin of the block is excluded, so heredoc terminators indented with the block are recognized. |
| `folds.scm` | Folds for units, `apply` constructs, `else` branches, and `shell` blocks. |
| `indents.scm` | Indentation for nvim-treesitter's `indentexpr`; text in `shell` blocks keeps the indentation of the previous line. |
| `textobjects.scm` | Text objects (Section 4). |
| `locals.scm`, `tags.scm` | Scopes, definitions, and references. |

## 3. Language Server

```lua
vim.lsp.config("dottr_lsp", {
  cmd = { "dottr-lsp" },
  filetypes = { "dottr" },
  root_markers = { "packages.dotfile", ".git" },
})
vim.lsp.enable("dottr_lsp")
```

Neovim's default LSP mappings then apply in manifests, among them `K`
(hover), `grn` (rename), `grr` (references), `gO` (document symbols), and
`CTRL-]` (definition). Completion is available through `vim.lsp.completion`
or a completion plugin.

### Format on Save

The server formats as `dottr-format` does. To format on every write, add an
`on_attach` function to the configuration above:

```lua
on_attach = function(client, bufnr)
  vim.api.nvim_create_autocmd("BufWritePre", {
    group = vim.api.nvim_create_augroup("dottr-format-" .. bufnr, { clear = true }),
    buffer = bufnr,
    callback = function()
      vim.lsp.buf.format({ bufnr = bufnr, id = client.id, timeout_ms = 1000 })
    end,
  })
end,
```

A manifest with a syntax error is written unformatted. Formatting through the
server is sufficient; a separate formatter source for `dottr-format` in
none-ls or conform.nvim would format the buffer a second time.

## 4. Text Objects

`textobjects.scm` uses the capture names of nvim-treesitter-textobjects:

| Capture | Outer | Inner |
|---------|-------|-------|
| `@function` | A unit | The statements of a block unit |
| `@block` | An `apply` construct or a `shell` block | Its body; for a `shell` block, the script |
| `@conditional` | An `apply_if` or `apply_unless` construct | The statements of a branch |
| `@assignment` | A `local` statement | The value (`@assignment.rhs` likewise; `@assignment.lhs` is the name) |
| `@parameter` | A unit name in `needs` | The same |
| `@statement`, `@comment`, `@string` | A statement, comment, or string | |

With mini.ai, map them to keys:

```lua
local ai = require("mini.ai")
local ts = ai.gen_spec.treesitter
ai.setup({
  custom_textobjects = {
    F = ts({ a = "@function.outer", i = "@function.inner" }),
    o = ts({
      a = { "@block.outer", "@conditional.outer" },
      i = { "@block.inner", "@conditional.inner" },
    }),
    ["="] = ts({ a = "@assignment.outer", i = "@assignment.inner" }),
  },
})
```

`vaF` then selects a unit, `cio` changes the script of a `shell` block, and
`ci=` changes the value of a `local`. The keys apply to every filetype with a
`textobjects` query.

## 5. Checking the Setup

* `:checkhealth vim.lsp` lists the attached `dottr_lsp` client.
* `:InspectTree` shows the syntax tree of the buffer.
* `:Inspect` on a word of a shell script lists `bash` or `zsh` captures,
  which confirms the injection.
