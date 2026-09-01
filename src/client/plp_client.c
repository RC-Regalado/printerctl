#define _GNU_SOURCE

#include "printerctl/client/plp_client.h"

#include "printerctl/protocol/plp1_parser.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void set_error(printerctl_client_error_t *out_error,
                      printerctl_client_status_t status,
                      uint16_t wire_status,
                      const char *format, ...)
{
    out_error->status = status;
    out_error->wire_status = wire_status;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static printerctl_client_status_t write_full(
    int fd, const uint8_t *buffer, size_t length, printerctl_client_error_t *out_error)
{
    size_t written = 0;
    while (written < length) {
        ssize_t n = send(fd, buffer + written, length - written, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(out_error, PRINTERCTL_CLIENT_ERR_IO, 0, "write socket: %s",
                      strerror(errno));
            return out_error->status;
        }
        if (n == 0) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_IO, 0, "write socket: wrote 0 bytes");
            return out_error->status;
        }
        written += (size_t)n;
    }
    return PRINTERCTL_CLIENT_OK;
}

printerctl_client_status_t printerctl_client_connect(
    printerctl_client_t *client,
    const char *socket_path,
    printerctl_client_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if (client == NULL || socket_path == NULL || socket_path[0] == '\0') {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT, 0,
                  "client and socket_path must be valid");
        return out_error->status;
    }

    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->next_request_id = 1;

    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT, 0,
                  "socket path is too long");
        return out_error->status;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_SOCKET_UNAVAILABLE, 0,
                  "socket(AF_UNIX): %s", strerror(errno));
        return out_error->status;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printerctl_client_status_t status =
            errno == ENOENT ? PRINTERCTL_CLIENT_ERR_SOCKET_UNAVAILABLE
                            : PRINTERCTL_CLIENT_ERR_CONNECT;
        set_error(out_error, status, 0, "connect(%s): %s", socket_path, strerror(errno));
        close(fd);
        return out_error->status;
    }

    client->fd = fd;
    return PRINTERCTL_CLIENT_OK;
}

void printerctl_client_close(printerctl_client_t *client)
{
    if (client != NULL && client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
}

static printerctl_client_status_t read_response(
    printerctl_client_t *client,
    uint8_t message_type,
    uint32_t request_id,
    int timeout_ms,
    printerctl_plp1_frame_t *out_response,
    printerctl_client_error_t *out_error)
{
    uint8_t chunk[512];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, client->response_buffer, sizeof(client->response_buffer));
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        printerctl_plp1_error_t plp_error;
        printerctl_plp1_parser_take_status_t take =
            printerctl_plp1_parser_take_frame(&parser, out_response, &plp_error);
        if (take == PRINTERCTL_PLP1_PARSER_FRAME_READY) {
            if ((out_response->flags & PRINTERCTL_PLP1_FLAG_RESPONSE) == 0 ||
                out_response->message_type != message_type) {
                set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, out_response->status,
                          "unexpected response type 0x%02X", out_response->message_type);
                return out_error->status;
            }
            if (out_response->request_id != request_id) {
                set_error(out_error, PRINTERCTL_CLIENT_ERR_CORRELATION, out_response->status,
                          "response id %u does not match request id %u",
                          out_response->request_id, request_id);
                return out_error->status;
            }
            if (out_response->status != PRINTERCTL_PLP1_SUCCESS) {
                char detail[PRINTERCTL_CLIENT_ERROR_MAX];
                detail[0] = '\0';
                if (out_response->payload_length > 0) {
                    (void)printerctl_plp1_decode_error_payload(
                        out_response->payload, out_response->payload_length,
                        detail, sizeof(detail), &plp_error);
                }
                if (detail[0] != '\0') {
                    set_error(out_error, PRINTERCTL_CLIENT_ERR_REMOTE, out_response->status,
                              "%s", detail);
                } else {
                    set_error(out_error, PRINTERCTL_CLIENT_ERR_REMOTE, out_response->status,
                              "remote status 0x%04X", out_response->status);
                }
                return out_error->status;
            }
            return PRINTERCTL_CLIENT_OK;
        }
        if (take == PRINTERCTL_PLP1_PARSER_FRAME_REJECTED ||
            take == PRINTERCTL_PLP1_PARSER_ERR_MALFORMED) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, plp_error.wire_status,
                      "malformed PLP response: %s", plp_error.message);
            return out_error->status;
        }
        if (take == PRINTERCTL_PLP1_PARSER_ERR_INVALID_ARGUMENT) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, 0, "%s", plp_error.message);
            return out_error->status;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                          (now.tv_nsec - start.tv_nsec) / 1000000L;
        int remaining_ms = timeout_ms - (int)elapsed_ms;
        if (remaining_ms < 0) {
            remaining_ms = 0;
        }

        struct pollfd pfd;
        pfd.fd = client->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = poll(&pfd, 1, remaining_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(out_error, PRINTERCTL_CLIENT_ERR_IO, 0, "poll socket: %s",
                      strerror(errno));
            return out_error->status;
        }
        if (rc == 0) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_TIMEOUT, 0, "timed out waiting for response");
            return out_error->status;
        }

        ssize_t n = recv(client->fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(out_error, PRINTERCTL_CLIENT_ERR_IO, 0, "read socket: %s",
                      strerror(errno));
            return out_error->status;
        }
        if (n == 0) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_IO, 0, "socket closed before response");
            return out_error->status;
        }

        size_t consumed = 0;
        if (printerctl_plp1_parser_feed(
                &parser, chunk, (size_t)n, &consumed, &plp_error) != PRINTERCTL_PLP1_OK) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, plp_error.wire_status,
                      "parse response: %s", plp_error.message);
            return out_error->status;
        }
        if (consumed != (size_t)n) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, 0,
                      "response buffer exhausted");
            return out_error->status;
        }
    }
}

