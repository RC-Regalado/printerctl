# Milestone 22 — Host Print CLI Migration

Status: IMPLEMENTED

## Summary

`printerctl print FILE` now starts daemon-owned host printing through
PLP/1 `PRINT_START`. The CLI no longer opens serial, owns
`print_engine`, or falls back to direct serial execution for non-dry-run
prints.

`--dry-run print FILE` remains local and daemon-independent.

## Files changed

- `include/printerctl/client/plp_client.h`
- `src/client/plp_client.c`
- `src/cli/cli.c`
- `CMakeLists.txt`
- `tests/test_cli.c`
- `tests/test_plp_client.c`
- `tests/test_printerctld.c`
- `README.md`
- `docs/phase-2-roadmap.md`
- `docs/design/m22-host-print-migration.md`

## Decisions

- The M22 CLI exits after `PRINT_START` acceptance. It does not poll
  `STATUS`, subscribe to events, or expose host pause/resume/cancel.
- Final print outcome rendering is deferred to later CLI UX milestones.
- Migrated `print` uses the client-owned config view; hardware config is
  ignored by the CLI and consumed only by `printerctld`.
- `printerctl_cli` no longer links against `printerctl_printer` or
  `printerctl_core`.

## Safety impact

- No new automatic homing, movement, heating, extrusion, M112/M410,
  resume, recovery, or cleanup G-code was added.
- `send G28` remains blocked before IPC as UX and by daemon policy.
- Print execution remains inside daemon worker ownership through the
  existing `print_engine` path.

## Compatibility impact

- Non-dry-run `printerctl print FILE` now requires `printerctld`.
- The CLI reports acceptance, not final streamed-print summary.
- `--dry-run print FILE` behavior is unchanged.

## Tests

- CLI tests cover daemon-unavailable `print`, ignored legacy hardware
  options, privacy logging, and unchanged dry-run behavior.
- PLP client tests cover the new `PRINT_START` wrapper.
- `printerctld` integration tests cover `printerctl print FILE` over the
  default socket and verify that execution continues after client exit.

## Risks

- Attached observation and final summary compatibility are not part of
  M22 and remain future work.
- `power low-battery` still uses the transitional marker/signal path
  until M24.

## Next steps

- M23: migrate host `pause`, `resume`, and `cancel` CLI UX to daemon
  print-control operations.
