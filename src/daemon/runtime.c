#include "printerctl/daemon/runtime.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void set_error(printerctl_daemon_error_t *out_error, printerctl_daemon_status_t status,
                       const char *format, ...)
{
    out_error->status = status;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static void record_runtime_error(printerctl_daemon_runtime_t *runtime, const char *message)
{
    snprintf(runtime->snapshot.last_runtime_error, sizeof(runtime->snapshot.last_runtime_error),
             "%s", message);
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_all_pipes(printerctl_daemon_runtime_t *runtime)
{
    close_fd(&runtime->cmd_pipe[0]);
    close_fd(&runtime->cmd_pipe[1]);
    close_fd(&runtime->event_pipe[0]);
    close_fd(&runtime->event_pipe[1]);
}

/*
 * Marks resource cleanup as done/in-progress: set on every terminal
 * path (a failed start(), or a completed shutdown()) so a later
 * shutdown() call -- however it got there -- never attempts a second
 * pthread_join() or a second round of closes. See runtime.h's
 * fd-ownership note.
 */
static void mark_terminal(printerctl_daemon_runtime_t *runtime)
{
    runtime->shutdown_started = true;
}

printerctl_daemon_status_t printerctl_daemon_runtime_start(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_worker_init_fn worker_init_hook,
    void *worker_init_context,
    printerctl_daemon_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (runtime == NULL) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT, "runtime must not be NULL");
        return out_error->status;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->cmd_pipe[0] = -1;
    runtime->cmd_pipe[1] = -1;
    runtime->event_pipe[0] = -1;
    runtime->event_pipe[1] = -1;
    runtime->next_token = 1; /* 0 is reserved for "not correlated", see ipc.h */
    runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_STARTING;
    runtime->snapshot.worker_state = PRINTERCTL_DAEMON_WORKER_INITIALIZING;

    if (pipe(runtime->cmd_pipe) != 0) {
        int saved_errno = errno;
        runtime->cmd_pipe[0] = -1;
        runtime->cmd_pipe[1] = -1;
        runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_FAILED;
        set_error(out_error, PRINTERCTL_DAEMON_ERR_PIPE, "cmd_pipe: %s", strerror(saved_errno));
        record_runtime_error(runtime, out_error->message);
        mark_terminal(runtime);
        return out_error->status;
    }

    if (pipe(runtime->event_pipe) != 0) {
        int saved_errno = errno;
        runtime->event_pipe[0] = -1;
        runtime->event_pipe[1] = -1;
        close_all_pipes(runtime); /* cmd_pipe must not leak */
        runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_FAILED;
        set_error(out_error, PRINTERCTL_DAEMON_ERR_PIPE, "event_pipe: %s", strerror(saved_errno));
        record_runtime_error(runtime, out_error->message);
        mark_terminal(runtime);
        return out_error->status;
    }

    runtime->worker_args.cmd_read_fd = runtime->cmd_pipe[0];
    runtime->worker_args.event_write_fd = runtime->event_pipe[1];
    runtime->worker_args.init_hook = worker_init_hook;
    runtime->worker_args.init_context = worker_init_context;

    int rc = pthread_create(&runtime->worker_thread, NULL, printerctl_daemon_worker_main,
                             &runtime->worker_args);
    if (rc != 0) {
        close_all_pipes(runtime); /* the thread never started; nothing to join */
        runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_FAILED;
        set_error(out_error, PRINTERCTL_DAEMON_ERR_THREAD, "pthread_create: %s", strerror(rc));
        record_runtime_error(runtime, out_error->message);
        mark_terminal(runtime);
        return out_error->status;
    }
    runtime->worker_thread_started = true;

    /* Startup handshake (Agents.md SS13): daemon_state may only become
     * RUNNING once the worker's own READY event was actually observed. */
    printerctl_daemon_event_t event;
    printerctl_daemon_error_t wait_error;
    printerctl_daemon_status_t status =
        printerctl_daemon_runtime_wait_event(runtime, &event, &wait_error);

    if (status != PRINTERCTL_DAEMON_OK) {
        /* wait_event() already set daemon_state = FAILED and recorded
         * last_runtime_error. */
        pthread_join(runtime->worker_thread, NULL);
        close_all_pipes(runtime);
        mark_terminal(runtime);
        *out_error = wait_error;
        return out_error->status;
    }

    if (event.type != PRINTERCTL_DAEMON_EVENT_READY) {
        pthread_join(runtime->worker_thread, NULL);
        close_all_pipes(runtime);
        runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_FAILED;
        mark_terminal(runtime);
        set_error(out_error, PRINTERCTL_DAEMON_ERR_WORKER_FAULT,
                  "worker did not reach READY: %s",
                  event.detail[0] != '\0' ? event.detail : "unexpected startup event");
        record_runtime_error(runtime, out_error->message);
        return out_error->status;
    }

    runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_RUNNING;
    return PRINTERCTL_DAEMON_OK;
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_nop(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (runtime == NULL || out_token == NULL) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                  "runtime and out_token must not be NULL");
        return out_error->status;
    }
    *out_token = 0;

    printerctl_daemon_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.wire_size = sizeof(cmd);
    cmd.type = PRINTERCTL_DAEMON_CMD_NOP;
    cmd.token = runtime->next_token++;

    if (printerctl_daemon_ipc_write_command(runtime->cmd_pipe[1], &cmd) !=
        PRINTERCTL_DAEMON_IPC_OK) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_IO, "cmd_pipe: write failed");
        return out_error->status;
    }

    *out_token = cmd.token;
    return PRINTERCTL_DAEMON_OK;
}

