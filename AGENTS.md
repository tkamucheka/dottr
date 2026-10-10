# dottr Project Instructions

## Layout
* Public headers: `include/dottr/` (include as `"dottr/<name>.h"`)
* Library code: `lib/`
* Program entry points: `tools/` (`tools/dottr.c` is the frontend;
  `tools/dottr-fuzzer/` is the libFuzzer harness)
* Tests and fixtures: `test/`
* Build output: `build/` (gitignored)

## Build Commands
* Create build directory: `mkdir -p build`
* Build tool: `cc -std=c11 -Wall -Wextra -Iinclude lib/*.c tools/dottr.c -o build/dottr`
* Build tests: `cc -std=c11 -Wall -Wextra -Iinclude lib/*.c test/*.c -o build/dottr-test`
* Run tests (from the repository root, after building the tool): `build/dottr-test`
* Regenerate expected fixture output: `build/dottr-test --update` (review the
  resulting diff of `test/cases/*.out` before committing)
* Build fuzzer (requires clang): `clang -std=c11 -Wall -Wextra -g -O1 -fsanitize=fuzzer,address,undefined -Iinclude lib/*.c tools/dottr-fuzzer/fuzz_manifest.c -o build/dottr-fuzzer`
* Run fuzzer: `mkdir -p build/fuzz-corpus && build/dottr-fuzzer -dict=tools/dottr-fuzzer/dottr.dict -artifact_prefix=build/ -max_total_time=300 build/fuzz-corpus test/cases`
  (new inputs go to `build/fuzz-corpus`, crash reproducers to `build/`;
  reproduce a crash with `build/dottr-fuzzer FILE`)
* Clean build directory: `rm -rf build`

## Code Style and Architecture
* Language Standard: C11
* Naming Convention: camelCase for variables and functions, PascalCase for types

## Implementation Guidelines
* All documentation must be technical/academic.
* Never use emojis in documentation, comments, or commit messages.

## Repository Hygiene
* Do not create temporary files in the project tree. Use the system temp
  directory (or an agent scratch directory) and clean up afterwards.
* Do not create markdown files
  unless explicitly asked. No summaries, notes, reports, or changelogs written
  as standalone files; report results in the conversation instead.
