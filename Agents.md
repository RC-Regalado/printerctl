---
title: printerctl Phase 1 Agent Guide
tags: [printerctl, agents, phase-1, codex, development]
status: active
updated: 2026-08-21
---

# AGENTS.md — printerctl Phase 1

## 1. Purpose

This file defines the operating contract for AI coding and review agents working on **printerctl Phase 1 — Daemon Foundation + Single-Print IPC**.

Agents are implementers and reviewers of an already approved architecture.

They must not silently redesign the phase while coding.

When implementation reveals a genuine architectural blocker, stop that portion of work, document the blocker and propose the smallest decision required to proceed.

## 2. Phase objective

Phase 1 introduces a persistent-service execution boundary without yet migrating the current CLI or implementing a job queue.

The target is:

```text
local client
      │
      ▼
Unix Domain Socket
      │
      ▼
 printerctld
      │
      ├── control context
      │
      ├── cmd_pipe ─────► worker
      │                   │
      ◄──── event_pipe ───┘
                          │
                          ▼
                     print_engine
                          │
                       printer
                          │
                        serial
```

The legacy `printerctl` CLI remains functional during Phase 1.

## 3. Sources of truth

Before implementing a Phase 1 task, read the relevant current documents.

Primary Phase 1 sources:

```text
docs/roadmap.md
docs/architecture.md
AGENTS.md
```

Project safety/testing/as-built sources remain authoritative where applicable:

```text
docs/safety.md
docs/testing.md
docs/apcupsd.md
M12 milestone/design documentation
current public headers and implementation
```

When an older roadmap conflicts with this Phase 1 package for M13–M17, this Phase 1 roadmap is the active execution plan.

Do not delete older documents solely because they are superseded. Preserve useful decision history and mark supersession explicitly when documentation is updated.

## 4. Decision status vocabulary

Use these labels consistently:

```text
CONFIRMED
PROPOSED
DEFERRED
SUPERSEDED
```

Do not present PROPOSED behavior as implemented or architecturally confirmed.

Current major decisions:

### CONFIRMED

- M12 print engine is reused rather than replaced.
- `print_engine` remains transport/UI agnostic.
- Phase 1 uses one worker thread.
- control and worker communicate through two unidirectional pipes.
- `cmd_pipe` direction is control → worker.
- `event_pipe` direction is worker → control.
- external local protocol is binary and length-prefixed.
- first four bytes represent the number of bytes following the length field.
- protocol uses explicit versioning, numeric message types, flags and request correlation.
- one daemon-controlled print may be active at a time.
- a second print start is rejected rather than queued.
- legacy CLI remains functional through Phase 1.
- no automatic print resume.
- no automatic worker restart.

### DEFERRED

- CLI socket-only migration — Phase 2.
- durable job model/queue/history — Phase 3.
- TUI — Phase 4.
- multi-printer — Phase 5.
- event subscriptions.
- timing prediction.
- automatic `Resend:` recovery/checksums/line numbers.

### SUPERSEDED

For current Phase 1 execution, previous roadmap definitions of M13–M17 are superseded by the Phase 1 roadmap in this package.

## 5. Agent workflow

Every milestone implementation should follow:

```text
1. Read required sources.
2. Inspect current repository state.
3. Restate the milestone boundary internally.
4. Implement only approved scope.
5. Add/adjust tests.
6. Run focused tests.
7. Run regression tests.
8. Run sanitizers where applicable.
9. Update milestone documentation.
10. Produce implementation report.
11. Hand off to an independent reviewer.
12. Fix required review findings.
13. Re-run verification before milestone closure.
```

Do not combine multiple milestones in one implementation merely because later work appears straightforward.

## 6. Implementation-agent responsibilities

The implementation agent must:

