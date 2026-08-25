---
title: Milestone 13 Design - Daemon Boundary & Runtime Model
milestone: 13
status: done
date: 2026-08-21
tags: [printerctl, milestone, design, daemon, runtime, concurrency, ipc]
---

# Milestone 13 Design — Daemon Boundary & Runtime Model

Source of truth: [[Agents]] §§9-24, [[Kernel/Architecture]],
`docs/roadmap.md` (Phase 1 — Daemon Foundation + Single-Print IPC,
which supersedes the older Phase II roadmap's M13-M17 definitions for
this milestone).

This note records the concrete design decisions behind the Milestone 13
implementation, ahead of the milestone report in
[[milestone-13|docs/milestones/milestone-13.md]].

## 1. Scope

`docs/roadmap.md` and `Agents.md` already specify this milestone's
architecture, ownership model and lifecycles in detail — this note only
records the parts left to the implementer: concrete module layout, fd
ownership, and the exact points where the M13 fault requirements needed
a real design decision to become testable.

A new top-level module, `daemon`, independent of `core`/`printer`/
`serial`/`marlin`/`gcode`/`cli` (Agents.md §10): it will eventually host
`printerctl_print_engine_t` (Milestone 12) inside a worker thread, but
integrates nothing from it yet. Split into three files, matching this
repo's one-library-per-translation-unit convention:

| File | Owns |
|---|---|
| `include/printerctl/daemon/ipc.h` / `src/daemon/ipc.c` | `internal_command`/`worker_event` envelopes and their pipe framing |
| `include/printerctl/daemon/worker.h` / `src/daemon/worker.c` | the worker's own lifecycle and its `cmd_pipe`/`event_pipe` loop |
| `include/printerctl/daemon/runtime.h` / `src/daemon/runtime.c` | the control context: daemon lifecycle, published snapshot, public API |

Dependency direction: `ipc` ← `worker` ← `runtime`. Only `runtime.c`
touches `<pthread.h>` and links `Threads::Threads` — the first thread in
this codebase (`grep -rn pthread src` found nothing before this
milestone).

No new executable target. `printerctld`'s `main()` arrives once a later
milestone gives it something to serve (a socket, in M15); until then
this is a library validated entirely by
`printerctl_daemon_ipc_tests`/`printerctl_daemon_runtime_tests`.

## 2. fd ownership: single closer, no thread-side self-closing

`pthread_create()` shares one process-wide fd table — unlike `fork()`,
the worker thread does not get its own duplicated copy of `cmd_pipe`/
`event_pipe`'s fds. The classic fork-based pattern ("each side closes
the end it doesn't use") does not translate safely here: both threads
would be closing the *same* fd-table entry, risking a double-close or a
close racing a live read/write on that same descriptor number.

Decision: **the control context is the sole owner and sole closer of
all four fds, always, at exactly one deterministic point per code
path** — a failed `pipe()`/`pthread_create()`, a failed startup
handshake, or after `pthread_join()` on shutdown. `src/daemon/worker.c`
never calls `close()`.

Consequences, worth stating plainly because they shape the fault model:

