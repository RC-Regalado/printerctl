# M23 Design — Host Print Control UX

## Objective

Make top-level pause/resume/cancel operate on daemon-owned host streams.

## Target

```text
printerctl pause
printerctl resume
printerctl cancel
```

SD becomes:

```text
printerctl sd pause
printerctl sd resume
printerctl sd cancel
```

## No context-sensitive dispatch

Never infer SD vs host from current state.

Intent must be explicit in syntax.

## Repeated operations

Add tests for:
- repeated pause while paused;
- repeated resume while resumed.

## Signals

Client signals are UI concerns, not execution ownership.

Do not restore PID-based host execution control through the client.