printerctl_client_status_t printerctl_client_request(
    printerctl_client_t *client,
    uint8_t message_type,
    const uint8_t *payload,
    uint16_t payload_length,
    int timeout_ms,
    printerctl_plp1_frame_t *out_response,
    printerctl_client_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if (client == NULL || client->fd < 0 || out_response == NULL ||
        (payload == NULL && payload_length > 0) || timeout_ms < 0) {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT, 0,
                  "invalid client request arguments");
        return out_error->status;
    }

    uint32_t request_id = client->next_request_id++;
    if (client->next_request_id == 0) {
        client->next_request_id = 1;
    }

    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = message_type;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = request_id;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    frame.payload = payload;
    frame.payload_length = payload_length;

    size_t length = 0;
    printerctl_plp1_error_t plp_error;
    if (printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &plp_error) !=
        PRINTERCTL_PLP1_OK) {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, 0,
                  "encode request: %s", plp_error.message);
        return out_error->status;
    }

    printerctl_client_status_t status = write_full(client->fd, buffer, length, out_error);
    if (status != PRINTERCTL_CLIENT_OK) {
        return status;
    }
    return read_response(client, message_type, request_id, timeout_ms, out_response, out_error);
}

printerctl_client_status_t printerctl_client_ping(
    printerctl_client_t *client,
    int timeout_ms,
    printerctl_client_error_t *out_error)
{
    printerctl_plp1_frame_t response;
    printerctl_client_status_t status = printerctl_client_request(
        client, PRINTERCTL_PLP1_MSG_PING, NULL, 0, timeout_ms, &response, out_error);
    if (status != PRINTERCTL_CLIENT_OK) {
        return status;
    }
    printerctl_plp1_error_t plp_error;
    if (printerctl_plp1_expect_no_payload(&response, &plp_error) != PRINTERCTL_PLP1_OK) {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, plp_error.wire_status,
                  "PING response: %s", plp_error.message);
        return out_error->status;
    }
    return PRINTERCTL_CLIENT_OK;
}

printerctl_client_status_t printerctl_client_version(
    printerctl_client_t *client,
    int timeout_ms,
    printerctl_plp1_version_response_t *out_version,
    printerctl_client_error_t *out_error)
{
    printerctl_plp1_frame_t response;
    printerctl_client_status_t status = printerctl_client_request(
        client, PRINTERCTL_PLP1_MSG_VERSION, NULL, 0, timeout_ms, &response, out_error);
    if (status != PRINTERCTL_CLIENT_OK) {
        return status;
    }
    printerctl_plp1_error_t plp_error;
    if (printerctl_plp1_decode_version_response(
            response.payload, response.payload_length, out_version, &plp_error) != PRINTERCTL_PLP1_OK) {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, plp_error.wire_status,
                  "VERSION response: %s", plp_error.message);
        return out_error->status;
    }
    return PRINTERCTL_CLIENT_OK;
}

printerctl_client_status_t printerctl_client_status(
    printerctl_client_t *client,
    uint32_t requested_fields,
    int timeout_ms,
    printerctl_plp1_status_response_t *out_status,
    printerctl_client_error_t *out_error)
{
    uint8_t payload[4];
    size_t payload_length = 0;
    printerctl_plp1_error_t plp_error;
    if (printerctl_plp1_encode_status_request(
            requested_fields, payload, sizeof(payload), &payload_length, &plp_error) !=
        PRINTERCTL_PLP1_OK) {
        if (out_error != NULL) {
            set_error(out_error, PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT, 0,
                      "STATUS request: %s", plp_error.message);
        }
        return PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT;
    }

    printerctl_plp1_frame_t response;
    printerctl_client_status_t status = printerctl_client_request(
        client, PRINTERCTL_PLP1_MSG_STATUS, payload, (uint16_t)payload_length, timeout_ms,
        &response, out_error);
    if (status != PRINTERCTL_CLIENT_OK) {
        return status;
    }
    if (printerctl_plp1_decode_status_response(
            response.payload, response.payload_length, out_status, &plp_error) != PRINTERCTL_PLP1_OK) {
        set_error(out_error, PRINTERCTL_CLIENT_ERR_PROTOCOL, plp_error.wire_status,
                  "STATUS response: %s", plp_error.message);
        return out_error->status;
    }
    return PRINTERCTL_CLIENT_OK;
}
