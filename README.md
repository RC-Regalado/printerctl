# printerctl

Command-line application written in C17 that controls 3D printers running
Marlin Firmware over USB serial. See `Agents.md`, `architecture.md` and
`roadmap.md` for project background, architecture and the development plan.

## Status

This repository currently implements Milestone 1 (CLI and configuration);
see `roadmap.md` for the milestone plan and
`docs/milestones/milestone-1.md` / `docs/design/milestone-1-cli-config.md`
for what this milestone covers and why.

The CLI parses the full command grammar and resolves configuration from
defaults, an optional `--config` file and CLI overrides, but **no serial,
G-code, Marlin protocol or logging functionality exists yet**. Every valid
printer command is parsed and its configuration fully resolved, then the
program reports that execution is not yet implemented (exit code `5`).
Nothing ever opens a serial device or talks to a printer in this build.

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

Use `build-clang` instead of `build` if you configured with Clang. Two
independent test binaries are run: `printerctl_tests` (CLI grammar) and
`printerctl_config_tests` (configuration module).

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

Every other valid command parses successfully, resolves configuration, and
then exits `5` because execution is not implemented yet in this build.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Success (including `--help`/`--version`) |
| `2` | Usage error |
| `3` | Configuration error |
| `4` | Safety policy rejection |
| `5` | Parsed and resolved successfully, but not yet available |

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
