---
title: Milestone 8 — Pause, Resume and Cancel
milestone: 8
status: done
date: 2026-07-10
tags: [printerctl, milestone, printer, session, signals, sd]
---

# Milestone 8 — Pause, Resume and Cancel

Source of truth: [[Agents]], [[architecture]], [[roadmap]]. Design
details: [[milestone-8-pause-resume-cancel|docs/design/milestone-8-pause-resume-cancel.md]].

## 1. Technical summary

Milestone 8 resolves architecture.md §13 item 1 — the MVP session
model — as **both**, through distinct mechanisms:

- **Host streaming sessions** are controlled in-process with signals:
  while `print FILE` streams, SIGUSR1 pauses after the current
  command's acknowledgement, SIGUSR2 resumes from the next unsent
  command, and SIGINT/SIGTERM cancels gracefully — the summary is still
  rendered (state `cancelled`) and the process exits with the new code
  9 (`PRINTERCTL_EXIT_PRINT_CANCELLED`). The printer module exposes
  this as an abstract control callback
  (`printerctl_printer_control_fn`, CONTINUE/PAUSE/CANCEL) polled only
  between acknowledged commands; the CLI owns the signal handling.
  Nothing is ever sent to the printer on a host-stream
  pause/resume/cancel (no parking, no heater changes — Agents.md safety
  policy), and a cancel is a non-error outcome
  (`PRINTERCTL_PRINTER_CANCELLED`, summary state
  `PRINTERCTL_PRINT_CANCELLED`, `state=print_cancelled` logged, no
  ERROR record).
- **SD prints** are controlled with the direct Marlin commands the
  roadmap names: `pause` → M25, `resume` → M24, `cancel` → M524, as
  `printerctl_printer_{pause,resume,cancel}_sd_print()` wrappers over
  the Milestone 6 exchange, rendered like `send` (`> M25` / `< ok`)
  plus a one-line report, logging `state=sd_*_requested`. Without a
  port they are configuration errors; under `--dry-run` they print the
  line they would send.

Supporting both safely required exclusive serial access:
`printerctl_serial_open()` now takes `TIOCEXCL` (released on close), so
a concurrent invocation against a busy port fails with EBUSY/exit 7
instead of interleaving bytes into an active exchange or stream.

## 2. Files changed

- `include/printerctl/printer.h` (control enum/callback,
  `PRINTERCTL_PRINTER_CANCELLED`, `PRINTERCTL_PRINT_CANCELLED`,
  extended `printerctl_printer_print_file()`, three SD-control
  functions)
- `src/printer/printer.c`
- `include/printerctl/serial.h`, `src/serial/serial.c` (TIOCEXCL /
  TIOCNXCL)
- `include/printerctl/cli.h` (exit code 9, contract update)
- `src/cli/cli.c` (signal handlers + control poll, cancelled summary,
  SD-control dispatch/rendering/dry-run, help)
- `tests/test_printer.c` (scripted-control pause/resume/cancel tests,
  SD-control tests)
- `tests/test_serial.c` (exclusive-open test)
- `tests/test_cli.c` (SD-control end-to-end and dry-run tests,
  forked-CLI signal tests with a delayed-reply mock, M1-era
  expectations updated: pause/resume/cancel without a port are now
  configuration errors, `home --i-understand-no-endstops` is the
  remaining unavailable command)
- `architecture.md` (§7.3 and §13 item 1 resolved)
- `README.md`
- `docs/design/milestone-8-pause-resume-cancel.md`
- `docs/milestones/milestone-8.md`

## 3. Acceptance validation

- Pause stops sending new commands after current acknowledgement: the
  control callback is polled only between acknowledged commands; a
  module test watches the mock's master while paused and fails on any
  transmitted byte; the CLI signal test observes `print paused` while
  acknowledgements are still pending.
- Resume continues from the next unsent command: the paused reader is
  untouched, so the module test completes 3/3 commands in order after a
  three-poll pause, and the CLI signal test finishes 4/4 with exit 0.
- Cancel stops streaming and records cancelled state: explicit
  `PRINTERCTL_PRINTER_CANCELLED` status, summary state cancelled,
  `state=print_cancelled` transition with the summary attached, exit
  code 9; asserted at module level (after an ack, before the first
  command, and while paused) and end-to-end via SIGINT.
