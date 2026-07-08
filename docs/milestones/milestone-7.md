---
title: Milestone 7 — Reliable G-code Streaming MVP
milestone: 7
status: done
date: 2026-07-07
tags: [printerctl, milestone, printer, streaming, gcode]
---

# Milestone 7 — Reliable G-code Streaming MVP

Source of truth: [[Agents]], [[architecture]], [[roadmap]]. Design
details: [[milestone-7-streaming|docs/design/milestone-7-streaming.md]].

## 1. Technical summary

Milestone 7 turns `print FILE` into a real streaming command:
`printerctl_printer_print_file()` (in the existing `printer` module)
composes the M3 G-code reader with the M6 exchange loop to send one
normalized command at a time, never reading the next command from the
file until the previous one was acknowledged with `ok` — a structural
no-flooding guarantee. `busy:`/`echo:`/informational lines are waited
out (the timeout bounds silence, not command duration); `Error:`,
`Resend:`, timeout, serial failure and G-code file errors (including a
G28 line, blocked before transmission) all stop the print gracefully:
nothing further is sent, the file is closed, and a fully filled
summary — final state, commands sent/acknowledged, last acknowledged
source line, byte progress — is returned, rendered on stdout (with the
file name) and logged (without it).

Progress is reported per acknowledged command: stdout renders one line
per whole-percent change, the log gets a DEBUG `progress` record per
acknowledgement plus an INFO record per percent change, and
`state_transition` records mark `state=printing` /
`state=print_completed` / `state=print_failed` with the summary
attached. `print` without a configured port is now a configuration
error (exit 3); the other exit codes follow the established mapping
(6 file error, 4 G28, 7 communication, 8 printer).

## 2. Files changed

- `include/printerctl/printer.h` (print summary, progress callback,
  `printerctl_printer_print_file()`, `PRINTERCTL_PRINTER_ERR_GCODE`)
- `src/printer/printer.c`
- `include/printerctl/cli.h` (contract update)
- `src/cli/cli.c` (print dispatch, progress/summary rendering, help)
- `tests/test_printer.c` (seven streaming tests, incl. the strict
  no-flooding mock)
- `tests/test_cli.c` (scripted multi-reply mock, four end-to-end print
  tests, two M1-era expectations updated)
- `README.md`
- `docs/design/milestone-7-streaming.md`
- `docs/milestones/milestone-7.md`

## 3. Acceptance validation

- Mock printer verifies no serial flooding: a strict forked mock polls
  for 150 ms after each received command line and fails if any byte
  arrives before its `ok`; the 4-command print passes with the child
  exiting 0.
- Progress updates as commands are acknowledged: per-ack progress
  callback asserted (3 calls, monotonic, ending at 100%), stdout
  progress lines and INFO/DEBUG `progress` records asserted end-to-end.
- Timeout stops print gracefully: acknowledgement silence yields
  `ERR_TIMEOUT`/exit 7 with a summary showing 2 sent / 1 acknowledged.
- `Error:` stops print gracefully: `ERR_PRINTER_ERROR`/exit 8, the
  third command never transmitted (asserted by reading the mock's
  master), Marlin's message carried to the operator.
- `Resend:` stops with explicit unsupported status: `ERR_RESEND`.
- Session summary includes final state, file and last acknowledged
  line: asserted in the module summary struct, the stdout summary line
  and the `state=print_*` log records.
- Additionally: a G28 line stops the print before transmission (only
  the preceding command went out), malformed/unopenable files map to
  `ERR_GCODE`/exit 6, busy keepalives are waited out, and an empty file
  completes with zero commands.

## 4. Verification

- GCC build (`build-gcc`): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`.
- Clang build (`build-clang`): warning-free with the same flags.
- CTest: 7/7 test binaries pass on both compilers.
- `clang-tidy` (`build-tidy`): exits 0; current output remains the
  historical strict-style warning set already present in earlier
  milestones, with no blocking analyzer failure.
- Real hardware smoke test against `/dev/ttyUSB0` (Marlin 2.1.2.1,
  115200 baud), streaming a file of harmless commands only
  (`M105`, `G4 P250`, `M114`, `M115`, `M105` — no motion, no heat, no
  extrusion, because the machine has only a Z-min endstop):
  - Exit 0; 5 commands sent, 5 acknowledged, last acknowledged line 7,
    100%.
  - Each command waited for its `ok`: M115's 37 `Cap:` informational
    lines were consumed without confusing the loop, and `G4 P250`'s
    acknowledgement arrived 253 ms after transmission without timeout
    or early transmission of the next command.
  - The log shows the full lifecycle: `state=connected` →
    `state=printing` → per-command `serial_tx`/`serial_rx` and
    `progress` → `state=print_completed ... percent=100` →
    `state=disconnected`.

## 5. Risks found

- The MVP session is in-process only: closing the terminal kills the
  print. Pause/resume/cancel across invocations is exactly the M8
  question (architecture.md §13 item 1, still open).
- A print interrupted by failure leaves no persistent record; the
  summary lives in stdout/logs only. Interrupted-session records are
  M9's scope.
- The no-flooding design costs one round-trip per command; at 115200
  baud with Marlin's command buffer this is the safe MVP trade
  (roadmap backlog items 1–3 — line numbers, checksums, resend — are
  the future throughput path).
- Motion G-code remains deliberately unvalidated on hardware; see
  design note §8.

## 6. Decisions taken

- Streaming lives in the `printer` module as
  `printerctl_printer_print_file()`; no new library. See design note §1.
- MVP state machine is control-flow plus observable
  `state_transition` records and an explicit final-state enum; richer
  states arrive with M8. See design note §2.
- Strictly one command in flight; the next command is not read from
  the file before the previous `ok`. See design note §3.
- One graceful-stop path for every failure origin, with file-origin
  failures mapped to `PRINTERCTL_PRINTER_ERR_GCODE` → exit 6 (G28 →
  exit 4), transport/printer failures keeping the M6 mapping. See
  design note §4.
- Progress rendered per percent change on stdout, logged per-ack at
  DEBUG and per percent change at INFO. See design note §5.
- `print` without a port is a configuration error before the file is
  touched. See design note §7.

## 7. Next steps

Continue with Milestone 8: pause, resume and cancel. That milestone
must first resolve `architecture.md` §13 item 1 (in-process session
controls vs. direct Marlin commands vs. both) — the MVP session model
decision that shapes whether `pause`/`resume`/`cancel` act on a live
in-process print or send `M25`/`M24`/`M524` to a printer running an SD
print.
