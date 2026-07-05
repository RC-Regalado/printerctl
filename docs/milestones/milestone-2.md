---
title: Milestone 2 - Logging Foundation
milestone: 2
status: done
date: 2026-07-05
tags: [printerctl, milestone, logging]
---

# Milestone 2 — Logging Foundation

Source of truth: [[Agents]], [[architecture]], [[roadmap]]. Design
details: [[milestone-2-logging|docs/design/milestone-2-logging.md]].

## 1. Technical summary

Implemented a structured, timestamped logging module and wired it into
the Milestone 1 CLI, without touching serial, G-code or Marlin protocol
code.

`printerctl_logging` (`include/printerctl/logging.h`,
`src/logging/logging.c`) is a new, independent static library: no
dependency on `cli` or `config`. It exposes a `printerctl_logger_t`
(a borrowed `FILE*`, a `verbose` flag and an `initialized` flag),
lifecycle functions (`printerctl_log_init`/`printerctl_log_shutdown`), a
generic `printerctl_log_record(logger, level, event, detail)` and five
minimal helpers (`printerctl_log_serial_tx`, `printerctl_log_serial_rx`,
`printerctl_log_state_transition`, `printerctl_log_progress`,
`printerctl_log_error`). Every record is one physical line of TSV —
`timestamp\tLEVEL\tevent\tdetail\n` — written field-by-field directly to
the stream with no heap allocation and no intermediate buffer; escaping
of `detail` (backslash, tab, CR, LF, other control bytes, DEL) is
centralized inside the module and is never the caller's responsibility.
Four levels (`DEBUG`/`INFO`/`WARNING`/`ERROR`); `DEBUG` is filtered
unless the logger is verbose, and `ERROR` flushes immediately. Eleven
fixed Milestone 2 events cover both what the CLI does today
(`session_start`, `session_end`, `config_summary`, `command_parsed`,
`command_unavailable`) and what future modules will need
(`serial_tx`, `serial_rx`, `state_transition`, `progress`, `error`,
`warning`).

`printerctl_cli_run()` (`src/cli/cli.c`) now logs every invocation other
than `--help`/`--version`. A parse error, configuration error, or the
`home` safety rejection each keep their existing Milestone 1
human-readable message and additionally record one `ERROR`/`error` log
line. Every valid, resolved command logs a session lifecycle
(`session_start` → `config_summary` → `command_parsed` [`DEBUG`,
`--verbose`-only] → `command_unavailable` [`WARNING`] → `session_end`)
around the unchanged Milestone 1 "not yet implemented" output. No exit
code changed and no Milestone 1 prose was removed. `config_summary` never
logs the raw `--port` value or the `--config` path (only
`port_set`/`config_file_set` booleans); `command_parsed` and
`command_unavailable` never log `print`'s file argument or `send`'s raw
command text — only `command=<name>`. See
[[milestone-2-logging]] §12–13 for the full integration and privacy
rationale.

## 2. Files added or modified

```text
include/printerctl/logging.h          (new)
src/logging/logging.c                 (new)
CMakeLists.txt                        (new printerctl_logging library, linked into printerctl_cli)
src/cli/cli.c                         (session logging + transient ERROR logging on error paths)
tests/CMakeLists.txt                  (new printerctl_logging_tests target)
tests/test_logging.c                  (new)
tests/test_cli.c                      (new logging-focused tests; existing tests still pass unchanged)
docs/design/milestone-2-logging.md    (new)
docs/logging.md                       (new)
docs/milestones/milestone-2.md        (this file)
architecture.md                       (§6.3 resolved: format, taxonomy, privacy; §13 item 2 resolved)
README.md                             (status, test target list, Logging section)
```

`include/printerctl/cli.h` and `src/main.c` are unchanged: the public CLI
contract (exit codes, `printerctl_cli_run()`'s signature) did not need to
change, and `main()` remains the same two-line shim.

## 3. Build and test targets

- `printerctl_logging` — new static library, `src/logging/logging.c`
  only. No dependency on `cli` or `config`; testable in complete
  isolation.
- `printerctl_cli` — now `PUBLIC`-linked against both `printerctl_config`
  and `printerctl_logging`.
- `printerctl_logging_tests` — new, independent CTest binary linking only
  `printerctl_logging`. Covers: RFC3339-with-milliseconds timestamp shape
  and four-field TSV structure; all four levels including `ERROR`'s
  immediate flush versus other levels only flushing at shutdown; verbose
  DEBUG filtering (both directions); escaping of backslash, tab, CR, LF, a
  C0 control byte, DEL and a raw `>= 0x80` byte, confirming exactly one
  physical line and exactly three structural tabs survive; all five
  helpers' fixed level/event pairs; init/shutdown idempotency (including a
  rejected `init(NULL stream)` leaving the logger cleanly uninitialized,
  and a second shutdown being a no-op); and practical errors (`NULL`
  logger on every function, out-of-range level/event enum values, a `NULL`
  detail rendering as an empty field, and a real `ERR_WRITE` from a stream
  reopened read-only).
