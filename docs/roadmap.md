---
title: printerctl Phase 1 Roadmap
tags: [printerctl, roadmap, phase-1, daemon, ipc]
status: active
updated: 2026-08-21
---

# printerctl — Phase 1 Roadmap

## 1. Purpose

This document defines the executable roadmap for **Phase 1 — Daemon Foundation + Single-Print IPC**.

Phase 1 is intentionally narrower than the previous Phase II roadmap. Its purpose is to validate the process boundary, daemon runtime, local IPC transport and single-print execution before adding job queues, new user interfaces or multi-printer scheduling.

The phase must preserve the currently working CLI path while the daemon path is introduced incrementally.

## 2. Authority and supersession

This Phase 1 roadmap is the current execution plan for milestones M13–M17.

It **SUPERSEDES** the previous definitions of M13–M17 in the older roadmap where those milestones represented, among other things, job queue, text persistence, socket protocol, socket server and daemon lifecycle.

Historical roadmap documents must not be deleted merely because they are superseded. They remain useful for understanding previous design intent.

The following prior decisions remain valid unless explicitly superseded here:

- M12 print engine extraction is complete and remains the foundation for daemon integration.
- the print engine remains independent from CLI, TUI, HTTP, JSON and socket protocol concerns;
- Marlin streaming remains ACK-driven with one command in flight;
- safety constraints remain unchanged;
- no automatic print resume is permitted;
- serial ownership must ultimately converge on `printerctld`, but the legacy CLI path remains temporarily functional during Phase 1.

## 3. Phase objective

At Phase 1 completion the project must support the following architecture:

```text
legacy printerctl CLI ───────────────┐
                                     │ temporary compatibility path
                                     ▼
                                  existing core/printer/serial

local test/client
      │
      │ Unix Domain Socket
      ▼
 printerctld
      │
      ├── control context
      │     ├── socket server
      │     ├── protocol dispatch
      │     └── published state
      │
      ├── cmd_pipe ───────────────► print worker
      │                                │
      ◄────────────── event_pipe ──────┘
                                       │
                                       ▼
                                  print_engine
                                       │
                                    printer
                                       │
                                     serial
                                       │
                                     Marlin
```

The critical property to validate is:

```text
client lifetime != print lifetime
```

A print started through `printerctld` must be able to continue after the initiating client disconnects, while another client can later inspect or control the active session.

## 4. Phase boundaries

### In scope

- daemon/runtime boundary;
- worker thread model;
- internal pipe IPC;
- local binary protocol design;
- Unix Domain Socket transport;
- single active print execution through the existing print engine;
- status/control of that single active session;
- lifecycle, fault and shutdown validation;
- preservation of the legacy CLI path during migration.

### Out of scope

The following capabilities are explicitly deferred:

- CLI migration to socket-only operation — **Phase 2**;
- persistent job model and print queue — **Phase 3**;
- persistent queue/history semantics — **Phase 3**;
- TUI — **Phase 4**;
- multi-printer support — **Phase 5**;
- automatic print recovery or resume;
- multiple simultaneous active prints in one printer context;
- Web/API gateway;
- event subscription protocol;
- timing prediction algorithms;
- line numbering/checksums/automatic `Resend:` recovery.

## 5. Dependency graph

```text
M12 — Print Engine Extraction [DONE]
 │
 ▼
M13 — Daemon Boundary & Runtime Model [DONE]
 │
 ▼
M14 — Printerctl Local Protocol v1 [DONE]
 │
 ▼
M15 — Unix Socket Transport
 │
 ▼
M16 — Single-Print Daemon Integration
 │
 ▼
M17 — Lifecycle, Fault & Compatibility Validation
 │
 ▼
PHASE 1 COMPLETE
```

M13 establishes internal concurrency and ownership.
M14 establishes the external wire contract.
M15 validates the transport without hardware.
M16 connects the daemon runtime to the existing print engine.
M17 closes fault, shutdown, power and compatibility behavior.

---

# M13 — Daemon Boundary & Runtime Model

**Status: DONE** — implemented and verified 2026-08-21. See
[[milestone-13|docs/milestones/milestone-13.md]] and
[[milestone-13-daemon-runtime|docs/design/milestone-13-daemon-runtime.md]]
for the as-built report, verification results and the specific points
where the implementation had to make a call this section left open
(fd ownership under a shared-fd-table thread model, the `FAULTED`
state also being reachable from `READY`, and the worker-initialization
fault-injection seam).

