---
title: Milestone 18 — Configuration Boundary Preparation
tags: [printerctl, milestone, phase-2, config]
status: implemented
updated: 2026-08-31
---

# Milestone 18 — Configuration Boundary Preparation

M18 adds owner-aware configuration resolution without migrating CLI
commands or changing physical printer behavior.

## Ownership model

```text
client view
  dry_run
  verbose

daemon view
  port
  baud
  timeout

legacy CLI transitional view
  port
  baud
  timeout
  dry_run
  verbose
```

The parser and config file format remain shared. Views decide which
already-parsed keys apply.

## Transitional legacy consumers

The current direct CLI commands may still use hardware config until they
migrate:

- `print`
- `send`
- `home`
- `pause`
- `resume`
- `cancel`
- `temp`
- `position`
- `info`

`power low-battery` does not use hardware config.

No new command was added to the transitional set. The set must only
shrink during Phase 2 and is removed in M25.

## Decisions

- Preserve precedence: defaults → config file → CLI flags.
- Keep `timeout` as Marlin response timeout.
- Reuse the existing parser; no duplicate config format.
- Add `printerctld --config` for daemon-owned hardware config.
- Do not add PLP client code or migrate commands in M18.

## Validation

- Config tests cover owner views, cross-owned valid keys, invalid
  cross-owned values and legacy compatibility.
- Process tests cover valid and invalid daemon config.
- Existing dry-run tests continue to prove local dry-run without daemon
  or serial access.

## M25 debt

Remove the transitional legacy CLI hardware-config view and reject direct
CLI serial ownership once all commands have migrated to daemon execution.
