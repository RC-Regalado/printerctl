---
title: Milestone 6 Design - Single Marlin Commands
milestone: 6
status: done
date: 2026-07-07
tags: [printerctl, milestone, design, printer, marlin, serial]
---

# Milestone 6 Design — Single Marlin Commands

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]].

This note records the concrete design decisions behind the Milestone 6
implementation, ahead of the milestone report in
[[milestone-6|docs/milestones/milestone-6.md]].

## 1. Module split: the first composition

A seventh static library, matching `architecture.md` §4's `printer`
module boundary and §5's component diagram:

- `printerctl_printer` (`include/printerctl/printer.h`,
  `src/printer/printer.c`): the printer session. Links `serial`,
  `marlin`, `logging`, `gcode` (for the shared G28 detection) and
  `config` (for the connect signature). This is the first module that
  composes others; everything below it remains independently testable.
- `printerctl_cli` now links `printerctl_printer` and dispatches
  `temp`, `position`, `info` and `send` to it.

Its own CTest binary, `printerctl_printer_tests`, drives the session
against scripted pseudo-terminal mock printers.

## 2. The exchange loop (architecture.md §7.2)

`printerctl_printer_execute()` implements the single-command flow: write
one line, then read lines until a terminal classification:

- `ACK` → success; `ERROR` → `ERR_PRINTER_ERROR` with Marlin's message;
  `RESEND` → `ERR_RESEND` (explicit unsupported status, per the M4
  decision).
- `BUSY`, `ECHO` and `INFO` lines are consumed and the loop continues.
  Each `read_line` call gets the full configured timeout, so a printer
  emitting `busy:` keepalives every ~2 s never times out while it is
  still talking — the timeout bounds *silence*, not command duration.
- Every received line (terminal included) is passed to an optional
  caller callback, in arrival order. The callback borrows the session's
  line buffer for the duration of the call — the same borrowed-pointer
  discipline as `marlin.h`'s payload.

Typed queries (`query_temperature`, `query_position`, `query_info`) are
thin probes over this loop: they pass a fixed safe command (M105, M114,
M115) and try the matching `marlin` report parser on every line —
classifying first, so a report riding on the `ok` payload
(`ok T:...`) and a standalone report line before the `ok` both decode.
An exchange that ends in `ok` but never produced the expected report is
`ERR_UNEXPECTED_RESPONSE`, never a silent zero-filled success.

## 3. `send` safety policy (resolving architecture.md §13 item 5)

`printerctl_printer_send()` rejects **G28 only**, using the exact
detection the G-code reader applies to files —
`printerctl_gcode_command_is_unsafe_g28()`, newly exported from
`gcode.h` so there is one implementation of the rule, not two drifting
copies. The check runs before anything touches the wire, in the
execution path (as Milestone 2's design already required: the safety
check must live in code, not be inferred from logs). There is no bypass
flag through `send`; homing will get its own confirmed command.
The CLI performs the same preflight before opening the port, so
`send G28` fails with the safety code even if no `--port` is configured
and never toggles serial control lines on real hardware.

Everything else passes through, **including M112**: `send` is the
operator speaking, not the software acting automatically. Agents.md's
rule is "never send M112 automatically" — blocking the operator's
explicit emergency stop would remove the only manual kill switch the
CLI has. The policy holds under `--dry-run` too: `--dry-run send G28`
exits with the safety code without opening anything, exactly like
`--dry-run home` never confirms implicitly.

## 4. Dry-run for single commands

With `dry_run` set (flag or config file), `temp`/`position`/`info`/
`send` print the line they would transmit and exit 0 without a port
even being configured. Two layers enforce §8 rule 7: the CLI never
calls connect in dry-run mode, and `printerctl_printer_connect()`
itself refuses a `dry_run` config as an invalid argument — the last
line of defense if a future caller wires it wrong.

## 5. Exit codes 7 and 8

Two new operator-facing categories, completing the §9 taxonomy for
single commands:

- **7 (`PRINTERCTL_EXIT_COMM_ERROR`)** — the conversation could not
  happen: port cannot be opened/configured, I/O failure, disconnect,
  timeout. Remediation is cables/ports/config.
- **8 (`PRINTERCTL_EXIT_PRINTER_ERROR`)** — the conversation happened
  and the printer said no: `Error:`, `Resend:`, or an `ok` without the
  expected report. Remediation is printer state.

`ERR_NO_PORT` maps to the existing exit 3 (configuration error): a
missing port is a configuration problem, with a message telling the
operator to pass `--port`.

## 6. TX/RX logging and the Milestone 2 privacy rules

Every transmitted line is logged via `printerctl_log_serial_tx` and
every received line via `printerctl_log_serial_rx` — both DEBUG, so the
full exchange is visible under `--verbose`. This is the purpose the M2
taxonomy built those events for; it coexists with the M2 privacy rule
about `command_parsed`/`command_unavailable` (which still never log
`send`'s text — those events describe CLI intent, `serial_tx` describes
the wire). Connect/disconnect record INFO `state_transition` events
carrying baud and timeout but never the port path; a failed exchange
records one generic ERROR (`command=temp status=timeout`) while the
human-readable message keeps the details (which may embed the path).
Logging inside the session is best-effort and never gates the exchange.

## 7. Scriptable mock Marlin tests

Two levels, both over pseudo-terminals:

- **Module level** (`tests/test_printer.c`): the test owns the master
  and scripts replies after connect (the serial open flushes stale
  input by design, so pre-scripted bytes would be discarded). The
  session's synchronous reads consume the pty-buffered script.
- **CLI level** (`tests/test_cli.c`): a forked child services the
  master — waits for one complete command line, writes the scripted
  reply, then drains until killed. This is required because
  `printerctl_cli_run()` opens (and flushes) the port itself, so the
  reply cannot exist before the flush; the child makes the mock
  genuinely reactive, per `architecture.md` §10's "scriptable so tests
  can define response sequences per transmitted line".

## 8. Deferred

- `home` execution (G28 with `--i-understand-no-endstops`): still exit
  5; the confirmed-homing path belongs with the session work in M7/M8.
- Streaming (`print` without dry-run): M7, reusing
  `printerctl_printer_execute()`'s loop shape per command.
- Reconnect policy: M9 (`ERR_SERIAL` vs timeout is already split for
  it).
- A broader hardware soak: initial contact on `/dev/ttyUSB0` passed for
  M105/M115/M114, but M6 intentionally does not exercise movement,
  homing, heating or long-running exchanges. If a future board hits a
  DTR-reset race, connect can grow an explicit "wait for start/banner
  quiet period".
