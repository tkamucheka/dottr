# Installation

`install.sh` downloads the dottr source, builds it, and installs the binary
into `~/.local/opt/dottr/bin`, with a symbolic link in `~/.local/bin`:

```
curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh
```

Options follow `sh -s --`:

```
curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh -s -- --ref v1.0
```

| Option | Environment variable | Default | Meaning |
|--------|----------------------|---------|---------|
| `--repo URL` | `DOTTR_REPO` | `https://github.com/tkamucheka/dottr.git` | Repository to download. |
| `--ref REF` | `DOTTR_REF` | `main` | Branch, tag, or commit to build. |
| `--prefix DIR` | `DOTTR_PREFIX` | `~/.local/opt/dottr` | Installation directory; the binary is `DIR/bin/dottr`. |
| `--bin-dir DIR` | `DOTTR_BIN_DIR` | `~/.local/bin` | Directory of the `dottr` link. |
| `--no-link` | | | Do not create the link. |
| `--source DIR` | | | Build from a local checkout instead of downloading. |

## Requirements

* A C11 compiler: `$CC`, default `cc`. `$CFLAGS` default to `-O2`.
* git, or, for a GitHub https repository, curl or wget and tar.

## Behavior

* The script consists of one function called on its last line, so a
  partially downloaded script does nothing.
* Work is done in a temporary directory that is removed on exit.
* The installed binary is replaced by a rename: a running dottr is not
  affected, and an interrupted installation leaves the previous binary in
  place. Running the script again upgrades the installation.
* An existing `dottr` in the link directory that is not a symbolic link is
  not replaced; the script stops before building.

## Private Repository

While the repository is private, neither the URL above nor an https clone
works without credentials. Fetch the script with the GitHub CLI and clone
over SSH:

```
gh api repos/tkamucheka/dottr/contents/install.sh -H 'Accept: application/vnd.github.raw' |
    sh -s -- --repo git@github.com:tkamucheka/dottr.git
```

From a checkout, `./install.sh --source .` builds and installs the working
tree.

## Removal

```
rm -rf ~/.local/opt/dottr ~/.local/bin/dottr
```
