#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "printerctl/daemon/socket.h"
#include "printerctl/protocol/plp1.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

typedef struct {
    printerctl_daemon_socket_server_t *server;
    printerctl_daemon_socket_status_t status;
    printerctl_daemon_socket_error_t error;
} serve_thread_t;

typedef struct {
    int master;
    printerctl_config_t config;
} mock_printer_t;

static void *serve_one_thread(void *arg)
{
    serve_thread_t *ctx = (serve_thread_t *)arg;
    ctx->status = printerctl_daemon_socket_serve_one(ctx->server, &ctx->error);
    return NULL;
}

static void make_temp_socket_path(char *out_path, size_t out_path_size, char *out_dir,
                                  size_t out_dir_size)
{
    snprintf(out_dir, out_dir_size, "/tmp/printerctl-socket-test-XXXXXX");
    check(mkdtemp(out_dir) != NULL, "mkdtemp succeeds");
    snprintf(out_path, out_path_size, "%s/sock", out_dir);
}

static void cleanup_temp_socket(const char *path, const char *dir)
{
    unlink(path);
    rmdir(dir);
}

static int connect_client(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    check(fd >= 0, "client socket succeeds");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    check(connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0, "client connects");
    return fd;
}

static void write_full(int fd, const uint8_t *buffer, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, buffer + written, length - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        check(n > 0, "write_full writes");
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
        check(n > 0, "read_full reads");
        received += (size_t)n;
    }
}

static uint32_t read_u32be(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
}

static size_t read_frame(int fd, uint8_t *buffer, size_t buffer_size)
{
    check(buffer_size >= PRINTERCTL_PLP1_HEADER_SIZE, "read frame buffer is large enough");
    read_full(fd, buffer, PRINTERCTL_PLP1_FRAME_PREFIX_SIZE);
    uint32_t frame_length = read_u32be(buffer);
    check(frame_length >= PRINTERCTL_PLP1_MIN_FRAME_LENGTH, "response frame length valid");
    check(frame_length <= PRINTERCTL_PLP1_MAX_FRAME_LENGTH, "response frame length within max");
    size_t total = PRINTERCTL_PLP1_FRAME_PREFIX_SIZE + (size_t)frame_length;
    check(total <= buffer_size, "response fits test buffer");
    read_full(fd, buffer + PRINTERCTL_PLP1_FRAME_PREFIX_SIZE, total - PRINTERCTL_PLP1_FRAME_PREFIX_SIZE);
    return total;
}

static size_t encode_request(uint8_t type, uint32_t request_id, const uint8_t *payload,
                             uint16_t payload_length, uint8_t *out, size_t out_size)
{
    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = type;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = request_id;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    frame.payload = payload;
    frame.payload_length = payload_length;

    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_frame(&frame, out, out_size, &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encode request");
    return length;
}

static void decode_response(const uint8_t *buffer, size_t length, printerctl_plp1_frame_t *out)
{
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_frame(buffer, length, out, &error) == PRINTERCTL_PLP1_OK,
          "decode response");
}

static void start_runtime_and_socket(printerctl_daemon_runtime_t *runtime,
                                     printerctl_daemon_socket_server_t *server,
                                     const char *path)
{
    printerctl_config_t config;
    printerctl_config_set_defaults(&config);
    printerctl_daemon_error_t runtime_error;
    check(printerctl_daemon_runtime_start_with_config(runtime, &config, NULL, NULL,
                                                      &runtime_error) == PRINTERCTL_DAEMON_OK,
          "runtime starts");

    printerctl_daemon_socket_error_t socket_error;
    check(printerctl_daemon_socket_listen(server, path, runtime, &socket_error) ==
              PRINTERCTL_DAEMON_SOCKET_OK,
          "socket listens");
}

static void start_runtime_and_socket_with_config(printerctl_daemon_runtime_t *runtime,
                                                 printerctl_daemon_socket_server_t *server,
                                                 const char *path,
                                                 const printerctl_config_t *config)
{
    printerctl_daemon_error_t runtime_error;
    check(printerctl_daemon_runtime_start_with_config(runtime, config, NULL, NULL,
                                                      &runtime_error) == PRINTERCTL_DAEMON_OK,
          "runtime starts");

    printerctl_daemon_socket_error_t socket_error;
    check(printerctl_daemon_socket_listen(server, path, runtime, &socket_error) ==
              PRINTERCTL_DAEMON_SOCKET_OK,
          "socket listens");
}