## Goal

Create the hardware-independent runtime foundation for `printerctld`.

The runtime must introduce:

- a control context;
- one worker thread;
- `cmd_pipe` from control to worker;
- `event_pipe` from worker to control;
- explicit daemon/runtime lifecycle;
- explicit worker lifecycle;
- internal command/event contracts;
- internal correlation tokens;
- a published runtime snapshot;
- deterministic startup, shutdown and fault propagation.

## Why now

The existing print engine is intentionally blocking. Running it directly in the future socket/control thread would make the daemon unresponsive during long prints.

M13 resolves this architectural constraint before any socket or hardware integration is introduced.

## Dependencies

- M12 print engine design and as-built documentation;
- existing C17/CMake/test conventions;
- existing logging/config conventions where applicable.

M13 must not depend on the external socket protocol.

## Architecture

```text
CONTROL CONTEXT
      │
      │ internal command
      ▼
   cmd_pipe
      │
      ▼
PRINT WORKER
      │
      │ internal event/result
      ▼
 event_pipe
      │
      ▼
CONTROL CONTEXT
```

### Confirmed ownership

Control context owns:

- daemon/runtime lifecycle;
- published runtime snapshot;
- worker availability;
- command dispatch;
- future socket resources.

Worker owns:

- future `print_engine` instance;
- future execution mutable state;
- future printer/serial execution path.

The control context must never directly mutate the future print engine.
The worker must never directly mutate the client-facing snapshot.

### Confirmed internal IPC

Use two unidirectional Unix pipes:

```text
cmd_pipe   : control → worker
event_pipe : worker  → control
```

M13 internal pipe messages are not the external Printerctl Local Protocol.

### Minimum internal commands

- `NOP` or equivalent test command;
- `SHUTDOWN`.

### Minimum worker events

- `READY`;
- `COMMAND_COMPLETED`;
- `STOPPING`;
- `STOPPED`;
- `FAULT`.

### Worker lifecycle

```text
INITIALIZING
     │
 ┌───┴────┐
 ▼        ▼
READY   FAULTED
 │
 ▼
STOPPING
 │
 ▼
STOPPED
```

### Runtime lifecycle

```text
STARTING
RUNNING
STOPPING
STOPPED
FAILED
```

Worker state and runtime state are separate dimensions.

## Scope

- runtime abstraction;
- thread creation/join;
- two pipes;
- internal framing/validation;
- correlation token;
- startup handshake;
- shutdown handshake;
- runtime snapshot;
- failure propagation;
- deterministic resource cleanup;
- hardware-independent tests.

## Out of scope

- AF_UNIX;
- SOCK_STREAM;
- external framing;
- external protocol commands;
- print engine integration;
- serial/Marlin/G-code;
- queue/job model/persistence.

## Failure model

At minimum, handle explicitly:

- first pipe creation failure;
- second pipe creation failure;
- worker creation failure;
- worker initialization failure;
- malformed internal message;
- command-pipe EOF;
- event-pipe EOF;
- unexpected worker termination;
- shutdown during startup;
- repeated shutdown;
- deterministic cleanup after partial initialization.

Automatic worker restart is prohibited.

## Tests

Required categories:

- unit tests for lifecycle transitions;
- unit tests for internal command/event representation;
- integration test for startup handshake;
- NOP round-trip correlation test;
- normal shutdown test;
- worker initialization fault test;
- pipe closure/fault tests;
- repeated start/stop tests;
- sanitizer runs where available, especially ThreadSanitizer.

All M13 tests must be runnable without a printer.

## Acceptance criteria

M13 is accepted only if:

- the runtime creates and joins one worker deterministically;
- two independent unidirectional pipes exist;
- startup reaches RUNNING only after worker READY;
- NOP performs a correlated control→worker→control round trip;
- normal shutdown traverses STOPPING before STOPPED;
- resource ownership and cleanup are deterministic on every error path;
- worker faults become visible in the runtime snapshot;
- no automatic worker restart exists;
- no serial port is opened;
- no G-code is sent;
- `print_engine` is not integrated or modified for daemon concerns;
- no Unix socket is implemented;
- no queue or new job persistence is introduced;
- the existing test suite continues to pass.

