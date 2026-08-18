---
title: Milestone 9 Design - Fault Tolerance and Recovery Records
milestone: 9
status: done
date: 2026-07-10
tags: [printerctl, milestone, design, record, recovery, safety]
---

# Milestone 9 Design — Fault Tolerance and Recovery Records

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]].

This note records the concrete design decisions behind the Milestone 9
implementation, ahead of the milestone report in
[[milestone-9|docs/milestones/milestone-9.md]].

## 1. A new `record` module, write-only

Interrupted-session metadata is persisted by a new dependency-free
module, `printerctl_record` (`include/printerctl/record.h`,
`src/record/record.c`). It is deliberately **write-only**: nothing in
printerctl reads a record back, because there is nothing safe to do
with one automatically — automatic resume is forbidden (Agents.md,
architecture.md §8 rule 6), and every record says so in its own header
comment. Records are data *for the operator*.

A dedicated module rather than CLI-local code because Milestone 10's
APCUPSD power events must persist exactly the same shape of data
("timestamp, file, last acknowledged line, progress and printer state"
— architecture.md §6.8); the module is the shared consumer both
milestones were waiting for.

## 2. What counts as an interrupted session

A `print` that **opened its file but did not complete** — failed
(timeout, serial failure/disconnect, `Error:`, `Resend:`, mid-stream
file error, G28 stop) or cancelled by the operator. Completed prints
leave no record (nothing to recover), and pre-open failures leave no
record either (nothing was ever streamed; the file itself is the
problem and the error message already says so). The trigger condition
in the CLI is summary state ≠ completed with a nonzero file size —
exactly "the reader opened, the print did not finish".

Cancelled counts as interrupted on purpose: the operator stopping a
print at 59% wants "where did I stop" at least as often as a crash
does, and the record's `state=cancelled` / `cause=cancelled` keeps the
two distinguishable.

## 3. Record format and location

Root-level `key=value` with full-line `#` comments — the same grammar
as the Milestone 1 config file, so the project has one tiny
human-readable format, not two. Fields: `version` (1), `timestamp`
(RFC3339 UTC, milliseconds), `event=print_interrupted`, `state`
(`failed`/`cancelled`), `cause` (the status name: `timeout`, `serial`,
`printer_error`, `resend`, `gcode`, `unsafe_command`, `cancelled`),
`file` (the G-code path as the operator passed it), and the summary
counters (`commands_sent`, `commands_acknowledged`,
`last_acknowledged_line`, `bytes_consumed`, `file_size`,
`progress_percent`).

Location: `$XDG_STATE_HOME/printerctl` when XDG_STATE_HOME is a
non-empty absolute path, else `$HOME/.local/state/printerctl` — the
XDG Base Directory spec's state-data location, which exists for exactly
this ("actions history, current state of the application"). One file
per interruption, `session-<UTC compact timestamp>.record`, created
with `O_EXCL` (a same-millisecond collision retries with a numeric
suffix) so a record is **never overwritten**. Directory 0700, file
0600: records are private operator data.

Durability: the file is fsync'd before close. A recovery record exists
to survive exactly the failures it describes (Milestone 10 will write
them on power loss); declaring it written while it only sits in page
cache would miss the point.

## 4. Privacy: the record may name the file; the log still must not

The record intentionally contains the G-code file path — a recovery
record without the file name is useless, and the file is private
(0600, user's own state directory). The Milestone 2 log privacy rules
are unchanged: the structured log gets a path-free
`state=session_recorded` transition (or a generic warning when the
write fails); the record path and the G-code path appear only in
human-readable output. Tests assert the log never contains the state
directory.

## 5. Record failures warn, never gate

The print's exit code always reflects the print. A record that cannot
be written (no resolvable state dir, uncreatable directory, I/O error)
produces a human-readable warning naming the reason plus a generic
WARNING log record, and the failed/cancelled exit code stands. A
partially written record file is removed rather than left as a corrupt
artifact.

## 6. Reconnect policy (deferred from Milestone 5): none in the MVP

No automatic reconnect, no retry limits to define. Rationale: reopening
a USB CDC serial device toggles DTR, which resets the controller —
Marlin reboots and loses all session state (position, temperatures,
steppers). A "reconnected" session would be talking to a printer in an
unknown, cold state while pretending continuity; and since automatic
resume is forbidden anyway, there is nothing a reconnect could safely
continue. A serial failure or disconnect therefore ends the session
gracefully with an interrupted-session record, and restarting is the
operator's explicit decision. (This also keeps "reconnect attempt
logging" moot — the roadmap required it only "if reconnect is
implemented".) A future daemon (backlog item 4) may revisit this for
idle-session reconnects; never for mid-print ones.

## 7. No automatic resume, proven

Beyond the absence of any code path that reads records: an end-to-end
test interrupts a print (record written), then reruns the same file and
asserts the whole file streams again from its first command — the
record's presence changes nothing. This is the acceptance criterion
"power or serial failure never triggers automatic resume" made
executable at the level this milestone can reach (serial failure);
Milestone 10 adds the power-event side on the same foundation.

## 8. Resource cleanup, verified

Failure paths already closed the reader and left the port to the
caller's disconnect (Milestone 7's single graceful-stop path); what was
missing was proof. A module test runs two full failure scenarios —
acknowledgement timeout and a mid-stream disconnect — through connect,
failed print and disconnect, and asserts via `/proc/self/fd` that the
process ends with exactly the descriptor count it started with.

## 9. Deferred

- Reading/rendering records (`printerctl status`-style inspection) —
  possible Milestone 11+ nicety; the format is deliberately
  human-readable in the meantime.
- Power-event records and the APCUPSD hooks: Milestone 10, on this
  module.
- Record rotation/pruning: records are tiny (<1 KB); not an MVP
  concern.
