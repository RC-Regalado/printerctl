# M28 Independent Re-Review

Date: 2026-10-14 (America/El_Salvador)
Inputs: [M28 review](m28-review.md), [M28 remediation record](m28-remediation.md),
`Agents.md` §8–12, `docs/design/m28-macro-execution.md`.

## Verdict: APPROVE WITH REQUIRED FIXES

The four product findings (M28-001, M28-002, M28-003, UX-001) are fixed in
code. Mutation checks confirm that the regression tests for M28-003, UX-001 and
most of M28-002 detect the original bugs. M28 must not be marked ACCEPTED yet,
for two reasons. First, M28-004 is **not** resolved: the ASan `printerctld_tests`
gate still times out intermittently in an isolated namespace with
`TMPDIR=/tmp`, and I identified a test-side cause (RR-001). Second, the
remediation record contains inaccurate closure claims (RR-004). The required
fixes are limited to tests, docs and one formatting rule. No macro runtime
redesign is needed.

### Scope and baseline

- The production remediation is committed in `HEAD` (`c338d34`). The new
  regression tests in `tests/test_daemon_print.c` are uncommitted
  (sha256 `c5b8d9dc…`). I reviewed and built a read-only snapshot,
  `git archive HEAD` plus the working-tree `tests/test_daemon_print.c`,
  under `/tmp/m28rr/snap`.