- Marlin commands mapped where applicable: M25/M24/M524 asserted
  byte-exact on the mock's master; `Error:` replies fail with the
  printer-error mapping; unconnected sessions report NOT_CONNECTED.
- Direct Marlin command mode is documented: help text, README,
  architecture.md §7.3 and the design note state that
  `pause`/`resume`/`cancel` target SD prints and cannot reach a host
  stream (whose port is held exclusively).
- Cancellation closes the session cleanly: the graceful-stop path
  closes the G-code reader, renders the summary, and disconnect closes
  the port (TIOCNXCL + close); verified in the module and CLI tests.

## 4. Verification

- GCC build (`build-gcc`): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`; 7/7 test binaries pass.
- Clang build (`build-clang`): warning-free; 7/7 test binaries pass.
- Codex validation on 2026-07-10: GCC/Clang builds and CTest were
  rerun successfully, `clang-tidy` exited 0 with the existing
  non-blocking strict-style warning set, `git diff --check` was clean,
  no pending-work markers were present in project sources/docs, and
  `--dry-run pause` / `resume` / `cancel` reported M25/M24/M524 without
  opening serial.
- Real hardware (`/dev/ttyUSB0`, Marlin 2.1.2.1, 115200, no SD print
  active — all commands harmless, no motion/heat/extrusion):
  - `pause`: `//action:pause` then `ok`, exit 0 (the action line is
    correctly non-terminal).
  - `resume`: `//action:resume` then `ok`, exit 0.
  - `cancel`: six `echo:busy: processing` keepalives over ~13 s (more
    than double the 5 s timeout), a position report, then `ok`, exit 0
    — the timeout-bounds-silence semantics exercised for real.
  - Host stream of a harmless file (M105/G4 only): SIGUSR1 paused after
    the in-flight acknowledgement (`state=print_paused`), SIGUSR2
    resumed, 7/7 commands acknowledged, exit 0; a second run cancelled
    with SIGINT after 4 acknowledgements: exit 9, summary
    `print cancelled … 4 command(s) sent, 4 acknowledged, last
    acknowledged line 4, 59%`, `state=print_cancelled` logged.
  - Exclusivity: `printerctl temp` against the port while a print
    streamed failed with `cannot open '/dev/ttyUSB0': Device or
    resource busy`.

## 5. Risks found

- A paused host print keeps the nozzle hot wherever it stopped; long
  pauses ooze. Deliberately not automated away (no automatic motion or
  heater changes); the operator decides.
- Signal pairs arriving faster than the ~100 ms control poll (a resume
  immediately followed by a new pause) can resolve as a single
  continue. Harmless at human timescales.
- `pause`/`resume`/`cancel` on firmware without SD support are visible
  no-ops (`echo:Unknown command` rendered) but still exit 0 — Marlin
  acknowledges unknown commands with `ok`. Detecting the echo pattern
  is a possible refinement.
- A cancelled/interrupted print still leaves no persistent record
  (stdout/logs only) — exactly Milestone 9's scope.
- Exclusive open does not protect against CAP_SYS_ADMIN processes or
  non-tty access paths; it is a guard against accidental concurrent
  printerctl use, not a lock service.

## 6. Decisions taken

- §13 item 1 resolved as both models with distinct mechanisms; no
  daemon in the MVP. See design note §1.
- Host-stream controls are signals (SIGUSR1/SIGUSR2/SIGINT/SIGTERM);
  SIGINT is now a graceful cancel with a rendered summary instead of
  default termination. See design note §2.
- The printer module takes an abstract control callback polled between
  acknowledged commands; it knows nothing about signals. See design
  note §3.
- Cancelled is a distinct non-error outcome: status
  `PRINTERCTL_PRINTER_CANCELLED`, exit code 9, no ERROR record. See
  design note §4.
- The serial port is opened exclusively (TIOCEXCL) for every session.
  See design note §5.
- SD controls live in the printer module as M25/M24/M524 wrappers with
  `send` semantics. See design note §6.

## 7. Next steps

Continue with Milestone 9: fault tolerance and recovery records —
persist interrupted-session metadata (file, last acknowledged line,
progress, timestamp, state), define retry/reconnect limits, and keep
automatic resume forbidden.
