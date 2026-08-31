# M18 Design — Configuration Boundary Preparation

## Objective

Prepare the configuration model for the Phase 2 ownership split without
breaking existing legacy direct CLI commands.

M18 establishes logical ownership and owner-aware configuration
resolution. Final enforcement happens as commands migrate to PLP and the
legacy path is removed in M25.

## Current config

```ini
port=/dev/ttyACM0
baud=115200
timeout=5000
dry_run=false
verbose=false
```

## Target ownership

```text
printerctld:
  port
  baud
  timeout

printerctl:
  dry_run
  verbose
```

## Required analysis before implementation

Codex must inspect and report:

1. current defaults/config/CLI precedence;
2. parser location and reuse potential;
3. how printerctld currently receives hardware settings;
4. unknown-key behavior;
5. duplicate-key behavior;
6. invalid-value behavior.

Preserve existing precedence unless a change is required and justified.

## Compatibility

A shared file may temporarily contain both daemon and client keys.

Each owner-aware view applies only keys it owns.

Valid cross-owned keys may be parsed but are not applied to the wrong
view. Invalid cross-owned values still fail during parsing, preserving
the existing validation behavior.

## Views

```text
parsed configuration
   ├── client-owned view
   │     dry_run
   │     verbose
   ├── daemon-owned view
   │     port
   │     baud
   │     timeout
   └── transitional legacy CLI view
         port
         baud
         timeout
         dry_run
         verbose
```

The transitional legacy CLI view exists only for direct serial commands
not yet migrated to PLP/daemon execution. No new command may be added to
that set, and the view is removed in M25.

## Out of scope

- new PLP message types;
- printer command migration;
- queue/jobs/history;
- daemon concurrency redesign;
- physical behavior changes.

## Acceptance

- printerctld loads/validates hardware config;
- printerctl retains dry-run/verbose;
- dry-run works without daemon;
- invalid hardware config fails safely;
- existing unmigrated direct CLI commands keep working through the
  explicitly documented transitional view;
- tests encode ownership and precedence.
