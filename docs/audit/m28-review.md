# M28 Technical Review

Date: 2026-10-13 (America/El_Salvador)
Verdict: M28 NOT ACCEPTED. Do not start M29 yet.

Review of the current working tree, including uncommitted M27/M28 work and
the later RAW_SEND timeout and operator-approved G28 file-policy amendments.
No implementation, existing test, milestone status or physical printer was
changed during this review. Only this report and isolated reproducers were added.

## Blocking Findings

### M28-001 - Unchecked numeric formatting corrupts the stack

- Severity: P1 / BLOCKING.
- Location: `src/daemon/worker.c:536`, especially lines 544-555;
  `src/macro/macro.c:98`, `src/config/config.c:88`.
- Problem: finite coordinates such as `1e200` pass config and full macro
  validation. `snprintf` returns the required length, not the stored length.
  The formatter adds that value to `used` even on truncation. The next append
  uses an out-of-bounds pointer and an underflowed `out_size - used`.
- Evidence: resolving `calibrate-front-left` with min X/Y=`1e200`, max
  X/Y=`2e200` succeeds; formatting its XY step causes ASan
  `stack-buffer-overflow`, WRITE of size 207, at `worker.c:548`.
  Reproduced with `repro/m28-format.c`, without opening serial.
- Why it matters: a configured built-in can crash/corrupt the daemon. In the
  real sequence, the Z lift precedes XY formatting, so formatting validation
  is incomplete before the first physical step.
- Required correction: bounded, checked formatting and complete validation
  of every generated command before execution is committed. Define numeric
  representability/precision rules, including positive feedrates that round
  to `F0.000`; reject unrepresentable commands without serial writes. This
  does not require inventing machine geometry or arbitrary travel limits.

### M28-002 - Power failure is ignored; shutdown cannot be observed in ACK wait

- Severity: P1 / BLOCKING.
- Location: `src/daemon/worker.c:490`, `src/daemon/worker.c:558`,
  `src/printer/printer.c:217`, `src/printerctld_main.c:69`.
- Problem: the macro control poll does not consume the worker's signal
  cancellation/power flags. PRINT_CANCEL_POWER_FAILURE is rejected as
  NOT_PRINTING. Macro execution calls the exchange API with no ACK-wait
  callback; SHUTDOWN is only polled between exchanges.
- Evidence: hold the G91 ACK, set the exact worker power-intent primitive
  used by the handler, enqueue the same power command used by daemon main,
  then release the ACK. `G1 Z5.000 F300.000` and final G90 are transmitted,
  and the macro snapshot becomes COMPLETED. See `repro/m28-runtime.c power`.
- Additional evidence: enqueue runtime shutdown while G91 waits for ACK.
  Continued `busy: processing` responses keep shutdown pending; releasing
  ACK permits it to complete. See `repro/m28-runtime.c shutdown`. The
  existing silence timeout does not bound a stream of nonterminal replies.
- Why it matters: low battery does not stop a daemon-owned physical macro;
  graceful termination may wait indefinitely on firmware chatter. M28's lack
  of a user-facing macro cancel command does not exempt execution from
  daemon shutdown/power safety.
- Required correction: worker-owned cancellation intent/commit semantics
  for macros, ACK-wait observation, and rejection of future TX after commit.
  Preserve async-signal-safe handlers and no physical cleanup G-code. Do not
  overload PRINT_CANCEL into a public macro cancellation API. Add controlled
  ACK-window and process-level signal tests before closure.
- Test limitation: these reproducers use the real runtime/worker/printer
  path and PTY, but inject signal intent rather than delivering OS SIGPWR
  to a printerctld process. The shutdown proof uses runtime SHUTDOWN rather
  than process SIGTERM. Those process-level macro scenarios remain NOT TESTED.

### M28-003 - Millimeter moves inherit firmware inch mode

- Severity: P1 / BLOCKING.
- Location: `src/daemon/worker.c:613`, `src/daemon/worker.c:536`;
  `include/printerctl/macro.h:73` (`feedrate_mm_min`).
- Problem: macros establish G90/G91 but never G21. The API/config express
  distances and feedrates in millimeters. A previous explicit G20 can leave
  compatible firmware in inches.
- Evidence: `repro/m28-runtime.c units` first sends G20 through the runtime,
  gets success, then observes exactly G91, G1 Z5.000 F300.000, G90: no G21.
  This is a wire-level proof, not a simulation of physical inch conversion.
