---
title: Testing
tags: [printerctl, testing, reference]
---

# Testing

Source of truth: [[Agents]], [[Kernel/Architecture]] §10. This document is the
concrete reference the roadmap's Milestone 11 requires: what each test
binary covers, the patterns behind them, the hardware-validation policy,
and how to run everything from a clean checkout.

## Philosophy

Primary tests must never require printer motion. Every module is
testable against pseudo-terminals, scripted mock printers or forked
`printerctl` processes; real hardware is used only for a final,
harmless-commands-only confirmation pass per milestone, never as the
primary verification method.

## Test binaries

Eight independent CMake/ctest targets, one per module plus the CLI
integration suite:

| Binary | Covers |
|---|---|
| `printerctl_tests` | CLI grammar, configuration precedence, logging integration, and end-to-end command execution against forked mock printers (`tests/test_cli.c`) |
| `printerctl_config_tests` | Configuration resolution: defaults, `--config` file, CLI overrides, precedence, validation |
| `printerctl_gcode_tests` | G-code file reading: normalization, comment/blank-line stripping, progress, malformed/oversized lines, `G28` detection |
| `printerctl_marlin_tests` | Marlin response classification and parsing: `ok`, `Error:`, `busy:`, `echo:`, `Resend:`, temperature/position/firmware-info reports |
| `printerctl_serial_tests` | Serial transport over pseudo-terminals: open/configure/read/write, timeout, exclusive access (`TIOCEXCL`) |
| `printerctl_printer_tests` | Printer session orchestration against scripted mock printers: connect, single-command exchange, streaming, pause/resume/cancel (operator and power-failure), descriptor-leak checks on failure paths |
| `printerctl_record_tests` | Session records and the active-print marker: state-dir resolution, record content, exclusive creation, failure modes, PID marker round-trip |
| `printerctl_logging_tests` | Logging module: formatting, level filtering |

Run all of them:

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=gcc
cmake --build build
ctest --test-dir build --output-on-failure
```

Substitute `-DCMAKE_C_COMPILER=clang` (and a different build directory)
to run the same suite under Clang; both must be warning-free with
`-Wall -Wextra -Wpedantic` per the global Definition of Done.

## Test patterns

- **Pseudo-terminal mock printer** (`posix_openpt`/`grantpt`/`unlockpt`):
  the serial and printer test suites open a pty pair, hand the slave
  path to the code under test as `--port`, and script replies on the
  master side. This is the primary substitute for real hardware.
- **Scripted control callback** (`control_script_t` in
  `tests/test_printer.c`): a fixed sequence of
  `printerctl_print_control_t` decisions (`CONTINUE`/`PAUSE`/`CANCEL`/
  `CANCEL_POWER_FAILURE`), one per poll, used to drive
  `printerctl_printer_print_file()` through pause/resume/cancel
  transitions deterministically, including a watch on the mock's master
  side for any stray transmission during a pause.
- **Forked CLI process with signals** (`cli_print_process_t` in
  `tests/test_cli.c`): forks a child that runs `printerctl_cli_run()`
  directly (no `exec`) with captured stdout, so the parent can poll for
  output and drive the child with real `kill()` signals
  (`SIGUSR1`/`SIGUSR2`/`SIGINT`/`SIGPWR`) — the same way an operator or
  the APCUPSD script would control a real running process. Because this
  forks the test binary itself rather than executing the built
  `printerctl` binary, `/proc/<pid>/comm` for the child is the test
  binary's own name, not `printerctl` — a constraint that shaped the
  Milestone 10 low-battery design (see
  [[docs/design/milestone-10-low-battery-cancellation]] §6).
- **Disposable `XDG_STATE_HOME`**: the whole CLI suite runs against a
  temporary state directory created in `main()` and removed at exit, so
  no test run ever touches the developer's real
  `~/.local/state/printerctl`. Record-specific tests additionally swap
  in their own temporary directory for the duration of a single test.
- **Descriptor-leak proof** (`count_open_fds()` via `/proc/self/fd`,
  Linux-only, self-skips if `/proc` is unavailable): runs full
  connect/print/disconnect cycles through multiple failure paths and
  asserts the process ends with exactly the file-descriptor count it
  started with.
- **No-auto-resume proof**: an end-to-end test interrupts a print
  (leaving a session record), reruns the identical file, and asserts
  the whole file streams again from the first command — proving the
  record's presence changes nothing, rather than only asserting no
  read-path exists by inspection.

## Hardware validation

Real hardware is a confirmation pass, not the primary test method, and
is limited to commands that cannot cause harm even if something is
misconfigured — see [[docs/safety]] §9. Confirmed so far, on
`/dev/ttyUSB0` (Marlin 2.1.2.1, 115200 baud, EasyThreed K9 / STM32):

| Milestone | Confirmed on real hardware |
|---|---|
| 6 | `temp` (`M105`), rendering hotend/bed temperatures |
| 7 | Streaming a file of harmless commands only (`M105`, `G4 P250`, `M114`, `M115`) |
| 8 | `pause`/`resume`/`cancel` SD-print controls; operator `SIGINT` cancel of a host stream |
| 9 | `SIGINT` cancel writing a session record with the exact expected fields; zero privacy leakage into the structured log |
| End-stop update (2026-07-21) | `home`, `home X`, `home Y`, `home Z` — all four confirmed `X:0.00 Y:0.00 Z:0.00` on completion; `send "G28"` and a `print` file containing `G28` confirmed still refused (exit `4`), no regression |

**Not yet performed on real hardware:** the Milestone 10 `SIGPWR`
power-failure path itself (no printer was attached during that
milestone's implementation) — see
[[milestone-10|docs/milestones/milestone-10.md]] §4 for the residual
risk assessment. The mechanism reuses the exact signal-handling and
graceful-stop code the `SIGINT` path above already exercised on
hardware; only `SIGPWR` specifically reaching that same code path is
unconfirmed.

## Verifying from a clean checkout

The Definition of Done requires the build and test instructions in
`README.md` to work from a fresh checkout, with no leftover build
artifacts or developer-local state assumed. To verify:

```bash
git clone <repo> /tmp/printerctl-clean-check
cd /tmp/printerctl-clean-check
cmake -S . -B build -DCMAKE_C_COMPILER=gcc
cmake --build build
ctest --test-dir build --output-on-failure
```

This should configure, build and pass all eight suites with no manual
setup beyond a C17 compiler, CMake >= 3.16, and the standard POSIX
tools the test suites use (`posix_openpt` and friends, already part of
glibc on the project's Linux target).
