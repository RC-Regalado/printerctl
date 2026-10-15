---
title: printerctl Phase 1 Architecture
tags: [printerctl, architecture, phase-1, daemon, ipc, safety]
status: active
updated: 2026-08-21
---

# printerctl — Phase 1 Architecture

## 1. Purpose

This document defines the architecture for **Phase 1 — Daemon Foundation + Single-Print IPC**.

It is intentionally scoped to the transition from a CLI-owned execution model toward a service boundary while preserving the existing print engine and legacy CLI path.

The architectural objective is not yet to build a complete print-management platform. Phase 1 validates the process boundary and establishes a stable base for later CLI migration, job queues, TUI and multi-printer support.

## 2. Current implemented baseline

The current system is a Linux C17 host application for Marlin printers over USB serial.

The relevant implemented layering is:

```text
CLI
 │
 ├── Config
 ├── Logging
 ├── Record
 │
 └── core / print_engine
          │
          ▼
        printer
       /   |    \
   gcode marlin serial
                  │
                  ▼
               Marlin
```

Milestone 12 extracted the print lifecycle into `core` / `print_engine`.

The engine:

- owns application-level print lifecycle;
- reuses `printer` for streaming and printer interaction;
- emits events;
- does not render terminal UI;
- does not know CLI, TUI, HTTP, JSON or Unix socket protocols;
- must remain independent of future transport concerns.

This separation is a core architectural invariant of Phase 1.

## 3. Existing layer responsibilities

### CLI

- parse operator intent;
- validate command-line input;
- render human-readable output;
- map application results to exit codes.

### Core / print engine

- orchestrate a single print lifecycle;
- connect/disconnect through existing lower layers;
- manage active-print marker/session recording according to current implementation;
- emit lifecycle/progress/failure events;
- remain interface-agnostic.

### Printer

- printer session semantics;
- single-command ACK-driven streaming;
- pause/resume/cancel semantics already supported by the existing engine/printer design;
- Marlin command/query orchestration.

### G-code

- safe file reading/normalization;
- source/progress tracking;
- file validation and caller-selected G28 policy.

### Marlin

- classify and parse `ok`, `Error:`, `busy:`, `echo:`, `Resend:` and informational responses;
- preserve one-command-in-flight acknowledgement behavior.

### Serial

- Linux serial transport;
- configuration/timeouts/I/O status;
- exclusive device behavior such as `TIOCEXCL`.

### Record/APCUPSD integration

- interrupted-session records according to existing behavior;
- active-print process marker;
- low-battery notification leading to safe transmission cancellation;
- never acts as automatic recovery/resume machinery.

## 4. Phase 1 target architecture

During Phase 1 two execution paths coexist temporarily.

```text
                            TEMPORARY LEGACY PATH

printerctl CLI ───────────────► core/print_engine ─► printer ─► serial


                             NEW DAEMON PATH

local client
    │
    │ Printerctl Local Protocol v1
    │ over AF_UNIX/SOCK_STREAM
    ▼
printerctld
    │
    ├── control context
    │     ├── socket lifecycle
    │     ├── protocol parser/serializer
    │     ├── request dispatch
    │     └── published runtime/execution snapshot
    │
    │        cmd_pipe
    ├──────────────────────────────► print worker
    │                                  │
    │                                  ▼
    │                              print_engine
    │                                  │
    │                               printer
    │                                  │
    │                                serial
    │                                  │
    │                                Marlin
    │
    │       event_pipe
    ◄──────────────────────────────────┘
```

The legacy path exists only to preserve incremental compatibility during Phase 1.

It is not the final ownership architecture.

## 5. Architectural invariants

### 5.1 Core isolation

The dependency direction must remain:

```text
printerctld → core → printer → serial
```

Never:

```text
core → printerctld
printer → printerctld
serial → printerctld
```

The core must not parse socket frames or know protocol message IDs.

### 5.2 One execution worker

Phase 1 contains one print worker per daemon runtime.

No worker pool, scheduler or multi-printer worker set is introduced.

### 5.3 Single active execution

At most one print may be active through the daemon.

