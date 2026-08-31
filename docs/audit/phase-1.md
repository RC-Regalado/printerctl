# Phase 1 Final Re-Audit

Date: 2026-08-26
Scope: M13-M17 remediation of R1-R6

## Verdict

PHASE 1 APPROVED

## Executive summary

The five blocking findings from the Phase 1 audit have been remediated without changing PLP/1 framing, migrating the CLI, adding queues, adding recovery, or replacing the existing print engine.

The daemon now reports differentiated PLP statuses for `PRINT_START` failures, proves client lifetime is independent from execution lifetime, proves bidirectional serial exclusivity with the legacy CLI, and has process-level ACK-window tests for `SIGTERM`, `SIGPWR` and `PRINT_CANCEL` during active PTY-backed prints.

The final cancellation race fix is a TX authorization gate in `print_engine`, used by the existing printer streaming path. Signal handlers still only record intent. The worker/core commits cancellation outside the handler, closes the TX gate, polls cancellation while waiting for ACK, and prevents any newly-authorized G-code command from starting after commit.

## Milestone status

M13 PASS

Evidence: runtime lifecycle, worker thread, pipe IPC, startup/shutdown, correlation tokens and TSan-covered daemon runtime tests pass.

M14 PASS

Evidence: PLP/1 wire contract unchanged; golden vectors and parser/payload tests pass.

M15 PASS

Evidence: Unix socket transport tests pass; malformed clients remain contained; boundary guardrail passes.

M16 PASS

Evidence: socket `PRINT_START` uses worker/adapter result status, not generic BUSY; second active start returns numeric `BUSY`; source/serial/worker failures return contract statuses.

M17 PASS

Evidence: `printerctld_tests` now covers process-level `SIGTERM` printing, `SIGPWR` printing, `PRINT_CANCEL` ACK-window cancellation, daemon marker PID targeting, no post-commit host transmission, and serial exclusivity in both directions.

## Architecture findings

ID: ARCH-001
Severity: ACCEPTABLE
Milestone: M13-M17
Location: `src/daemon/*`, `src/protocol/*`, `src/printer/*`, `src/serial/*`, `CMakeLists.txt`
Problem: Dependency direction remains aligned with the approved architecture.
Why it matters: Phase 2 can move the CLI onto PLP without pulling daemon/socket concerns into core/printer/serial.
Required correction: None.
Evidence: include scan shows no `core -> daemon/socket/PLP`, no `printer/serial -> daemon`, and no `protocol -> printer/serial`.

ID: ARCH-002
Severity: FIXED
Milestone: M16-M17
Location: `src/daemon/worker.c`, `include/printerctl/daemon/ipc.h`, `src/daemon/socket.c`
Problem: `PRINT_START` result detail previously lost the domain failure class before the socket response.
Why it matters: The CLI-over-PLP path needs operable protocol statuses.
Required correction: Completed.
Evidence: internal `COMMAND_COMPLETED` now carries numeric `wire_status`; socket responses copy that status. Tests assert `BUSY`, `FILE_NOT_FOUND`, `FILE_NOT_READABLE`, `WORKER_UNAVAILABLE` and `SERIAL_ERROR`.

## Safety findings

ID: SAFE-001
Severity: FIXED
Milestone: M17
Location: `tests/test_printerctld.c`
Problem: Missing process-level SIGPWR active-print validation, later narrowed to an ACK-window cancellation race.
Why it matters: Low battery is a physical-safety path.
Required correction: Completed.
Evidence: test starts real `printerctld`, starts a PTY-backed print, verifies active marker PID equals daemon PID, sends `SIGPWR`, verifies the daemon remains responsive before releasing the held ACK, then ACKs the in-flight command and verifies marker removal and no further host command.

ID: SAFE-002
Severity: FIXED
Milestone: M17
Location: `src/daemon/worker.c`, `src/printerctld_main.c`, `tests/test_printerctld.c`
Problem: Missing process-level SIGTERM active-print validation; remediation exposed a race allowing one extra command after cancellation intent.
Why it matters: Shutdown must stop host transmission deterministically.
Required correction: Completed.
Evidence: signal handler records intent only; worker/core commits cancellation into the TX gate during ACK wait. Test sends `SIGTERM` while ACK is held, releases ACK after the worker can commit cancellation, and asserts process exit, socket cleanup, marker cleanup and no next host command.

