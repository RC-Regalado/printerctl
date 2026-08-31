# Phase 2 Roadmap — CLI Client Migration

## Phase objective

Convert `printerctl` from a direct Marlin/serial controller into a client of `printerctld` over PLP/1, preserving the current CLI behavior where practical and preserving all safety guarantees.

## Confirmed architectural decisions

1. `printerctld` is the only serial owner at the end of Phase 2.
2. A migrated command has **no automatic fallback** to direct serial access.
3. `--dry-run print FILE` remains completely local and must not require the daemon.
4. `port`, `baud`, and Marlin `timeout` belong to `printerctld`.
5. `verbose` and `dry_run` are client concerns.
6. PLP `STATUS` remains snapshot-only and must not trigger Marlin I/O.
7. `temp`, `position`, and `info` use explicit PLP operations.
8. `home` uses an explicit PLP operation and is never implemented through generic raw send.
9. Generic `send` continues to reject `G28` authoritatively in the daemon.
10. PLP `PRINT_PAUSE`, `PRINT_RESUME`, and `PRINT_CANCEL` mean host-stream control.
11. SD-card print controls are distinct operations.
12. Existing SIGPWR/daemon-marker integration remains valid unless a milestone explicitly proves a safer replacement.
13. No queue/jobs/history/TUI/Web/multi-printer work belongs to Phase 2.

## CLI UX direction

Target:

```text
printerctl print FILE
printerctl pause
printerctl resume
printerctl cancel

printerctl sd pause
printerctl sd resume
printerctl sd cancel
```

Host-stream `pause/resume/cancel` become the primary service-level controls.

The historical SD semantics move to the explicit `sd` namespace.

This is a deliberate compatibility change and must be documented and tested.

## M18 — Configuration Boundary Preparation

**Status:** PROPOSED

### Goal

Prepare client-owned and daemon-owned configuration views while keeping
existing unmigrated direct CLI commands working.

### Ownership

`printerctl`:
- `dry_run`
- `verbose`

`printerctld`:
- `port`
- `baud`
- Marlin `timeout`

### Acceptance

- daemon can obtain hardware configuration independently from CLI printer commands;
- `port`/`baud`/Marlin timeout have an explicit daemon-owned view;
- `dry_run`/`verbose` have an explicit client-owned view;
- unmigrated legacy direct CLI commands may temporarily use hardware
  config through a documented compatibility view;
- `--dry-run` still works with daemon unavailable.
- no PLP client, PLP operation, command migration or new physical
  behavior is introduced.

## M19 — PLP Client Library

**Status:** PROPOSED

### Goal

Create a reusable local IPC client layer for `printerctl`.

### Responsibilities

- Unix socket connect;
- request IDs;
- PLP serialization;
- write-all;
- incremental response parse;
- response correlation;
- connection failure handling;
- protocol/version validation;
- bounded request behavior.

### Boundary

```text
CLI command
   ↓
client API
   ↓
PLP protocol
   ↓
Unix socket
```

CLI command handlers must not manually construct PLP byte frames.

## M20 — Read-Only Printer Queries

**Status:** PROPOSED

Migrate:

- `temp`
- `position`
- `info`

Use explicit PLP query operations. Do not overload STATUS.

Prefer structured PLP responses based on existing Marlin/domain parsing.

## M21 — Explicit Physical Commands

**Status:** PROPOSED

Migrate:

- `send COMMAND`
- `home [X|Y|Z]`
- `sd pause`
- `sd resume`
- `sd cancel`

Safety rule:

```text
RAW_SEND("G28") -> SAFETY_REJECTED
HOME(X)         -> explicit homing intent
```

Safety is authoritative in the daemon.

## M22 — Host Print CLI Migration

**Status:** PROPOSED

Migrate:

```text
printerctl print FILE
```

to daemon-owned execution over `PRINT_START`.

The daemon validates/opens the source authoritatively.

The client may remain attached as observer using STATUS polling, but execution lifetime is independent from client lifetime.

## M23 — Host Print Control UX

**Status:** PROPOSED

Make:

```text
printerctl pause
printerctl resume
printerctl cancel
```

control daemon-owned host streams.

Historical SD control becomes:

```text
printerctl sd pause
printerctl sd resume
printerctl sd cancel
```

Add explicit repeated-pause and repeated-resume tests.

## M24 — Power Integration Alignment

**Status:** PROPOSED

Preferred initial path:

```text
APCUPSD
   ↓
printerctl power low-battery
   ↓
daemon marker / PID
   ↓
SIGPWR printerctld
```

Preserve the audited SIGPWR path unless a concrete requirement justifies changing it.

## M25 — Remove Legacy Serial Ownership

**Status:** PROPOSED

Remove from real `printerctl` execution paths:

- serial open/read/write;
- printer ownership;
- Marlin exchange;
- host print_engine execution.

Final forbidden dependencies:

```text
printerctl -> serial
printerctl -> printer
printerctl -> direct Marlin exchange
printerctl -> host print_engine execution
```

Add deterministic build/dependency checks.

## M26 — Compatibility & Fault Validation

**Status:** PROPOSED

Required matrix includes:

1. daemon absent;
2. socket unavailable;
3. protocol mismatch;
4. daemon busy;
5. temp success/failure;
6. position success/failure;
7. info success/failure;
8. send success;
9. send G28 rejection;
10. home all/X/Y/Z;
11. SD pause/resume/cancel;
12. host print start;
13. host progress;
14. terminal success;
15. client disconnect;
16. second-client control;
17. repeated pause;
18. repeated resume;
19. cancel;
20. Ctrl+C;
21. SIGTERM;
22. SIGPWR;
23. serial disconnect;
24. Marlin Error;
25. Resend;
26. timeout;
27. no legacy serial ownership;
28. dry-run with daemon down;
29. exit-code compatibility;
30. no automatic physical action regression.

## Dependency order

```text
M18 Configuration Boundary Preparation
        ↓
M19 PLP Client
        ↓
M20 Read-Only Queries
        ↓
M21 Explicit Physical Commands
        ↓
M22 Host Print Migration
        ↓
M23 Host Print Control UX
        ↓
M24 Power Integration Alignment
        ↓
M25 Remove Legacy Serial Ownership
        ↓
M26 Final Validation
```