## Exit condition

A repeatable hardware-independent test demonstrates:

```text
create runtime
→ worker INITIALIZING
→ worker READY
→ runtime RUNNING
→ NOP #N
→ COMMAND_COMPLETED #N
→ SHUTDOWN
→ worker STOPPING
→ worker STOPPED
→ join
→ runtime STOPPED
```

---

# M14 — Printerctl Local Protocol v1

**Status: DONE** — implemented and verified 2026-08-21. See
[[milestone-14|docs/milestones/milestone-14.md]] and
[[protocol-v1|docs/design/protocol-v1.md]] (the durable wire spec, not a
per-milestone note) for the as-built report, verification results and
the field-width/error-classification decisions the implementation had
to make that this section left open.

## Goal

Define the versioned binary protocol used by local clients to communicate with `printerctld` over a Unix stream socket.

M14 is a protocol design milestone. The transport implementation belongs to M15.

## Requirements

The protocol must be:

- binary;
- length-prefixed;
- versioned;
- bounded;
- fragmentation-safe;
- independent of C struct memory layout;
- capable of request/result correlation;
- extensible without ambiguous field interpretation;
- suitable for C17 clients and daemon implementation.

## Confirmed framing direction

A frame begins with a four-byte length field.

```text
┌─────────────────────────────┐
│ uint32 frame_length         │
├─────────────────────────────┤
│ fixed binary header         │
├─────────────────────────────┤
│ optional payload            │
└─────────────────────────────┘
```

`frame_length` means the number of bytes following the four-byte length field.

Do not alternate between total-frame length and header-only length semantics.

## Confirmed protocol concepts

The header must provide concepts equivalent to:

- protocol version;
- numeric message type;
- flags bitmap;
- request/correlation ID;
- optional payload following the header.

Exact field widths beyond the already confirmed 4-byte frame length belong in the M14 protocol design document and must be frozen before M15 implementation.

## Encoding rules

Approved direction:

- multi-byte integers use an explicitly documented byte order; network byte order/big-endian is the recommended default;
- strings are length-prefixed, not NUL-terminated on wire;
- mutually exclusive domain states use enums;
- orthogonal properties/features use bitmaps;
- maximum frame size is mandatory;
- unknown/unsupported versions and message types produce explicit protocol errors;
- a peer must never allocate directly from an unvalidated frame length.

## Bitmaps

Use bitmaps for orthogonal features or optional fields, for example:

```text
00000001 → feature/field A
00000010 → feature/field B
00000100 → feature/field C
```

Do not encode mutually exclusive states such as PRINTING and PAUSED as independent bits.

Separate:

- per-frame flags;
- peer/protocol capability bits.

Capabilities may be reserved in v1 even if only a minimal subset is initially used.

## Minimum Phase 1 semantics

The protocol must support semantic operations equivalent to:

- `PING`;
- `VERSION`;
- `STATUS`;
- `PRINT_START`;
- `PRINT_PAUSE`;
- `PRINT_RESUME`;
- `PRINT_CANCEL`.

Numeric message identifiers must be stable once published and must not be reused for unrelated operations.

## Explicitly excluded protocol families in Phase 1

Do not define yet:

- `JOB_*`;
- `QUEUE_*`;
- `HISTORY_*`;
- event subscriptions;
- multi-printer routing;
- WebSocket/HTTP representations.

## Parser requirements for M15

M14 must specify enough behavior that M15 can correctly handle:

```text
read #1: half a length prefix
read #2: remaining prefix + partial frame
read #3: remaining frame + complete next frame + partial third frame
```

The protocol cannot rely on `write()` boundaries matching `read()` boundaries.

## Safety

Protocol query operations must never cause implicit:

- homing;
- movement;
- heating;
- extrusion;
- emergency stop;
- print resume.

`PRINT_START`, pause/resume and cancel are explicit operator intents and must retain the safety semantics of the existing print engine when integrated in M16.

## Acceptance criteria

M14 is complete when a design document specifies unambiguously:

- frame-length semantics;
- header layout and field widths;
- byte order;
- version compatibility behavior;
- message-type registry;
- flags/capability registry;
- request ID semantics;
- payload definitions for Phase 1 operations;
- error representation;
- maximum frame size;
- malformed/truncated/oversized-frame behavior;
- parser expectations for fragmented and coalesced stream reads.

