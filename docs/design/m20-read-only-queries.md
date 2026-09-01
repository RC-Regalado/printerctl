# M20 Design — Read-Only Printer Queries

## Objective

Migrate `temp`, `position`, and `info` through the daemon.

## Constraints

- explicit PLP operations;
- no STATUS overload;
- worker remains execution/printer owner;
- daemon performs Marlin I/O;
- structured response preferred;
- client only renders.

## Safety

These operations must not cause movement, homing, heating, extrusion, pause, resume, or cancellation.

## Acceptance

`printerctl` cannot reach serial for these commands.

PTY/mock evidence must show:
query -> daemon -> printer -> structured response -> CLI output.

## As-built

Status: IMPLEMENTED

- PLP/1 adds explicit `QUERY_TEMP` (`0x30`), `QUERY_POSITION` (`0x31`) and `QUERY_INFO` (`0x32`) operations.
- Requests have no payload; success responses carry structured fixed-point/string payloads documented in `docs/design/protocol-v1.md`.
- `printerctld` dispatches each query through the runtime worker. The worker owns the printer session and calls the existing read-only `printerctl_printer_query_*` functions.
- `printerctl temp`, `printerctl position` and `printerctl info` use the PLP client and do not open serial directly.
- `--dry-run` remains local and does not require daemon or serial access.

No socket configuration, queue, jobs, history, command migration, or new physical operation was added.