- follow approved ownership boundaries;
- inspect existing code before choosing file/module names;
- preserve project conventions;
- keep the project compilable after each coherent change;
- avoid unrelated refactors;
- implement explicit error paths;
- maintain deterministic resource ownership;
- add tests for failure paths, not only happy paths;
- update documentation only to reflect behavior actually implemented;
- report deviations from the approved design rather than silently introducing them.

The implementation agent is not authorized to change phase ordering or pull deferred product capabilities into the current milestone.

## 7. Reviewer responsibilities

The reviewer must not limit review to style.

Review priorities:

1. safety;
2. ownership/lifetime;
3. concurrency;
4. failure handling;
5. architectural dependency direction;
6. scope compliance;
7. protocol correctness where applicable;
8. test quality;
9. regression risk;
10. documentation accuracy;
11. technical debt.

The reviewer must actively search for implementation that works on the happy path but violates the future architecture.

## 8. Safety contract

Safety has priority over convenience, recovery and UX.

No agent may add behavior that automatically performs:

- homing;
- movement;
- heating;
- extrusion;
- emergency stop;
- print resume after power restoration, daemon restart or serial recovery.

### Homing

X/Y/Z minimum end-stops are currently installed and dedicated `home` operations may exist according to current project behavior.

This does not authorize implicit homing.

Confirmed amendment (2026-10-12): the operator has tested the X/Y/Z
minimum end-stops and authorizes G28 in explicitly selected print files.
`print FILE` and its dry run permit G28 and axis variants. Selecting a
file authorizes its homing commands; printerctl must not insert any.
This supersedes the original Phase 1 restriction on sliced files.

Rules that remain:

- raw `send` must not gain an automatic/bypass G28 path;
- sliced G-code print paths must preserve ACK ordering and cancellation
  barriers for G28 just as for other source commands;
- daemon startup must never home;
- reconnect/restart must never home.

### Emergency stop

`M112` must never be emitted automatically.

### Cancellation

Cancellation means stopping transmission and cleaning up software resources according to the existing lifecycle.

Do not add automatic physical shutdown sequences such as:

- `M112`;
- `M410`;
- homing;
- moves;
- heating changes;
- resume commands.

### Power failure

Low-battery handling must retain existing safety semantics:

- no new G-code after cancellation is processed;
- close resources cleanly;
- record interrupted-session information according to current behavior;
- distinguish power-failure cancellation where supported;
- never resume automatically after power returns.

APCUPSD/scripts must never directly own the serial port.

## 9. Serial and Marlin invariants

Existing serial/Marlin behavior remains authoritative unless a milestone explicitly changes it.

### Serial

- configurable Linux serial transport;
- exclusive access behavior must remain intact;
- no automatic reconnect during an active print;
- serial disconnect remains a terminal execution failure.

### Marlin

Streaming remains:

```text
send one logical command
→ receive zero or more informational lines
→ receive terminal ok / Error: / Resend:
→ send next command only after ok
```

Current semantics:

- `ok` → acknowledged;
- `Error:` → terminal failure;
- `busy:`/`echo:`/other info → non-terminal;
- `Resend:` → terminal unsupported condition in current implementation.

Do not implement line numbering, checksums or resend recovery during Phase 1.

## 10. Core boundary

The following dependency direction is mandatory:

```text
printerctld
    ↓
print_engine
    ↓
printer
    ↓
serial / marlin / gcode
```

Never introduce:

```text
core → daemon
core → socket
core → protocol frames
printer → daemon
serial → daemon
```

The print engine must not know:

- socket FDs;
- AF_UNIX;
- protocol version;
- message IDs;
- CLI/TUI/Web rendering;
- external request IDs.

## 11. Runtime ownership contract

### Control context owns

- daemon/runtime lifecycle;
- future socket descriptors;
- external protocol parsing/serialization;
- request dispatch;
- published snapshot;
- worker availability state.

### Worker owns

- future print engine instance;
- future mutable print execution state;
- future printer/serial session through existing layers.

### Forbidden

Do not let both threads mutate the same execution object merely by adding a mutex.