Two independent implementation agents should be able to implement a client encoder and server decoder from the document without informal coordination.

---

# M15 — Unix Socket Transport

**Status: DONE**

## Goal

Implement and validate the Unix Domain Socket transport and the M14 protocol parser/serializer without involving printer hardware or the print engine.

## Dependencies

- M13 runtime foundation;
- frozen M14 protocol design.

## Scope

- `AF_UNIX`;
- `SOCK_STREAM`;
- socket creation/bind/listen/accept;
- socket path lifecycle;
- permissions;
- stale-socket handling;
- client connection lifecycle;
- stream receive buffers;
- M14 frame parsing;
- response serialization;
- bounded input;
- commands that do not require hardware, such as protocol `PING`, `VERSION` and idle `STATUS`.

## Out of scope

- print-engine integration;
- serial;
- real `PRINT_START`/pause/resume/cancel execution;
- queue/jobs/history.

## Failure model

At minimum:

- bind failure;
- permission failure;
- live socket path already in use;
- stale socket;
- partial client frame;
- malformed frame;
- oversized frame;
- unsupported version/type;
- client EOF;
- write failure;
- client crash.

Never blindly unlink a socket path without determining whether a live instance owns it.

## Tests

- parser/serializer unit tests;
- fragmented frame tests;
- multiple frames per read;
- malformed/truncated/oversized frames;
- real AF_UNIX integration tests;
- disconnect/reconnect;
- multiple sequential clients;
- concurrency behavior if multiple simultaneous clients are supported by the selected runtime design.

## Acceptance criteria

- daemon runtime can start without serial;
- local socket accepts connections;
- protocol `PING` works;
- `VERSION` works;
- `STATUS` can report runtime/idle state;
- fragmented and coalesced frames are handled correctly;
- malformed clients do not crash the daemon;
- disconnecting a client does not terminate the runtime;
- no hardware is touched.

---

# M16 — Single-Print Daemon Integration

**Status: DONE**

## Goal

Connect the daemon runtime and protocol to the existing print engine so that exactly one active print session can be initiated, observed and controlled through `printerctld`.

## Dependencies

- M13 runtime;
- M14 protocol;
- M15 Unix socket transport;
- existing M12 print engine.

## Architecture

```text
client
  │
  ▼
M14 protocol / M15 socket
  │
  ▼
control context
  │
  ▼
internal command
  │
  ▼
cmd_pipe
  │
  ▼
print worker
  │
  ▼
print_engine
  │
  ▼
printer
  │
  ▼
serial / Marlin
```

Engine events/results flow back through `event_pipe` and update the control-owned published snapshot.

## Scope

- start one print through daemon IPC;
- status snapshot for active execution;
- pause;
- resume;
- cancel;
- reject a second print while one is active;
- preserve client/print lifetime independence;
- map engine events/results into daemon-visible state;
- reuse current serial/Marlin/printer behavior without duplicating the streamer.

## Out of scope

- queueing a second print;
- persistent job identity;
- durable queue/history;
- CLI migration;
- TUI;
- multi-printer.

## State authority

- print engine remains authority for execution lifecycle;
- daemon/control context owns the published snapshot and service availability state;
- daemon must derive execution visibility from engine/worker events rather than inventing a competing mutable state machine.

## Required behavior

If print A is active and another `PRINT_START` arrives:

```text
PRINT_START B
→ explicit busy/invalid-state error
```

It must not silently create a queue.

Client disconnect after accepted start:

```text
client A → PRINT_START
client A disconnects
printerctld continues print
client B connects
client B → STATUS
```

This is a core acceptance scenario.

## Observability

Expose only data that can be obtained reliably from the current engine. Candidate fields include:

- execution state;
- source path;
- started time;
- elapsed time;
- progress;
- commands sent;
- commands acknowledged;
- current source position;
- failure/cancellation reason.

Do not fabricate fields solely to match the future persistent job model.

## Safety

Preserve all existing behavior:

- no automatic home;
- no automatic movement;
- no automatic heating;
- no automatic extrusion;
- no automatic emergency stop;
- no automatic resume;
- embedded/disallowed G28 behavior remains unchanged;
- cancellation stops transmission without adding physical cleanup commands;
- `Error:`, `Resend:`, timeout and disconnect retain their current terminal semantics.

