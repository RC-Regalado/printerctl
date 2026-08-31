---
title: Milestone 16 — Single-Print Daemon Integration
milestone: 16
status: done
date: 2026-08-25
tags: [printerctl, milestone, daemon, print-engine, plp1]
---

# Milestone 16 — Single-Print Daemon Integration

Source of truth: [[Agents]], [[docs/architecture|architecture]],
[[docs/roadmap|roadmap]]. Design/as-built:
[[milestone-16-single-print-daemon-integration|docs/design/milestone-16-single-print-daemon-integration.md]].

## Technical summary

M16 connects PLP/1 `PRINT_START`, `PRINT_PAUSE`, `PRINT_RESUME` and
`PRINT_CANCEL` to one worker-owned print engine. The control context never
receives an engine pointer; it sends internal commands and updates its
published snapshot from worker events.

## Files modified

- `include/printerctl/daemon/ipc.h`, `src/daemon/ipc.c`
- `include/printerctl/daemon/worker.h`, `src/daemon/worker.c`
- `include/printerctl/daemon/runtime.h`, `src/daemon/runtime.c`
- `include/printerctl/core/print_engine.h`, `src/core/print_engine.c`
- `include/printerctl/protocol/plp1.h`
- `src/daemon/socket.c`
- `src/printerctld_main.c`
- `tests/test_daemon_print.c`
- `tests/test_daemon_socket.c`
- `CMakeLists.txt`, `tests/CMakeLists.txt`

## Tests added

- print completes through daemon runtime and updates STATUS fields;
- pause/resume through daemon runtime;
- second `PRINT_START` is rejected while active;
- cancel through daemon runtime;
- `PRINT_START` over Unix socket accepts, client disconnects, and the
  print continues to completion.

## Decisions taken

- Use the engine's existing between-command control poll instead of adding
  a second print execution thread.
- `PRINT_START` response means accepted/rejected, not final print result.
- No queue: a second active start returns error.
- `printerctld` now accepts serial config options for daemon-owned prints.

## Risks found

- M15 still serves one client at a time. A client that stays connected and
  idle can occupy the service until it disconnects.
- Active shutdown is cooperative via engine cancel; M17 should harden
  signal/power/fault behavior around active prints.

## Verification

- `cmake --build build-m15`
- `ctest --test-dir build-m15 --output-on-failure`
- ASan/UBSan full suite:
  `ctest --test-dir build-m16-asan --output-on-failure`
- ThreadSanitizer daemon subset:
  `ctest --test-dir build-m16-tsan --output-on-failure -R '(daemon_(runtime|socket|print)|printerctld)'`

Result: normal suite 17/17, ASan/UBSan 17/17, TSan daemon subset 4/4.
