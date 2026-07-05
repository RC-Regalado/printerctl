# printerctl Roadmap

## 1. Roadmap Strategy

This roadmap is designed for autonomous development by two agents:

- **Codex**: architecture, design review, documentation, testing strategy and acceptance validation.
- **Claude**: production implementation, test implementation, bug fixing and refactoring.

Each milestone must finish with documentation updates, automated tests and Codex validation before the next milestone starts.

## 2. Global Definition of Done

A task is complete only when:

1. The code builds with CMake.
2. GCC and Clang builds are warning-free with `-Wall -Wextra -Wpedantic`.
3. Relevant automated tests pass.
4. Safety policy remains enforced.
5. Operator-facing errors are clear.
6. Logs capture important behavior.
7. Documentation is updated when behavior changes.
8. Codex validates acceptance criteria.

## 3. Milestone 0 — Project Bootstrap

### Goal

Create a clean, buildable C17 project skeleton with documentation, tests and autonomous-agent conventions.

### Codex Tasks

- Confirm initial repository structure.
- Define coding conventions and module boundaries.
- Define test strategy and fixture layout.
- Review CMake quality gates.

### Claude Tasks

- Create CMake project.
- Create `src`, `include`, `tests`, `docs`, `scripts`, `docker` and `ansible` directories as needed.
- Add placeholder modules for CLI, config, logging, serial, Marlin, G-code and printer orchestration.
- Add minimal test executable.
- Add basic `README.md` development instructions.

### Acceptance Criteria

- `cmake -S . -B build` succeeds.
- `cmake --build build` succeeds.
- `ctest --test-dir build` succeeds.
- Empty/minimal binary can print version/help output.
- Documentation files exist under `docs/`.

## 4. Milestone 1 — CLI and Configuration

### Goal

Implement a stable command-line interface and configuration model without touching printer hardware.

### Codex Tasks

- Review CLI grammar and unsafe-command policy.
- Define exit-code categories.
- Define configuration precedence.

### Claude Tasks

- Implement global options: `--port`, `--baud`, `--timeout`, `--dry-run`, `--verbose`, `--config`.
- Implement commands: `print`, `pause`, `resume`, `cancel`, `temp`, `position`, `info`, `send`, `home`.
- Implement config defaults and CLI override behavior.
- Reject invalid argument combinations.
- Enforce `home --i-understand-no-endstops` requirement.

### Acceptance Criteria

- CLI tests cover valid and invalid commands.
- `printerctl --help` documents all MVP commands.
- `printerctl --dry-run print file.gcode` is accepted.
- `printerctl home` fails safely.
- `printerctl home --i-understand-no-endstops` is accepted by the parser but not yet required to perform hardware action.

## 5. Milestone 2 — Logging Foundation

### Goal

Provide structured logging for all future modules.

### Codex Tasks

- Approve log fields and event taxonomy.
- Decide initial format: plain line-oriented text or JSON Lines.
- Review privacy and operational safety implications.

### Claude Tasks

- Implement logger initialization and shutdown.
- Add timestamped info, warning and error logs.
- Add verbose-mode support.
- Add log helpers for serial TX/RX, state transitions, progress and errors.
- Add tests for formatting and level filtering.

### Acceptance Criteria

- Logs include timestamps.
- Verbose mode changes output detail.
- Logging API is usable by all modules without circular dependencies.
- Tests validate representative log records.

## 6. Milestone 3 — G-code Parser and Dry Run

### Goal

Read and normalize G-code files safely without serial communication.

### Codex Tasks

- Define allowed normalization rules.
- Review unsafe-command detection.
- Define progress calculation method.

### Claude Tasks

- Implement G-code file reader.
- Strip full-line and inline comments.
- Ignore blank lines.
- Track source line number and progress.
- Detect overly long lines.
- Block `G28` unless explicit confirmation is active.
- Implement dry-run output and logs.

### Acceptance Criteria

- Fixture files are parsed deterministically.
- Comments and blank lines are ignored.
- Progress is reported.
- Dry-run never opens serial devices.
- Unsafe `G28` fixture fails by default.
- Unit tests cover normal, empty, malformed and unsafe files.