Clear ownership is preferred over shared mutable state.

## 12. Internal pipe contract

Phase 1 uses:

```text
cmd_pipe   : control → worker
event_pipe : worker  → control
```

These messages are internal runtime messages, not wire frames.

Agents must not reuse the external binary protocol struct/layout directly inside the pipe as a shortcut.

The internal representation may evolve with the implementation, but must preserve:

- explicit type;
- bounded size;
- internal correlation token where applicable;
- explicit result/error semantics;
- robust handling of partial I/O/EOF/EINTR according to the chosen framing strategy.

Do not assume `write(struct) == read(struct)` unless a documented Linux guarantee and bounded message design make the assumption valid for the exact path being implemented.

## 13. Worker lifecycle contract

Expected worker states:

```text
INITIALIZING
READY
STOPPING
STOPPED
FAULTED
```

The runtime must not report a ready service backend merely because `pthread_create()` succeeded.

Worker readiness requires an explicit READY handshake.

Automatic worker restart is prohibited.

## 14. Runtime snapshot contract

The client-facing snapshot belongs to the control context.

Worker changes are communicated as events.

```text
worker
  │
  ▼
event_pipe
  │
  ▼
control
  │
  ▼
published snapshot
```

Do not permit the worker to directly mutate client-facing state.

## 15. Printerctl Local Protocol v1 contract

The external protocol is distinct from internal pipe messages.

### Confirmed framing

```text
4-byte frame length
fixed binary header
optional payload
```

`frame_length` is the number of bytes after the four-byte field.

### Required properties

- explicit protocol version;
- numeric message type;
- external request/correlation ID;
- flags bitmap;
- bounded maximum frame size;
- explicit byte order;
- length-prefixed strings;
- fragmentation-safe parsing.

### Bitmaps

Use bitmaps for orthogonal properties/capabilities.

Use enums for mutually exclusive states.

### Phase 1 message semantics

Only introduce what the current milestone requires from this set:

```text
PING
VERSION
STATUS
PRINT_START
PRINT_PAUSE
PRINT_RESUME
PRINT_CANCEL
```

Do not introduce:

```text
JOB_*
QUEUE_*
HISTORY_*
EVENT_SUBSCRIBE
printer routing IDs
```

## 16. Milestone boundaries

### M13 — Runtime foundation

Implement:

- worker thread;
- two pipes;
- internal message contract;
- lifecycle;
- snapshot;
- NOP round-trip;
- shutdown/fault behavior.

Do not implement socket, protocol parser or print engine integration.

### M14 — Protocol design

Produce/freeze the wire specification.

Do not treat a vague header sketch as sufficient implementation authority.

Field widths, byte order, registry values, errors, limits and payload layouts must be explicit before M15.

### M15 — Socket transport

Implement AF_UNIX/SOCK_STREAM and Protocol v1 parser/serializer with hardware-independent commands only.

Do not connect print engine yet.

### M16 — Single-print integration

Connect daemon worker to existing print engine.

A second active start must fail explicitly; never create a queue.

Client disconnect must not automatically cancel an accepted print.

### M17 — Lifecycle/fault validation

Close:

- SIGTERM/service shutdown;
- client failures;
- serial failure;
- Marlin failure;
- timeout;
- SIGPWR/low battery;
- legacy CLI coexistence;
- regressions.

Do not migrate CLI to socket-only behavior in M17.

## 17. Explicit phase-level prohibitions

During Phase 1, agents must not implement:

- persistent job queue;
- durable job IDs;
- queue ordering/reordering;
- job scheduler;
- queue recovery after restart;
- TUI;
- Web UI;
- HTTP gateway;
- multi-printer architecture;
- multiple active print workers;
- automatic serial reconnect/resume;
- automatic daemon print recovery;
- event subscriptions;
- predictive timing algorithms.

If code seems to require one of these features to complete a Phase 1 milestone, treat that as an architectural issue rather than silently adding the feature.

