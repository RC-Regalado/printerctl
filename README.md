# printerctl

Command-line application written in C17 that controls 3D printers running
Marlin Firmware over USB serial. See `Agents.md`, `architecture.md` and
`roadmap.md` for project background, architecture and the development plan.

## Status

This repository currently implements Milestone 5 (Serial Transport);
see `roadmap.md` for the milestone plan and
`docs/milestones/milestone-5.md` /
`docs/design/milestone-5-serial.md` for what this milestone covers and
why (earlier milestones are covered by their own reports under
`docs/milestones/`).

The CLI parses the full command grammar and resolves configuration from
defaults, an optional `--config` file and CLI overrides, and every command
invocation now produces structured, timestamped logs on `stderr` (see
`docs/logging.md`). `--dry-run print FILE` parses and normalizes the
G-code file, reports commands and byte progress, and never opens a serial
device. The Marlin response parser (`printerctl_marlin`) classifies
`ok`/`Error:`/`busy:`/`echo:`/`Resend:` lines and decodes temperature,
position and firmware-info reports. The serial transport
(`printerctl_serial`) opens a terminal device in raw 8N1 mode and
exchanges timeout-bounded command/response lines, validated against
pseudo-terminals. The pieces are not composed yet: no CLI command opens
a serial port, and non-dry-run commands still exit `5` until
Milestone 6 wires `serial` + `marlin` + `logging` into `temp`,
`position`, `info` and `send`.

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

Use `build-clang` instead of `build` if you configured with Clang. Six
independent test binaries are run: `printerctl_tests` (CLI grammar,
including its logging integration), `printerctl_config_tests`
(configuration module), `printerctl_gcode_tests` (G-code reader),
`printerctl_marlin_tests` (Marlin response parser),
`printerctl_serial_tests` (serial transport over pseudo-terminals) and
`printerctl_logging_tests` (logging module).

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

Every non-dry-run valid command parses successfully, resolves
configuration, and then exits `5` because serial execution is not
implemented yet in this build.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Success (including `--help`/`--version`) |
| `2` | Usage error |
| `3` | Configuration error |
| `4` | Safety policy rejection |
| `5` | Parsed and resolved successfully, but not yet available |
| `6` | G-code file error |

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
records (e.g. `command_parsed`). Logs never include the raw `--port`
value, the `--config` path, `print`'s file argument or `send`'s raw
command text — only booleans and the command name. See `docs/logging.md`
for the full format and event reference.

## Run

```bash
./build/printerctl --help
./build/printerctl --version
./build/printerctl --dry-run print file.gcode
./build/printerctl home
./build/printerctl home --i-understand-no-endstops
```

## Versioning

The project version is declared once, in `CMakeLists.txt`, via
`project(printerctl VERSION ...)`. It is exposed to C code through a header
generated at configure time (`include/printerctl/version.h`, produced from
`include/printerctl/version.h.in`), so there is no second place where the
version number needs to be kept in sync manually.
