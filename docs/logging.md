---
title: Logging
status: Milestone 2 (Logging Foundation)
date: 2026-07-05
tags: [printerctl, logging]
---

# Logging

Source of truth: [[Agents]], [[architecture]], [[roadmap]]. Design
rationale: [[milestone-2-logging|docs/design/milestone-2-logging.md]].
Milestone report: [[milestone-2|docs/milestones/milestone-2.md]].

## Module

`printerctl_logging` (`include/printerctl/logging.h`,
`src/logging/logging.c`) is an independent static library with no
dependency on `cli` or `config`. `printerctl_cli` links it, not the other
way around.

## Format

Every log record is exactly one physical line, four tab-separated fields:

```text
<RFC3339 UTC timestamp with milliseconds>	<LEVEL>	<event>	<detail>
```

Example:

```text
2026-07-05T20:10:06.813Z	INFO	config_summary	port_set=false config_file_set=false baud=115200 timeout_ms=5000 dry_run=false verbose=false
```

This exact layout is an implementation detail of `logging.c`; callers
never assemble it themselves, only ever call
`printerctl_log_record()` or one of the typed helpers below. `detail` is
opaque text: call sites may write `key=value` pairs inside it for
readability (as the CLI does above), but it is not a second, contractual
grammar that a future consumer may depend on parsing.

### Escaping

`detail` is escaped centrally, inside `logging.c`, never by the caller:

| Input | Output |
|---|---|
| `\` | `\\` |
| tab | `\t` |
| CR | `\r` |
| LF | `\n` |
| other control byte or DEL (`0x7F`) | `\xHH` |
| byte `>= 0x80` | unchanged |

This guarantees a record is always exactly one physical line and never
gains a stray tab field, no matter what text (a `send` argument, a
printer response, a file path) ends up in `detail`.

## Levels

`DEBUG`, `INFO`, `WARNING`, `ERROR`.

- `DEBUG` records are written only when the logger was initialized with
  `verbose = true`; otherwise they are silently discarded (this is not an
  error).
- `ERROR` records are flushed to the stream immediately.
- `INFO`/`WARNING` records rely on normal stream buffering and are
  guaranteed flushed only once `printerctl_log_shutdown()` runs.

## Events (Milestone 2 taxonomy)

`session_start`, `session_end`, `config_summary`, `command_parsed`,
`command_unavailable`, `serial_tx`, `serial_rx`, `state_transition`,
`progress`, `error`, `warning`.

The first five are used today by the CLI. `serial_tx`/`serial_rx`,
`state_transition` and `progress` exist so the serial, Marlin and printer
session modules (later milestones) have a defined event to log into
without inventing an ad hoc string.

## API

```c
printerctl_log_status_t printerctl_log_init(printerctl_logger_t *logger, FILE *stream, bool verbose);
printerctl_log_status_t printerctl_log_shutdown(printerctl_logger_t *logger);

printerctl_log_status_t printerctl_log_record(printerctl_logger_t *logger,
                                                printerctl_log_level_t level,
                                                printerctl_log_event_t event,
                                                const char *detail);

printerctl_log_status_t printerctl_log_serial_tx(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_serial_rx(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_state_transition(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_progress(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_error(printerctl_logger_t *logger, const char *detail);
```

`stream` is borrowed, never owned or closed by the logger.
`printerctl_log_init()` overwrites `*logger` fully on both success and
failure. `printerctl_log_shutdown()` flushes (never closes) the stream
and is idempotent — safe to call more than once, or on a logger that was
never initialized.

Every call returns an explicit `printerctl_log_status_t`
(`PRINTERCTL_LOG_OK` or one of five specific error values); there are no
silent failures. See `logging.h` for the full contract of each function
and each error value.

## Threading and durability

The module is single-threaded and unbuffered beyond the underlying
`FILE*`: no internal heap allocation, no internal line buffer, no locking.
Nothing here is safe to call concurrently against the same logger from
more than one thread, and a record's write is not atomic with respect to
other writers on the same stream. This is acceptable for the current
single-threaded CLI process; it must be revisited before any
multi-threaded daemon mode is built.

## CLI behavior

`printerctl` currently only ever logs to `stderr`, using the same stream
`main()` already passes to `printerctl_cli_run()` for human-readable
error output:

- `--help` and `--version` produce no log output at all.
- A usage error or a configuration error each print their existing
  human-readable message and additionally record one `ERROR`/`error`
  log line before logging is even initialized. For a usage error, that
  line's detail is the same text as the human-readable message. For a
  configuration error, the detail is the generic string
  `configuration error` instead, because the human-readable message may
  embed the `--config` path. (`home` no longer has a safety rejection of
  its own now that min end-stops are enabled on all three axes — see
  Agents.md's Home amendment; it resolves and executes like any other
  command below.)
- Every other valid, resolved command logs (in order) `session_start`,
  `config_summary`, `command_parsed` (`DEBUG`, only visible with
  `--verbose`), the command's own outcome record(s) — an `ERROR` line if
  it failed, or a `state_transition`/`progress` record on success — then
  `session_end`. `command_unavailable` (`WARNING`) is reserved for a
  parsed command with no execution path at all; with every command now
  implemented, it is not reached in ordinary use.

`config_summary` never logs the raw `--port` value or the `--config`
file path — only whether each was set (`port_set`, `config_file_set`),
plus `baud`, `timeout_ms`, `dry_run` and `verbose`. `command_parsed` and
`command_unavailable` only ever log `command=<name>`, never `print`'s
file argument or `send`'s raw command text.

If logging itself fails for any reason, `printerctl` prints one short
`printerctl: warning: ...` line to `stderr` and continues with its
original exit code and human-readable output unchanged — a logging
failure never hides or replaces the primary result.

## What this milestone does not do

No persistent log file, no rotation, no JSON/structured output, no
async/batched writes, no locking. All are deferred until a real consumer
(a daemon, a log shipper, an unattended long print) exists to justify
them; see [[milestone-2-logging]] §14 for the rationale.
