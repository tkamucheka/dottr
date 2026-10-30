# The Manifest Language

A dottr manifest describes the software and configuration of a machine as a
set of *units*. dottr evaluates the units in dependency order: it installs
packages with the system package manager, observes the host with checks, and
runs shell scripts gated on those checks. This document describes the
language; its formal grammar is given in [Grammar](grammar.md), and the
commands that read a manifest in [Command Line](command-line.md).

## 1. Files

A manifest is a text file with the extension `.dotfile`.
`dottr` reads `packages.dotfile` in the current directory unless `-f` names
another file.

The language is line-oriented. A comma continues a `pkg`, `needs`, `systems`, or `on`
list onto the next line; blank and comment-only lines may intervene.
Otherwise a line end terminates the statement. `#` begins a comment that extends to the end of the line,
except inside a string literal or a shell script.

## 2. Units

A unit is declared by a top-level package installation or a `unit` block.

**Package units.** `pkg NAME` installs the package `NAME` with the system
package manager. A comma-separated list declares one unit per package,
equivalent to separate declarations:

```
pkg git, curl, "tree-sitter-cli"
```

A name that is not an identifier, such as one containing `-`, is written as a
string; such a string is not empty and contains no `@{...}` reference.
`pkg tmux` and `pkg "tmux"` declare the same unit. Identifiers consist of
ASCII letters, digits, and `_`, and do not begin with a digit.

**Block units.** `unit NAME do ... end` is satisfied by evaluating the
statements of its body in order:

```
unit tmux do
  needs git
  pkg tmux
  local TPM_DIR = "@{dottr.env.home}/.config/tmux/plugins/tpm"
  check directory TPM_DIR
  apply_unless TPM_DIR do
    shell do
      ```
      git clone https://github.com/tmux-plugins/tpm "$TPM_DIR"
      ```
    end
  end
end
```

A unit is *satisfied* if every statement of its body completes without error.
Unit names are unique within a manifest, and units are declared only at the
top level of the manifest; a block unit cannot contain another block unit.
A block unit must contain at least one statement on each system on which it
is declared. A `needs` statement alone is a valid grouping unit. Package
units always have their installation statement.

The statements of a block unit are:

| Statement | Effect |
|-----------|--------|
| `needs A, B` | Requires the units `A` and `B` (Section 3). |
| `pkg A, B` | Installs the packages `A` and `B` (Section 2.1). |
| `local NAME = "..."` | Binds a variable (Section 4). |
| `check KIND NAME` | Records a fact about the host (Section 5). |
| `create directory NAME` | Creates a directory (Section 5.2). |
| `link SOURCE TARGET` | Creates a symbolic link (Section 5.3). |
| `link tree SOURCE TARGET` | Links a directory's files recursively (Section 5.3). |
| `link tree SOURCE TARGET ignore RULES` | Links a tree using an ignore file (Section 5.3). |
| `apply do ... end` | Runs actions (Section 6). |
| `apply_if NAME do ... [else ...] end` | Runs actions if a fact is true (Section 6). |
| `apply_unless NAME do ... end` | Runs actions if a fact is false (Section 6). |
| `on SYSTEM, ... do ... end` | Selects statements by system (Section 7). |

### 2.1 Packages

dottr installs packages with the package manager of the host:

| Package manager | Systems | Command |
|-----------------|---------|---------|
| `pacman` | Arch Linux and derivatives | `sudo pacman -S --needed --noconfirm NAME` |
| `apt` | Debian, Ubuntu, and derivatives | `sudo apt-get install -y NAME` |
| `dnf` | Fedora, RHEL, and derivatives | `sudo dnf install -y NAME` |
| `brew` | macOS | `brew install NAME` |

On Linux, dottr uses the first of `pacman`, `apt-get`, and `dnf` found in
`PATH`; on macOS, Homebrew. `sudo` is omitted when dottr runs as root.
Homebrew does not install packages as root, so on macOS dottr runs as a
regular user. The option `--package-manager` selects another package manager
([Command Line](command-line.md)).

Each command treats a package that is already installed as success, so
applying a package unit again changes nothing. dottr does not refresh the
package index, for example with `apt-get update`; a manifest that requires it
does so in a shell block.

`NAME` is the name of the package for the package manager, and names differ
between package managers, for example `fd` with pacman and `fd-find` with
apt. `on` blocks select the name for each system (Section 7).

