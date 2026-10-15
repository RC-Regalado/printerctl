# Phase 2 Roadmap — CLI Client Migration

**Phase status:** DONE

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

**Status:** DONE

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

**Status:** DONE

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

**Status:** IMPLEMENTED

Migrate:

- `temp`
- `position`
- `info`

Use explicit PLP query operations. Do not overload STATUS.

Prefer structured PLP responses based on existing Marlin/domain parsing.

## M21 — Explicit Physical Commands

**Status:** IMPLEMENTED

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

Implementation notes:

- `send COMMAND`, `home [X|Y|Z]` and `sd pause`/`sd resume`/`sd cancel`
  use PLP/1 and no longer use the legacy CLI hardware-config view.
- Bare `pause`, `resume` and `cancel` are reserved for host-stream
  control in M23 and are not SD aliases.
- M22 removes the final direct-serial `print FILE` CLI path.

## M22 — Host Print CLI Migration

**Status:** IMPLEMENTED

Migrate:

```text
printerctl print FILE
```

to daemon-owned execution over `PRINT_START`.

The daemon validates/opens the source authoritatively.

Implementation notes:

- non-dry-run `printerctl print FILE` sends PLP/1 `PRINT_START` through
  the reusable M19 client API;
- the CLI exits after daemon acceptance; execution lifetime is owned by
  `printerctld` and survives client disconnect;
- `--dry-run print FILE` remains local and never connects to the daemon
  or serial;
- migrated `print` no longer consumes `port`, `baud` or Marlin
  `timeout`, and has no direct-serial fallback.

## M23 — Host Print Control UX

**Status:** IMPLEMENTED

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

Implementation notes:

- `pause`, `resume` and `cancel` use the M19 PLP client (`printerctl_
  client_print_pause/resume/cancel()`) over the pre-existing PLP/1
  `PRINT_PAUSE`/`PRINT_RESUME`/`PRINT_CANCEL` messages and daemon-side
  handling (already built and socket-tested ahead of this milestone);
  no new PLP/1 wire messages were needed.
- Repeated pause (while paused) and repeated resume (while running)
  are safe no-ops, and any connected client may issue any of the three
  regardless of which client's `PRINT_START` began the print.
- `printerctl print FILE`'s attached progress observer (PRINT-OBS-001)
  installs no signal handler: an unhandled Ctrl+C kills only the
  observer, never the daemon-owned print. A latent bug in that same
  observer was fixed alongside `cancel`'s CLI wiring: it now reports
  `PRINTERCTL_EXIT_PRINT_CANCELLED`/`_POWER_FAILURE` instead of exit 0
  when the print it is observing is cancelled by another client.
- See `docs/design/m23-host-control-ux.md`'s "As-built notes" and
  `docs/milestones/milestone-23.md` for full details.

## M24 — Power Integration Alignment

**Status:** IMPLEMENTED

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

Implementation notes:

- No code change was required to make the diagram above literally
  true: `print_engine` has written the active-print marker with its
  own PID since Milestone 16/22 moved print execution into
  `printerctld`'s worker thread, so `printerctl power low-battery`
  (unchanged since Milestone 10) was already signalling the daemon,
  not a legacy CLI process. `power low-battery` still never talks
  PLP/1 to `printerctld` — it resolves the marker and signals SIGPWR
  directly, exactly as designed.
- M24's actual work was closing a real test-coverage gap: no existing
  test ran the real `printerctl power low-battery` command against a
  live `printerctld` process end-to-end (every prior SIGPWR test
  delivered the signal with a raw `kill()`, bypassing the CLI
  entirely) — added in `tests/test_printerctld.c`.
- Added a regression test for a scenario M23 made possible for the
  first time: SIGPWR arriving while a daemon-owned print is genuinely
  `PAUSED` (not just blocked on an in-flight command's ack). Confirmed
  `on_control()`'s existing priority order (power-failure beats pause)
  already handles it correctly.
- A stale doc-comment in `src/cli/cli.c` describing the marker/SIGPWR
  path as "transitional... until M24" was corrected to describe it as
  the final, deliberately preserved mechanism.
- See `docs/design/m24-power-integration.md`'s "As-built notes" and
  `docs/milestones/milestone-24.md` for full details.

## M25 — Remove Legacy Serial Ownership

**Status:** IMPLEMENTED

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

Implementation notes:

- All four forbidden dependencies were already gone from `printerctl`'s
  real execution paths as a side effect of M20-M22's migrations: the
  `printerctl_cli` CMake target links only `printerctl_config`,
  `printerctl_logging`, `printerctl_gcode` (dry-run parsing only),
  `printerctl_record` (session/marker files) and `printerctl_client`
  (PLP) — none of which pull in `printerctl_serial`,
  `printerctl_printer`, `printerctl_marlin` or `printerctl_core`
  (print_engine). No source change was needed to achieve the removal
  itself.
- The one genuinely missing piece — "add deterministic build/
  dependency checks" — is now a permanent `ctest` target,
  `printerctl_build_boundary_tests` (`tests/test_build_boundaries.c`):
  it runs `nm` on the actual built `printerctl` executable and fails if
  any `printerctl_serial_*`/`printerctl_printer_*`/`printerctl_marlin_*`/
  `printerctl_print_engine_*` symbol appears in it — a physical
  guarantee about the linked binary, not just a CMakeLists.txt
  link-graph audit, so it would catch a forbidden dependency
  reintroduced through any path.
- "Local G-code parsing may remain only for dry-run" already held:
  `printerctl_gcode` is used only by `run_dry_run_print()`.
- "Migrated command → explicit failure, never silent direct-serial
  fallback" is now structurally guaranteed, not just behaviorally
  tested: since no serial code is linked into `printerctl` at all, a
  silent direct-serial fallback is physically impossible, not merely
  absent by convention.
- See `docs/design/m25-remove-legacy-serial.md`'s "As-built notes" and
  `docs/milestones/milestone-25.md` for full details.

## M26 — Compatibility & Fault Validation

**Status:** DONE

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
