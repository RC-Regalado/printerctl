# Phase 2 Contracts

## Configuration

Current example:

```ini
port=/dev/ttyACM0
baud=115200
timeout=5000
dry_run=false
verbose=false
```

Daemon owns:

```text
port
baud
timeout
```

Client owns:

```text
dry_run
verbose
```

`timeout` continues to mean Marlin response timeout, not PLP/socket timeout.

M18 provides owner-aware views over the shared parser. Until each command
migrates, unmigrated direct CLI commands may use the transitional legacy
hardware-config view. Migrated commands must not use that view or fall
back to direct serial.

## Dry-run

`printerctl --dry-run print FILE` must:

- parse/normalize locally;
- work with daemon down;
- never require daemon connection;
- never open/write serial.

## Print source

- CLI may normalize/canonicalize path;
- daemon validates/opens authoritatively;
- client prevalidation is UX only;
- Phase 2 does not require streaming file contents through PLP.

## Exit codes

Preserve baseline:

```text
0  success
2  usage error
3  configuration error
4  safety policy rejection
5  parsed successfully but unavailable
6  G-code file error
7  communication error
8  printer error
9  operator-cancelled host print
10 power-cancelled host print
```

Do not collapse all daemon errors into exit 7.

Map deliberately:

- daemon/socket unavailable;
- protocol mismatch;
- file/source failure;
- serial failure;
- printer/Marlin failure;
- safety rejection;
- busy;
- operator cancel;
- power cancel.

## Host vs SD controls

Host:

```text
pause
resume
cancel
```

SD:

```text
sd pause
sd resume
sd cancel
```

No state-dependent/context-sensitive dispatch.

## Raw send

`G28` is always rejected through generic raw send.

No bypass flag.

## Home

`home`, `home X`, `home Y`, `home Z` are explicit physical-intent operations.

Homing must never occur automatically at startup, print start, reconnect, recovery, or raw-send bypass.

## Power

`power low-battery` remains idempotent.

No active print is not an error.

Power restoration never causes automatic resume.

## Signals

The daemon is execution owner.

Client PID signals are UI concerns, not ownership.

For attached print:

- SIGINT/Ctrl+C may request PLP cancel;
- SIGTERM may request PLP cancel if compatibility contract requires it;
- SIGUSR1/SIGUSR2 are not part of the long-term service control model.

## PLP framing

Do not change existing PLP/1 semantics:

```text
offset  size
0       4  frame_length
4       1  version
5       1  message_type
6       2  flags
8       4  request_id
12      2  status
14      2  reserved
16      N  payload
```

- frame_length excludes its 4-byte field;
- big-endian;
- version 1;
- bounded maximum frame;
- explicit unknown-version/type errors;
- incremental parser.

New message types require stable numeric IDs and golden-vector tests.
