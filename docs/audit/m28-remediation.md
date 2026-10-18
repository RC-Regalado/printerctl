# M28 Remediation Record

Date: 2026-10-14 (America/El_Salvador)
Scope: closure of the findings in [M28 Technical Review](m28-review.md).
The review report itself is preserved unchanged as decision history.

## Summary

| Finding | Correction (code) | Regression (tests) | Status |
|---|---|---|---|
| M28-001 numeric formatting | `format_move_command()` (`src/daemon/worker.c`) rejects truncated output, non-zero values that round to `0.000` and feedrates below `0.001`; all step commands are formatted before the serial port is opened | `test_macro_unrepresentable_values_have_zero_serial_side_effects` (±1e200 coordinates, feedrate 0.0004): INVALID_ARGUMENT, IDLE snapshot, zero TX; clean under ASan/UBSan | FIXED |
| M28-002 power/shutdown in ACK wait | `printerctl_printer_execute_controlled()` uses `printerctl_serial_read_line_interruptible()`; the macro control poll consumes signal intents, PRINT_CANCEL_POWER_FAILURE and SHUTDOWN, checked before TX and during ACK wait | `test_macro_power_failure_during_ack_wait_stops_future_steps` (held ACK, late ACK, no further TX, CANCELLED/SHUTTING_DOWN); `test_macro_shutdown_during_busy_chatter_completes` (continuous `busy:` every 20 ms, shutdown finishes < 1.5 s, CANCELLED) | FIXED |
| M28-003 inch mode | Worker sends `G21` and requires its terminal response before any coordinate-mode or motion command | Existing macro sequences now expect `G21` first; `test_macro_establishes_millimeters_after_inch_mode` (RAW_SEND G20 precondition); `test_macro_unit_mode_failure_prevents_motion` (`Error:` on G21 → FAILED, zero further TX) | FIXED |
| UX-001 premature completion | `run_print()` publishes RUNNING with reset counters through `event_pipe` before the PRINT_START command result, so `wait_command()` applies it before the client sees success | `test_sequential_print_start_publishes_new_execution` (previous COMPLETED and previous FAILED, then new start: RUNNING, zero counters, no stale failure_reason, terminal counters belong to the new job) | FIXED |
| M28-004 intermittent sanitizer timeout | Initially misdiagnosed here as a `TMPDIR` problem (that only explains local socket-test hangs). Real cause found by the re-review (RR-001): `test_cli_print_sigint_kills_observer_not_the_print` asserted COMPLETED right after the 4th ACK, racing the worker's terminal event; the failing test then left its daemon alive, so ctest reported a timeout | See Addendum | FIXED (addendum) |

Mutation checks (temporary source edits, reverted): disabling ACK-wait
interruption, removing the pre-acceptance RUNNING event, or removing the
truncation check each makes `printerctl_daemon_print_tests` fail.

## Unit-mode postcondition

A successful macro leaves the firmware in `G21` + `G90`. A macro that fails
or is cancelled after `G21` leaves millimetres established and the
coordinate mode unknown; no cleanup command is sent.

## Verification

| Check | Result |
|---|---|
| Normal build + CTest (`TMPDIR=/tmp`) | 21/21 PASS |
| ASan/UBSan build + full CTest | 21/21 PASS |
| TSan: macro, runtime, daemon print, socket suites | 4/4 PASS (after making the test-only chatter flag atomic) |
| Git object integrity (`git fsck --full`) | PASS (missing blobs restored with exact hashes) |

Process-level SIGTERM/SIGPWR delivered to a `printerctld` process during a
macro remained NOT TESTED in M28 because macros had no external entry point.
They are covered only once M29's MACRO_RUN exists (uncommitted M29 work:
`test_sigterm_during_macro_is_safe`, `test_sigpwr_during_macro_cancels_with_power_semantics`).

## Addendum — re-review fixes (`docs/audit/m28-re-review.md`)

Note: M29 implementation started before this re-review returned, by the
operator's decision; M28 acceptance still depends on the items below.

| Finding | Correction | Evidence |
|---|---|---|
| RR-001 / M28-004 | New `query_print_status_until_state()` waits for the terminal state itself; the SIGINT-observer test uses it | `tests/test_printerctld.c` |
| RR-002 | `format_move_command()` enforces wire representability: \|coordinate\| ≤ 9999.999 mm, feedrate in [0.001, 99999.999] mm/min, non-zero \|value\| ≥ 0.001, `-0.0` sent as `0.000`, line ≤ 95 bytes (Marlin MAX_CMD_SIZE 96) | `test_macro_unrepresentable_values_have_zero_serial_side_effects` (1e100, −10000, feedrate 100000, 0.0004), `test_macro_boundary_values_are_sent_exactly` |
| RR-003 | Signal-flag path tested directly; busy-chatter shutdown now asserts zero further TX | `test_macro_signal_power_flag_during_ack_wait` (mutation disabling the flag check makes the suite hang/fail); `test_macro_shutdown_during_busy_chatter_completes` |
| RR-004 | This record corrected; `docs/design/m28-macro-execution.md` documents G21, unit-mode postcondition, ACK-wait cancellation, power/shutdown handling and formatting limits | this file, design doc |

