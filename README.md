# printerctl

Command-line application written in C17 that controls 3D printers running
Marlin Firmware over USB serial. See `Agents.md`, `docs/architecture.md`
and `docs/roadmap.md` for project background, architecture and the
development plan; `docs/safety.md`, `docs/testing.md` and
`docs/apcupsd.md` for the consolidated safety policy, test suite and
power-failure references.

## Status

This repository has completed Phase 2 CLI client migration and is in
Phase 3 — Safe Macros with Milestone 28 (Built-in Macro Execution),
after completing
`docs/roadmap.md`'s Phase 1 —
Daemon Foundation + Single-Print IPC (milestones 0-9 keep their original
scope; milestones 10+ follow the revised roadmap). Milestone 12 (Print
Engine Extraction) moved the application-level print lifecycle behind
`printerctl_print_engine_t`; Milestone 13
added the independent control-context/worker-thread/two-pipe runtime
foundation; Milestone 14 froze PLP/1; and Milestone 15 adds a local
`AF_UNIX`/`SOCK_STREAM` transport for hardware-independent `PING`,
`VERSION` and idle `STATUS`, exposed through the `printerctld`
executable; Milestone 16 connects PLP/1 `PRINT_START`, `PRINT_PAUSE`,
`PRINT_RESUME` and `PRINT_CANCEL` to one worker-owned print engine. It
does not add queueing, history, event subscriptions or multi-printer
support. Milestone 18 added owner-aware configuration views, and
Milestone 19 adds a reusable PLP client library, Milestone 20 migrates
`temp`, `position` and `info` through `printerctld`, and Milestone 21
migrates `send`, `home` and `sd pause`/`sd resume`/`sd cancel`; Milestone
22 migrates `print FILE` to daemon-owned `PRINT_START`; milestones 23-26
complete host print control, power alignment, legacy serial removal and Phase 2
validation. M27 adds a pure typed macro domain and built-in macro registry
metadata; M28 adds worker-owned physical execution for validated built-in
MOVE macros only, without PLP macro operations or CLI macro UX. The Phase 1
remediation audit is recorded in
`docs/audit/phase-1.md`. See
`docs/milestones/milestone-22.md` /
`docs/design/m22-host-print-migration.md`,
`docs/milestones/milestone-21.md` /
`docs/design/m21-explicit-physical-commands.md`,
`docs/milestones/milestone-20.md` /
`docs/design/m20-read-only-queries.md`,
`docs/milestones/milestone-19.md` /
`docs/design/m19-plp-client.md`,
`docs/milestones/milestone-18.md` /
`docs/design/m18-configuration-boundary.md`,
`docs/milestones/milestone-17.md` /
`docs/design/milestone-17-lifecycle-fault-compatibility.md`,
`docs/milestones/milestone-16.md` /
`docs/design/milestone-16-single-print-daemon-integration.md`,
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
`docs/logging.md`). `temp`, `position` and `info` now call
`printerctld` over PLP/1; the daemon owns serial access for those
read-only queries and returns structured responses for CLI rendering.
`send COMMAND`, `home [X|Y|Z]` and `sd pause`/`sd resume`/`sd cancel`
also go through `printerctld`; the CLI has no direct serial fallback for
them. `send` refuses G28 with no bypass; the operator's explicit `M112`
passes through. `print FILE` now sends `PRINT_START` to `printerctld`,
which validates the source, opens serial and owns host streaming through
the worker/print engine. The CLI exits after daemon acceptance; the print
continues if the client disconnects. `pause`, `resume` and `cancel`
control that daemon-owned host print from any client, over the same
`printerctld` connection as every other migrated command; SD-card print
control remains explicit under `sd pause`, `sd resume` and `sd cancel`.
`--dry-run print FILE` parses and normalizes the file without opening a
serial device; `--dry-run` with a single command prints the line it
would transmit and exits `0`. A daemon-owned print that started but did
not complete leaves an interrupted-session record under
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

Use `build-clang` instead of `build` if you configured with Clang. 20
independent test binaries are run: `printerctl_tests` (CLI grammar,
logging integration, daemon-unavailable paths and dry-run behavior),
`printerctl_config_tests` (configuration module),
`printerctl_gcode_tests` (G-code reader), `printerctl_macro_tests` (macro
domain validation and registry), `printerctl_marlin_tests`
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
`printerctl_daemon_socket_tests` (Milestone 15 Unix socket transport)
and `printerctld_tests` (Milestone 15 process smoke test over PLP/1).
`printerctl_daemon_print_tests` covers Milestone 16 daemon/worker print
integration over pseudo-terminals. `printerctl_client_tests` covers the
PLP client library, including M20 query wrappers, M21 physical-command
wrappers and M22 `PRINT_START`.
wrappers. `printerctld_tests` also covers M20 CLI-to-daemon read-only
queries and M21 explicit physical commands over a pseudo-terminal mock.
`printerctl_build_boundary_tests` is the M25 build/dependency gate: it
inspects the actual linked `printerctl` binary and fails if it ever
regains a `printerctl_serial`/`printerctl_printer`/`printerctl_marlin`/
`printerctl_core` (print_engine) symbol.
Run the daemon runtime/socket/process tests under ThreadSanitizer when checking
concurrency changes. See
`docs/testing.md` for what each covers in detail, the patterns behind
them and the hardware-validation record.

