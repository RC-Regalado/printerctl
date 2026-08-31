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
