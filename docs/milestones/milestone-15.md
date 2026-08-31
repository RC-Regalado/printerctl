---
title: Milestone 15 — Unix Socket Transport
milestone: 15
status: done
date: 2026-08-25
tags: [printerctl, milestone, daemon, socket, plp1]
---

# Milestone 15 — Unix Socket Transport

Source of truth: [[Agents]], [[docs/architecture|architecture]],
[[docs/roadmap|roadmap]], [[protocol-v1|docs/design/protocol-v1.md]].
Design/as-built: [[milestone-15-unix-socket-transport|docs/design/milestone-15-unix-socket-transport.md]].

## Technical summary

M15 binds the M14 PLP/1 codec to a real Unix Domain Socket transport owned
by the M13 control context. The implementation accepts local
`AF_UNIX`/`SOCK_STREAM` clients, parses arbitrary byte streams with the
incremental PLP/1 parser, and responds to the hardware-independent
commands required for this milestone: `PING`, `VERSION` and idle
`STATUS`. The `printerctld` executable wraps that transport in a minimal
process with `--socket`, `--help` and `--version`.

The transport is deliberately sequential, not multi-client concurrent.
That satisfies M15's required sequential-client behavior while avoiding a
second thread/dispatch model before M16 introduces real print ownership.

## Files modified

- `include/printerctl/daemon/socket.h` — new public M15 transport API.
- `src/daemon/socket.c` — socket lifecycle, stale/live path detection,
  PLP/1 dispatch, response writes, client lifecycle handling.
- `src/printerctld_main.c` — minimal daemon process wrapper.
- `tests/test_daemon_socket.c` — real `AF_UNIX` integration tests.
- `tests/test_printerctld.c` — process smoke test for `printerctld`.
- `CMakeLists.txt` — new `printerctl_daemon_socket` library.
- `tests/CMakeLists.txt` — new `printerctl_daemon_socket_tests` and
  `printerctld_tests` targets.
- `docs/design/milestone-15-unix-socket-transport.md` — design/as-built.
- `docs/milestones/milestone-15.md` — this milestone report.
- `docs/roadmap.md`, `docs/architecture.md`, `README.md` — status updates.

No lower layer was modified for M15: `core`, `printer`, `serial`,
`marlin`, `gcode` and the CLI remain unchanged.

## Tests added

`tests/test_daemon_socket.c` validates:

- fragmented `PING` request over a real Unix socket;
- coalesced `VERSION` + `STATUS` requests;
- idle STATUS reports `daemon_state`/`worker_state` and omits unsupported
  requested fields;
- disconnect/reconnect with multiple sequential clients;
- stale socket replacement;
- live socket rejection without unlinking the active path;
- malformed oversized stream containment followed by a successful next
  client.
- `printerctld` process startup, PLP/1 `PING` over the configured socket,
  and clean `SIGTERM` shutdown.

## Decisions taken

- `VERSION` advertises only `PRINTER_STATUS`. `PRINT_CONTROL` remains
  false until M16 wires real print execution.
- Malformed semantic/framing input closes the client connection instead
  of inventing partial recovery. The server remains alive.
- Simultaneous clients are not supported in M15. Add concurrency only
  when a later milestone has a concrete owner/dispatch requirement.
- STATUS does not fabricate progress, execution state or printer data.
- Default `printerctld` socket path is `/tmp/printerctl.sock`.

## Risks found

- The socket path length is limited by Linux `sockaddr_un.sun_path`
  (`107` usable bytes plus NUL). Longer paths are rejected.
- This is a library-level transport, not a long-running `printerctld`
  executable. Daemon process lifecycle remains future work.

## Verification

- `cmake -S . -B build-m15`
- `cmake --build build-m15`
- `ctest --test-dir build-m15 --output-on-failure`
- ASan/UBSan:
  `cmake -S . -B build-m15-asan -DCMAKE_C_COMPILER=clang -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'`
  + build + full `ctest`
- ThreadSanitizer:
  `cmake -S . -B build-m15-tsan -DCMAKE_C_COMPILER=clang -DCMAKE_C_FLAGS='-fsanitize=thread -g'`
  + `printerctl_daemon_runtime_tests` and `printerctl_daemon_socket_tests`

Result: 16/16 tests passed, including the Unix socket integration suite
and the `printerctld` process smoke test. ASan/UBSan passed 16/16.
TSan passed 3/3 on the runtime/socket/process targets, with no reported
races.

## Next steps

Proceed to M16 only after review accepts the transport boundary: connect
the daemon runtime to the existing print engine for exactly one active
print session, without adding queue/persistence/multi-printer behavior.