- Why it matters: on Marlin with INCH_MODE_SUPPORT, G20 changes subsequent
  coordinates to inches; a requested 5 mm lift can instead request 5 inches.
  The outcome must not depend on inherited unit mode or an assumed USB reset.
  [Marlin G20 reference](https://marlinfw.org/docs/gcode/G020.html).
- Required correction: explicitly establish millimeters, obtain the terminal
  response before motion, and document the unit-mode postcondition. A G21
  failure/cancellation must prevent all subsequent moves. Add a modal-state
  regression with G20 precondition. No homing or new movement is needed.

## Validation Gap

M28-004: P2 / required validation before closure.

The full ASan/UBSan regression run was NOT clean: 19/20 passed;
`printerctld_tests` timed out at 60 seconds with `FAIL: read_full reads`.
A targeted rerun passed in 21.88 seconds. This is intermittent evidence,
not proof of a resolved failure. It is not attributed to M28 without further
diagnosis; the larger working tree contains earlier signal/observer changes.
Preserve the failed result and isolate the failure before declaring the
current baseline fully validated. Existing macro tests also omit the three
hostile/modal/power scenarios reproduced above.

## Additional UX Finding

### UX-001 - Print observer shows only immediate completion

- Severity: P1 / BLOCKING, lifecycle/correlation. Initially classified as
  provisional P2 UX; controlled reproduction below justifies escalation.
- Scope: existing host-print CLI/runtime observability, not a new macro feature.
- Location: `src/cli/cli.c:842` (`run_print_progress_observer`),
  `src/daemon/worker.c:686` (PRINT_START acceptance before engine run),
  `src/core/print_engine.c:358` (pre-scan before STARTED event),
  `src/daemon/runtime.c:493` (command-result correlation).
- Reported behavior: the operator starts
  `build/printerctl print gcode/Soporte-Part-2.gcode`, sees acceptance followed
  directly by `Completed: 100% (10058/10058)`, with no intermediate progress.
- Operator-provided evidence:

```text
2026-10-14T01:47:29.433Z INFO session_start
printerctl: print accepted by printerctld: 'gcode/Soporte-Part-2.gcode'
printerctl: Completed: 100% (10058/10058)
2026-10-14T01:47:29.434Z INFO session_end
```

- Confirmed cause: the worker emits PRINT_START success before engine run;
  the engine pre-scans the source before publishing STARTED. During that
  window, STATUS returns the previous execution's terminal snapshot. The
  observer accepts that unrelated terminal result and exits successfully.
- Controlled evidence: job A completes 1/1. Job B has two source commands.
  Hold B at the first pre-scan fopen without modifying production binaries.
  CLI B prints `Completed: 100% (1/1)` and exits 0 while B has transmitted
  zero commands. Release the gate: B actually starts RUNNING 0/2, then
  eventually completes 2/2. Reproduced once, then in 10/10 isolated repeats.
- Expected behavior: show the accepted execution's progress and remain attached
  until that execution terminates. Do not manufacture percentages or require
  a RUNNING display for jobs that genuinely finish before the first poll.
- Required correction: publish the newly accepted execution's nonterminal
  state and reset counters through worker events before success can reach
  the client. Preserve control-owned snapshot updates and actual completion
  semantics. Do not hide the race with sleeps or require observing RUNNING
  for jobs that genuinely complete quickly.
- Required regressions: previous COMPLETED/FAILED/CANCELLED execution followed
  by a new start; delayed pre-scan; held first ACK; counters and terminal
  result belonging to the new execution; genuinely fast completion.
- Status: CONFIRMED / NOT FIXED. See [UX-001 check](ux001-check.md) and its
  executable/PTY reproducer. The daemon continues printing independently;
  the confirmed defect is false completion and loss of CLI observation.

## Contracts That Hold

- Client/PLP layers gained no public macro surface or arbitrary macro G-code.
- Control sends internal commands; worker alone executes mutable macro state.
- PRINT and MACRO ownership exclusion is covered in both directions.
- Registry/config resolution and typed validation precede serial connection.
- Successful tested sequences end in G90. Error and silence timeout stop
  subsequent steps; no rollback, lowering cleanup, homing or restart is added.
- Macro events flow through event_pipe to the control-owned snapshot.
- Engine reuse for PRINT remains intact; macros do not duplicate a print engine.
- `park` remains non-executable. Queue, user DSL, Web/TUI and history are absent.

## Verification Evidence

All suites ran with a separate /tmp namespace, excluding the real daemon socket.

| Check | Result | Evidence |
|---|---|---|
| Normal build | PASS | `cmake --build build -j 4` |
| Full normal CTest | PASS | 20/20, 28.36 s, per-test timeout 60 s |
| ASan/UBSan build | PASS | `build-asan`, GCC, address+undefined flags |
| Full ASan/UBSan CTest | FAIL | 19/20; printerctld_tests timeout / read_full failure |
| Targeted sanitizer rerun | PASS | printerctld_tests, 21.88 s; does not erase failure |
| TSan build | PASS | `build-tsan`, Clang, thread sanitizer |
| Relevant TSan suites | PASS | macro, daemon runtime, daemon print: 3/3, 1.32 s |
| Numeric-overflow repro | FAIL contract | ASan stack-buffer-overflow at worker.c:548 |
| Inch-mode repro | FAIL contract | G20 accepted, macro emits no G21 |
| Power-intent repro | FAIL contract | subsequent move and COMPLETED observed |
| Shutdown ACK-wait repro | FAIL lifecycle | remains pending throughout chatter until ACK |
| Boundary guardrail | PASS | `.codex/hooks/check-boundaries.sh` |
| Physical-action diff guardrail | NOT VALIDATED | Git object unreadable; hook prints misleading PASS |
| git diff --check | NOT VALIDATED | unable to read a8329dec250477aedddd07ccb37ac578c18072bf |
| Real-hardware macro tests | NOT TESTED | review intentionally uses PTY only |

The physical-action hook failed to read object
`a8d2fe03f529e387c2a1f2e0438d1e069107acbe`. Its swallowed Git error is not
a clean scan. No guardrail suppression or Git repair was performed.
TSan PASS applies only to the three suites actually executed; it does not
establish signal correctness. No Valgrind or full TSan suite was run.

## Reproduction

From the repository root, using the builds above:

```bash
gcc -I include docs/audit/repro/m28-runtime.c -o /tmp/m28-runtime \
  -Wl,--start-group build/libprinterctl_*.a -Wl,--end-group -pthread -lm
gcc -fsanitize=address,undefined -g -O1 -I include \
  docs/audit/repro/m28-format.c -o /tmp/m28-format \
  -Wl,--start-group build-asan/libprinterctl_*.a -Wl,--end-group -pthread -lm
/tmp/m28-format
bwrap --ro-bind / / --dev-bind /dev /dev --proc /proc --tmpfs /tmp \
  --ro-bind /tmp/m28-runtime /tmp/review /tmp/review units
bwrap --ro-bind / / --dev-bind /dev /dev --proc /proc --tmpfs /tmp \
  --ro-bind /tmp/m28-runtime /tmp/review /tmp/review power
bwrap --ro-bind / / --dev-bind /dev /dev --proc /proc --tmpfs /tmp \
  --ro-bind /tmp/m28-runtime /tmp/review /tmp/review shutdown
```

The formatter is included verbatim to instrument the production static
function; it is not a second implementation. The runtime reproducer reuses
existing PTY helpers. Successful repro exits mean the *bug was observed*,
not contract acceptance; the overflow repro exits nonzero with ASan.

Suite isolation used:

```bash
bwrap --ro-bind / / --dev-bind /dev /dev --proc /proc --tmpfs /tmp \
  --bind "$PWD" "$PWD" ctest --test-dir build --output-on-failure --timeout 60
```

For sanitizers, use build-asan and pass
`env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`
before ctest. For TSan, use build-tsan and select
`^(printerctl_macro_tests|printerctl_daemon_runtime_tests|printerctl_daemon_print_tests)$`.

## Documentation And Accepted Debt

- The design correctly states that homed-state tracking and authoritative
  machine bounds are absent. This is a deliberate M28 limit, not a demand
  to implement new geometry/homing machinery in remediation.
- M28 legitimately remains IN PROGRESS. README describes implemented paths,
  not an independent acceptance; no status was silently advanced.
- `docs/safety.md` section 3 still lists only file/send/home/SD sources of
  physical commands, omitting explicitly selected daemon-owned macros.
  Reconcile that policy when M28 closes; do not reinterpret it as permission
  for automatic macros.
- M29/M30 public surfaces and user-defined macros remain future work.

## Required Remediation And Exit Gate

1. Fix M28-001: full safe wire formatting/representability before any TX;
   add an automated ASan regression for extreme finite values.
2. Fix M28-002: power/shutdown cancellation intent, commit and ACK-wait
   handling; add macro signal/ACK-window tests and no-extra-command assertions.
3. Fix M28-003: explicit unit-mode semantics and G20 regression.
4. Fix UX-001's confirmed premature-completion lifecycle defect and add
   sequential-print/startup-window regressions.
5. Diagnose M28-004's intermittent regression; restore trustworthy Git-based
   scans or document an independently reproducible replacement with evidence.
6. Re-run the affected suites, sanitizer gates and this technical review;
   only then accept M28 and define M29.

No general redesign, recovery feature, CLI macro UX or physical shutdown
sequence is required by these findings. M29 readiness: NOT READY.
