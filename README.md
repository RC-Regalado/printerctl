# printerctl

Command-line application written in C17 that controls 3D printers running
Marlin Firmware over USB serial. See `Agents.md`, `docs/architecture.md`
and `docs/roadmap.md` for project background, architecture and the
development plan; `docs/safety.md`, `docs/testing.md` and
`docs/apcupsd.md` for the consolidated safety policy, test suite and
power-failure references.

## Status

This repository currently implements Milestone 15 (Unix Socket Transport)
of `docs/roadmap.md`'s Phase 1 —
Daemon Foundation + Single-Print IPC (milestones 0-9 keep their original
scope; milestones 10+ follow the revised roadmap). Milestone 12 (Print
Engine Extraction) moved the application-level print lifecycle behind
`printerctl_print_engine_t`, reused unchanged by the CLI; Milestone 13
added the independent control-context/worker-thread/two-pipe runtime
foundation; Milestone 14 froze PLP/1; and Milestone 15 adds a local
`AF_UNIX`/`SOCK_STREAM` transport for hardware-independent `PING`,
`VERSION` and idle `STATUS`. It still does not integrate `print_engine`,
serial, Marlin or print execution into the daemon, and changes nothing
about the CLI's behavior described below. See
`docs/milestones/milestone-15.md` /
`docs/design/milestone-15-unix-socket-transport.md`,
`docs/milestones/milestone-14.md` /
`docs/design/protocol-v1.md`,
`docs/milestones/milestone-13.md` /
`docs/design/milestone-13-daemon-runtime.md` and
`docs/milestones/milestone-12.md` /
`docs/design/milestone-12-print-engine.md` for what each covers (earlier
milestones have their own reports under `docs/milestones/`). Since
Milestone 10 (2026-07-21), min end-stops were installed and enabled on
all three axes, so `home` is now a real, unconfirmed command — see
`Agents.md`'s Home amendment.

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
last acknowledged line; while it streams, the operator controls the
session with signals (SIGUSR1 pauses, SIGUSR2 resumes, SIGINT/SIGTERM
cancels gracefully with exit code `9`). `pause`, `resume` and `cancel`
send `M25`/`M24`/`M524` to a print running from the printer's SD card.
`--dry-run print FILE` parses and normalizes the file without opening a
serial device; `--dry-run` with a single command prints the line it
would transmit and exits `0`. A print that started but did not complete
leaves an interrupted-session record under
`$XDG_STATE_HOME/printerctl` (see Session records below); printerctl
never resumes a print automatically. `power low-battery` notifies a
running print of an external low-battery condition (meant to be run by
an APCUPSD script; see Power events below), cancelling it gracefully
with its own distinct exit code. `home` sends `G28` (all axes); `home
X`/`home Y`/`home Z` home a single axis — see Homing below.

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

