---
title: Milestone 0 - Project Bootstrap
milestone: 0
status: done
date: 2026-07-04
tags: [printerctl, milestone, bootstrap, cmake, cli]
---

# Milestone 0 — Project Bootstrap

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

## 1. Technical summary

Bootstrapped `printerctl` as a buildable, warning-free C17/CMake project.
The executable currently understands exactly two arguments, `--help` and
`--version`; any other invocation (no arguments, an unknown flag, or extra
arguments) fails with an explicit message on `stderr` and a non-zero exit
code. No serial, Marlin, G-code, config or logging code exists yet — this
milestone is scaffolding and CLI plumbing only, matching the roadmap's
"Empty/minimal binary can print version/help output" acceptance criterion.

The version number is declared once in `CMakeLists.txt`
(`project(printerctl VERSION 0.1.0 ...)`) and exposed to C through a header
generated at configure time from `include/printerctl/version.h.in`. There is
no second, hand-maintained copy of the version string.

`main()` is a two-line shim that forwards to `printerctl_cli_run()`, which
lives in a separate static library (`printerctl_cli`) built from
`src/cli/cli.c`. That function takes explicit `FILE *out` / `FILE *err`
parameters instead of writing to `stdout`/`stderr` directly, so the test
executable can link the library directly and capture output in memory
(`open_memstream`) rather than shelling out to the built binary. This is
what makes the "linkable and testable" requirement concrete: the test
binary and the production binary share the same compiled CLI logic.

## 2. Files added

```text
CMakeLists.txt
include/printerctl/cli.h
include/printerctl/version.h.in
src/main.c
src/cli/cli.c
tests/CMakeLists.txt
tests/test_cli.c
README.md
docs/milestones/milestone-0.md
```

`include/printerctl/version.h` is generated at configure time into the
build directory; it is not committed source.

## 3. Build and test targets

- `printerctl_cli` — static library containing all CLI argument handling
  (help text, version text, error messages, exit codes). Depends on the
  generated `version.h`.
- `printerctl` — production executable; only contains `main()`.
- `printerctl_tests` — independent CTest executable. Links `printerctl_cli`
  directly (not `printerctl`), calls `printerctl_cli_run()` in-process for
  each scenario, and asserts on both the exit code and the captured
  `stdout`/`stderr` content (not just "did it crash").

Compiler flags: `-std=c17` (`CMAKE_C_EXTENSIONS OFF`) with
`-Wall -Wextra -Wpedantic`, applied project-wide via `add_compile_options`.

## 4. Decisions

1. **No test framework.** `tests/test_cli.c` is a plain C program with a
   small local `check()` helper and a failure counter; it returns
   `EXIT_FAILURE` if any check failed. Justified by the small number of
   scenarios at this milestone; revisit if/when Milestone 3+ needs
   parameterized fixtures or richer assertions.
2. **No placeholder modules.** `architecture.md` §4 explicitly allows the
   repository layout to "start smaller." The roadmap's Milestone 0 task list
   suggests placeholder modules for `config`, `logging`, `serial`, `marlin`,
   `gcode` and `printer`, but empty placeholder `.c`/`.h` files with no
   behavior would be dead code with nothing to test, contradicting the "no
   dead code" requirement. Only `cli` exists, because it is the one module
   this milestone actually needs (`--help`/`--version`). The other modules
   will be created together with their first real implementation, starting
   at Milestone 1 (`cli` argument surface) and continuing per the roadmap.
3. **No `mock_marlin`, scripts, Docker or Ansible.** These belong to later
   milestones (serial/integration testing, environment isolation) and have
   no purpose yet with zero hardware/serial code in the tree.
4. **Documentation scope under `docs/`.** Only this milestone report is
   added under `docs/`. `AGENTS.md` lists `docs/marlin.md`, `docs/testing.md`,
   `docs/safety.md`, `docs/logging.md` and `docs/apcupsd.md` as documents to
   maintain, but each describes behavior that doesn't exist yet (Marlin
   protocol, logging, APCUPSD integration, hardware/fault testing). Writing
   them now would either be empty stubs or speculative documentation of
   unbuilt behavior. They will be created alongside the milestones that
   implement what they describe (Milestones 2, 4, 9/10 and the general
   testing work in Milestones 5–9).
