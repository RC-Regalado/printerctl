---
title: Milestone 16 — Single-Print Daemon Integration Design
milestone: 16
status: done
date: 2026-08-25
tags: [printerctl, daemon, print-engine, ipc, plp1]
---

# Milestone 16 — Single-Print Daemon Integration Design

Source of truth: [[Agents]], [[docs/architecture|architecture]],
[[docs/roadmap|roadmap]], [[protocol-v1|docs/design/protocol-v1.md]].

M16 connects PLP/1 print-control requests to the existing M12
`printerctl_print_engine_t` through the M13 worker boundary.

```text
PLP/1 PRINT_* request
        │
        ▼
M15 socket/control context
        │
        ▼
cmd_pipe
        │
        ▼
worker-owned print_engine
        │
        ▼
printer / serial / Marlin
```

The worker is the only context that touches the engine. The control
context sends bounded internal commands and applies `PRINT_UPDATE` events
to its snapshot.

Because `printerctl_print_engine_run()` is blocking, M16 adds the generic
`printerctl_print_engine_set_control_poll()` hook. It has no daemon types.
The worker uses it to drain pending pause/resume/cancel/shutdown commands
during the engine's existing between-command control poll.

Implemented protocol surface:

- `PRINT_START`;
- `PRINT_PAUSE`;
- `PRINT_RESUME`;
- `PRINT_CANCEL`;
- STATUS execution fields: execution state, progress, commands sent,
  commands acknowledged, source position and failure reason.

`PRINT_START` success means accepted by the worker, not completed. The
client may disconnect after acceptance; execution continues in the worker.

No queue, persistent job ID, history, event subscription, CLI migration,
multi-printer support or automatic restart/resume was added.
