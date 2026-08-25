---
title: Milestone 13 — Daemon Boundary & Runtime Model
milestone: 13
status: done
date: 2026-08-21
tags: [printerctl, milestone, daemon, runtime, concurrency, ipc]
---

# Milestone 13 — Daemon Boundary & Runtime Model

Source of truth: [[Agents]], [[Kernel/Architecture]]. Roadmap: `docs/roadmap.md`
(Phase 1). Design details:
[[milestone-13-daemon-runtime|docs/design/milestone-13-daemon-runtime.md]].

## 1. Technical summary

Milestone 12 extracted the print lifecycle into
`printerctl_print_engine_t`, deliberately transport/UI-agnostic. M13
builds the hardware-independent runtime foundation the future
`printerctld` daemon needs to host that engine inside a worker thread
without blocking whatever eventually serves client requests (the
existing engine is intentionally blocking; running it directly on a
future socket-handling thread would make the daemon unresponsive during
a long print).

This milestone adds a new, independent module, `daemon`: a control
context (`printerctl_daemon_runtime_t`) that owns the daemon/runtime
lifecycle and a published snapshot, one worker thread, and two
unidirectional pipes (`cmd_pipe` control→worker, `event_pipe`
worker→control) carrying a small internal command/event contract with
correlation tokens. It integrates none of the print engine, `printer`,
`serial`, `marlin`, G-code execution, the Unix socket, or the external
wire protocol — all of that is explicitly deferred to Milestones 14-17
per `docs/roadmap.md`.

This is the first thread introduced anywhere in the codebase.

## 2. Files changed

- `include/printerctl/daemon/ipc.h`, `src/daemon/ipc.c` (new) — the
  `internal_command`/`worker_event` envelopes and their pipe framing
  (fixed-size structs, `wire_size` self-check, partial-I/O-safe
  read/write loops).
- `include/printerctl/daemon/worker.h`, `src/daemon/worker.c` (new) —
  the worker thread's own lifecycle and its `cmd_pipe`/`event_pipe`
  loop (`NOP`/`SHUTDOWN`, `READY`/`COMMAND_COMPLETED`/`STOPPING`/
  `STOPPED`/`FAULT`), plus the worker-init fault-injection seam.
- `include/printerctl/daemon/runtime.h`, `src/daemon/runtime.c` (new) —
  the control context: `start()`/`send_nop()`/`wait_event()`/
  `shutdown()`/`snapshot()`, the startup/shutdown handshakes, fd
  ownership and cleanup.
- `tests/test_daemon_ipc.c` (new) — framing/validation unit tests.
- `tests/test_daemon_runtime.c` (new) — lifecycle, IPC round-trip,
  fault-injection and repeated-cycle/descriptor-leak tests, plus (review
  fix round) a deadlock-regression test.
- `CMakeLists.txt` — `find_package(Threads REQUIRED)` (new); three new
  static libraries (`printerctl_daemon_ipc`, `_worker`, `_runtime`).
- `tests/CMakeLists.txt` — two new test targets.
- `docs/architecture.md` — new §21 implementation-status note (this
  file had already been relocated from the repository root and rewritten
  as the Phase 1 target architecture ahead of this milestone, outside
  this implementation session).
- `docs/roadmap.md` — M13 status line updated to reflect completion.
- `README.md` — Status section and the Test section's binary count/list
  updated from Milestone 11/eight binaries to Milestone 13/eleven
  binaries (review fix round; this had also drifted during Milestone 12
  and was corrected here).

No changes to `core`, `printer`, `serial`, `marlin`, `gcode`, `cli`, or
`Components.md`/`context-ia.md` (M13 has no effect on the serial/Marlin
wire contract those two describe).

## 2a. Review fixes (post-implementation round)

An independent review required three fixes before merge, all addressed
in this same milestone rather than deferred:

1. **`shutdown()` after an already-drained post-`READY` `FAULT`
   deadlocked.** `src/daemon/runtime.c`'s `printerctl_daemon_runtime_shutdown()`
   unconditionally sent `SHUTDOWN` and drained events until a second
   terminal event arrived — but if the worker had already faulted and a
   caller had already observed that `FAULT` via an earlier
   `wait_event()` call, the worker thread body had already returned and
   nothing would ever write another event, while `event_pipe[1]`
   (per the single-closer fd-ownership rule) stays open until final
   cleanup and so never signals EOF on its own. Fixed by checking
   `snapshot.worker_state` first: if it is already `STOPPED` or
   `FAULTED`, `shutdown()` skips the write/drain and goes straight to
   `pthread_join()` + fd cleanup. See the design doc's updated §2.
2. **Regression test added.** `test_shutdown_after_drained_fault_does_not_deadlock()`
   in `tests/test_daemon_runtime.c` forces a post-`READY` fault, drains
   it, then calls `shutdown()` under a 5-second `SIGALRM` watchdog that
   turns a hang into a loud, immediate failure rather than blocking the
   suite forever. Verified against the pre-fix code (watchdog fired,
   confirming the test actually catches the bug) before confirming it
   passes against the fix.