5. **Exit codes.** Only two are defined so far:
   `PRINTERCTL_EXIT_OK` (0) and `PRINTERCTL_EXIT_USAGE_ERROR` (1). The
   architecture document (§9, "Open Technical Decisions" #6 in roadmap
   terms) leaves a full exit-code taxonomy (CLI usage vs. config vs. serial
   vs. safety-policy errors, etc.) as an open decision; that taxonomy is
   Milestone 1's concern once real commands and config parsing exist.
6. **Program name in messages is a literal, not `argv[0]`.** Usage/help/error
   text always says `printerctl` regardless of how the binary was invoked,
   so output is deterministic and independent of the invocation path — this
   is what the tests assert against.

## 5. Deviations from the roadmap's literal Milestone 0 task list

- Did not add placeholder modules for `config`, `logging`, `serial`,
  `marlin`, `gcode`, `printer` (see Decision 2 above).
- Did not create `/scripts`, `/docker`, `/ansible` directories (see
  Decision 3 above).
- Did not create `docs/marlin.md`, `docs/testing.md`, `docs/safety.md`,
  `docs/logging.md`, `docs/apcupsd.md` (see Decision 4 above).

These were reviewed and approved deviations, not omissions; `architecture.md`
§4 explicitly sanctions a smaller starting layout.

## 6. Risks

- **Exit code taxonomy is provisional.** Only "success" and "usage error"
  exist. Milestone 1 must decide whether config errors, safety-policy
  rejections, etc., need distinct codes, and that may change the value of
  `PRINTERCTL_EXIT_USAGE_ERROR` for existing call sites. Low risk: only one
  call site exists today.
- **CLI grammar will change substantially in Milestone 1** (global options,
  subcommands, `home --i-understand-no-endstops`). The current strict
  "exactly one argument, must be `--help` or `--version`" parser will be
  replaced, not extended in place — `printerctl_cli_run()`'s current
  argument-count check does not generalize to option/subcommand parsing and
  is expected to be rewritten rather than incrementally patched.
- **`open_memstream` is POSIX, not standard C17.** It is used only in the
  test executable, gated behind `_POSIX_C_SOURCE 200809L`, and never in
  production code. This is acceptable because the target platform is Linux
  only (per `Agents.md`), but it means the test binary is not portable to a
  hypothetical non-POSIX target if that were ever needed.

## 7. Testing

`ctest --test-dir <build-dir> --output-on-failure` runs `printerctl_tests`,
which exercises, against the real exit code and captured output:

- `--help` → exit 0, `stdout` documents usage, `--help` and `--version`,
  `stderr` empty.
- `--version` → exit 0, `stdout` contains `printerctl <version>`, `stderr`
  empty.
- no arguments → non-zero exit, explicit "missing command" on `stderr`,
  `stdout` empty.
- unknown argument (`--bogus`) → non-zero exit, `stderr` names the
  offending argument, `stdout` empty.
- extra arguments (`--help extra`) → non-zero exit, explicit "too many
  arguments" on `stderr`.

Verified locally with both GCC 16.1.1 and Clang 22.1.6, `-std=c17
-Wall -Wextra -Wpedantic`, zero warnings, all five checks passing under
`ctest` for both compilers.

## 8. Next steps

Milestone 1 (CLI and Configuration), per `roadmap.md` §4:

- Introduce the real argument grammar: global options (`--port`, `--baud`,
  `--timeout`, `--dry-run`, `--verbose`, `--config`) and subcommands
  (`print`, `pause`, `resume`, `cancel`, `temp`, `position`, `info`, `send`,
  `home`).
- Introduce the `config` module and its precedence rules (defaults → config
  file → CLI overrides).
- Enforce `home --i-understand-no-endstops` at the parser level.
- Decide and document the fuller exit-code taxonomy referenced in Risk 1
  above.
- Expand `printerctl --help` to document all MVP commands, per Milestone
  1's acceptance criteria.