- `printerctl_tests` — extended with: help/version producing zero log
  output; a full non-verbose session lifecycle (exactly four log lines,
  no `command_parsed`); `--verbose` adding the fifth (`command_parsed`)
  line; `config_summary` omitting the raw port value and config path
  while still reporting their `*_set` booleans; `print`/`send` never
  leaking their file path or raw command text; and each of the three
  error paths (usage, configuration, safety) adding exactly one
  `ERROR`/`error` log line alongside its unchanged human-readable message.
  All pre-existing Milestone 1 tests pass unmodified, since they assert
  on `stdout` content or exit codes that logging never touches, or use
  `strstr` checks broad enough to tolerate the new `stderr` lines.
- `printerctl_config_tests` — unchanged.

All three binaries are registered with `enable_testing()`/`add_test()`
and run under `ctest`.

## 4. Decisions

1. **Plain TSV, not JSON Lines**, resolving `architecture.md` §13 item 2.
   Chosen because Milestone 2 has no real log consumer yet beyond a human
   reading `stderr`; the API already hides the format behind
   `printerctl_log_record()` and five helpers, so switching to JSON Lines
   later is a `logging.c`-only change. See [[milestone-2-logging]] §2.
2. **`detail` is opaque text, never a second parsed grammar.** Call sites
   may write `key=value` pairs inside it for readability, but
   `logging.c` never parses or validates that shape, and no future
   change may silently promote it to a contract. See
   [[milestone-2-logging]] §3.