## Completion Validation - 2026-10-15

The resumed working tree already contained the M28 production fixes, the
RR-001 terminal-state wait, RR-002 wire bounds, and uncommitted M29 work.
That work was preserved. No new M29 capability or importer fix was implemented
as part of this completion; M29's separate review remains applicable.

Additional closure work:

- `printerctl_print_observer_tests` now reproduces the exact UX-001 startup
  window against real CLI/daemon processes and PTY. It holds the new file's
  pre-scan with a test-only preload shim, after a previous COMPLETED, FAILED
  or CANCELLED execution. It requires the observer to remain alive before
  any new command, then through the first held ACK, and finally to report
  only the new job's 2/2 completion. It also checks visible initial progress.
- `test_macro_signal_flag_during_ack_wait(bool)` covers both cancellation
  and power-failure signal flags without an accompanying internal command.
  Each case checks its terminal reason and no TX after a late ACK.
- The existing working-tree process-level macro SIGTERM/SIGPWR tests were
  executed as part of `printerctld_tests`; they use the already implemented
  M29 entry point. This closes the process-test gap on this working tree,
  not retroactively on the historical M28-only commit.
- The new observer harness cleans up its child processes even on assertion
  failure. It uses an unused socket only; all executions below isolated /tmp.

### Results

| Check | Result |
|---|---|
| GCC normal build and full CTest | 22/22 PASS, 34.27 s |
| GCC ASan/UBSan build and full CTest | 22/22 PASS, 35.48 s; leak/error checks enabled |
| TSan relevant suites | 5/5 PASS: macro, runtime, daemon socket, daemon print, new observer; 6.37 s |
| Controlled observer regression, repeat until-fail:10 | 10/10 PASS, 30.86 s; three previous terminal states per run |
| ASan/UBSan printerctld_tests, repeat until-fail:5 | 5/5 PASS, 118.13 s |
| Macro signal flags after adding normal-cancel case | Focused normal, ASan/UBSan and TSan daemon-print suites: 3/3 PASS |
| Architecture boundary hook | PASS |
| git diff --check | PASS |
| git fsck --full | No missing/corrupt objects; dangling blobs reported, left intact |
| Physical-action hook | Exit 3: manual review requested, not an automatic PASS |

Normal and sanitizer full runs precede the final test-only extension adding
the normal-cancel flag case; the affected suite was then rebuilt and rerun
in all three configurations. No production change followed those full runs.

Commands use this isolation prefix, replacing the build directory as needed:

```bash
bwrap --unshare-pid --die-with-parent --ro-bind / / --dev-bind /dev /dev \
  --proc /proc --tmpfs /tmp --bind "$PWD" "$PWD" env TMPDIR=/tmp \
  ctest --test-dir build --output-on-failure --timeout 60
```

Sanitizer runs add `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1` to env. TSan uses `TSAN_OPTIONS=halt_on_error=1`.
The repetition gates select `^printerctld_tests$` or
`^printerctl_print_observer_tests$` and use `--repeat until-fail:5` / `:10`.

### Physical-Action Review

The physical-action hook scans committed HEAD~1...HEAD, so its tokens include
the earlier operator-authorized G28 file policy, typed macro G1 generation,
documentation prohibiting emergency cleanup, and PTY test vectors. Its
request for manual review was not suppressed. Inspection of production TX
paths confirms that macro commands still come from the validated registry,
G21 requires an ACK before motion, and cancellation never appends movement,
heating, extrusion, homing, M112 or M410. The new harness uses synthetic G1
commands only against its PTY; it does not contact the real daemon socket.

### Disposition

M28-001, M28-002, M28-003 and UX-001 are implemented with regressions.
RR-001 through RR-004 have code/test/doc evidence and the requested five-run
sanitizer stability gate passes. This is completion evidence, not a new
independent acceptance verdict: M28 remains REMEDIATED pending formal review.
The original review and re-review findings are retained as dated history.
No real-hardware macro test, full TSan suite or Valgrind run was performed.
No daemon restart or deployment was performed. The static-ASan harness limit
and future observer execution-identity issue (RR-006) remain documented debt.

## Operator Acceptance - 2026-10-18

The operator approved the remediation: "Remediacion aprobada". M28 is now
ACCEPTED on the basis of the completion evidence above. This decision
supersedes the pending-acceptance disposition of 2026-10-15; the original
review, re-review and validation results remain unchanged as dated history.

This records operator acceptance, not a new independent technical review.
M29 remains subject to its separate review and required fixes. No additional
tests, hardware operations, deployment or daemon restart were performed for
this documentation-only acceptance update. Previously documented limitations
and debt remain applicable.

