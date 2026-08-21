---
title: Milestone 12 Design - Print Engine Extraction
milestone: 12
status: done
date: 2026-08-21
tags: [printerctl, milestone, design, core, print-engine, daemon]
---

# Milestone 12 Design — Print Engine Extraction

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]].

This note starts Phase II. The goal is to extract the application-level
print engine from the current CLI path without implementing the daemon,
Unix socket, web gateway, job queue or IPC protocol yet.

## 1. Current state

The working streamer already exists and is tested in `printer`:

- `printerctl_printer_print_file()` owns the ACK-driven loop.
- It reads G-code through `gcode`.
- It sends one command at a time through `serial`.
- It waits for Marlin `ok` through `marlin`.
- It reports progress by callback.
- It receives pause/cancel decisions by callback.
- It distinguishes operator cancellation from power-failure
  cancellation.

The current coupling lives one layer above that, in `cli`:

- CLI argument shape (`printerctl_command_args_t`).
- terminal output (`FILE *out`, `FILE *err`, `fprintf()`).
- POSIX signal installation for pause/resume/cancel/SIGPWR.
- active-print PID marker publication.
- interrupted-session record creation.
- exit-code mapping.

Milestone 12 should move the application print lifecycle out of `cli`,
not rewrite the lower-level streamer.

## 2. Boundary decision

The print engine becomes the owner of this lifecycle:

```text
connect serial
publish active session marker
emit started event
run existing ACK-driven streamer
emit progress/state events
write interrupted-session record when needed
remove active session marker
disconnect serial
return typed result
```

The print engine must not know about:

- `argv` or parsed CLI command structs.
- terminal rendering.
- TUI.
- HTTP.
- WebSocket.
- JSON.
- Unix socket protocol.
- daemon process management.

The CLI becomes one client adapter:

```text
argv/signals/stdout/stderr
        |
        v
print engine API + callbacks
        |
        v
printer streamer / record / serial
```

The future daemon will become another client adapter over the same engine.

## 3. Minimal public API shape

The roadmap gives a conceptual API. The implementation should keep it
small and C-style, matching existing modules: stack values, explicit
status/error outputs and borrowed pointers unless documented otherwise.

Recommended public surface:

```c
typedef struct printerctl_print_engine printerctl_print_engine_t;

typedef enum {
    PRINTERCTL_PRINT_ENGINE_OK = 0,
    PRINTERCTL_PRINT_ENGINE_ERR_INVALID_ARGUMENT,
    PRINTERCTL_PRINT_ENGINE_ERR_CONNECT,
    PRINTERCTL_PRINT_ENGINE_ERR_PRINT,
    PRINTERCTL_PRINT_ENGINE_ERR_RECORD,
    PRINTERCTL_PRINT_ENGINE_ERR_DISCONNECT
} printerctl_print_engine_status_t;

typedef enum {
    PRINTERCTL_PRINT_CANCEL_NONE = 0,
    PRINTERCTL_PRINT_CANCEL_OPERATOR,
    PRINTERCTL_PRINT_CANCEL_POWER_FAILURE
} printerctl_print_cancel_reason_t;

typedef enum {
    PRINTERCTL_PRINT_EVENT_STARTED = 0,
    PRINTERCTL_PRINT_EVENT_PROGRESS,
    PRINTERCTL_PRINT_EVENT_PAUSED,
    PRINTERCTL_PRINT_EVENT_RESUMED,
    PRINTERCTL_PRINT_EVENT_CANCELLED,
    PRINTERCTL_PRINT_EVENT_COMPLETED,
    PRINTERCTL_PRINT_EVENT_FAILED,
    PRINTERCTL_PRINT_EVENT_RECORD_WRITTEN,
    PRINTERCTL_PRINT_EVENT_RECORD_FAILED
} printerctl_print_event_type_t;
```

Concrete function names may change during implementation, but the
contract should stay narrow:

- initialize a stack-value engine from config/logger/callbacks;
- start one print job from a file path;
- request pause/resume/cancel from the owning process;
- return a final summary equivalent to
  `printerctl_print_summary_t`;
- surface all operator-facing messages as events, not `fprintf()`.

Do not add a job queue or job IDs beyond the existing `job-<pid>` record
placeholder in this milestone. Milestone 13 owns the real queue model.

## 4. Cancellation model

Cancellation remains asynchronous at the client boundary and synchronous
inside the stream loop:

- CLI signal handlers may only set `sig_atomic_t` flags.
- The CLI adapter translates those flags into engine control requests.
- The engine translates its control state into the existing
  `printerctl_print_control_t` callback consumed by
  `printerctl_printer_print_file()`.
- Operator cancel maps to `PRINTERCTL_PRINT_CONTROL_CANCEL`.
- Power-failure cancel maps to
  `PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE`.

No cancellation path sends extra G-code to the printer. The only safety
action is still to stop transmitting after the current acknowledgement.

## 5. State model

