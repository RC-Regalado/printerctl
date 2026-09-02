#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "printerctl/cli.h"
#include "printerctl/client/plp_client.h"
#include "printerctl/protocol/plp1.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TEST_PRINTERCTLD_PATH
#error TEST_PRINTERCTLD_PATH must be defined
#endif
#ifndef TEST_PRINTERCTL_PATH
#error TEST_PRINTERCTL_PATH must be defined
#endif

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

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

static int connect_with_retry(const char *path)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        check(fd >= 0, "client socket");

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        struct timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = 10000000L;
        nanosleep(&delay, NULL);
    }
    check(0, "connect_with_retry connects");
    return -1;
}

static void make_temp_socket_path(char *out_path, size_t out_path_size, char *out_dir,
                                  size_t out_dir_size)
{
    snprintf(out_dir, out_dir_size, "/tmp/printerctld-test-XXXXXX");
    check(mkdtemp(out_dir) != NULL, "mkdtemp");
    snprintf(out_path, out_path_size, "%s/sock", out_dir);
}

static size_t encode_ping(uint8_t *out, size_t out_size)
{
    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_PING;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = 123;
    frame.status = PRINTERCTL_PLP1_SUCCESS;

    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_frame(&frame, out, out_size, &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encode PING request");
    return length;
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

static size_t read_frame(int fd, uint8_t *buffer, size_t buffer_size)
{
    read_full(fd, buffer, PRINTERCTL_PLP1_FRAME_PREFIX_SIZE);
    uint32_t frame_length = read_u32be(buffer);
    check(frame_length >= PRINTERCTL_PLP1_MIN_FRAME_LENGTH, "frame length minimum");
    check(frame_length <= PRINTERCTL_PLP1_MAX_FRAME_LENGTH, "frame length maximum");
    size_t total = PRINTERCTL_PLP1_FRAME_PREFIX_SIZE + (size_t)frame_length;
    check(total <= buffer_size, "frame fits");
    read_full(fd, buffer + PRINTERCTL_PLP1_FRAME_PREFIX_SIZE,
              total - PRINTERCTL_PLP1_FRAME_PREFIX_SIZE);
    return total;
}

static pid_t start_printerctld(const char *path)
{
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        execl(TEST_PRINTERCTLD_PATH, "printerctld", "--socket", path, (char *)NULL);
        _exit(127);
    }
    return child;
}

static pid_t start_printerctld_with_port(const char *socket_path, const char *port_path,
                                         const char *state_home)
{
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        if (state_home != NULL) {
            setenv("XDG_STATE_HOME", state_home, 1);
        }
        execl(TEST_PRINTERCTLD_PATH, "printerctld", "--socket", socket_path, "--port", port_path,
              "--timeout", "1000", (char *)NULL);
        _exit(127);
    }
    return child;
}

static pid_t start_printerctld_default_with_port(const char *port_path)
{
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        execl(TEST_PRINTERCTLD_PATH, "printerctld", "--port", port_path, "--timeout", "1000",
              (char *)NULL);
        _exit(127);
    }
    return child;
}

static pid_t start_printerctld_default_with_port_and_state(const char *port_path,
                                                           const char *state_home)
{
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        setenv("XDG_STATE_HOME", state_home, 1);
        execl(TEST_PRINTERCTLD_PATH, "printerctld", "--port", port_path, "--timeout", "1000",
              (char *)NULL);
        _exit(127);
    }
    return child;
}

static pid_t start_printerctld_with_config(const char *socket_path, const char *config_path,
                                           const char *state_home)
{
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        if (state_home != NULL) {
            setenv("XDG_STATE_HOME", state_home, 1);
        }
        execl(TEST_PRINTERCTLD_PATH, "printerctld", "--socket", socket_path, "--config",
              config_path, (char *)NULL);
        _exit(127);
    }
    return child;
}

static void stop_printerctld_after_signal(pid_t child)
{
    int status = 0;
    check(waitpid(child, &status, 0) == child, "waitpid");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "printerctld exits cleanly");
}

static void stop_printerctld(pid_t child)
{
    check(kill(child, SIGTERM) == 0, "SIGTERM printerctld");
    int status = 0;
    check(waitpid(child, &status, 0) == child, "waitpid");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "printerctld exits cleanly");
}

typedef struct {
    int master;
    char slave_path[128];
} mock_printer_t;

