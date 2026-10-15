---
title: Testing
tags: [printerctl, testing, reference]
---

# Testing

Source of truth: [[Agents]], [[docs/architecture|architecture]] §18. This document is the
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

Twenty independent CMake/ctest targets, one per module plus integration
suites:

| Binary | Covers |
|---|---|
| `printerctl_tests` | CLI grammar, configuration precedence, logging integration, and end-to-end command execution against forked mock printers (`tests/test_cli.c`) |
| `printerctl_config_tests` | Configuration resolution: defaults, `--config` file, CLI overrides, owner-aware views, precedence, validation |
| `printerctl_gcode_tests` | G-code file reading: normalization, comment/blank-line stripping, progress, malformed/oversized lines, `G28` detection |
| `printerctl_macro_tests` | M27/M28 macro domain: typed definitions, validation, config-backed built-in resolution, duplicate IDs and deterministic iteration without serial/daemon execution |
| `printerctl_marlin_tests` | Marlin response classification and parsing: `ok`, `Error:`, `busy:`, `echo:`, `Resend:`, temperature/position/firmware-info reports |
| `printerctl_serial_tests` | Serial transport over pseudo-terminals: open/configure/read/write, timeout, exclusive access (`TIOCEXCL`) |
| `printerctl_printer_tests` | Printer session orchestration against scripted mock printers: connect, single-command exchange, streaming, pause/resume/cancel (operator and power-failure), descriptor-leak checks on failure paths |
| `printerctl_record_tests` | Session records and the active-print marker: state-dir resolution, record content, exclusive creation, failure modes, PID marker round-trip |
| `printerctl_logging_tests` | Logging module: formatting, level filtering |
| `printerctl_core_tests` | Milestone 12 print engine extraction: lifecycle, dry-run behavior, control callbacks and session recording without CLI parsing |
| `printerctl_daemon_ipc_tests` | Milestone 13 internal command/event pipe framing, correlation token validation and malformed/truncated message handling |
| `printerctl_daemon_runtime_tests` | Milestone 13 runtime lifecycle: worker startup/shutdown handshakes, NOP round-trip, fault injection and repeated start/stop |
| `printerctl_plp1_frame_tests` | PLP/1 frame codec: byte-exact golden vectors, flags, versioning, bounds and message registry, including M21 physical-command vectors |
| `printerctl_plp1_payload_tests` | PLP/1 payload codecs: VERSION, STATUS, PRINT_START, M20 read-only query responses, M21 physical-command payloads and error payload validation |
| `printerctl_plp1_parser_tests` | Milestone 14 incremental parser: fragmentation, coalescing, malformed frames and parser recovery boundaries |
| `printerctl_client_tests` | PLP client library: Unix socket connect, request IDs, write-all, incremental response parsing, correlation, timeout, remote errors and PING/VERSION/STATUS/M20 query/M21 physical-command wrappers |
| `printerctl_daemon_socket_tests` | Milestone 15 Unix socket transport: real `AF_UNIX`, fragmented/coalesced requests, stale/live socket paths, sequential clients and malformed-client containment |
| `printerctld_tests` | Daemon executable smoke/config/CLI-migration test: fork `printerctld`, connect through PLP/1, validate `PING`, terminate with `SIGTERM`, load valid daemon config, reject invalid daemon config, and cover M20/M21 CLI-to-daemon commands over PTY |
| `printerctl_daemon_print_tests` | Milestone 16 daemon print integration and M28 macro execution: worker-owned print engine, worker-owned typed MOVE macro execution, pseudo-terminal Marlin, PRINT/MACRO exclusion, error/timeout behavior and liveness |
| `printerctl_build_boundary_tests` | Milestone 25 build/dependency gate: runs `nm` on the real, linked `printerctl` executable and fails if any `printerctl_serial`/`printerctl_printer`/`printerctl_marlin`/`printerctl_core` (print_engine) symbol appears in it |

Run all of them:

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=gcc
cmake --build build
ctest --test-dir build --output-on-failure
```

Substitute `-DCMAKE_C_COMPILER=clang` (and a different build directory)
to run the same suite under Clang; both must be warning-free with
`-Wall -Wextra -Wpedantic` per the global Definition of Done.

The CLI and executable tests use `/tmp/printerctl.sock`; some expect
no daemon there and others create their own mock daemon. Isolate `/tmp`
when a real daemon is running, so tests cannot reach hardware or unlink
its socket. On hosts with Bubblewrap:

```bash
bwrap --ro-bind / / --dev-bind /dev /dev --proc /proc --tmpfs /tmp \
  --bind "$PWD" "$PWD" ctest --test-dir build --output-on-failure
```

The 2026-10-12 G28 file-policy tests cover dry-run rendering, printer
streaming, and CLI -> daemon -> PTY execution of G28 and X/Y/Z variants.
The process test holds each homing ACK, checks that no next command is
sent, and verifies that the final source command count includes homing.
Existing raw-send rejection remains. PRINT_CANCEL, SIGTERM and SIGPWR
ACK-window tests now place G28 next in the source and verify that
cancellation prevents it from being transmitted.

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
- **In-memory binary protocol vectors**: the PLP/1 tests assert literal
  wire bytes for stable protocol examples and run fragmentation/coalescing
  through caller-owned buffers, without sockets or daemon dependencies.
- **Real Unix socket integration**: the M15 test suite uses disposable
  socket paths under `/tmp`, real `AF_UNIX`/`SOCK_STREAM` clients, and
  pthread-backed `serve_one()` calls to verify stream behavior without
  serial hardware or print execution.
- **Daemon process tests**: `printerctld_tests` forks the built
  `printerctld` binary, waits for its Unix socket, performs real PLP/1
  exchanges, validates idle shutdown, process-level `SIGTERM` and
  `SIGPWR` during active PTY-backed prints, active-print marker PID
  targeting, ACK-window cancellation for `SIGTERM`, `SIGPWR` and
  `PRINT_CANCEL`, no post-commit host transmission, and both directions
  of legacy CLI / daemon serial exclusivity.
- **Daemon print/macro integration**: M16/M28 tests drive the daemon runtime
  directly against a pseudo-terminal mock Marlin printer, proving
  `PRINT_START`, pause/resume/cancel, active-print rejection, typed macro
  movement translation, PRINT/MACRO mutual exclusion, failure barriers and
  macro liveness without real hardware.

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

This should configure, build and pass all nineteen suites with no manual
setup beyond a C17 compiler, CMake >= 3.16, and the standard POSIX/
binutils tools the test suites use (`posix_openpt` and friends, already
part of glibc on the project's Linux target, plus `nm` for the M25
build-boundary gate).
