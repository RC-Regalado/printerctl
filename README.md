# printerctl

Command-line application written in C17 that controls 3D printers running
Marlin Firmware over USB serial. See `Agents.md`, `architecture.md` and
`roadmap.md` for project background, architecture and the development plan.

## Status

This repository currently implements Milestone 7 (Reliable G-code
Streaming MVP); see `roadmap.md` for the milestone plan and
`docs/milestones/milestone-7.md` /
`docs/design/milestone-7-streaming.md` for what this milestone covers
and why (earlier milestones are covered by their own reports under
`docs/milestones/`).

The CLI parses the full command grammar and resolves configuration from
defaults, an optional `--config` file and CLI overrides, and every command
invocation produces structured, timestamped logs on `stderr` (see
`docs/logging.md`). `temp`, `position`, `info` and `send COMMAND` now
talk to the printer over the configured `--port`: one command is
transmitted, response lines are read until Marlin's terminal reply
(waiting out `busy:`/`echo:` keepalives), and the result is rendered
human-readably. `send` refuses G28 with no bypass; the operator's
explicit `M112` passes through. `print FILE` streams the G-code file
one acknowledged command at a time — the next command is not sent (or
even read from the file) until Marlin replies `ok` — reporting progress
per whole-percent change and a session summary with the final state and
last acknowledged line. `--dry-run print FILE` parses and normalizes
the file without opening a serial device; `--dry-run` with a single
command prints the line it would transmit and exits `0`. Only `home`,
`pause`, `resume` and `cancel` still exit `5`.

## Requirements

- CMake >= 3.16
- A C17-capable compiler: GCC or Clang
- Linux (development target: Arch Linux)

## Build

### GCC

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=gcc
cmake --build build
```

### Clang

```bash
cmake -S . -B build-clang -DCMAKE_C_COMPILER=clang
cmake --build build-clang
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Use `build-clang` instead of `build` if you configured with Clang. Seven
independent test binaries are run: `printerctl_tests` (CLI grammar,
logging integration and end-to-end commands against forked mock
printers), `printerctl_config_tests` (configuration module),
`printerctl_gcode_tests` (G-code reader), `printerctl_marlin_tests`
(Marlin response parser), `printerctl_serial_tests` (serial transport
over pseudo-terminals), `printerctl_printer_tests` (printer session
against scripted mock printers) and `printerctl_logging_tests`
(logging module).

## Usage

```text
printerctl [GLOBAL_OPTIONS] COMMAND [COMMAND_ARGS]
printerctl --help
printerctl --version
```

Global options (must appear before `COMMAND`, `--option VALUE` form only,
at most once each):

| Option | Description | Default |
|---|---|---|
| `--port VALUE` | Serial device path (must not be empty) | none |
| `--baud VALUE` | Baud rate, integer 1-4000000 | `115200` |
| `--timeout VALUE` | Response timeout in milliseconds, integer 1-600000 | `5000` |
| `--dry-run` | Never open or write to the serial device | off |
| `--verbose` | Enable verbose output | off |
| `--config PATH` | Load `key=value` configuration from `PATH` | none |

Commands:

```bash
printerctl --dry-run print file.gcode
printerctl print file.gcode
printerctl send "M115"
printerctl home --i-understand-no-endstops
printerctl pause
printerctl resume
printerctl cancel
printerctl temp
printerctl position
printerctl info
```

`print FILE` and `send COMMAND` reject an empty `FILE`/`COMMAND` argument
as a usage error (exit code `2`).

`home` is refused (exit code `4`) unless `--i-understand-no-endstops` is
given explicitly, because endstop sensors are not available on the target
printer. This applies even under `--dry-run`.

`--dry-run print FILE` exits `0` when the file parses cleanly and writes
the normalized command stream with source line numbers and whole-percent
progress. It strips comments and blank lines, rejects malformed or
oversized lines with exit code `6`, and rejects `G28` with exit code `4`.
No serial device is opened.

`print FILE` (without `--dry-run`) streams the file over the configured
port, waiting for each command's `ok` before sending the next; `busy:`
and `echo:` keepalives never time the print out (the timeout bounds
silence). It stops gracefully — nothing further is sent and a summary
names the final state, commands sent/acknowledged and last acknowledged
line — on `Error:` (exit `8`), `Resend:` (exit `8`, unsupported in the
MVP), timeout or disconnect (exit `7`), a malformed file (exit `6`) or
a `G28` line (exit `4`, blocked before transmission).

`temp`, `position`, `info` and `send COMMAND` require a configured port
(`--port` or the config file; exit code `3` otherwise), transmit their
Marlin command (`M105`, `M114`, `M115`, or the raw text), wait for the
terminal reply and render it. `send` refuses `G28` (exit code `4`, no
bypass). Under `--dry-run` they print the line they would send and exit
`0` without touching any device. `print` without `--dry-run`, `home`,
`pause`, `resume` and `cancel` still exit `5`.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Success (including `--help`/`--version`) |
| `2` | Usage error |
| `3` | Configuration error |
| `4` | Safety policy rejection |
| `5` | Parsed and resolved successfully, but not yet available |
| `6` | G-code file error |
| `7` | Communication error (cannot open port, I/O failure, timeout) |
| `8` | Printer error (`Error:`/`Resend:` reply, or missing report) |

### Configuration file

Loaded only when `--config PATH` is given explicitly (no implicit path).
Root-level `key=value` lines, blank lines and full-line `#` comments,
whitespace trimmed around keys and values:

```ini
# printerctl config
port=/dev/ttyACM0
baud=115200
timeout=5000
dry_run=false
verbose=false
```

Recognized keys are exactly `port`, `baud`, `timeout`, `dry_run`,
`verbose`; booleans must be exactly `true` or `false`. An unknown key, a
duplicated key, a malformed line, an invalid or out-of-range value, an
empty `port` value, or a file that cannot be opened or read is a
configuration error (exit code `3`). Precedence is: built-in defaults,
then the config file, then CLI options — CLI options always win.

### Logging

Every command invocation (other than `--help`/`--version`) writes
structured, timestamped log lines to `stderr`, alongside the existing
human-readable messages. `--verbose` additionally enables `DEBUG`-level
records: `command_parsed`, and for the printer commands the full wire
exchange as `serial_tx`/`serial_rx` records. Logs never include the raw
`--port` value, the `--config` path or `print`'s file argument; the
`command_parsed`/`command_unavailable` events carry only the command
name (never `send`'s text — the wire exchange is visible only through
the `serial_tx`/`serial_rx` records under `--verbose`). See
`docs/logging.md` for the full format and event reference.

## Run

```bash
./build/printerctl --help
./build/printerctl --version
./build/printerctl --dry-run print file.gcode
./build/printerctl --port /dev/ttyACM0 print file.gcode
./build/printerctl --port /dev/ttyACM0 temp
./build/printerctl --port /dev/ttyACM0 position
./build/printerctl --port /dev/ttyACM0 info
./build/printerctl --port /dev/ttyACM0 send "M115"
./build/printerctl home
```

## Versioning

The project version is declared once, in `CMakeLists.txt`, via
`project(printerctl VERSION ...)`. It is exposed to C code through a header
generated at configure time (`include/printerctl/version.h`, produced from
`include/printerctl/version.h.in`), so there is no second place where the
version number needs to be kept in sync manually.
