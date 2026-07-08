---
title: Milestone 5 — Serial Transport
milestone: 5
status: done
date: 2026-07-07
tags: [printerctl, milestone, serial]
---

# Milestone 5 — Serial Transport

Source of truth: [[Agents]], [[architecture]], [[roadmap]]. Design
details: [[milestone-5-serial|docs/design/milestone-5-serial.md]].

## 1. Technical summary

Milestone 5 adds a standalone `serial` module implementing Linux USB
serial communication with safe, explicit timeouts.
`printerctl_serial_open()` opens a terminal device, configures raw 8N1
suitable for Marlin (no echo, no canonical input, no CR/LF translation,
no software flow control, `CLOCAL`, receiver enabled), applies one of
the standard baud rates to both directions, verifies the configuration
by reading it back, and flushes stale bytes.
`printerctl_serial_write_line()` transmits exactly one command line
terminated by a single `\n`, handling partial writes under a deadline.
`printerctl_serial_read_line()` returns one received line with `\n` and
`\r\n` endings normalized. All I/O is non-blocking under `poll()`
against a `CLOCK_MONOTONIC` per-call deadline; nothing ever blocks
indefinitely. Timeouts (`ERR_TIMEOUT`), disconnects
(`ERR_DISCONNECTED`: hangup, EOF, `EIO`/`ENXIO`/`ENODEV`) and other I/O
failures (`ERR_IO`) are distinct, explicit statuses.

The module has no dependency on any other printerctl module and nothing
links it yet: Milestone 6 composes `serial` + `marlin` + `logging` into
the first real commands. Dry-run safety is enforced one layer up — the
CLI's dry-run path never constructs a serial port, and `printerctl_cli`
does not link `printerctl_serial`.

## 2. Files changed

- `CMakeLists.txt`
- `include/printerctl/serial.h`
- `src/serial/serial.c`
- `tests/CMakeLists.txt`
- `tests/test_serial.c`
- `README.md`
- `docs/design/milestone-5-serial.md`
- `docs/milestones/milestone-5.md`

## 3. Acceptance validation

- Serial module communicates with a pseudo-terminal mock:
  `printerctl_serial_tests` owns the pty master as the "printer" and
  the module opens the slave path; exact TX bytes, RX line
  normalization, bursts and empty lines are asserted.
- Timeout reported explicitly: read silence and a never-drained 1 MiB
  write both return `ERR_TIMEOUT`, with the read case bounded by wall
  clock against the configured 200 ms.
- I/O errors reported explicitly: a vanished device (master closed)
  reports `ERR_DISCONNECTED` on read and fails explicitly on write,
  never misreported as a timeout; open failures distinguish
  `OPEN_FAILED` (missing device) from `CONFIG_FAILED` (not a terminal).
- No writes in dry-run mode: the dry-run code path (M3) never opens a
  port, and the CLI does not link this module at all; the existing CLI
  dry-run tests continue to pass unchanged.
- Tests cover open failure, timeout and normal line exchange, plus
  disconnects, overlong lines, unsupported bauds and the
  invalid-argument/lifecycle matrix.

## 4. Verification

- GCC build (`build-gcc`): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`.
- Clang build (`build-clang`): warning-free with the same flags.
- CTest: 6/6 test binaries pass on both compilers
  (`printerctl_serial_tests` is new; the other five are unchanged and
  still pass).
- `clang-tidy`: executed with the Milestone 2 analyzer exclusions; it
  exits successfully. Remaining findings are readability/test-harness
  noise, existing feature-test macro false positives, and expected
  literal baud-rate constants in the serial transport.

## 5. Risks found

- Marlin's nonstandard 250000 baud is rejected
  (`ERR_UNSUPPORTED_BAUD`): supporting it needs `termios2`/`BOTHER`.
  The target printer (EasyThreed K9) runs 115200, so this is a
  documented limitation, not a defect; revisit only if a real 250000
  target appears.
- Pseudo-terminals validate line discipline and timing but not
  USB-specific behavior (latency timers, kernel driver quirks). First
  contact with real hardware happens in M6 and may surface
  device-specific issues.
- `write_line` timing out mid-line leaves a partially transmitted
  command on the wire; the header documents that callers must treat the
  exchange as failed and never blindly retry, which the M7 state
  machine must respect.

## 6. Decisions taken

- Stack-value port struct with `printerctl_` naming, deviating from
  `architecture.md` §6.4's opaque-heap-handle sketch to keep the
  project-wide no-heap discipline; see design note §2.
- Raw mode spelled out flag by flag and verified by readback rather
  than trusting `tcsetattr()`'s any-change-counts success; see design
  note §3.
- One `poll()`-based `CLOCK_MONOTONIC` deadline per call; `VMIN`/`VTIME`
  zeroed so termios adds no second timeout layer; see design note §4.
- `\n` as the sole TX terminator; embedded `\n`/`\r` in a command line
  rejected as invalid arguments; RX strips trailing `\r`; see design
  note §5.
- `ERR_DISCONNECTED` split from `ERR_IO` so the M9 reconnect policy can
  distinguish a vanished printer from a kernel failure; see design
  note §6.
- Dry-run enforcement stays in the CLI layer, not the transport; see
  design note §7.
- `_DEFAULT_SOURCE` in `serial.c` for the Linux-only high baud
  constants, documented in the file header; see design note §8.

## 7. Next steps

Continue with Milestone 6: single Marlin commands (`temp` via M105,
`position` via M114, `info` via M115, `send` with acknowledgement
handling), composing `serial` + `marlin` + `logging` with TX/RX logging
of every line, plus the first scriptable mock-Marlin integration tests.
