#include "printerctl/daemon/ipc.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

/*
 * Writes exactly `size` bytes from `buffer` to `fd`, loop-retrying
 * partial writes and EINTR/EAGAIN, matching
 * printerctl_serial_write_line()'s partial-write loop in
 * src/serial/serial.c. Returns PRINTERCTL_DAEMON_IPC_OK once every byte
 * is written, or PRINTERCTL_DAEMON_IPC_ERR_IO on the first
 * unrecoverable write() failure.
 */
static printerctl_daemon_ipc_status_t write_full(int fd, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    size_t written = 0;
    while (written < size) {
        ssize_t rc = write(fd, bytes + written, size - written);
        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return PRINTERCTL_DAEMON_IPC_ERR_IO;
        }
        written += (size_t)rc;
    }
    return PRINTERCTL_DAEMON_IPC_OK;
}

/*
 * Reads exactly `size` bytes into `buffer`, loop-retrying partial reads
 * and EINTR/EAGAIN. A read() of 0 before any byte has arrived is a
 * clean PRINTERCTL_DAEMON_IPC_EOF (the peer closed its write end at a
 * message boundary); 0 after some but not all bytes have arrived is
 * PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED (the peer closed mid-message).
 */
static printerctl_daemon_ipc_status_t read_full(int fd, void *buffer, size_t size)
{
    unsigned char *bytes = buffer;
    size_t received = 0;
    while (received < size) {
        ssize_t rc = read(fd, bytes + received, size - received);
        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return PRINTERCTL_DAEMON_IPC_ERR_IO;
        }
        if (rc == 0) {
            return received == 0 ? PRINTERCTL_DAEMON_IPC_EOF
                                  : PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED;
        }
        received += (size_t)rc;
    }
    return PRINTERCTL_DAEMON_IPC_OK;
}

static bool command_type_valid(printerctl_daemon_command_type_t type)
{
    switch (type) {
    case PRINTERCTL_DAEMON_CMD_NOP:
    case PRINTERCTL_DAEMON_CMD_SHUTDOWN:
    case PRINTERCTL_DAEMON_CMD_PRINT_START:
    case PRINTERCTL_DAEMON_CMD_PRINT_PAUSE:
    case PRINTERCTL_DAEMON_CMD_PRINT_RESUME:
    case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL:
    case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL_POWER_FAILURE:
        return true;
    }
    return false;
}

static bool event_type_valid(printerctl_daemon_event_type_t type)
{
    switch (type) {
    case PRINTERCTL_DAEMON_EVENT_READY:
    case PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED:
    case PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE:
    case PRINTERCTL_DAEMON_EVENT_STOPPING:
    case PRINTERCTL_DAEMON_EVENT_STOPPED:
    case PRINTERCTL_DAEMON_EVENT_FAULT:
        return true;
    }
    return false;
}

static bool result_valid(printerctl_daemon_result_t result)
{
    return result == PRINTERCTL_DAEMON_RESULT_OK || result == PRINTERCTL_DAEMON_RESULT_ERROR;
}

printerctl_daemon_ipc_status_t printerctl_daemon_ipc_write_command(
    int fd, const printerctl_daemon_command_t *cmd)
{
    return write_full(fd, cmd, sizeof(*cmd));
}

printerctl_daemon_ipc_status_t printerctl_daemon_ipc_write_event(
    int fd, const printerctl_daemon_event_t *event)
{
    return write_full(fd, event, sizeof(*event));
}

printerctl_daemon_ipc_status_t printerctl_daemon_ipc_read_command(
    int fd, printerctl_daemon_command_t *out_cmd)
{
    memset(out_cmd, 0, sizeof(*out_cmd));

    printerctl_daemon_ipc_status_t status = read_full(fd, out_cmd, sizeof(*out_cmd));
    if (status != PRINTERCTL_DAEMON_IPC_OK) {
        memset(out_cmd, 0, sizeof(*out_cmd));
        return status;
    }

    if (out_cmd->wire_size != sizeof(*out_cmd) || !command_type_valid(out_cmd->type)) {
        memset(out_cmd, 0, sizeof(*out_cmd));
        return PRINTERCTL_DAEMON_IPC_ERR_MALFORMED;
    }

    return PRINTERCTL_DAEMON_IPC_OK;
}

printerctl_daemon_ipc_status_t printerctl_daemon_ipc_read_event(
    int fd, printerctl_daemon_event_t *out_event)
{
    memset(out_event, 0, sizeof(*out_event));

    printerctl_daemon_ipc_status_t status = read_full(fd, out_event, sizeof(*out_event));
    if (status != PRINTERCTL_DAEMON_IPC_OK) {
        memset(out_event, 0, sizeof(*out_event));
        return status;
    }

    if (out_event->wire_size != sizeof(*out_event) || !event_type_valid(out_event->type) ||
        !result_valid(out_event->result)) {
        memset(out_event, 0, sizeof(*out_event));
        return PRINTERCTL_DAEMON_IPC_ERR_MALFORMED;
    }
    /* `detail` is attacker/bug-controlled bytes off the pipe in the
     * malformed-message tests below; guarantee NUL-termination within
     * bounds regardless of what was written. */
    out_event->detail[PRINTERCTL_DAEMON_EVENT_DETAIL_MAX - 1] = '\0';

    return PRINTERCTL_DAEMON_IPC_OK;
}
