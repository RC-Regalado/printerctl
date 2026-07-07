---
title: Milestone 3 — G-code Parser and Dry Run
status: done
date: 2026-07-07
tags: [printerctl, milestone, gcode, dry-run]
---

# Milestone 3 — G-code Parser and Dry Run

Source of truth: [[Agents]], [[architecture]], [[roadmap]]. Design
details: [[milestone-3-gcode-dry-run|docs/design/milestone-3-gcode-dry-run.md]].

## 1. Technical summary

Milestone 3 adds a standalone `gcode` module and wires it into
`--dry-run print FILE`. The dry run parses a G-code file, normalizes
commands, reports source line numbers and progress, logs state/progress,
and never touches serial transport.

## 2. Files changed

- `CMakeLists.txt`
- `include/printerctl/cli.h`
- `include/printerctl/gcode.h`
- `src/cli/cli.c`
- `src/gcode/gcode.c`
- `tests/CMakeLists.txt`
- `tests/test_cli.c`
- `tests/test_gcode.c`
- `README.md`
- `architecture.md`
- `docs/design/milestone-3-gcode-dry-run.md`
- `docs/milestones/milestone-3.md`

## 3. Acceptance validation

- Deterministic parser fixtures: covered by `printerctl_gcode_tests`.
- Comments and blank lines ignored: covered by parser and CLI tests.
- Progress reported: parser result tests and dry-run output tests.
- Dry-run never opens serial devices: there is still no serial module
  linked into the dry-run path.
- Unsafe `G28` fails by default: parser and CLI tests return safety error.
- Normal, empty, malformed and unsafe files: covered by unit tests.

## 4. Verification

- GCC 16.1.1 build: passed with `-std=c17 -Wall -Wextra -Wpedantic`.
- Clang 22.1.6 build: passed with `-std=c17 -Wall -Wextra -Wpedantic`.
- CTest: 4/4 passed.
- `clang-tidy`: executed with the Milestone 2 analyzer exclusions; it
  exits successfully. Remaining findings are readability/test-harness
  noise and the existing POSIX macro false positive.

## 5. Risks found

- The parser is intentionally strict around parenthesized comments and
  control bytes. This is safer for dry-run validation but may reject unusual
  slicer output.
- `G28` detection is conservative. It blocks `G28` variants before any
  future explicit print-file homing confirmation exists.
- Progress is byte-based, not motion/time based. This is correct for M3
  dry-run reporting and should not be presented as ETA.

## 6. Decisions taken

- G-code line limit: `4096` bytes, excluding newline.
- Dry-run `print` returns success only after full parse.
- `G28` in print files is not confirmable from the CLI in M3.
- G-code file errors use exit code `6`; unsafe `G28` keeps exit code `4`.
- Structured logs omit G-code paths and command text.

## 7. Next steps

Continue with Milestone 4: Marlin response parser. Do not add serial I/O
until Milestone 5.
