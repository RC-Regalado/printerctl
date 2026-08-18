---
title: Milestone 4 — Marlin Response Parser
milestone: 4
status: done
date: 2026-07-07
tags: [printerctl, milestone, marlin]
---

# Milestone 4 — Marlin Response Parser

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]]. Design
details: [[milestone-4-marlin|docs/design/milestone-4-marlin.md]].

## 1. Technical summary

Milestone 4 adds a standalone `marlin` module that parses Marlin
response lines independently from any serial transport.
`printerctl_marlin_classify()` maps every received line to one of six
explicit types — `ACK` (`ok`, terminal success), `ERROR` (`Error:`,
terminal failure), `BUSY` (`busy:` and `echo:busy:`, keep waiting),
`ECHO` (informational), `RESEND` (`Resend:`, terminal-unsupported in the
MVP, with the requested line number decoded when clean) and `INFO`
(anything else, never an error) — with the streaming decision
precomputed in a `terminal` flag. Three typed parsers decode the report
lines the Milestone 6 commands will render: temperature (M105, spaced
and unspaced `actual/target` variants, `B@:` never confused with `B:`),
position (M114, logical coordinates before `Count`, X/Y/Z required, E
optional) and firmware info (M115, space-containing values delimited by
the known key set, `FIRMWARE_NAME` required).

Pure text processing: no I/O, no heap, no global state, no dependency on
any other module. Nothing links it yet — serial (M5) and the single
Marlin commands (M6) are its first consumers.

## 2. Files changed

- `CMakeLists.txt`
- `include/printerctl/marlin.h`
- `src/marlin/marlin.c`
- `tests/CMakeLists.txt`
- `tests/test_marlin.c`
- `README.md`
- `docs/design/milestone-4-marlin.md`
- `docs/milestones/milestone-4.md`

## 3. Acceptance validation

- `ok` recognized as acknowledgement: classification tests, including
  `ok <payload>` and ADVANCED_OK shapes; `okay`/`OK` lookalikes stay
  informational.
- `busy:`/`echo:` non-terminal informational: classification tests
  assert `terminal == false`, and `echo:busy:` classifies as busy, not
  echo.
- `Error:` becomes failure: classified terminal with its message as
  payload.
- `Resend:` becomes unsupported/resend-required: classified terminal
  with the requested line number decoded (`Resend: 5`, `Resend:12345`),
  and a malformed value reporting no number rather than a guess.
- Temperature and position responses renderable for users: typed
  parsers return decoded numeric structs from representative M105/M114
  lines, including via a classified `ok T:...` payload; M115 decodes
  firmware name, machine type, protocol version, UUID and extruder
  count.

## 4. Verification

- GCC build (`build-gcc`): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`.
- Clang build (`build-clang`): warning-free with the same flags.
- CTest: 5/5 test binaries pass on both compilers
  (`printerctl_marlin_tests` is new; the other four are unchanged and
  still pass).
- `clang-tidy`: executed with the Milestone 2 analyzer exclusions; it
  exits successfully. Remaining findings are readability/test-harness
  noise and the existing POSIX macro false positive.

## 5. Risks found

- Prefix matching is exact-case by design; a firmware fork emitting
  nonstandard case would classify as `INFO`. That is fail-safe (an
  unrecognized line never acknowledges or fails a command) but would
  surface as a timeout in M7 rather than a parse error.
- Multi-hotend reports (`T0:`/`T1:`) are not decoded; the target printer
  is single-extruder. A multi-tool target would need parser extension.
- `payload` borrows the caller's line buffer; the future serial receive
  loop must not reuse its line buffer while a classified response is
  still being read. Documented in `marlin.h`.

## 6. Decisions taken

- Response classification table with a precomputed `terminal` flag; see
  design note §2.
- `echo:busy:` checked before `echo:` so busy keepalives are never
  demoted to echo.
- `Resend:` is terminal-unsupported for the MVP; recovery, line numbers
  and checksums stay in the post-MVP backlog.
- Unknown lines are `INFO`, never errors: classification cannot fail for
  text.
- `NO_MATCH` is a normal status for the typed parsers, so callers can
  probe informational lines cheaply while waiting for `ok`.
- M115 text fields are fixed 128-byte buffers with silent truncation
  (display-only data).

## 7. Next steps

Continue with Milestone 5: serial transport over POSIX pseudo-terminals
with configurable baud/timeout. That milestone connects this parser to
real received lines; no serial I/O was added here.