static void mock_open(mock_printer_t *mock)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    check(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "create pty");
    const char *name = ptsname(master);
    check(name != NULL, "ptsname");
    mock->master = master;
    printerctl_config_set_defaults(&mock->config);
    snprintf(mock->config.port, sizeof(mock->config.port), "%s", name);
    mock->config.timeout_ms = 1000;
}

static void mock_close(mock_printer_t *mock)
{
    close(mock->master);
}

static void mock_reply_ok(mock_printer_t *mock)
{
    check(write(mock->master, "ok\n", 3) == 3, "mock writes ok");
}

static void mock_expect_command(mock_printer_t *mock, const char *expected)
{
    char buffer[128];
    size_t received = 0;
    while (received < sizeof(buffer) - 1) {
        struct pollfd pfd;
        pfd.fd = mock->master;
        pfd.events = POLLIN;
        pfd.revents = 0;
        check(poll(&pfd, 1, 1000) > 0, "printer received command");
        ssize_t n = read(mock->master, buffer + received, sizeof(buffer) - 1 - received);
        check(n > 0, "read printer command");
        received += (size_t)n;
        buffer[received] = '\0';
        if (strchr(buffer, '\n') != NULL) {
            break;
        }
    }
    check(strcmp(buffer, expected) == 0, "printer command matches expected");
}

static void write_gcode_file(char *path, size_t path_size, const char *contents)
{
    snprintf(path, path_size, "/tmp/printerctl-socket-print-XXXXXX");
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    size_t length = strlen(contents);
    check(write(fd, contents, length) == (ssize_t)length, "write gcode");
    close(fd);
}

static void send_print_start(int client, uint32_t request_id, const char *gcode_path)
{
    uint8_t payload[PRINTERCTL_PLP1_PRINT_START_PATH_MAX + 2];
    size_t payload_length = 0;
    printerctl_plp1_error_t plp_error;
    check(printerctl_plp1_encode_print_start_request(gcode_path, payload, sizeof(payload),
                                                     &payload_length, &plp_error) ==
              PRINTERCTL_PLP1_OK,
          "encode PRINT_START payload");

    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_PRINT_START, request_id, payload,
                                           (uint16_t)payload_length, request, sizeof(request));
    write_full(client, request, request_length);
}

static printerctl_plp1_frame_t read_response_frame(int client, uint8_t expected_type,
                                                   uint32_t expected_request_id)
{
    uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(client, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    decode_response(response, response_length, &decoded);
    check(decoded.message_type == expected_type, "response type matches request");
    check(decoded.request_id == expected_request_id, "response id matches request");
    return decoded;
}

static void send_status_request(int client, uint32_t request_id, uint32_t fields)
{
    uint8_t payload[8];
    size_t payload_length = 0;
    printerctl_plp1_error_t plp_error;
    check(printerctl_plp1_encode_status_request(fields, payload, sizeof(payload),
                                                &payload_length, &plp_error) ==
              PRINTERCTL_PLP1_OK,
          "encode STATUS payload");
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_STATUS, request_id, payload,
                                           (uint16_t)payload_length, request, sizeof(request));
    write_full(client, request, request_length);
}

static void send_empty_request(int client, uint8_t type, uint32_t request_id)
{
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_request(type, request_id, NULL, 0, request, sizeof(request));
    write_full(client, request, request_length);
}