## Usage

```text
printerctl [GLOBAL_OPTIONS] COMMAND [COMMAND_ARGS]
printerctl --help
printerctl --version
```

The daemon executable exposes the M18 owner-aware hardware configuration
surface and the M16 single-print protocol surface:

```text
printerctld [--socket PATH] [--port PATH] [--baud N] [--timeout MS] [--config PATH] [--help] [--version]
```

Default socket path: `/tmp/printerctl.sock`.

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
printerctl sd pause
printerctl sd resume
printerctl sd cancel
printerctl temp
printerctl position
printerctl info
printerctl power low-battery
```

`print FILE` and `send COMMAND` reject an empty `FILE`/`COMMAND` argument
as a usage error (exit code `2`).

### Homing

Min end-stops are enabled on all three axes (X, Y, Z), so `home` needs
no confirmation flag: it sends `G28` through the daemon-owned execution
path. `home X`, `home Y` and
`home Z` home a single axis, sending `G28 X`, `G28 Y` and `G28 Z`
respectively. After operator verification of the minimum end-stops,
`print FILE` also permits `G28` and axis variants present in the source.
Review your slicer's start G-code: these commands will home when printed.
`send "G28"` remains rejected before IPC and by daemon policy; use
`home` for standalone homing. No homing is inserted automatically. See
`docs/safety.md` for the full safety policy reference.

`--dry-run print FILE` exits `0` when the file parses cleanly and writes
the normalized command stream with source line numbers and whole-percent
progress. It strips comments and blank lines, rejects malformed or
oversized lines with exit code `6`, and accepts source `G28` commands.
No serial device is opened.

`print FILE` starts daemon-owned streaming and returns after
`printerctld` accepts the job. Host `pause`, `resume` and `cancel`
control that daemon-owned stream from any client, whether or not it is
the one that started it: `pause` stops the daemon from sending its next
queued command, `resume` continues from the same counters (never resets
progress), and `cancel` stops the stream for good. Repeating `pause`
while already paused, or `resume` while already running, is a safe
no-op.

Nothing is sent to the printer on pause/resume/cancel/power-cancel of a
host stream: it never moves, heats or parks on its own, so a long pause
leaves the nozzle hot where it stopped — the operator decides. The
serial port is held exclusively by `printerctld` while a session runs.

`sd pause`, `sd resume` and `sd cancel` instead control a print running
from the **printer's SD card**, as daemon-owned Marlin commands: `M25`
(pause SD print), `M24` (resume SD print) and `M524` (abort SD print).
They cannot reach a host stream. On
firmware without SD support Marlin answers `echo:Unknown command`
(rendered) and still acknowledges, so the command is a visible no-op.

`temp`, `position`, `info`, `send COMMAND`, `home`, `print FILE`,
`pause`/`resume`/`cancel` and `sd pause`/`sd resume`/`sd cancel` require
`printerctld` and do not consume `--port`/`--baud`/`--timeout` in
`printerctl`. `send`
refuses `G28` (exit code `4`, no bypass). Under `--dry-run`, migrated
physical commands print the line they would send and exit `0` without
touching daemon or serial.

`send` and `home` wait for the daemon's terminal response without a
client-side 5-second deadline. Marlin's silence timeout remains owned
by `printerctld`: for slow commands such as `send "M303 E0 S200 C6"`,
start the daemon with an appropriate `--timeout` (for example, `60000`
milliseconds). Each received response line restarts that timeout;
it does not limit the total command duration. A CLI timeout or disconnect
does not abort an operation already running in firmware.

`power low-battery` never requires a port and never opens a serial
device itself — see Power events below.

### Power events

`printerctl power low-battery` is the command an external notifier (the
APCUPSD script at `scripts/apcupsd/printerctl-low-battery.sh`, see
`docs/scripts/printerctl-low-battery.md`) runs when the UPS reports a
low battery. During M22 it looks up the daemon process currently
streaming a print — via a live marker at
`$XDG_STATE_HOME/printerctl/active-print.pid`, published only while a
print streams — and delivers it `SIGPWR`, which the daemon handles as a
power-failure cancellation. No active print is not an
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
| `9` | Print observed to end cancelled (e.g. another client ran `cancel`) |
| `10` | Print observed to end cancelled by a power-failure/low-battery signal |

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
./build/printerctld --port /dev/ttyACM0
./build/printerctl temp
./build/printerctl position
./build/printerctl info
./build/printerctl send "M115"
./build/printerctl sd pause
./build/printerctl sd resume
./build/printerctl sd cancel
./build/printerctl power low-battery
./build/printerctl home
./build/printerctl home X
```

## Versioning

The project version is declared once, in `CMakeLists.txt`, via
`project(printerctl VERSION ...)`. It is exposed to C code through a header
generated at configure time (`include/printerctl/version.h`, produced from
`include/printerctl/version.h.in`), so there is no second place where the
version number needs to be kept in sync manually.
