# dottr Project Instructions

## Layout
* Public headers: `include/dottr/` (include as `"dottr/<name>.h"`)
* Library code: `lib/`. Operating system services are behind
  `include/dottr/host.h`, implemented by `lib/host_posix.c` (Linux, macOS)
  and `lib/host_windows.c` (Windows); no other library code is specific to
  an operating system.
* Programs: `tools/<program>/<program>.c`, one directory each: `dottr` (the
  frontend), `dottr-format` (formatter), `dottr-lsp` (language server), and
  `dottr-fuzzer` (libFuzzer harness)
* Tests and fixtures: `test/`
* Editor support: `utils/`, as git submodules (clone with
  `--recurse-submodules` or run `git submodule update --init`):
  `utils/tree-sitter-dottr/` is the tree-sitter grammar and queries, and
  `utils/vscode-dottr/` the VS Code extension; editor setup in
  `docs/reference/editors.md`
* Windows resources (the UTF-8 application manifest): `tools/windows/`
* Build output: `build/` (gitignored)
* Install scripts: `install.sh` (Linux, macOS) and `install.ps1` (Windows),
  documented in `docs/reference/installation.md`
* Continuous integration: `.github/workflows/ci.yml` (tests on Linux, macOS,
  and Windows with MinGW and MSVC; a package installation with each package
  manager) and `.github/workflows/release.yml` (release archives, built and
  published when a tag `v*` is pushed)

## Build Commands
* Create build directory: `mkdir -p build`
* Build programs: `for p in dottr dottr-format dottr-lsp; do cc -std=c11 -Wall -Wextra -Iinclude lib/*.c tools/$p/$p.c -o build/$p; done`
* Build tests: `cc -std=c11 -Wall -Wextra -Iinclude lib/*.c test/*.c -o build/dottr-test`
* Run tests (from the repository root, after building the programs): `build/dottr-test`
* Regenerate expected fixture output: `build/dottr-test --update` (review the
  resulting diff of `test/cases/*.out` before committing)
* Fixture headers: `# args:` and `# status:` are required;
  `# os: linux, macos` limits a fixture to the named operating systems, and
  `# requires: COMMAND` skips it when `COMMAND` is not installed
* Cross-compile for Windows (requires mingw-w64-gcc): `x86_64-w64-mingw32-windres tools/windows/dottr.rc -O coff -o build/dottr-resources.o`, then `for p in dottr dottr-format dottr-lsp; do x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -D__USE_MINGW_ANSI_STDIO=1 -Iinclude lib/*.c tools/$p/$p.c build/dottr-resources.o -o build/$p.exe; done`,
  and the tests with `lib/*.c test/*.c -o build/dottr-test.exe`; run them
  from the repository root with `wine build/dottr-test.exe`. Wine's
  `powershell` is a stub that runs nothing, so the fixture `apply-windows`
  fails under Wine; PowerShell blocks are verified by the Windows jobs of
  continuous integration.
* Build fuzzer (requires clang): `clang -std=c11 -Wall -Wextra -g -O1 -fsanitize=fuzzer,address,undefined -Iinclude lib/*.c tools/dottr-fuzzer/fuzz_manifest.c -o build/dottr-fuzzer`
* Run fuzzer: `mkdir -p build/fuzz-corpus && build/dottr-fuzzer -dict=tools/dottr-fuzzer/dottr.dict -artifact_prefix=build/ -max_total_time=300 build/fuzz-corpus test/cases`
  (new inputs go to `build/fuzz-corpus`, crash reproducers to `build/`;
  reproduce a crash with `build/dottr-fuzzer FILE`)
* Format a manifest: `build/dottr fmt -w -f FILE` (`--check` to verify;
  `test/cases/*.dotfile` are fixtures and are kept as written)
* Clean build directory: `rm -rf build`
* Tree-sitter grammar (requires the tree-sitter CLI; run in
  `utils/tree-sitter-dottr`): regenerate the parser with
  `tree-sitter generate` after editing `grammar.js` and commit the
  regenerated `src/`; run the corpus and highlight tests with
  `tree-sitter test`. Commit grammar changes in the submodule first, then
  commit the updated submodule reference in this repository.

## Code Style and Architecture
* Language Standard: C11
* Naming Convention: camelCase for variables and functions, PascalCase for types

## Implementation Guidelines
* Put reference documentation of the implemented language and tools, such as
  the grammar, in `docs/reference/`.
* Keep README files short and concise: what the project is, how to install
  and use it, and links to the documents in `docs/reference/` for everything
  else. A README does not repeat reference documentation.
* A repository used on its own, such as the tree-sitter grammar submodule, is
  self-contained: its README leads with generic installation and
  configuration sections, as the README of an editor plugin does, and does
  not depend on documentation in this repository.
* README files and reference documentation are product documentation,
  written for the users of dottr and its tools: what a feature does and how
  to use it. They do not describe the current state of the repositories
  (such as a repository being private), the maintainer's own setup,
  implementation techniques, design history, or plans.
* All documentation must be technical/academic.
* Never use emojis in documentation, comments, or commit messages.

## Repository Hygiene
* Do not create temporary files in the project tree. Use the system temp
  directory (or an agent scratch directory) and clean up afterwards.
* Do not create markdown files outside
  `docs/reference/`, other than README files, unless explicitly asked.
  No summaries, notes, reports, or changelogs written as standalone files;
  report results in the conversation instead.