3. **`README.md` updated** to Milestone 13 and the correct 11-binary
   test list (it had drifted since Milestone 11/eight binaries, not
   updated during Milestone 12 either).

## 3. Verification

- `cmake -S . -B build-verify-gcc -DCMAKE_C_COMPILER=gcc` +
  `cmake --build`: clean, zero warnings under `-Wall -Wextra -Wpedantic`.
- Same with `-DCMAKE_C_COMPILER=clang`: clean, zero warnings.
- `ctest --test-dir build-verify-{gcc,clang} --output-on-failure`:
  11/11 test binaries pass under both compilers (the existing 9 plus
  `printerctl_daemon_ipc_tests` and `printerctl_daemon_runtime_tests`).
- `printerctl_daemon_runtime_tests` run standalone 15 times in a row
  (outside ctest) with zero failures, to check for scheduling-dependent
  flakiness before trusting the single ctest run.
- ThreadSanitizer (`-fsanitize=thread`, Clang): `printerctl_daemon_runtime_tests`
  and `printerctl_daemon_ipc_tests` built and run 10x each — no data
  races reported.
- ASan+UBSan (`-fsanitize=address,undefined`, Clang): full 11-binary
  suite built and run — 11/11 pass, no sanitizer reports.
- Valgrind: not run. Not part of this project's existing workflow
  (`docs/testing.md` does not mention it); reported here as unavailable
  rather than silently skipped, per the task's own instruction to report
  tooling that could not be used and why.
- **Review fix round**: all of the above (GCC, Clang, 11/11 ctest under
  both, TSan x10, ASan+UBSan) re-run after the `shutdown()` deadlock fix
  (§2a) — all still pass. The new regression test was additionally
  confirmed to actually fail (via the `SIGALRM` watchdog) against the
  pre-fix code before confirming it passes against the fix.

## 4. Risks found

- **Thread-shared fd table breaks the naive fork-style "close what you
  don't use" pipe pattern.** Resolved by making the control context the
  sole closer of all four fds (see the design doc §2) — the main
  architectural risk of this milestone, and the one most worth an
  independent reviewer's attention.
- **Worker-crash detection is inherently weaker than a forked-process
  daemon would give for free.** A worker that crashes without emitting
  its own `FAULT`/`STOPPED` event is not currently distinguishable from
  "still working." Documented as an accepted M13 limitation (design doc
  §2) rather than solved — there is no real hardware/engine ownership
  yet for a crash to meaningfully protect against, so over-engineering
  crash detection now would be premature.
- **No genuine failure mode exists yet for "worker initialization."**
  Resolved with a minimal, production-inert fault-injection seam (design
  doc §5) rather than skipping that required test path.

## 5. Decisions taken

See [[milestone-13-daemon-runtime|the design doc]] §§2-6 for the full
reasoning behind each. Summary:

- Control context is the sole owner/closer of all four pipe fds.
- `daemon_state` and `worker_state` are updated independently;
  `wait_event()` only ever changes `daemon_state` on total event-channel
  loss, never on a well-formed `FAULT` event.
- `FAULTED` is reachable from `READY`, not only from `INITIALIZING`
  (the roadmap's lifecycle diagram shows only the latter edge, but the
  required FAULT-in-snapshot acceptance criterion needs the former too).
- `cmd_pipe` EOF is orderly (`STOPPING`→`STOPPED`), not a fault.
- `printerctl_daemon_runtime_start()` takes an explicit
  `worker_init_hook`/`worker_init_context` pair rather than hiding a
  test seam behind an internal-only code path; production call sites
  pass `NULL`/`NULL`.
- Message envelopes are fixed-size, same-binary structs with a
  self-describing `wire_size` field — not a serialized wire format
  (that belongs to Milestone 14's genuinely cross-process protocol).
- Three separate CMake library targets (`ipc`/`worker`/`runtime`)
  rather than one bundled `daemon` library, matching this repo's
  existing one-library-per-translation-unit convention.

## 6. Next steps

- Milestone 14 designs the external Printerctl Local Protocol v1 (wire
  framing, versioning, message registry) — a pure design milestone, no
  implementation.
- Milestone 15 implements the `AF_UNIX`/`SOCK_STREAM` transport and the
  M14 parser/serializer, still without touching the print engine.
- Milestone 16 connects this runtime's worker to
  `printerctl_print_engine_t`, replacing the worker's current
  no-op `init_hook`/`NOP` handling with the real engine lifecycle, and
  is the milestone that will actually exercise the worker-fault path
  with a genuine failure mode (a real connect failure) instead of the
  M13 test-only seam.
- Independent review should focus first on the fd-ownership model
  (design doc §2) and the `daemon_state`/`worker_state` update rules
  (design doc §3), per Agents.md §7's review priorities (safety,
  ownership/lifetime, concurrency ahead of style).