A second start request is rejected explicitly rather than queued.

### 5.4 No hidden queue

No data structure or persistence behavior may implicitly create queued work in Phase 1.

### 5.5 Client lifetime independence

Once the daemon has accepted a print start and the execution has begun, disconnecting the initiating client must not automatically cancel the print.

### 5.6 Safety first

No new component may automatically generate:

- homing;
- movement;
- heating;
- extrusion;
- emergency stop;
- print resume after interruption/recovery.

Safety filtering and existing engine behavior must remain authoritative.

## 6. Runtime concurrency model

### 6.1 Rationale

The existing print engine is blocking for the duration of a print lifecycle.

Executing it in the same context that services local clients would make `printerctld` unable to respond to state/control requests during a print.

Phase 1 therefore adopts one dedicated worker thread.

### 6.2 Control context ownership

The control context owns:

- daemon/runtime lifecycle;
- Unix socket descriptors once M15 is implemented;
- protocol parsing and serialization;
- mapping external requests to internal commands;
- client-facing published snapshot;
- worker availability state.

The control context does **not** directly mutate the print engine.

### 6.3 Worker ownership

The print worker owns:

- future `print_engine` instance;
- future printer execution state;
- future active printer session through existing modules;
- future serial ownership acquired by the normal engine/printer path.

The worker does not directly modify the public/client-facing snapshot.

### 6.4 Internal communication

Two unidirectional pipes are confirmed:

```text
cmd_pipe   : control → worker
event_pipe : worker  → control
```

The control context translates an external request into an internal command.

```text
wire frame
    │
    ▼
validate / parse
    │
    ▼
internal command
    │
    ▼
cmd_pipe
    │
    ▼
worker
```

The reverse path is:

```text
engine/worker result or event
    │
    ▼
internal event
    │
    ▼
event_pipe
    │
    ▼
control context
    │
    ├── update snapshot
    └── create protocol response if applicable
```

The external wire representation must never be passed directly to `print_engine`.

## 7. Internal command/event model

M13 introduces an internal message envelope independent of the public protocol.

Conceptually:

```text
internal_command
├── size/type metadata
├── command type
├── internal correlation token
└── command-specific data
```

and:

```text
worker_event
├── size/type metadata
├── event type
├── internal correlation token
├── result/status
└── event-specific data
```

Minimum M13 commands:

- NOP/test-equivalent;
- SHUTDOWN.

Minimum M13 events:

- READY;
- COMMAND_COMPLETED;
- STOPPING;
- STOPPED;
- FAULT.

Print-specific commands/events are added only in M16.

### Correlation

Internal tokens correlate control commands and worker results.

They are not identical by definition to external protocol request IDs.

This permits future mapping without coupling internal runtime layout to the wire protocol.

## 8. Runtime state model

### 8.1 Daemon/runtime lifecycle

```text
STARTING
  │
  ▼
RUNNING
  │
  ▼
STOPPING
  │
  ▼
STOPPED
```

Failure paths can transition to:

```text
FAILED
```

### 8.2 Worker lifecycle

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

Runtime state and worker state are separate.

For example:

```text
daemon = RUNNING
worker = FAULTED
```

is representable and diagnostically meaningful.

### 8.3 Startup handshake

`pthread_create()` success is not equivalent to a ready execution backend.

The runtime may publish RUNNING only after receiving worker READY through the internal event path.

### 8.4 Public snapshot

The control context owns the published snapshot.

M13 snapshot begins with runtime-level information such as:

- daemon/runtime state;
- worker state;
- last runtime error.

M16 extends it with execution information derived from print-engine events.

The worker must not directly write client-visible state.

## 9. Worker failure semantics

Automatic worker restart is prohibited.

Future hardware ownership makes blind restart unsafe because the system cannot assume a failed/partitioned execution context released the serial/printer lifecycle correctly.

Unexpected worker termination therefore results in explicit runtime failure/unavailability requiring controlled operator/service recovery.

## 10. Local Protocol v1

M14 defines a binary, versioned, length-prefixed protocol.

### 10.1 Framing

