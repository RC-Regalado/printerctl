---
title: Milestone 19 — PLP Client Library
tags: [printerctl, milestone, phase-2, plp, client]
status: implemented
updated: 2026-08-31
---

# Milestone 19 — PLP Client Library

M19 adds `printerctl_client`, a reusable client-side PLP/1 transport
library. It does not migrate CLI commands.

## Technical summary

- Added Unix domain socket connect/close.
- Added request construction with monotonic non-zero request IDs.
- Reused PLP/1 frame encoder and incremental parser.
- Added write-all and bounded response wait.
- Added response type/request ID correlation.
- Added wrappers for `PING`, `VERSION` and `STATUS`.
- Classified absent socket, connect/I/O failure, timeout, malformed PLP,
  unsupported version, request ID mismatch and remote status.

## Files modified

- `include/printerctl/client/plp_client.h`
- `src/client/plp_client.c`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/test_plp_client.c`
- `README.md`
- `docs/testing.md`
- `docs/phase-2-roadmap.md`
- `docs/design/m19-plp-client.md`

## Scope

No new PLP operations, CLI command migration, hardware command, queue,
jobs, history, TUI, Web/API, multi-printer work or daemon concurrency
redesign was added.

## Safety

M19 performs no serial access and no G-code execution. It cannot move,
home, heat, extrude, emergency-stop or resume a printer.

## Tests

`printerctl_client_tests` covers:

- absent socket;
- PING;
- VERSION;
- STATUS requested-fields payload;
- request ID mismatch;
- malformed frame;
- unsupported PLP version;
- connection reset before response;
- bounded wait timeout;
- remote error status;
- invalid STATUS requested-fields rejected before I/O.

## Next

M20 can use `printerctl_client` to migrate read-only printer queries
without letting CLI handlers construct PLP byte frames.