static printerctl_plp1_status_response_t read_status_response(int client, uint32_t request_id)
{
    uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(client, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    decode_response(response, response_length, &decoded);
    check(decoded.message_type == PRINTERCTL_PLP1_MSG_STATUS, "STATUS response type");
    check(decoded.request_id == request_id, "STATUS response id");
    check(decoded.status == PRINTERCTL_PLP1_SUCCESS, "STATUS succeeds");

    printerctl_plp1_status_response_t status;
    printerctl_plp1_error_t plp_error;
    check(printerctl_plp1_decode_status_response(decoded.payload, decoded.payload_length, &status,
                                                 &plp_error) == PRINTERCTL_PLP1_OK,
          "decode STATUS response");
    return status;
}

static void wait_until_execution(printerctl_daemon_runtime_t *runtime, uint8_t state)
{
    printerctl_daemon_error_t error;
    for (;;) {
        printerctl_daemon_event_t event;
        check(printerctl_daemon_runtime_wait_event(runtime, &event, &error) == PRINTERCTL_DAEMON_OK,
              "wait print event");
        printerctl_daemon_snapshot_t snapshot;
        printerctl_daemon_runtime_snapshot(runtime, &snapshot);
        if (snapshot.execution_state == state) {
            return;
        }
    }
}

static void stop_runtime_and_socket(printerctl_daemon_runtime_t *runtime,
                                    printerctl_daemon_socket_server_t *server)
{
    printerctl_daemon_socket_close(server);
    printerctl_daemon_error_t runtime_error;
    check(printerctl_daemon_runtime_shutdown(runtime, &runtime_error) == PRINTERCTL_DAEMON_OK,
          "runtime shuts down");
}

static void serve_one_joined(printerctl_daemon_socket_server_t *server, int client_fd)
{
    serve_thread_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;

    pthread_t thread;
    check(pthread_create(&thread, NULL, serve_one_thread, &ctx) == 0, "serve thread starts");
    shutdown(client_fd, SHUT_WR);
    check(pthread_join(thread, NULL) == 0, "serve thread joins");
    check(ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "serve_one succeeds");
}

static void test_ping_fragmented(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket(&runtime, &server, path);

    serve_thread_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = &server;
    pthread_t thread;
    check(pthread_create(&thread, NULL, serve_one_thread, &ctx) == 0, "serve thread starts");

    int client = connect_client(path);
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_PING, 42, NULL, 0, request,
                                           sizeof(request));
    for (size_t i = 0; i < request_length; ++i) {
        write_full(client, request + i, 1);
    }

    uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(client, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    decode_response(response, response_length, &decoded);
    check(decoded.message_type == PRINTERCTL_PLP1_MSG_PING, "PING response type");
    check(decoded.flags == PRINTERCTL_PLP1_FLAG_RESPONSE, "PING response flags");
    check(decoded.request_id == 42, "PING response request_id");
    check(decoded.status == PRINTERCTL_PLP1_SUCCESS, "PING response status");
    check(decoded.payload_length == 0, "PING response has no payload");

    shutdown(client, SHUT_WR);
    check(pthread_join(thread, NULL) == 0, "serve thread joins");
    check(ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "serve_one succeeds");
    close(client);

    stop_runtime_and_socket(&runtime, &server);
    cleanup_temp_socket(path, dir);
}

static void test_version_and_status_coalesced(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket(&runtime, &server, path);

    serve_thread_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = &server;
    pthread_t thread;
    check(pthread_create(&thread, NULL, serve_one_thread, &ctx) == 0, "serve thread starts");

    int client = connect_client(path);
    uint8_t status_payload[8];
    size_t status_payload_length = 0;
    printerctl_plp1_error_t plp_error;
    uint32_t requested = PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE |
                         PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE |
                         PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS;
    check(printerctl_plp1_encode_status_request(requested, status_payload, sizeof(status_payload),
                                                &status_payload_length, &plp_error) ==
              PRINTERCTL_PLP1_OK,
          "encode STATUS request payload");

    uint8_t requests[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t version_length = encode_request(PRINTERCTL_PLP1_MSG_VERSION, 7, NULL, 0, requests,
                                           sizeof(requests));
    size_t status_length = encode_request(PRINTERCTL_PLP1_MSG_STATUS, 8, status_payload,
                                          (uint16_t)status_payload_length, requests + version_length,
                                          sizeof(requests) - version_length);
    write_full(client, requests, version_length + status_length);

    uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(client, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    decode_response(response, response_length, &decoded);
    check(decoded.message_type == PRINTERCTL_PLP1_MSG_VERSION, "VERSION response type");
    check(decoded.request_id == 7, "VERSION request_id");
    printerctl_plp1_version_response_t version;
    check(printerctl_plp1_decode_version_response(decoded.payload, decoded.payload_length, &version,
                                                  &plp_error) == PRINTERCTL_PLP1_OK,
          "decode VERSION payload");
    check(version.protocol_version == PRINTERCTL_PLP1_VERSION_1, "VERSION protocol");
    check(version.capability_bitmap ==
              (PRINTERCTL_PLP1_CAP_PRINTER_STATUS | PRINTERCTL_PLP1_CAP_PRINT_CONTROL),
          "VERSION exposes M16 status and print-control capabilities");

    response_length = read_frame(client, response, sizeof(response));
    decode_response(response, response_length, &decoded);
    check(decoded.message_type == PRINTERCTL_PLP1_MSG_STATUS, "STATUS response type");
    check(decoded.request_id == 8, "STATUS request_id");
    printerctl_plp1_status_response_t status;
    check(printerctl_plp1_decode_status_response(decoded.payload, decoded.payload_length, &status,
                                                 &plp_error) == PRINTERCTL_PLP1_OK,
          "decode STATUS payload");
    check(status.present_fields == (PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE |
                                    PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE |
                                    PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS),
          "STATUS reports M16 runtime and execution fields");
    check(status.daemon_state == PRINTERCTL_PLP1_DAEMON_STATE_RUNNING, "daemon state running");
    check(status.worker_state == PRINTERCTL_PLP1_WORKER_STATE_READY, "worker state ready");

    shutdown(client, SHUT_WR);
    check(pthread_join(thread, NULL) == 0, "serve thread joins");
    check(ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "serve_one succeeds");
    close(client);

    stop_runtime_and_socket(&runtime, &server);
    cleanup_temp_socket(path, dir);
}

static void test_disconnect_reconnect(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket(&runtime, &server, path);

    for (int i = 0; i < 2; ++i) {
        int client = connect_client(path);
        serve_one_joined(&server, client);
        close(client);
    }

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_RUNNING,
          "client disconnects do not stop runtime");

    stop_runtime_and_socket(&runtime, &server);
    cleanup_temp_socket(path, dir);
}

static void test_stale_socket_replaced(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    int stale_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    check(stale_fd >= 0, "stale socket fd");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    check(bind(stale_fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0, "bind stale socket");
    close(stale_fd);

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket(&runtime, &server, path);
    stop_runtime_and_socket(&runtime, &server);
    cleanup_temp_socket(path, dir);
}

static void test_live_socket_not_unlinked(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    int live_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    check(live_fd >= 0, "live socket fd");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    check(bind(live_fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0, "bind live socket");
    check(listen(live_fd, 1) == 0, "listen live socket");

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t runtime_error;
    check(printerctl_daemon_runtime_start(&runtime, NULL, NULL, &runtime_error) ==
              PRINTERCTL_DAEMON_OK,
          "runtime starts");

    printerctl_daemon_socket_server_t server;
    printerctl_daemon_socket_error_t socket_error;
    check(printerctl_daemon_socket_listen(&server, path, &runtime, &socket_error) ==
              PRINTERCTL_DAEMON_SOCKET_ERR_IN_USE,
          "live socket is rejected");

    struct stat st;
    check(lstat(path, &st) == 0 && S_ISSOCK(st.st_mode), "live socket path remains");

    close(live_fd);
    printerctl_daemon_runtime_shutdown(&runtime, &runtime_error);
    cleanup_temp_socket(path, dir);
}

static void test_malformed_client_does_not_kill_server(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket(&runtime, &server, path);

    serve_thread_t bad_ctx;
    memset(&bad_ctx, 0, sizeof(bad_ctx));
    bad_ctx.server = &server;
    pthread_t bad_thread;
    check(pthread_create(&bad_thread, NULL, serve_one_thread, &bad_ctx) == 0,
          "bad serve thread starts");
    int bad_client = connect_client(path);
    uint8_t too_large_prefix[4] = {0x00, 0x01, 0x00, 0x01};
    write_full(bad_client, too_large_prefix, sizeof(too_large_prefix));
    check(pthread_join(bad_thread, NULL) == 0, "bad serve thread joins");
    check(bad_ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "malformed client is contained");
    close(bad_client);

    serve_thread_t good_ctx;
    memset(&good_ctx, 0, sizeof(good_ctx));
    good_ctx.server = &server;
    pthread_t good_thread;
    check(pthread_create(&good_thread, NULL, serve_one_thread, &good_ctx) == 0,
          "good serve thread starts");
    int good_client = connect_client(path);
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_PING, 9, NULL, 0, request,
                                           sizeof(request));
    write_full(good_client, request, request_length);
    uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(good_client, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    decode_response(response, response_length, &decoded);
    check(decoded.message_type == PRINTERCTL_PLP1_MSG_PING, "server accepts next client");
    shutdown(good_client, SHUT_WR);
    check(pthread_join(good_thread, NULL) == 0, "good serve thread joins");
    check(good_ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "good serve succeeds");
    close(good_client);

    stop_runtime_and_socket(&runtime, &server);
    cleanup_temp_socket(path, dir);
}

static void test_print_start_over_socket_survives_client_disconnect(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nM114\n");

    mock_printer_t mock;
    mock_open(&mock);

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket_with_config(&runtime, &server, socket_path, &mock.config);

    serve_thread_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = &server;
    pthread_t thread;
    check(pthread_create(&thread, NULL, serve_one_thread, &ctx) == 0, "serve thread starts");

    int client = connect_client(socket_path);
    send_print_start(client, 55, gcode_path);
    printerctl_plp1_frame_t decoded =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_START, 55);
    check(decoded.message_type == PRINTERCTL_PLP1_MSG_PRINT_START, "PRINT_START response type");
    check(decoded.request_id == 55, "PRINT_START response id");
    check(decoded.status == PRINTERCTL_PLP1_SUCCESS, "PRINT_START accepted");

    shutdown(client, SHUT_WR);
    check(pthread_join(thread, NULL) == 0, "serve thread joins");
    check(ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "serve_one succeeds");
    close(client);

    mock_expect_command(&mock, "M105\n");
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "M114\n");
    mock_reply_ok(&mock);
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    stop_runtime_and_socket(&runtime, &server);
    mock_close(&mock);
    unlink(gcode_path);
    cleanup_temp_socket(socket_path, dir);
}

static void test_second_client_observes_active_print_after_start_disconnect(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nM114\n");

    mock_printer_t mock;
    mock_open(&mock);

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket_with_config(&runtime, &server, socket_path, &mock.config);

    serve_thread_t start_ctx;
    memset(&start_ctx, 0, sizeof(start_ctx));
    start_ctx.server = &server;
    pthread_t start_thread;
    check(pthread_create(&start_thread, NULL, serve_one_thread, &start_ctx) == 0,
          "START serve thread starts");

    int client_a = connect_client(socket_path);
    send_print_start(client_a, 56, gcode_path);
    printerctl_plp1_frame_t start_response =
        read_response_frame(client_a, PRINTERCTL_PLP1_MSG_PRINT_START, 56);
    check(start_response.status == PRINTERCTL_PLP1_SUCCESS, "client A START accepted");

    shutdown(client_a, SHUT_WR);
    check(pthread_join(start_thread, NULL) == 0, "START serve thread joins");
    check(start_ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "START serve succeeds");
    close(client_a);

    mock_expect_command(&mock, "M105\n");

    serve_thread_t status_ctx;
    memset(&status_ctx, 0, sizeof(status_ctx));
    status_ctx.server = &server;
    pthread_t status_thread;
    check(pthread_create(&status_thread, NULL, serve_one_thread, &status_ctx) == 0,
          "STATUS serve thread starts");

    int client_b = connect_client(socket_path);
    send_status_request(client_b, 57, PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE);
    printerctl_plp1_status_response_t status = read_status_response(client_b, 57);
    check((status.present_fields & PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE) != 0,
          "STATUS includes execution state");
    check(status.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING,
          "client B sees daemon-owned active execution");

    shutdown(client_b, SHUT_WR);
    check(pthread_join(status_thread, NULL) == 0, "STATUS serve thread joins");
    check(status_ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "STATUS serve succeeds");
    close(client_b);

    mock_reply_ok(&mock);
    mock_expect_command(&mock, "M114\n");
    mock_reply_ok(&mock);
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    stop_runtime_and_socket(&runtime, &server);
    mock_close(&mock);
    unlink(gcode_path);
    cleanup_temp_socket(socket_path, dir);
}

static uint16_t run_print_start_status_case(const char *gcode_path,
                                            const printerctl_config_t *config)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket_with_config(&runtime, &server, socket_path, config);

    serve_thread_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = &server;
    pthread_t thread;
    check(pthread_create(&thread, NULL, serve_one_thread, &ctx) == 0, "serve thread starts");

    int client = connect_client(socket_path);
    send_print_start(client, 60, gcode_path);
    printerctl_plp1_frame_t response =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_START, 60);
    uint16_t status = response.status;

    shutdown(client, SHUT_WR);
    check(pthread_join(thread, NULL) == 0, "serve thread joins");
    check(ctx.status == PRINTERCTL_DAEMON_SOCKET_OK, "serve_one succeeds");
    close(client);

    stop_runtime_and_socket(&runtime, &server);
    cleanup_temp_socket(socket_path, dir);
    return status;
}

