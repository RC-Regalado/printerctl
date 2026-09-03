# HOME-RPC-001 / DAEMON-LIFE-001 Remediation

Status: IMPLEMENTED

## Summary

`printerctl home` remains a synchronous typed physical command:

```text
CLI HOME request
→ printerctld worker
→ G28 transmitted once
→ daemon waits for Marlin terminal response
→ PLP HOME response
→ CLI reports completion or daemon-authoritative error
```

The client no longer applies the short 5000 ms PLP response deadline to
HOME. `timeout` remains daemon-owned Marlin response timeout; it is not
reinterpreted as an IPC timeout.

Disconnected clients are treated as client lifecycle events. If a HOME
response cannot be delivered because the client closed or reset the
socket, `printerctld` discards that response, closes the client
connection, and remains available.

## Files changed

- `include/printerctl/client/plp_client.h`
- `src/client/plp_client.c`
- `src/cli/cli.c`
- `src/daemon/socket.c`
- `tests/test_printerctld.c`
- `docs/milestones/home-rpc-remediation.md`

## Decisions

- `timeout_ms == 0` in the PLP client means no client-side response
  deadline.
- Only HOME uses the no-deadline policy because Phase 2 HOME completion
  is intentionally synchronous and bounded by daemon/Marlin terminal
  response semantics.
- Existing short RPCs keep their bounded timeout behavior.
- `EPIPE` and `ECONNRESET` while returning a response are not daemon
  service failures when runtime/worker remain healthy.

## Safety impact

- Explicit HOME intent still produces exactly one intended `G28`.
- Client disconnect after `G28` does not generate `M112`, `M410`,
  cleanup movement, resume, recovery, or any additional physical command.
- Phase 1 R6 cancellation transmission barrier is unchanged.

## Tests

PTY/mock tests cover:

- quick HOME completion;
- HOME duration greater than 5 seconds;
- daemon-authoritative Marlin timeout;
- terminal `Error:` propagation;
- client close/reset after `G28` transmission;
- daemon availability after undeliverable response.

The existing PLP client short-timeout regression test remains in
`printerctl_client_tests`.
