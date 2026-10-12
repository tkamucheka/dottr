# Editors

Editor support for dottr manifests has two parts:

* Highlighting, including the scripts of `shell` blocks: a TextMate grammar
  for VS Code, and a tree-sitter grammar,
  [tree-sitter-dottr](https://github.com/tkamucheka/tree-sitter-dottr), for
  Neovim and other tree-sitter editors.
* The language server, `dottr-lsp`, for diagnostics, navigation, rename,
  hover, completion, and formatting ([Language Server](lsp.md)). It is
  installed with dottr ([Installation](installation.md)) and works with any
  editor that supports the Language Server Protocol.

Manifests use the extension `.dotfile`.

## 1. Visual Studio Code

The extension [vscode-dottr](https://github.com/tkamucheka/vscode-dottr)
provides highlighting and starts `dottr-lsp`. It requires VS Code 1.91 or
later and Node.js to build:

```
git clone https://github.com/tkamucheka/vscode-dottr.git
cd vscode-dottr
npm ci
npm run package
code --install-extension vscode-dottr-0.1.0.vsix
```

| Setting | Default | Meaning |
|---------|---------|---------|
| `dottr.server.path` | `""` | Path of `dottr-lsp`; if empty, `dottr-lsp` is found in `PATH`. |
| `dottr.trace.server` | `"off"` | Logs the communication with the server: `off`, `messages`, or `verbose`. |

To format manifests on save, add to `settings.json`:

```json
"[dottr]": {
  "editor.formatOnSave": true
}
```

## 2. Neovim

Requirements:

* Neovim 0.12 or later
* [nvim-treesitter](https://github.com/nvim-treesitter/nvim-treesitter), `main`
  branch, with its requirements (`curl`, `tar`, the tree-sitter CLI, and a C
  compiler)

### 2.1 Tree-sitter Grammar

With [gardener.nvim](https://github.com/tkamucheka/gardener.nvim) and
[lazy.nvim](https://github.com/folke/lazy.nvim):

```lua
{
  "tkamucheka/gardener.nvim",
  dependencies = { "nvim-treesitter/nvim-treesitter" },
  lazy = false,
  opts = {
    ensure_installed = { "bash", "zsh" },
    custom_parsers = {
      dottr = {
        install_info = {
          url = "https://github.com/tkamucheka/tree-sitter-dottr",
          queries = "queries",
        },
        filetypes = { "dottr" },
        extensions = { "dotfile" },
      },
    },
    manage_folds = true,
    manage_indent = true,
  },
}
```

gardener detects files ending in `.dotfile` as the filetype `dottr`,
installs the parser when a manifest is first opened, and starts
highlighting, folds, and indentation. The `bash` and `zsh` parsers highlight
the scripts of `shell` blocks. Installation without gardener and the text
objects of the grammar are described in the
[tree-sitter-dottr README](https://github.com/tkamucheka/tree-sitter-dottr#readme).

### 2.2 Comment String

Set the comment string in `ftplugin/dottr.lua`:

```lua
vim.bo.commentstring = "# %s"
```

### 2.3 Language Server

```lua
vim.lsp.config("dottr", {
  cmd = { "dottr-lsp" },
  filetypes = { "dottr" },
  root_markers = { "packages.dotfile", ".git" },
})
vim.lsp.enable("dottr")
```

Neovim's default LSP mappings then apply in manifests, among them `K`
(hover), `grn` (rename), `grr` (references), `gO` (document symbols), and
`CTRL-]` (definition).

To format on every write, add to the configuration:

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

A manifest with a syntax error is written unformatted. Alternatively,
`dottr-format` can serve as the formatter of a plugin such as conform.nvim;
configure only one of the two.

### 2.4 Checking the Setup

* `:checkhealth vim.lsp` lists the attached `dottr` client.
* `:InspectTree` shows the syntax tree of the buffer.
* `:Inspect` on a word of a shell script lists `bash` or `zsh` captures,
  which confirms that shell scripts are highlighted.

## 3. Other Editors

Configure the editor's language client to start `dottr-lsp`, without
arguments, for files with the extension `.dotfile`. Editors that build
tree-sitter grammars from source can use tree-sitter-dottr, which includes
the generated parser and its queries.
