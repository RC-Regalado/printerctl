---
title: Unix Socket Service Analysis
status: superseded
date: 2026-07-10
tags: [printerctl, design, daemon, unix-socket, docker, web]
---

# Unix Socket Service Analysis

**Status (Milestone 11, 2026-07-21): superseded in its protocol
specifics.** The revised `roadmap.md` now formally schedules the daemon
transition as Phase II (Milestones 12-27: extract the print engine,
then build `printerctld` and its Unix socket, then the TUI and web
gateway), with a RESP-inspired frame protocol rather than this
document's proposed `key=value` frames. The analysis below correctly
anticipated the daemon-owns-the-serial-port shape, the client/server
split, and most of the safety rules Phase II must preserve — kept here
as background, not as the current protocol design. See `roadmap.md`'s
Phase II section and [[architecture]] §13 item 6 for the current plan.

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

## 1. Conclusion

Unix domain socket control is viable and is the right direction for a
web/Docker consumer, but it should be introduced as its own daemon
milestone before expanding automation. It is not a small Milestone 10
APCUPSD detail.

Recommended shape:

```text
web container
  |
  | bind-mounted AF_UNIX socket
  v
printerctld daemon  ---- owns ----> /dev/ttyUSB0
  |
  v
existing printer/config/logging/gcode/record modules
```

The daemon, not the web container, owns the serial device. The web
container receives only a socket mount and optional upload/spool
directory access.

## 2. Why this fits the current architecture

Current milestones already exposed the right pressure:

- Milestone 7 streams with one command in flight.
- Milestone 8 proved that a second CLI process cannot control a host
  stream without shared process state.
- Milestone 8 added `TIOCEXCL` because two processes talking to one
  serial port corrupt the protocol.
- Milestone 9 records interruptions but explicitly does not resume
  automatically.

A daemon resolves the process-state gap cleanly: one long-lived process
owns the serial session, active print state, pause/resume/cancel
controls, progress, logs and interrupted-session records.

## 3. What should change

Add a new executable, likely `printerctld`, with these responsibilities:

- open and hold the serial port;
- accept local clients over an AF_UNIX `SOCK_STREAM` socket;
- serialize printer operations through one owner;
- stream status/progress events to connected clients;
- expose pause/resume/cancel for host-stream sessions without POSIX
  signals;
- keep existing safety gates: no automatic home, heat, motion,
  extrusion, reconnect or resume.

Keep `printerctl` as a CLI. It can either keep direct mode for simple
local use or become a thin client when `--socket PATH` is provided.

## 4. Minimal IPC protocol

Use a dependency-free line protocol, not HTTP and not a JSON parser yet.
The project already uses simple text formats (`key=value` config and
records), and C17 has no JSON dependency today.

Request frame:

```text
id=1
command=status

```

Response frame:

```text
id=1
ok=true
state=idle

```

Event frame:

```text
event=progress
state=printing
progress_percent=42
last_acknowledged_line=1203

```

Blank line terminates a frame. Unknown keys are ignored by clients;
unknown commands fail with an explicit error. Add JSON only when a real
consumer proves the text protocol is insufficient.

## 5. Initial command surface

Start small:

| Command | Purpose | Notes |
|---|---|---|
| `status` | report daemon/printer state | safe, read-only |
| `temp` | query `M105` | safe typed command |
| `position` | query `M114` | safe typed command |
| `info` | query `M115` | safe typed command |
| `print` | start host-stream print from an approved path | daemon owns session |
| `pause` | pause active host stream | no Marlin command sent |
| `resume` | resume active host stream | continues next unsent command |
| `cancel` | cancel active host stream | graceful stop + record |

Defer raw `send` over the socket until authentication/authorization is
settled. `send` is powerful enough to move, heat or emergency-stop the
printer.

## 6. Docker/web boundary

Do not pass `/dev/ttyUSB0` into the web container. Mount only:

- the Unix socket, for example `/run/printerctl/printerctld.sock`;
- a controlled spool directory for uploaded G-code files, if needed.

The daemon should validate that `print` paths are regular files inside
the configured spool directory or another explicit allowlist. This keeps
the web service from asking the daemon to read arbitrary host files.

Socket permissions should be filesystem-based:

- runtime dir mode `0750`;
- socket mode `0660`;
- owner `printerctl`;
- group `printerctl` or another operator-controlled group.

Use `SO_PEERCRED` for audit/logging and optional local authorization.

## 7. Safety rules that remain mandatory

- No automatic resume after power restoration.
- No automatic reconnect mid-print.
- No automatic homing, movement, heating, extrusion or parking.
- `G28` remains blocked in streamed files.
- `M112` is never generated automatically.
- One serial owner at a time.
- A failed/cancelled/interrupted print writes a record.

Automation must mean "scheduled explicit operator policy", not hidden
recovery logic.

## 8. Impact on Milestone 10

Milestone 10 currently targets APCUPSD informational records. If a
daemon is accepted, the better sequence is:

1. finish and commit Milestone 9;
2. add a new daemon/socket foundation milestone;
3. implement APCUPSD against the daemon/record model.

Otherwise Milestone 10 will likely implement APCUPSD in CLI-local shape
and then be refactored when the daemon arrives.

## 9. Acceptance criteria for the daemon milestone

- daemon starts, creates the socket and rejects world-writable access;
- one client can query `status`;
- multiple clients can connect without corrupting printer state;
- only one print can be active;
- `print` streams through the existing one-ack-at-a-time path;
- socket `pause`/`resume`/`cancel` control the active host stream;
- web/Docker can consume the socket through a bind mount without serial
  device access;
- interrupted sessions still write records;
- tests use Unix socket pairs or temporary socket paths, no hardware.

## 10. Main risks

- Authorization: a mounted socket is printer control. Filesystem
  permissions and peer credentials are required from day one.
- File access: `print path=/...` can become arbitrary host-file access
  unless paths are restricted.
- Protocol creep: implementing HTTP/JSON/WebSocket in C now adds more
  surface than the project needs. Keep the daemon local and simple; let
  the web service translate HTTP/WebSocket to the Unix socket protocol.
- State ownership: direct CLI serial mode and daemon mode can conflict.
  Prefer daemon mode for web control; keep direct mode only for explicit
  maintenance or when the daemon is stopped.

