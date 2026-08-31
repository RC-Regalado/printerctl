#ifndef PRINTERCTL_DAEMON_RUNTIME_H
#define PRINTERCTL_DAEMON_RUNTIME_H

#include "printerctl/daemon/ipc.h"
#include "printerctl/daemon/worker.h"
#include "printerctl/config.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Control context for printerctld's runtime lifecycle, its one print worker
 * thread, the two internal pipes connecting them, and the published runtime
 * snapshot. M13 introduced the hardware-independent boundary; M16 connects
 * worker-owned print_engine execution through that boundary.
 *
 * ## fd ownership (Agents.md SS19)
 *
 * pthread_create() shares one process-wide fd table -- unlike fork(),
 * it does not give the worker thread its own duplicated copy of each
 * fd. Both `cmd_pipe[1]` (control's write end) and `event_pipe[1]`
 * (the worker's write end) therefore have exactly one fd-table entry
 * each, referenced by whichever thread was handed that integer. Given
 * that, "each side closes the end it doesn't use" -- the classic
 * fork()-based pipe pattern -- would let one thread close a number the
 * other still relies on, or race a fd-table slot getting reused by an
 * unrelated open() elsewhere in the process.
 *
 * printerctl_daemon_runtime_t resolves this with a single rule: **the
 * control context is the sole owner and sole closer of all four fds,
 * always, at exactly one deterministic point per code path** (a failed
 * pipe()/pthread_create(), a failed startup handshake, or after
 * pthread_join() on shutdown). The worker (src/daemon/worker.c) never
 * calls close() on anything.
 *
 * One consequence: a worker thread cannot signal "I'm gone" through
 * pipe EOF the way a crashed *process* would (the OS only closes an
 * fd when its single remaining reference -- control's -- is closed).
 * Normal and faulted worker termination are therefore detected through
 * the worker's own well-formed STOPPED/FAULT event, with
 * pthread_join() as the hard confirmation that the OS thread is truly
 * gone -- not through reading EOF on event_pipe. `event_pipe` EOF/read
 * failure is reserved for a different, rarer case: the channel itself
 * became unusable (e.g. a bug, or a test closing a fd directly), which
 * this runtime treats as total loss of observability
 * (`daemon_state = PRINTERCTL_DAEMON_FAILED`) -- distinct from a
 * well-formed FAULT *event*, which only marks `worker_state =
 * PRINTERCTL_DAEMON_WORKER_FAULTED` and leaves `daemon_state` alone
 * (see printerctl_daemon_runtime_wait_event()). A worker that crashes
 * outright, skipping its own event emission, is not distinguishable
 * from "still working" by this design -- an accepted, documented limit
 * of thread-based (not process-based) workers, not a silent gap.
 *
 * ## daemon_state vs. worker_state
 *
 * These are deliberately separate dimensions (roadmap M13 goal): a
 * mid-run worker FAULT leaves `daemon_state == PRINTERCTL_DAEMON_RUNNING`
 * while `worker_state == PRINTERCTL_DAEMON_WORKER_FAULTED` -- the
 * control context itself is still alive and able to report this, and
 * nothing here decides on its own to shut down or restart the worker
 * (no automatic worker restart, ever). Only
 * printerctl_daemon_runtime_start()/_shutdown() ever change
 * `daemon_state`; printerctl_daemon_runtime_wait_event() only changes
 * it on the channel-loss case described above.
 *
 * ## Ownership summary (Agents.md SS11)
 *
 * Control context owns: daemon/runtime lifecycle, the published
 * snapshot, worker availability, command dispatch, all four fds. It
 * never mutates anything worker-owned directly. The worker owns: its
 * own internal state machine and (from M16 on) the print engine
 * instance; it never mutates the published snapshot directly -- only
 * through events the control context applies.
 */

typedef enum {
    PRINTERCTL_DAEMON_STARTING = 0,
    PRINTERCTL_DAEMON_RUNNING,
    PRINTERCTL_DAEMON_STOPPING,
    PRINTERCTL_DAEMON_STOPPED,
    PRINTERCTL_DAEMON_FAILED
} printerctl_daemon_state_t;

#define PRINTERCTL_DAEMON_ERROR_MAX 256

/*
 * The client-facing published state (roadmap SS11, Agents.md SS14).
 * Written only by the control context, from printerctl_daemon_runtime_start(),
 * _shutdown() and _wait_event() -- never by the worker thread.
 */
typedef struct {
    printerctl_daemon_state_t daemon_state;
    printerctl_daemon_worker_state_t worker_state;
    uint8_t execution_state;
    uint8_t progress_percent;
    uint32_t commands_sent;
    uint32_t commands_acknowledged;
    uint32_t source_position;
    uint16_t failure_reason;
    char last_runtime_error[PRINTERCTL_DAEMON_ERROR_MAX]; /* empty when none recorded */
} printerctl_daemon_snapshot_t;

typedef enum {
    PRINTERCTL_DAEMON_OK = 0,
    PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
    PRINTERCTL_DAEMON_ERR_ALREADY,       /* shutdown() called again on a runtime
                                           * that already finished shutting down */
    PRINTERCTL_DAEMON_ERR_PIPE,          /* pipe() failed */
    PRINTERCTL_DAEMON_ERR_THREAD,        /* pthread_create() failed */
    PRINTERCTL_DAEMON_ERR_WORKER_FAULT,  /* the worker did not reach READY during
                                           * the startup handshake */
    PRINTERCTL_DAEMON_ERR_IO             /* the event channel was lost (EOF,
                                           * a malformed message, or a read
                                           * failure) */
} printerctl_daemon_status_t;

typedef struct {
    printerctl_daemon_status_t status;
    char message[PRINTERCTL_DAEMON_ERROR_MAX];
} printerctl_daemon_error_t;

typedef struct {
    int cmd_pipe[2];   /* [0] worker reads commands; [1] control writes them */
    int event_pipe[2]; /* [0] control reads events; [1] worker writes them */
    pthread_t worker_thread;
    bool worker_thread_started;
    printerctl_daemon_worker_args_t worker_args;
    printerctl_daemon_snapshot_t snapshot;
    uint32_t next_token; /* single-threaded control side in M13: plain increment,
                           * no atomics needed. Starts at 1; 0 is never assigned,
                           * reserved for "not correlated to a command" (see ipc.h). */
    bool shutdown_started;
} printerctl_daemon_runtime_t;

/*
 * Zeroes *runtime, creates cmd_pipe and event_pipe, spawns the worker
 * thread, then blocks for the startup handshake: the first event the
 * worker ever sends must be READY (success) or FAULT (initialization
 * failure) -- daemon_state only becomes PRINTERCTL_DAEMON_RUNNING once
 * READY was actually observed, never merely because pthread_create()
 * returned 0 (Agents.md SS13).
 *
 * Every failure branch leaves no fd and no thread leaked:
 *   - cmd_pipe fails to create: nothing else was touched.
 *   - event_pipe fails to create: cmd_pipe is closed first.
 *   - pthread_create() fails: all four fds are closed.
 *   - the worker never reaches READY (FAULT, or a malformed/EOF/IO
 *     event instead): the worker thread is still joined, then all four
 *     fds are closed.
 * In every failure branch, daemon_state ends at PRINTERCTL_DAEMON_FAILED
 * and *out_error describes what failed.
 *
 * `runtime` and `out_error` must be non-NULL. Like printerctl_printer_connect(),
 * start() unconditionally overwrites *runtime -- it does not detect
 * reuse of an already-running instance, the same convention
 * printer.h's connect() already uses. Declare a fresh
 * printerctl_daemon_runtime_t for each start()/shutdown() cycle (the
 * required tests do exactly this).
 *
 * `worker_init_hook`/`worker_init_context` are forwarded verbatim into
 * printerctl_daemon_worker_args_t (worker.h) -- production callers
 * always pass NULL/NULL, meaning "the worker's initialization trivially
 * succeeds" (M13 has no real work to do there yet). Tests pass a
 * failing hook to exercise the worker-initialization-fault path
 * deterministically; this is the only way M13 can test that path, since
 * nothing real is initialized yet for a real failure to come from.
 */
printerctl_daemon_status_t printerctl_daemon_runtime_start(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_worker_init_fn worker_init_hook,
    void *worker_init_context,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_start_with_config(
    printerctl_daemon_runtime_t *runtime,
    const printerctl_config_t *config,
    printerctl_daemon_worker_init_fn worker_init_hook,
    void *worker_init_context,
    printerctl_daemon_error_t *out_error);

/*
 * Sends a NOP command and returns immediately with the token assigned
 * to it via *out_token -- it does NOT wait for the matching
 * COMMAND_COMPLETED; that is a separate printerctl_daemon_runtime_wait_event()
 * call, so that command dispatch and event reception stay the two
 * distinct owned responsibilities the roadmap describes them as.
 */
printerctl_daemon_status_t printerctl_daemon_runtime_send_nop(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_start(
    printerctl_daemon_runtime_t *runtime,
    const char *path,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_pause(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_resume(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_cancel(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_cancel_power_failure(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_wait_command(
    printerctl_daemon_runtime_t *runtime,
    uint32_t token,
    printerctl_daemon_event_t *out_event,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_drain_events(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_error_t *out_error);

/*
 * Blocks for exactly one event_pipe message and returns it via
 * *out_event, so a caller (or a test) can assert its type/token
 * directly. Also applies it to the published snapshot:
 *
 *   READY/STOPPING/STOPPED -> worker_state updated to match
 *   COMMAND_COMPLETED      -> no state change (the caller correlates
 *                             the token itself)
 *   FAULT                  -> worker_state = FAULTED,
 *                             last_runtime_error = event detail;
 *                             daemon_state is left untouched (see the
 *                             header comment above)
 *
 * On PRINTERCTL_DAEMON_IPC_EOF/_ERR_TRUNCATED/_ERR_MALFORMED/_ERR_IO
 * from the underlying read -- the event *channel* itself failed, not a
 * well-formed FAULT event -- this sets daemon_state = PRINTERCTL_DAEMON_FAILED,
 * records the reason in last_runtime_error, and returns
 * PRINTERCTL_DAEMON_ERR_IO; *out_event is zeroed in that case.
 */
printerctl_daemon_status_t printerctl_daemon_runtime_wait_event(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_event_t *out_event,
    printerctl_daemon_error_t *out_error);

/*
 * Sends SHUTDOWN, then repeatedly waits for events (discarding any
 * COMMAND_COMPLETED left over from a NOP the caller never drained)
 * until STOPPED arrives or the event channel fails, pthread_join()s the
 * worker thread, closes all four fds, and sets
 * daemon_state = PRINTERCTL_DAEMON_STOPPED. Idempotent guard: a second
 * call on a runtime that already finished shutting down returns
 * PRINTERCTL_DAEMON_ERR_ALREADY without touching anything (no
 * double-close, no double-join).
 */
printerctl_daemon_status_t printerctl_daemon_runtime_shutdown(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_error_t *out_error);

/* Copies the current published snapshot into *out_snapshot. */
void printerctl_daemon_runtime_snapshot(
    const printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_snapshot_t *out_snapshot);

#endif
