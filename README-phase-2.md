# printerctl — Phase 2 Codex Package

## Purpose

This package defines the implementation contract for **Phase 2 — CLI Client Migration**.

Phase 1 is complete and approved. `printerctld` is now the process that owns mutable print execution and serial access. Phase 2 migrates `printerctl` from direct hardware ownership to a PLP/1 client of `printerctld`.

The goal is **not** to add new product features. The goal is to move existing CLI capabilities across the daemon boundary while preserving safety, compatibility, observability, and existing print semantics.

## Phase 2 success condition

```text
printerctl
├── CLI parsing / presentation
├── local dry-run
├── client-owned configuration
└── PLP/1 client
        │
        ▼
printerctld
├── daemon runtime
├── worker
├── print_engine
├── printer
├── Marlin
└── serial
```

Final invariant:

```text
printerctl X-> serial
printerctl X-> printer
printerctl X-> direct Marlin I/O
```

`printerctld` is the sole owner of real printer I/O.

## Required reading order

1. `docs/phase-2-roadmap.md`
2. `docs/phase-2-architecture.md`
3. `docs/phase-2-contracts.md`
4. `docs/phase-2-testing.md`
5. milestone-specific design file

## Scope

Included:

- hardware/client configuration split;
- reusable local PLP client;
- read-only printer queries;
- explicit physical commands;
- host print migration;
- host print control UX migration;
- APCUPSD/power integration alignment;
- removal of legacy serial ownership;
- compatibility/fault validation.

Excluded:

- queue;
- persistent jobs;
- history;
- scheduler;
- TUI;
- Web/API;
- event subscriptions unless strictly required by an approved milestone;
- multi-printer;
- automatic retry/recovery;
- automatic resume;
- new physical safety behavior;
- automatic homing, movement, heating, extrusion, parking, cooldown, M112, or M410.

## Status vocabulary

Use only:

- `CONFIRMED`
- `PROPOSED`
- `DEFERRED`
- `SUPERSEDED`
- `BLOCKED`
- `DONE`
