---
title: Milestone 2 Design - Logging Foundation
milestone: 2
status: done
date: 2026-07-05
tags: [printerctl, milestone, design, logging]
---

# Milestone 2 Design — Logging Foundation

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

This note records the concrete design decisions behind the Milestone 2
implementation, ahead of the milestone report in
[[milestone-2|docs/milestones/milestone-2.md]].

## 1. Module split

A third independent static library, matching `architecture.md` §4's
`logging` module boundary:

- `printerctl_logging` (`include/printerctl/logging.h`,
  `src/logging/logging.c`): logger lifecycle, the generic
  `printerctl_log_record()` API, escaping and the fixed Milestone 2 event
  helpers. No knowledge of `argv`, CLI syntax or the configuration model.
- `printerctl_cli` now links `printerctl_config` and `printerctl_logging`
  (`target_link_libraries(printerctl_cli PUBLIC printerctl_config
  printerctl_logging)`). Neither `config` nor `logging` depends on `cli`,
  and `logging` does not depend on `config` — a module that only needs
  logging (e.g. a future `serial` module) never has to pull in the CLI
  grammar or the config file parser.

Its own CTest binary, `printerctl_logging_tests`, verifies the logging
module in complete isolation, the same pattern `printerctl_config_tests`
established in Milestone 1.

## 2. Format: plain TSV, hidden behind the API

`architecture.md` §6.3 left the initial format an open technical decision
between line-oriented plain text and JSON Lines (§13, item 2). This
milestone resolves it: **plain TSV**, one physical line per record:

```text
<RFC3339 UTC timestamp with milliseconds>\t<LEVEL>\t<event>\t<detail>\n
```

Example:

```text
2026-07-05T20:10:06.813Z	INFO	config_summary	port_set=false config_file_set=false baud=115200 timeout_ms=5000 dry_run=false verbose=false
```

