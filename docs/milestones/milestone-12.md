---
title: Milestone 12 — Extract Print Engine
milestone: 12
status: done
date: 2026-08-21
tags: [printerctl, milestone, core, print-engine, daemon]
---

# Milestone 12 — Extract Print Engine

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]]. Design
details: [[milestone-12-print-engine|docs/design/milestone-12-print-engine.md]].

## 1. Technical summary

Phase II started with Milestone 12. The application-level print
lifecycle that used to live inside `src/cli/cli.c`'s
`run_print_command()` — connect, publish the active-print marker,
stream the file, write an interrupted-session record, remove the
marker, disconnect — is now owned by a new core module,
`printerctl_print_engine_t` (`include/printerctl/core/print_engine.h`,
`src/core/print_engine.c`). The CLI is a thin client adapter over it:
`run_print_command()` wires `argv`/config to the engine, installs the
operator's session-control signal handlers for the duration of the
stream, and renders every event the engine emits back into the exact
text `print` produced before this milestone.

The extraction reuses `printerctl_printer_print_file()` unchanged as
the lower-level ACK-driven streamer, exactly as planned — no rewrite of
the tested streaming path.

## 2. Files modified

- `include/printerctl/core/print_engine.h` (new) — public engine API:
  connect/run/disconnect, four async-signal-safe
  `printerctl_print_engine_request_*()` control setters, and the
  `printerctl_print_event_t` event vocabulary.
- `src/core/print_engine.c` (new) — the engine implementation: connect
  (+ best-effort active-print marker), run (streaming + interrupted-
  session record + terminal event), disconnect.
- `tests/test_print_engine.c` (new) — six engine-level tests over the
  same pty mock-Marlin pattern `tests/test_printer.c` uses; deliberately
  does not include `printerctl/cli.h` or anything from `src/cli`.
- `CMakeLists.txt` — new `printerctl_core` static library
  (`printerctl_config`, `printerctl_logging`, `printerctl_printer`,
  `printerctl_record` → `printerctl_core` → `printerctl_cli`).
- `tests/CMakeLists.txt` — new `printerctl_core_tests` target.
- `src/cli/cli.c` — `run_print_command()` rewritten as a client adapter;
  removed `print_progress_renderer_t`/`render_print_progress()`,
  `print_control_reporter_t`/`print_control_poll()`, the four
  `g_print_*_requested` file-scope flags and `write_session_record()`
  (all moved into or replaced by the engine); added
  `print_event_context_t`/`render_print_event()` (translates engine
  events back to the original operator-facing text) and
  `g_active_engine` + four signal handlers that each call one
  `printerctl_print_engine_request_*()` setter.

## 3. Risks found

- The current streamer is already tested; replacing it would add risk
  without satisfying new acceptance criteria. **Resolved**: reused
  `printerctl_printer_print_file()` verbatim; no changes to
  `src/printer`, `src/gcode`, `src/marlin` or `src/serial`.
- Terminal output and record-warning behavior must become events before
  the daemon can consume the same core. **Resolved**: every operator-
  facing message the CLI used to `fprintf()` directly around a print
  (start, progress, pause/resume, the power-failure notice, the marker
  warning, the record outcome, the terminal summary) is now a
  `printerctl_print_event_t`; `src/cli/cli.c`'s `render_print_event()`
  is the only place left that turns those into text.
- Signal handling must remain outside the core. **Resolved**: the
  engine never calls `sigaction()`; it only exposes four
  `request_*()` setters, each a single `sig_atomic_t` assignment. The
  CLI still owns all `sigaction()` calls.
- A fake non-blocking `tick()` API would be premature unless the daemon
  implementation actually needs it. **Resolved as planned**: not built.
  `printerctl_print_engine_run()` is a single blocking call, same as
  `printerctl_printer_print_file()`.

## 4. Decisions taken

- Reused `printerctl_printer_status_t`/`printerctl_printer_error_t`
  throughout the engine's public API instead of introducing a parallel
  `printerctl_print_engine_status_t` enum as the design doc's §3
  sketched. `printerctl_print_engine_connect()`,
  `_run()` and `_disconnect()` all return the same status vocabulary
  `printer.h` already defines, so the CLI's existing
  `printer_status_to_exit_code()` and `report_printer_failure()` needed
  no new mapping layer. See the design doc's new §10 for the full
  rationale.
- The event enum ended up with 11 members instead of the design doc's
  original 9: added `MARKER_WARNING` (the active-print marker publish
  failure was an operator-facing `fprintf()` too, and had to become an
  event under the same rule) and `CANCELLING_POWER_FAILURE` (see next
  bullet).
- The power-failure cancellation notice is now edge-triggered — emitted
  once, the first time a power-failure cancel is observed — instead of
  repeating on every ~100 ms control poll while the signal stays set,
  which is what the CLI's old `print_control_poll()` did. A deliberate,
  minor behavior simplification; `tests/test_cli.c`'s live-signal tests
  (which exercise this exact path end-to-end) still pass.
- `PRINTERCTL_PRINT_EVENT_PROGRESS` forwards every acknowledged-command
  callback undeduplicated; the CLI's event handler keeps its own
  `last_percent` state to reproduce the original once-per-whole-percent
  terminal output. The dedup is presentation policy, not core behavior.
- Session-record and active-print-marker logging (the `state_transition`
  / `WARNING` structured-log records around `write_session_record()`)
  moved into `src/core/print_engine.c` alongside the record-writing
  logic itself, following the same pattern `src/printer/printer.c`
  already uses (a module logs its own operations through a borrowed
  logger). The CLI's event handler only renders human text now; it
  performs no structured logging for `print`.
- The CLI installs its `sigaction()` handlers only after
  `printerctl_print_engine_connect()` succeeds (previously they were
  installed before `printerctl_printer_connect()`). A Ctrl+C during the
  connect phase now behaves the same as it already does for every other
  single-exchange command (`temp`, `send`, `home`, ...), which never
  installed custom signal handlers around its own connect either.

## 5. Verification

- `cmake -S . -B build-verify-gcc -DCMAKE_C_COMPILER=gcc` +
  `cmake --build build-verify-gcc`: clean, zero warnings under
  `-Wall -Wextra -Wpedantic`.
- `cmake -S . -B build-verify-clang -DCMAKE_C_COMPILER=clang` +
  `cmake --build build-verify-clang`: clean, zero warnings.
- `ctest --test-dir build-verify-gcc --output-on-failure`: 9/9 test
  binaries pass, including the new `printerctl_core_tests` and the
  existing `printerctl_tests` (CLI end-to-end, including the live-signal
  pause/resume/cancel/power-failure integration tests against a real
  subprocess).
- `ctest --test-dir build-verify-clang --output-on-failure`: 9/9, same
  result.
- `printerctl_core_tests` covers: a clean completion (event sequence and
  marker cleanup), a mid-stream cancel (record written, record content
  verified on disk), a power-failure cancel taking priority over an
  unresolved pause (record cause verified), a pause/resume cycle that
  still completes, a connect failure that never reaches the marker step,
  and a marker-publish failure surfaced as `MARKER_WARNING` without
  failing the connect. None of it includes `printerctl/cli.h` or any
  `src/cli` symbol.

## 6. Next steps

- Milestone 13 (job queue model) can build on
  `printerctl_print_engine_t` as its execution unit; no changes to this
  engine's public API were anticipated for that work beyond what
  `docs/design/milestone-12-print-engine.md` §9 already deferred.
- The Unix socket transport (Milestone 17) and the daemon itself remain
  future client adapters over this same engine, per the original design
  goal — no code changes needed here to enable that.
