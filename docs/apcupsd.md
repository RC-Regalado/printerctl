---
title: APCUPSD Integration
tags: [printerctl, apcupsd, power, safety, reference]
---

# APCUPSD Integration

Source of truth: [[Agents]], [[Kernel/Architecture]]. Implemented in
[[milestone-10|docs/milestones/milestone-10.md]] /
[[docs/design/milestone-10-low-battery-cancellation]]; this document is
the operator-facing reference the roadmap's Milestone 11 requires
("qué evento activa la cancelación, cómo se comunica el script, qué
recursos se cierran, qué información se conserva, por qué no se
reanuda, cómo inspeccionar el trabajo cancelado").

## Why this exists

The host computer running `printerctl` is protected by a UPS; the
printer itself is not. That does not make a low-battery signal useless
to the printer session, though: the action printerctl takes does not
depend on the printer's own power at all — it is the host choosing to
stop sending G-code cleanly, while it still has enough runtime left to
do so, rather than waiting for an uncontrolled disconnect when the host
itself eventually loses power. See the APCUPSD Integration amendment in
`Agents.md` for the full reasoning.

## What event triggers the cancellation

APCUPSD does not call printerctl directly. The operator wires one of
its event hooks (see `/etc/apcupsd/apccontrol` on the host running
`apcupsd`) — commonly `doshutdown` (fires immediately before apcupsd
would initiate a system shutdown), and optionally `onbattery` for an
earlier warning while there is still runtime left — to run:

```bash
scripts/apcupsd/printerctl-low-battery.sh
```

That script's only job is to run `printerctl power low-battery` with a
bounded timeout and log the outcome; see
[[docs/scripts/printerctl-low-battery]] for its full documentation
(arguments, environment variables, exit codes, failure modes, recovery
procedure).

## How the script communicates with the process

There is no daemon yet (Phase II, Milestones 12+), so `printerctl power
low-battery` uses a small, deliberately temporary mechanism instead of a
proper IPC channel:

1. While a print streams, the printerctl process publishes its own PID
   to a live marker file: `$XDG_STATE_HOME/printerctl/active-print.pid`
   (default `~/.local/state/printerctl/active-print.pid`).
2. `printerctl power low-battery` reads that marker, checks the named
   process is still alive (`kill(pid, 0)`), and sends it `SIGPWR` — the
   POSIX/Linux signal historically reserved for exactly this purpose.
3. The streaming process's control loop checks for `SIGPWR` ahead of
   everything else (including an unresolved pause), turning it into a
   graceful cancellation.
4. The marker is removed when the stream ends, on every outcome — so a
   notification with no print running, or one that arrives after the
   print already stopped, correctly finds nothing to do.

This is why `printerctl power low-battery`, and therefore the script,
is always safe to run: with no print active it does nothing and exits
`0`; run twice, the second call finds nothing to cancel. Milestone 19
plans to replace steps 1-2 with the daemon's Unix socket without
changing the script or the `power low-battery` command itself.

## What resources are closed

Exactly the same graceful-stop path an operator's own `Ctrl+C` uses
(Milestone 8), because `SIGPWR` is handled by the same control check as
`SIGINT`/`SIGTERM`, just tagged with its own cause:

- No further G-code is transmitted — the check happens between
  acknowledged commands, never mid-exchange.
- The G-code file is closed.
- The serial port is disconnected.
- Nothing extra is ever sent to the printer to get there: no `M112`, no
  `M410`, no homing, no resume command, no additional movement (see
  [[docs/safety]] §6).

## What information is preserved

An interrupted-session record — the same mechanism Milestone 9 built
for any failed or cancelled print — is written to
`$XDG_STATE_HOME/printerctl/session-<timestamp>.record` (private, mode
0600, fsync'd, never overwritten). For a power-failure cancellation
specifically:

```ini
state=cancelled_power_failure
cause=apcupsd_low_battery
```

alongside the fields every record carries: `timestamp`, `job_id`
(`job-<pid>`, a placeholder ahead of Milestone 13's real job model),
`file` (the G-code path), `serial_device` (the configured port),
`commands_sent`, `commands_acknowledged`, `last_acknowledged_line`,
`bytes_consumed`, `file_size`, `progress_percent`, and `printer_state`
(always `unknown` — no live query is sent to populate it, for the same
reason nothing extra is sent to the printer at all: minimizing
interaction at the moment something has just interrupted the print).

The operator is told the record's path on stdout when the print's own
process exits; the structured log stream never contains the record path
or the G-code path (only a path-free `state=session_recorded`
transition) — see [[docs/logging]] for the full privacy rules.

## Why it never resumes automatically

Nothing in printerctl reads a session record back, for any reason —
this is provable by the absence of a read path in the codebase, not
just a policy statement (`record.h`'s module contract states this
explicitly). Power restoration therefore cannot trigger anything: there
is no code that would notice power came back, and no code that would do
anything with a record if it did. Restarting a job — from wherever the
record says it stopped — is always the operator's own, later, manual
decision. See [[docs/design/milestone-9-recovery-records]] §7 for the
end-to-end test that proves this (interrupting a print and rerunning
the same file streams the whole file again, from the first command).

## How to inspect a cancelled job

```bash
ls "${XDG_STATE_HOME:-$HOME/.local/state}/printerctl"/session-*.record
cat "${XDG_STATE_HOME:-$HOME/.local/state}/printerctl"/session-<timestamp>.record
```

Each record is plain `key=value` text (the same grammar as the
`--config` file) with a header comment restating that printerctl never
resumes automatically. The fields that matter for deciding how to
restart the job are `last_acknowledged_line` (where streaming truly
stopped) and `progress_percent`; `state`/`cause` distinguish a
power-failure cancellation from an operator cancel from an ordinary
failure, in case that history matters for the decision.
