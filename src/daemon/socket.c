#define _GNU_SOURCE

#include "printerctl/daemon/socket.h"

#include "printerctl/protocol/plp1.h"
#include "printerctl/protocol/plp1_parser.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void set_error(printerctl_daemon_socket_error_t *out_error,
                      printerctl_daemon_socket_status_t status,
                      const char *format, ...)
{
    out_error->status = status;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int connect_unix_socket(const char *socket_path, int *saved_errno)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        *saved_errno = errno;
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    int rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    *saved_errno = errno;
    close(fd);
    return rc;
}

static printerctl_daemon_socket_status_t prepare_socket_path(
    const char *socket_path, printerctl_daemon_socket_error_t *out_error)
{
    struct stat st;
    if (lstat(socket_path, &st) != 0) {
        if (errno == ENOENT) {
            return PRINTERCTL_DAEMON_SOCKET_OK;
        }
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_SOCKET, "lstat(%s): %s", socket_path,
                  strerror(errno));
        return out_error->status;
    }

    if (!S_ISSOCK(st.st_mode)) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_IN_USE,
                  "%s exists and is not a Unix socket", socket_path);
        return out_error->status;
    }

    int saved_errno = 0;
    if (connect_unix_socket(socket_path, &saved_errno) == 0) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_IN_USE,
                  "%s is owned by a live Unix socket server", socket_path);
        return out_error->status;
    }

    if (saved_errno != ECONNREFUSED && saved_errno != ENOENT) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_SOCKET,
                  "could not prove stale socket %s: %s", socket_path, strerror(saved_errno));
        return out_error->status;
    }

    if (unlink(socket_path) != 0 && errno != ENOENT) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_SOCKET, "unlink(%s): %s", socket_path,
                  strerror(errno));
        return out_error->status;
    }
    return PRINTERCTL_DAEMON_SOCKET_OK;
}

printerctl_daemon_socket_status_t printerctl_daemon_socket_listen(
    printerctl_daemon_socket_server_t *server,
    const char *socket_path,
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_socket_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_SOCKET_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (server == NULL || socket_path == NULL || runtime == NULL) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_INVALID_ARGUMENT,
                  "server, socket_path and runtime must not be NULL");
        return out_error->status;
    }

    size_t path_len = strlen(socket_path);
    if (path_len == 0 || path_len >= PRINTERCTL_DAEMON_SOCKET_PATH_MAX) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PATH_TOO_LONG,
                  "socket path length must be 1..%u bytes",
                  (unsigned)PRINTERCTL_DAEMON_SOCKET_PATH_MAX - 1u);
        return out_error->status;
    }

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    snprintf(server->socket_path, sizeof(server->socket_path), "%s", socket_path);
    server->runtime = runtime;

    printerctl_daemon_socket_status_t path_status =
        prepare_socket_path(socket_path, out_error);
    if (path_status != PRINTERCTL_DAEMON_SOCKET_OK) {
        return path_status;
    }

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_SOCKET, "socket(AF_UNIX): %s",
                  strerror(errno));
        return out_error->status;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    mode_t old_umask = umask(0077);
    int bind_rc = bind(server->listen_fd, (const struct sockaddr *)&addr, sizeof(addr));
    int bind_errno = errno;
    umask(old_umask);
    if (bind_rc != 0) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_BIND, "bind(%s): %s", socket_path,
                  strerror(bind_errno));
        close_fd(&server->listen_fd);
        return out_error->status;
    }
    server->owns_socket_path = true;

    if (chmod(socket_path, S_IRUSR | S_IWUSR) != 0) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_SOCKET, "chmod(%s): %s", socket_path,
                  strerror(errno));
        printerctl_daemon_socket_close(server);
        return out_error->status;
    }

    if (listen(server->listen_fd, 8) != 0) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_LISTEN, "listen(%s): %s", socket_path,
                  strerror(errno));
        printerctl_daemon_socket_close(server);
        return out_error->status;
    }

    return PRINTERCTL_DAEMON_SOCKET_OK;
}