ID: SAFE-004
Severity: FIXED
Milestone: M17
Location: `tests/test_printerctld.c`
Problem: Serial exclusivity had only partial validation.
Why it matters: Legacy CLI and daemon must never own the same serial device effectively at once.
Required correction: Completed.
Evidence: process-level tests cover daemon owns serial -> legacy `printerctl send` exits with communication error, and legacy `printerctl print` owns serial -> daemon `PRINT_START` returns numeric `SERIAL_ERROR`.

## Protocol findings

ID: PROTO-001
Severity: FIXED
Milestone: M16
Location: `src/daemon/socket.c`, `src/daemon/worker.c`, `docs/design/protocol-v1.md`
Problem: `PRINT_START` collapsed all command errors to `BUSY`.
Why it matters: `BUSY` must only mean active execution.
Required correction: Completed.
Evidence: `BUSY` is emitted only for active print rejection. Other tested failures map to `FILE_NOT_FOUND`, `FILE_NOT_READABLE`, `WORKER_UNAVAILABLE` or `SERIAL_ERROR`; docs now list final daemon mapping.

## Concurrency/lifetime findings

ID: LIFE-001
Severity: FIXED
Milestone: M17
Location: `src/daemon/worker.c`, `src/core/print_engine.c`, `src/printer/printer.c`, `src/serial/serial.c`
Problem: Signal-to-worker cancellation initially used non-atomic shared flags, and the first fix did not serialize cancellation commit with next-TX authorization.
Why it matters: Shutdown/cancel safety must not depend on observing a flag only between command exchanges.
Required correction: Completed.
Evidence: signal intent flags are atomic; cancellation commit closes a `pthread_mutex`-protected TX gate; ACK waits poll control via a serial wait hook; TSan daemon subset passes.

ID: LIFE-002
Severity: ACCEPTABLE
Milestone: M15-M17
Location: socket transport
Problem: Transport remains single-client-at-a-time.
Why it matters: This is still a known Phase 2 concern for concurrent monitoring, not a Phase 1 blocker.
Required correction: None for Phase 1.
Evidence: R2 is satisfied by sequential reconnect after client A disconnects; no concurrent client model was added.

## Test findings

ID: TEST-001
Severity: FIXED
Milestone: M17
Location: `tests/test_printerctld.c`
Problem: Missing process-level SIGTERM/SIGPWR printing tests.
Why it matters: Safety/lifecycle cannot be validated only by inspection.
Required correction: Completed.
Evidence: `printerctld_tests` includes active-print `SIGTERM`, `SIGPWR` and `PRINT_CANCEL` ACK-window cases and passes under normal, ASan/UBSan and TSan runs.

ID: TEST-002
Severity: FIXED
Milestone: M16
Location: `tests/test_daemon_socket.c`
Problem: Missing client A disconnect -> client B STATUS observes active execution.
Why it matters: Client connection must not own execution.
Required correction: Completed.
Evidence: new socket test starts print from client A, disconnects A, connects B, requests STATUS and asserts `execution_state == RUNNING`.

## Documentation drift

- `docs/design/protocol-v1.md`: updated with final `PRINT_START` daemon error mapping.
- `docs/testing.md`: updated to describe process-level daemon tests and serial exclusivity coverage.
- `docs/design/milestone-17-lifecycle-fault-compatibility.md`: updated to mention signal race remediation, marker targeting, second-client STATUS and inverse serial exclusivity.
- `README.md`: updated to reference the Phase 1 remediation audit and state that Phase 2 is ready but not started.
- Historical milestone comments that mention older queue/job plans remain as history; no Phase 1 blocker.

## Scenario matrix

