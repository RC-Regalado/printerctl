# M21 Design — Explicit Physical Commands

Status: IMPLEMENTED

## Objective

Migrate explicit operator physical commands while preserving policy.

## Operations

- raw send;
- home all/X/Y/Z;
- SD pause;
- SD resume;
- SD cancel.

## Authoritative policy

Safety is enforced daemon-side before printer I/O.

Client validation may improve UX but is never authoritative.

## G28

```text
raw send G28 -> reject
dedicated HOME -> allowed explicit intent
```

No bypass.

## SD vs host

SD operations are distinct PLP message types from host-stream control.

Do not route `sd pause` into host `PRINT_PAUSE`.

## Review requirement

Dedicated safety review required before DONE.

## As-built notes

- PLP/1 adds `RAW_SEND`, `HOME`, `SD_PAUSE`, `SD_RESUME` and `SD_CANCEL`.
- `RAW_SEND("G28")` maps to `SAFETY_REJECTED`.
- CLI `pause`, `resume` and `cancel` are reserved for host-stream control
  in M23; SD controls are only `sd pause`, `sd resume` and `sd cancel`.
- `print FILE` remains the only legacy direct-serial CLI printer command
  until M22.