static void mock_open(mock_printer_t *mock)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    check(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "create pty");
    const char *name = ptsname(master);
    check(name != NULL, "ptsname");
    mock->master = master;
    snprintf(mock->slave_path, sizeof(mock->slave_path), "%s", name);
}

static void mock_close(mock_printer_t *mock)
{
    close(mock->master);
}

static void mock_reply_ok(mock_printer_t *mock)
{
    check(write(mock->master, "ok\n", 3) == 3, "mock writes ok");
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (milliseconds % 1000) * 1000000L;
    nanosleep(&delay, NULL);
}

static bool mock_read_command(mock_printer_t *mock, char *buffer, size_t buffer_size,
                              int timeout_ms)
{
    size_t received = 0;
    int remaining_ms = timeout_ms;
    while (received < buffer_size - 1) {
        struct pollfd pfd;
        pfd.fd = mock->master;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int slice_ms = remaining_ms > 50 ? 50 : remaining_ms;
        int rc = poll(&pfd, 1, slice_ms);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc == 0 || (pfd.revents & POLLIN) == 0) {
            if (rc > 0 && (pfd.revents & POLLIN) == 0) {
                sleep_ms(slice_ms);
            }
            remaining_ms -= slice_ms;
            if (remaining_ms > 0) {
                continue;
            }
            buffer[received] = '\0';
            return false;
        }
        ssize_t n = read(mock->master, buffer + received, buffer_size - 1 - received);
        check(n > 0, "read printer command");
        received += (size_t)n;
        buffer[received] = '\0';
        if (strchr(buffer, '\n') != NULL) {
            return true;
        }
    }
    return true;
}

static void mock_expect_command(mock_printer_t *mock, const char *expected)
{
    char buffer[128];
    check(mock_read_command(mock, buffer, sizeof(buffer), 5000), "printer received command");
    check(strcmp(buffer, expected) == 0, "printer command matches expected");
}

static void mock_expect_no_command(mock_printer_t *mock)
{
    char buffer[128];
    check(!mock_read_command(mock, buffer, sizeof(buffer), 250),
          "printer receives no command after shutdown signal");
}

static void start_daemon_print(pid_t child, const char *socket_path, const char *gcode_path,
                               mock_printer_t *mock);

static void write_gcode_file(char *path, size_t path_size, const char *contents)
{
    snprintf(path, path_size, "/tmp/printerctld-gcode-XXXXXX");
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    size_t length = strlen(contents);
    check(write(fd, contents, length) == (ssize_t)length, "write gcode");
    close(fd);
}

static void write_config_file(char *path, size_t path_size, const char *contents)
{
    snprintf(path, path_size, "/tmp/printerctld-config-XXXXXX");
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp config");
    size_t length = strlen(contents);
    check(write(fd, contents, length) == (ssize_t)length, "write config");
    close(fd);
}

static void make_temp_state_home(char *out_dir, size_t out_dir_size)
{
    snprintf(out_dir, out_dir_size, "/tmp/printerctld-state-XXXXXX");
    check(mkdtemp(out_dir) != NULL, "state mkdtemp");
}

static void remove_tree(const char *path)
{
    char command[256];
    snprintf(command, sizeof(command), "rm -rf '%s'", path);
    check(system(command) == 0, "remove temp tree");
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

static void send_print_cancel(int client, uint32_t request_id)
{
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_PRINT_CANCEL, request_id, NULL, 0,
                                           request, sizeof(request));
    write_full(client, request, request_length);
}

