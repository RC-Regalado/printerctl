---
title: Milestone 10 Design - Low-Battery Cancellation
milestone: 10
status: done
date: 2026-07-20
tags: [printerctl, milestone, design, apcupsd, power, safety]
---

# Milestone 10 Design — Low-Battery Cancellation

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]].

This note records the concrete design decisions behind the Milestone 10
implementation, ahead of the milestone report in
[[milestone-10|docs/milestones/milestone-10.md]].

## 0. Roadmap replacement, and why this milestone acts rather than only records

The roadmap that shipped through Milestone 9 defined Milestone 10 as
"APCUPSD Informational Integration": record power events, never act on
them. A revised `roadmap.md` supersedes milestones 10 and later; the new
Milestone 10 is "Cancelación por batería baja" and requires printerctl
to actively stop an active print on an APCUPSD low-battery signal, not
merely record it.

This also updates `Agents.md`'s and `architecture.md`'s older statement
that "the printer is NOT connected to the UPS, therefore APCUPSD
integration is informational only." That statement is still true as
written — the printer's own power is unaffected by the host's UPS — but
the action this milestone adds does not depend on the printer's power at
all: it only stops the *host* from sending more G-code, which is
squarely the host's own decision to make before it, not the printer,
loses power. Framed that way, a low-battery signal arriving while the
host still has runtime left is exactly the moment to stop cleanly and
record state, rather than waiting for an uncontrolled disconnect (the
serial-failure path Milestone 9 already handles, but without any advance
warning). The rule that has not changed, and is reinforced rather than
weakened, is that this is the *only* action taken: no automatic resume,
no automatic homing, no `M112`, no `M410`, no extra movement -- see §6.

## 1. Mechanism: a distinct signal plus a PID marker, not a new IPC layer

The roadmap explicitly allows a Unix signal or "a temporary internal
interface" for this phase, reserving the daemon's Unix socket for
Milestone 17+. There is no daemon yet, and each `print` is its own
foreground process, so the interim design is:

- **`SIGPWR`** is the signal a printerctl print session reacts to as a
  power-failure cancellation, distinct from the operator's own
  `SIGINT`/`SIGTERM`. `SIGPWR` is the POSIX/Linux signal historically
  reserved for exactly this purpose ("power failure"), so it reads as
  what it is rather than overloading an existing operator signal.
- **An active-print PID marker** (`record.h`'s
  `printerctl_record_write_active_pid()`/`_read_active_pid()`/
  `_remove_active_pid()`), at
  `$XDG_STATE_HOME/printerctl/active-print.pid`: `run_print_command()`
  writes its own PID there right after connecting, and removes it when
  the stream ends, on every outcome. This is the "temporary internal
  interface" — an external notifier has no other way to find the right
  process in a no-daemon architecture.
- **`printerctl power low-battery`** is the one command an external
  notifier runs: it resolves the marker, verifies the named process is
  still alive (`kill(pid, 0)`), and sends it `SIGPWR`. This is also the
  seam for Milestone 19: that milestone's roadmap text describes the
  exact same CLI surface (`printerctl power low-battery` /
  `POWER.LOW_BATTERY`) migrating to the Unix socket transport
  underneath — the APCUPSD script does not need to change at all, only
  what `power low-battery` does internally.

This marker is **not** an interrupted-session record. It exists only
while a print streams, is silently overwritten (never rejected as
already-existing) if a stale one is found, and carries nothing but a
PID. See `record.h` for why it lives in the same module as the
Milestone 9 records regardless (shared state-directory plumbing,
dependency-free).

## 2. A new, distinct final state and status

`printerctl_printer_status_t` gains
`PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE` and
`printerctl_print_final_state_t` gains
`PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE`, both handled exactly like
their Milestone 8 operator-cancel counterparts (not errors, same
graceful-stop code path in `printerctl_printer_print_file()`) but kept
distinguishable everywhere they surface: the CLI summary line, the exit
code (`PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE` = `10`, additive —
no existing exit code changes meaning), and the session record's
`state`/`cause` fields (`cancelled_power_failure` /
`apcupsd_low_battery`, versus plain `cancelled` for an operator cancel).
`printerctl_print_control_t` gains a matching
`PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE`, checked first in
`print_control_poll()` — ahead of an unresolved pause, and ahead of an
ordinary cancel flag — so a power event is never left waiting behind a
pause the operator forgot to resume.

## 3. Cancellation semantics, mapped to the existing graceful-stop path

The roadmap's eight cancellation requirements map directly onto
Milestone 7/8 machinery already in place, not new code:

1. No further G-code sent — the existing `on_control` check between
   acknowledged commands, never mid-exchange.
2. No indefinite wait for a new `ok` — the existing per-response
   timeout already bounds this; a power event does not need to
   interrupt an in-flight exchange, only stop the *next* one.
