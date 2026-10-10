# dottr

dottr sets up a machine from a manifest, `packages.dotfile`, written in a
small language. Each unit either installs a package with pacman or runs shell
scripts, gated on checks of the host, and may depend on other units.

````
pkg git

pkg tmux do
  local TPM_DIR = "@{dottr.env.home}/.config/tmux/plugins/tpm"
  needs git
  pkg tmux
  check directory TPM_DIR
  apply_unless TPM_DIR do
    shell do
      ```
      git clone https://github.com/tmux-plugins/tpm "$TPM_DIR"
      ```
    end
  end
end
````

## Install

```
curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh
```

This builds `dottr`, `dottr-format`, and `dottr-lsp` and installs them into
`~/.local/opt/dottr`, linked from `~/.local/bin`. It requires a C compiler,
and git or curl. Options and the installation from a private repository are
described in [Installation](docs/reference/installation.md).

## Use

```
dottr check              # validate packages.dotfile
dottr plan [UNIT...]     # print what apply would do
dottr apply [UNIT...]    # apply the units, and the units they need
dottr-format -w FILE...  # format manifests
```

`-f FILE` selects another manifest. `apply` installs packages with
`sudo pacman`.

## Documentation

* [Installation](docs/reference/installation.md)
* [Grammar](docs/reference/grammar.md): the manifest language
* [Formatting](docs/reference/formatting.md): `dottr-format` and `dottr fmt`
* [Language server](docs/reference/lsp.md): `dottr-lsp`
* [Neovim](docs/reference/neovim.md): the tree-sitter grammar and the
  language server

The tree-sitter grammar is in the submodule `utils/tree-sitter-dottr`; clone
with `--recurse-submodules`.