Nothing outside `logging.c` assembles this line. Callers only see
`printerctl_log_record()` and a handful of typed helpers; the exact
separator, field order and escaping rules are private to the module. This
is the same design goal `architecture.md` §6.3 already stated ("hide
format details behind a logging API so JSON Lines can become the default
without refactoring callers") — TSV today, but every call site already
goes through the same narrow surface, so switching later is a `logging.c`
change, not a repository-wide one.

TSV was chosen over JSON Lines for Milestone 2 specifically because there
is still exactly one consumer (a human reading `stderr`, or `grep`/`cut`
on it) and no downstream parser yet. A hand-rolled JSON encoder would add
escaping rules of its own without a consumer to justify the complexity;
revisit this the day a real log consumer (dashboard, structured log
shipper) exists.

## 3. `detail` is opaque text, not a second grammar

`detail` is a single opaque string field. Call sites are free to write
`key=value`-shaped text inside it for human readability (and the CLI
integration does exactly that for `config_summary`, `command_parsed` and
`command_unavailable`), but `logging.c` never parses, validates or
special-cases that shape. There is no contractual guarantee that `detail`
is machine-parseable key=value text, and no future change may quietly turn
this informal convention into one without a design update here. This
keeps the module's contract to exactly what it promises: one opaque
string per record, escaped for safety, nothing more.

## 4. Escaping is centralized and mandatory

All escaping happens inside `printerctl_log_record()` (specifically
`write_escaped_detail()` in `logging.c`), never at the call site. A caller
cannot forget to escape, and cannot introduce a fake extra field or a
second physical line by passing attacker- or user-controlled text (a
`send` command string, a file path, a firmware string from the printer)
straight into `detail`.

Rules, applied byte-by-byte to `detail`:

| Input byte | Output |
|---|---|
| `\` | `\\` |
| `\t` | `\t` (two characters: backslash, `t`) |
| `\r` | `\r` (two characters) |
| `\n` | `\n` (two characters) |
| other C0 control byte (`< 0x20`) or DEL (`0x7F`) | `\xHH` (uppercase hex) |
| any byte `>= 0x80` | passed through unchanged |

Bytes `>= 0x80` are preserved as-is rather than escaped because this
format has no defined text encoding contract beyond "not corrupted by
control bytes or embedded line breaks"; UTF-8 text (e.g. from a printer's
`M115` response) round-trips correctly, and a non-UTF-8 byte sequence is
no worse than what a raw `cat` of the log would already show. Escaping
"visible" control characters instead of stripping them was a deliberate
choice: a log line stays exactly one line no matter what a caller passes
in, and the escape sequences are human-readable in a terminal (`\t`,
`\r`, `\n` read naturally; `\xHH` is unambiguous for anything else).
`NULL` is treated identically to an empty string — never a distinct
"missing" marker requiring special handling downstream.

## 5. No heap, no line buffer, single-threaded

`printerctl_log_record()` allocates nothing on the heap and never
assembles the record into an intermediate fixed-size buffer that could be
silently truncated. Every field — timestamp, level name, event name, each
escaped byte or byte sequence of `detail`, the trailing newline — is
written directly to the `FILE*` given at `printerctl_log_init()` time via
`fprintf`/`fputs`/`fputc`, checking each call's return value for a write
error. This mirrors Milestone 1's "ownership without heap" principle
(`docs/design/milestone-1-cli-config.md` §4), extended here to "no
buffering surface to overflow or truncate" rather than just "no
heap-owned pointers."

The header documents explicitly that this module is single-threaded and
makes no atomicity promise: nothing serializes a `printerctl_log_record()`
call against another writer on the same stream, and nothing here is safe
to call concurrently against the same `printerctl_logger_t` from more than
one thread. This is acceptable today because `printerctl` is a
single-threaded CLI process. It must be revisited — most likely with an
internal lock around the stream, or a per-thread logger — the day a
multi-threaded daemon mode (roadmap.md's post-MVP backlog, item 4) is
actually built. Promising thread safety now, with no multi-threaded
consumer to test it against, would be exactly the kind of untested,
speculative guarantee this project avoids.

## 6. `printerctl_logger_t` lifecycle

```c
typedef struct {
    FILE *stream;
    bool verbose;
    bool initialized;
} printerctl_logger_t;
```

- `stream` is borrowed, never owned. `printerctl_log_shutdown()` calls
  `fflush()`, never `fclose()`. The caller opens and closes the stream (in
  the CLI's case, `stderr`, which `main()` never closes).
- `printerctl_log_init(logger, stream, verbose)` always overwrites the
  full struct, on both success and failure, so a caller never needs to
  zero it first (the same "output parameters are always fully
  initialized" convention `cli.h`/`config.h` already use). Passing a NULL
  `stream` is a usage error and leaves `*logger` in a well-defined
  not-initialized state rather than partially set.
- `printerctl_log_shutdown()` flushes and marks the logger uninitialized.
  It is idempotent: calling it again (already shut down, or never
  initialized) is a no-op returning `PRINTERCTL_LOG_OK`, so a caller
  cleaning up on multiple exit paths never needs to track whether
  shutdown already ran.
- Every operation returns an explicit `printerctl_log_status_t`. There are
  no silent failures: an invalid enum value, an uninitialized/NULL logger,
  a clock read failure, a stream write failure or a flush failure are all
  distinct, checkable statuses.

## 7. Levels and flushing

Four levels: `DEBUG`, `INFO`, `WARNING`, `ERROR`.

- `DEBUG` records are silently discarded (not an error — returns
  `PRINTERCTL_LOG_OK`) unless the logger was initialized with
  `verbose == true`. This is the only level affected by verbosity;
  `INFO`/`WARNING`/`ERROR` are always written.
- `ERROR` records `fflush()` the stream immediately after writing, so an
  error is durable on disk/terminal even if the process crashes or is
  killed immediately afterward, without requiring every caller to
  remember to flush after logging a failure.
- Every other level relies on the stream's normal buffering and is only
  guaranteed flushed by `printerctl_log_shutdown()`. This keeps
  high-frequency future logging (e.g. `serial_tx`/`serial_rx` during
  active streaming) from forcing a flush per line.

## 8. Fixed Milestone 2 event taxonomy

```c
session_start, session_end, config_summary, command_parsed,
command_unavailable, serial_tx, serial_rx, state_transition, progress,
error, warning
```

This is the complete set `roadmap.md` §5 and `Agents.md`'s logging section
call for at this milestone: session boundaries, configuration visibility,
CLI command flow (`command_parsed`/`command_unavailable` — real today),
and typed events for capabilities that do not exist yet
(`serial_tx`/`serial_rx`, `state_transition`, `progress`) so the serial,
Marlin and printer-session modules (Milestones 5–7) have an event to call
into without inventing their own ad hoc string the day they land. Adding a
new event later is an explicit, reviewed change to the enum and to the
name table in `logging.c` — never a free-form string a caller can invent.

## 9. Generic API plus five minimal helpers

```c
printerctl_log_record(logger, level, event, detail);

printerctl_log_serial_tx(logger, detail);          /* DEBUG, serial_tx */
printerctl_log_serial_rx(logger, detail);          /* DEBUG, serial_rx */
printerctl_log_state_transition(logger, detail);   /* INFO,  state_transition */
printerctl_log_progress(logger, detail);           /* INFO,  progress */
printerctl_log_error(logger, detail);              /* ERROR, error */
```

Every other fixed event (`session_start`, `session_end`,
`config_summary`, `command_parsed`, `command_unavailable`, `warning`) is
logged through the generic `printerctl_log_record()` call directly — the
CLI integration (§10) does exactly this. The five helpers exist only for
event shapes a *future* module will call repeatedly and that have an
obvious, single correct level (serial transmission is inherently
high-volume/DEBUG; a state transition or progress update is
operator-relevant/INFO; an error is always `ERROR`). Each helper takes
`const char *detail` and delegates straight to
`printerctl_log_record()` — no new detail type, no formatting logic of
its own. This matches the approved decision to keep the helper surface
minimal rather than pre-building a helper per event before any real
caller needs it.

## 10. Timestamps: `timespec_get` + `gmtime_r`, no clock abstraction

```c
struct timespec ts;
timespec_get(&ts, TIME_UTC);       /* returns TIME_UTC on success, 0 on failure */
struct tm tm_utc;
gmtime_r(&ts.tv_sec, &tm_utc);     /* NULL on failure */
```

Both are read directly in `logging.c`; there is no injectable clock
interface, no dependency-injected "now" function and no mockable time
source. `printerctl_logging_tests` validates the *shape* of the emitted
timestamp (24-character RFC3339-with-milliseconds pattern, via
`sscanf`/`isdigit` checks in `test_timestamp_format_and_structure`) rather
than mocking the clock to assert an exact value — this was an explicit
scope decision: introducing a clock abstraction purely to make timestamps
deterministic in tests would add an indirection layer with no other
consumer, for a milestone whose acceptance criterion is "logs include
timestamps," not "timestamps are injectable." `gmtime_r` (POSIX, not
ISO C) requires `_POSIX_C_SOURCE 200809L` defined before any header in
`logging.c`, the same pattern `tests/test_cli.c` already uses for
`open_memstream`/`mkstemp`.

## 11. Explicit status for every failure mode

```c
typedef enum {
    PRINTERCTL_LOG_OK = 0,
    PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
    PRINTERCTL_LOG_ERR_NOT_INITIALIZED,
    PRINTERCTL_LOG_ERR_CLOCK,
    PRINTERCTL_LOG_ERR_WRITE,
    PRINTERCTL_LOG_ERR_FLUSH
} printerctl_log_status_t;
```

- `PRINTERCTL_LOG_ERR_INVALID_ARGUMENT` — a `NULL` logger, a `NULL` stream
  passed to `printerctl_log_init()`, an out-of-range `level`/`event` enum
  value, or (defensively) a logger whose `stream` is `NULL` despite
  `initialized == true`.
- `PRINTERCTL_LOG_ERR_NOT_INITIALIZED` — `printerctl_log_record()` called
  on a logger that was never initialized, or was already shut down.
- `PRINTERCTL_LOG_ERR_CLOCK` — `timespec_get()` or `gmtime_r()` failed.
- `PRINTERCTL_LOG_ERR_WRITE` — a stream write failed (verified in tests
  with a stream reopened read-only, `test_practical_errors` in
  `tests/test_logging.c`).
- `PRINTERCTL_LOG_ERR_FLUSH` — `fflush()` failed, either from an `ERROR`
  record's immediate flush or from `printerctl_log_shutdown()`.

`detail == NULL` is explicitly *not* an error: it is treated identically
to an empty detail field, consistent with how `printerctl_cli.h`'s
optional fields already behave.

## 12. CLI integration (`printerctl_cli_run`)

No exit code changed, and no Milestone 1 human-readable prose was
removed — logging is purely additive. `printerctl_cli_run()` in
`src/cli/cli.c`:

- `--help`/`--version` return before any logger is touched: they produce
  no log lines, verified by
  `test_help_and_version_produce_no_logs` in `tests/test_cli.c`.
- A parse error, a configuration error, or the `home` safety rejection
  each already print one human-readable line to `err` (unchanged from
  Milestone 1) and now additionally call a small `log_transient_error()`
  helper: it initializes a logger scoped to `err` (verbose value does not
  matter — `ERROR` is never filtered), logs one `ERROR`/`error` record,
  and shuts down immediately. For a parse error or the `home` safety
  rejection, that record's detail is the same message just printed to
  `err`. For a configuration error, the detail is instead the generic
  string `configuration error`: the human-readable configuration-error
  message may embed the `--config` path, and that path must appear in
  `stderr` at most once (in the human-readable line), never in the
  structured TSV stream. Using a transient logger here — rather than
  delaying until a persistent one exists — is necessary because
  `config.verbose` is not resolved yet at the parse-error point, and is
  irrelevant anyway since `ERROR` records ignore verbosity.
- Once configuration resolves and the `home` safety gate passes, a
  persistent `printerctl_logger_t` is initialized against `err` with
  `config.verbose`, and the session is logged in order:
  `session_start` (INFO, no detail) → `config_summary` (INFO) →
  `command_parsed` (DEBUG, only visible under `--verbose`) → [the
  Milestone 1 "not yet implemented" line, unchanged, to `out`] →
  `command_unavailable` (WARNING, `command=<name>`) → `session_end`
  (INFO, no detail) → `printerctl_log_shutdown()`.
- `config_summary`'s detail is deliberately **not** `config.port` or the
  `--config` path. It reports `port_set`/`config_file_set` as booleans
  (derived from `request.overrides.has_port`/`has_config_path`) plus
  `baud`, `timeout_ms`, `dry_run` and `verbose`. `command_parsed` and
  `command_unavailable` report only `command=<name>`, never `print`'s
  file argument or `send`'s raw command text. See §13 for why.
- If `printerctl_log_init()` itself fails (defensively handled, though
  `err` is always a valid, non-NULL `FILE*` in practice), or any
  individual `printerctl_log_record()`/`printerctl_log_shutdown()` call
  returns a non-OK status, `printerctl_cli_run()` prints one short
  `printerctl: warning: ...` line to `err` and continues: the function's
  primary exit code and its `out`/`err` human-readable messages are never
  altered by a logging failure. Logging is observability, not a gate.

## 13. Privacy: what the CLI deliberately never logs

Per `Agents.md`'s privacy-adjacent framing and this milestone's review:

- The resolved `port` value (a device path, potentially identifying local
  hardware layout) is never logged — only whether it was set
  (`port_set=true|false`).
- The `--config` file path is never logged — only whether one was given
  (`config_file_set=true|false`). Its *contents* were already validated
  and folded into `baud`/`timeout_ms`/`dry_run`/`verbose`, which are
  logged (they are operational, not identifying).
- `print`'s file argument is never logged — only `command=print`.
- `send`'s raw command text is never logged — only `command=send`. This
  matters because `send` can carry arbitrary, potentially sensitive or
  unsafe-looking text; Milestone 1 already deferred `send`'s `G28` safety
  review to Milestone 6 (`docs/design/milestone-1-cli-config.md` §9), and
  logging its raw content now would be a second, unrelated way for
  operator-typed text to leak into a log file.

`tests/test_cli.c`'s `test_config_summary_omits_raw_port_and_config_path`
and `test_print_and_send_do_not_leak_content` assert these negatives
directly (the sensitive value is passed in, then `strstr()` on the full
captured `stderr` confirms it never appears).

## 14. Deferred until a real consumer exists

Explicitly out of scope for Milestone 2, matching the roadmap's framing of
this milestone as "Logging Foundation," not "final logging system":

- **Persistent log files.** Milestone 2 only ever writes to the `FILE*`
  given to it (`stderr` in the CLI). No file path, rotation or retention
  policy exists yet.
- **Log rotation.** Not applicable until there is a persistent file.
- **JSON Lines (or any structured) output.** TSV is the only format; the
  API is shaped so this can change later without touching call sites (see
  §2), but no second format is implemented speculatively.
- **Async/buffered/batched logging.** Every record is written
  synchronously, field-by-field, on the calling thread.
- **Locking/thread safety.** No mutex, no atomics, no thread-local
  logger. See §5.

All five are natural follow-ups once a real consumer needs them: a daemon
mode (locking, async), a log-shipping/monitoring integration (JSON,
rotation), or long-running unattended prints (persistent files). Building
any of them now, against zero real usage, would be exactly the kind of
speculative abstraction this project's engineering practice avoids.
