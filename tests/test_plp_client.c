#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "printerctl/client/plp_client.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

typedef enum {
    SERVER_VALID,
    SERVER_ID_MISMATCH,
    SERVER_MALFORMED,
    SERVER_UNSUPPORTED_VERSION,
    SERVER_RESET,
    SERVER_SILENT,
    SERVER_REMOTE_ERROR
} server_mode_t;

typedef struct {
    char path[108];
    int listen_fd;
    server_mode_t mode;
    uint8_t expected_type;
    uint32_t requested_fields;
    pthread_t thread;
} server_t;

static uint32_t read_u32be(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
}

static void write_full(int fd, const uint8_t *buffer, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, buffer + written, length - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        check(n > 0, "write_full");
        written += (size_t)n;
    }
}

static void read_full(int fd, uint8_t *buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        ssize_t n = read(fd, buffer + received, length - received);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        check(n > 0, "read_full");
        received += (size_t)n;
    }
}

static size_t read_frame(int fd, uint8_t *buffer, size_t buffer_size)
{
    read_full(fd, buffer, PRINTERCTL_PLP1_FRAME_PREFIX_SIZE);
    uint32_t frame_length = read_u32be(buffer);
    size_t total = PRINTERCTL_PLP1_FRAME_PREFIX_SIZE + (size_t)frame_length;
    check(total <= buffer_size, "frame fits");
    read_full(fd, buffer + PRINTERCTL_PLP1_FRAME_PREFIX_SIZE,
              total - PRINTERCTL_PLP1_FRAME_PREFIX_SIZE);
    return total;
}

static void send_frame(int fd, uint8_t type, uint16_t flags, uint32_t request_id,
                       uint16_t status, const uint8_t *payload, uint16_t payload_length)
{
    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = type;
    frame.flags = flags;
    frame.request_id = request_id;
    frame.status = status;
    frame.payload = payload;
    frame.payload_length = payload_length;
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encode response");
    write_full(fd, buffer, length);
}

static void *server_thread(void *arg)
{
    server_t *server = arg;
    int client = accept(server->listen_fd, NULL, NULL);
    check(client >= 0, "accept");

    uint8_t request_buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = read_frame(client, request_buffer, sizeof(request_buffer));
    printerctl_plp1_frame_t request;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_frame(request_buffer, request_length, &request, &error) ==
              PRINTERCTL_PLP1_OK,
          "decode request");
    check(request.message_type == server->expected_type, "request type");

    if (server->mode == SERVER_RESET) {
        close(client);
        return NULL;
    }
    if (server->mode == SERVER_SILENT) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
        nanosleep(&delay, NULL);
        close(client);
        return NULL;
    }
    if (server->mode == SERVER_MALFORMED) {
        const uint8_t malformed[] = {0, 0, 0, 1};
        write_full(client, malformed, sizeof(malformed));
        close(client);
        return NULL;
    }
    if (server->mode == SERVER_REMOTE_ERROR) {
        send_frame(client, request.message_type,
                   PRINTERCTL_PLP1_FLAG_RESPONSE | PRINTERCTL_PLP1_FLAG_ERROR,
                   request.request_id, PRINTERCTL_PLP1_BUSY, NULL, 0);
        close(client);
        return NULL;
    }

    uint32_t response_id =
        server->mode == SERVER_ID_MISMATCH ? request.request_id + 1 : request.request_id;
    uint8_t payload[64];
    size_t payload_length = 0;
    if (request.message_type == PRINTERCTL_PLP1_MSG_VERSION) {
        printerctl_plp1_version_response_t version = {
            PRINTERCTL_PLP1_VERSION_1,
            PRINTERCTL_PLP1_CAP_PRINT_CONTROL
        };
        check(printerctl_plp1_encode_version_response(
                  &version, payload, sizeof(payload), &payload_length, &error) ==
                  PRINTERCTL_PLP1_OK,
              "encode VERSION payload");
    } else if (request.message_type == PRINTERCTL_PLP1_MSG_STATUS) {
        uint32_t requested = 0;
        check(printerctl_plp1_decode_status_request(
                  request.payload, request.payload_length, &requested, &error) ==
                  PRINTERCTL_PLP1_OK,
              "decode STATUS request");
        server->requested_fields = requested;
        printerctl_plp1_status_response_t status;
        memset(&status, 0, sizeof(status));
        status.present_fields = PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE |
                                PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE;
        status.daemon_state = PRINTERCTL_PLP1_DAEMON_STATE_RUNNING;
        status.worker_state = PRINTERCTL_PLP1_WORKER_STATE_READY;
        check(printerctl_plp1_encode_status_response(
                  &status, payload, sizeof(payload), &payload_length, &error) ==
                  PRINTERCTL_PLP1_OK,
              "encode STATUS payload");
    }

    if (server->mode == SERVER_UNSUPPORTED_VERSION) {
        uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
        printerctl_plp1_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.version = PRINTERCTL_PLP1_VERSION_1;
        frame.message_type = request.message_type;
        frame.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
        frame.request_id = response_id;
        frame.status = PRINTERCTL_PLP1_SUCCESS;
        size_t length = 0;
        check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
                  PRINTERCTL_PLP1_OK,
              "encode unsupported-version response");
        buffer[4] = 2;
        write_full(client, buffer, length);
    } else {
        send_frame(client, request.message_type, PRINTERCTL_PLP1_FLAG_RESPONSE, response_id,
                   PRINTERCTL_PLP1_SUCCESS, payload, (uint16_t)payload_length);
    }
    close(client);
    return NULL;
}

