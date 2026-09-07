---
title: Milestone 23 — Host Print Control UX
milestone: 23
status: implemented
date: 2026-09-03
tags: [printerctl, milestone, phase-2, daemon, plp1, cli]
---

# Milestone 23 — Host Print Control UX

Source of truth: `Agents.md`, `docs/architecture.md`, `docs/phase-2-roadmap.md`.
Design details: `docs/design/m23-host-control-ux.md`.

## 1. Technical summary

Before M23, bare `printerctl pause`/`resume`/`cancel` were parsed but
unconditionally rejected with "reserved for host print control ... not
available until M23" (`PRINTERCTL_EXIT_UNAVAILABLE`) — a placeholder
left by M21. The daemon-side plumbing for host-stream control
(`PLP1_MSG_PRINT_PAUSE`/`PRINT_RESUME`/`PRINT_CANCEL`, `socket.c`
dispatch, `runtime.c`'s `printerctl_daemon_runtime_send_print_*()`,
`worker.c`'s command handling and idempotent pause/resume via
`printerctl_print_engine_request_pause/resume()`) already existed and
was already covered by direct-socket PTY tests
(`test_second_print_start_maps_to_busy_over_socket`,
`test_print_pause_resume_preserves_progress` from PRINT-OBS-001). What
was missing was the CLI-facing half: a PLP client library wrapper and
the CLI command dispatch that turns `pause`/`resume`/`cancel` into
those PLP requests.

M23 adds:

- `printerctl_client_print_pause/resume/cancel()` in the M19 client
  library, alongside the existing `sd_pause/resume/cancel` wrappers
  (both now share one `control_no_payload()` helper).
- CLI dispatch: bare `pause`/`resume`/`cancel` now connect to
  `printerctld` and issue the corresponding PLP request, exactly like
  `send`/`home`/`sd ...` already do. `--dry-run pause/resume/cancel`
  reports the intended action without a daemon or serial connection
  (unlike `sd pause/resume/cancel`'s dry-run, which names a Marlin
  command — a host-stream pause/resume/cancel never has one).
- A real, previously-latent bug fix in the CLI's PRINT-OBS-001 progress
  observer: `run_print_progress_observer()` mapped every terminal
  execution state other than `FAILED` to `PRINTERCTL_EXIT_OK`, so an
  operator (or another client) cancelling a print an attached
  `printerctl print FILE` was observing was invisible in its exit
  code. It now returns the pre-existing (previously unused)
  `PRINTERCTL_EXIT_PRINT_CANCELLED` / `PRINTERCTL_EXIT_PRINT_CANCELLED_
  POWER_FAILURE` codes, distinguished via the same `failure_reason ==
  SHUTTING_DOWN` convention `worker.c` already encodes for
  `PRINTERCTL_PRINT_EVENT_CANCELLED`. This was unreachable before M23
  because nothing could yet issue a host `PRINT_CANCEL` from the CLI.

No PLP/1 wire changes: `PRINT_PAUSE`/`PRINT_RESUME`/`PRINT_CANCEL`
message types, and the daemon-side control path, already existed.

## 2. Architecture

Implemented path (new CLI half only; the daemon half predates M23):

```text
printerctl pause|resume|cancel
  ↓
printerctl_client_print_pause/resume/cancel()  (client library, M19)
  ↓
PLP1_MSG_PRINT_PAUSE/RESUME/CANCEL
  ↓
printerctld socket dispatch (pre-existing)
  ↓
runtime command → worker → print_engine (pre-existing)
```

No new mutable execution-state machine was introduced anywhere: the
daemon-owned `print_engine`/worker/runtime pipeline established by
M16/PRINT-OBS-001 remains the single source of truth. The CLI never
retains pause/resume/cancel state between invocations — each command
is a one-shot connect/request/response/exit, matching `send`/`home`/`sd
...`, and any client may issue it (see `test_print_pause_resume_
repeated_and_second_client` and the CLI-level tests below).

## 3. Files modified

- `include/printerctl/client/plp_client.h` — `printerctl_client_print_
  pause/resume/cancel()` declarations.
- `src/client/plp_client.c` — `sd_control()` generalized to `control_
  no_payload()` (adds a `label` parameter for its error message) and
  reused by the three new host-control wrappers.