static uint8_t map_daemon_state(printerctl_daemon_state_t state)
{
    switch (state) {
    case PRINTERCTL_DAEMON_STARTING: return PRINTERCTL_PLP1_DAEMON_STATE_STARTING;
    case PRINTERCTL_DAEMON_RUNNING: return PRINTERCTL_PLP1_DAEMON_STATE_RUNNING;
    case PRINTERCTL_DAEMON_STOPPING: return PRINTERCTL_PLP1_DAEMON_STATE_STOPPING;
    case PRINTERCTL_DAEMON_STOPPED: return PRINTERCTL_PLP1_DAEMON_STATE_STOPPED;
    case PRINTERCTL_DAEMON_FAILED: return PRINTERCTL_PLP1_DAEMON_STATE_FAILED;
    default: return PRINTERCTL_PLP1_DAEMON_STATE_UNKNOWN;
    }
}

static uint8_t map_worker_state(printerctl_daemon_worker_state_t state)
{
    switch (state) {
    case PRINTERCTL_DAEMON_WORKER_INITIALIZING: return PRINTERCTL_PLP1_WORKER_STATE_INITIALIZING;
    case PRINTERCTL_DAEMON_WORKER_READY: return PRINTERCTL_PLP1_WORKER_STATE_READY;
    case PRINTERCTL_DAEMON_WORKER_STOPPING: return PRINTERCTL_PLP1_WORKER_STATE_STOPPING;
    case PRINTERCTL_DAEMON_WORKER_STOPPED: return PRINTERCTL_PLP1_WORKER_STATE_STOPPED;
    case PRINTERCTL_DAEMON_WORKER_FAULTED: return PRINTERCTL_PLP1_WORKER_STATE_FAULTED;
    default: return PRINTERCTL_PLP1_WORKER_STATE_UNKNOWN;
    }
}

static printerctl_daemon_socket_status_t write_full(
    int fd, const uint8_t *buffer, size_t length,
    printerctl_daemon_socket_error_t *out_error)
{
    size_t written = 0;
    while (written < length) {
        ssize_t n = send(fd, buffer + written, length - written, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                return PRINTERCTL_DAEMON_SOCKET_OK;
            }
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_IO, "write(client): %s",
                      strerror(errno));
            return out_error->status;
        }
        if (n == 0) {
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_IO, "write(client): wrote 0 bytes");
            return out_error->status;
        }
        written += (size_t)n;
    }
    return PRINTERCTL_DAEMON_SOCKET_OK;
}

static printerctl_daemon_socket_status_t send_frame(
    int client_fd, uint8_t message_type, uint16_t flags, uint32_t request_id,
    uint16_t wire_status, const uint8_t *payload, uint16_t payload_length,
    printerctl_daemon_socket_error_t *out_error)
{
    uint8_t frame_buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = message_type;
    frame.flags = flags;
    frame.request_id = request_id;
    frame.status = wire_status;
    frame.payload = payload;
    frame.payload_length = payload_length;

    size_t frame_length = 0;
    printerctl_plp1_error_t plp_error;
    if (printerctl_plp1_encode_frame(&frame, frame_buffer, sizeof(frame_buffer), &frame_length,
                                     &plp_error) != PRINTERCTL_PLP1_OK) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL, "encode response: %s",
                  plp_error.message);
        return out_error->status;
    }

    return write_full(client_fd, frame_buffer, frame_length, out_error);
}

static printerctl_daemon_socket_status_t send_error_response(
    int client_fd, const printerctl_plp1_frame_t *request, uint16_t wire_status,
    printerctl_daemon_socket_error_t *out_error)
{
    return send_frame(client_fd, request->message_type,
                      PRINTERCTL_PLP1_FLAG_RESPONSE | PRINTERCTL_PLP1_FLAG_ERROR,
                      request->request_id, wire_status, NULL, 0, out_error);
}

static int32_t to_milli(double value)
{
    return (int32_t)(value >= 0.0 ? value * 1000.0 + 0.5 : value * 1000.0 - 0.5);
}

