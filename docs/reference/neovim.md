# Neovim

Neovim support for dottr manifests has two parts:

* The tree-sitter grammar,
  [tree-sitter-dottr](https://github.com/tkamucheka/tree-sitter-dottr), for
  highlighting, highlighting of shell scripts, folds, indentation, and text
  objects.
* The language server, `dottr-lsp`, for diagnostics, navigation, rename,
  hover, completion, and formatting (`docs/reference/lsp.md`).

## Requirements

* Neovim 0.12 or later
* [nvim-treesitter](https://github.com/nvim-treesitter/nvim-treesitter), `main`
  branch, with its requirements (`curl`, `tar`, the tree-sitter CLI, and a C
  compiler)
* `dottr-lsp` on `PATH` (`docs/reference/installation.md`)

## 1. Filetype

Manifests use the extension `.dotfile`:

```lua
vim.filetype.add({ extension = { dotfile = "dottr" } })
```

Set the comment string in `ftplugin/dottr.lua`:

```lua
vim.bo.commentstring = "# %s"
```

## 2. Tree-sitter Grammar

Install the grammar with
[gardener.nvim](https://github.com/tkamucheka/gardener.nvim), here with
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
      },
    },
    manage_folds = true,
    manage_indent = true,
  },
}
```

gardener installs the parser when a manifest is first opened and starts
highlighting, folds, and indentation. The `bash` and `zsh` parsers highlight
the scripts of `shell` blocks.

Installation without gardener and the text objects of the grammar are
described in the
[tree-sitter-dottr README](https://github.com/tkamucheka/tree-sitter-dottr#readme).

## 3. Language Server

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

A manifest with a syntax error is written unformatted. `dottr-format` can
serve instead as the formatter of a plugin such as conform.nvim; configure
only one of the two, or each write is formatted twice.

## 4. Checking the Setup

* `:checkhealth vim.lsp` lists the attached `dottr` client.
* `:InspectTree` shows the syntax tree of the buffer.
* `:Inspect` on a word of a shell script lists `bash` or `zsh` captures,
  which confirms that shell scripts are highlighted.