- `src/cli/cli.c` — `is_reserved_host_control_command()` renamed to
  `is_host_control_command()` (no longer "reserved"); new `run_host_
  control_command()`; dry-run branch for bare pause/resume/cancel;
  `run_print_progress_observer()`'s CANCELLED exit-code fix; help text
  and usage doc-comment updates.
- `include/printerctl/cli.h` — doc-comment update superseding the
  stale Milestone 7/8/10 SIGINT/SIGUSR1/SIGUSR2 narrative with the
  actual M22/M23 daemon-owned behavior.
- `tests/test_plp_client.c` — `PRINT_PAUSE` client-wrapper coverage.
- `tests/test_cli.c` — replaced the "reserved until M23" unit test with
  daemon-unavailable and `--dry-run` coverage for bare pause/resume/
  cancel.
- `tests/test_printerctld.c` — `test_print_pause_resume_repeated_and_
  second_client` (repeated pause/repeated resume, each from its own
  connection); `test_cli_pause_resume_use_daemon_print_control`;
  `test_cli_cancel_uses_daemon_print_control`; `test_cli_print_sigint_
  kills_observer_not_the_print`.
- `docs/phase-2-roadmap.md`, `docs/design/m23-host-control-ux.md`,
  `docs/milestones/milestone-23.md` — this milestone's documentation.

## 4. Verification

```bash
cmake -S . -B build-verify-gcc -DCMAKE_C_COMPILER=gcc -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-verify-gcc
ctest --test-dir build-verify-gcc --output-on-failure

cmake -S . -B build-verify-clang -DCMAKE_C_COMPILER=clang
cmake --build build-verify-clang
ctest --test-dir build-verify-clang --output-on-failure
```

Result: both compilers warning-free; 18/18 test binaries pass on each.

ASan+UBSan (`-fsanitize=address,undefined -g -O1`, `build-asan`):
18/18 pass, no sanitizer reports.

## 5. Risks found

### Blocking

None found after the full suite passed.

### Non-blocking

- `printerctld` serves one client connection at a time
  (`printerctl_daemon_socket_serve_one_interruptible()` calls `accept()`
  once and serves that connection until it disconnects before accepting
  the next). This is pre-existing, not introduced by M23, but it means
  a long-lived `printerctl print FILE` observer connection blocks any
  *other* new connection from being serviced until it disconnects.
  `test_cli_print_sigint_kills_observer_not_the_print` was written
  around this (it never opens a second, overlapping connection while
  the observer is attached) after first hitting it directly: an
  earlier draft of that test opened a concurrent raw STATUS connection
  while the observer stayed attached, which starved that connection
  for the full Marlin ack timeout before the observer itself timed out,
  disconnected, and freed the daemon's single accept slot. Documented
  here since it will matter again for any future "second client
  observes a live print" test design; M24/M25/M26 do not currently
  require true connection concurrency.

### Future

- M24 (Power Integration Alignment) and M25 (Remove Legacy Serial
  Ownership) are unaffected by this milestone's changes.

## 6. Decisions taken

- Reused the existing `sd_control()` helper (generalized, not
  duplicated) for the new host-control wrappers, since both are
  structurally identical no-payload request/response pairs — this
  matches the design doc's "no context-sensitive dispatch" principle
  at the code level too (SD vs. host is which message type is sent,
  never inferred from state).
- Fixed the `run_print_progress_observer()` CANCELLED exit-code gap
  (see §1) as in-scope for M23 rather than deferring it, since M23 is
  what makes a CLI-issued `cancel` reachable at all, and the existing
  `PRINTERCTL_EXIT_PRINT_CANCELLED`/`_POWER_FAILURE` codes already
  existed for exactly this purpose but were unreachable dead code.
- `--dry-run pause/resume/cancel` reports a host-specific message
  ("would ask printerctld to ... the active host print") rather than
  reusing `sd pause/resume/cancel`'s "would send 'M25'"-style wording,
  because a host-stream pause/resume/cancel has no corresponding
  Marlin command to name.
- Verified "Ctrl+C observer" (docs/phase-2-testing.md's M23 test list)
  by confirming the *absence* of any signal handler is itself correct
  behavior (client signals are a UI concern per the design doc, not an
  execution-ownership one) rather than adding new signal-handling code
  to the CLI.

## 7. Next steps

Request review of M23, in particular the exit-code fix and the
single-connection-server risk note, before advancing to M24 (Power
Integration Alignment).
