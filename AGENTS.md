# dottr Project Instructions

## Layout
* Public headers: `include/dottr/` (include as `"dottr/<name>.h"`)
* Library code: `lib/`
* Programs: `tools/<program>/<program>.c`, one directory each: `dottr` (the
  frontend), `dottr-format` (formatter), `dottr-lsp` (language server), and
  `dottr-fuzzer` (libFuzzer harness)
* Tests and fixtures: `test/`
* Editor support: `utils/`, as git submodules (clone with
  `--recurse-submodules` or run `git submodule update --init`):
  `utils/tree-sitter-dottr/` is the tree-sitter grammar and queries, and
  `utils/vscode-dottr/` the VS Code extension; Neovim setup in
  `docs/reference/neovim.md`
* Build output: `build/` (gitignored)
* Install script: `install.sh` (documented in `docs/reference/installation.md`)

## Build Commands
* Create build directory: `mkdir -p build`
* Build programs: `for p in dottr dottr-format dottr-lsp; do cc -std=c11 -Wall -Wextra -Iinclude lib/*.c tools/$p/$p.c -o build/$p; done`
* Build tests: `cc -std=c11 -Wall -Wextra -Iinclude lib/*.c test/*.c -o build/dottr-test`
* Run tests (from the repository root, after building the programs): `build/dottr-test`
* Regenerate expected fixture output: `build/dottr-test --update` (review the
  resulting diff of `test/cases/*.out` before committing)
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
* All documentation must be technical/academic.
* Never use emojis in documentation, comments, or commit messages.

## Repository Hygiene
* Do not create temporary files in the project tree. Use the system temp
  directory (or an agent scratch directory) and clean up afterwards.
* Do not create markdown files outside
  `docs/reference/`, other than README files, unless explicitly asked.
  No summaries, notes, reports, or changelogs written as standalone files;
  report results in the conversation instead.
