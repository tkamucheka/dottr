# Command Line

dottr consists of three programs:

| Program | Purpose |
|---------|---------|
| `dottr` | Validates a manifest, shows what applying it would do, and applies it. |
| `dottr-format` | Formats manifests ([Formatting](formatting.md)). |
| `dottr-lsp` | Language server for editors ([Language Server](lsp.md)). |

## 1. dottr

```
dottr check [-f FILE]
dottr plan  [-f FILE] [--package-manager NAME] [UNIT...]
dottr apply [-f FILE] [--package-manager NAME] [UNIT...]
dottr fmt   [-f FILE] [-w | --check]
dottr lsp
```

| Command | Effect |
|---------|--------|
| `check` | Validates the manifest and reports its errors. Nothing on the host is read or changed. |
| `plan` | Prints what `apply` would do, without changing the host. |
| `apply` | Applies the manifest: installs packages, creates directories, and runs shell scripts. |
| `fmt` | Formats the manifest, as `dottr-format` does ([Formatting](formatting.md)). |
| `lsp` | Runs the language server, as `dottr-lsp` does ([Language Server](lsp.md)). |

| Option | Meaning |
|--------|---------|
| `-f FILE` | The manifest to read; `-` reads standard input. Default: `packages.dotfile` in the current directory. |
| `--package-manager NAME` | `plan` and `apply` only: install packages with `NAME`, one of `pacman`, `apt`, `dnf`, `brew`, or `winget` (Section 1.3). Default: the package manager of the host. |
| `-w` | `fmt` only: rewrite the file instead of printing it. |
| `--check` | `fmt` only: print nothing, and exit with status 1 if the file is not formatted. |
| `-h`, `--help` | Print a summary of the usage. |
| `--` | Ends the options; the remaining arguments are unit names. |

### 1.1 Selecting Units

`plan` and `apply` process the manifest of the host's system: the units and
statements that apply on its package manager, or on the one named by
`--package-manager` ([The Manifest Language](language.md), Section 7.1).
They process every unit of that manifest, or, if unit names are given, those
units and the units they need, directly or indirectly:

```
dottr apply neovim tmux
```

Units are processed in dependency order (see
[The Manifest Language](language.md), Section 3). A name that the manifest
does not declare, or declares only for other systems, is an error, and
nothing is applied.

If no package manager is found, the operating system's systems are
intersected with the manifest's declaration. All remaining systems must
select the same statements; otherwise `--package-manager` must name one.
An empty intersection, or a detected or explicit manager outside the
declaration, is refused with exit status 2 before any actions are performed.
For example, `--package-manager dnf` selects `fedora`; the other mappings
are pacman to `arch`, apt to `debian`, brew to `macos`, and winget to `windows`.

### 1.2 Validation

Every command except `lsp` first validates the whole manifest, and stops at
the first lexical or syntax error or after reporting all semantic errors.
`check`, `plan`, and `apply` validate every declared system, or every
supported system in the absence of a declaration, so that a
manifest checked on one system is valid on the others; an error that occurs
on some systems only is followed by those systems, as in
`(on linux, macos)`.
Errors are written to standard error in the form

```
FILE:LINE:COLUMN: error: MESSAGE
```

for example

```
packages.dotfile:2:9: error: unit `b` is not declared; declare it at top level, for example `pkg b`
```

### 1.3 plan

`plan` prints, for each unit, its variables with their values, the package
commands it would run, its checks with their current results, and the scripts
of its shell blocks:

```
unit git
  install package git: sudo pacman -S --needed --noconfirm git

unit tmux
  local TPM_DIR = "/home/user/.config/tmux/plugins/tpm"
  needs git
  install package tmux: sudo pacman -S --needed --noconfirm tmux
  check directory TPM_DIR: /home/user/.config/tmux/plugins/tpm (currently false)
  apply_unless TPM_DIR
    shell (sh)
      git clone https://github.com/tmux-plugins/tpm "$TPM_DIR"
```

The commands are those of the package manager of the host
([The Manifest Language](language.md), Section 2.1). With
`--package-manager`, `plan` shows the commands of another package manager,
for example those of a macOS machine with `--package-manager brew`, or of a
Windows machine with `--package-manager winget`:

```
  install package git: winget install --exact --silent --accept-package-agreements --accept-source-agreements --id git
```

Checks are evaluated against the current state of the host, which they only
read. Since `plan` performs no actions, a check that follows an action in a
unit reports the state before that action; `apply` may observe a different
result.

### 1.4 apply

`apply` evaluates the selected units and reports each step, the branch taken
by `apply_if` and `apply_unless`, and the outcome of each unit:

```
==> unit demo
  -> check directory DIR: false
  -> apply_unless DIR: fact is false, evaluating body
  -> shell (sh), line 5
==> unit broken
  -> shell (sh), line 14
packages.dotfile:14:5: error: shell block exited with status 1
==> unit broken failed
==> unit after skipped: dependency broken was not satisfied
summary: 1 satisfied, 1 failed, 1 skipped
```

On Linux, package installations run the package manager with `sudo`, which
may ask for a password. `apply` accepts only a package manager of the host's
operating system in `--package-manager`. Shell scripts inherit the standard streams of dottr and may be interactive. A unit
stops at its first failing statement; units that need it are skipped, and the
remaining units are still applied.

### 1.5 Exit Status

| Status | `check` | `plan` | `apply` |
|--------|---------|--------|---------|
| 0 | The manifest is valid. | The plan was printed. | Every selected unit was satisfied. |
| 1 | | A value could not be computed, such as an unknown home directory, or no package manager was found. | A unit failed or was skipped. |
| 2 | Usage error, unreadable manifest, or invalid manifest. | As for `check`, an undeclared unit name, or an unknown package manager. | As for `plan`, or a package manager of another operating system. |

## 2. dottr-format

```
dottr-format [-w | --check] [FILE...]
```

Formats manifests; see [Formatting](formatting.md).

## 3. dottr-lsp

```
dottr-lsp
```

Runs the language server on standard input and output. It takes no
arguments and is started by an editor; see [Language Server](lsp.md) and
[Editors](editors.md).
