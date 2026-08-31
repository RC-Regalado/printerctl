---
title: Milestone 17 — Lifecycle, Fault & Compatibility Validation Design
milestone: 17
status: done
date: 2026-08-25
tags: [printerctl, daemon, lifecycle, fault, apcupsd, phase-1]
---

# Milestone 17 — Lifecycle, Fault & Compatibility Validation Design

Source of truth: [[Agents]], [[docs/architecture|architecture]],
[[docs/roadmap|roadmap]].

M17 hardens the M13-M16 daemon path enough to close Phase 1.

## Changes made

- `printerctld` handles `SIGPWR` by sending an internal power-failure
  cancel command to the worker.
- The socket transport has an interruptible `serve_one()` variant so
  `SIGTERM`/`SIGINT`/`SIGPWR` can break an idle client session.
- Socket writes use `MSG_NOSIGNAL`, so a client-side disconnect during a
  response does not terminate the daemon with `SIGPIPE`.
- Worker power-failure cancellation remains distinguishable in the runtime
  snapshot via `PRINTERCTL_PLP1_SHUTTING_DOWN`.
- The worker also observes process-signal cancellation flags, reset at
  runtime startup, to close the race where `SIGTERM`/`SIGPWR` was received
  after a command ACK but before the main loop had written the matching
  internal command.
- Cancellation now commits through a `print_engine` TX authorization gate.
  `PRINT_CANCEL`, `SIGTERM` and `SIGPWR` all close the same gate before
  reporting committed cancellation, and `printer` polls control while waiting
  for ACK so a held-ACK window cannot authorize the next G-code command after
  cancellation has committed.

## Validated behavior

- idle daemon shutdown;
- shutdown while a print is active;
- power-failure cancellation while a print is active;
- active-print marker targets the `printerctld` process during daemonized
  printing;
- `SIGPWR` with no active print is a safe no-op;
- client disconnect after print acceptance does not cancel the print;
- a second client can reconnect and observe the daemon-owned active print
  through `STATUS`;
- `SIGTERM` interrupts an idle connected client;
- Marlin `Error:` terminates the daemon-owned print as failed;
- `Resend:` remains terminal/unsupported;
- response timeout terminates the daemon-owned print as failed;
- legacy serial open is rejected while the daemon owns the serial device.
- daemon `PRINT_START` fails cleanly with `SERIAL_ERROR` while legacy
  `printerctl` owns the serial device.
- ACK-window cancellation for `SIGTERM`, `SIGPWR` and PLP `PRINT_CANCEL`:
  command N may finish its already-started write/ACK, but command N+1 is not
  authorized after cancellation commit.

No automatic homing, movement, heating, extrusion, emergency stop, resume,
queueing, durable job persistence, auto-reconnect or worker restart was
added.

## Remaining deliberate limit

The transport remains single-client-at-a-time. This is acceptable for
Phase 1 validation but should be revisited before UI/web automation relies
on concurrent monitoring during a long-lived client session.

## R6 cancellation/TX barrier review answers

1. TX is authorized when `printerctl_printer_print_file_controlled()` receives
   `CONTINUE` from `print_engine`'s `tx_gate_begin()` callback.
2. Cancellation is committed when `printerctl_print_engine_commit_cancellation()`
   or the engine control path closes `tx_gate_closed` under `tx_gate`.
3. The `pthread_mutex_t tx_gate` serializes commit with TX authorization and the
   start of `printerctl_serial_write_line()`.
4. If cancellation arrives after a serial write already began, that command may
   finish; the commit waits for the gate and then blocks later commands.
5. If cancellation arrives while waiting for ACK, the serial wait hook polls
   control, commits cancellation, and continues waiting for the in-flight ACK.
6. If cancellation arrives after ACK but before the next TX, the next control
   poll or `tx_gate_begin()` closes/rejects before writing.
7. `PRINT_CANCEL`, `SIGTERM` and `SIGPWR` use the same commit/gate primitive.
8. Host-print streaming G-code writes go through `printerctl_printer_print_file_controlled()`
   and the gate; explicit operator `send`/SD controls remain separate direct
   commands by design.
9. Signal handlers still only record intent with `sig_atomic_t`/atomic stores.
10. No new lock inversion was introduced: the TX gate is held only across the
    bounded serial write, never across ACK wait, pipe waits, socket I/O, logging
    or event emission.