static printerctl_plp1_frame_t read_response_frame(int client, uint8_t expected_type,
                                                   uint32_t expected_request_id)
{
    static uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(client, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_frame(response, response_length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decode response");
    check(decoded.message_type == expected_type, "response type matches");
    check(decoded.request_id == expected_request_id, "response id matches");
    return decoded;
}

static void expect_child_exit(pid_t child, int expected_exit)
{
    int status = 0;
    check(waitpid(child, &status, 0) == child, "wait child");
    check(WIFEXITED(status) && WEXITSTATUS(status) == expected_exit, "child exit status matches");
}

static pid_t start_printerctl_query(const char *command, const char *config_home, int *out_read_fd)
{
    int stdout_pipe[2];
    check(pipe(stdout_pipe) == 0, "stdout pipe");
    pid_t child = fork();
    check(child >= 0, "fork printerctl query");
    if (child == 0) {
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[1]);
        setenv("XDG_CONFIG_HOME", config_home, 1);
        execl(TEST_PRINTERCTL_PATH, "printerctl", command, (char *)NULL);
        _exit(127);
    }
    close(stdout_pipe[1]);
    *out_read_fd = stdout_pipe[0];
    return child;
}

static pid_t start_printerctl_args(const char *arg1, const char *arg2, const char *arg3,
                                   const char *config_home, int *out_read_fd)
{
    int stdout_pipe[2];
    check(pipe(stdout_pipe) == 0, "stdout pipe");
    pid_t child = fork();
    check(child >= 0, "fork printerctl command");
    if (child == 0) {
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[1]);
        if (config_home != NULL) {
            setenv("XDG_CONFIG_HOME", config_home, 1);
        }
        if (arg3 != NULL) {
            execl(TEST_PRINTERCTL_PATH, "printerctl", arg1, arg2, arg3, (char *)NULL);
        } else if (arg2 != NULL) {
            execl(TEST_PRINTERCTL_PATH, "printerctl", arg1, arg2, (char *)NULL);
        } else {
            execl(TEST_PRINTERCTL_PATH, "printerctl", arg1, (char *)NULL);
        }
        _exit(127);
    }
    close(stdout_pipe[1]);
    *out_read_fd = stdout_pipe[0];
    return child;
}

static void read_output(int fd, char *buffer, size_t buffer_size)
{
    size_t used = 0;
    while (used + 1 < buffer_size) {
        ssize_t n = read(fd, buffer + used, buffer_size - used - 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }
    buffer[used] = '\0';
    close(fd);
}

static pid_t read_active_marker_pid(const char *state_home)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/printerctl/active-print.pid", state_home);
    FILE *file = fopen(path, "r");
    check(file != NULL, "active marker exists");
    long pid = 0;
    check(fscanf(file, "%ld", &pid) == 1 && pid > 0, "active marker contains pid");
    fclose(file);
    return (pid_t)pid;
}

static bool active_marker_exists(const char *state_home)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/printerctl/active-print.pid", state_home);
    return access(path, F_OK) == 0;
}

