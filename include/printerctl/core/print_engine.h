#ifndef PRINTERCTL_CORE_PRINT_ENGINE_H
#define PRINTERCTL_CORE_PRINT_ENGINE_H

#include "printerctl/config.h"
#include "printerctl/gcode.h"
#include "printerctl/logging.h"
#include "printerctl/printer.h"
#include "printerctl/record.h"

#include <signal.h>
#include <stdbool.h>

/*
 * Milestone 12 print engine: the application-level lifecycle that used
 * to live inside the CLI's run_print_command() (connect, publish the
 * active-print marker, stream the file, write an interrupted-session
 * record, remove the marker, disconnect), extracted so a future daemon
 * can become a second client adapter over the same core instead of a
 * second copy of that lifecycle.
 *
 * This module reuses printerctl_printer_print_file() unchanged for the
 * ACK-driven streaming itself (architecture.md §7.2); it does not
 * reimplement or replace it. It knows nothing about argv, parsed CLI
 * command structs, terminal rendering, TUI, HTTP, JSON or the future
 * Unix socket protocol -- every operator-facing message is surfaced as
 * an event instead of fprintf(), so a client adapter (today: the CLI;
 * later: the daemon) decides how to present it.
 *
 * Control model: printerctl_printer_print_file()'s on_control callback
 * is polled between acknowledged commands (and, while paused, roughly
 * every 100 ms -- see printer.h). This engine owns that polling
 * decision internally through four `volatile sig_atomic_t` flags on
 * printerctl_print_engine_t, set by the printerctl_print_engine_request_*()
 * functions below -- each one a single assignment, safe to call directly
 * from a POSIX signal handler, exactly like the flags run_print_command()
 * used to keep at file scope. The CLI still owns `sigaction()`; this
 * module never installs a signal handler itself (Agents.md safety
 * policy: signals are a client-transport detail, not core-control API).
 *
 * Same discipline as every other module in this codebase: no heap,
 * stack-value engine, output parameters always fully initialized on
 * success and failure, idempotent disconnect. printerctl_print_engine_run()
 * is a single blocking call -- no tick()/poll() API. Forcing the
 * existing blocking streamer into a non-blocking shape before a daemon
 * actually needs one would add complexity with no present consumer (see
 * docs/design/milestone-12-print-engine.md §8).
 */

/*
 * Every event a print can produce while printerctl_print_engine_run()
 * is running, in the order a single print emits them:
 *
 *   STARTED               emitted once, right before streaming begins.
 *   MARKER_WARNING        the active-print marker could not be
 *                         published (best-effort; the print still runs
 *                         -- see printerctl_print_engine_connect()).
 *   PROGRESS              emitted after every acknowledged command,
 *                         exactly as often as printerctl_printer_print_file()
 *                         invokes its own on_progress callback (no
 *                         percent-change deduplication here -- a
 *                         terminal renderer wants at most one line per
 *                         whole percent, but a live subscriber may want
 *                         every acknowledgement; that filtering belongs
 *                         to the client, not the core).
 *   PAUSED / RESUMED      emitted once per edge (pause requested while
 *                         running / resumed while paused), never
 *                         repeated while the state holds.
 *   CANCELLING_POWER_FAILURE
 *                         emitted once, the first time a power-failure
 *                         cancellation is observed (edge-triggered, the
 *                         same way as PAUSED/RESUMED -- a deliberate
 *                         simplification of the CLI's previous
 *                         behavior, which re-printed this notice on
 *                         every control poll for as long as the signal
 *                         stayed set; see the Milestone 12 design doc).
 *   COMPLETED / CANCELLED / FAILED
 *                         emitted exactly once, after streaming stops,
 *                         carrying the same summary/error also returned
 *                         by printerctl_print_engine_run() -- the event
 *                         exists so a non-CLI client never needs
 *                         fprintf()-shaped code to learn the outcome.
 *   RECORD_WRITTEN / RECORD_FAILED
 *                         emitted at most once, only when the print did
 *                         not complete and had already started
 *                         (mirrors the interrupted-session record rule
 *                         in record.h).
 */
typedef enum {
    PRINTERCTL_PRINT_EVENT_STARTED = 0,
    PRINTERCTL_PRINT_EVENT_MARKER_WARNING,
    PRINTERCTL_PRINT_EVENT_PROGRESS,
    PRINTERCTL_PRINT_EVENT_PAUSED,
    PRINTERCTL_PRINT_EVENT_RESUMED,
    PRINTERCTL_PRINT_EVENT_CANCELLING_POWER_FAILURE,
    PRINTERCTL_PRINT_EVENT_COMPLETED,
    PRINTERCTL_PRINT_EVENT_CANCELLED,
    PRINTERCTL_PRINT_EVENT_FAILED,
    PRINTERCTL_PRINT_EVENT_RECORD_WRITTEN,
    PRINTERCTL_PRINT_EVENT_RECORD_FAILED
} printerctl_print_event_type_t;