## 7. Milestone 4 — Marlin Response Parser

### Goal

Parse Marlin responses independently from serial transport.

### Codex Tasks

- Define response classification table.
- Define behavior for `busy:`, `echo:`, `Error:` and `Resend:`.
- Review compatibility assumptions with Marlin.

### Claude Tasks

- Implement parser for `ok`, `Error:`, `busy:`, `echo:`, `Resend:`.
- Implement parsers for temperature, position and firmware info responses.
- Return explicit parser status values.
- Add unit tests for representative Marlin output.

### Acceptance Criteria

- `ok` is recognized as acknowledgement.
- `busy:` and `echo:` are non-terminal informational responses.
- `Error:` becomes failure.
- `Resend:` becomes unsupported/resend-required status in MVP.
- Temperature and position responses can be rendered for users.

## 8. Milestone 5 — Serial Transport

### Goal

Implement Linux USB serial communication with safe timeouts.

### Codex Tasks

- Review POSIX serial configuration.
- Define timeout behavior.
- Review reconnect policy for MVP.

### Claude Tasks

- Implement serial open, configure, read line, write line and close.
- Support configurable baud rate and timeout.
- Use raw mode suitable for Marlin communication.
- Ensure command lines are terminated correctly.
- Add integration tests using pseudo terminals.

### Acceptance Criteria

- Serial module can communicate with a pseudo-terminal mock.
- Timeout is reported explicitly.
- I/O errors are reported explicitly.
- No writes occur in dry-run mode.
- Tests cover open failure, timeout and normal line exchange.

## 9. Milestone 6 — Single Marlin Commands

### Goal

Implement operator commands that send one Marlin command and render the result.

### Codex Tasks

- Review command mapping.
- Review safety policy for `send`.
- Validate output expectations.

### Claude Tasks

- Implement `temp` using `M105`.
- Implement `position` using `M114`.
- Implement `info` using `M115`.
- Implement `send "COMMAND"` with acknowledgement handling.
- Log all TX/RX lines.

### Acceptance Criteria

- Commands wait for terminal Marlin response.
- `temp`, `position` and `info` produce human-readable output.
- `send` logs full exchange.
- Unsafe commands remain blocked unless explicitly allowed.
- Mock Marlin integration tests pass.

## 10. Milestone 7 — Reliable G-code Streaming MVP

### Goal

Stream a G-code file line by line, waiting for `ok` before sending the next command.

### Codex Tasks

- Review streaming state machine.
- Define failure and cancellation semantics.
- Validate progress and logging behavior.

### Claude Tasks

- Implement print session state machine.
- Send one normalized G-code command at a time.
- Wait for `ok` before continuing.
- Continue waiting on `busy:` and `echo:`.
- Stop gracefully on `Error:`, timeout, disconnect or `Resend:`.
- Report and log progress.

### Acceptance Criteria

- Mock printer verifies no serial flooding.
- Progress updates as commands are acknowledged.
- Timeout stops print gracefully.
- `Error:` stops print gracefully.
- `Resend:` stops with explicit unsupported status.
- Session summary includes final state, file and last acknowledged line.

## 11. Milestone 8 — Pause, Resume and Cancel

### Goal

Support operator control over active or direct print sessions.

### Codex Tasks

- Decide MVP session model: in-process only, direct Marlin command only, or both.
- Review command semantics for host streaming versus SD printing.
- Define testable behavior.

### Claude Tasks

- Implement pause behavior.
- Implement resume behavior.
- Implement cancel behavior.
- Map Marlin commands where applicable: `M25`, `M24`, `M524`.
- Ensure cancellation closes session cleanly.

### Acceptance Criteria

- Pause stops sending new commands after current acknowledgement.
- Resume continues from the next unsent command when supported.
- Cancel stops streaming and records cancelled state.
- Direct Marlin command mode is documented if used.
- Tests cover pause/resume/cancel state transitions.

## 12. Milestone 9 — Fault Tolerance and Recovery Records

### Goal

Improve robustness around communication failures and interrupted sessions.

