# Phase 2 Architecture

## Baseline

Phase 1 is approved.

Existing daemon path:

```text
PLP/socket
   ↓
printerctld control
   ↓
runtime command
   ↓
worker
   ↓
print_engine
   ↓
printer
   ↓
serial / Marlin
```

Do not invert or bypass this ownership.

## Target architecture

```text
                         printerctl
                            │
              ┌─────────────┴─────────────┐
              │                           │
        local operations              PLP client
              │                           │
     help/version/dry-run                  │
     presentation                          ▼
                                     Unix socket
                                          │
                                          ▼
                                     printerctld
                                          │
                                    daemon runtime
                                          │
                                         worker
                                          │
                                    print_engine
                                          │
                                       printer
                                          │
                                       serial
                                          │
                                       Marlin
```

## printerctl owns

- argument parsing;
- CLI UX;
- stdout/stderr rendering;
- exit-code mapping;
- client diagnostics;
- `--dry-run print FILE`;
- client-side configuration.

During M18-M24, unmigrated direct CLI commands may still use the
transitional legacy hardware-config view (`port`, `baud`, Marlin
`timeout`). A command loses that access when it migrates to PLP/daemon
execution. The view is removed in M25.

## printerctld owns

- serial device;
- baud rate;
- Marlin response timeout;
- mutable execution;
- real printer queries;
- raw command execution;
- homing execution;
- SD control commands;
- host print lifecycle;
- safety enforcement tied to hardware.

## Strict dependency rules

Allowed:

```text
cli -> client
client -> protocol
client -> socket API
daemon socket -> protocol
daemon worker -> core
core -> printer
printer -> serial
```

Forbidden:

```text
cli -> serial
cli -> printer
cli -> daemon internals
protocol -> printer
protocol -> serial
core -> socket
core -> PLP
socket handler -> direct printer execution
signal handler -> printer / serial / print_engine
```

## Migration rule

```text
LEGACY_DIRECT
     ↓
DAEMON_RPC
```

After migration, a command does not silently fall back to serial.

## Query architecture

STATUS is snapshot-only.

Active queries:

```text
CLI
 ↓
PLP query
 ↓
worker
 ↓
printer
 ↓
Marlin
 ↓
domain parser
 ↓
structured PLP response
```

## Physical intent

```text
SEND "G28"
→ reject

HOME X
→ explicit intent
→ validate
→ execute
```

Client checks are optional UX. Daemon checks are authoritative.

## Host print

```text
printerctl print FILE
        ↓
PRINT_START
        ↓
printerctld accepts
        ↓
client observes via STATUS/polling
```

Execution survives client disconnect.

## SD print

```text
sd pause  -> M25
sd resume -> M24
sd cancel -> M524
```

Never confuse these with host-stream PRINT_PAUSE/RESUME/CANCEL.

## Cancellation

Phase 1 R6 remains authoritative:

```text
cancellation committed
   ↓
TX gate closed
   ↓
no newly-authorized G-code TX
```

## Multi-client note

The Phase 1 server is single-client-at-a-time.

Evaluate this early when an attached print observer and a second control client must coexist. Do not redesign concurrency unless required by actual Phase 2 behavior.