3. **Escaping is centralized in `printerctl_log_record()`, never the
   caller's job.** Backslash, tab, CR, LF, other C0 control bytes and DEL
   become visible `\`-sequences; bytes `>= 0x80` pass through unchanged.
   Guarantees one physical line per record regardless of what untrusted
   or arbitrary text (a `send` argument, a printer response) ends up in
   `detail`. See [[milestone-2-logging]] §4.
4. **No heap, no line buffer.** Every field is written directly to the
   `FILE*`, checking each write's return value; nothing is assembled in
   an intermediate buffer that could silently truncate. See
   [[milestone-2-logging]] §5.
5. **Single-threaded, no atomicity guarantee, documented explicitly
   rather than left implicit.** No mutex, no thread-local logger. Revisit
   only when a multi-threaded daemon mode is actually built. See
   [[milestone-2-logging]] §5.
6. **`printerctl_logger_t` follows the same output-parameter discipline
   as `cli.h`/`config.h`:** `printerctl_log_init()` always overwrites the
   full struct, on success or failure; `printerctl_log_shutdown()` is
   idempotent (never double-flushes, safe to call on an
   already-shut-down or never-initialized logger) and never calls
   `fclose()` on a stream it does not own.
7. **Every operation returns an explicit status; nothing fails
   silently.** Six status values cover invalid arguments, an
   uninitialized/already-shut-down logger, a clock failure, a write
   failure and a flush failure. `detail == NULL` is explicitly not an
   error — treated as an empty field.
8. **Generic API plus exactly five minimal helpers**
   (`serial_tx`/`serial_rx`/`state_transition`/`progress`/`error`), as
   approved. Every other fixed event is logged through
   `printerctl_log_record()` directly — no helper was pre-built for an
   event without a repeating call-site shape yet.
9. **Timestamps use `timespec_get`/`gmtime_r` directly; no clock
   abstraction.** Tests validate the emitted timestamp's shape (24-byte
   RFC3339-with-milliseconds pattern), not an exact injected value —
   adding a mockable clock for this milestone's acceptance criteria would
   be speculative. See [[milestone-2-logging]] §10.
10. **CLI logging is purely additive: no exit code changed, no
    Milestone 1 prose removed.** A logging failure — a bad
    `stream`/logger state, an unexpected write or flush error — prints
    one brief `printerctl: warning: ...` line and never alters the
    primary exit code or `out`/`err` human-readable content.
11. **The CLI never logs the raw `--port` value, the `--config` path,
    `print`'s file argument, or `send`'s raw command text.** Only
    booleans (`port_set`, `config_file_set`) and `command=<name>`. See
    [[milestone-2-logging]] §13.

## 5. Deviations from `architecture.md`

None beyond resolving the open decision `architecture.md` §13 item 2
already flagged (plain text vs. JSON Lines) — resolved in favor of plain
TSV, recorded above and in `architecture.md` §6.3/§13 directly, rather
than a deviation from an already-fixed design.

## 6. Risks

- **No persistent log file yet.** `printerctl` only ever logs to whatever
  stream it is given (`stderr` in the CLI). There is no on-disk log,
  rotation or retention policy. Deferred until a real consumer (a daemon,
  a long unattended print, a log-shipping integration) exists to justify
  the design work; see [[milestone-2-logging]] §14.
- **No structured (JSON) output.** TSV is the only format. The API shape
  is deliberately narrow so this can change later without touching call
  sites, but no second format exists today.
- **No async/batched logging and no locking.** Every record is written
  synchronously on the calling thread with no internal buffering beyond
  the C stream itself, and nothing here is thread-safe. Both are
  explicitly out of scope until a multi-threaded daemon mode (roadmap.md
  post-MVP backlog, item 4) is real.
- **`gmtime_r`/`open_memstream`/`mkstemp` are POSIX**, consistent with
  the Linux-only target platform (`Agents.md`). `logging.c` (production
  code, not just tests) now also requires `_POSIX_C_SOURCE 200809L` for
  `gmtime_r`, the same pattern the test binaries already used for
  Milestone 1's POSIX-only test helpers.
- **`send`'s `G28` safety review is still deferred to Milestone 6**
  (carried forward from Milestone 1, unaffected by this milestone): `send`
  still does not execute anything, and now also never logs its raw
  command text, which is an additional reason a future implementation
  must not assume `send`'s content is ever visible in the log for
  auditing — the safety check itself must live in the execution path,
  not be inferred from logs.

## 7. Testing

All three suites pass under `ctest --test-dir <build-dir>
--output-on-failure`, on both compilers, from a clean build
(`cmake --build <build-dir> --clean-first`):

- **GCC 16.1.1**, `-std=c17 -Wall -Wextra -Wpedantic`: zero warnings,
  zero errors. `printerctl_tests`, `printerctl_config_tests` and
  `printerctl_logging_tests` all pass (3/3 via `ctest`).
- **Clang 22.1.6**, same flags and clean-build procedure: zero warnings,
  zero errors. All three test binaries pass (3/3 via `ctest`).

Manual smoke test of the built binary confirmed the exact behavior
described in §1 and in `docs/logging.md`:

- `printerctl --help` / `--version`: empty `stderr`, no log lines, exit
  `0`.
- `printerctl pause` (non-verbose): exit `5`; `stderr` contains exactly
  four TSV lines — `session_start`, `config_summary`
  (`port_set=false config_file_set=false baud=115200 timeout_ms=5000
  dry_run=false verbose=false`), `command_unavailable`
  (`command=pause`), `session_end` — no `command_parsed` line.
- `printerctl --verbose pause`: exit `5`; same four lines plus a fifth
  `DEBUG`/`command_parsed` line (`command=pause`).
- `printerctl --bogus pause`: existing usage-error message and usage
  hint unchanged on `stderr`, plus one additional
  `ERROR`/`error` line with the same message; exit `2`.
- `printerctl --config /nonexistent pause`: existing configuration-error
  message unchanged, plus one `ERROR`/`error` line; exit `3`.
- `printerctl home`: existing safety-rejection message unchanged, plus
  one `ERROR`/`error` line; exit `4`.

## 8. Next steps

Milestone 3 (G-code Parser and Dry Run), per `roadmap.md` §6:

- Implement the G-code file reader, comment/blank-line stripping, source
  line/progress tracking and the `G28` unsafe-command check, all
  independent of serial communication.
- Give the G-code streamer real `serial_tx`-shaped events once it starts
  producing normalized commands — the event and the `printerctl_log_serial_tx`
  helper already exist and are unused in production code today.
- Revisit `send`'s deferred `G28` safety gate (carried forward from
  Milestone 1 §9) once Milestone 6 gives `send` a real execution path;
  this milestone's decision that `send`'s content is never logged makes it
  more important, not less, that the safety check live in code rather
  than be inferred from an audit log.

Also carried forward, not part of Milestone 3 but recorded so it is not
lost: persistent log files, log rotation, JSON/structured output,
async/batched logging and any locking/thread-safety work are all
deferred until a real consumer exists (a daemon mode, a log-shipping
integration, or long-running unattended prints) — see
[[milestone-2-logging]] §14. Building any of them speculatively, against
zero real usage, would not match this project's engineering practice.
