# M29 Design — Macro Public Surface and PrusaSlicer Configuration Import

## Scope

M29 exposes the M28 built-in macros to clients and gives operators a way to
produce the daemon-owned `calibration.*` configuration from PrusaSlicer.

```text
printerctl macro list            -> PLP MACRO_LIST -> control context (registry + config)
printerctl macro run ID          -> PLP MACRO_RUN  -> worker (M28 execution path)
printerctl config from-prusa ... -> local file transform, stdout
```

Out of scope: `MACRO_CANCEL`, user-defined macros, a macro DSL, `park`
execution, queues/jobs/history, event subscriptions, TUI/Web.

## Protocol

See `docs/design/protocol-v1.md` §20a. Two additive message types in the
future range (`0x60`, `0x61`) and capability bit `MACROS`. Wire bounds equal
the macro domain bounds (`_Static_assert` in `src/daemon/socket.c`).

## Ownership

- MACRO_LIST is answered by the control context. It reads the immutable
  built-in registry and the configuration the runtime was started with
  (`printerctl_daemon_runtime_config()`, written before `pthread_create()`
  and never mutated) and calls the pure `printerctl_macro_resolve_builtin()`.
  No internal command, no serial I/O, no shared mutable state.
- MACRO_RUN only forwards the stable id over `cmd_pipe`. The worker keeps
  sole ownership of resolution, validation, command formatting, the
  print/macro exclusion and every serial write (M28).
- `printerctl_daemon_macro_calibration_config()` is the single projection
  of `calibration.*` keys used by both paths, so "listed as executable" and
  "accepted by the worker" use the same rules (the worker additionally
  rejects unrepresentable commands, M28-001).

## CLI

- `macro list`: one line per macro with id, category, `ready (N steps)` or
  `unavailable`, display name and description.
- `macro run ID`: prints acceptance, then reuses the PRINT-OBS-001 STATUS
  observer until the daemon reports a terminal state. Exit codes follow
  `print`: 0 completed, 9 cancelled, 10 power-failure cancel, otherwise
  mapped from `failure_reason`. Ctrl+C detaches the observer only.
  Rejections are explained (`INVALID_STATE` → not executable with the
  daemon configuration; `INVALID_ARGUMENT` → unknown; `BUSY`).
- `--dry-run` reports what would be requested without contacting the daemon.

## PrusaSlicer import

`printerctl config from-prusa FILE [INSET_MM [SAFE_Z_MM]]` reads a PrusaSlicer
printer profile (`.ini`) or the configuration block of a sliced ASCII
`.gcode`, and writes a printerctld configuration to stdout.

| printerctld key | Source |
|---|---|
| `calibration.min_x/max_x/min_y/max_y` | `bed_shape` bounding box shrunk by INSET_MM (default 30) |
| `calibration.xy_feedrate` | 60 × min(`travel_speed`, `machine_max_feedrate_x`, `machine_max_feedrate_y`) |
| `calibration.z_feedrate` | 60 × min(`travel_speed_z` if > 0, `machine_max_feedrate_z`) |
| `calibration.safe_z` | SAFE_Z_MM (default 5); checked against `max_print_height` |

Rules: normal-mode value of machine-limit pairs; absent or zero values are
unavailable, never "unlimited"; the slowest available source wins; config
bundles (`[sections]`) and binary G-code are rejected; serial keys are only
emitted commented out because a printer profile does not carry them. The
output is validated by the existing config loader in tests (round trip).

The 30 mm default inset keeps calibration points away from bed edges and
leveling screws; it is an operator default, not machine geometry.

## Safety

- No automatic motion: macros run only on explicit `macro run`.
- Macros never home; operators run `home` first. No homed-state tracking.
- Calibration bounds come from slicer data plus an inset; printerctl still
  makes no machine-bound safety claim (M28 limit, unchanged).
- `printerctl power low-battery` finds work through the active-print marker,
  which only host prints publish; a macro in progress is not signalled by
  that command. Direct SIGPWR to printerctld does cancel a macro (tested).
  Accepted debt: macros are a few seconds long.