static void test_print_start_error_mapping_over_socket(void)
{
    printerctl_config_t config;
    printerctl_config_set_defaults(&config);

    uint16_t missing = run_print_start_status_case("/tmp/printerctl-missing-source.gcode", &config);
    check(missing == PRINTERCTL_PLP1_FILE_NOT_FOUND, "missing source maps to FILE_NOT_FOUND");

    char unreadable_path[128];
    write_gcode_file(unreadable_path, sizeof(unreadable_path), "M105\n");
    check(chmod(unreadable_path, 0000) == 0, "make source unreadable");
    uint16_t unreadable = run_print_start_status_case(unreadable_path, &config);
    check(unreadable == PRINTERCTL_PLP1_FILE_NOT_READABLE,
          "unreadable source maps to FILE_NOT_READABLE");
    chmod(unreadable_path, 0600);
    unlink(unreadable_path);

    char serial_path[128];
    write_gcode_file(serial_path, sizeof(serial_path), "M105\n");
    snprintf(config.port, sizeof(config.port), "/tmp/printerctl-missing-serial");
    uint16_t serial = run_print_start_status_case(serial_path, &config);
    check(serial == PRINTERCTL_PLP1_SERIAL_ERROR, "serial startup failure maps to SERIAL_ERROR");
    unlink(serial_path);
}