| Scenario | Status | Evidence |
|---|---|---|
| PRINT_START active -> BUSY | PASS | `printerctl_daemon_socket_tests` asserts numeric `PRINTERCTL_PLP1_BUSY` |
| PRINT_START source failure -> correct PLP status | PASS | missing source -> `FILE_NOT_FOUND`; unreadable source -> `FILE_NOT_READABLE` |
| PRINT_START worker failure -> correct PLP status | PASS | closed command pipe -> `WORKER_UNAVAILABLE` |
| Client A START -> disconnect | PASS | socket acceptance test |
| Client B STATUS sees active execution | PASS | second client STATUS asserts `RUNNING` |
| daemon owns serial -> CLI fails cleanly | PASS | process-level legacy `printerctl send` exits `PRINTERCTL_EXIT_COMM_ERROR` |
| CLI owns serial -> daemon START fails cleanly | PASS | process-level daemon response status `SERIAL_ERROR` |
| SIGTERM while printing | PASS | ACK-window process-level `printerctld_tests` |
| no physical commands after SIGTERM | PASS | PTY receives no second command after cancellation commit/ACK |
| SIGPWR while printing | PASS | ACK-window process-level `printerctld_tests` |
| active marker targets daemon correctly | PASS | marker PID equals `printerctld` child PID |
| no physical commands after SIGPWR | PASS | PTY receives no second command after cancellation commit/ACK |
| PRINT_CANCEL ACK-window | PASS | PLP cancel response returns before held ACK is released; PTY receives no next command |
| no auto-resume | PASS | regression suite no-auto-resume proof still passes; no recovery path added |
| full regression suite | PASS | normal CTest 17/17 |

## Dependency graph

Observed build graph remains:

```text
printerctld
├── daemon runtime / worker
├── daemon socket transport
├── PLP/1 protocol
├── execution adapter
└── core
    └── printer
        └── serial

printerctl
└── legacy CLI path
    └── core
        └── printer
            └── serial
```

No new inverse dependency was introduced by remediation.

## Technical debt

BLOCKING

- None.

EARLY PHASE 2

- Single-client-at-a-time transport remains a migration risk for concurrent observers.
- Hostile process-level socket fuzzing remains narrower than parser-level fuzzing.
- Repeated pause/resume explicit contract tests remain an early Phase 2 hardening item.

ACCEPTABLE

- Source prevalidation in the worker distinguishes missing/unreadable/not-regular before opening serial; `print_engine` remains authoritative for streaming and lifecycle.
- Signal cancellation flags are process-local atomics used only to close the OS-signal race; normal control still flows through cmd pipe.

FUTURE

- Clean up old historical references to Phase II queue/job terminology when those docs are next touched.

## Required fixes before Phase 2

All required fixes are complete:

- PROTO-001 FIXED
- TEST-002 FIXED
- SAFE-004 FIXED
- SAFE-002 FIXED
- SAFE-001 FIXED

## Verification

- Normal build: PASS (`cmake --build build-remediation --parallel`)
- Full CTest: PASS 17/17 (`ctest --test-dir build-remediation --output-on-failure`)
- New remediation tests: PASS in `printerctl_daemon_socket_tests` and `printerctld_tests`
- ACK-window stress: PASS 100/100 (`ctest --test-dir build-remediation --output-on-failure -R '^printerctld_tests$' --repeat until-fail:100`)
- ASan/UBSan: PASS 17/17 (`build-remediation-asan`)
- TSan: PASS 4/4 daemon subset (`build-remediation-tsan`, regex `(daemon_(runtime|socket|print)|printerctld)`)
- Codex build hook: PASS
- Codex test hook: PASS
- Boundary guardrail: PASS
- Physical-action scan: PASS
- Scope scan: PASS; only reserved/documented future symbols and existing record metadata matched

## Phase 2 readiness

READY

The daemon can own and control a single print safely and observably across client disconnect, process shutdown, power-failure signal, serial ownership conflicts and protocol-level start failures. Phase 2 may begin after this remediation is reviewed/accepted, but Phase 2 was not started in this task.

## Final recommendation

It is safe to begin the next-phase migration:

```text
printerctl
   |
   +-- PLP/1
        |
        v
    printerctld
```

Do not add queues, recovery or multi-printer behavior as part of that migration.
