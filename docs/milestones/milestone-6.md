---
title: Milestone 6 — Single Marlin Commands
milestone: 6
status: done
date: 2026-07-07
tags: [printerctl, milestone, printer, marlin, serial]
---

# Milestone 6 — Single Marlin Commands

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]]. Design
details: [[milestone-6-printer-commands|docs/design/milestone-6-printer-commands.md]].

## 1. Technical summary

Milestone 6 adds the `printer` session module — the first composition
of `serial` + `marlin` + `logging` — and turns `temp`, `position`,
`info` and `send` into real commands. `printerctl_printer_execute()`
implements the §7.2 exchange: transmit one line, read until a terminal
classification (`ok` success, `Error:` failure, `Resend:` explicit
unsupported), waiting out `busy:`/`echo:`/informational lines with the
timeout bounding silence rather than command duration. Typed queries
decode M105/M114/M115 reports whether they ride on the `ok` payload or
precede it; an `ok` without the expected report is an explicit
`ERR_UNEXPECTED_RESPONSE`. The CLI renders human-readable output
(temperatures with targets, coordinates, firmware fields, and for
`send` the full `>`/`<` exchange).

Safety: `send` rejects every G28 variant before anything is
transmitted, sharing the G-code reader's detection via the newly
exported `printerctl_gcode_command_is_unsafe_g28()`; there is no bypass
(resolving `architecture.md` §13 item 5). M112 passes through — it is
the operator's explicit emergency stop, and only *automatic* M112 is
forbidden. Dry-run never opens a port (two layers: the CLI never
connects, and connect itself refuses a dry-run config) and
`send G28` is preflighted before opening a port; `--dry-run send G28`
also fails with the safety code. Exit codes 7
(communication error) and 8 (printer error) complete the taxonomy;
no port maps to the existing configuration error 3.

Logging: every TX line is a DEBUG `serial_tx` record and every RX line
a DEBUG `serial_rx` record (full exchange visible with `--verbose`);
connect/disconnect are INFO `state_transition` records with baud and
timeout but never the port path; failures add one generic ERROR record
while the human-readable message keeps the detail.

## 2. Files changed

- `CMakeLists.txt`
- `include/printerctl/printer.h`
- `src/printer/printer.c`
- `include/printerctl/gcode.h` (export the shared G28 detection)
- `src/gcode/gcode.c`
- `include/printerctl/cli.h` (exit codes 7/8, contract update)
- `src/cli/cli.c` (dispatch, rendering, dry-run for single commands, help)
- `tests/CMakeLists.txt`
- `tests/test_printer.c`
- `tests/test_cli.c`
- `README.md`
- `architecture.md` (§13 item 5 resolved)
- `docs/design/milestone-6-printer-commands.md`
- `docs/milestones/milestone-6.md`

## 3. Acceptance validation

- Commands wait for terminal Marlin response: the exchange loop stops
  only on `ok`/`Error:`/`Resend:`; busy-keepalive sequences are waited
  out (module test) and a silent printer times out explicitly.
- `temp`, `position`, `info` produce human-readable output: end-to-end
  CLI tests against forked mock printers assert the rendered
  temperatures, coordinates and firmware fields.
- `send` logs full exchange: `>`/`<` rendering on stdout plus
  `serial_tx`/`serial_rx` records asserted under `--verbose`.