static void test_ping_and_clean_shutdown(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    pid_t child = start_printerctld(path);

    int fd = connect_with_retry(path);
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_ping(request, sizeof(request));
    write_full(fd, request, request_length);

    uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(fd, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_frame(response, response_length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decode PING response");
    check(decoded.message_type == PRINTERCTL_PLP1_MSG_PING, "PING response type");
    check(decoded.flags == PRINTERCTL_PLP1_FLAG_RESPONSE, "PING response flags");
    check(decoded.request_id == 123, "PING response request id");
    check(decoded.status == PRINTERCTL_PLP1_SUCCESS, "PING response status");
    close(fd);

    stop_printerctld(child);

    unlink(path);
    rmdir(dir);
}

static void test_sigterm_interrupts_idle_client(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    pid_t child = start_printerctld(path);
    int fd = connect_with_retry(path);
    stop_printerctld(child);
    close(fd);
    unlink(path);
    rmdir(dir);
}

static void test_sigpwr_without_active_print_is_safe(void)
{
    char path[108];
    char dir[128];
    make_temp_socket_path(path, sizeof(path), dir, sizeof(dir));

    pid_t child = start_printerctld(path);
    int fd = connect_with_retry(path);
    close(fd);
    check(kill(child, SIGPWR) == 0, "SIGPWR printerctld");

    int ping_fd = connect_with_retry(path);
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_ping(request, sizeof(request));
    write_full(ping_fd, request, request_length);
    uint8_t response[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t response_length = read_frame(ping_fd, response, sizeof(response));
    printerctl_plp1_frame_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_frame(response, response_length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decode PING response after SIGPWR");
    close(ping_fd);

    stop_printerctld(child);
    unlink(path);
    rmdir(dir);
}

static void test_daemon_config_file_supplies_hardware_config(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\n");

    mock_printer_t mock;
    mock_open(&mock);
    char config_path[128];
    char config_body[256];
    snprintf(config_body, sizeof(config_body), "port=%s\nbaud=115200\ntimeout=1000\n",
             mock.slave_path);
    write_config_file(config_path, sizeof(config_path), config_body);

    pid_t child = start_printerctld_with_config(socket_path, config_path, state_home);
    start_daemon_print(child, socket_path, gcode_path, &mock);
    mock_reply_ok(&mock);
    stop_printerctld(child);

    mock_close(&mock);
    unlink(config_path);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_daemon_invalid_config_file_fails_before_listen(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char config_path[128];
    write_config_file(config_path, sizeof(config_path), "timeout=not-a-number\n");

    pid_t child = start_printerctld_with_config(socket_path, config_path, NULL);
    int status = 0;
    check(waitpid(child, &status, 0) == child, "wait invalid-config daemon");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 3,
          "invalid daemon config exits with configuration error");
    check(access(socket_path, F_OK) != 0, "invalid daemon config does not create socket");

    unlink(config_path);
    unlink(socket_path);
    rmdir(dir);
}

static void start_daemon_print(pid_t child, const char *socket_path, const char *gcode_path,
                               mock_printer_t *mock)
{
    (void)child;
    int client = connect_with_retry(socket_path);
    send_print_start(client, 200, gcode_path);
    printerctl_plp1_frame_t response =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_START, 200);
    check(response.status == PRINTERCTL_PLP1_SUCCESS, "daemon PRINT_START succeeds");
    close(client);
    mock_expect_command(mock, "M105\n");
}

static void test_sigterm_during_active_print_is_safe(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nM114\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print(child, socket_path, gcode_path, &mock);

    check(kill(child, SIGTERM) == 0, "SIGTERM printerctld during print");
    sleep_ms(100);
    mock_reply_ok(&mock);
    stop_printerctld_after_signal(child);
    mock_expect_no_command(&mock);
    check(!active_marker_exists(state_home), "active marker removed after SIGTERM shutdown");
    check(access(socket_path, F_OK) != 0, "socket path cleaned after SIGTERM shutdown");

    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_sigpwr_during_active_print_uses_daemon_marker_and_is_safe(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nM114\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print(child, socket_path, gcode_path, &mock);

    check(read_active_marker_pid(state_home) == child, "active marker targets printerctld");
    check(kill(child, SIGPWR) == 0, "SIGPWR printerctld during print");

    int ping_fd = connect_with_retry(socket_path);
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_ping(request, sizeof(request));
    write_full(ping_fd, request, request_length);
    printerctl_plp1_frame_t ping = read_response_frame(ping_fd, PRINTERCTL_PLP1_MSG_PING, 123);
    check(ping.status == PRINTERCTL_PLP1_SUCCESS, "daemon remains alive after SIGPWR cancel");
    close(ping_fd);

    mock_reply_ok(&mock);
    mock_expect_no_command(&mock);
    check(!active_marker_exists(state_home), "active marker removed after SIGPWR cancellation");
    stop_printerctld(child);

    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_print_cancel_ack_window_blocks_next_command(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nM114\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print(child, socket_path, gcode_path, &mock);

    int cancel_fd = connect_with_retry(socket_path);
    send_print_cancel(cancel_fd, 202);
    printerctl_plp1_frame_t cancel =
        read_response_frame(cancel_fd, PRINTERCTL_PLP1_MSG_PRINT_CANCEL, 202);
    check(cancel.status == PRINTERCTL_PLP1_SUCCESS,
          "PRINT_CANCEL commits before the held ACK is released");
    close(cancel_fd);

    mock_reply_ok(&mock);
    mock_expect_no_command(&mock);
    check(!active_marker_exists(state_home), "active marker removed after PRINT_CANCEL");
    stop_printerctld(child);

    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_cli_read_only_queries_use_daemon(void)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");
    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port(mock.slave_path);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd = -1;
    pid_t cli = start_printerctl_query("temp", config_home, &out_fd);
    mock_expect_command(&mock, "M105\n");
    const char *temp = "ok T:210.00 /210.00 B:60.00 /60.00 @:127 B@:0\n";
    check(write(mock.master, temp, strlen(temp)) == (ssize_t)strlen(temp),
          "mock writes M105 response");
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    char output[512];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "hotend 210.00 C (target 210.00 C)") != NULL,
          "CLI temp renders daemon response");

    cli = start_printerctl_query("position", config_home, &out_fd);
    mock_expect_command(&mock, "M114\n");
    const char *position = "X:1.00 Y:2.00 Z:3.00 E:4.00 Count X:0 Y:0 Z:0\nok\n";
    check(write(mock.master, position, strlen(position)) == (ssize_t)strlen(position),
          "mock writes M114 response");
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "position X=1.00 Y=2.00 Z=3.00 E=4.00") != NULL,
          "CLI position renders daemon response");

    cli = start_printerctl_query("info", config_home, &out_fd);
    mock_expect_command(&mock, "M115\n");
    const char *info = "FIRMWARE_NAME:Marlin 2.1.2 MACHINE_TYPE:EasyThreed K9 "
                       "EXTRUDER_COUNT:1\nok\n";
    check(write(mock.master, info, strlen(info)) == (ssize_t)strlen(info),
          "mock writes M115 response");
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "firmware: Marlin 2.1.2") != NULL &&
              strstr(output, "machine: EasyThreed K9") != NULL &&
              strstr(output, "extruders: 1") != NULL,
          "CLI info renders daemon response");

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

