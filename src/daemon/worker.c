#include "printerctl/daemon/worker.h"

#include "printerctl/daemon/ipc.h"

#include <stdio.h>
#include <string.h>

static void emit(int event_write_fd, printerctl_daemon_event_type_t type, uint32_t token,
                  printerctl_daemon_result_t result, const char *detail)
{
    printerctl_daemon_event_t event;
    memset(&event, 0, sizeof(event));
    event.wire_size = sizeof(event);
    event.type = type;
    event.token = token;
    event.result = result;
    if (detail != NULL) {
        snprintf(event.detail, sizeof(event.detail), "%s", detail);
    }

    /* Best-effort: if the event channel itself is gone, there is
     * nothing left to report to and nothing more this thread can do
     * about it -- it still returns, ending the thread deterministically
     * either way. */
    (void)printerctl_daemon_ipc_write_event(event_write_fd, &event);
}

void *printerctl_daemon_worker_main(void *arg)
{
    printerctl_daemon_worker_args_t *args = arg;

    if (args->init_hook != NULL) {
        char message[PRINTERCTL_DAEMON_EVENT_DETAIL_MAX];
        message[0] = '\0';
        if (!args->init_hook(args->init_context, message, sizeof(message))) {
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_FAULT, 0,
                 PRINTERCTL_DAEMON_RESULT_ERROR,
                 message[0] != '\0' ? message : "worker initialization failed");
            return NULL;
        }
    }

    emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_READY, 0, PRINTERCTL_DAEMON_RESULT_OK,
         NULL);

    for (;;) {
        printerctl_daemon_command_t cmd;
        printerctl_daemon_ipc_status_t status =
            printerctl_daemon_ipc_read_command(args->cmd_read_fd, &cmd);

        if (status == PRINTERCTL_DAEMON_IPC_EOF) {
            /* Orderly: the control side went away without an explicit
             * SHUTDOWN. Still a clean stop, not a fault. */
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPING, 0,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPED, 0,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            return NULL;
        }

        if (status != PRINTERCTL_DAEMON_IPC_OK) {
            const char *detail = status == PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED
                ? "cmd_pipe: truncated message"
                : status == PRINTERCTL_DAEMON_IPC_ERR_MALFORMED
                    ? "cmd_pipe: malformed message"
                    : "cmd_pipe: read failed";
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_FAULT, 0,
                 PRINTERCTL_DAEMON_RESULT_ERROR, detail);
            return NULL;
        }

        switch (cmd.type) {
        case PRINTERCTL_DAEMON_CMD_NOP:
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            break;

        case PRINTERCTL_DAEMON_CMD_SHUTDOWN:
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPING, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            /* No real cleanup to perform in M13: no engine, no printer,
             * no serial owned yet. */
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPED, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            return NULL;

        default:
            /* Unreachable in practice: printerctl_daemon_ipc_read_command()
             * already rejects any type outside the enum. Kept as a
             * defensive fault path so an unrecognized command never
             * silently falls through. */
            emit(args->event_write_fd, PRINTERCTL_DAEMON_EVENT_FAULT, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_ERROR, "unknown command type");
            return NULL;
        }
    }
}
