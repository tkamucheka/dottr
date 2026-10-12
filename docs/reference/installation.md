# Installation

dottr is built from source by its installation script. The script installs
the programs `dottr`, `dottr-format`, and `dottr-lsp` into
`~/.local/opt/dottr/bin` and links them from `~/.local/bin`, so no root
privileges are required.

## Requirements

* Linux or macOS. `dottr apply` installs packages with pacman, apt, or dnf
  on Linux, and with Homebrew on macOS.
* A C11 compiler: `$CC`, by default `cc`.
* git, or, for a repository on GitHub, curl or wget and tar.

`~/.local/bin` must be in `PATH`.

## Installing

```
curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh
```

Options follow `sh -s --`, for example to install a tagged release:

```
curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh -s -- --ref v1.0
```

| Option | Environment variable | Default | Meaning |
|--------|----------------------|---------|---------|
| `--repo URL` | `DOTTR_REPO` | `https://github.com/tkamucheka/dottr.git` | Repository to download. |
| `--ref REF` | `DOTTR_REF` | `main` | Branch, tag, or commit to build. |
| `--prefix DIR` | `DOTTR_PREFIX` | `~/.local/opt/dottr` | Installation directory; the programs are installed in `DIR/bin`. |
| `--bin-dir DIR` | `DOTTR_BIN_DIR` | `~/.local/bin` | Directory of the links to the programs. |
| `--no-link` | | | Do not create the links. |
| `--source DIR` | | | Build from a local checkout instead of downloading. |
| `-h`, `--help` | | | Print the options. |

`$CFLAGS` sets the compiler flags; the default is `-O2`.

To build from a clone of the repository:

```
git clone https://github.com/tkamucheka/dottr.git
cd dottr
./install.sh --source .
```

## Upgrading

Running the script again builds and installs the requested version over the
existing installation. All programs are built before any is installed, and
each program is replaced atomically, so a failed or interrupted upgrade
leaves the previous version in place and running programs are unaffected.

The script does not replace a file in the link directory that has the name
of a program and is not a symbolic link; it reports the file and stops
before building.

## Uninstalling

```
rm -rf ~/.local/opt/dottr
rm -f ~/.local/bin/dottr ~/.local/bin/dottr-format ~/.local/bin/dottr-lsp
```

With other `--prefix` or `--bin-dir` directories, remove those instead.
