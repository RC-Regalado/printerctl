# Milestone 29 — Macro Public Surface and PrusaSlicer Configuration Import

## Status

ACCEPTED BY OPERATOR — 2026-10-18. The independent review returned
APPROVE WITH REQUIRED FIXES (`docs/audit/m29-review.md`). Operator acceptance
does not close M29-R1 through M29-R5: their remediation has no recorded closure
evidence. Technical closure remains pending those fixes and verification.

## Summary

- PLP/1 `MACRO_LIST` (0x60) and `MACRO_RUN` (0x61), capability bit `MACROS`.
- Client library `printerctl_client_macro_list()` / `printerctl_client_macro_run()`.
- CLI `printerctl macro list`, `printerctl macro run ID`.
- CLI `printerctl config from-prusa FILE [INSET_MM [SAFE_Z_MM]]` and the pure
  `printerctl_prusa_import` library.

Design: `docs/design/m29-macro-public-surface.md`.

## Prerequisite

M28 remediation (`docs/audit/m28-remediation.md`) closed the M28 review
findings before M29 started; the independent re-review is recorded in
`docs/audit/m28-re-review.md`.

## Files

- `include/printerctl/protocol/plp1.h`, `src/protocol/plp1.c`
- `include/printerctl/client/plp_client.h`, `src/client/plp_client.c`
- `include/printerctl/daemon/runtime.h`, `src/daemon/runtime.c`
- `include/printerctl/daemon/worker.h`, `src/daemon/worker.c`
- `src/daemon/socket.c`
- `include/printerctl/cli.h`, `src/cli/cli.c`
- `include/printerctl/prusa_import.h`, `src/config/prusa_import.c`
- `tests/test_plp1_payloads.c`, `tests/test_plp1_frame.c`,
  `tests/test_daemon_socket.c`, `tests/test_printerctld.c`,
  `tests/test_cli.c`, `tests/test_prusa_import.c`

## Tests added

- Codec: MACRO_LIST/MACRO_RUN round trips, exact lengths, truncation,
  trailing bytes, count limit, empty id, reserved flags, buffer limits;
  frame-level payload rules for both types.
- Socket: MACRO_LIST executability with and without calibration config and
  without any serial port; MACRO_RUN unknown/non-executable/no-port/empty id;
  MACRO_RUN execution with RUNNING visible in STATUS, BUSY for a concurrent
  PRINT_START, COMPLETED after the last ACK.
- Process (`printerctld_tests`): CLI `macro list` and `macro run` against a
  configured daemon and PTY printer; SIGTERM during a macro ACK wait (clean
  exit, zero further TX); SIGPWR during a macro ACK wait (CANCELLED with
  SHUTTING_DOWN, daemon alive, zero further TX).
- CLI: grammar/usage errors, dry runs, daemon-unavailable paths,
  `config from-prusa` output, options and error exit codes.
- Import: real BIQU B1 profile, sliced G-code block, round trip through the
  config loader, rejections, overlong lines.

## Decisions

- Message types in the `0x60–0x7F` future range; additive only.
- MACRO_LIST is control-context only (no serial); MACRO_RUN keeps worker
  ownership.
- No MACRO_CANCEL; shutdown and power failure remain the stop paths.
- PrusaSlicer import is a stdout transform; the operator installs the file
  and passes it with `printerctld --config`.

## Risks / debt

- `power low-battery` does not reach a running macro (no active-print
  marker). FUTURE_IMPROVEMENT.
- STATUS does not expose execution kind or macro id; observers other than
  the starting CLI cannot tell a macro from a print. FUTURE_IMPROVEMENT.