### Codex Tasks

- Define retry/reconnect limits.
- Review session interruption record format.
- Confirm that automatic resume remains forbidden.

### Claude Tasks

- Add reconnect attempt logging if reconnect is implemented.
- Persist interrupted session metadata.
- Record last acknowledged line and progress.
- Ensure failure paths close file and serial resources.

### Acceptance Criteria

- Interrupted sessions record file, line, progress, timestamp and state.
- Power or serial failure never triggers automatic resume.
- Resource cleanup is verified in tests where practical.

## 13. Milestone 10 — APCUPSD Informational Integration

### Goal

Record UPS events without controlling the printer automatically.

### Codex Tasks

- Review APCUPSD integration model.
- Validate that `auto_resume=false` is enforced.
- Define event record fields.

### Claude Tasks

- Parse APCUPSD config section.
- Implement power-failure event recording hook.
- Implement power-restored event recording hook.
- Add tests for event recording.

### Acceptance Criteria

- Power failure records timestamp, file, last acknowledged line, progress and printer state.
- Power restored records event and notifies operator.
- Auto-resume is unavailable or hard-disabled.
- Tests prove no automatic resume occurs.

## 14. Milestone 11 — Documentation Completion

### Goal

Make the project usable and maintainable for future autonomous development.

### Codex Tasks

- Review and complete all documentation.
- Ensure docs match implementation.
- Add troubleshooting and safety guidance.

### Claude Tasks

- Update README usage examples.
- Maintain `docs/marlin.md`.
- Maintain `docs/testing.md`.
- Maintain `docs/safety.md`.
- Maintain `docs/logging.md`.
- Maintain `docs/apcupsd.md`.

### Acceptance Criteria

- README explains build, test and basic usage.
- Architecture and roadmap are current.
- Safety limitations are explicit.
- Testing instructions work from a fresh checkout.

## 15. Milestone 12 — MVP Release Candidate

### Goal

Validate the complete MVP against acceptance criteria.

### Codex Tasks

- Run final acceptance review.
- Check technical debt.
- Check security and safety implications.
- Confirm Marlin compatibility expectations.
- Approve or reject release candidate.

### Claude Tasks

- Fix final defects.
- Stabilize CLI output.
- Clean up dead code.
- Ensure all tests pass.
- Prepare release notes.

### Acceptance Criteria

- Builds with CMake.
- Connects to Marlin over USB serial.
- Streams G-code safely.
- Waits for `ok` before next command.
- Handles communication errors.
- Shows progress.
- Supports pause, resume and cancel according to documented MVP semantics.
- Queries temperatures, coordinates and printer information.
- Produces structured logs.
- Supports dry-run mode.
- Passes automated tests.
- Includes complete documentation.

## 16. Backlog After MVP

These items are intentionally outside the first MVP unless Codex reprioritizes them:

1. Marlin line numbering.
2. Checksums.
3. Full `Resend:` recovery.
4. Daemon mode for controlling long-running prints from separate CLI invocations.
5. Persistent job database.
6. Rich terminal UI.
7. More advanced printer state detection.
8. Multi-printer support.
9. Packaging for Arch Linux.
10. VM-based kernel-level serial testing.
11. Ansible provisioning for reproducible development environments.

## 17. Suggested First Autonomous Task

```text
Goal:
Bootstrap the printerctl repository as a C17/CMake project.

Scope:
Create the initial project structure, minimal CLI help/version output, empty module boundaries, one passing test target and developer build instructions.

Out of scope:
Serial communication, G-code parsing, Marlin parsing and real printer control.

Affected modules:
CMake, src/cli, include, tests, docs.

Safety considerations:
No printer communication must occur in this task.

Required tests:
- Binary runs with --help.
- Binary runs with --version.
- Test target passes under ctest.

Acceptance criteria:
- CMake configure/build/test succeeds.
- GCC and Clang builds are warning-free where available.
- Repository layout matches docs/architecture.md.
- README contains bootstrap build and test commands.

Documentation updates:
README.md and docs/roadmap.md if implementation diverges.
```