```text
┌─────────────────────────────┐
│ uint32 frame_length         │
├─────────────────────────────┤
│ fixed binary header         │
├─────────────────────────────┤
│ optional payload            │
└─────────────────────────────┘
```

`frame_length` is the number of bytes following the length field.

The first field therefore always has one stable meaning.

### 10.2 Header concepts

The fixed header contains, at minimum conceptually:

- protocol version;
- numeric message type;
- frame flags bitmap;
- external request/correlation ID.

Exact widths are frozen in the M14 protocol design before M15 implementation.

### 10.3 Data representation

Approved direction:

- explicitly defined byte order for every multi-byte integer, with big-endian/network order recommended;
- numeric enums for mutually exclusive states/types;
- bitmaps for orthogonal flags/capabilities;
- length-prefixed strings;
- bounded payloads;
- mandatory maximum frame size.

### 10.4 Bitmaps

Use bitmaps only where combinations are meaningful.

Good examples:

- frame has optional field X;
- peer supports capability Y;
- response has additional metadata Z.

Bad example:

```text
PRINTING | PAUSED
```

if those states are mutually exclusive.

### 10.5 Stream semantics

The parser must assume:

```text
one write != one read
```

It must support:

- partial length prefix;
- partial header/payload;
- multiple complete frames in one read;
- a complete frame followed by a partial next frame.

### 10.6 Minimum message families

Phase 1 protocol semantics include:

- PING;
- VERSION;
- STATUS;
- PRINT_START;
- PRINT_PAUSE;
- PRINT_RESUME;
- PRINT_CANCEL.

No queue/job/history/event-subscription protocol is introduced.

## 11. Unix Domain Socket transport

M15 binds Protocol v1 to:

```text
AF_UNIX + SOCK_STREAM
```

The transport layer owns:

- socket path lifecycle;
- bind/listen/accept;
- client connection state;
- buffering and frame extraction;
- protocol parser invocation;
- serialized response writes;
- stale/live socket-path distinction;
- resource cleanup.

The transport layer does not own print-domain logic.

## 12. Single-print integration

M16 connects the worker to the existing print engine.

### 12.1 Request path

```text
PRINT_START frame
      │
      ▼
protocol validation
      │
      ▼
service/control intent
      │
      ▼
internal command
      │
      ▼
cmd_pipe
      │
      ▼
worker
      │
      ▼
print_engine
```

### 12.2 Execution authority

The print engine remains authority for execution lifecycle.

The daemon must not invent an independent mutable print state that can diverge from the engine.

Instead, worker/engine events update a control-owned snapshot.

### 12.3 No job domain yet

Phase 1 represents an active execution/session, not a durable persistent job entity.

A durable `job_id`, queue ordering, history and restart persistence belong to Phase 3.

### 12.4 Second start

With one active execution:

```text
PRINT_START second-file
→ BUSY / INVALID_STATE
```

never implicit enqueue.

## 13. Serial ownership during migration

### Phase 1 transitional rule

Both the legacy CLI path and daemon path are capable of requesting serial at different times, but there must never be two effective simultaneous owners.

Existing exclusive-open behavior must remain in force.

```text
legacy CLI ─────┐
                ├── serial capability, mutually exclusive in practice
printerctld ────┘
```

### Final direction after Phase 2

```text
printerctld
    │
    └── sole normal serial owner
```

CLI/TUI/Web clients must then use daemon IPC rather than direct hardware access.

## 14. Shutdown architecture

### Idle

```text
SIGTERM / service stop
→ runtime STOPPING
→ worker SHUTDOWN
→ worker STOPPED
→ join
→ socket cleanup
→ runtime STOPPED
```

### Active print

M17 requires coordinated safe cancellation through the established engine lifecycle before the worker/runtime is destroyed.

Never use abrupt worker termination as the normal shutdown mechanism.

Never add automatic physical cleanup commands.

## 15. Low-battery architecture

Existing low-battery semantics remain authoritative:

- stop transmitting new G-code;
- distinguish power-failure cancellation from operator cancellation where supported;
- close resources cleanly;
- preserve interrupted-session record behavior;
- never automatically resume after power returns.

