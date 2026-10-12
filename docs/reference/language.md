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

The language is line-oriented: each statement occupies one line, and blank
lines are ignored. `#` begins a comment that extends to the end of the line,
except inside a string literal or a shell script.

## 2. Units

A unit is declared with `pkg` in one of two forms.

**Package units.** `pkg NAME` installs the package `NAME` with the system
package manager:

```
pkg git
pkg "tree-sitter-cli"
```

A name that is not an identifier, such as one containing `-`, is written as a
string; such a string is not empty and contains no `@{...}` reference.
`pkg tmux` and `pkg "tmux"` declare the same unit. Identifiers consist of
ASCII letters, digits, and `_`, and do not begin with a digit.

**Block units.** `pkg NAME do ... end` is satisfied by evaluating the
statements of its body in order:

```
pkg tmux do
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

The statements of a block unit are:

| Statement | Effect |
|-----------|--------|
| `needs A, B` | Requires the units `A` and `B` (Section 3). |
| `pkg NAME` | Installs the package `NAME` (Section 2.1). |
| `local NAME = "..."` | Binds a variable (Section 4). |
| `check KIND NAME` | Records a fact about the host (Section 5). |
| `create directory NAME` | Creates a directory (Section 5.2). |
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

Inside a block unit, `pkg NAME` installs the package `NAME` at that point of
the body. It is commonly used to install the package that a block unit
configures, as `pkg tmux` does above. Such a package is not a unit of its own:
`needs tmux` in another unit refers to the top-level unit `tmux`.

## 3. Dependencies

`needs` names units that must be satisfied before the declaring unit is
evaluated:

```
pkg neovim do
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
empty. Other uses of `@` are ordinary text. A `@{` that does not begin a
well-formed reference, or a reference to a variable that is not bound at that
point, is an error. The language has no escape for a literal `@{` in a
string.

Interpolation applies to string literals only. Shell scripts are passed to the
shell unchanged and read variables from the environment (Section 6.1).

### 4.3 Reserved Names

Keywords cannot be used as variable or unit identifiers:

```
pkg  do  end  local  needs  check  create  apply  apply_if  apply_unless
else  shell  command  file  directory  on
```

A variable cannot be named after one of the environment variables that
control how a shell starts or finds commands, since every variable is passed
to shell scripts in the environment: `BASHOPTS`, `BASH_ENV`, `CDPATH`, `ENV`,
`HOME`, `IFS`, `OLDPWD`, `PATH`, `PWD`, `SHELL`, `SHELLOPTS`, and `ZDOTDIR`.

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
backticks. The opening fence may name the shell that runs the script, `sh`,
`bash`, or `zsh`; the default is `sh`:

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

Each block runs as a separate process, `INTERPRETER -e -c SCRIPT`, with the
interpreter found in `PATH`, so the script
stops at the first command that fails, and a non-zero exit status fails the
unit. The script runs in the working directory of dottr, with its standard
input, output, and error. Its environment is the environment of dottr,
together with every variable of the unit bound before the block:

```
local TPM_DIR = "@{dottr.env.home}/.config/tmux/plugins/tpm"
...
    git clone https://github.com/tmux-plugins/tpm "$TPM_DIR"
```

No state is carried from one block to the next: a `cd` or `export` in one
block does not affect another.

## 7. Systems

A manifest can describe several systems: a system is an operating system
with its package manager. An `on` block selects its contents for the systems
it names:

```
pkg fd do
  on pacman, dnf, brew do
    pkg fd
  end
  on apt do
    pkg "fd-find"
  end
  on windows do
    pkg "sharkdp.fd"
  end
end
```

| System | Operating system | Package manager |
|--------|------------------|-----------------|
| `pacman` | Linux | pacman |
| `apt` | Linux | apt |
| `dnf` | Linux | dnf |
| `brew` | macOS | Homebrew |
| `winget` | Windows | winget |

An `on` block names systems by package manager, as in the table, or by
operating system: `linux` stands for `pacman`, `apt`, and `dnf`, `macos` for
`brew`, and `windows` for `winget`. The block applies on every system it
names.

`on` blocks may appear at the top level of the manifest, around units; in a
unit, around its statements; and in `apply`, `apply_if`, and `apply_unless`,
around `shell` blocks:

````
on macos do
  pkg rectangle
end

pkg tpm do
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

A manifest is valid if it is valid on every system. `dottr check` therefore
reports an error that occurs on some systems only with the systems on which
it occurs:

```
packages.dotfile:21:19: error: undefined variable `X` (on linux, macos)
```

Consequently:

* A variable bound in an `on` block can be used after it only on the systems
  of the block; to use it on every system, bind it in a block for each.
* A unit declared in an `on` block exists only on its systems, and only units
  on the same systems may need it.
* A unit or variable may be declared once per system, as `TPM_DIR` is above.

## 8. Idempotence

A manifest is meant to be applied repeatedly, each time bringing the host to
the declared state. Package units are idempotent, since installing an
installed package changes nothing. Block units are idempotent if their actions are: work that must
happen once, such as cloning a repository, is gated with `check` and
`apply_if` or `apply_unless`, and work that is safe to repeat, such as
updating a clone, runs in the other branch.