/*
 * Only the field(s) documented for `type` above are valid; the rest are
 * NULL. Every pointer borrows engine-owned or stack storage and is
 * valid only for the duration of the callback, exactly like
 * printerctl_printer_line_fn's contract in printer.h -- copy anything
 * that must outlive the call.
 */
typedef struct {
    printerctl_print_event_type_t type;
    const char *path;                          /* STARTED: file being printed.
                                                  * RECORD_WRITTEN: record path. */
    const char *message;                       /* MARKER_WARNING, RECORD_FAILED. */
    const printerctl_gcode_result_t *progress;  /* PROGRESS. */
    const printerctl_print_summary_t *summary;  /* COMPLETED, CANCELLED, FAILED. */
    const printerctl_printer_error_t *error;    /* FAILED. */
} printerctl_print_event_t;

typedef void (*printerctl_print_engine_event_fn)(
    const printerctl_print_event_t *event, void *context);

typedef struct {
    printerctl_printer_session_t printer;
    printerctl_logger_t *logger;
    bool logger_ready;
    printerctl_print_engine_event_fn on_event;
    void *event_context;

    char state_dir[PRINTERCTL_RECORD_DIR_MAX];
    bool state_dir_resolved;
    bool marker_active;
    char serial_device[PRINTERCTL_RECORD_DEVICE_MAX];

    bool paused;
    bool cancel_power_failure_notified;
    volatile sig_atomic_t pause_requested;
    volatile sig_atomic_t resume_requested;
    volatile sig_atomic_t cancel_requested;
    volatile sig_atomic_t cancel_power_failure_requested;
} printerctl_print_engine_t;

/*
 * Opens the serial port (via printerctl_printer_connect(), so the same
 * dry-run refusal and error semantics apply) and, best-effort, publishes
 * the active-print marker so `printerctl power low-battery` can find
 * this process. A marker failure never fails connect(): it emits
 * PRINTERCTL_PRINT_EVENT_MARKER_WARNING through `on_event` and the print
 * proceeds without one, exactly as run_print_command() used to warn and
 * continue.
 *
 * Always overwrites the full contents of *engine, on both success and
 * failure. `engine`, `config`, `out_error` must be non-NULL; `on_event`
 * may be NULL to disable event delivery entirely. `logger` may be NULL
 * (logger_ready is then ignored).
 */
printerctl_printer_status_t printerctl_print_engine_connect(
    printerctl_print_engine_t *engine,
    const printerctl_config_t *config,
    printerctl_logger_t *logger,
    bool logger_ready,
    printerctl_print_engine_event_fn on_event,
    void *event_context,
    printerctl_printer_error_t *out_error);

/*
 * Streams the G-code file at `path` to completion, cancellation or
 * failure: emits STARTED, forwards to printerctl_printer_print_file()
 * with this engine's internal progress/control callbacks (see the
 * event enum above), writes an interrupted-session record when the
 * print did not complete and had already started, removes the
 * active-print marker, and emits the matching terminal event.
 *
 * Returns exactly what printerctl_printer_print_file() returned, and
 * fills *out_summary and *out_error the same way -- this engine reuses
 * that status/error vocabulary rather than inventing a parallel one, so a
 * caller that already knows printer.h's status codes (exit-code
 * mapping, failure reporting) does not need a second mapping.
 *
 * `engine` must be connected; `path`, `out_summary` and `out_error` must
 * be non-NULL. The session remains connected afterwards; the caller
 * disconnects via printerctl_print_engine_disconnect().
 */
printerctl_printer_status_t printerctl_print_engine_run(
    printerctl_print_engine_t *engine,
    const char *path,
    printerctl_print_summary_t *out_summary,
    printerctl_printer_error_t *out_error);

/*
 * Async-signal-safe control requests: each is a single assignment to a
 * `volatile sig_atomic_t` field on `engine`, safe to call directly from
 * a POSIX signal handler. Consulted by printerctl_print_engine_run()
 * between acknowledged commands, with the same priority
 * run_print_command() used to enforce: an unresolved power-failure
 * cancellation always wins over cancel, which always wins over an
 * unresolved pause. `engine` must be non-NULL (not otherwise validated,
 * to keep these signal-handler-safe).
 */
void printerctl_print_engine_request_pause(printerctl_print_engine_t *engine);
void printerctl_print_engine_request_resume(printerctl_print_engine_t *engine);
void printerctl_print_engine_request_cancel(printerctl_print_engine_t *engine);
void printerctl_print_engine_request_cancel_power_failure(printerctl_print_engine_t *engine);

/*
 * Removes the active-print marker if it is still active (idempotent)
 * and closes the serial port via printerctl_printer_disconnect(). Safe
 * to call after a failed connect() or a never-connected engine, same as
 * the underlying printer session.
 */
printerctl_printer_status_t printerctl_print_engine_disconnect(
    printerctl_print_engine_t *engine,
    printerctl_printer_error_t *out_error);

#endif
