#ifndef PRINTERCTL_LOGGING_H
#define PRINTERCTL_LOGGING_H

#include <stdbool.h>
#include <stdio.h>

/*
 * Structured, timestamped logging for printerctl.
 *
 * The on-stream format is plain TSV, exactly one physical line per record:
 *
 *   <RFC3339 UTC timestamp with milliseconds><TAB><LEVEL><TAB><event><TAB><detail>\n
 *
 * The format is an implementation detail hidden behind this API; callers
 * never assemble a line themselves and must not depend on its exact shape
 * beyond "one line, four tab-separated fields". `detail` is opaque text,
 * not a second key=value grammar: call sites may write `key=value` pairs
 * inside it for human readability, but this module never parses or
 * validates that structure.
 *
 * This module is single-threaded and unbuffered beyond the underlying
 * FILE* stream: every field of a record is written directly to the stream
 * given at printerctl_log_init() time, with no internal heap allocation
 * and no internal line buffer. Nothing here makes a single record's write
 * atomic with respect to other writers on the same stream, and nothing
 * here is safe to call concurrently from more than one thread against the
 * same printerctl_logger_t. Revisit both points if a multi-threaded daemon
 * mode is ever built (see roadmap.md's post-MVP backlog).
 */

typedef enum {
    PRINTERCTL_LOG_DEBUG = 0,
    PRINTERCTL_LOG_INFO,
    PRINTERCTL_LOG_WARNING,
    PRINTERCTL_LOG_ERROR
} printerctl_log_level_t;

/*
 * Fixed Milestone 2 event taxonomy. Extending it is a deliberate, reviewed
 * change to this enum and to the name table in logging.c, not something a
 * caller can do with a free-form string.
 */
typedef enum {
    PRINTERCTL_LOG_EVENT_SESSION_START = 0,
    PRINTERCTL_LOG_EVENT_SESSION_END,
    PRINTERCTL_LOG_EVENT_CONFIG_SUMMARY,
    PRINTERCTL_LOG_EVENT_COMMAND_PARSED,
    PRINTERCTL_LOG_EVENT_COMMAND_UNAVAILABLE,
    PRINTERCTL_LOG_EVENT_SERIAL_TX,
    PRINTERCTL_LOG_EVENT_SERIAL_RX,
    PRINTERCTL_LOG_EVENT_STATE_TRANSITION,
    PRINTERCTL_LOG_EVENT_PROGRESS,
    PRINTERCTL_LOG_EVENT_ERROR,
    PRINTERCTL_LOG_EVENT_WARNING
} printerctl_log_event_t;

typedef enum {
    PRINTERCTL_LOG_OK = 0,
    PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
    PRINTERCTL_LOG_ERR_NOT_INITIALIZED,
    PRINTERCTL_LOG_ERR_CLOCK,
    PRINTERCTL_LOG_ERR_WRITE,
    PRINTERCTL_LOG_ERR_FLUSH
} printerctl_log_status_t;

/*
 * `stream` is borrowed, never owned: printerctl_log_shutdown() flushes it
 * but never closes it. The caller is responsible for the stream's lifetime
 * both before printerctl_log_init() and after shutdown.
 */
typedef struct {
    FILE *stream;
    bool verbose;
    bool initialized;
} printerctl_logger_t;

/*
 * Initializes *logger to write records to `stream`. Always overwrites the
 * full contents of *logger, on both success and failure, so callers never
 * need to zero it first.
 *
 * `logger` must be non-NULL (returns PRINTERCTL_LOG_ERR_INVALID_ARGUMENT
 * otherwise, with nothing written). `stream` must be non-NULL; passing
 * NULL is a usage error (PRINTERCTL_LOG_ERR_INVALID_ARGUMENT) and leaves
 * *logger fully initialized to a well-defined, not-initialized state
 * rather than partially set.
 *
 * `verbose` controls whether PRINTERCTL_LOG_DEBUG records are written;
 * when false, printerctl_log_record() silently discards DEBUG records and
 * returns PRINTERCTL_LOG_OK (filtering is not an error).
 */
printerctl_log_status_t printerctl_log_init(printerctl_logger_t *logger, FILE *stream, bool verbose);

/*
 * Flushes logger->stream (fflush only, never fclose: the stream is not
 * owned by the logger) and marks *logger uninitialized. Idempotent:
 * calling this again on an already-shut-down (or never-initialized)
 * logger is a no-op that returns PRINTERCTL_LOG_OK without touching any
 * stream. Returns PRINTERCTL_LOG_ERR_INVALID_ARGUMENT for a NULL logger,
 * or PRINTERCTL_LOG_ERR_FLUSH if fflush() fails; either way *logger is
 * left marked uninitialized, so a failed shutdown can never be retried
 * into a double flush.
 */
printerctl_log_status_t printerctl_log_shutdown(printerctl_logger_t *logger);

/*
 * Writes one physical TSV log line: timestamp, level, event and escaped
 * detail. `detail` is opaque text; NULL is treated the same as "" (an
 * empty detail field). Escaping is centralized here and is never the
 * caller's responsibility: backslash, tab, CR, LF and other C0 control
 * bytes plus DEL (0x7F) become visible escape sequences (`\\`, `\t`,
 * `\r`, `\n`, `\xHH`); bytes >= 0x80 are passed through unchanged.
 *
 * Returns PRINTERCTL_LOG_OK for a filtered PRINTERCTL_LOG_DEBUG record
 * (see printerctl_log_init()). Returns
 * PRINTERCTL_LOG_ERR_INVALID_ARGUMENT for a NULL logger, an invalid
 * `level` or `event` enum value, or a logger whose stream is NULL;
 * PRINTERCTL_LOG_ERR_NOT_INITIALIZED if the logger was never initialized
 * or was already shut down; PRINTERCTL_LOG_ERR_CLOCK if the current time
 * cannot be read; PRINTERCTL_LOG_ERR_WRITE on a stream write failure.
 *
 * A PRINTERCTL_LOG_ERROR record additionally fflush()es immediately once
 * written, and a flush failure there is reported as
 * PRINTERCTL_LOG_ERR_FLUSH even though the record itself was already
 * written successfully. Every other level is left to the stream's normal
 * buffering and is only guaranteed flushed by printerctl_log_shutdown().
 */
printerctl_log_status_t printerctl_log_record(printerctl_logger_t *logger,
                                                printerctl_log_level_t level,
                                                printerctl_log_event_t event,
                                                const char *detail);

/*
 * Minimal helpers for the event shapes future modules (serial, printer
 * session) will need repeatedly. Each is a thin wrapper over
 * printerctl_log_record() with a fixed level/event pair; `detail` is
 * passed through unchanged and is still opaque text — these helpers do
 * not introduce any new detail format or type. Every other fixed event
 * (session_start, session_end, config_summary, command_parsed,
 * command_unavailable, warning) is logged by calling
 * printerctl_log_record() directly; Milestone 2 adds no helper for them.
 */
printerctl_log_status_t printerctl_log_serial_tx(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_serial_rx(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_state_transition(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_progress(printerctl_logger_t *logger, const char *detail);
printerctl_log_status_t printerctl_log_error(printerctl_logger_t *logger, const char *detail);

#endif
