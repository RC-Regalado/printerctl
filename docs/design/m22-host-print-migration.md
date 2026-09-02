# M22 Design — Host Print CLI Migration

Status: IMPLEMENTED

## Objective

Make `printerctl print FILE` start daemon-owned execution over PLP/1.

## Flow

```text
CLI
   ↓
PRINT_START
   ↓
daemon authoritative validate/open
   ↓
worker
   ↓
print_engine
   ↓
printer
```

## Observation

After accepted START, CLI may remain attached and poll STATUS/progress.

Execution must survive client disconnect.

Do not add subscriptions unless polling cannot satisfy compatibility.

M22 implements the smaller accepted path: the CLI returns after
`PRINT_START` is accepted and does not poll `STATUS`. Host print control
UX remains M23 scope.

## Dry run

Dry-run remains local and does not call PRINT_START.

## Exit behavior

M22 preserves exit-code classes for errors known before acceptance:

- daemon/socket/protocol failure -> communication error (`7`);
- daemon-reported source failure -> G-code file error (`6`);
- daemon-reported safety rejection -> safety error (`4`);
- daemon busy/unavailable -> communication error (`7`).

After acceptance, execution is daemon-owned and the CLI no longer reports
final print summaries, operator cancel, power cancel, printer error or
timeout outcomes. Those remain observable through daemon state and are
reintroduced in CLI UX by later milestones.

## As-built notes

- `printerctl_client_print_start()` wraps PLP/1 `PRINT_START`; CLI
  handlers do not build protocol frames directly.
- Non-dry-run `print` uses the client-owned configuration view and does
  not consume `port`, `baud` or Marlin `timeout`.
- The `printerctl_cli` target no longer links `printerctl_printer` or
  `printerctl_core`; host streaming is owned by `printerctld`.
- `--dry-run print FILE` still uses the local G-code reader and remains
  hardware-independent.