printerctl_daemon_status_t printerctl_daemon_runtime_wait_event(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_event_t *out_event,
    printerctl_daemon_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (runtime == NULL || out_event == NULL) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                  "runtime and out_event must not be NULL");
        return out_error->status;
    }
    memset(out_event, 0, sizeof(*out_event));

    printerctl_daemon_ipc_status_t ipc_status =
        printerctl_daemon_ipc_read_event(runtime->event_pipe[0], out_event);

    if (ipc_status != PRINTERCTL_DAEMON_IPC_OK) {
        memset(out_event, 0, sizeof(*out_event));
        const char *reason;
        switch (ipc_status) {
        case PRINTERCTL_DAEMON_IPC_EOF: reason = "event_pipe: unexpected end of stream"; break;
        case PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED: reason = "event_pipe: truncated message"; break;
        case PRINTERCTL_DAEMON_IPC_ERR_MALFORMED: reason = "event_pipe: malformed message"; break;
        default: reason = "event_pipe: read failed"; break;
        }
        /* The channel itself is lost -- total loss of observability,
         * distinct from a well-formed FAULT event below. */
        runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_FAILED;
        record_runtime_error(runtime, reason);
        set_error(out_error, PRINTERCTL_DAEMON_ERR_IO, "%s", reason);
        return out_error->status;
    }

    switch (out_event->type) {
    case PRINTERCTL_DAEMON_EVENT_READY:
        runtime->snapshot.worker_state = PRINTERCTL_DAEMON_WORKER_READY;
        break;
    case PRINTERCTL_DAEMON_EVENT_STOPPING:
        runtime->snapshot.worker_state = PRINTERCTL_DAEMON_WORKER_STOPPING;
        break;
    case PRINTERCTL_DAEMON_EVENT_STOPPED:
        runtime->snapshot.worker_state = PRINTERCTL_DAEMON_WORKER_STOPPED;
        break;
    case PRINTERCTL_DAEMON_EVENT_FAULT:
        /* Well-formed signal: the worker itself is broken. daemon_state
         * is deliberately left untouched -- see runtime.h. */
        runtime->snapshot.worker_state = PRINTERCTL_DAEMON_WORKER_FAULTED;
        record_runtime_error(runtime, out_event->detail);
        break;
    case PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED:
        break; /* caller correlates the token itself */
    }

    return PRINTERCTL_DAEMON_OK;
}

printerctl_daemon_status_t printerctl_daemon_runtime_shutdown(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (runtime == NULL) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT, "runtime must not be NULL");
        return out_error->status;
    }

    if (runtime->shutdown_started) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_ALREADY,
                  "no running worker to shut down (already shut down, or start() never succeeded)");
        return out_error->status;
    }
    mark_terminal(runtime);

    runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_STOPPING;

    /* If an earlier printerctl_daemon_runtime_wait_event() call already
     * observed the worker's own STOPPED (e.g. a prior cmd_pipe-EOF
     * orderly termination) or FAULT event, the worker thread body has
     * already returned -- it will never read a SHUTDOWN command or emit
     * another event. Sending one anyway would just sit unread in
     * cmd_pipe's kernel buffer (harmless), but then waiting for a
     * *second* terminal event that will never arrive would block
     * read(event_pipe[0]) forever: nothing closes event_pipe[1] except
     * final cleanup (see the fd-ownership note above), so its read end
     * never sees EOF on its own. Skip straight to join + cleanup in
     * that case. */
    bool worker_already_terminal =
        runtime->snapshot.worker_state == PRINTERCTL_DAEMON_WORKER_STOPPED ||
        runtime->snapshot.worker_state == PRINTERCTL_DAEMON_WORKER_FAULTED;

    if (!worker_already_terminal) {
        printerctl_daemon_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.wire_size = sizeof(cmd);
        cmd.type = PRINTERCTL_DAEMON_CMD_SHUTDOWN;
        cmd.token = runtime->next_token++;

        if (printerctl_daemon_ipc_write_command(runtime->cmd_pipe[1], &cmd) !=
            PRINTERCTL_DAEMON_IPC_OK) {
            set_error(out_error, PRINTERCTL_DAEMON_ERR_IO,
                      "cmd_pipe: write failed while requesting shutdown");
            runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_FAILED;
            record_runtime_error(runtime, out_error->message);
            pthread_join(runtime->worker_thread, NULL);
            close_all_pipes(runtime);
            return out_error->status;
        }

        /* Drain events until the worker thread body is done (STOPPED on
         * the normal path, FAULT if something went wrong instead) --
         * either one means the thread has returned and pthread_join()
         * will proceed promptly. Anything else (a stray
         * COMMAND_COMPLETED left over from an undrained NOP, STOPPING)
         * is discarded. */
        for (;;) {
            printerctl_daemon_event_t event;
            printerctl_daemon_error_t wait_error;
            printerctl_daemon_status_t status =
                printerctl_daemon_runtime_wait_event(runtime, &event, &wait_error);

            if (status != PRINTERCTL_DAEMON_OK) {
                pthread_join(runtime->worker_thread, NULL);
                close_all_pipes(runtime);
                *out_error = wait_error;
                return out_error->status;
            }

            if (event.type == PRINTERCTL_DAEMON_EVENT_STOPPED ||
                event.type == PRINTERCTL_DAEMON_EVENT_FAULT) {
                break;
            }
        }
    }

    pthread_join(runtime->worker_thread, NULL);
    close_all_pipes(runtime);
    runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_STOPPED;
    return PRINTERCTL_DAEMON_OK;
}

void printerctl_daemon_runtime_snapshot(
    const printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_snapshot_t *out_snapshot)
{
    *out_snapshot = runtime->snapshot;
}