Use `build-clang` instead of `build` if you configured with Clang. 15
independent test binaries are run: `printerctl_tests` (CLI grammar,
logging integration and end-to-end commands against forked mock
printers), `printerctl_config_tests` (configuration module),
`printerctl_gcode_tests` (G-code reader), `printerctl_marlin_tests`
(Marlin response parser), `printerctl_serial_tests` (serial transport
over pseudo-terminals), `printerctl_printer_tests` (printer session
against scripted mock printers), `printerctl_record_tests` (session
records and the active-print marker), `printerctl_logging_tests`
(logging module), `printerctl_core_tests` (the Milestone 12 print
engine, without any CLI parsing or terminal output),
`printerctl_daemon_ipc_tests` (Milestone 13 internal command/event
framing and validation) and `printerctl_daemon_runtime_tests`
(Milestone 13 daemon runtime lifecycle, IPC round-trips, fault
injection and repeated start/stop cycles), `printerctl_plp1_frame_tests`,
`printerctl_plp1_payload_tests` and `printerctl_plp1_parser_tests`
(Milestone 14 PLP/1 wire contract), and
`printerctl_daemon_socket_tests` (Milestone 15 Unix socket transport).
Run the daemon runtime/socket tests under ThreadSanitizer when checking
concurrency changes. See
`docs/testing.md` for what each covers in detail, the patterns behind
them and the hardware-validation record.

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
printerctl home
printerctl home X
printerctl pause
printerctl resume
printerctl cancel
printerctl temp
printerctl position
printerctl info
printerctl power low-battery
```

`print FILE` and `send COMMAND` reject an empty `FILE`/`COMMAND` argument
as a usage error (exit code `2`).

### Homing

Min end-stops are enabled on all three axes (X, Y, Z), so `home` needs
no confirmation flag: it runs like `temp`/`position`/`info`, requiring
only a configured `--port`, and sends `G28`. `home X`, `home Y` and
`home Z` home a single axis, sending `G28 X`, `G28 Y` and `G28 Z`
respectively. `G28` remains refused everywhere else — `send "G28"` and
a `print` file containing `G28` both still fail with exit code `4`, no
bypass — so homing is only ever available through the dedicated `home`
command, never an unexpected side effect of a raw command or a sliced
file. See `docs/safety.md` for the full safety policy reference.

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

While `print FILE` streams, signal the printerctl process to control
the session (e.g. `kill -USR1 PID` from another terminal):

| Signal | Effect |
|---|---|
| `SIGUSR1` | Pause after the current command is acknowledged |
| `SIGUSR2` | Resume from the next unsent command |
| `SIGINT` / `SIGTERM` | Cancel gracefully (Ctrl+C; summary still printed, exit code `9`) |
| `SIGPWR` | Cancel gracefully as a power-failure/low-battery event, exit code `10` (see Power events below; not meant to be sent by the operator directly) |

Nothing is sent to the printer on pause/resume/cancel/power-cancel of a
host stream: it never moves, heats or parks on its own, so a long pause
leaves the nozzle hot where it stopped — the operator decides. The
serial port is held exclusively while a session runs; a concurrent
printerctl invocation against the same port fails to open it (exit `7`)
instead of corrupting the stream.

`pause`, `resume` and `cancel` instead control a print running from the
**printer's SD card**, as direct Marlin commands: `M25` (pause SD
print), `M24` (resume SD print) and `M524` (abort SD print). They
render the exchange like `send` and cannot reach a host stream. On
firmware without SD support Marlin answers `echo:Unknown command`
(rendered) and still acknowledges, so the command is a visible no-op.

`temp`, `position`, `info`, `send COMMAND`, `home`, `pause`, `resume`
and `cancel` require a configured port (`--port` or the config file;
exit code `3` otherwise), transmit their Marlin command (`M105`,
`M114`, `M115`, the raw text, `G28`/`G28 X`/`G28 Y`/`G28 Z`, `M25`,
`M24`, `M524`), wait for the terminal reply and render it. `send`
refuses `G28` (exit code `4`, no bypass). Under `--dry-run` they print
the line they would send and exit `0` without touching any device.

`power low-battery` never requires a port and never opens a serial
device itself — see Power events below.

### Power events

`printerctl power low-battery` is the command an external notifier (the
APCUPSD script at `scripts/apcupsd/printerctl-low-battery.sh`, see
`docs/scripts/printerctl-low-battery.md`) runs when the UPS reports a
low battery. It looks up the printerctl process currently streaming a
print — via a live marker at
`$XDG_STATE_HOME/printerctl/active-print.pid`, published only while a
print streams — and delivers it `SIGPWR`, which that process turns into
a graceful cancellation exactly like an operator's Ctrl+C, but recorded
and reported as its own distinct cause. No active print is not an
error: the command always exits `0` in that case, and running it more
than once is safe. See `docs/apcupsd.md` for the full mechanism and how
to inspect a job cancelled this way.

```bash
printerctl power low-battery
printerctl --dry-run power low-battery
```

### Session records

A `print` that opened its file but did not complete — failed or
cancelled — writes one interrupted-session record to
`$XDG_STATE_HOME/printerctl` (default `~/.local/state/printerctl`), and
names the file it wrote on stdout:

```ini
# printerctl interrupted print session record
# printerctl never resumes a print automatically (safety policy);
# use this record to decide how to restart the job manually.
version=1
timestamp=2026-07-10T23:39:36.159Z
event=print_interrupted
job_id=job-48213
state=cancelled_power_failure
cause=apcupsd_low_battery
file=job.gcode
serial_device=/dev/ttyACM0
commands_sent=4
commands_acknowledged=4
last_acknowledged_line=4
bytes_consumed=26
file_size=44
progress_percent=59
printer_state=unknown
```

Records are private (0600), fsync'd and never overwritten (one
timestamped file per interruption). printerctl never reads them back:
there is no automatic resume and no automatic reconnect — a serial
failure or disconnect ends the session gracefully with a record, and
restarting the job is always the operator's explicit decision. `state`
and `cause` distinguish an operator cancel (`cancelled`/`cancelled`)
from a power-failure cancel (`cancelled_power_failure`/
`apcupsd_low_battery`) from an ordinary failure (`failed`/the specific
cause). `job_id` is a placeholder (`job-<pid>`) ahead of a real job
model; `printer_state` is always `unknown` — no live query is sent to
populate it. Completed prints and files that could not be opened at all
leave no record. A record that cannot be written is a warning on stderr
and never changes the print's exit code.

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
| `9` | Print cancelled by the operator (SIGINT/SIGTERM during `print`) |
| `10` | Print cancelled by a power-failure/low-battery signal (SIGPWR) |

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
`--port` value, the `--config` path, a session record's path or `print`'s file argument; the
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
./build/printerctl --port /dev/ttyACM0 pause
./build/printerctl --port /dev/ttyACM0 resume
./build/printerctl --port /dev/ttyACM0 cancel
./build/printerctl power low-battery
./build/printerctl --port /dev/ttyACM0 home
./build/printerctl --port /dev/ttyACM0 home X
```

## Versioning

The project version is declared once, in `CMakeLists.txt`, via
`project(printerctl VERSION ...)`. It is exposed to C code through a header
generated at configure time (`include/printerctl/version.h`, produced from
`include/printerctl/version.h.in`), so there is no second place where the
version number needs to be kept in sync manually.