static void test_worker_unavailable_maps_to_plp_status(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\n");

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket(&runtime, &server, socket_path);
    close(runtime.cmd_pipe[1]);
    runtime.cmd_pipe[1] = -1;

    serve_thread_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = &server;
    pthread_t thread;
    check(pthread_create(&thread, NULL, serve_one_thread, &ctx) == 0, "serve thread starts");

    int client = connect_client(socket_path);
    send_print_start(client, 61, gcode_path);
    printerctl_plp1_frame_t response =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_START, 61);
    check(response.status == PRINTERCTL_PLP1_WORKER_UNAVAILABLE,
          "worker command failure maps to WORKER_UNAVAILABLE");

    shutdown(client, SHUT_WR);
    check(pthread_join(thread, NULL) == 0, "serve thread joins");
    close(client);

    printerctl_daemon_socket_close(&server);
    printerctl_daemon_error_t runtime_error;
    (void)printerctl_daemon_runtime_shutdown(&runtime, &runtime_error);
    unlink(gcode_path);
    cleanup_temp_socket(socket_path, dir);
}

static void test_second_print_start_maps_to_busy_over_socket(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nM114\n");

    mock_printer_t mock;
    mock_open(&mock);

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_socket_server_t server;
    start_runtime_and_socket_with_config(&runtime, &server, socket_path, &mock.config);

    serve_thread_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = &server;
    pthread_t thread;
    check(pthread_create(&thread, NULL, serve_one_thread, &ctx) == 0, "serve thread starts");

    int client = connect_client(socket_path);
    send_print_start(client, 62, gcode_path);
    printerctl_plp1_frame_t first =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_START, 62);
    check(first.status == PRINTERCTL_PLP1_SUCCESS, "first PRINT_START accepted");
    mock_expect_command(&mock, "M105\n");

    send_empty_request(client, PRINTERCTL_PLP1_MSG_PRINT_PAUSE, 63);
    mock_reply_ok(&mock);
    printerctl_plp1_frame_t pause =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_PAUSE, 63);
    check(pause.status == PRINTERCTL_PLP1_SUCCESS, "PAUSE accepted before second START");

    send_print_start(client, 64, gcode_path);
    printerctl_plp1_frame_t second =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_START, 64);
    check(second.status == PRINTERCTL_PLP1_BUSY, "active execution maps second START to BUSY");

    send_empty_request(client, PRINTERCTL_PLP1_MSG_PRINT_RESUME, 65);
    printerctl_plp1_frame_t resume =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_RESUME, 65);
    check(resume.status == PRINTERCTL_PLP1_SUCCESS, "RESUME accepted after BUSY check");
    mock_expect_command(&mock, "M114\n");
    mock_reply_ok(&mock);
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    shutdown(client, SHUT_WR);
    check(pthread_join(thread, NULL) == 0, "serve thread joins");
    close(client);

    stop_runtime_and_socket(&runtime, &server);
    mock_close(&mock);
    unlink(gcode_path);
    cleanup_temp_socket(socket_path, dir);
}

int main(void)
{
    test_ping_fragmented();
    test_version_and_status_coalesced();
    test_disconnect_reconnect();
    test_stale_socket_replaced();
    test_live_socket_not_unlinked();
    test_malformed_client_does_not_kill_server();
    test_print_start_over_socket_survives_client_disconnect();
    test_second_client_observes_active_print_after_start_disconnect();
    test_print_start_error_mapping_over_socket();
    test_worker_unavailable_maps_to_plp_status();
    test_second_print_start_maps_to_busy_over_socket();
    return 0;
}
