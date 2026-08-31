#ifndef PRINTERCTL_DAEMON_WORKER_H
#define PRINTERCTL_DAEMON_WORKER_H

#include "printerctl/daemon/ipc.h"
#include "printerctl/config.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Print worker: M13 introduced the thread body and M16 made it the owner
 * of printerctl_print_engine_t. The control context sends bounded commands
 * over `cmd_pipe`; the worker reports state/progress over `event_pipe`.
 *
 * Worker lifecycle (Agents.md SS13):
 *
 *   INITIALIZING -> READY -> STOPPING -> STOPPED
 *                \-> FAULTED
 *
 * FAULTED is also reachable from READY (a malformed/IO command read, or
 * an unrecognized command type slipping past printerctl_daemon_ipc_read_command()'s
 * own validation) -- the worker lifecycle diagram in the roadmap shows
 * only the startup-failure edge, but the required FAULT event and the
 * "worker fault reflected in the snapshot" acceptance criterion only
 * make sense if a fault can also happen mid-operation, not only during
 * initialization.
 *
 * This module never calls close() on any file descriptor: fd ownership
 * belongs entirely to the control context (see runtime.h's fd-ownership
 * note). The worker only reads `cmd_read_fd` and writes `event_write_fd`
 * until it returns.
 */

typedef enum {
    PRINTERCTL_DAEMON_WORKER_INITIALIZING = 0,
    PRINTERCTL_DAEMON_WORKER_READY,
    PRINTERCTL_DAEMON_WORKER_STOPPING,
    PRINTERCTL_DAEMON_WORKER_STOPPED,
    PRINTERCTL_DAEMON_WORKER_FAULTED
} printerctl_daemon_worker_state_t;

/*
 * Fault-injection seam for "worker initialization failure": M13 has no
 * real initialization work (no print engine yet), so this is the only
 * way to exercise that required fault path deterministically. Returns
 * true on success; on false, writes a NUL-terminated reason into
 * `out_message` (best-effort, may be empty). Production callers
 * (printerctl_daemon_runtime_start()) always pass NULL, meaning
 * "trivially succeeds" -- only tests set this.
 */
typedef bool (*printerctl_daemon_worker_init_fn)(
    void *context, char *out_message, size_t out_message_size);

typedef struct {
    int cmd_read_fd;
    int event_write_fd;
    printerctl_config_t config;
    printerctl_daemon_worker_init_fn init_hook; /* NULL => trivial success */
    void *init_context;
} printerctl_daemon_worker_args_t;

void printerctl_daemon_worker_reset_signal_requests(void);
void printerctl_daemon_worker_request_cancel_from_signal(void);
void printerctl_daemon_worker_request_power_failure_from_signal(void);

/*
 * pthread entry point (matches void *(*)(void *)): runs `init_hook` if
 * set, emits READY or FAULT accordingly, then loops reading commands
 * from `cmd_read_fd` and emitting the matching event on
 * `event_write_fd`:
 *
 *   NOP           -> COMMAND_COMPLETED(token)
 *   PRINT_START   -> accept/reject one active print; accepted prints run
 *                    through the existing print_engine
 *   PRINT_PAUSE/
 *   PRINT_RESUME/
 *   PRINT_CANCEL  -> applied while a print is active, rejected otherwise
 *   SHUTDOWN      -> STOPPING(token), cleanup, STOPPED(token), return
 *   cmd_pipe EOF  -> STOPPING(0), STOPPED(0), return (orderly: the
 *                    control side went away without an explicit
 *                    SHUTDOWN -- still a clean stop, not a fault)
 *   malformed/IO  -> FAULT(token or 0), return
 *
 * `arg` must point to a live printerctl_daemon_worker_args_t for the
 * duration of the call (the caller -- printerctl_daemon_runtime_start()
 * -- keeps it embedded in the runtime struct for exactly that reason).
 * Always returns NULL.
 */
void *printerctl_daemon_worker_main(void *arg);

#endif