## Acceptance criteria

- a print can be started through IPC;
- daemon remains responsive during the print;
- initiating-client disconnect does not cancel the print;
- another client can query state;
- pause/resume/cancel work through daemon control;
- second start is rejected, not queued;
- existing print engine is reused;
- streamer is not duplicated;
- no new automatic physical behavior exists;
- existing and new tests pass.

---

# M17 — Lifecycle, Fault & Legacy Compatibility Validation

**Status: DONE**

## Goal

Close all lifecycle, power, serial-failure and compatibility scenarios required to declare Phase 1 production-ready for the next migration phase.

## Dependencies

- M13–M16 complete.

## Scope

- daemon shutdown while idle;
- daemon shutdown during print;
- worker fault behavior with integrated engine;
- client failures during active print;
- serial disconnect;
- Marlin `Error:`;
- `Resend:` terminal behavior;
- timeout;
- low-battery/SIGPWR behavior;
- active-print marker behavior;
- legacy CLI coexistence;
- regression and fault testing.

## Shutdown safety

During an active print, daemon termination must perform coordinated safe cancellation/cleanup through the existing lifecycle rather than abruptly killing the worker.

No automatic G-code cleanup sequence may be introduced.

## Power failure

The existing APCUPSD integration must continue to provide the low-battery cancellation guarantee for a daemon-owned print.

The implementation may adapt how the active process is identified, but it must preserve these semantics:

- stop sending new G-code;
- distinguish power-failure cancellation from operator cancellation where the existing lifecycle supports it;
- close resources;
- record interrupted-session information according to existing policy;
- never auto-resume when power returns.

## Transitional serial ownership

During Phase 1 the legacy CLI remains capable of opening serial when `printerctld` does not own it.

The transitional invariant is:

```text
at most one effective serial owner at a time
```

Existing exclusive serial behavior such as `TIOCEXCL` must continue to prevent competing active owners.

The stronger invariant:

```text
only printerctld may request serial
```

belongs to Phase 2.

## Critical fault scenarios

Validate at minimum:

- client crashes immediately after print acceptance;
- client disconnects during a control request;
- malformed client requests during active print;
- serial disconnect;
- Marlin `Error:`;
- `Resend:`;
- response timeout;
- SIGPWR;
- SIGTERM;
- worker failure;
- socket write failure while print execution remains valid.

A client transport failure must not automatically become a print failure.

## Acceptance criteria

Phase 1 closes only when all are true:

1. `printerctld` can execute one print using the existing engine.
2. The daemon remains responsive during the print.
3. Client lifetime is independent from print lifetime.
4. At most one effective serial owner exists.
5. Legacy CLI remains functional when it owns serial and the daemon does not.
6. Low-battery cancellation semantics remain intact.
7. SIGTERM/shutdown produces coordinated safe termination.
8. `Error:`, `Resend:`, timeout and disconnect semantics are preserved.
9. No new automatic physical action exists.
10. No hidden queue or durable job persistence was introduced.
11. Local protocol v1 is versioned and documented.
12. Existing regression suite and Phase 1 tests pass.

## Exit condition

A representative end-to-end test can demonstrate:

```text
client A connects
→ starts safe test print
→ daemon accepts
→ client A disconnects

client B connects
→ STATUS = printing
→ PAUSE
→ STATUS = paused
→ RESUME
→ STATUS = printing
→ CANCEL
→ STATUS = cancelled

printerctld remains alive throughout
```

Equivalent PTY/mock-Marlin fault tests must cover the required failure paths before hardware smoke tests are considered.

---

# 6. Phase completion and next phases

After M17 is accepted, Phase 1 is complete. M17 is implemented as of
2026-08-25; see [[milestone-17|docs/milestones/milestone-17.md]].

The project then proceeds independently through:

```text
Phase 2 — CLI Migration
printerctl → Unix socket → printerctld
printerctld becomes the normal exclusive serial authority

Phase 3 — Job Management
persistent job model + queue + history

Phase 4 — TUI
second full client of the daemon API

Phase 5 — Multi-Printer
multiple printer contexts and scheduling policy
```

Phase 1 must not pre-implement these capabilities merely because they are known future requirements.
