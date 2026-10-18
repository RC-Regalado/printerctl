# Milestone 28 — Built-in Macro Execution

## Status

ACCEPTED — operator approval of the remediation on 2026-10-18. Findings of `docs/audit/m28-review.md` closed
in `docs/audit/m28-remediation.md`; the re-review (`docs/audit/m28-re-review.md`)
returned APPROVE WITH REQUIRED FIXES, addressed in the remediation addendum.
The 2026-10-15 completion validation in `docs/audit/m28-remediation.md`
records passing regression gates, including five consecutive sanitizer daemon
runs and the controlled UX-001 process regression. The operator approval is
recorded in that report; it is not a new independent technical review and
does not accept the separate M29 work.

## Summary

M28 adds the first worker-owned physical execution path for built-in typed MOVE
macros. It keeps macro definitions daemon-owned and does not expose PLP macro
operations or CLI macro UX.

## Files

- `include/printerctl/macro.h`
- `src/macro/macro.c`
- `include/printerctl/config.h`
- `src/config/config.c`
- `include/printerctl/daemon/ipc.h`
- `src/daemon/ipc.c`
- `include/printerctl/daemon/runtime.h`
- `src/daemon/runtime.c`
- `src/daemon/worker.c`
- `tests/test_macro.c`
- `tests/test_config.c`
- `tests/test_daemon_print.c`
- `docs/design/m28-macro-execution.md`

## Decisions

- `maintenance-z-lift` is the first geometry-independent executable macro, but
  still requires explicit `calibration.safe_z` and `calibration.z_feedrate`.
- Calibration macros use optional daemon-owned geometry config and fail cleanly
  with zero serial writes when geometry is missing or invalid.
- Successful macro completion leaves firmware in `G90`.
- Failure before the final `G90` leaves modal state unknown; no cleanup command
  is generated.
- `park` remains non-executable registry metadata.

## Risks

- Absolute XY calibration movement assumes the operator has established valid
  coordinates. M28 does not add homed-state tracking.
- Macro control was internal only in M28; M29 adds the protocol and CLI surfaces.

## Next steps

- M29: public macro surface (see `docs/milestones/milestone-29.md`).