static printerctl_daemon_status_t send_query_command(
    printerctl_daemon_socket_server_t *server, uint8_t message_type, uint32_t *out_token,
    printerctl_daemon_error_t *out_error)
{
    switch (message_type) {
    case PRINTERCTL_PLP1_MSG_QUERY_TEMP:
        return printerctl_daemon_runtime_send_query_temp(server->runtime, out_token, out_error);
    case PRINTERCTL_PLP1_MSG_QUERY_POSITION:
        return printerctl_daemon_runtime_send_query_position(server->runtime, out_token,
                                                             out_error);
    case PRINTERCTL_PLP1_MSG_QUERY_INFO:
        return printerctl_daemon_runtime_send_query_info(server->runtime, out_token, out_error);
    default:
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
}

static printerctl_daemon_socket_status_t send_query_success(
    int client_fd, const printerctl_plp1_frame_t *request,
    const printerctl_daemon_event_t *event, printerctl_daemon_socket_error_t *out_error)
{
    uint8_t payload[640];
    size_t payload_length = 0;
    printerctl_plp1_error_t plp_error;

    if (request->message_type == PRINTERCTL_PLP1_MSG_QUERY_TEMP) {
        printerctl_plp1_query_temp_response_t value;
        memset(&value, 0, sizeof(value));
        if (event->query_has_hotend) {
            value.present_fields |= PRINTERCTL_PLP1_QUERY_TEMP_FIELD_HOTEND;
        }
        if (event->query_has_bed) {
            value.present_fields |= PRINTERCTL_PLP1_QUERY_TEMP_FIELD_BED;
        }
        value.hotend_actual_milli_c = to_milli(event->query_hotend_actual);
        value.hotend_target_milli_c = to_milli(event->query_hotend_target);
        value.bed_actual_milli_c = to_milli(event->query_bed_actual);
        value.bed_target_milli_c = to_milli(event->query_bed_target);
        if (printerctl_plp1_encode_query_temp_response(
                &value, payload, sizeof(payload), &payload_length, &plp_error) !=
            PRINTERCTL_PLP1_OK) {
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                      "encode TEMP payload: %s", plp_error.message);
            return out_error->status;
        }
    } else if (request->message_type == PRINTERCTL_PLP1_MSG_QUERY_POSITION) {
        printerctl_plp1_query_position_response_t value;
        memset(&value, 0, sizeof(value));
        if (event->query_has_e) {
            value.present_fields |= PRINTERCTL_PLP1_QUERY_POSITION_FIELD_E;
        }
        value.x_milli_mm = to_milli(event->query_x);
        value.y_milli_mm = to_milli(event->query_y);
        value.z_milli_mm = to_milli(event->query_z);
        value.e_milli_mm = to_milli(event->query_e);
        if (printerctl_plp1_encode_query_position_response(
                &value, payload, sizeof(payload), &payload_length, &plp_error) !=
            PRINTERCTL_PLP1_OK) {
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                      "encode POSITION payload: %s", plp_error.message);
            return out_error->status;
        }
    } else {
        printerctl_plp1_query_info_response_t value;
        memset(&value, 0, sizeof(value));
        snprintf(value.firmware_name, sizeof(value.firmware_name), "%s",
                 event->query_firmware_name);
        snprintf(value.machine_type, sizeof(value.machine_type), "%s",
                 event->query_machine_type);
        snprintf(value.protocol_version, sizeof(value.protocol_version), "%s",
                 event->query_protocol_version);
        snprintf(value.uuid, sizeof(value.uuid), "%s", event->query_uuid);
        value.has_extruder_count = event->query_has_extruder_count;
        if (event->query_extruder_count >= 0 && event->query_extruder_count <= 65535) {
            value.extruder_count = (uint16_t)event->query_extruder_count;
        }
        if (printerctl_plp1_encode_query_info_response(
                &value, payload, sizeof(payload), &payload_length, &plp_error) !=
            PRINTERCTL_PLP1_OK) {
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                      "encode INFO payload: %s", plp_error.message);
            return out_error->status;
        }
    }

    return send_frame(client_fd, request->message_type, PRINTERCTL_PLP1_FLAG_RESPONSE,
                      request->request_id, PRINTERCTL_PLP1_SUCCESS, payload,
                      (uint16_t)payload_length, out_error);
}