M17 adapts the process/daemon routing necessary to preserve these guarantees once the daemon owns the active print.

APCUPSD or its script must never directly own serial or emit printer commands.

## 16. Failure boundaries

### Client failure

A client crash/disconnect is a transport event.

It does not automatically imply print failure.

### Protocol failure

Malformed/unsupported frames affect the offending request/connection according to Protocol v1 rules and must not crash the daemon.

### Worker failure

Worker failure makes the execution backend unavailable and is surfaced explicitly.

No automatic worker restart.

### Serial/printer failure

Continue to use existing engine/printer/Marlin semantics:

- timeout terminates execution;
- serial disconnect terminates execution;
- `Error:` terminates execution;
- `Resend:` remains terminal/unsupported for the current implementation.

No auto-reconnect or auto-resume is introduced.

## 17. Safety model

Safety takes precedence over UX, automation and recovery.

### Never automatic

`printerctld`, the worker and protocol handlers must never automatically initiate:

- `G28` homing;
- `G0`/`G1` movement;
- heating;
- extrusion;
- `M112` emergency stop;
- resume after daemon restart, power restoration or serial recovery.

### Explicit operator actions

Explicit commands may request actions already permitted by the existing application contract, but Phase 1 must not widen those permissions silently.

The original Phase 1 G28 restriction on print files is superseded by
the operator-approved 2026-10-12 amendment in `Agents.md` §8: selected
files may contain G28, while raw send still rejects it. The dry run,
source command pre-scan and actual streaming all permit source G28.
The worker retains execution ownership, ACK ordering and the existing
cancellation barrier; no implicit homing is added.

## 18. Test architecture

### M13

Hardware-independent runtime tests.

### M14

Protocol specification plus parser vectors/fixtures sufficient for independent implementations.

### M15

Real AF_UNIX integration tests without printer hardware.

### M16

PTY/mock-Marlin integration with the actual print engine.

### M17

Fault, signal, power and compatibility scenarios.

Real hardware smoke tests remain limited to explicitly safe operations and must never be the primary validation mechanism for concurrency/protocol behavior.

Sanitizers should be used where compatible:

- ASan;
- UBSan;
- ThreadSanitizer, particularly after M13;
- Valgrind where it is already part of the project workflow.

## 19. Phase 1 non-goals

Do not introduce:

- job queue;
- persistent job database;
- scheduler;
- TUI;
- Web UI;
- HTTP/WebSocket gateway;
- multi-printer model;
- multiple print workers;
- automatic recovery;
- protocol event subscriptions;
- predictive timing model;
- higher-throughput serial command windows.

These are intentionally deferred to later phases.

## 20. Architecture after Phase 1

Successful Phase 1 establishes:

```text
client
  │
  ▼
Unix Domain Socket
  │
  ▼
printerctld
  │
  ├── stable control/runtime boundary
  ├── stable binary local protocol
  ├── responsive worker architecture
  └── single active print execution
         │
         ▼
      existing core
         │
       printer
         │
       serial
```

This becomes the foundation for Phase 2, where `printerctl` itself migrates from direct serial/core ownership to the daemon API.

## 21. Milestone 13 implementation status

§§6-9 above describe M13 as target architecture; M13 is now implemented
and verified as of 2026-08-21. See
[[milestone-13|docs/milestones/milestone-13.md]] and
[[milestone-13-daemon-runtime|docs/design/milestone-13-daemon-runtime.md]]
for the full as-built report, file list and verification results
(GCC/Clang, the full regression suite, ThreadSanitizer, ASan/UBSan).
`include/printerctl/daemon/{ipc,worker,runtime}.h` and
`src/daemon/{ipc,worker,runtime}.c` implement the control
context/worker/pipe model described here, unchanged in shape from §6-8.

Two points where the implementation resolved something this document
leaves at the conceptual level, both explained in the design doc:

- **§8.2's worker lifecycle diagram** shows `FAULTED` only as a sibling
  outcome of `INITIALIZING`. The implementation also reaches `FAULTED`
  from `READY` (a malformed/IO `cmd_pipe` read after startup) — the
  required "worker fault reflected in the snapshot" behavior only
  exercises something meaningful if a fault can happen mid-operation,
  not only during startup.
