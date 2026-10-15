#include "printerctl/daemon/runtime.h"

#include "printerctl/protocol/plp1.h"

#include <errno.h>
#include <poll.h>
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

static printerctl_daemon_status_t send_command(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_command_type_t type,
    const char *path,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

static printerctl_daemon_status_t send_command_payload(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_command_type_t type,
    const char *path,
    const char *text,
    uint8_t axis,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error);

printerctl_daemon_status_t printerctl_daemon_runtime_start(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_worker_init_fn worker_init_hook,
    void *worker_init_context,
    printerctl_daemon_error_t *out_error)
{
    printerctl_config_t config;
    printerctl_config_set_defaults(&config);
    return printerctl_daemon_runtime_start_with_config(
        runtime, &config, worker_init_hook, worker_init_context, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_start_with_config(
    printerctl_daemon_runtime_t *runtime,
    const printerctl_config_t *config,
    printerctl_daemon_worker_init_fn worker_init_hook,
    void *worker_init_context,
    printerctl_daemon_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (runtime == NULL || config == NULL) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                  "runtime and config must not be NULL");
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
    runtime->snapshot.execution_state = PRINTERCTL_PLP1_EXECUTION_STATE_IDLE;
    printerctl_daemon_worker_reset_signal_requests();

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
    runtime->worker_args.config = *config;
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
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_NOP, NULL, out_token, out_error);
}

static printerctl_daemon_status_t send_command(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_command_type_t type,
    const char *path,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command_payload(runtime, type, path, NULL, 0, out_token, out_error);
}

static printerctl_daemon_status_t send_command_payload(
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_command_type_t type,
    const char *path,
    const char *text,
    uint8_t axis,
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
    cmd.type = type;
    cmd.token = runtime->next_token++;
    if (path != NULL) {
        if (strlen(path) >= sizeof(cmd.path)) {
            set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                      "print path exceeds %zu bytes", sizeof(cmd.path) - 1u);
            return out_error->status;
        }
        snprintf(cmd.path, sizeof(cmd.path), "%s", path);
    }
    if (text != NULL) {
        char *target = type == PRINTERCTL_DAEMON_CMD_MACRO_EXECUTE ? cmd.macro_id : cmd.text;
        size_t target_size =
            type == PRINTERCTL_DAEMON_CMD_MACRO_EXECUTE ? sizeof(cmd.macro_id) : sizeof(cmd.text);
        if (strlen(text) >= target_size) {
            set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                      "command text exceeds %zu bytes", target_size - 1u);
            return out_error->status;
        }
        snprintf(target, target_size, "%s", text);
    }
    cmd.axis = axis;

    if (printerctl_daemon_ipc_write_command(runtime->cmd_pipe[1], &cmd) !=
        PRINTERCTL_DAEMON_IPC_OK) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_IO, "cmd_pipe: write failed");
        return out_error->status;
    }

    *out_token = cmd.token;
    return PRINTERCTL_DAEMON_OK;
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_start(
    printerctl_daemon_runtime_t *runtime,
    const char *path,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    if (path == NULL || path[0] == '\0') {
        if (out_error != NULL) {
            memset(out_error, 0, sizeof(*out_error));
            set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                      "print path must not be empty");
        }
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_PRINT_START, path, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_pause(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_PRINT_PAUSE, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_resume(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_PRINT_RESUME, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_cancel(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_PRINT_CANCEL, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_print_cancel_power_failure(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_PRINT_CANCEL_POWER_FAILURE, NULL, out_token,
                        out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_query_temp(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_QUERY_TEMP, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_query_position(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_QUERY_POSITION, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_query_info(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_QUERY_INFO, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_raw_send(
    printerctl_daemon_runtime_t *runtime,
    const char *command,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    if (command == NULL || command[0] == '\0') {
        if (out_error != NULL) {
            memset(out_error, 0, sizeof(*out_error));
            set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                      "raw command must not be empty");
        }
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    return send_command_payload(runtime, PRINTERCTL_DAEMON_CMD_RAW_SEND, NULL, command, 0,
                                out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_home(
    printerctl_daemon_runtime_t *runtime,
    uint8_t axis,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    if (axis > PRINTERCTL_PLP1_HOME_Z) {
        if (out_error != NULL) {
            memset(out_error, 0, sizeof(*out_error));
            set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                      "invalid HOME axis %u", axis);
        }
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    return send_command_payload(runtime, PRINTERCTL_DAEMON_CMD_HOME, NULL, NULL, axis,
                                out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_sd_pause(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_SD_PAUSE, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_sd_resume(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_SD_RESUME, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_sd_cancel(
    printerctl_daemon_runtime_t *runtime,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    return send_command(runtime, PRINTERCTL_DAEMON_CMD_SD_CANCEL, NULL, out_token, out_error);
}

printerctl_daemon_status_t printerctl_daemon_runtime_send_macro_execute(
    printerctl_daemon_runtime_t *runtime,
    const char *id,
    uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    if (id == NULL || id[0] == '\0' || strlen(id) >= PRINTERCTL_MACRO_ID_MAX) {
        if (out_error != NULL) {
            memset(out_error, 0, sizeof(*out_error));
            set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                      "macro id must be non-empty and bounded");
        }
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    return send_command_payload(runtime, PRINTERCTL_DAEMON_CMD_MACRO_EXECUTE, NULL, id, 0,
                                out_token, out_error);
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
    case PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE:
        runtime->snapshot.execution_kind = out_event->execution_kind;
        runtime->snapshot.execution_state = out_event->execution_state;
        runtime->snapshot.progress_percent = out_event->progress_percent;
        runtime->snapshot.commands_sent = out_event->commands_sent;
        runtime->snapshot.commands_acknowledged = out_event->commands_acknowledged;
        runtime->snapshot.commands_total = out_event->commands_total;
        runtime->snapshot.source_position = out_event->source_position;
        runtime->snapshot.failure_reason = out_event->failure_reason;
        snprintf(runtime->snapshot.macro_id, sizeof(runtime->snapshot.macro_id), "%s",
                 out_event->macro_id);
        runtime->snapshot.macro_current_step = out_event->macro_current_step;
        runtime->snapshot.macro_total_steps = out_event->macro_total_steps;
        runtime->snapshot.macro_terminal_result = out_event->macro_terminal_result;
        if (out_event->detail[0] != '\0') {
            record_runtime_error(runtime, out_event->detail);
        }
        break;
    }

    return PRINTERCTL_DAEMON_OK;
}

printerctl_daemon_status_t printerctl_daemon_runtime_wait_command(
    printerctl_daemon_runtime_t *runtime,
    uint32_t token,
    printerctl_daemon_event_t *out_event,
    printerctl_daemon_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (runtime == NULL || out_event == NULL || token == 0) {
        set_error(out_error, PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT,
                  "runtime, out_event and token must be valid");
        return out_error->status;
    }

    for (;;) {
        printerctl_daemon_status_t status =
            printerctl_daemon_runtime_wait_event(runtime, out_event, out_error);
        if (status != PRINTERCTL_DAEMON_OK) {
            return status;
        }
        if (out_event->type == PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED &&
            out_event->token == token) {
            return PRINTERCTL_DAEMON_OK;
        }
        if (out_event->type == PRINTERCTL_DAEMON_EVENT_FAULT ||
            out_event->type == PRINTERCTL_DAEMON_EVENT_STOPPED) {
            set_error(out_error, PRINTERCTL_DAEMON_ERR_IO,
                      "worker stopped before command %u completed", token);
            return out_error->status;
        }
    }
}

printerctl_daemon_status_t printerctl_daemon_runtime_drain_events(
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

    for (;;) {
        struct pollfd pfd;
        pfd.fd = runtime->event_pipe[0];
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(out_error, PRINTERCTL_DAEMON_ERR_IO, "event_pipe poll: %s",
                      strerror(errno));
            return out_error->status;
        }
        if (rc == 0) {
            return PRINTERCTL_DAEMON_OK;
        }
        if ((pfd.revents & POLLIN) == 0) {
            set_error(out_error, PRINTERCTL_DAEMON_ERR_IO,
                      "event_pipe poll reported unusable fd");
            runtime->snapshot.daemon_state = PRINTERCTL_DAEMON_FAILED;
            snprintf(runtime->snapshot.last_runtime_error,
                     sizeof(runtime->snapshot.last_runtime_error), "%s", out_error->message);
            return out_error->status;
        }

        printerctl_daemon_event_t event;
        printerctl_daemon_status_t status =
            printerctl_daemon_runtime_wait_event(runtime, &event, out_error);
        if (status != PRINTERCTL_DAEMON_OK) {
            return status;
        }
    }
}

int printerctl_daemon_runtime_event_fd(const printerctl_daemon_runtime_t *runtime)
{
    return runtime != NULL ? runtime->event_pipe[0] : -1;
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
