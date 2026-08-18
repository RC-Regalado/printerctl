---
title: Milestone 10 — Low-Battery Cancellation
milestone: 10
status: done
date: 2026-07-20
tags: [printerctl, milestone, apcupsd, power, safety, record]
---

# Milestone 10 — Low-Battery Cancellation

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]]. Design
details: [[milestone-10-low-battery-cancellation|docs/design/milestone-10-low-battery-cancellation.md]].

## 1. Technical summary

A revised `roadmap.md` replaces milestones 10 and later; this milestone
implements the new Milestone 10, "Cancelación por batería baja," which
requires printerctl to actively cancel an active print on an APCUPSD
low-battery notification, superseding the original roadmap's
record-only Milestone 10.

`printerctl power low-battery` is the command an external notifier (the
new `scripts/apcupsd/printerctl-low-battery.sh`) runs. It never opens a
serial device: it resolves an active-print marker
(`$XDG_STATE_HOME/printerctl/active-print.pid`, published by
`run_print_command()` only while a print streams, and removed when it
ends) and, if the named process is still alive, delivers it `SIGPWR`.
That process's `print_control_poll()` checks for `SIGPWR` ahead of an
unresolved pause and ahead of the operator's own cancel flag, turning it
into `PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE` — handled by
`printerctl_printer_print_file()` through the exact same graceful-stop
path as an operator cancel (no further G-code sent, file and serial
closed) but tagged with its own status
(`PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE`), final state
(`PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE`) and exit code
(`PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE` = `10`), distinct from
an operator cancel everywhere they surface. The session record gains
`job_id`, `serial_device` and `printer_state` fields (populated for
every record, not only power-failure ones), and a power-failure
cancellation records `state=cancelled_power_failure` /
`cause=apcupsd_low_battery` rather than the plain `cancelled` an
operator cancel already used. `power low-battery` is idempotent: no
active print is not an error, and it is safe to run more than once.

Nothing about the existing safety guarantees changed: no automatic
resume (still no code path reads a record or the marker back), and a
power-failure cancellation sends nothing beyond "stop transmitting" — no
`M112`, no `M410`, no homing, no additional movement.

## 2. Files changed

- `include/printerctl/printer.h`, `src/printer/printer.c` (new
  `PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE` status,
  `PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE` final state,
  `PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE` control value, handled
  in `printerctl_printer_print_file()`)
- `include/printerctl/record.h`, `src/record/record.c` (new
  `PRINTERCTL_RECORD_ERR_NOT_FOUND` status; new `job_id`,
  `serial_device`, `printer_state` record fields; new
  `printerctl_record_write_active_pid()`/`_read_active_pid()`/
  `_remove_active_pid()` functions)
- `include/printerctl/cli.h` (new `PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE`
  = `10`; new `PRINTERCTL_COMMAND_POWER_LOW_BATTERY`)
- `src/cli/cli.c` (`power low-battery` parsing, dispatch and execution;
  SIGPWR handling in `run_print_command()`; active-print marker
  publish/remove around the stream; extended `write_session_record()`;
  updated help text, split to stay under the ISO C99 string-literal
  length limit)
- `scripts/apcupsd/printerctl-low-battery.sh` (new)
- `docs/scripts/printerctl-low-battery.md` (new)
- `tests/test_printer.c` (two new tests: power-failure cancel as its
  own distinct status/state, and priority over an unresolved pause)
- `tests/test_record.c` (new fields in `sample_record()`/assertions;
  three new active-print-marker tests)
- `tests/test_cli.c` (help-text assertions fixed for the "Milestone 1"
  substring collision with "Milestone 10"; five new `power low-battery`
  tests: subcommand grammar, dry-run, no active print, stale-marker
  cleanup, and a full end-to-end cancellation of a real streaming print)
- `architecture.md` (§6.8 apcupsd revised, §6.9 record extended, new
  §7.4 flow, new §8 rule 8, config-fields note revised)
- `Agents.md` (dated amendment to the APCUPSD Integration section)
- `README.md`
- `docs/design/milestone-10-low-battery-cancellation.md` (new)
- `docs/milestones/milestone-10.md` (new)

## 3. Acceptance validation

Mapped to the new roadmap's Milestone 10 acceptance criteria:

- **A low-battery notification cancels the active session:**
  `test_power_low_battery_cancels_active_print_end_to_end` (`test_cli.c`)
  starts a real streaming print against a slow mock printer, calls
  `printerctl power low-battery` from another (in-process) invocation
  sharing the same state directory, and asserts the print exits
  `PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE` promptly.
- **No new lines are sent after the cancellation is processed:** the
  same test asserts the summary never reports completion; the
  module-level `test_print_file_cancel_power_failure_stops_after_current_acknowledgement`
  (`test_printer.c`) additionally asserts nothing beyond the
  already-in-flight command reaches the mock's transmit buffer.
- **The job is marked `cancelled_power_failure`:** asserted in both the
  CLI summary text and the session record's `state=` field.
- **The last acknowledged line is persisted:** the session record's
  `last_acknowledged_line` field, asserted by content in the end-to-end
  test.