- Unsafe commands remain blocked unless explicitly allowed: six G28
  variants rejected at the module level with nothing transmitted
  (asserted by reading the mock's master side), `send G28` exits 4 at
  the CLI level before requiring/opening a port, and the block holds
  under `--dry-run`. M112 passes as the operator's explicit command.
- Mock Marlin integration tests pass: scripted pty mocks at module
  level and forked reactive mocks at CLI level, covering success,
  `Error:`, `Resend:`, timeout, disconnect and missing-report paths.

## 4. Verification

- GCC build (`build-gcc`): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`.
- Clang build (`build-clang`): warning-free with the same flags.
- CTest: 7/7 test binaries pass on both compilers
  (`printerctl_printer_tests` is new; `printerctl_tests` gained ten
  end-to-end tests and two Milestone 1 expectations were updated —
  `temp`/`position`/`info`/`send` without a port are now configuration
  errors instead of "unavailable").
- `clang-tidy`: executed with the Milestone 2 analyzer exclusions; it
  exits successfully. Remaining findings are readability/test-harness
  noise, include-cleaner false positives through public module headers,
  and the existing feature-test macro warnings.
- Real-binary smoke test against a Python pty mock: `--verbose temp`
  exited 0, rendered `hotend 24.10 C (target 0.00 C), bed 23.50 C
  (target 0.00 C)`, and logged the full session
  (`session_start` → `config_summary` → `command_parsed` →
  `state=connected` → `serial_tx M105` → `serial_rx ok T:...` →
  `state=disconnected` → `session_end`).
- Real hardware smoke test against `/dev/ttyUSB0` at 115200 baud, with
  no homing or motion:
  - `temp` sent `M105`, exited 0, and rendered hotend `27.46 C`
    target `0.00 C` plus bed `-15.00 C` target `0.00 C`.
  - `info` sent `M115`, exited 0, and rendered Marlin `2.1.2.1`,
    machine `3D Printer`, protocol `1.0`, extruders `1`, UUID
    `cede2a2f-41a2-4748-9b12-c55c62f367ff`.
  - `position` sent `M114`, exited 0, and rendered
    `X=0.00 Y=0.00 Z=0.00 E=0.00`.
  - `send G28` exited 4 before opening a printer session; verbose logs
    contain no `state=connected` and no `serial_tx`.

## 5. Risks found

- Hardware validation is intentionally limited to read-only/status
  commands because this machine only has Z-min homing hardware. No
  `home`, movement, extrusion or heating command was executed.
- Marlin DTR-reset behavior did not block the `/dev/ttyUSB0` smoke test,
  but pty mocks still cannot reproduce every USB-driver quirk. If a
  future board needs it, connect can grow an explicit banner wait.
- The exchange loop has no cap on non-terminal lines; a pathological
  firmware that streams informational lines forever (with less than the
  timeout between them) would hold the command open. The timeout covers
  silence; operator interrupt covers chatter. Revisit in M7 if needed.
- `send` passes M112 through by design. This is documented operator
  power, not an oversight — see design note §3.

## 6. Decisions taken

- `printer` composes `serial` + `marlin` + `logging` + `gcode`; the
  session is a stack value owning the port and borrowing the logger.
- Timeout bounds silence, not command duration: each response line
  restarts the read deadline, so `busy:` keepalives keep a long command
  alive. See design note §2.
- `send` blocks G28 only, with no bypass, sharing the reader's
  detection through one exported function; M112 is allowed as explicit
  operator action (resolves `architecture.md` §13 item 5). See design
  note §3.
- Exit 7 = the conversation could not happen (open/I/O/disconnect/
  timeout); exit 8 = the printer said no (`Error:`/`Resend:`/missing
  report); no port = exit 3. See design note §5.
- TX/RX logged at DEBUG via the Milestone 2 helpers — the full exchange
  is a `--verbose` view, and the port path never enters the structured
  log. See design note §6.
- CLI mock printers are forked reactive children (the serial open
  flushes pre-scripted input by design); module-level mocks script
  after connect. See design note §7.

## 7. Next steps

Continue with Milestone 7: the reliable G-code streaming MVP — the
print session state machine that sends one normalized command at a
time, waits for `ok`, keeps waiting on `busy:`/`echo:`, stops
gracefully on `Error:`/timeout/disconnect/`Resend:`, and reports
progress. The building blocks are now all in place: the G-code reader
(M3), the response classifier (M4), the transport (M5) and the exchange
loop shape (M6).