static void server_start(server_t *server, server_mode_t mode, uint8_t expected_type)
{
    memset(server, 0, sizeof(*server));
    snprintf(server->path, sizeof(server->path), "/tmp/printerctl-client-test-XXXXXX");
    char *dir = mkdtemp(server->path);
    check(dir != NULL, "mkdtemp");
    size_t used = strlen(server->path);
    snprintf(server->path + used, sizeof(server->path) - used, "/sock");
    server->mode = mode;
    server->expected_type = expected_type;
    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    check(server->listen_fd >= 0, "server socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", server->path);
    check(bind(server->listen_fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0, "bind");
    check(listen(server->listen_fd, 1) == 0, "listen");
    check(pthread_create(&server->thread, NULL, server_thread, server) == 0, "pthread_create");
}

static void server_stop(server_t *server)
{
    pthread_join(server->thread, NULL);
    close(server->listen_fd);
    unlink(server->path);
    char dir[108];
    snprintf(dir, sizeof(dir), "%s", server->path);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        rmdir(dir);
    }
}

static void test_absent_socket(void)
{
    printerctl_client_t client;
    printerctl_client_error_t error;
    check(printerctl_client_connect(
              &client, "/tmp/printerctl-client-missing.sock", &error) ==
              PRINTERCTL_CLIENT_ERR_SOCKET_UNAVAILABLE,
          "missing socket is unavailable");
}

static void test_ping_version_status(void)
{
    {
        server_t server;
        server_start(&server, SERVER_VALID, PRINTERCTL_PLP1_MSG_PING);
        printerctl_client_t client;
        printerctl_client_error_t error;
        check(printerctl_client_connect(&client, server.path, &error) == PRINTERCTL_CLIENT_OK,
              "connect PING");
        check(printerctl_client_ping(&client, 1000, &error) == PRINTERCTL_CLIENT_OK,
              "PING succeeds");
        printerctl_client_close(&client);
        server_stop(&server);
    }
    {
        server_t server;
        server_start(&server, SERVER_VALID, PRINTERCTL_PLP1_MSG_VERSION);
        printerctl_client_t client;
        printerctl_client_error_t error;
        printerctl_plp1_version_response_t version;
        check(printerctl_client_connect(&client, server.path, &error) == PRINTERCTL_CLIENT_OK,
              "connect VERSION");
        check(printerctl_client_version(&client, 1000, &version, &error) == PRINTERCTL_CLIENT_OK,
              "VERSION succeeds");
        check(version.protocol_version == PRINTERCTL_PLP1_VERSION_1, "VERSION protocol");
        check(version.capability_bitmap == PRINTERCTL_PLP1_CAP_PRINT_CONTROL,
              "VERSION capabilities");
        printerctl_client_close(&client);
        server_stop(&server);
    }
    {
        server_t server;
        server_start(&server, SERVER_VALID, PRINTERCTL_PLP1_MSG_STATUS);
        printerctl_client_t client;
        printerctl_client_error_t error;
        printerctl_plp1_status_response_t status;
        check(printerctl_client_connect(&client, server.path, &error) == PRINTERCTL_CLIENT_OK,
              "connect STATUS");
        check(printerctl_client_status(
                  &client, PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE, 1000, &status, &error) ==
                  PRINTERCTL_CLIENT_OK,
              "STATUS succeeds");
        check(server.requested_fields == PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE,
              "STATUS requested_fields sent");
        check(status.daemon_state == PRINTERCTL_PLP1_DAEMON_STATE_RUNNING,
              "STATUS daemon state");
        check(status.worker_state == PRINTERCTL_PLP1_WORKER_STATE_READY,
              "STATUS worker state");
        printerctl_client_close(&client);
        server_stop(&server);
    }
}

static void test_faults(void)
{
    const struct {
        server_mode_t mode;
        printerctl_client_status_t expected;
        const char *name;
    } cases[] = {
        {SERVER_ID_MISMATCH, PRINTERCTL_CLIENT_ERR_CORRELATION, "request id mismatch"},
        {SERVER_MALFORMED, PRINTERCTL_CLIENT_ERR_PROTOCOL, "malformed response"},
        {SERVER_UNSUPPORTED_VERSION, PRINTERCTL_CLIENT_ERR_PROTOCOL, "unsupported version"},
        {SERVER_RESET, PRINTERCTL_CLIENT_ERR_IO, "connection reset"},
        {SERVER_SILENT, PRINTERCTL_CLIENT_ERR_TIMEOUT, "bounded wait timeout"},
        {SERVER_REMOTE_ERROR, PRINTERCTL_CLIENT_ERR_REMOTE, "remote status"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        server_t server;
        server_start(&server, cases[i].mode, PRINTERCTL_PLP1_MSG_PING);
        printerctl_client_t client;
        printerctl_client_error_t error;
        check(printerctl_client_connect(&client, server.path, &error) == PRINTERCTL_CLIENT_OK,
              "connect fault case");
        check(printerctl_client_ping(&client, 10, &error) == cases[i].expected, cases[i].name);
        printerctl_client_close(&client);
        server_stop(&server);
    }
}

static void test_invalid_status_request(void)
{
    printerctl_client_t client;
    memset(&client, 0, sizeof(client));
    client.fd = -1;
    printerctl_plp1_status_response_t status;
    printerctl_client_error_t error;
    check(printerctl_client_status(
              &client, PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK << 16, 1000, &status, &error) ==
              PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT,
          "invalid STATUS requested_fields rejected before I/O");
}

int main(void)
{
    test_absent_socket();
    test_ping_version_status();
    test_faults();
    test_invalid_status_request();
    printf("All PLP client checks passed\n");
    return 0;
}
