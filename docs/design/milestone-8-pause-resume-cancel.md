---
title: Milestone 8 Design - Pause, Resume and Cancel
milestone: 8
status: done
date: 2026-07-10
tags: [printerctl, milestone, design, printer, session, signals]
---

# Milestone 8 Design — Pause, Resume and Cancel

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

This note records the concrete design decisions behind the Milestone 8
implementation, ahead of the milestone report in
[[milestone-8|docs/milestones/milestone-8.md]].

## 1. The session model (architecture.md §13 item 1): both, through distinct mechanisms

The open decision was whether MVP pause/resume/cancel targets direct
Marlin commands, local streaming sessions, or both. The answer is both,
because the two cases are physically different and neither mechanism
can serve the other:

- **A host streaming session** lives inside one `printerctl print`
  process. The MVP has no daemon (roadmap backlog item 4), so a second
  CLI invocation cannot reach that session's state — and it cannot even
  reach its serial port, which is held exclusively (§5). The only
  operator channel into a running CLI process is POSIX signals, which
  is also the channel operators already use instinctively (Ctrl+C).
- **An SD print** runs on the printer itself; the host holds no session
  state at all. There, pause/resume/cancel genuinely are one Marlin
  command each — M25, M24, M524 — with exactly the `send`-style
  exchange semantics Milestone 6 built.

Conflating the two (e.g. making `printerctl pause` "find" a host
stream) would require the daemon this MVP deliberately does not have.

## 2. Host-stream controls are signals

While `print FILE` streams:

| Signal | Effect |
|---|---|
| `SIGUSR1` | pause after the current command's acknowledgement |
| `SIGUSR2` | resume from the next unsent command |
| `SIGINT` / `SIGTERM` | cancel gracefully (summary rendered, exit code 9) |

The handlers are installed for the duration of `run_print_command()`
only and restored afterwards; they do nothing but set `sig_atomic_t`
flags — the sole mutable file-scope state in the CLI, reset on every
entry. A control callback (`print_control_poll()`) turns the flags into
decisions and renders the `print paused` / `print resumed` edges on
stdout. Making SIGINT a *graceful* cancel is itself a safety feature:
the previous behavior (default termination) killed the process mid-
exchange with no summary; now the operator always learns exactly how
far the print got. The serial module already restarts every poll/read/
write on EINTR (Milestone 5), so a signal arriving mid-exchange never
corrupts the wire protocol.

## 3. The module boundary: a control callback, not signal handling

`printerctl_printer_print_file()` gained `on_control`/`control_context`
(and, for symmetry, its progress `context` was renamed
`progress_context`). The printer module knows nothing about signals —
it polls an abstract CONTINUE/PAUSE/CANCEL decision:

- Consulted **before each command is read from the file**, i.e. only
  between acknowledged commands, never mid-exchange. This makes the
  roadmap's "pause stops sending new commands after current
  acknowledgement" a structural property, identical in shape to the
  Milestone 7 one-in-flight guarantee.
- While PAUSE, the callback is re-polled every ~100 ms and nothing is
  transmitted; the reader is untouched, so resume continues from the
  next unsent command by construction.
- CANCEL (honored streaming or paused) takes the same graceful-stop
  path as every Milestone 7 failure — nothing further sent, file
  closed, summary fully filled — but with its own non-error status
  `PRINTERCTL_PRINTER_CANCELLED`, summary state
  `PRINTERCTL_PRINT_CANCELLED` and a `state=print_cancelled` log
  transition.

Nothing is sent to the printer on pause, resume or cancel of a host
stream: no parking move, no heater change, no M112. The Agents.md
safety policy forbids automatic motion/heat, and on this printer (no
X/Y endstops) any "smart" park would be exactly the kind of blind move
the policy exists to prevent. Marlin drains what it already
acknowledged and idles. The design note risk: a long pause leaves the
nozzle hot wherever it stopped (oozing); that is the operator's call,
documented, not automated away.

## 4. Cancelled is not failed

A cancel is the operator's own request carried out. It gets a distinct
exit code (9, `PRINTERCTL_EXIT_PRINT_CANCELLED`) so scripts can
distinguish completed (0) / cancelled (9) / failed (6/7/8); the CLI
renders the summary with state `cancelled` but writes no error report
and no ERROR log record — the INFO `state=print_cancelled` transition
(with the summary attached) is the log's record of the event.

## 5. Exclusive serial open (TIOCEXCL)

Milestone 8 is the milestone that invites an operator to run a second
printerctl against a busy port ("`printerctl pause` while it prints").
For a host stream that second invocation would be corruption, not
control: two readers splitting Marlin's replies, a stray command
interleaved into the acknowledged stream. `printerctl_serial_open()`
now takes `TIOCEXCL` on the descriptor (released on close), so the
second invocation fails cleanly — `cannot open '/dev/ttyUSB0': Device
or resource busy`, exit 7 — instead of silently racing. Verified on
real hardware and by a serial-module test. Kernel caveat, documented in
serial.h: CAP_SYS_ADMIN processes bypass exclusive mode.

## 6. SD controls in the printer module

`printerctl_printer_pause_sd_print()` / `..._resume_sd_print()` /
`..._cancel_sd_print()` are thin wrappers over the Milestone 6 exchange
(`printerctl_printer_execute()`), so the mapping lives in the printer
module (architecture.md §6.7's table), not in the CLI. A successful
exchange logs `state=sd_pause_requested` / `sd_resume_requested` /
`sd_cancel_requested`. They are explicit operator commands — the safety
policy's "no automatic motion" is about the software acting alone, and
M24 resuming an SD print is the operator speaking, same class as
`send`. Firmware without SD support answers `echo:Unknown command`
before its `ok`; the echo line is rendered to the operator (`< echo:…`)
exactly as `send` renders it, so a no-op control is visible. Dry-run
prints the `would send 'M25'` line through the existing single-command
path, without touching any device.

## 7. Deferred

- Interrupted-session records (a cancelled print leaves only
  stdout/logs): Milestone 9.
- Pause-time keepalives (e.g. periodic M105) or heater management while
  paused: out of MVP scope, and against the no-automatic-commands
  policy as currently written.
- A daemon so `printerctl pause` can reach a host stream: backlog
  item 4, explicitly out of the MVP (§13 item 6 remains open).