3. Last acknowledged line preserved — the existing summary fields.
4. Marked as cancelled for a power failure — §2 above.
5. G-code file closed — the existing single close path in
   `printerctl_printer_print_file()`, unconditional on every exit.
6. Cancellation recorded — the existing Milestone 9 record write path,
   with the state/cause distinction from §2.
7. Serial resources closed correctly — the existing
   `printerctl_printer_disconnect()` call in `run_print_command()`,
   unconditional on every outcome.
8. No resume on power return — unchanged from Milestone 9: nothing
   reads a record back, ever.

Nothing extra is ever sent to the printer: no `M112`, no `M410`, no
homing, no resume command, no additional movement. A power-failure
cancellation is not a substitute physical safety sequence — it is
strictly "stop transmitting," matching Agents.md's safety rules and the
existing pause/resume/cancel design (`docs/design/milestone-8-pause-resume-cancel.md`).

## 4. Session record additions

The Milestone 9 record schema gains three fields, populated for every
record (not only power-failure ones), because they are useful
regardless of cause and adding them once, uniformly, is simpler than a
conditional schema:

- `job_id` — `job-<pid>` of the streaming process. There is no job
  queue yet (Phase II's Milestone 13 introduces one); this is a
  placeholder scoped to the current one-print-at-a-time architecture,
  not a forward-compatible ID format. It satisfies the roadmap's
  minimum record fields without inventing a queue early.
- `serial_device` — the configured port. Private operator data, like
  `file` already was (see the Milestone 9 design note §4): it may appear
  in the record, never in the structured log stream.
- `printer_state` — always `unknown`. No live query (e.g. another
  `M105`) is sent to populate this after a fault or a power-event
  cancellation: doing so would itself be exactly the kind of additional
  interaction with the printer the safety policy avoids at the moment
  something has just gone wrong. The field exists so the schema has
  everywhere the roadmap asks for, honestly populated as "not queried"
  rather than guessed.

## 5. `power low-battery`: idempotent, safe with nothing to cancel

`printerctl power low-battery` never opens a serial device. It always
exits `0` when it successfully determined there was nothing to do
(no marker, or a stale marker whose process is gone) — an APCUPSD script
must be able to run this unconditionally, whether or not a print happens
to be active, and safely more than once (the later Milestone 19
acceptance criteria require exactly this; there is no reason for the
pre-daemon version to behave differently). It exits non-zero only when
it could not even determine whether a print was active (state directory
unusable, or the marker exists but could not be signalled) — a real
operational problem, not "nothing to cancel."

`--dry-run power low-battery` reports what it would check without
touching the marker or signalling anything, for symmetry with every
other command's dry-run behavior, even though this command never opens
a serial device regardless.

## 6. Risks and accepted limitations

- **Stale-marker PID reuse.** If printerctl crashes without removing its
  marker and the OS later reuses that PID for an unrelated process,
  `power low-battery`'s only defense is a liveness check
  (`kill(pid, 0)`), not a process-identity check. An early implementation
  additionally verified `/proc/<pid>/comm == "printerctl"`, but this
  could not be exercised by the existing test harness, which drives
  every signal-based scenario by forking the test binary itself and
  calling `printerctl_cli_run()` in-process (never `exec`ing a real
  `printerctl` binary) — so `/proc/<pid>/comm` for the "print process"
  under test is the test binary's own name, not `printerctl`. Weighed
  against that, the identity check's actual safety value is marginal: it
  only helps when a reused PID belongs to a process that happens not to
  be named `printerctl`; if the OS reused the PID for a *new* printerctl
  invocation (the more likely accidental-collision case if the operator
  restarts a print quickly), the check would not catch it either. The
  check was dropped rather than kept untested. Milestone 17's daemon
  removes this whole class of risk: a single long-lived, well-known
  process replaces PID-file indirection entirely.
- **No directory `fsync` for the marker.** Unlike a session record, the
  marker is a liveness indicator, not a durability record — if it is
  lost to a crash before `fsync`, the operator simply loses the ability
  to pre-emptively cancel that one print via `power low-battery`; the
  print itself, and its eventual disconnect-triggered record (Milestone
  9), are unaffected.
- **`job_id` is not a real job identifier.** See §4; it will be replaced
  by the real Job Queue Model in Milestone 13.

## 7. Deferred

- Real job IDs and a job queue: Milestone 13.
- `POWER.LOW_BATTERY` over the Unix socket, replacing the PID-signal
  transport underneath `power low-battery`: Milestone 19. The CLI
  surface and the APCUPSD script are designed not to need to change.
- Power-restored notification/recording as its own explicit event
  (distinct from "the record already states no auto-resume"): not
  required by this milestone's acceptance criteria; revisit if Codex
  wants an explicit `power.restored`-style record ahead of Milestone 20's
  event subscriptions.