static printerctl_daemon_status_t send_physical_command(
    printerctl_daemon_socket_server_t *server, const printerctl_plp1_frame_t *request,
    uint32_t *out_token, printerctl_daemon_error_t *out_error)
{
    switch (request->message_type) {
    case PRINTERCTL_PLP1_MSG_RAW_SEND: {
        printerctl_plp1_raw_send_request_t value;
        printerctl_plp1_error_t plp_error;
        if (printerctl_plp1_decode_raw_send_request(request->payload, request->payload_length,
                                                    &value, &plp_error) != PRINTERCTL_PLP1_OK) {
            return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
        }
        return printerctl_daemon_runtime_send_raw_send(server->runtime, value.command, out_token,
                                                       out_error);
    }
    case PRINTERCTL_PLP1_MSG_HOME: {
        printerctl_plp1_home_request_t value;
        printerctl_plp1_error_t plp_error;
        if (printerctl_plp1_decode_home_request(request->payload, request->payload_length,
                                                &value, &plp_error) != PRINTERCTL_PLP1_OK) {
            return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
        }
        return printerctl_daemon_runtime_send_home(server->runtime, value.axis, out_token,
                                                   out_error);
    }
    case PRINTERCTL_PLP1_MSG_SD_PAUSE:
        return printerctl_daemon_runtime_send_sd_pause(server->runtime, out_token, out_error);
    case PRINTERCTL_PLP1_MSG_SD_RESUME:
        return printerctl_daemon_runtime_send_sd_resume(server->runtime, out_token, out_error);
    case PRINTERCTL_PLP1_MSG_SD_CANCEL:
        return printerctl_daemon_runtime_send_sd_cancel(server->runtime, out_token, out_error);
    default:
        return PRINTERCTL_DAEMON_ERR_INVALID_ARGUMENT;
    }
}

static printerctl_daemon_socket_status_t send_physical_success(
    int client_fd, const printerctl_plp1_frame_t *request,
    const printerctl_daemon_event_t *event, printerctl_daemon_socket_error_t *out_error)
{
    if (request->message_type != PRINTERCTL_PLP1_MSG_RAW_SEND) {
        return send_frame(client_fd, request->message_type, PRINTERCTL_PLP1_FLAG_RESPONSE,
                          request->request_id, PRINTERCTL_PLP1_SUCCESS, NULL, 0, out_error);
    }

    uint8_t payload[PRINTERCTL_PLP1_RAW_RESPONSE_MAX + 2];
    size_t payload_length = 0;
    printerctl_plp1_error_t plp_error;
    if (printerctl_plp1_encode_raw_send_response(event->response_text, payload, sizeof(payload),
                                                 &payload_length, &plp_error) !=
        PRINTERCTL_PLP1_OK) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                  "encode RAW_SEND payload: %s", plp_error.message);
        return out_error->status;
    }
    return send_frame(client_fd, request->message_type, PRINTERCTL_PLP1_FLAG_RESPONSE,
                      request->request_id, PRINTERCTL_PLP1_SUCCESS, payload,
                      (uint16_t)payload_length, out_error);
}

