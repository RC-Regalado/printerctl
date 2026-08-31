---
title: Milestone 17 — Lifecycle, Fault & Legacy Compatibility Validation
milestone: 17
status: done
date: 2026-08-25
tags: [printerctl, milestone, daemon, lifecycle, phase-1]
---

# Milestone 17 — Lifecycle, Fault & Legacy Compatibility Validation

Source of truth: [[Agents]], [[docs/architecture|architecture]],
[[docs/roadmap|roadmap]]. Design/as-built:
[[milestone-17-lifecycle-fault-compatibility|docs/design/milestone-17-lifecycle-fault-compatibility.md]].

## Technical summary

M17 closes Phase 1 hardening around lifecycle, power, serial-failure and
legacy compatibility. The daemon path now has cooperative active-print
shutdown, SIGPWR-driven power-failure cancellation, interruptible socket
client handling and explicit regression coverage for printer faults.

## Files modified

- `include/printerctl/daemon/ipc.h`, `src/daemon/ipc.c`
- `include/printerctl/daemon/runtime.h`, `src/daemon/runtime.c`
- `include/printerctl/daemon/socket.h`, `src/daemon/socket.c`
- `src/daemon/worker.c`
- `src/printerctld_main.c`
- `tests/test_daemon_print.c`
- `tests/test_printerctld.c`
- `tests/test_daemon_ipc.c`
- `docs/apcupsd.md`
- `docs/roadmap.md`, `docs/architecture.md`, `README.md`,
  `docs/testing.md`

## Tests added/expanded

- power-failure cancel remains distinguishable from operator cancel;
- shutdown during active print cancels through the engine lifecycle;
- Marlin `Error:` maps to failed execution;
- `Resend:` maps to failed execution;
- timeout maps to failed execution;
- legacy serial open is rejected while daemon owns serial;
- `SIGTERM` interrupts an idle connected client;
- `SIGPWR` with no active print is safe and the daemon remains responsive.

## Decisions taken

- `SIGPWR` is mapped to a daemon-internal power-failure cancel command
  instead of letting APCUPSD or a script touch serial.
- Socket handling remains sequential; M17 only makes a connected idle
  client interruptible by daemon lifecycle signals.
- No physical cleanup command is emitted on shutdown or power failure.

## Risks found

- Single-client transport is still a throughput/UX limit for future UI
  automation. It is not a Phase 1 blocker because sequential clients and
  lifecycle interrupts are now validated.

## Verification

- `git diff --check`
- `cmake --build build-m15`
- `ctest --test-dir build-m15 --output-on-failure`
- ASan/UBSan full suite:
  `ctest --test-dir build-m17-asan --output-on-failure`
- ThreadSanitizer daemon subset:
  `ctest --test-dir build-m17-tsan --output-on-failure -R '(daemon_(runtime|socket|print)|printerctld)'`

Result: normal suite 17/17, ASan/UBSan 17/17, TSan daemon subset 4/4.

## Phase 1 status

Phase 1 is complete pending review approval.