- **fd ownership** under `pthread_create()`'s shared fd table (not
  `fork()`'s per-process copies) needed an explicit rule this document
  does not spell out: the control context is the sole closer of all
  four pipe fds, always; the worker never calls `close()`. See the
  design doc §2 for why the naive "each side closes what it doesn't
  use" pattern is unsafe here.

M14-M17 implementation status is recorded below.

## 22. Milestone 14 implementation status

§10 above describes PLP/1 as target architecture; M14 is now
implemented and verified as of 2026-08-21. See
[[milestone-14|docs/milestones/milestone-14.md]] and
[[protocol-v1|docs/design/protocol-v1.md]] (the durable wire
specification, superseding §10 for field-level detail) for the full
as-built report. `include/printerctl/protocol/{plp1,plp1_parser}.h` and
`src/protocol/{plp1,plp1_parser}.c` implement exactly the framing,
message registry and flags §10 sketches, plus the field widths §10 left
unspecified (STATUS payload layout — see `protocol-v1.md` §16).

Confirmed independent of every other module, including `daemon/`: the
new CMake targets (`printerctl_protocol_plp1`,
`printerctl_protocol_plp1_parser`) link nothing beyond each other, and
`grep -n "#include" src/protocol/*.c include/printerctl/protocol/*.h`
shows only standard C headers and the module's own headers — no socket,
no `daemon/`, no `core/`.

M17 implementation status is recorded in §25.

## 23. Milestone 15 implementation status

§11 above describes the Unix socket transport as target architecture; M15
is now implemented and verified as of 2026-08-25. See
[[milestone-15|docs/milestones/milestone-15.md]] and
[[milestone-15-unix-socket-transport|docs/design/milestone-15-unix-socket-transport.md]]
for the full as-built report.

`include/printerctl/daemon/socket.h` and `src/daemon/socket.c` implement
the control-context-owned `AF_UNIX`/`SOCK_STREAM` transport. The module
depends on the M13 runtime snapshot and M14 PLP/1 parser/serializer, and
does not depend on `core`, `printer`, `serial`, `marlin`, `gcode` or the
CLI. `src/printerctld_main.c` adds the minimal process wrapper around
that transport.

Implemented protocol surface is intentionally limited to:

- `PING`;
- `VERSION`;
- idle `STATUS` for `daemon_state` and `worker_state`.

Event subscriptions, queueing and extended daemon lifecycle hardening
remain future milestones. The M15/M16 server supports multiple sequential
clients, not simultaneous clients.

## 24. Milestone 16 implementation status

§12 above describes single-print daemon integration as target
architecture; M16 is now implemented and verified as of 2026-08-25. See
[[milestone-16|docs/milestones/milestone-16.md]] and
[[milestone-16-single-print-daemon-integration|docs/design/milestone-16-single-print-daemon-integration.md]]
for the full as-built report.

The worker owns the M12 `printerctl_print_engine_t`; the control context
only sends bounded internal commands and applies worker `PRINT_UPDATE`
events to its published snapshot. PLP/1 `PRINT_START`, `PRINT_PAUSE`,
`PRINT_RESUME` and `PRINT_CANCEL` are dispatched through that boundary.

No queue, persistent job model, history, event subscription, CLI
migration, multi-printer support or automatic restart/resume was added.

## 25. Milestone 17 implementation status

M17 is implemented and verified as of 2026-08-25. See
[[milestone-17|docs/milestones/milestone-17.md]] and
[[milestone-17-lifecycle-fault-compatibility|docs/design/milestone-17-lifecycle-fault-compatibility.md]]
for the full as-built report.

Phase 1 daemon lifecycle, power-failure, serial-failure and compatibility
scenarios are covered by regression tests. The daemon path preserves the
existing safety model: no automatic homing, movement, heating, extrusion,
emergency stop, resume, queueing, durable job persistence, auto-reconnect
or automatic worker restart.
