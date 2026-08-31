# M22 Design — Host Print CLI Migration

## Objective

Make `printerctl print FILE` start daemon-owned execution over PLP/1.

## Flow

```text
CLI normalize path
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

## Dry run

Dry-run remains local and does not call PRINT_START.

## Exit behavior

Preserve user-facing summary and exit-code semantics where possible.

Explicitly document mappings for:
- source failure;
- serial failure;
- printer error;
- operator cancel;
- power cancel;
- daemon communication failure.