## 18. Testing policy

### Hardware independence first

M13 and M15 must be fully testable without a printer.

M16/M17 should use PTY/mock-Marlin integration before real hardware.

### Required categories as applicable

- unit;
- integration;
- fault injection;
- protocol;
- concurrency;
- regression;
- sanitizer verification;
- limited safe hardware smoke tests only when necessary.

### Concurrency

For thread/runtime changes actively test:

- repeated start/stop;
- startup races;
- shutdown races;
- pipe EOF;
- unexpected worker exit;
- resource leaks;
- deadlocks;
- data races.

Use ThreadSanitizer where the environment supports it.

### Protocol

Test:

- fragmented length prefix;
- fragmented frame;
- multiple frames per read;
- complete + partial next frame;
- unknown version/type;
- malformed lengths;
- oversized frame;
- client EOF.

### Regression

Run the existing suite after every milestone.

Do not accept a new daemon path that breaks the current CLI path during Phase 1.

## 19. Resource ownership review

For every new resource an agent must be able to answer:

```text
Who creates it?
Who owns it?
Who closes/destroys it?
When?
What happens if initialization fails halfway?
```

This applies especially to:

- pipe read/write ends;
- worker thread lifetime;
- socket FDs;
- client FDs;
- receive buffers;
- runtime allocations;
- synchronization primitives;
- print-engine/printer/serial resources in M16/M17.

## 20. Documentation policy

Documentation is part of the implementation contract.

After each milestone:

- update its status;
- document actual architecture, not intended future behavior;
- record deliberate deviations from preliminary designs;
- preserve useful historical decisions;
- mark superseded documents/sections explicitly;
- do not claim later-phase features are implemented.

If documentation contradicts actual behavior:

1. identify the contradiction;
2. determine which source reflects current code;
3. report the discrepancy;
4. update documentation explicitly as part of the approved task.

Do not silently rewrite history.

## 21. Technical debt classification

Classify findings as:

```text
BLOCKING
RESOLVE_SOON
ACCEPTABLE
FUTURE_IMPROVEMENT
```

Examples of blocking debt in Phase 1:

- ambiguous serial/engine ownership;
- data races;
- unjoined worker;
- file-descriptor leaks on lifecycle paths;
- automatic worker restart;
- safety regression;
- hidden queue behavior;
- daemon state diverging from engine state;
- client disconnect automatically terminating a valid print without explicit contract.

Do not promote minor style preferences into roadmap work.

## 22. Implementation task contract

Milestone-specific implementation prompts are task artifacts, not skills.

A task should define:

- Objective;
- Context;
- Required reading;
- Scope;
- Out of scope;
- Architectural constraints;
- Interfaces/contracts;
- Expected affected modules;
- Implementation guidance;
- Required tests;
- Safety constraints;
- Acceptance criteria;
- Documentation updates;
- Do not.

The implementation agent must not infer permission to work outside that contract.

## 23. Review task contract

Prefer an independent reviewer context after implementation.

The reviewer must verify:

- scope;
- architecture;
- safety;
- ownership/lifetime;
- concurrency;
- error handling;
- protocol correctness where applicable;
- compatibility;
- tests;
- documentation;
- technical debt.

Required verdict vocabulary:

```text
APPROVE
APPROVE WITH REQUIRED FIXES
REJECT
```

## 24. Definition of Phase 1 success

Phase 1 is not complete merely because a Unix socket accepts commands.

It is complete when the architecture demonstrates safely and repeatedly:

```text
client A
  → start print through printerctld
  → disconnect

printerctld
  → remains alive
  → print continues

client B
  → connect
  → STATUS
  → PAUSE
  → RESUME
  → CANCEL

printerctld
  → maintains correct lifecycle
  → preserves existing safety/failure semantics
```

and the legacy CLI remains functional as the temporary direct path when it is the serial owner.

Only after this boundary is proven should Phase 2 migrate the CLI fully onto the daemon API.