static void test_cli_explicit_physical_commands_use_daemon(void)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");
    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port(mock.slave_path);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd = -1;
    pid_t cli = start_printerctl_args("send", "M500", NULL, config_home, &out_fd);
    mock_expect_command(&mock, "M500\n");
    const char *send_reply = "echo:Settings Stored\nok\n";
    check(write(mock.master, send_reply, strlen(send_reply)) == (ssize_t)strlen(send_reply),
          "mock writes RAW_SEND response");
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    char output[512];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "> M500") != NULL && strstr(output, "< echo:Settings Stored") != NULL &&
              strstr(output, "< ok") != NULL,
          "CLI send renders daemon-owned exchange");

    cli = start_printerctl_args("home", "X", NULL, config_home, &out_fd);
    mock_expect_command(&mock, "G28 X\n");
    mock_reply_ok(&mock);
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "home complete: X axis") != NULL,
          "CLI home renders daemon acknowledgement");

    cli = start_printerctl_args("sd", "pause", NULL, config_home, &out_fd);
    mock_expect_command(&mock, "M25\n");
    mock_reply_ok(&mock);
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "SD print pause requested") != NULL,
          "CLI sd pause renders daemon acknowledgement");

    uint8_t payload[32];
    size_t payload_length = 0;
    printerctl_plp1_error_t plp_error;
    check(printerctl_plp1_encode_raw_send_request("G28", payload, sizeof(payload),
                                                  &payload_length, &plp_error) == PRINTERCTL_PLP1_OK,
          "encode RAW_SEND G28 request");
    int client = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_RAW_SEND, 901, payload,
                                           (uint16_t)payload_length, request, sizeof(request));
    write_full(client, request, request_length);
    printerctl_plp1_frame_t response =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_RAW_SEND, 901);
    check(response.status == PRINTERCTL_PLP1_SAFETY_REJECTED,
          "daemon rejects RAW_SEND G28 with SAFETY_REJECTED");
    close(client);
    mock_expect_no_command(&mock);

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

static void test_cli_print_uses_daemon_print_start(void)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "G21\nM105\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port_and_state(mock.slave_path, state_home);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd = -1;
    pid_t cli = start_printerctl_args("print", gcode_path, NULL, config_home, &out_fd);
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    char output[512];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "print accepted by printerctld") != NULL,
          "CLI print reports daemon acceptance");

    mock_expect_command(&mock, "G21\n");
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "M105\n");
    mock_reply_ok(&mock);

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

static void test_cli_print_source_error_is_daemon_reported(void)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));

    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port_and_state(mock.slave_path, state_home);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd = -1;
    pid_t cli = start_printerctl_args("print", "/tmp/printerctld-missing-cli.gcode", NULL,
                                      config_home, &out_fd);
    expect_child_exit(cli, PRINTERCTL_EXIT_GCODE_ERROR);
    char output[128];
    read_output(out_fd, output, sizeof(output));
    check(output[0] == '\0', "missing source prints no acceptance message");
    mock_expect_no_command(&mock);

    stop_printerctld(daemon);
    mock_close(&mock);
    remove_tree(state_home);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

int main(void)
{
    test_ping_and_clean_shutdown();
    test_sigterm_interrupts_idle_client();
    test_sigpwr_without_active_print_is_safe();
    test_daemon_config_file_supplies_hardware_config();
    test_daemon_invalid_config_file_fails_before_listen();
    test_sigterm_during_active_print_is_safe();
    test_sigpwr_during_active_print_uses_daemon_marker_and_is_safe();
    test_print_cancel_ack_window_blocks_next_command();
    test_cli_read_only_queries_use_daemon();
    test_cli_explicit_physical_commands_use_daemon();
    test_cli_print_uses_daemon_print_start();
    test_cli_print_source_error_is_daemon_reported();
    return 0;
}
