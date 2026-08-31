# Codex Review Task Template — Phase 2

Review independently from implementation.

## Scope
- stayed inside milestone?
- future-phase features introduced?

## Architecture
- ownership remains CLI -> client -> PLP/socket -> daemon -> worker -> core -> printer -> serial?
- inverse dependency?
- migrated command still has direct serial access?

## Safety
- new automatic G28/G0/G1/M104/M109/M140/M190/M112/M410?
- auto-resume?
- raw-send G28 bypass?
- cancellation barrier regression?

## Protocol
- stable message IDs;
- bounded payloads;
- incremental framing;
- golden vectors;
- explicit errors.

## Testing
- build;
- unit;
- protocol;
- PTY/mock;
- process-level;
- ASan;
- UBSan;
- TSan where applicable;
- boundary scan;
- physical-action scan.

## Verdict

Use exactly one:
- `MILESTONE APPROVED`
- `MILESTONE APPROVED WITH REQUIRED FIXES`
- `MILESTONE REJECTED`

Separate blockers from early-next-milestone debt.
