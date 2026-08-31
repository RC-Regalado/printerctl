# M24 Design — Power Integration Alignment

## Objective

Preserve audited low-battery cancellation with daemon-owned execution.

## Preferred path

```text
APCUPSD
  ↓
printerctl power low-battery
  ↓
daemon marker/PID
  ↓
SIGPWR printerctld
```

## Constraints

Do not replace SIGPWR merely for PLP symmetry.

Preserve:
- idempotence;
- no-active-print success;
- interrupted-session semantics;
- R6 TX barrier;
- no auto-resume;
- no physical cleanup commands.
