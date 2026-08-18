# `scripts/apcupsd/printerctl-low-battery.sh`

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]]. Implemented
as part of [[milestone-10|docs/milestones/milestone-10.md]].

## Purpose

Notifies a running `printerctl print` that APCUPSD detected a
low-battery / imminent-shutdown condition, so the print stops
transmitting gracefully — closing the G-code file and the serial port,
and writing an interrupted-session record — before the host itself may
lose power. It never touches the serial port and never writes any of
printerctl's own files itself: it only asks the `printerctl` binary to
act, through `printerctl power low-battery`.

Power restoration never triggers automatic resume, with or without this
script: printerctl has no code path that resumes a print, only records
that say how far one got (see [[docs/design/milestone-10-low-battery-cancellation.md]]).

## Usage

Configure APCUPSD to run this script from one of its event hooks (see
`/etc/apcupsd/apccontrol` on the host running `apcupsd`) — commonly
`doshutdown` (fires right before apcupsd would initiate a shutdown), and
optionally `onbattery` for an earlier warning while there is still time
left on the battery:

```bash
scripts/apcupsd/printerctl-low-battery.sh
```

It takes no command-line arguments.

## Arguments

None. All configuration is through the environment variables below.

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `PRINTERCTL_LOW_BATTERY_TIMEOUT` | `10` | Seconds to wait for `printerctl power low-battery` before giving up (passed to `timeout`). |
| `PRINTERCTL_BIN` | `printerctl` | The binary to run; override if `printerctl` is not on `PATH` under its default name. |

## Dependencies

- `bash` (the shebang interpreter).
- `timeout` (coreutils), to bound how long the script waits.
- `logger` (util-linux/bsdutils), for syslog reporting; a missing
  `logger` degrades to a `stderr` message rather than failing the
  script (see Possible failures below).
- The `printerctl` binary itself, reachable via `PATH` or
  `PRINTERCTL_BIN`.

## Side effects

- Runs `printerctl power low-battery`, which may deliver a `SIGPWR`
  signal to a printerctl process elsewhere on the host (see
  `printerctl --help` and [[docs/design/milestone-10-low-battery-cancellation.md]]).
  The script itself opens no device and writes no printerctl file.
- Writes one log line to syslog (tag `printerctl-low-battery`), or to
  its own stderr if syslog is unavailable.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | `printerctl power low-battery` ran and reported success — whether or not a print happened to be active (that command is intentionally a safe no-op with nothing to cancel). |
| `1` | `printerctl power low-battery` ran but reported a failure (a nonzero, non-timeout exit code). |
| `2` | `printerctl power low-battery` did not finish within `PRINTERCTL_LOW_BATTERY_TIMEOUT` seconds. |
| `3` | The `printerctl` binary could not be found on `PATH` (or at `PRINTERCTL_BIN`). |

## Possible failures

- **`printerctl` not installed or not on `PATH`**: exit `3`, logged
  before any other action is attempted.
- **`printerctl power low-battery` hangs** (e.g. the state directory is
  on an unresponsive filesystem): bounded by `timeout`; exit `2`.
- **No active print at the time of the signal**: not a failure —
  `printerctl power low-battery` itself exits `0` and reports "no
  active print"; this script exits `0` too.
- **`logger` is unavailable**: the script still reports the same
  message to its own stderr instead, so a failure is never silent, only
  differently routed.
- **Run more than once for the same event** (APCUPSD retries, or more
  than one hook fires): safe. `printerctl power low-battery` is
  idempotent — a second run after the print already stopped finds
  nothing to cancel and exits `0`.

## Recovery procedure

This script never leaves printerctl in a state that needs "recovery" —
worst case, it fails to deliver the notification (exit `1`/`2`/`3`) and
whatever print was running keeps streaming until it completes or fails
on its own. If it was meant to stop a print ahead of a real power loss
and did not:

1. Check the exit code and the syslog line (`journalctl -t
   printerctl-low-battery` or equivalent) for which of the four cases
   above occurred.
2. If `printerctl` was not found (exit `3`), fix `PATH` or set
   `PRINTERCTL_BIN` in the environment APCUPSD runs the script under.
3. If it timed out (exit `2`), raise `PRINTERCTL_LOW_BATTERY_TIMEOUT` or
   investigate why the state directory (`$XDG_STATE_HOME/printerctl`,
   default `~/.local/state/printerctl`) is slow to reach.
4. If the print is still running and power is genuinely about to fail,
   the operator can always fall back to sending the signal directly:
   `kill -PWR $(pgrep -f 'printerctl.*print')`, or simply cancel the
   print in its own terminal (`Ctrl+C`).
5. After any cancellation, a session record under
   `$XDG_STATE_HOME/printerctl` names the last acknowledged line and
   progress — printerctl never resumes automatically, so restarting the
   job (from that point on, manually) is always the operator's decision.