- **The process correctly releases file and serial:** unconditional in
  `run_print_command()`/`printerctl_printer_print_file()` regardless of
  which control value stopped the loop — the same single close/disconnect
  paths Milestone 9 already proved leak-free via `/proc/self/fd`
  (`test_failure_paths_release_all_descriptors`, unchanged and still
  passing, now also exercised by a third control value).
- **Power restoration does not start or resume jobs:** unchanged from
  Milestone 9 — there remains no code path anywhere that reads a record
  or the new active-print marker back to resume or start anything; the
  marker is removed once a print ends, so a `power low-battery` call
  after restoration finds nothing to act on
  (`test_power_low_battery_with_no_active_print`, and the end-of-test
  assertion in the end-to-end test that a second notification after the
  print already ended is a no-op).
- **The APCUPSD script is documented:**
  `docs/scripts/printerctl-low-battery.md` covers purpose, usage,
  arguments, environment variables, dependencies, side effects, exit
  codes, possible failures and a recovery procedure.
- **Tests demonstrate no auto-resume:** as above; additionally,
  `test_power_low_battery_cleans_up_a_stale_marker` proves a leftover
  marker from a process that no longer exists is treated as "nothing to
  cancel," never as a trigger for anything.

## 4. Verification

- GCC build (`build-gcc`, clean rebuild): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`; 8/8 test binaries pass.
- Clang build (`build-clang`, clean rebuild): warning-free; 8/8 test
  binaries pass.
- Codex validation on 2026-07-21: GCC/Clang builds and CTest were
  rerun successfully after review; `clang-tidy` exited 0 with the
  existing non-blocking strict-style warning set; `bash -n` passed for
  `scripts/apcupsd/printerctl-low-battery.sh`; `git diff --check` was
  clean; `power low-battery` with no active print and `--dry-run power
  low-battery` both exited 0 without opening serial. During review,
  Codex also aligned the lower-level `printerctl_printer_send()` G28
  rejection message with the current policy: homing is available only
  through the dedicated `home` command.
- `scripts/apcupsd/printerctl-low-battery.sh` exercised standalone
  against a stand-in `printerctl` binary covering all four documented
  exit codes (`0` success, `1` reported failure, `2` timeout, `3` binary
  not found); `bash -n` syntax check passes.
- **Not performed this session:** a real-hardware run of the SIGPWR
  path. No printer was attached to this machine while this milestone was
  implemented (unlike Milestones 8 and 9's hardware confirmations on
  `/dev/ttyUSB0`). The mechanism the hardware would exercise is not new,
  though: `print_control_poll()`'s SIGPWR branch reuses the exact same
  signal-handling and graceful-stop code paths Milestone 8 already
  validated on real hardware for SIGINT, and the record write on
  cancellation reuses the exact path Milestone 9 validated for a SIGINT
  cancel. The residual, genuinely untested surface is narrow: whether a
  real Marlin controller's serial behavior differs in any way once
  `SIGPWR` specifically (rather than `SIGINT`) is the signal received —
  it should not, since both reach the same `print_control_poll()` call
  with the same effect on the streaming loop, but this has not been
  physically confirmed. Recommend a real-hardware SIGPWR cancellation
  test (harmless G-code, as in prior milestones) before relying on this
  path operationally.

## 5. Risks found

- Stale-marker PID reuse is defended only by a liveness check
  (`kill(pid, 0)`), not a process-identity check — see the design note
  §6 for why an identity check was implemented and then dropped (it
  could not be exercised by this project's existing signal-test harness,
  and its actual safety value against the specific risky case was
  marginal). Milestone 17's daemon removes this class of risk entirely.
- The active-print marker is not `fsync`'d — it is a liveness indicator,
  not a durability record; losing it to a crash before sync only costs
  the ability to pre-emptively cancel that one print, not the eventual
  disconnect-triggered record Milestone 9 already provides.
- `job_id` (`job-<pid>`) is a placeholder, not a real job identifier;
  Milestone 13 replaces it.
- No real-hardware validation this session (see §4).

## 6. Decisions taken

- The originally-scheduled record-only Milestone 10 is superseded by the
  new roadmap's active-cancellation Milestone 10; reconciled with
  Agents.md's "informational only" APCUPSD text via a dated amendment.
  See design note §0.
- `SIGPWR` plus an active-print PID marker is the interim IPC mechanism
  ahead of Milestone 17's daemon and Unix socket; `printerctl power
  low-battery` is designed as the stable CLI surface Milestone 19 will
  keep, only swapping the transport underneath. See design note §1.
- A new, distinct final state/status/exit code for a power-failure
  cancellation, handled identically to an operator cancel structurally.
  See design note §2.
- The session record gains `job_id`, `serial_device` and `printer_state`
  fields, populated for every record. See design note §4.
- `power low-battery` is unconditionally idempotent and never an error
  when there is simply nothing to cancel. See design note §5.
- No config keys added in this milestone (`apcupsd_enabled`/
  `apcupsd_auto_resume` remain aspirational; see architecture.md §6.2).

## 7. Next steps

Continue with Milestone 11: core documentation completion (the roadmap's
new milestone 11, covering printing-lifecycle documentation, the
power-failure behavior write-up and per-script documentation
requirements) ahead of Phase II's daemon/IPC architecture.
