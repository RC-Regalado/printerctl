# M25 Design — Remove Legacy Serial Ownership

## Objective

Make the daemon boundary mandatory for real printer operations.

## Required removal from printerctl

- serial open/write/read;
- printer ownership;
- Marlin exchange;
- host print_engine execution.

Local G-code parsing may remain only for dry-run.

## Build graph gate

Add deterministic check failing if CLI regains forbidden hardware dependencies.

## Failure

If daemon unavailable:

```text
migrated command -> explicit failure
```

Never:

```text
migrated command -> silent direct-serial fallback
```
