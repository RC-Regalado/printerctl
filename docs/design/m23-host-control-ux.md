# M23 Design — Host Print Control UX

## Objective

Make top-level pause/resume/cancel operate on daemon-owned host streams.

## Target

```text
printerctl pause
printerctl resume
printerctl cancel
```

SD becomes:

```text
printerctl sd pause
printerctl sd resume
printerctl sd cancel
```

## No context-sensitive dispatch

Never infer SD vs host from current state.

Intent must be explicit in syntax.

## Repeated operations

Add tests for:
- repeated pause while paused;
- repeated resume while resumed.

## Signals

Client signals are UI concerns, not execution ownership.

Do not restore PID-based host execution control through the client.

## As-built notes

- `printerctl_client_print_pause/resume/cancel()` (client library) and
  bare `pause`/`resume`/`cancel` CLI dispatch (`run_host_control_
  command()` in `src/cli/cli.c`) reuse PLP1's existing `PRINT_PAUSE`/
  `PRINT_RESUME`/`PRINT_CANCEL` message types and the daemon-side
  handling already built for them — no new PLP/1 wire messages.
- The client library's `sd_control()` helper was generalized to
  `control_no_payload()` (adds a `label` parameter) and is shared by
  both the SD and host-stream control wrappers, per "no
  context-sensitive dispatch": the two families are never routed
  through shared *decision* logic, only shared no-payload
  request/response *plumbing*.
- No signal handler was added to `printerctl print FILE`'s attached
  observer. An unhandled SIGINT simply kills the observer process; the
  daemon-owned print is unaffected (verified by
  `test_cli_print_sigint_kills_observer_not_the_print` in
  `tests/test_printerctld.c`), which is exactly what "client signals
  are UI concerns" above requires without writing any new code for it.
- Fixed a related, previously-unreachable bug while wiring `cancel` up
  to the CLI: `run_print_progress_observer()` mapped every terminal
  state except `FAILED` to exit code 0, so a print cancelled by another
  client while being observed reported success. It now returns
  `PRINTERCTL_EXIT_PRINT_CANCELLED` / `_POWER_FAILURE` (both pre-existing,
  previously-dead exit codes), matching the operator-vs-SIGPWR
  distinction `worker.c` already encodes via `failure_reason`. See
  `docs/milestones/milestone-23.md` for full details and verification.
- Repeated pause (while paused) and repeated resume (while running) are
  safe no-ops end-to-end: `print_engine`'s own `request_pause`/
  `request_resume` were already idempotent (M8), and `worker.c`
  unconditionally reports `SUCCESS` for `PRINT_PAUSE`/`PRINT_RESUME`
  whenever a print is active, regardless of current pause state. Tested
  in `test_print_pause_resume_repeated_and_second_client`, with each
  repeated request arriving on its own fresh connection.