Inside a block unit, `pkg A, B` installs the packages at that point of the
body without declaring units. Identifiers and quoted names may be mixed;
repeating a name within one list is a syntax error. At top level, a name
colliding with any other unit is a semantic error. `needs` refers only to
declared top-level units.

pacman, apt, dnf, and brew receive all names of a statement in one command.
A failed batch reports the list and its exit status. On Windows, winget
queries and installs each package separately because `--id` accepts one ID.
`pkg NAME do` is rejected; block units use `unit NAME do`.

## 3. Dependencies

`needs` names units that must be satisfied before the declaring unit is
evaluated:

```
unit neovim do
  needs git, npm
  ...
end
```

Every name in `needs` must be declared as a top-level unit, and the
dependencies must not form a cycle. dottr evaluates units in an order that
places every unit after the units it needs; units that do not depend on each
other are evaluated in the order of the manifest.

If a unit fails, every unit that needs it, directly or through other units,
is skipped. Units that do not depend on the failed unit are still evaluated.

## 4. Variables

`local` binds a variable of the unit to a string:

```
local SRC_DIR = "@{dottr.env.home}/.local/src/neovim"
local BUILD_DIR = "@{SRC_DIR}/build"
```

A variable is visible in the statements of its unit that follow it, and in no
other unit. A name can be bound only once in a unit.

### 4.1 Strings

A string literal is enclosed in double quotes and contained in one line.
`\"` and `\\` denote a double quote and a backslash; any other backslash
sequence is an error.

### 4.2 Interpolation

Within a string literal, `@{NAME}` is replaced with the value of the variable
`NAME`, and `@{dottr.env.home}` with the home directory of the user: `$HOME`,
or the home directory recorded in the password database if `HOME` is unset or
empty, and `%USERPROFILE%` on Windows. Other uses of `@` are ordinary text. A
`@{` that does not begin a well-formed reference, or a reference to a variable
that is not bound at that point, is an error. The language has no escape for a
literal `@{` in a string.

Interpolation applies to string literals only. Shell scripts are passed to the
shell unchanged and read variables from the environment (Section 6.1).

### 4.3 Reserved Names

Keywords cannot be used as variable or unit identifiers:

```
pkg  unit  systems  do  end  local  needs  check  create  apply  apply_if  apply_unless
else  shell  command  file  directory  on  link
```

A variable cannot be named after one of the environment variables that
control how a shell starts or finds commands, since every variable is passed
to shell scripts in the environment: `BASHOPTS`, `BASH_ENV`, `CDPATH`, `ENV`,
`HOME`, `IFS`, `OLDPWD`, `PATH`, `PWD`, `SHELL`, `SHELLOPTS`, and `ZDOTDIR`
for POSIX shells, and `APPDATA`, `ComSpec`, `LOCALAPPDATA`, `PATHEXT`,
`PSModulePath`, `SystemRoot`, `TEMP`, `TMP`, and `USERPROFILE` for
PowerShell and Windows. Since Windows does not distinguish the case of
environment variable names, neither does this rule: `Path` and `path` are
reserved as well.

## 5. Checks

`check KIND NAME` tests the value of the variable `NAME` against the host and
records the result as a *fact* about `NAME`:

| Kind | The fact is true if the value ... |
|------|-----------------------------------|
| `command` | names an executable file: a path containing `/`, or a command found in `PATH` |
| `file` | names an existing regular file |
| `directory` | names an existing directory |

Symbolic links are followed. A fact is not a variable: it cannot be
interpolated, and it does not change the value of `NAME`. A fact is a
snapshot of the host at the time of the check; an action that later changes
the host does not change it. Checking the same variable again replaces the
fact.

```
local NVIM = "nvim"
check command NVIM
```

### 5.1 Ordering

Statements are evaluated in order, so a check observes the effects of the
statements before it, including package installations and created
directories.

### 5.2 Creating Directories

`create directory NAME` creates the directory named by the value of `NAME`,
including missing parent directories. An existing directory is left
unchanged; an existing file of another type is an error. `create` records no
fact.

### 5.3 Symbolic Links

`link SOURCE TARGET` creates a symbolic link at `TARGET` that refers to
`SOURCE`. `link` always creates symbolic links, not hard links. Both operands
name local variables bound earlier in the unit:

```
unit editor_config do
  local SOURCE = "@{dottr.env.home}/.dotfiles/.config/nvim"
  local CONFIG = "@{dottr.env.home}/.config"
  local TARGET = "@{CONFIG}/nvim"
  create directory CONFIG
  link SOURCE TARGET
end
```

The source may be a file, directory, or symbolic link, including a dangling
symbolic link. An absent source or special filesystem object is an error.
A directory source is linked as a single directory symlink. The target's
parent directories must already exist; use `create directory` when needed.
No fact is recorded by `link`.

An existing symbolic link whose normalized destination is the requested
source is left unchanged. Existing files, directories, and links to other
paths are conflicts. Dottr reports an error without replacing, adopting,
backing up, or deleting them. Target ancestor directories must be real
directories. Directory links that already point to the requested source
subtree are retained in tree mode without traversing them; other directory
links and Windows junctions are conflicts.

`link tree SOURCE TARGET` instead merges the contents of a real source
directory into a real target directory, creating missing directories and
linking each source file:

```
unit editor_files do
  local SOURCE = "@{dottr.env.home}/.dotfiles/.config/nvim"
  local TARGET = "@{dottr.env.home}/.config/nvim"
  link tree SOURCE TARGET
end
```

Source directory symlinks are linked as leaves; they are not traversed.
Existing target directories are reused, and unrelated target files are
retained. Empty source directories create real empty target directories.
Existing matching directory symlinks, such as those left by Stow's tree
folding, are retained without unfolding or writing through them. Their
contents remain exposed by that existing directory link.
The target may contain the source repository, allowing a clone at
`$HOME/.dotfiles` to be deployed into `$HOME`. A target inside the source,
or a mapped destination that would write into the source itself, is refused.
Entries are processed in
lexical order, and dottr checks the whole tree for conflicts before the first
write. Filesystem errors or concurrent changes during application can still
leave earlier successful operations in place; deployment is not transactional.

No files are excluded implicitly. An optional `ignore RULES` reads exclusion
patterns from the file named by a bound local. For example, deploy a whole
repository using an existing Stow ignore file:

```
unit dotfiles do
  local SOURCE = "@{dottr.env.home}/.dotfiles"
  local TARGET = "@{dottr.env.home}"
  local RULES = "@{SOURCE}/.stow-local-ignore"
  link tree SOURCE TARGET ignore RULES
end
```

