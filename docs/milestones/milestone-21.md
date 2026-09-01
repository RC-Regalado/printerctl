---
title: Milestone 21 — Explicit Physical Commands
tags: [printerctl, milestone, phase-2, daemon, plp1, safety]
status: implemented
date: 2026-09-01
---

# Milestone 21 — Explicit Physical Commands

## Technical summary

M21 migrates explicit operator physical commands from direct CLI serial
ownership to daemon-owned PLP/1 execution:

- `printerctl send COMMAND`
- `printerctl home [X|Y|Z]`
- `printerctl sd pause`
- `printerctl sd resume`
- `printerctl sd cancel`

The CLI now uses the M19 PLP client wrappers for those commands and does
not consume the legacy hardware-config view for them. `print FILE`
remains the only legacy direct-serial printer command until M22.

Bare `pause`, `resume` and `cancel` are reserved for M23 host-stream
control and are no longer SD aliases.

## Architecture

Implemented path:

```text
printerctl
  ↓
PLP client
  ↓
printerctld socket/control
  ↓
runtime command
  ↓
worker
  ↓
printer
  ↓
serial / Marlin
```

No lower layer gained daemon/socket dependencies. `protocol/` remains a
pure codec. `printerctld` control owns PLP dispatch and the worker owns
the printer session used for physical commands.

## Protocol additions

PLP/1 message IDs:

| ID | Name |
|---|---|
| `0x33` | `RAW_SEND` |
| `0x34` | `HOME` |
| `0x35` | `SD_PAUSE` |
| `0x36` | `SD_RESUME` |
| `0x37` | `SD_CANCEL` |

`RAW_SEND` request and response use existing PLP length-prefixed
strings. `HOME` request is one axis byte (`0` all, `1` X, `2` Y, `3`
Z). SD controls carry no payload.

`SAFETY_REJECTED` (`0x0205`) was added so daemon-side `RAW_SEND("G28")`
maps cleanly to CLI exit code `4`.

## Safety

- `RAW_SEND("G28")` is rejected by the daemon before serial write.
- The CLI keeps an early `send G28` rejection for UX, but it is not the
authoritative safety gate.
- `HOME` is the only path that intentionally sends `G28`/axis variants.
- No automatic homing, movement, heating, extrusion, emergency stop,
resume or recovery was introduced.
- The Phase 1 R6 cancellation transmission barrier was not modified.

## Files modified

- `include/printerctl/protocol/plp1.h`
- `src/protocol/plp1.c`
- `include/printerctl/client/plp_client.h`
- `src/client/plp_client.c`
- `include/printerctl/daemon/ipc.h`
- `src/daemon/ipc.c`
- `include/printerctl/daemon/runtime.h`
- `src/daemon/runtime.c`
- `src/daemon/socket.c`
- `src/daemon/worker.c`
- `include/printerctl/cli.h`
- `src/cli/cli.c`
- `tests/test_plp1_frame.c`
- `tests/test_plp1_payloads.c`
- `tests/test_plp_client.c`
- `tests/test_cli.c`
- `tests/test_printerctld.c`
- `README.md`
- `docs/design/protocol-v1.md`
- `docs/phase-2-roadmap.md`
- `docs/testing.md`
- `docs/milestones/milestone-21.md`

## Tests

Executed:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Result: 18/18 test binaries passed.

Added/updated coverage:

- byte-exact PLP vectors for `RAW_SEND` and `HOME`;
- payload tests for raw command/transcript and HOME axis validation;
- PLP client wrappers for `RAW_SEND`, `HOME` and SD controls;
- CLI contract changes for daemon-unavailable migrated commands and
  reserved host controls;
- `printerctld` process + PTY integration for `send`, `home X`,
  `sd pause`, and daemon-side `RAW_SEND("G28")` safety rejection.

## Risks

### Blocking

None found after the normal test suite passed.

### Non-blocking

- `RAW_SEND` transcript is bounded to 4096 bytes. This is enough for the
  one-command exchanges currently supported; streaming/evented output is
  out of scope.

### Future

- M22 will migrate `print FILE`, removing the final direct printer
  command from the CLI.
- M23 will activate host-stream `pause`/`resume`/`cancel` UX.
- M25 removes the transitional legacy hardware-config path completely.

## Decisions

- Kept `pause`/`resume`/`cancel` unavailable until M23 rather than
  routing them to SD behavior, because Phase 2 contracts require SD
  controls to live under the explicit `sd` namespace.
- Reused existing printer module safety and exchange behavior in the
  worker instead of duplicating Marlin semantics in daemon/socket code.

## Next steps

Request review of M21, especially safety and host-vs-SD command
separation, before advancing to M22.
