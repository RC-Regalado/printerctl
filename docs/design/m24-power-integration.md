# M24 Design — Power Integration Alignment

## Objective

Preserve audited low-battery cancellation with daemon-owned execution.

## Preferred path

```text
APCUPSD
  ↓
printerctl power low-battery
  ↓
daemon marker/PID
  ↓
SIGPWR printerctld
```

## Constraints

Do not replace SIGPWR merely for PLP symmetry.

Preserve:
- idempotence;
- no-active-print success;
- interrupted-session semantics;
- R6 TX barrier;
- no auto-resume;
- no physical cleanup commands.

## As-built notes

- The "daemon marker/PID" step of the preferred path was already true
  before this milestone started any work: `printerctl_record_write_
  active_pid()` is called from `printerctl_print_engine_connect()`
  (`src/core/print_engine.c`), which since Milestone 16/22 runs inside
  `printerctld`'s worker thread. The marker has held the daemon's own
  PID, not a CLI PID, since the M22 migration -- this milestone found
  no gap to close there and made no code change to it.
- `power low-battery`'s implementation (`run_power_low_battery_
  command()` in `src/cli/cli.c`) is unchanged since Milestone 10: it
  resolves `$XDG_STATE_HOME/printerctl/active-print.pid`, verifies the
  PID is alive (`kill(pid, 0)`), and sends `SIGPWR` directly -- no PLP/1
  round trip, per this design doc's explicit constraint.
- Real gap found and closed: no test exercised the actual `printerctl
  power low-battery` *command* against a live `printerctld`; every
  prior SIGPWR test signalled the daemon with a raw `kill()`. Added
  `test_cli_power_low_battery_signals_daemon_owned_print` in
  `tests/test_printerctld.c` (a new `start_printerctl_power_low_
  battery()` helper sets `$XDG_STATE_HOME` to match the daemon's, since
  this is the first CLI-spawn test helper needing that variable rather
  than `$XDG_CONFIG_HOME`).
- Real interaction confirmed and regression-tested: SIGPWR delivered
  while a print is genuinely `PAUSED` (M23) -- not merely blocked on an
  ack-wait, the only case any prior test covered. `on_control()` in
  `print_engine.c` already checks power-failure cancellation before
  pause state on every iteration of the pause-wait poll loop, so this
  was already correct; `test_sigpwr_while_paused_cancels_with_power_
  failure_semantics` makes it a permanent regression test, including
  asserting `failure_reason == SHUTTING_DOWN` survives through the
  pause the same way it does for an unpaused cancellation.
- All constraints above were re-confirmed rather than re-implemented:
  idempotence and no-active-print success (`tests/test_cli.c`'s
  existing `test_power_low_battery_with_no_active_print`/`_cleans_up_
  a_stale_marker`), interrupted-session semantics and R6/no-auto-resume
  (unchanged code paths, exercised by the tests above and the
  pre-existing `test_sigpwr_during_active_print_uses_daemon_marker_
  and_is_safe`).
- `src/cli/cli.c`'s doc-comment above `run_power_low_battery_command()`
  described the mechanism as transitional pending this milestone; it
  now states plainly that the marker/SIGPWR path is the deliberately
  preserved final design, not a stand-in for a future PLP operation.
