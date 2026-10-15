# UX-001 Controlled Verification

Date: 2026-10-13 (America/El_Salvador; trace timestamps use UTC).
Status: CONFIRMED / NOT FIXED.
Severity: P1 / BLOCKING lifecycle/correlation, superseding provisional P2 UX.

## Result

The CLI reports successful completion of the previous print after accepting
a new one. The new daemon-owned print continues independently after the CLI
has already exited. This is not merely missing intermediate percentages.

One controlled run and 10/10 independent repeats reproduced the defect.
The actual printer and existing daemon socket were never contacted.
Production source and binaries were not modified.

## Controlled Scenario

1. Start a real printerctld executable against a PTY mock printer, inside
   a separate PID namespace and /tmp namespace.
2. CLI A prints a one-command regular file. ACK it and verify completion 1/1.
3. CLI B starts a two-command regular file against the same daemon.
4. A preload shim blocks only the first fopen of B's file in `rb` mode:
   the real pre-scan entry point. Two pipes synchronize entry and release.
   It does not mock PLP, change snapshots or replace execution logic.
5. While B is held, assert CLI B exits 0 and reports A's completion counters.
   Verify that the PTY has received no B command.
6. Release pre-scan. Receive B's first command but hold its ACK; STATUS now
   reports RUNNING, 0 acknowledged, total 2. CLI B has already exited.
7. ACK the two commands and verify genuine B completion 2/2; shut down the
   mock daemon and clean up resources.

The pipe gate, not disk speed or probabilistic repetition, exposes the race.
Polling waitpid has a bounded 1-second budget. An overall 15-second alarm
and the isolated PID namespace bound failed reproductions.

## Captured Evidence

```text
Job A: completed with 1/1 acknowledged.
Job B observer output while pre-scan is held:
printerctl: print accepted by printerctld: '/tmp/printerctld-gcode-IYWO8s'
printerctl: Completed: 100% (1/1)
REPRODUCED: CLI B exited 0; B has transmitted zero commands.
Job B: later RUNNING 0/2, then genuinely COMPLETED 2/2.
```

In the first run, CLI B session_start was `2026-10-14T04:25:35.184Z`
and session_end was `2026-10-14T04:25:35.185Z`, matching the operator's
approximately 1 ms acceptance-to-false-completion pattern.

## Root Cause

- `src/daemon/worker.c:686`: successful command result is emitted before
  calling `printerctl_print_engine_run`.
- `src/core/print_engine.c:358`: source command pre-scan runs before the
  engine emits STARTED at line 364.
- `src/daemon/runtime.c:493`: waiting for command completion returns at the
  matching command result; this does not reset the execution snapshot.
- `src/daemon/socket.c:464`: STATUS drains available events, but no new
  STARTED event exists while pre-scan is held. Draining cannot remove a
  valid but obsolete terminal snapshot by itself.
- `src/cli/cli.c:871`: the observer accepts that terminal state without
  associating it with the newly accepted execution and returns success.

The operator's reported 10058/10058 is consistent with stale counters, but
this verification did not inspect that historical physical job. The
controlled two-job reproduction independently establishes the defect.

## Reproduce

From the repository root, after a normal build:

```bash
gcc -shared -fPIC docs/audit/repro/ux001-prescan-hold.c \
  -o /tmp/ux001-prescan-hold.so -ldl
gcc -I include \
  -DTEST_PRINTERCTLD_PATH="\"$PWD/build/printerctld\"" \
  -DTEST_PRINTERCTL_PATH="\"$PWD/build/printerctl\"" \
  docs/audit/repro/ux001-print.c build/libprinterctl_protocol_plp1.a \
  -o /tmp/ux001-print
bwrap --unshare-pid --die-with-parent --ro-bind / / --dev-bind /dev /dev \
  --proc /proc --tmpfs /tmp --ro-bind /tmp/ux001-print /tmp/review \
  --ro-bind /tmp/ux001-prescan-hold.so /tmp/hold.so /tmp/review /tmp/hold.so
```

A zero exit code means the reproducer observed the *bug*, not that the
contract passed. It is intentionally a diagnostic, not a passing regression
for the fixed behavior. The two sources reuse existing process/PTY helpers.
The preload environment is inherited only by the mock daemon, not the CLI.

## Correction Criteria

Before returning successful PRINT_START, ensure the control context has
consumed a worker-owned event establishing the new execution's nonterminal
state, counters and absent terminal error. Preserve engine ownership and
worker -> event_pipe -> snapshot updates. Alternatively, use an explicit
execution-correlation contract if a reviewed design requires it; new wire
features are not inherently necessary for this defect.

Do not add an arbitrary observer delay, reset the snapshot from the CLI,
generate fake progress, wait for the entire print before accepting START,
or require a visible RUNNING poll before accepting genuine quick completion.

Regression coverage must include starts after COMPLETED, FAILED and CANCELLED,
slow pre-scan, pending first ACK, fresh counters and eventual terminal result,
plus true rapid completion. Existing client-disconnect behavior must remain:
disconnecting the observer must not cancel daemon-owned execution.

## Limits

No implementation fix or hardware action was performed. Full CTest and
sanitizers were not rerun for this diagnostic-only addition; their earlier
M28 review results remain recorded separately. These tests establish the
startup/observation defect, not every possible cause of a missing UI update.
