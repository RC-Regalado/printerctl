---
title: Milestone 24 — Power Integration Alignment
milestone: 24
status: implemented
date: 2026-09-03
tags: [printerctl, milestone, phase-2, daemon, safety, power]
---

# Milestone 24 — Power Integration Alignment

Source of truth: `Agents.md`, `docs/architecture.md`, `docs/phase-2-roadmap.md`.
Design details: `docs/design/m24-power-integration.md`.

## 1. Technical summary

M24's stated goal was to confirm the audited Milestone 10 SIGPWR path
("APCUPSD → `printerctl power low-battery` → daemon marker/PID →
`SIGPWR printerctld`") still holds now that print execution is
daemon-owned (M16/M22) and host-stream pause/resume exist (M23) — and,
per the design doc's explicit constraint, to *not* replace it with a
PLP operation merely for architectural symmetry.

Investigation found the "daemon marker/PID" step was already true with
zero code changes needed: `print_engine.c` writes the active-print
marker with its own PID from inside `printerctl_print_engine_connect()`,
and since M16/M22 that code runs inside `printerctld`'s worker thread,
not a CLI process. `power low-battery` (`src/cli/cli.c`, unchanged
since M10) still resolves that marker and sends `SIGPWR` directly, with
no PLP/1 round trip — exactly per the constraint.

What M24 actually needed was closing two real coverage gaps this
investigation surfaced:

1. No test ran the real `printerctl power low-battery` *command*
   against a live `printerctld` — every existing SIGPWR test (from
   M10/M16/M17) delivered the signal with a raw `kill()`, bypassing the
   CLI's own marker-resolution and liveness-check logic entirely.
2. No test covered SIGPWR arriving while a print is genuinely `PAUSED`
   — a state M23 made reachable for the first time. Prior tests only
   covered SIGPWR while blocked on an in-flight command's ack.

Both gaps are now closed with regression tests; both scenarios were
already handled correctly by existing code (see Decisions below).

## 2. Files changed

- `tests/test_printerctld.c` — new `start_printerctl_power_low_
  battery()` helper (sets `$XDG_STATE_HOME`, the first CLI-spawn helper
  in this file to need it instead of `$XDG_CONFIG_HOME`);
  `test_cli_power_low_battery_signals_daemon_owned_print`;
  `test_sigpwr_while_paused_cancels_with_power_failure_semantics`.
- `src/cli/cli.c` — doc-comment above `run_power_low_battery_command()`
  corrected from "transitional... until M24" to describe the marker/
  SIGPWR path as the final, deliberately preserved design.
- `docs/phase-2-roadmap.md`, `docs/design/m24-power-integration.md`,
  `docs/milestones/milestone-24.md` — this milestone's documentation.

No production code outside that one comment changed: this milestone is
verification-and-documentation, not new functionality.

## 3. Verification

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

## 4. Risks found

### Blocking

None.

### Non-blocking

- The first draft of `test_cli_power_low_battery_signals_daemon_owned_
  print` merged the CLI subprocess's stdout and stderr into one pipe
  with a 256-byte buffer; the structured `session_start`/
  `config_summary`/`state_transition` log lines (written to stderr)
  filled the buffer before the human-readable "signalled the active
  print" line (written to stdout) could be read, producing a false
  failure. Fixed by capturing stdout only, matching every other
  `start_printerctl_*` helper in this file. Worth remembering for any
  future helper that captures a `printerctl` subprocess's output: this
  binary's default (non-`--verbose`) logging is already substantial.

### Future

- M25 (Remove Legacy Serial Ownership) and M26 (Final Validation) are
  unaffected by this milestone.

## 5. Decisions taken

- Did not migrate `power low-battery` to a PLP/1 operation. The design
  doc's constraint ("do not replace SIGPWR merely for PLP symmetry")
  and roadmap decision #12 ("Existing SIGPWR/daemon-marker integration
  remains valid unless a milestone explicitly proves a safer
  replacement") both directly rule it out, and nothing found during
  investigation constitutes such proof — the mechanism already worked
  correctly end-to-end.
- Verified rather than reimplemented `on_control()`'s power-failure-
  before-pause priority (`src/core/print_engine.c`, pre-existing
  comment: "an unresolved power-failure cancellation wins over cancel,
  which wins over an unresolved pause"): the new paused-SIGPWR test
  confirms this behavior rather than adding a redundant code path.
- Used the real `printerctl power low-battery` CLI command (not a
  fabricated marker file) for the new end-to-end test, so it also
  exercises the CLI's own config resolution and privacy/logging paths
  against a real daemon, not just the signal-delivery mechanics.

## 6. Next steps

Request review of M24 before advancing to M25 (Remove Legacy Serial
Ownership).