The engine should expose explicit states because the daemon will need
them later, but M12 should only implement states exercised by the
current single-print lifecycle:

```text
idle
connecting
printing
paused
cancelling
completed
cancelled_operator
cancelled_power_failure
failed
```

Do not implement `queued` in M12. There is no queue until Milestone 13.

## 6. File/module plan

Start with the fewest production files that hold real behavior:

- `include/printerctl/core/print_engine.h`
- `src/core/print_engine.c`
- `tests/test_print_engine.c`

Add `cancellation`, `print_session`, `printer_state` or `job_state`
modules only when a second real caller or a non-trivial type needs the
split. The roadmap lists those as proposed modules; creating empty or
thin placeholder files would violate the no-dead-code rule.

## 7. Compatibility requirements

The CLI behavior must remain compatible:

- Existing print command output remains equivalent.
- Existing exit codes remain unchanged.
- Existing signal controls keep working.
- Existing session records keep the same schema.
- Existing tests continue passing.
- New engine tests prove the core can run without CLI parsing or
  terminal output.

## 8. Risks

- Rewriting `printerctl_printer_print_file()` would risk regressions in
  the most tested path. Reuse it.
- Moving record writing into the engine changes ownership of warnings:
  those must become events so CLI can still render them and daemon can
  store/log them.
- Signal handling must stay outside the engine. Signals are a CLI
  transport detail, not a core-control API.
- A non-blocking `tick()` API would be useful for a daemon, but forcing
  the existing blocking streamer into a fake tick loop in M12 would add
  complexity before the daemon exists.

## 9. Deferred

- Unix domain socket command transport: Milestone 17.
- Web gateway/WebSocket/SSE subscriptions: later Phase II milestones.
- Real job queue and durable job IDs: Milestone 13.
- Multi-client access control: daemon milestones.
- Non-blocking event loop integration: only if the daemon implementation
  proves the blocking engine cannot be isolated safely.

## 10. Implementation notes (as built)

Implemented in `include/printerctl/core/print_engine.h` and
`src/core/print_engine.c`; see
[[milestone-12|docs/milestones/milestone-12.md]] §2-§5 for the file list
and verification results. A few points where the built engine differs
from this note, each a deliberate simplification rather than a scope
change:

- **No separate `printerctl_print_engine_status_t`.** §3 sketched a
  parallel status/error enum for the engine. The implementation reuses
  `printerctl_printer_status_t`/`printerctl_printer_error_t` directly:
  `connect()`, `run()` and `disconnect()` all return exactly what the
  underlying `printer.h` calls would have returned. This means the CLI's
  existing `printer_status_to_exit_code()` and `report_printer_failure()`
  work unchanged against the engine's return value — the "narrow
  contract" §3 asked for turned out to be narrower still: one
  vocabulary, not two synchronized ones.
- **Event enum grew by two members.** Beyond the 9 event types §3
  proposed, the implementation adds `MARKER_WARNING` (the active-print
  marker publish failure is exactly the kind of operator-facing
  `fprintf()` the "surface all operator-facing messages as events" rule
  targets, and §3's list simply omitted it) and
  `CANCELLING_POWER_FAILURE` (see next point).
- **The power-failure cancelling notice is edge-triggered.** The CLI's
  pre-M12 `print_control_poll()` re-printed "print cancelling:
  power-failure/low-battery signal received" on every ~100 ms control
  poll for as long as the signal stayed set (e.g. while also paused).
  The engine emits `CANCELLING_POWER_FAILURE` exactly once, the first
  time the condition is observed, the same edge-triggered treatment
  `PAUSED`/`RESUMED` already get. Compatibility is judged against
  `docs/design/milestone-12-print-engine.md` §7's actual criteria (exit
  codes, passing tests, working signal controls, unchanged record
  schema) — all still hold; `tests/test_cli.c`'s live-signal integration
  tests pass unchanged.
- **`PROGRESS` is undeduplicated.** The engine forwards every
  acknowledged-command callback as a `PROGRESS` event; the CLI's event
  handler keeps its own `last_percent` state to reproduce the original
  once-per-whole-percent terminal line. A live daemon subscriber may
  want every acknowledgement, not just percent changes, so the dedup
  stayed a client concern rather than moving into the core.
- **Record/marker structured logging lives in the engine, not the CLI.**
  `write_session_record()`'s `state_transition`/`WARNING` log calls
  moved into `src/core/print_engine.c` alongside the record-writing code
  they describe, matching the pattern `src/printer/printer.c` already
  uses (a module logs its own operations through a borrowed logger). The
  CLI's event handler renders text only; it performs no structured
  logging for `print` anymore.
- **Signal handlers install after `connect()`, not before.** The
  pre-M12 CLI installed `sigaction()` before opening the serial port; the
  adapter now installs them only once `printerctl_print_engine_connect()`
  has succeeded. A Ctrl+C during the connect phase now behaves like every
  other single-exchange command (`temp`, `send`, `home`, ...), none of
  which ever installed custom handlers around their own connect either.
