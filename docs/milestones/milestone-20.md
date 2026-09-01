# Milestone 20 — Read-Only Printer Queries

Date: 2026-09-01

## Summary

M20 migrates `printerctl temp`, `printerctl position` and `printerctl info`
from direct CLI serial access to `printerctld` over PLP/1.

The daemon worker remains the exclusive owner of printer/serial interaction
for these queries. The CLI connects to the local PLP client, sends an explicit
query operation, decodes the structured response and renders the same
human-readable output shape.

## Files changed

- `include/printerctl/protocol/plp1.h`
- `src/protocol/plp1.c`
- `include/printerctl/client/plp_client.h`
- `src/client/plp_client.c`
- `include/printerctl/daemon/ipc.h`
- `src/daemon/ipc.c`
- `include/printerctl/daemon/runtime.h`
- `src/daemon/runtime.c`
- `src/daemon/worker.c`
- `src/daemon/socket.c`
- `src/cli/cli.c`
- `CMakeLists.txt`
- `tests/test_plp1_payloads.c`
- `tests/test_plp_client.c`
- `tests/test_cli.c`
- `tests/test_printerctld.c`
- `docs/design/protocol-v1.md`
- `docs/design/m20-read-only-queries.md`
- `docs/phase-2-roadmap.md`
- `docs/testing.md`
- `README.md`

## Decisions

- Added explicit PLP/1 message types:
  - `QUERY_TEMP` (`0x30`)
  - `QUERY_POSITION` (`0x31`)
  - `QUERY_INFO` (`0x32`)
- Kept `STATUS` snapshot-only; no Marlin I/O is triggered by `STATUS`.
- Used fixed-point signed milli-units for numeric query payloads to avoid native C floating-point wire layout.
- Kept `printerctl` socket selection at the existing default path; no new socket configuration was added in M20.
- Kept `timeout` semantics unchanged: config `timeout` remains the Marlin response timeout used by the daemon, not the CLI PLP wait timeout.
- Queries are rejected as `BUSY` while the worker is actively printing instead of opening a second serial session.

## Safety impact

- No homing, movement, heating, extrusion, emergency stop, quick stop, resume, recovery or cancellation behavior was added.
- The only Marlin commands used by M20 are existing read-only query commands:
  - `M105`
  - `M114`
  - `M115`
- The Phase 1 R6 cancellation transmission barrier is unchanged.

## Compatibility impact

- `printerctl temp`, `printerctl position` and `printerctl info` now require `printerctld`.
- These commands no longer consume `--port`, `--baud` or `--timeout` in the CLI.
- `--dry-run temp|position|info` remains local and succeeds without daemon or serial access.
- Transitional direct-serial CLI commands remain:
  - `print`
  - `send`
  - `home`
  - `pause`
  - `resume`
  - `cancel`

## Tests

Executed:

```bash
ctest --test-dir build --output-on-failure
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

Results:

- normal suite: 18/18 passed
- ASan/UBSan suite: 18/18 passed
- TSan suite: 18/18 passed

Coverage added:

- PLP query payload codec tests.
- PLP client query wrapper tests.
- CLI no-daemon behavior for migrated queries.
- End-to-end `printerctl` → `printerctld` → PTY mock → structured response → CLI output for `temp`, `position` and `info`.

## Risks

### Blocking

None found.

### Non-blocking

- CLI still uses the fixed default socket path. Socket path configuration is intentionally deferred until a milestone requires it.

### Future

- Remaining direct-serial CLI commands must continue shrinking through M21–M25.
- The transitional legacy hardware-config path is still scheduled for removal in M25.

## Next steps

- Proceed to M21: explicit physical command migration.
