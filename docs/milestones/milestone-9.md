---
title: Milestone 9 — Fault Tolerance and Recovery Records
milestone: 9
status: done
date: 2026-07-10
tags: [printerctl, milestone, record, recovery, safety]
---

# Milestone 9 — Fault Tolerance and Recovery Records

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]]. Design
details: [[milestone-9-recovery-records|docs/design/milestone-9-recovery-records.md]].

## 1. Technical summary

Milestone 9 adds durable interrupted-session records through a new
dependency-free, write-only module, `printerctl_record`. Any `print`
that opened its G-code file but did not complete — failed (timeout,
serial failure, `Error:`, `Resend:`, mid-stream file error, G28 stop)
or cancelled by the operator — persists one record to
`$XDG_STATE_HOME/printerctl` (default
`$HOME/.local/state/printerctl`): a private (0700/0600), fsync'd,
never-overwritten `key=value` file named
`session-<UTC timestamp>.record` carrying version, timestamp, event,
state, cause, the G-code file path, commands sent/acknowledged, last
acknowledged line, byte progress and percent — plus a header comment
stating that printerctl never resumes automatically. Completed prints
and pre-open failures leave no record. The operator is told the record
path on stdout; the structured log gets only a path-free
`state=session_recorded` transition (Milestone 2 privacy rules). A
record that cannot be written warns and never changes the print's exit
code.

The Milestone 5 open question on reconnects is resolved: **no automatic
reconnect in the MVP**. Reopening a USB CDC device toggles DTR and
resets the controller, so a "reconnected" session would face a printer
with all state lost — and automatic resume is forbidden regardless. A
serial failure ends the session gracefully with a record; restarting is
the operator's explicit decision.

## 2. Files changed

- `include/printerctl/record.h`, `src/record/record.c` (new module)
- `src/cli/cli.c` (record writing on interrupted prints)
- `CMakeLists.txt`, `tests/CMakeLists.txt` (`printerctl_record` library
  and test target)
- `tests/test_record.c` (new: state-dir resolution, record content,
  exclusive creation, failure modes)
- `tests/test_cli.c` (record end-to-end tests, suite-wide disposable
  XDG_STATE_HOME, cancelled-record assertion in the SIGINT test)
- `tests/test_printer.c` (descriptor-leak test over failure paths)
- `architecture.md` (record module, reconnect resolution)
- `README.md`
- `docs/design/milestone-9-recovery-records.md`
- `docs/milestones/milestone-9.md`

## 3. Acceptance validation

- Interrupted sessions record file, line, progress, timestamp and
  state: asserted field-by-field in `test_record.c` (module) and
  `test_cli.c` (end-to-end for a timeout failure and a SIGINT cancel:
  `state=failed`/`cause=timeout` and `state=cancelled`/
  `cause=cancelled`, `last_acknowledged_line`, counters, timestamp,
  the no-resume policy text).
- Power or serial failure never triggers automatic resume: printerctl
  contains no code path that reads a record; an end-to-end test
  interrupts a print (record present), reruns the same file and asserts
  the whole file streams again from its first command; every record
  itself states the policy. (Power-event records arrive with
  Milestone 10 on this same module.)
- Resource cleanup is verified in tests where practical: a module test
  runs a timeout failure and a mid-stream disconnect through full
  connect/print/disconnect cycles and asserts via `/proc/self/fd` that
  the descriptor count ends exactly where it started.
- Additionally: records are never overwritten (same-millisecond writes
  yield distinct files), completed prints write no record, a failed
  record write is a warning that preserves the print's exit code, and
  the structured log never contains the state directory or record
  path.

## 4. Verification

- GCC build (`build-gcc`): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`; 8/8 test binaries pass.
- Clang build (`build-clang`): warning-free; 8/8 test binaries pass.
- Real hardware (`/dev/ttyUSB0`, Marlin 2.1.2.1, harmless M105/G4 file,
  no motion/heat/extrusion): a host print cancelled with SIGINT at 59%
  exited 9 and wrote
  `session-20260710T233936.159Z.record` with `state=cancelled`,
  `cause=cancelled`, `commands_sent=4`, `commands_acknowledged=4`,
  `last_acknowledged_line=4`, `progress_percent=59` and the correct
  byte counters; stdout named the record path, and the log carried only
  `state=session_recorded` with zero occurrences of the state directory
  or the G-code file name.

## 5. Risks found

- Records accumulate (one per interruption, <1 KB each); no pruning in
  the MVP.
- The record's `file=` value is the path as the operator passed it,
  which may be relative; combined with the record's own timestamp this
  is sufficient for a human but not guaranteed machine-resolvable
  later. Canonicalization was deliberately skipped (realpath allocates
  or needs PATH_MAX assumptions, and the record is operator-facing).
- fsync covers the record file, not its directory entry; a power cut in
  the same instant can lose the *name* on some filesystems. Considered
  acceptable for the MVP; Milestone 10 can add a directory fsync if
  Codex wants the stronger guarantee.
- A test relies on `/proc/self/fd` (Linux-only), consistent with the
  project's Linux-only target; it self-skips when /proc is absent.

## 6. Decisions taken

- Session records live in a new write-only `record` module that
  Milestone 10 will reuse for power events. See design note §1.
- Interrupted = opened but not completed; cancelled prints count,
  completed prints and pre-open failures do not. See design note §2.
- config-style `key=value` format in the XDG state directory, exclusive
  creation, fsync'd. See design note §3.
- The record may contain the G-code path; the structured log still may
  not. See design note §4.
- Record failures warn but never gate the print's exit code. See design
  note §5.
- No automatic reconnect in the MVP (DTR reset makes it unsafe and
  auto-resume is forbidden anyway); reconnect-attempt logging is
  therefore moot. See design note §6.

## 7. Next steps

Continue with Milestone 10: APCUPSD informational integration —
power-failure and power-restored event records (on the `record`
module), the `apcupsd_enabled`/`apcupsd_auto_resume` config keys
entering the model, and tests proving auto-resume stays hard-disabled.