Ignore files contain one expression per line. Blank lines and comments
beginning with an unescaped `#` are omitted. Expressions without `/` match
the complete basename; expressions containing `/` match complete subpaths
of the source-relative path prefixed by `/`. This follows
[Stow's ignore matching conventions](https://www.gnu.org/software/stow/manual/html_node/Types-And-Syntax-Of-Ignore-Lists.html).
The selected ignore file is itself omitted if it is inside the source tree.
Ignored directories are not traversed, and ignored target entries are left
unchanged.

The portable matcher supports literal bytes, escaped punctuation, `.`,
`^`, `$`, and the `*`, `+`, and `?` quantifiers. Character classes, grouping,
alternation, counted repetition, and backreferences are unsupported and
produce a diagnostic with the rule file and line before any writes occur.
Patterns are not Git ignore globs. No external matcher or Stow process runs.
No default ignore file is searched for; the manifest selects one explicitly.
Removed source files do not cause old target links to be removed automatically.

Paths are normalized to absolute paths relative to dottr's working directory,
as shell actions are. `.` and `..` components are normalized lexically.
Links store absolute source paths so their meaning does not depend on the
target's parent directory. Moving the source repository requires applying
an updated declaration after resolving the old links as conflicts. On systems
with a symlinked temporary or home directory, use its real path for targets.

`tree` is a contextual modifier when followed by two variable names; it is
not reserved as a package or variable name. For example, `link tree TARGET`
uses the variable `tree` as its source. Statements may appear in a unit or
an `on` block within it, alongside `create directory`; they are not actions
inside `apply`, `apply_if`, or `apply_unless`.

`dottr check` validates operands without reading the filesystem. `plan`
inspects existing trees and reports links and directories it would create
without writing them. A missing source is reported as currently missing;
it may be supplied by a preceding package or shell action during `apply`.
Similarly, missing parents are allowed in a plan, since preceding
`create directory` statements are not executed by planning. Application
requires those sources and parents to exist when the statement is evaluated.
An ignore file must exist and parse when the source tree exists; a missing
source is deferred by planning as above.

On Windows, dottr uses native symbolic links for files and directories;
junctions and copies are not substituted. Creating links requires Windows
Developer Mode or symbolic-link privileges. Failure to obtain them is an
error, with an instruction to enable Developer Mode or use an account with
the required privileges.

## 6. Actions

`apply`, `apply_if`, and `apply_unless` contain actions, which are `shell`
blocks:

```
apply do
  shell do
    ```
    systemctl --user enable --now syncthing
    ```
  end
end
```

`apply_if NAME` runs its actions if the fact recorded for `NAME` is true, and
the actions of its optional `else` branch otherwise. `apply_unless NAME` runs
its actions if the fact is false. Both require a `check` of `NAME` earlier in
the unit.

```
check directory TPM_DIR
apply_if TPM_DIR do
  shell do
    ```
    git -C "$TPM_DIR" pull --ff-only
    ```
  end
else
  shell do
    ```
    git clone --depth=1 https://github.com/tmux-plugins/tpm "$TPM_DIR"
    ```
  end
end
```

The actions of a construct run in order, and the first action that fails
stops the unit; the unit then fails.

### 6.1 Shell Blocks

The script of a `shell` block is enclosed in fences, lines consisting of three
backticks. The opening fence may name the shell that runs the script: `sh`,
`bash`, `zsh`, `pwsh` (PowerShell 7), or `powershell` (Windows PowerShell).
Without a name, the script runs with `sh`, or with `powershell` on Windows:

````
shell do
  ```bash
  shopt -s nullglob
  for file in "$CONFIG_DIR"/*.conf; do
    echo "$file"
  done
  ```
end
````

The script is taken as written, without interpretation by dottr. `end`,
`else`, `#`, and `@{` inside it belong to the shell. The indentation of the
opening fence is removed from every line of the script, and each non-blank
line must be indented at least as far as the fence. The script keeps its
indentation relative to the fence, so heredocs and indented heredoc content
work as in a shell script file:

````
shell do
  ```
  cat > "$CONFIG" <<EOF
  [core]
      editor = nvim
  EOF
  ```
end
````

Each block runs as a separate process, with the interpreter found in `PATH`,
and a non-zero exit status fails the unit. The script runs in the working
directory of dottr, with its standard input, output, and error. Its
environment is the environment of dottr, together with every variable of the
unit bound before the block:

```
local TPM_DIR = "@{dottr.env.home}/.config/tmux/plugins/tpm"
...
    git clone https://github.com/tmux-plugins/tpm "$TPM_DIR"
```

No state is carried from one block to the next: a `cd` or `export` in one
block does not affect another.

`sh`, `bash`, and `zsh` run the script as `INTERPRETER -e -c SCRIPT`, so the
script stops at the first command that fails.

`pwsh` and `powershell` run the script from a temporary file, as
`INTERPRETER -NoLogo -NoProfile -ExecutionPolicy Bypass -File FILE`, and
remove the file afterwards. The script runs with `$ErrorActionPreference`
set to `'Stop'`, so the first error stops it. PowerShell does not treat a
failing native program as an error unless told to: `pwsh` scripts run with
`$PSNativeCommandUseErrorActionPreference` set to `$true`, which stops them
at a failing native program in PowerShell 7.4 and later. Windows PowerShell
has no such setting; a script checks `$LASTEXITCODE` after a native program
whose failure matters, and ends with `exit $LASTEXITCODE` if its last
command is a native program. Variables are read as `$env:NAME`:

````
local NVIM_DIR = "@{dottr.env.home}/AppData/Local/nvim"
...
    shell do
      ```powershell
      git clone https://github.com/user/nvim-config $env:NVIM_DIR
      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
      ```
    end
````

## 7. Systems

A manifest can describe distribution families and operating systems. An
`on` block selects its contents for the systems it names:

```
unit fd do
  on arch, fedora, macos do
    pkg fd
  end
  on debian do
    pkg "fd-find"
  end
  on windows do
    pkg "sharkdp.fd"
  end
end
```

| Selector | Package manager | Example distributions |
|----------|-----------------|-----------------------|
| `arch` | pacman | Arch Linux, EndeavourOS, Manjaro, CachyOS |
| `debian` | apt | Debian, Ubuntu, Linux Mint, Pop!_OS |
| `fedora`, `redhat`, `rhel` | dnf | Fedora, RHEL, CentOS Stream, Rocky Linux, AlmaLinux |
| `macos` | Homebrew | macOS |
| `windows` | winget | Windows |

`linux` selects `arch`, `debian`, and `fedora`. The selectors `fedora`,
`redhat`, and `rhel` are interchangeable; diagnostics use `fedora`.
Selection is determined by the package manager detected on the host,
without reading `/etc/os-release`. A new distribution using a supported
manager needs no new selector. Package-manager selectors such as `on apt`
are rejected with a suggested family replacement. The command-line option
`--package-manager` still takes manager names.

Selecting a single distribution within a family is not supported. A
manifest can use `check`, such as `check command SNAP`, or a shell test to
choose an action that depends on a particular host capability.

`on` blocks may appear at the top level of the manifest, around units; in a
unit, around its statements; and in `apply`, `apply_if`, and `apply_unless`,
around `shell` blocks:

````
on macos do
  pkg rectangle
end

unit tpm do
  needs git
  on linux, macos do
    local TPM_DIR = "@{dottr.env.home}/.config/tmux/plugins/tpm"
  end
  on windows do
    local TPM_DIR = "@{dottr.env.home}/.tmux/plugins/tpm"
  end
  check directory TPM_DIR
  apply_unless TPM_DIR do
    on linux, macos do
      shell do
        ```
        git clone https://github.com/tmux-plugins/tpm "$TPM_DIR"
        ```
      end
    end
    on windows do
      shell do
        ```
        git clone https://github.com/tmux-plugins/tpm $env:TPM_DIR
        ```
      end
    end
  end
end
````

`on` blocks may be nested; a nested block applies on the systems that both
blocks name, and a nested block that shares no system with the enclosing
block is an error. `on` blocks have no `else` branch.

### 7.1 The Manifest of a System

On a system, a manifest consists of the statements that apply on it: the
contents of `on` blocks for other systems are not part of it. dottr applies
the manifest of the host's system, which is selected by its package manager
([Command Line](command-line.md), Section 1.3).

A manifest is valid if it is valid on every declared system, or every
supported system when there is no `systems` declaration. `dottr check`
annotates an error with its systems only when it occurs on a proper subset
of that scope:

```
packages.dotfile:21:19: error: undefined variable `X` (on linux, macos)
```

Consequently:

* A variable bound in an `on` block can be used after it only on the systems
  of the block; to use it on every system, bind it in a block for each.
* A unit declared in an `on` block exists only on its systems, and only units
  on the same systems may need it.
* A unit or variable may be declared once per system, as `TPM_DIR` is above.

### 7.2 Declaring Manifest Systems

An optional declaration precedes all units and `on` blocks:

```
systems arch, debian

unit dotfiles do
  local ROOT = "@{dottr.env.home}/.dotfiles"
end
```

The declaration occurs at most once and takes the same selectors as `on`.
Lists may continue after a comma, with intervening blank or comment-only
lines. Without it the manifest supports every system. With it, the manifest
behaves as though enclosed in an `on` block for the declared systems:
statements and checking are restricted to that set. An `on` block with no
overlap is an error; a partially overlapping selector, such as `on linux`
within `systems arch, debian`, narrows to the shared systems.

`plan` and `apply` refuse systems outside the declaration with exit status
2, including systems selected through `--package-manager`:

```
dottr: error: packages.dotfile is for arch, debian; this host uses dnf (fedora)
```

### 7.3 Placement of System Conditions

Place `on` around a unit when that unit exists only on those systems, for
example an Arch-specific boot configuration. Other units may depend on it
only where it exists. Place `on` inside a unit when its purpose is shared
but its packages, dependencies, scripts, or installation method differ.

A block unit that has no statements on some of its systems is an error;
otherwise dependencies could succeed without the intended work:

```
unit ghostty do
  on debian do
    pkg ghostty
  end
end
```

This unit is empty on arch, fedora, macos, and windows. Declare the unit
inside `on debian` instead, or provide statements for its other systems.
Only systems within the manifest's declaration are considered. A unit
containing only `needs` is a deliberate grouping and is valid.

## 8. Idempotence

A manifest is meant to be applied repeatedly, each time bringing the host to
the declared state. Package units are idempotent, since installing an
installed package changes nothing. Symlink statements retain matching links
and existing real directories in tree mode. Block units are idempotent if
their actions are: work that must
happen once, such as cloning a repository, is gated with `check` and
`apply_if` or `apply_unless`, and work that is safe to repeat, such as
updating a clone, runs in the other branch.