static printerctl_daemon_socket_status_t handle_frame(
    printerctl_daemon_socket_server_t *server, int client_fd,
    const printerctl_plp1_frame_t *request, printerctl_daemon_socket_error_t *out_error)
{
    if ((request->flags & PRINTERCTL_PLP1_FLAG_REQUEST) == 0) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                  "client sent a non-request frame");
        return out_error->status;
    }

    if (request->message_type == PRINTERCTL_PLP1_MSG_PING) {
        return send_frame(client_fd, PRINTERCTL_PLP1_MSG_PING, PRINTERCTL_PLP1_FLAG_RESPONSE,
                          request->request_id, PRINTERCTL_PLP1_SUCCESS, NULL, 0, out_error);
    }

    if (request->message_type == PRINTERCTL_PLP1_MSG_VERSION) {
        uint8_t payload[8];
        size_t payload_length = 0;
        printerctl_plp1_version_response_t value;
        value.protocol_version = PRINTERCTL_PLP1_VERSION_1;
        value.capability_bitmap =
            PRINTERCTL_PLP1_CAP_PRINTER_STATUS | PRINTERCTL_PLP1_CAP_PRINT_CONTROL;

        printerctl_plp1_error_t plp_error;
        if (printerctl_plp1_encode_version_response(&value, payload, sizeof(payload),
                                                    &payload_length, &plp_error) !=
            PRINTERCTL_PLP1_OK) {
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                      "encode VERSION payload: %s", plp_error.message);
            return out_error->status;
        }
        return send_frame(client_fd, PRINTERCTL_PLP1_MSG_VERSION, PRINTERCTL_PLP1_FLAG_RESPONSE,
                          request->request_id, PRINTERCTL_PLP1_SUCCESS, payload,
                          (uint16_t)payload_length, out_error);
    }

    if (request->message_type == PRINTERCTL_PLP1_MSG_STATUS) {
        uint32_t requested_fields = 0;
        printerctl_plp1_error_t plp_error;
        if (printerctl_plp1_decode_status_request(request->payload, request->payload_length,
                                                  &requested_fields, &plp_error) !=
            PRINTERCTL_PLP1_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                                       out_error);
        }

        printerctl_daemon_error_t runtime_error;
        if (printerctl_daemon_runtime_drain_events(server->runtime, &runtime_error) !=
            PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
                                       out_error);
        }

        printerctl_daemon_snapshot_t snapshot;
        printerctl_daemon_runtime_snapshot(server->runtime, &snapshot);

        printerctl_plp1_status_response_t value;
        memset(&value, 0, sizeof(value));
        if (requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE;
            value.daemon_state = map_daemon_state(snapshot.daemon_state);
        }
        if (requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE;
            value.worker_state = map_worker_state(snapshot.worker_state);
        }
        if (requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE;
            value.execution_state = snapshot.execution_state;
        }
        if (requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS;
            value.progress = snapshot.progress_percent;
        }
        if (requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_SENT) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_SENT;
            value.commands_sent = snapshot.commands_sent;
        }
        if (requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED;
            value.commands_acknowledged = snapshot.commands_acknowledged;
        }
        if (requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_SOURCE_POSITION) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_SOURCE_POSITION;
            value.source_position = snapshot.source_position;
        }
        if ((requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON) &&
            snapshot.failure_reason != 0) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON;
            value.failure_reason = snapshot.failure_reason;
        }
        if ((requested_fields & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_TOTAL) &&
            snapshot.commands_total != 0) {
            value.present_fields |= PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_TOTAL;
            value.commands_total = snapshot.commands_total;
        }

        uint8_t payload[32];
        size_t payload_length = 0;
        if (printerctl_plp1_encode_status_response(&value, payload, sizeof(payload),
                                                   &payload_length, &plp_error) !=
            PRINTERCTL_PLP1_OK) {
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                      "encode STATUS payload: %s", plp_error.message);
            return out_error->status;
        }
        return send_frame(client_fd, PRINTERCTL_PLP1_MSG_STATUS, PRINTERCTL_PLP1_FLAG_RESPONSE,
                          request->request_id, PRINTERCTL_PLP1_SUCCESS, payload,
                          (uint16_t)payload_length, out_error);
    }

    if (request->message_type == PRINTERCTL_PLP1_MSG_PRINT_START) {
        printerctl_plp1_print_start_request_t start;
        printerctl_plp1_error_t plp_error;
        if (printerctl_plp1_decode_print_start_request(request->payload, request->payload_length,
                                                       &start, &plp_error) != PRINTERCTL_PLP1_OK ||
            start.source_path[0] == '\0') {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                                       out_error);
        }

        uint32_t token = 0;
        printerctl_daemon_error_t runtime_error;
        if (printerctl_daemon_runtime_send_print_start(server->runtime, start.source_path, &token,
                                                       &runtime_error) != PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
                                       out_error);
        }
        printerctl_daemon_event_t event;
        if (printerctl_daemon_runtime_wait_command(server->runtime, token, &event,
                                                   &runtime_error) != PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
                                       out_error);
        }
        if (event.result != PRINTERCTL_DAEMON_RESULT_OK) {
            uint16_t status = event.wire_status != 0 ? event.wire_status
                                                     : PRINTERCTL_PLP1_INTERNAL_ERROR;
            return send_error_response(client_fd, request, status, out_error);
        }
        return send_frame(client_fd, PRINTERCTL_PLP1_MSG_PRINT_START,
                          PRINTERCTL_PLP1_FLAG_RESPONSE, request->request_id,
                          PRINTERCTL_PLP1_SUCCESS, NULL, 0, out_error);
    }

    if (request->message_type == PRINTERCTL_PLP1_MSG_PRINT_PAUSE ||
        request->message_type == PRINTERCTL_PLP1_MSG_PRINT_RESUME ||
        request->message_type == PRINTERCTL_PLP1_MSG_PRINT_CANCEL) {
        uint32_t token = 0;
        printerctl_daemon_error_t runtime_error;
        printerctl_daemon_status_t send_status;
        if (request->message_type == PRINTERCTL_PLP1_MSG_PRINT_PAUSE) {
            send_status =
                printerctl_daemon_runtime_send_print_pause(server->runtime, &token, &runtime_error);
        } else if (request->message_type == PRINTERCTL_PLP1_MSG_PRINT_RESUME) {
            send_status = printerctl_daemon_runtime_send_print_resume(server->runtime, &token,
                                                                      &runtime_error);
        } else {
            send_status = printerctl_daemon_runtime_send_print_cancel(server->runtime, &token,
                                                                      &runtime_error);
        }
        if (send_status != PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_INTERNAL_ERROR,
                                       out_error);
        }

        printerctl_daemon_event_t event;
        if (printerctl_daemon_runtime_wait_command(server->runtime, token, &event,
                                                   &runtime_error) != PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
                                       out_error);
        }
        if (event.result != PRINTERCTL_DAEMON_RESULT_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_NOT_PRINTING,
                                       out_error);
        }
        return send_frame(client_fd, request->message_type, PRINTERCTL_PLP1_FLAG_RESPONSE,
                          request->request_id, PRINTERCTL_PLP1_SUCCESS, NULL, 0, out_error);
    }

    if (request->message_type == PRINTERCTL_PLP1_MSG_QUERY_TEMP ||
        request->message_type == PRINTERCTL_PLP1_MSG_QUERY_POSITION ||
        request->message_type == PRINTERCTL_PLP1_MSG_QUERY_INFO) {
        uint32_t token = 0;
        printerctl_daemon_error_t runtime_error;
        if (send_query_command(server, request->message_type, &token, &runtime_error) !=
            PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
                                       out_error);
        }

        printerctl_daemon_event_t event;
        if (printerctl_daemon_runtime_wait_command(server->runtime, token, &event,
                                                   &runtime_error) != PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
                                       out_error);
        }
        if (event.result != PRINTERCTL_DAEMON_RESULT_OK) {
            uint16_t status = event.wire_status != 0 ? event.wire_status
                                                     : PRINTERCTL_PLP1_INTERNAL_ERROR;
            return send_error_response(client_fd, request, status, out_error);
        }
        return send_query_success(client_fd, request, &event, out_error);
    }

    if (request->message_type == PRINTERCTL_PLP1_MSG_RAW_SEND ||
        request->message_type == PRINTERCTL_PLP1_MSG_HOME ||
        request->message_type == PRINTERCTL_PLP1_MSG_SD_PAUSE ||
        request->message_type == PRINTERCTL_PLP1_MSG_SD_RESUME ||
        request->message_type == PRINTERCTL_PLP1_MSG_SD_CANCEL) {
        uint32_t token = 0;
        printerctl_daemon_error_t runtime_error;
        if (send_physical_command(server, request, &token, &runtime_error) !=
            PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                                       out_error);
        }

        printerctl_daemon_event_t event;
        if (printerctl_daemon_runtime_wait_command(server->runtime, token, &event,
                                                   &runtime_error) != PRINTERCTL_DAEMON_OK) {
            return send_error_response(client_fd, request, PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
                                       out_error);
        }
        if (event.result != PRINTERCTL_DAEMON_RESULT_OK) {
            uint16_t status = event.wire_status != 0 ? event.wire_status
                                                     : PRINTERCTL_PLP1_INTERNAL_ERROR;
            return send_error_response(client_fd, request, status, out_error);
        }
        return send_physical_success(client_fd, request, &event, out_error);
    }

    return send_error_response(client_fd, request, PRINTERCTL_PLP1_INVALID_STATE, out_error);
}

