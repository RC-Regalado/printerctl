---
title: Milestone 15 — Unix Socket Transport Design
milestone: 15
status: done
date: 2026-08-25
tags: [printerctl, daemon, socket, protocol, plp1]
---

# Milestone 15 — Unix Socket Transport Design

Source of truth: [[Agents]], [[docs/architecture|architecture]],
[[docs/roadmap|roadmap]], [[protocol-v1|docs/design/protocol-v1.md]].

## Scope implemented

M15 adds the first control-context transport:

```text
AF_UNIX/SOCK_STREAM client
        │
        ▼
printerctl_daemon_socket_serve_one()
        │
        ▼
PLP/1 parser + dispatcher
        │
        ▼
M13 runtime snapshot
```

Implemented commands:

- `PING` → empty success response;
- `VERSION` → protocol version `1`, capability bitmap
  `PRINTERCTL_PLP1_CAP_PRINTER_STATUS`;
- `STATUS` → reports only fields owned by M13:
  `daemon_state` and `worker_state`.

Requested STATUS fields not owned by M13/M15, such as progress or
execution state, are omitted from `present_fields`; the transport does
not synthesize placeholder printer data.

Out of scope remains out of code: no `print_engine`, serial, Marlin,
G-code execution, queue, persistence, event subscriptions, web API or CLI
migration.

## Runtime model

`printerctl_daemon_socket_server_t` is owned by the control context and
holds:

- listening socket fd;
- bound socket path;
- ownership flag for unlink-on-close;
- pointer to the M13 runtime snapshot owner.

The server is intentionally single-client-at-a-time for M15. Sequential
clients are supported by repeated calls to
`printerctl_daemon_socket_serve_one()`. Simultaneous clients are deferred
until a milestone actually needs them; this avoids introducing a second
concurrency model before print execution exists.

## Socket path lifecycle

Before binding, `printerctl_daemon_socket_listen()` checks the existing
path:

- missing path: bind normally;
- non-socket path: reject;
- socket path with successful `connect()`: reject as live owner;
- socket path with `ECONNREFUSED` or `ENOENT`: treat as stale and unlink;
- other probe errors: reject.

The implementation never blindly unlinks a socket path.

The created socket is chmodded to `0600`. On close, the server closes its
listening fd and unlinks only the path it successfully bound.

## Stream handling

The transport uses the M14 incremental parser. It does not assume one
`read()` equals one frame:

- fragmented frames are accumulated;
- coalesced frames are drained in order;
- oversized or structurally malformed streams close only that client;
- client EOF ends that client session and leaves the runtime alive.

Responses are serialized through `printerctl_plp1_encode_frame()` and
written with an EINTR-aware full-write loop.

## Failure semantics

Transport failures are local to the transport unless they indicate API
misuse or an I/O failure in the active client session. Malformed clients
are contained by dropping the connection; the listening socket and M13
runtime remain usable for later clients.

M15 does not automatically restart workers, mutate worker-owned state, or
translate protocol commands into print execution.