- While I was reviewing, the shared working tree changed: someone started
  uncommitted M29 work (MACRO_RUN, `CAP_MACROS`, `milestone-29.md`). It also
  changed `docs/milestones/milestone-28.md` to "REMEDIATED … acceptance per
  m28-re-review.md". The macro execution path in `src/daemon/worker.c` differs
  from HEAD only by the rename `macro_config_from_runtime` →
  `printerctl_daemon_macro_calibration_config`, so these conclusions apply to
  it. A working-tree build taken mid-edit failed
  `printerctl_daemon_socket_tests` deterministically ("VERSION exposes M16
  status…"), because the test and `socket.c` capability bitmaps were out of
  sync at that moment. That is M29 work in progress, not M28.

## Per-finding verification

| Finding | Fixed? | Code evidence | Test evidence |
|---|---|---|---|
| M28-001 unchecked formatting | **Yes** (residual: RR-002) | `src/daemon/worker.c:552-568`: every `snprintf` is checked for `n<0` and truncation, `used` only advances on success, non-zero \|v\|<0.001 is rejected, F<0.001 is rejected. `worker.c:603-610` formats **all** steps before `printerctl_printer_connect` (`:613`), so a rejection causes zero TX. NaN/Inf are impossible: `config.c:97` and `macro.c:106-107` require finite values and feedrate >0. | `test_macro_unrepresentable_values_have_zero_serial_side_effects` (±1e200, F0.0004): INVALID_ARGUMENT, IDLE, no TX. Feedrate-floor mutation → test FAILS. The tiny-coordinate mutation (`fabs<0.001`) **survives** because no test covers it; my reproducer showed the code rejects `min_x=0.0004`. The faithful "remove truncation check" mutation was not run (see Verification). |
| M28-002 power/shutdown in ACK wait | **Yes** | `printer.c:245-248` checks control before TX. `printer.c:103-111` and `serial.c:108-111,118-121,135` check the interrupt before every poll/byte, with 100 ms slices that stay bounded during chatter. `worker.c:492-550`: the macro poll consumes the signal power/cancel intents, accepts PRINT_CANCEL_POWER_FAILURE (`:538-542`) and SHUTDOWN (`:511-518`). Cancellation is sticky in `ctx->macro_cancel`, so no later TX is possible. The final G90 runs only on OK (`:650`). There is no cleanup G-code. STOPPED is emitted after the terminal macro update (`:676-679`), and the worker returns (`:838-840`). | `…power_failure_during_ack_wait…` and `…shutdown_during_busy_chatter…`. Mutations that remove ACK-wait interruption, shutdown→cancel, power-failure distinction or STOPPED make the tests FAIL (the STOPPED mutation only shows as a hang). The signal-intent mutation (`worker.c:495-500` disabled) **survives**: RR-003. |
| M28-003 inch mode | **Yes** | `worker.c:630`: G21 is sent and must return a terminal `ok` before any G90/G91 or move. Failure or cancellation skips the loop and the final G90 (`:631`, `:650`). | `…establishes_millimeters_after_inch_mode`, `…unit_mode_failure_prevents_motion`. Removing G21 → test FAILS. |
| UX-001 stale terminal snapshot | **Yes** | `worker.c:704-709` emits RUNNING (counters 0; `print_engine.c:132` memset resets `commands_total`) on `event_pipe` before COMMAND_COMPLETED. `runtime.c:470-487` replaces all execution fields, and `wait_command` (`runtime.c:493-527`) applies them in FIFO order before returning success. | `test_sequential_print_start_publishes_new_execution` (after COMPLETED and after FAILED). Removing the pre-acceptance RUNNING → test FAILS. |
| M28-004 sanitizer timeout | **No**: reproduced, and the record's diagnosis is wrong | See RR-001. | ASan `printerctld_tests` in bwrap (private /tmp, `TMPDIR=/tmp`): **3/5 Timeout (120 s)**. Without isolation: 1/3 Timeout. |

### Edge-case reproducers (`/tmp/m28rr/rr.c`, real runtime, worker and PTY, ASan)

| Scenario | Result |
|---|---|
| Power-failure command during **G21** ACK wait | CANCELLED, reason SHUTTING_DOWN, step 0, no TX after late ACK |
| `printerctl_daemon_worker_request_power_failure_from_signal()` only (no command) during G21 | Same as above: the signal path works but is untested (RR-003) |
| Shutdown during **final G90** ACK wait | Finishes without the ACK, CANCELLED (step 1/1), worker STOPPED, no further TX |
| Shutdown during G91 ACK wait | `runtime_shutdown` OK, worker STOPPED, daemon STOPPED, CANCELLED, no TX after late ACK |
| `min_x = -0.0` | Accepted; sends `G1 X-0.000 …` (harmless) |
| `min_x = 0.0004`, `safe_z = 0.0005` | Rejected before connect, zero TX |
| `safe_z = 1e60` | Accepted; sends `G1 Z999…904.000 F300.000` (78 B), beyond float32 range (RR-002) |
| `safe_z = 1e100` | Accepted; sends a **119-byte** line, beyond Marlin's default `MAX_CMD_SIZE` of 96 (RR-002) |

I found no TX after cancellation commit, no automatic cleanup G-code, and no
unbounded block in the macro path once cancellation is requested. Connect,
write and close are all bounded by `timeout_ms`.

## New findings

### RR-001: M28-004 root cause is a racy test plus orphaned daemons (RESOLVE_SOON, required before M28 closure)

- Location: `tests/test_printerctld.c:1837-1840`
  (`test_cli_print_sigint_kills_observer_not_the_print`) together with
  `query_print_status_at_least`, `:499-514`. Also every `check()` failure
  path in that file.
- Problem: the helper returns as soon as `commands_acknowledged >= 4`. The
  worker publishes PROGRESS(ack=4, RUNNING) before COMPLETED, so under ASan
  the next assertion often sees RUNNING. It failed with "FAIL: the
  daemon-owned print reaches COMPLETED…" in 1/3 runs from the ctest working
  directory and 3/3 from another. When `check()` calls `exit(1)`, the spawned
  `printerctld` is not killed. It is reparented to PID 1 (I observed it and
  then killed it) and keeps ctest's output pipe open, so ctest reports a
  120 s **Timeout** instead of a FAIL. The remediation's "long TMPDIR" cause
  and "NOT REPRODUCED" status (`m28-remediation.md:15`) are therefore
  incorrect: the failure reproduces with `TMPDIR=/tmp` in a private /tmp. The
  hard-coded default socket `/tmp/printerctl.sock` also lets concurrent runs
  from different build trees collide: one of my runs overlapped an orphaned
  `build/printerctld` started at the same second.
- Fix: poll until a terminal `execution_state` (with a bounded retry budget)
  instead of an ack threshold. Kill and reap spawned daemons and CLIs on
  failure (atexit/cleanup registry, or `PR_SET_PDEATHSIG` in the child). Run
  the suite in a private /tmp (bwrap) or parameterize the socket path. Then
  re-run the ASan gate at least 5 times and record the results.

### RR-002: The representability rule ignores firmware line-length and numeric range (RESOLVE_SOON)

- Location: `src/daemon/worker.c:552-568,603`. The 128-byte buffer is the
  only bound.
- Problem: lines up to 127 bytes and values up to about 1e100 pass. Marlin's
  default `MAX_CMD_SIZE` is 96, so a longer line can be truncated by the
  firmware (for example, dropping `F`). Values above FLT_MAX (about 3.4e38)
  parse as ±inf in the firmware. This does not invent machine geometry, but
  it does fall short of the original review's requirement to "define numeric
  representability/precision rules" for the wire. It needs an operator-authored
  absurd config, so it is not blocking.
- Fix: reject generated lines longer than 95 bytes (a named constant) and
  \|value\| above a documented representable maximum (for example ≤1e6 mm,
  or at least float32-finite). Normalize `-0.0` to `0`. Add tests for the
  tiny non-zero coordinate, the long line and the over-range cases.

### RR-003: The macro signal-intent path has no regression test (RESOLVE_SOON)

- Location: `src/daemon/worker.c:495-500`. Tests: `tests/test_daemon_print.c:794-832`.
- Problem: the original M28-002 finding was specifically that the macro poll
  ignored the worker's signal flags. The new test exercises only the
  PRINT_CANCEL_POWER_FAILURE *command*. Disabling the flag consumption leaves
  `printerctl_daemon_print_tests` green. The behaviour is correct today (my
  reproducer passed), but nothing protects it.
- Fix: add a test that calls
  `printerctl_daemon_worker_request_power_failure_from_signal()` (and the
  cancel variant) during a held ACK, with no command, and asserts CANCELLED,
  the right reason, and no TX after the late ACK. Also make
  `test_macro_shutdown_during_busy_chatter_completes` assert no TX after
  shutdown: its comment (`:906`) promises this but the check is missing.

### RR-004: The remediation record and docs over-claim closure (RESOLVE_SOON)

- `m28-remediation.md:15`: M28-004 status and root cause are wrong (RR-001).
- `m28-remediation.md:36-38`: says "M29 adds MACRO_RUN and covers both
  scenarios in `printerctld_tests`." HEAD has no MACRO_RUN and no
  process-level macro SIGTERM/SIGPWR test. Process-level macro signal
  delivery remains NOT TESTED.
- `docs/design/m28-macro-execution.md:148-183` does not yet describe G21,
  the unit-mode postcondition, ACK-wait cancellation, power-failure macro
  semantics or the representability rule.
- The working tree already starts M29 and marks M28 "REMEDIATED … acceptance
  per m28-re-review.md". That contradicts `milestone-28.md`'s former "Do not
  start M29 until M28 is accepted" and this verdict.
- Fix: correct the record, update the design doc, and keep M28 status short
  of ACCEPTED until RR-001 to RR-003 are closed.

### RR-005: Signal intents are consumed by the macro but latched by the print path (ACCEPTABLE)

- `worker.c:495-499` uses `atomic_exchange`, while `poll_control_commands`
  (`:389-395`) only loads, and the idle loop clears the flags on
  SHUTDOWN/PF. After a macro consumes SIGTERM intent, a command that reaches
  the worker before the follow-up SHUTDOWN is not pre-cancelled by the flag.
  The window is small and SHUTDOWN still follows, so this is consistent with
  the current design. Consider latching the cancel intent until SHUTDOWN.

### RR-006: Observer and cancel residuals (FUTURE_IMPROVEMENT)

- The CLI print observer (`src/cli/cli.c:842`) does not check
  `execution_kind` and has no execution id. With M29 MACRO_RUN, a macro
  starting right after a fast print could be attributed to the print.
- A cancelled macro leaves a transmitted command un-ACKed. Shutdown during
  the final G90 reports CANCELLED although every move completed. Both match
  the documented postcondition ("coordinate mode unknown"), and no cleanup is
  correct per `Agents.md` §8.
- Busy chatter without cancellation keeps a macro step waiting indefinitely.
  That is acceptable, because legitimate long moves emit `busy:`, and
  cancellation now interrupts it.

## Verification commands and results

| Command | Result |
|---|---|
| `cmake -S . -B build-rereview -DCMAKE_C_COMPILER=gcc && cmake --build build-rereview -j8` (working tree) | Built |
| `TMPDIR=/tmp ctest --test-dir build-rereview --timeout 120` (working tree, mid-M29 edits) | 20/21. `printerctl_daemon_socket_tests` failed deterministically (M29 capability-bitmap edit in flight) |
| Snapshot `git archive HEAD` + new test file, normal: `TMPDIR=/tmp ctest --test-dir b --timeout 120` | **20/20 PASS** |
| Snapshot ASan/UBSan (`-fsanitize=address,undefined -g -O1`), full ctest | 20/20 PASS (one run) |
| `printerctl_daemon_print_tests` ×15 normal, ×15 ASan | 30/30 PASS |
| ASan `printerctld_tests` ×3 unisolated | 1 Timeout, 2 PASS |
| ASan `printerctld_tests` ×5 in `bwrap --tmpfs /tmp --unshare-pid`, `TMPDIR=/tmp` | **3 Timeout**, 2 PASS |
| ASan `printerctld_tests` binary run directly ×6 (bwrap) | 4 FAIL "daemon-owned print reaches COMPLETED…", 2 PASS |
| Mutation script `/tmp/m28rr/mutate.py` (snapshot only) | See the per-finding table. 7 mutations killed, 2 survived (tiny-coordinate check, signal intents). A third mutant (per-axis truncation check replaced by a clamp) also passed, because the final `F` check still rejects the line. That is defense in depth, not a test gap. |
| Edge reproducers `/tmp/m28rr/rr.c` (ASan) | See the reproducer table |
| Faithful M28-001 mutation (drop the truncation check entirely, ASan) | NOT RUN: the user denied the command |
| TSan | NOT RUN in this re-review |
| Real hardware | NOT TESTED (PTY only, by design) |

I only touched the repository to create this report and the `build-rereview*`
build directories. Snapshot builds, reproducers and mutations ran under
`/tmp/m28rr`. I killed the orphaned `printerctld` processes that my runs
created. An orphaned `build/printerctld --port /dev/pts/1` that was not
started by me was left running.

## Exit gate for ACCEPTED

1. RR-001 fixed, and the ASan `printerctld_tests` gate passes at least 5 times
   in an isolated /tmp.
2. RR-003 tests added, and the signal-intent mutation fails the suite.
3. RR-002 rule and tests added (or explicitly deferred by the operator with
   the residual documented).
4. RR-004 documentation corrected.
