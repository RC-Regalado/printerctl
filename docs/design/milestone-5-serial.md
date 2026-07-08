---
title: Milestone 5 Design - Serial Transport
milestone: 5
status: done
date: 2026-07-07
tags: [printerctl, milestone, design, serial]
---

# Milestone 5 Design — Serial Transport

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

This note records the concrete design decisions behind the Milestone 5
implementation, ahead of the milestone report in
[[milestone-5|docs/milestones/milestone-5.md]].

## 1. Module split

A sixth independent static library, matching `architecture.md` §4's
`serial` module boundary:

- `printerctl_serial` (`include/printerctl/serial.h`,
  `src/serial/serial.c`): open/configure/read-line/write-line/close over
  a POSIX terminal device. No dependency on any other printerctl module
  — it knows nothing about Marlin semantics (`marlin.h`'s job), G-code,
  configuration or logging (callers log TX/RX themselves, Milestone 6).
- Nothing links it yet. Milestone 6 (single Marlin commands) composes
  `serial` + `marlin` + `logging` for the first time.

Its own CTest binary, `printerctl_serial_tests`, exercises the module
against pseudo-terminals only — no real hardware, per `Agents.md`'s
testing policy.

## 2. API shape: stack value, not opaque heap handle

`architecture.md` §6.4 sketched `serial_open(serial_port_t **out, ...)`
with an opaque heap-allocated handle. The implementation deviates
deliberately: `printerctl_serial_port_t` is a plain stack value
(`{fd, open, baud, timeout_ms}`), matching the no-heap discipline every
other module already follows (`gcode`'s reader, `logging`'s logger).
The struct is small and its fields are documented as read-only for
callers; nothing about the transport needs hidden state. Function names
carry the `printerctl_` prefix like every other module, rather than the
sketch's bare `serial_*`.

## 3. Raw mode, spelled out

The port is configured for Marlin as raw 8N1, with every flag written
explicitly rather than via `cfmakeraw()` so the configuration is
visible and verifiable:

- input: no break/parity tricks, no strip, **no CR/LF translation, no
  XON/XOFF** (G-code bytes must never be altered or flow-controlled in
  software);
- output: no post-processing (`OPOST` off — `\n` must go out as `\n`);
- local: no echo, no canonical buffering, no signal characters;
- control: 8 data bits, no parity, one stop bit, receiver enabled,
  `CLOCAL` (a USB CDC device has no meaningful modem-control lines).

`tcsetattr()` succeeds if *any* change applied, so the configuration is
read back and the critical parts (both speeds, ICANON/ECHO off) are
verified; a partial application is `ERR_CONFIG_FAILED`, not a silent
degradation. Both directions are flushed after configuration so a
session never begins by reading bytes that predate the open.

## 4. Timeouts: one deadline per call, poll-based

The descriptor stays `O_NONBLOCK` for its whole life; all timing lives
in `poll()` loops against a `CLOCK_MONOTONIC` deadline computed at the
start of each call. `VMIN`/`VTIME` are zeroed so termios never adds a
second, conflicting timeout layer. `read_line` has the whole timeout to
produce one complete line; `write_line` has the whole timeout to flush
the line through partial writes. `EINTR` restarts the wait with the
remaining time. A timeout is always the explicit
`PRINTERCTL_SERIAL_ERR_TIMEOUT`, never a silent retry — the M7
streaming loop turns it into a graceful stop.

## 5. Line semantics

- **TX**: `write_line(port, "M105")` transmits exactly `M105\n`. The
  line must not contain embedded `\n`/`\r` (INVALID_ARGUMENT), so one
  call is always exactly one Marlin command; the G-code reader already
  guarantees this for streamed files. `\n` alone is the terminator —
  Marlin accepts it, and `\r\n` would just cost a byte per command.
- **RX**: `read_line` consumes through the next `\n` (not stored) and
  strips trailing `\r`, so Marlin's `\n` and `\r\n` endings read
  identically. Bytes are read one at a time after each poll — at
  115200 baud the syscall cost is irrelevant and the code stays free of
  a second buffering layer whose contents could survive a reopen.
- The caller owns the line buffer and its size; a line that does not
  fit is `ERR_LINE_TOO_LONG` with the NUL-terminated prefix preserved
  for diagnostics.

## 6. Explicit failure taxonomy

Ten statuses; the ones that matter operationally:

- `ERR_TIMEOUT` — deadline expired (silence, or a never-drained write).
- `ERR_DISCONNECTED` — the device went away: poll hangup, `read() == 0`,
  or `EIO`/`ENXIO`/`ENODEV` (a pseudo-terminal whose peer closed and an
  unplugged USB device both land here). Distinct from `ERR_IO` so the
  future reconnect policy (M9) can tell "printer vanished" from "kernel
  I/O error".
- `ERR_UNSUPPORTED_BAUD` — the rate has no `Bxxx` constant. Notably
  Marlin's nonstandard 250000: supporting it needs `termios2`/`BOTHER`,
  and the target printer runs 115200, so it is rejected clearly instead
  of half-supported. The config module's 1–4000000 range check remains
  the CLI-facing validation; this is the transport's own, stricter
  gate, applied before the device is touched.
- `ERR_OPEN_FAILED` vs `ERR_CONFIG_FAILED` — "could not open the path"
  vs "opened but it is not a terminal / raw mode did not apply".

## 7. Dry-run enforcement stays out of the transport

`architecture.md` §8 rule 7 ("dry-run must never open or write to a
serial device") is enforced one layer up: the CLI's dry-run path never
constructs a `printerctl_serial_port_t` at all (Milestone 3), and
`printerctl_cli` does not even link `printerctl_serial` yet. Putting a
`dry_run` flag inside the transport would make every caller carry a
mode the transport itself should never see.

## 8. `_DEFAULT_SOURCE`, not `_POSIX_C_SOURCE`

`serial.c` needs `cfsetispeed`/`cfsetospeed` with the high Linux baud
constants (`B460800`+), which POSIX does not define. The project is
Linux-only (`Agents.md`), so `_DEFAULT_SOURCE` is used and documented in
the file header — the same "POSIX-plus where the platform demands it"
pattern `logging.c` set with `_POSIX_C_SOURCE 200809L`.

## 9. Testing on pseudo-terminals

`tests/test_serial.c` drives the module through
`posix_openpt`/`grantpt`/`unlockpt`/`ptsname`: the test owns the master
side (the "printer") and the module opens the slave path exactly as it
would open `/dev/ttyACM0`. Covered: raw-mode flags and speeds verified
via `tcgetattr` on the configured port; exact TX bytes (`M105\n`, no
echo, nothing extra); RX with `\n`, `\r\n`, burst-of-two-lines, and
empty-line cases; read timeout bounded by wall clock (150 ms–2 s window
around the configured 200 ms) with partial bytes preserved; write
timeout via a 1 MiB line nobody drains; disconnect (master closed) on
both read and write paths; buffer-overflow rejection; open failures
(missing path, `/dev/null` as non-terminal, unsupported bauds); and the
full invalid-argument/lifecycle matrix (NOT_OPEN before open and after
close, idempotent close, embedded line endings rejected).

## 10. Deferred

- Wiring into commands and TX/RX logging: M6.
- Reconnect policy and attempt logging: M9.
- 250000 baud via `termios2`/`BOTHER`: only if a real target needs it.
- Scriptable mock-Marlin server: M6, where response sequencing starts
  to matter (`architecture.md` §10).