- A worker thread cannot signal "I'm gone" through pipe EOF the way a
  crashed *process* would (the OS only closes an fd once its one
  remaining reference — control's — closes). Normal and faulted worker
  termination are detected primarily through the worker's own
  well-formed `STOPPED`/`FAULT` event, with `pthread_join()` as the hard
  confirmation the OS thread is truly gone.
- `event_pipe` EOF/read-failure is therefore a distinct, rarer case: the
  channel itself became unusable (in practice, only reachable in M13 by
  a test closing a fd directly), not "the worker exited normally."
  `printerctl_daemon_runtime_wait_event()` treats it as total loss of
  observability — `daemon_state = PRINTERCTL_DAEMON_FAILED` — which is
  deliberately *different* from a well-formed `FAULT` *event*, which
  only sets `worker_state = PRINTERCTL_DAEMON_WORKER_FAULTED` and
  leaves `daemon_state` alone (see §3).
- A worker that crashes outright, skipping its own event emission, is
  not distinguishable from "still working" by this design. This is an
  accepted, documented limitation of a thread-based (not process-based)
  worker — recorded here rather than silently glossed over, per
  Agents.md §20 — and is exactly the kind of risk `Agents.md` flags for
  why the worker will need very deliberate treatment once it owns real
  hardware state in later milestones.
- Tests simulate `cmd_pipe`/`event_pipe` failure by reaching into the
  transparent runtime struct and calling `close()` directly (e.g.
  `close(runtime.cmd_pipe[1])`) — the same technique
  `tests/test_printer.c` already uses on `session.port.fd`. No
  additional test-only API was needed.

**Review fix: `shutdown()` after an already-drained `FAULT` used to
deadlock.** The first implementation always sent `SHUTDOWN` and then
looped `printerctl_daemon_runtime_wait_event()` until it saw `STOPPED`
or `FAULT`. That is correct when the worker is still alive to answer —
but if the worker had already faulted *and* a caller had already
observed that `FAULT` through an earlier `wait_event()` call (draining
it), the worker thread body had already returned: nothing will ever
read the buffered `SHUTDOWN` command, and nothing will ever write
another event. Per the single-closer rule above, `event_pipe[1]` stays
open until final cleanup, so it never signals EOF on its own — the
drain loop's `read()` blocked forever. Fixed by checking
`snapshot.worker_state` before the write/drain: if it is already
`STOPPED` or `FAULTED` (meaning some earlier `wait_event()` call already
observed the worker's terminal event), `shutdown()` skips straight to
`pthread_join()` + fd cleanup instead of waiting for a second terminal
event that will never arrive.
`tests/test_daemon_runtime.c`'s
`test_shutdown_after_drained_fault_does_not_deadlock()` reproduces the
exact sequence (force a fault after `READY`, drain it with
`wait_event()`, then call `shutdown()`) under a `SIGALRM` watchdog that
turns a regression into a loud, immediate test failure instead of a
hung test binary — verified against the un-fixed code to confirm it
actually catches the bug before confirming it passes against the fix.

## 3. `daemon_state` vs. `worker_state`

These are deliberately separate dimensions, per the roadmap's own
worked example (`daemon = RUNNING`, `worker = FAULTED`, representable
without collapsing the two). Only
`printerctl_daemon_runtime_start()`/`_shutdown()` change `daemon_state`
in the ordinary course of the lifecycle;
`printerctl_daemon_runtime_wait_event()` changes it in exactly one other
case — the event-channel-lost failure from §2 — and otherwise only ever
updates `worker_state` (and `last_runtime_error` on a `FAULT` event).
This is what keeps "what to do about a faulted worker" a caller
decision: nothing here shuts anything down automatically, matching the
no-automatic-worker-restart invariant (Agents.md §8, §13).

## 4. Worker states reachable from more than one place

The worker lifecycle diagram in `docs/roadmap.md`/`Agents.md` §13 shows
`FAULTED` only as a sibling outcome of `INITIALIZING`. The
implementation also reaches `FAULTED` from `READY` — a malformed/IO
`cmd_pipe` read, or (defensively) a command type that slipped past
`printerctl_daemon_ipc_read_command()`'s own validation — because the
required `FAULT` event and the "worker fault reflected in the snapshot"
acceptance criterion only exercise something interesting if a fault can
also happen mid-operation, not only during startup. `cmd_pipe` EOF is
handled as a *separate*, non-faulting path: the worker treats it as
`control went away without an explicit SHUTDOWN`, still an orderly
`STOPPING`/`STOPPED` transition, not a fault (§7 of the roadmap's M13
section: "cmd_pipe EOF → orderly worker termination").

## 5. Worker-initialization fault-injection seam

M13 integrates nothing real yet — there is no genuine failure mode for
"worker initialization" to exercise, yet the acceptance criteria
require a real, deterministic fault test for exactly that path
("Worker initialization failure impide entrar en RUNNING"). Decision:
`printerctl_daemon_worker_args_t` carries an optional `init_hook`
function pointer (`bool (*)(void *context, char *out_message, size_t
out_message_size)`), and `printerctl_daemon_runtime_start()` takes it
as an explicit parameter (`worker_init_hook`/`worker_init_context`) —
production call sites always pass `NULL`/`NULL`, meaning "the worker's
initialization trivially succeeds." `tests/test_daemon_runtime.c`
passes a failing hook to exercise `PRINTERCTL_DAEMON_ERR_WORKER_FAULT`
deterministically, with no sleeps or timing assumptions.

This is a single function pointer, not a general plugin system — its
obvious real successor is Milestone 16's actual print-engine
initialization, which will need exactly this seam's shape (do real
work, report success/failure with a message) without any redesign.

## 6. Message framing

`printerctl_daemon_command_t`/`printerctl_daemon_event_t` are fixed-size
structs, known to both reader and writer at compile time — same binary,
no cross-version/wire concern, unlike the future Milestone 14 external
protocol. Each starts with a `wire_size` self-check field (satisfying
the roadmap's literal "size/type metadata" framing requirement)
followed by `type`/`token`/(`result`)/(`detail`).
`printerctl_daemon_ipc_write_*()`/`_read_*()` never assume one
`write()`/`read()` moves a whole message: they loop with
`EINTR`/`EAGAIN` retry, the exact same pattern
`printerctl_serial_write_line()`/`_read_line()` already use in
`src/serial/serial.c`. A short read exactly at a message boundary (0
bytes) is `PRINTERCTL_DAEMON_IPC_EOF`; a short read mid-message is
`PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED`; a full-length read whose
`wire_size`/`type`/`result` do not match what `ipc.h` defines is
`PRINTERCTL_DAEMON_IPC_ERR_MALFORMED`.

Neither envelope carries a payload: none of `NOP`/`SHUTDOWN` or the five
M13 worker events need one. `ipc.h` documents where Milestone 16 adds
one (once `PRINT_START` needs to carry a G-code path into the worker)
without redesigning the envelope — `wire_size` exists precisely so that
becomes a detectable, versioned-in-place change.

## 7. Testing

`tests/test_daemon_ipc.c` (framing/validation, no threads, plain
`pipe()` fd pairs): valid command/event round-trips (all five event
types), correlation token round-trip, unknown command/event type
rejected, invalid `result` rejected, a message truncated mid-write
rejected as distinct from a clean EOF on an empty pipe.

`tests/test_daemon_runtime.c` (lifecycle + integration + fault +
concurrency, modeled on `tests/test_printer.c`'s structure and its
`count_open_fds()`/`/proc/self/fd` descriptor-leak technique):
startup-reaches-RUNNING-only-after-READY, a correlated NOP round-trip,
normal shutdown (`STOPPING`→`STOPPED`→joined→`STOPPED`), the
worker-init-fault path (§5), a `pipe()`-creation failure via a tightened
`RLIMIT_NOFILE` (proving zero fd leakage on that path), `cmd_pipe` EOF
read as orderly termination rather than a fault, `event_pipe` failure
reported as `daemon_state = FAILED`, a rejected repeated `shutdown()`,
and a 20-cycle repeated create/start/NOP/shutdown loop asserting the
descriptor count returns to baseline every time — the primary defense
against leaks/deadlocks/races the milestone asks for.

`pipe()`-creation failure testing is deliberately scoped to "the first
`pipe()` call fails" (a maximally tight `RLIMIT_NOFILE` margin, robust
regardless of exact fd numbering) rather than also precision-testing
"the second `pipe()` call fails specifically." That second scenario's
no-leak guarantee is structurally visible by inspection in
`printerctl_daemon_runtime_start()` (`event_pipe` failure explicitly
closes `cmd_pipe` before returning) rather than proven by a second,
inherently more fragile rlimit-timing test.

`test_shutdown_after_drained_fault_does_not_deadlock()` (see the review
fix above) forces a post-`READY` `FAULT`, drains it with a direct
`wait_event()` call, then calls `shutdown()` under a `SIGALRM` watchdog
bounded at 5 seconds — a regression prints a labeled failure and
`_exit()`s rather than hanging the test binary. Confirmed to actually
fail (watchdog fires) against the pre-fix code before confirming it
passes against the fix.

Verification run: GCC and Clang, both warning-free under
`-Wall -Wextra -Wpedantic`; full `ctest` suite (11/11 binaries, existing
9 plus the 2 new) under both compilers; the daemon runtime binary run
standalone 15x outside ctest with a hard `timeout` wrapper to rule out
flakiness/hangs; ThreadSanitizer on the daemon test binaries (10
repeated runs, no races reported); ASan+UBSan on the full suite (11/11
pass) -- all re-run after the shutdown/deadlock fix, not only before it.
Valgrind was not run — it is not part of this project's existing
workflow (`docs/testing.md` never mentions it) and adding new tooling
infrastructure for it was out of this milestone's scope; noted here as
unavailable rather than silently skipped.

## 8. Deferred

- `AF_UNIX`/`SOCK_STREAM`, socket path/permissions/accept loop —
  Milestone 15.
- Printerctl Local Protocol v1 (external framing, versioning, message
  types) — Milestone 14 (design) / 15 (implementation).
- `print_engine` integration inside the worker, `printer`/`serial`/
  `marlin` involvement, real G-code execution — Milestone 16.
- `START`/`PAUSE`/`RESUME`/`CANCEL` internal commands and any
  execution/progress event payloads — conceptually reserved in `ipc.h`'s
  comments, not implemented.
- A `printerctld` executable/`main()` — arrives once M15 gives it a
  socket to serve.
- Automatic worker restart — permanently out of scope, not merely
  deferred (Agents.md §8, §13, §17, §21).
- Job queue, durable job IDs, persistence — Phase 3.
