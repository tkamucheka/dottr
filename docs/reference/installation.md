# Installation

dottr consists of three programs: `dottr`, `dottr-format`, and `dottr-lsp`.
The installation scripts install them for the current user, without
administrator privileges.

## 1. Linux and macOS

```
curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh
```

The script installs the programs into `~/.local/opt/dottr/bin` and links them
from `~/.local/bin`, which must be in `PATH`.

### 1.1 Release Archives and Source Builds

By default the script downloads the release archive for the platform:
`linux-x86_64`, `linux-aarch64`, or `macos-universal` (Apple silicon and
Intel). The programs of the Linux archives are statically linked and run on
any distribution. The script verifies the archive against the `SHA256SUMS` of
the release before installing it, which requires `sha256sum` or `shasum`.

The script builds dottr from source instead when there is no archive for the
platform or no release yet, or when `--ref`, `--repo`, or `--source` asks for
a source. A build from source requires a C11 compiler (`$CC`, by default
`cc`, with `$CFLAGS`, by default `-O2`) and git, or, for a repository on
GitHub, curl or wget and tar.

### 1.2 Options

Options follow `sh -s --`, for example to install a particular release:

```
curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh -s -- --version v2.0.0
```

| Option | Environment variable | Default | Meaning |
|--------|----------------------|---------|---------|
| `--version TAG` | `DOTTR_VERSION` | the latest release | Release to install. |
| `--archive FILE` | | | Install a downloaded release archive. |
| `--ref REF` | `DOTTR_REF` | | Build the branch, tag, or commit `REF` from source. |
| `--repo URL` | `DOTTR_REPO` | `https://github.com/tkamucheka/dottr.git` | Build from this repository. |
| `--source DIR` | | | Build from a local checkout. |
| `--prefix DIR` | `DOTTR_PREFIX` | `~/.local/opt/dottr` | Installation directory; the programs are installed in `DIR/bin`. |
| `--bin-dir DIR` | `DOTTR_BIN_DIR` | `~/.local/bin` | Directory of the links to the programs. |
| `--no-link` | | | Do not create the links. |
| `-h`, `--help` | | | Print the options. |

`DOTTR_RELEASES` sets the base URL of the releases, for a mirror; the
default is `https://github.com/tkamucheka/dottr/releases`.

To build from a clone of the repository:

```
git clone https://github.com/tkamucheka/dottr.git
cd dottr
./install.sh --source .
```

### 1.3 Upgrading and Uninstalling

Running the script again installs the requested version over the existing
installation. The programs are obtained and checked before any is
installed, and each program is replaced atomically, so a failed or
interrupted upgrade leaves the previous version in place and running
programs are unaffected.

The script does not replace a file in the link directory that has the name
of a program and is not a symbolic link; it reports the file and stops
before downloading.

To uninstall:

```
rm -rf ~/.local/opt/dottr
rm -f ~/.local/bin/dottr ~/.local/bin/dottr-format ~/.local/bin/dottr-lsp
```

With other `--prefix` or `--bin-dir` directories, remove those instead.

## 2. Windows

In PowerShell:

```
irm https://raw.githubusercontent.com/tkamucheka/dottr/main/install.ps1 | iex
```

The script downloads the release archive `windows-x86_64`, verifies it
against the `SHA256SUMS` of the release, installs the programs into
`%LOCALAPPDATA%\Programs\dottr\bin`, and adds that directory to the user's
`PATH`; terminals opened afterwards find the programs. It runs in Windows
PowerShell 5.1 and PowerShell 7, on Windows 10 version 1903 or later. On Arm
devices the programs run under x64 emulation.

### 2.1 Options

A script run with `iex` takes no arguments, so its options are environment
variables, set before running it:

```
$env:DOTTR_VERSION = 'v2.0.0'
irm https://raw.githubusercontent.com/tkamucheka/dottr/main/install.ps1 | iex
```

| Environment variable | Default | Meaning |
|----------------------|---------|---------|
| `DOTTR_VERSION` | the latest release | Release to install. |
| `DOTTR_ARCHIVE` | | Install a downloaded release archive. |
| `DOTTR_PREFIX` | `%LOCALAPPDATA%\Programs\dottr` | Installation directory; the programs are installed in `PREFIX\bin`. |
| `DOTTR_NO_PATH` | | If set, do not change the user's `PATH`. |
| `DOTTR_RELEASES` | `https://github.com/tkamucheka/dottr/releases` | Base URL of the releases. |

### 2.2 Upgrading and Uninstalling

Running the script again installs the requested version over the existing
installation. A program that is running is renamed and replaced; the running
process is unaffected.

To uninstall, remove the directory and its entry in the user's `PATH`:

```
Remove-Item -Recurse "$env:LOCALAPPDATA\Programs\dottr"
$bin = "$env:LOCALAPPDATA\Programs\dottr\bin"
$path = [Environment]::GetEnvironmentVariable('Path', 'User')
[Environment]::SetEnvironmentVariable('Path',
    (($path -split ';' | Where-Object { $_ -and $_ -ne $bin }) -join ';'), 'User')
```

### 2.3 Building from Source

dottr builds with MSVC (Visual Studio 2019 version 16.8 or later) from a
Developer PowerShell, in a clone of the repository:

```
New-Item -ItemType Directory -Force build\obj | Out-Null
rc /nologo /fo build\dottr.res tools\windows\dottr.rc
foreach ($p in 'dottr', 'dottr-format', 'dottr-lsp') {
    cl /nologo /std:c11 /O2 /Iinclude /D_CRT_SECURE_NO_WARNINGS /Fo:build\obj\ `
        lib\*.c "tools\$p\$p.c" build\dottr.res "/Fe:build\$p.exe"
}
```

The resource file embeds the application manifest, which makes the programs
use UTF-8 for file names and arguments.

## 3. Requirements of `dottr apply`

`dottr apply` installs packages with the package manager of the system:
pacman, apt, or dnf on Linux (the first found in `PATH`), Homebrew on macOS,
and winget on Windows. Shell blocks run with `sh`, `bash`, `zsh`, `pwsh`, or
`powershell`, found in `PATH`; see
[The Manifest Language](language.md), Section 6.1.