printerctl_daemon_socket_status_t printerctl_daemon_socket_serve_one(
    printerctl_daemon_socket_server_t *server,
    printerctl_daemon_socket_error_t *out_error)
{
    return printerctl_daemon_socket_serve_one_interruptible(server, NULL, NULL, out_error);
}

printerctl_daemon_socket_status_t printerctl_daemon_socket_serve_one_interruptible(
    printerctl_daemon_socket_server_t *server,
    printerctl_daemon_socket_should_stop_fn should_stop,
    void *context,
    printerctl_daemon_socket_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_DAEMON_SOCKET_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (server == NULL || server->listen_fd < 0 || server->runtime == NULL) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_INVALID_ARGUMENT,
                  "server must be listening and bound to a runtime");
        return out_error->status;
    }

    int client_fd;
    do {
        client_fd = accept(server->listen_fd, NULL, NULL);
    } while (client_fd < 0 && errno == EINTR);

    if (client_fd < 0) {
        set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_ACCEPT, "accept: %s",
                  strerror(errno));
        return out_error->status;
    }

    uint8_t parser_buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, parser_buffer, sizeof(parser_buffer));

    for (;;) {
        if (should_stop != NULL && should_stop(context)) {
            close(client_fd);
            return PRINTERCTL_DAEMON_SOCKET_OK;
        }

        struct pollfd pfds[2];
        pfds[0].fd = client_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = printerctl_daemon_runtime_event_fd(server->runtime);
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        int poll_rc = poll(pfds, 2, 250);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_IO, "poll(client/runtime): %s",
                      strerror(errno));
            close(client_fd);
            return out_error->status;
        }
        if (poll_rc == 0) {
            continue;
        }

        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            printerctl_daemon_error_t runtime_error;
            if (printerctl_daemon_runtime_drain_events(server->runtime, &runtime_error) !=
                PRINTERCTL_DAEMON_OK) {
                set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_IO,
                          "runtime event drain: %s", runtime_error.message);
                close(client_fd);
                return out_error->status;
            }
        }

        if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) {
            continue;
        }

        uint8_t chunk[4096];
        ssize_t n = read(client_fd, chunk, sizeof(chunk));
        if (n == 0) {
            close(client_fd);
            return PRINTERCTL_DAEMON_SOCKET_OK;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECONNRESET) {
                close(client_fd);
                return PRINTERCTL_DAEMON_SOCKET_OK;
            }
            set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_IO, "read(client): %s",
                      strerror(errno));
            close(client_fd);
            return out_error->status;
        }

        size_t offset = 0;
        while (offset < (size_t)n) {
            size_t consumed = 0;
            printerctl_plp1_error_t plp_error;
            if (printerctl_plp1_parser_feed(&parser, chunk + offset, (size_t)n - offset,
                                            &consumed, &plp_error) != PRINTERCTL_PLP1_OK) {
                set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                          "parser feed: %s", plp_error.message);
                close(client_fd);
                return out_error->status;
            }
            offset += consumed;

            bool made_progress = consumed > 0;
            for (;;) {
                printerctl_plp1_frame_t frame;
                printerctl_plp1_parser_take_status_t take_status =
                    printerctl_plp1_parser_take_frame(&parser, &frame, &plp_error);
                if (take_status == PRINTERCTL_PLP1_PARSER_FRAME_READY) {
                    made_progress = true;
                    printerctl_daemon_socket_status_t handle_status =
                        handle_frame(server, client_fd, &frame, out_error);
                    if (handle_status != PRINTERCTL_DAEMON_SOCKET_OK) {
                        close(client_fd);
                        return handle_status;
                    }
                    continue;
                }
                if (take_status == PRINTERCTL_PLP1_PARSER_NEED_MORE) {
                    break;
                }

                /* Bad client stream: drop this client, keep the listening server alive. */
                close(client_fd);
                return PRINTERCTL_DAEMON_SOCKET_OK;
            }

            if (!made_progress) {
                set_error(out_error, PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL,
                          "parser made no progress");
                close(client_fd);
                return out_error->status;
            }
        }
    }
}

void printerctl_daemon_socket_close(printerctl_daemon_socket_server_t *server)
{
    if (server == NULL) {
        return;
    }
    close_fd(&server->listen_fd);
    if (server->owns_socket_path) {
        unlink(server->socket_path);
        server->owns_socket_path = false;
    }
    server->runtime = NULL;
}
