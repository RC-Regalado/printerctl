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

static pid_t start_printerctld_default_with_port_timeout(const char *port_path,
                                                         const char *timeout_ms)
{
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        execl(TEST_PRINTERCTLD_PATH, "printerctld", "--port", port_path, "--timeout",
              timeout_ms, (char *)NULL);
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

static void mock_write_text(mock_printer_t *mock, const char *text)
{
    size_t length = strlen(text);
    check(write(mock->master, text, length) == (ssize_t)length, "mock writes text");
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

static void send_print_pause(int client, uint32_t request_id)
{
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_PRINT_PAUSE, request_id, NULL, 0,
                                           request, sizeof(request));
    write_full(client, request, request_length);
}

static void send_print_resume(int client, uint32_t request_id)
{
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_PRINT_RESUME, request_id, NULL, 0,
                                           request, sizeof(request));
    write_full(client, request, request_length);
}


static void send_status(int client, uint32_t request_id, uint32_t requested_fields)
{
    uint8_t payload[4];
    payload[0] = (uint8_t)(requested_fields >> 24);
    payload[1] = (uint8_t)(requested_fields >> 16);
    payload[2] = (uint8_t)(requested_fields >> 8);
    payload[3] = (uint8_t)requested_fields;
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_STATUS, request_id, payload,
                                           sizeof(payload), request, sizeof(request));
    write_full(client, request, request_length);
}

static void send_query_temp(int client, uint32_t request_id)
{
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_QUERY_TEMP, request_id, NULL, 0,
                                           request, sizeof(request));
    write_full(client, request, request_length);
}

static void send_home(int client, uint32_t request_id, uint8_t axis)
{
    uint8_t payload[1];
    size_t payload_length = 0;
    printerctl_plp1_error_t plp_error;
    check(printerctl_plp1_encode_home_request(axis, payload, sizeof(payload), &payload_length,
                                              &plp_error) == PRINTERCTL_PLP1_OK,
          "encode HOME payload");
    uint8_t request[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t request_length = encode_request(PRINTERCTL_PLP1_MSG_HOME, request_id, payload,
                                           (uint16_t)payload_length, request, sizeof(request));
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

/* Sends STATUS for EXECUTION_STATE|PROGRESS|COMMANDS_ACKNOWLEDGED|COMMANDS_TOTAL
 * on a fresh connection and decodes the response, so callers can model an
 * independent observer (PRINT-OBS-001 client-independence coverage). */
static printerctl_plp1_status_response_t query_print_status(const char *socket_path,
                                                             uint32_t request_id)
{
    int client = connect_with_retry(socket_path);
    send_status(client, request_id,
               PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE |
                   PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS |
                   PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED |
                   PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_TOTAL);
    printerctl_plp1_frame_t response =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_STATUS, request_id);
    check(response.status == PRINTERCTL_PLP1_SUCCESS, "STATUS succeeds");
    printerctl_plp1_status_response_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_status_response(response.payload, response.payload_length,
                                                 &decoded, &error) == PRINTERCTL_PLP1_OK,
          "STATUS response decodes");
    close(client);
    return decoded;
}

/* Retries query_print_status() until commands_acknowledged reaches
 * min_acknowledged or the budget is exhausted. The daemon updates its
 * snapshot asynchronously (worker thread -> event_pipe -> control
 * context) relative to this test process's own socket round-trip, so a
 * query issued immediately after mock_reply_ok() can occasionally
 * observe the previous count; the mock never acknowledges a further
 * command until the test explicitly calls mock_reply_ok() again, so
 * commands_acknowledged can only be at-or-below min_acknowledged here,
 * never overshoot it. */
static printerctl_plp1_status_response_t query_print_status_at_least(
    const char *socket_path, uint32_t request_id, uint32_t min_acknowledged)
{
    printerctl_plp1_status_response_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    for (int attempt = 0; attempt < 50; ++attempt) {
        decoded = query_print_status(socket_path, request_id);
        if (decoded.commands_acknowledged >= min_acknowledged) {
            return decoded;
        }
        sleep_ms(10);
    }
    check(decoded.commands_acknowledged >= min_acknowledged,
          "commands_acknowledged reaches the expected value within the retry budget");
    return decoded;
}

static void expect_child_exit(pid_t child, int expected_exit)
{
    int status = 0;
    check(waitpid(child, &status, 0) == child, "wait child");
    check(WIFEXITED(status) && WEXITSTATUS(status) == expected_exit, "child exit status matches");
}

static void expect_child_still_running(pid_t child)
{
    int status = 0;
    check(waitpid(child, &status, WNOHANG) == 0, "child is still running");
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

static pid_t start_printerctl_args_combined(const char *arg1, const char *arg2,
                                            const char *arg3, const char *config_home,
                                            int *out_read_fd)
{
    int output_pipe[2];
    check(pipe(output_pipe) == 0, "output pipe");
    pid_t child = fork();
    check(child >= 0, "fork printerctl command");
    if (child == 0) {
        close(output_pipe[0]);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[1]);
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
    close(output_pipe[1]);
    *out_read_fd = output_pipe[0];
    return child;
}

/* M24: `printerctl power low-battery` never talks to `printerctld`
 * over PLP/1 -- it resolves the active-print marker under
 * $XDG_STATE_HOME and signals that PID directly (the audited SIGPWR
 * path from Milestone 10, preserved unchanged through the daemon
 * migration; see docs/design/m24-power-integration.md). It needs the
 * SAME state_home the daemon was started with, unlike every other
 * `start_printerctl_*` helper above, which only ever set
 * XDG_CONFIG_HOME. */
static pid_t start_printerctl_power_low_battery(const char *state_home, int *out_read_fd)
{
    int stdout_pipe[2];
    check(pipe(stdout_pipe) == 0, "stdout pipe");
    pid_t child = fork();
    check(child >= 0, "fork printerctl power low-battery");
    if (child == 0) {
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[1]);
        setenv("XDG_STATE_HOME", state_home, 1);
        execl(TEST_PRINTERCTL_PATH, "printerctl", "power", "low-battery", (char *)NULL);
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

static void start_daemon_print_without_waiting_for_command(const char *socket_path,
                                                           const char *gcode_path)
{
    int client = connect_with_retry(socket_path);
    send_print_start(client, 200, gcode_path);
    printerctl_plp1_frame_t response =
        read_response_frame(client, PRINTERCTL_PLP1_MSG_PRINT_START, 200);
    check(response.status == PRINTERCTL_PLP1_SUCCESS, "daemon PRINT_START succeeds");
    close(client);
}

static void assert_ping_ok(const char *socket_path)
{
    int client = connect_with_retry(socket_path);
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_ping(request, sizeof(request));
    write_full(client, request, request_length);
    printerctl_plp1_frame_t response = read_response_frame(client, PRINTERCTL_PLP1_MSG_PING, 123);
    check(response.status == PRINTERCTL_PLP1_SUCCESS, "daemon PING succeeds");
    close(client);
}

static void write_linear_gcode_file(char *path, size_t path_size, int count)
{
    char contents[4096];
    size_t used = 0;
    for (int i = 0; i < count; ++i) {
        int n = snprintf(contents + used, sizeof(contents) - used, "G1 X%d\n", i);
        check(n > 0 && used + (size_t)n < sizeof(contents), "linear gcode fits");
        used += (size_t)n;
    }
    write_gcode_file(path, path_size, contents);
}

static void test_print_advances_after_m109_without_secondary_request(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path),
                     "G1 X0\nG1 X1\nG1 X2\nG1 X3\nG1 X4\nG1 X5\nG1 X6\nG1 X7\nG1 X8\n"
                     "M109 S200\nG1 X10\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);

    char expected[32];
    for (int i = 0; i < 9; ++i) {
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);
    }
    mock_expect_command(&mock, "M109 S200\n");
    mock_write_text(&mock, "T:180 /200\n");
    mock_write_text(&mock, "echo:busy: processing\n");
    mock_write_text(&mock, "T:190 /200\n");
    mock_write_text(&mock, "echo:busy: processing\n");
    mock_write_text(&mock, "T:200 /200\n");
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X10\n");
    mock_reply_ok(&mock);
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_print_progress_event_backlog_does_not_stall_execution(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 48);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);

    char expected[32];
    for (int i = 0; i < 48; ++i) {
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);
    }
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_status_during_print_has_no_serial_side_effects(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "G1 X0\nG1 X1\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);
    mock_expect_command(&mock, "G1 X0\n");

    int client = connect_with_retry(socket_path);
    send_status(client, 260, PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE |
                                 PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_SENT);
    printerctl_plp1_frame_t status = read_response_frame(client, PRINTERCTL_PLP1_MSG_STATUS, 260);
    check(status.status == PRINTERCTL_PLP1_SUCCESS, "STATUS succeeds during active print");
    close(client);
    mock_expect_no_command(&mock);

    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X1\n");
    mock_reply_ok(&mock);
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_temp_during_print_is_busy_and_has_no_serial_side_effects(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "G1 X0\nG1 X1\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);
    mock_expect_command(&mock, "G1 X0\n");

    int client = connect_with_retry(socket_path);
    send_query_temp(client, 270);
    printerctl_plp1_frame_t temp = read_response_frame(client, PRINTERCTL_PLP1_MSG_QUERY_TEMP, 270);
    check(temp.status == PRINTERCTL_PLP1_BUSY, "TEMP returns BUSY during active print");
    close(client);
    mock_expect_no_command(&mock);

    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X1\n");
    mock_reply_ok(&mock);
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

/* PRINT-OBS-001: acknowledged/total progress advances and is monotonic
 * across a 10-command print, and STATUS itself sends nothing to the
 * printer. */
static void test_print_progress_advances_with_acknowledgements(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 10);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);

    uint32_t previous_acknowledged = 0;
    char expected[32];
    for (int i = 0; i < 10; ++i) {
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);

        printerctl_plp1_status_response_t decoded =
            query_print_status_at_least(socket_path, 300 + (uint32_t)i, (uint32_t)(i + 1));
        /* The 10th acknowledgement is also the file's last command, so
         * the engine may already have reached EOF and transitioned to
         * COMPLETED by the time this query runs. */
        bool is_last = i == 9;
        check(decoded.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING ||
                  (is_last && decoded.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED),
              "execution_state is RUNNING while acknowledgements are still arriving "
              "(or COMPLETED exactly at the final acknowledgement)");
        check(decoded.commands_acknowledged == (uint32_t)(i + 1),
              "commands_acknowledged reflects exactly the acknowledgements observed so far");
        check(decoded.commands_total == 10,
              "commands_total reports the pre-scanned source command count");
        check(decoded.commands_acknowledged >= previous_acknowledged,
              "commands_acknowledged never regresses between polls (monotonicity)");
        previous_acknowledged = decoded.commands_acknowledged;
    }
    check(previous_acknowledged == 10, "the final poll observed all 10 acknowledgements");
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

/* PRINT-OBS-001: pausing must not reset progress, and resuming must
 * continue from the same acknowledged count -- never from zero. */
static void test_print_pause_resume_preserves_progress(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 10);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);

    char expected[32];
    for (int i = 0; i < 4; ++i) {
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);
    }

    /* The 5th command (G1 X4) is sent but deliberately held un-acked --
     * the same ack-window technique test_print_cancel_ack_window_blocks_
     * next_command() above uses -- so PRINT_PAUSE is guaranteed to
     * commit (via poll_control_commands()'s ack-wait polling) before
     * on_control() is ever consulted for the next command, making the
     * "G1 X5 must not be sent" assertion below deterministic rather
     * than racing the engine's own control poll. */
    mock_expect_command(&mock, "G1 X4\n");

    int control = connect_with_retry(socket_path);
    send_print_pause(control, 311);
    printerctl_plp1_frame_t pause_response =
        read_response_frame(control, PRINTERCTL_PLP1_MSG_PRINT_PAUSE, 311);
    check(pause_response.status == PRINTERCTL_PLP1_SUCCESS, "PRINT_PAUSE is accepted");
    close(control);

    mock_reply_ok(&mock); /* releases G1 X4's ack: 5 of 10 now acknowledged */
    mock_expect_no_command(&mock); /* G1 X5 must not be transmitted while paused */

    printerctl_plp1_status_response_t paused = query_print_status(socket_path, 312);
    check(paused.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_PAUSED,
          "execution_state reports PAUSED");
    check(paused.commands_acknowledged == 5,
          "commands_acknowledged is unchanged by pausing (not reset to 0)");
    check(paused.progress > 0, "progress percent is not reset to 0 by pausing");

    int resume = connect_with_retry(socket_path);
    send_print_resume(resume, 313);
    printerctl_plp1_frame_t resume_response =
        read_response_frame(resume, PRINTERCTL_PLP1_MSG_PRINT_RESUME, 313);
    check(resume_response.status == PRINTERCTL_PLP1_SUCCESS, "PRINT_RESUME is accepted");
    close(resume);

    for (int i = 5; i < 10; ++i) {
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);
    }

    printerctl_plp1_status_response_t completed = query_print_status_at_least(socket_path, 314, 10);
    check(completed.commands_acknowledged == 10,
          "printing resumes from 5 and continues to completion, never restarting from 0");
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

/* M23: repeated PRINT_PAUSE while already paused, and repeated
 * PRINT_RESUME while already running, must both stay safe no-ops --
 * neither resets progress nor is rejected -- and every PAUSE/RESUME in
 * this test arrives on its own fresh connection, distinct from the one
 * that issued PRINT_START, proving control is not tied to a particular
 * client (docs/design/m23-host-control-ux.md's "no context-sensitive
 * dispatch" concern is about SD vs. host syntax, not about which
 * connection may issue a given, unambiguous host-control request). */
static void test_print_pause_resume_repeated_and_second_client(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 6);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);

    mock_expect_command(&mock, "G1 X0\n");
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X1\n");
    mock_reply_ok(&mock);

    /* Ack-window technique (see test_print_pause_resume_preserves_progress
     * above): G1 X2 is sent but held un-acked so both PRINT_PAUSE
     * requests below are guaranteed to commit, via poll_control_commands()'s
     * ack-wait polling, before on_control() is next consulted for G1 X3. */
    mock_expect_command(&mock, "G1 X2\n");

    int pause_a = connect_with_retry(socket_path);
    send_print_pause(pause_a, 401);
    check(read_response_frame(pause_a, PRINTERCTL_PLP1_MSG_PRINT_PAUSE, 401).status ==
              PRINTERCTL_PLP1_SUCCESS,
          "first PRINT_PAUSE (client A) is accepted");
    close(pause_a);

    int pause_b = connect_with_retry(socket_path);
    send_print_pause(pause_b, 402);
    check(read_response_frame(pause_b, PRINTERCTL_PLP1_MSG_PRINT_PAUSE, 402).status ==
              PRINTERCTL_PLP1_SUCCESS,
          "repeated PRINT_PAUSE (client B, already paused) is still accepted");
    close(pause_b);

    mock_reply_ok(&mock); /* releases G1 X2's ack: 3 of 6 now acknowledged */
    mock_expect_no_command(&mock); /* G1 X3 must not be transmitted while paused */

    printerctl_plp1_status_response_t paused = query_print_status(socket_path, 403);
    check(paused.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_PAUSED,
          "execution_state reports PAUSED after two PAUSE requests from two clients");
    check(paused.commands_acknowledged == 3,
          "repeated pause does not disturb commands_acknowledged");

    int resume_a = connect_with_retry(socket_path);
    send_print_resume(resume_a, 404);
    check(read_response_frame(resume_a, PRINTERCTL_PLP1_MSG_PRINT_RESUME, 404).status ==
              PRINTERCTL_PLP1_SUCCESS,
          "first PRINT_RESUME (client C) is accepted");
    close(resume_a);

    /* G1 X3 is not yet acked, so the print is still active regardless of
     * pause state -- this second RESUME cannot race a completed print. */
    int resume_b = connect_with_retry(socket_path);
    send_print_resume(resume_b, 405);
    check(read_response_frame(resume_b, PRINTERCTL_PLP1_MSG_PRINT_RESUME, 405).status ==
              PRINTERCTL_PLP1_SUCCESS,
          "repeated PRINT_RESUME (client D, already running) is still accepted");
    close(resume_b);

    for (int i = 3; i < 6; ++i) {
        char expected[32];
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);
    }

    printerctl_plp1_status_response_t completed = query_print_status_at_least(socket_path, 406, 6);
    check(completed.commands_acknowledged == 6,
          "printing resumes from 3 and reaches completion despite repeated pause/resume");
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

/* PRINT-OBS-001: client independence -- the client that started the
 * print may disconnect; a second, unrelated client must still observe
 * correct current progress from the daemon-owned snapshot. */
static void test_print_progress_client_independence(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 10);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);

    /* Client A starts the print and disconnects immediately (mirrors
     * start_daemon_print_without_waiting_for_command, but explicit here
     * about client A's lifetime). */
    int client_a = connect_with_retry(socket_path);
    send_print_start(client_a, 320, gcode_path);
    printerctl_plp1_frame_t accepted =
        read_response_frame(client_a, PRINTERCTL_PLP1_MSG_PRINT_START, 320);
    check(accepted.status == PRINTERCTL_PLP1_SUCCESS, "PRINT_START is accepted");
    close(client_a);

    char expected[32];
    for (int i = 0; i < 6; ++i) {
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);
    }

    /* Client B never sent PRINT_START and has no prior connection to
     * this daemon -- everything it needs comes from the daemon-owned
     * snapshot. */
    printerctl_plp1_status_response_t observed_by_b =
        query_print_status_at_least(socket_path, 330, 6);
    check(observed_by_b.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING,
          "client B observes the print still running after client A disconnected");
    check(observed_by_b.commands_acknowledged == 6,
          "client B observes the correct current acknowledged count");
    check(observed_by_b.commands_total == 10,
          "client B observes the correct commands_total known only to the daemon");

    for (int i = 6; i < 10; ++i) {
        snprintf(expected, sizeof(expected), "G1 X%d\n", i);
        mock_expect_command(&mock, expected);
        mock_reply_ok(&mock);
    }
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

/* PRINT-OBS-001: repeated STATUS polling during an active print must
 * never itself produce serial traffic. */
static void test_repeated_status_polling_has_no_serial_side_effects(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "G1 X0\nG1 X1\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);
    mock_expect_command(&mock, "G1 X0\n");

    for (int i = 0; i < 10; ++i) {
        printerctl_plp1_status_response_t decoded = query_print_status(socket_path, 340 + (uint32_t)i);
        check(decoded.commands_acknowledged == 0,
              "repeated STATUS polling does not itself advance execution");
    }
    mock_expect_no_command(&mock);

    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X1\n");
    mock_reply_ok(&mock);
    assert_ping_ok(socket_path);

    stop_printerctld(child);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

static void test_sigterm_during_active_print_is_safe(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nG28\n");

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
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nG28\n");

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

/* M24: `test_sigpwr_during_active_print_uses_daemon_marker_and_is_safe`
 * above already proves the daemon-side half of the "APCUPSD ->
 * printerctl power low-battery -> daemon marker/PID -> SIGPWR
 * printerctld" path -- but it delivers SIGPWR with a raw kill(),
 * never through the real `printerctl power low-battery` command. This
 * test is the missing end-to-end link: a real `printerctl` subprocess
 * resolves the marker printerctld itself wrote and signals it. */
static void test_cli_power_low_battery_signals_daemon_owned_print(void)
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
    pid_t daemon = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print(daemon, socket_path, gcode_path, &mock);
    check(read_active_marker_pid(state_home) == daemon,
          "active marker targets printerctld before signalling it");

    int out_fd = -1;
    pid_t power = start_printerctl_power_low_battery(state_home, &out_fd);
    expect_child_exit(power, PRINTERCTL_EXIT_OK);
    char output[256];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "signalled the active print") != NULL,
          "'printerctl power low-battery' reports signalling the daemon-owned print");

    mock_reply_ok(&mock);
    mock_expect_no_command(&mock);
    check(!active_marker_exists(state_home),
          "active marker removed once printerctld cancels after the CLI's SIGPWR");
    assert_ping_ok(socket_path);

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(socket_path);
    rmdir(dir);
}

/* M24: SIGPWR must win even while a daemon-owned print is genuinely
 * PAUSED (M23), not only while blocked on an in-flight command's ack
 * (the scenario test_sigpwr_during_active_print_uses_daemon_marker_
 * and_is_safe above exercises). on_control() checks power-failure
 * cancellation before pause state on every iteration of the pause-wait
 * loop (src/core/print_engine.c), so this must cancel promptly rather
 * than waiting for a resume that never comes. */
static void test_sigpwr_while_paused_cancels_with_power_failure_semantics(void)
{
    char socket_path[108];
    char dir[128];
    make_temp_socket_path(socket_path, sizeof(socket_path), dir, sizeof(dir));
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 4);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t child = start_printerctld_with_port(socket_path, mock.slave_path, state_home);
    start_daemon_print_without_waiting_for_command(socket_path, gcode_path);

    mock_expect_command(&mock, "G1 X0\n");
    mock_reply_ok(&mock);

    /* Ack-window technique: hold G1 X1's ack so PRINT_PAUSE is
     * guaranteed to commit before G1 X2 is ever considered. */
    mock_expect_command(&mock, "G1 X1\n");
    int control = connect_with_retry(socket_path);
    send_print_pause(control, 411);
    check(read_response_frame(control, PRINTERCTL_PLP1_MSG_PRINT_PAUSE, 411).status ==
              PRINTERCTL_PLP1_SUCCESS,
          "PRINT_PAUSE is accepted");
    close(control);
    mock_reply_ok(&mock); /* releases G1 X1's ack: 2 of 4 now acknowledged */
    mock_expect_no_command(&mock); /* G1 X2 must not be transmitted while paused */

    printerctl_plp1_status_response_t paused = query_print_status(socket_path, 412);
    check(paused.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_PAUSED,
          "execution_state reports PAUSED before SIGPWR");

    check(kill(child, SIGPWR) == 0, "SIGPWR printerctld while paused");
    assert_ping_ok(socket_path);
    mock_expect_no_command(&mock); /* no resume, no cleanup G-code after power-failure cancel */
    check(!active_marker_exists(state_home),
          "active marker removed after SIGPWR cancels a paused print");

    int status_client = connect_with_retry(socket_path);
    send_status(status_client, 413,
               PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE |
                   PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON);
    printerctl_plp1_frame_t response =
        read_response_frame(status_client, PRINTERCTL_PLP1_MSG_STATUS, 413);
    printerctl_plp1_status_response_t decoded;
    printerctl_plp1_error_t decode_error;
    check(printerctl_plp1_decode_status_response(response.payload, response.payload_length,
                                                 &decoded, &decode_error) == PRINTERCTL_PLP1_OK,
          "decode STATUS after SIGPWR-while-paused");
    close(status_client);
    check(decoded.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED,
          "a paused print reaches CANCELLED, not stuck PAUSED, after SIGPWR");
    check((decoded.present_fields & PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON) != 0 &&
              decoded.failure_reason == PRINTERCTL_PLP1_SHUTTING_DOWN,
          "failure_reason distinguishes the power-failure cancel from an operator cancel");

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
    write_gcode_file(gcode_path, sizeof(gcode_path), "M105\nG28\n");

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

/* M23: `printerctl pause`/`printerctl resume` reach the daemon-owned
 * host stream through the same PLP client library `print`/`send`/`home`
 * already use -- proving the CLI wiring, not just the protocol/socket
 * layer (which test_second_print_start_maps_to_busy_over_socket and the
 * PRINT_PAUSE/PRINT_RESUME PTY tests above already cover directly). */
static void test_cli_pause_resume_use_daemon_print_control(void)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 4);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port_and_state(mock.slave_path, state_home);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    send_print_start(ready, 500, gcode_path);
    check(read_response_frame(ready, PRINTERCTL_PLP1_MSG_PRINT_START, 500).status ==
              PRINTERCTL_PLP1_SUCCESS,
          "PRINT_START accepted before CLI pause/resume");
    close(ready);

    mock_expect_command(&mock, "G1 X0\n");
    mock_reply_ok(&mock);

    /* Ack-window technique: G1 X1 is sent but held un-acked so
     * `printerctl pause` is guaranteed to commit before G1 X2 is ever
     * considered (see test_print_pause_resume_preserves_progress). */
    mock_expect_command(&mock, "G1 X1\n");

    int out_fd = -1;
    pid_t cli_pause = start_printerctl_args("pause", NULL, NULL, config_home, &out_fd);
    expect_child_exit(cli_pause, PRINTERCTL_EXIT_OK);
    char output[256];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "host print pause requested") != NULL,
          "CLI pause renders daemon acknowledgement");

    mock_reply_ok(&mock); /* releases G1 X1's ack: 2 of 4 now acknowledged */
    mock_expect_no_command(&mock); /* G1 X2 must not be transmitted while paused */

    printerctl_plp1_status_response_t paused =
        query_print_status(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, 501);
    check(paused.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_PAUSED,
          "'printerctl pause' actually pauses the daemon-owned host print");
    check(paused.commands_acknowledged == 2, "pause via the CLI does not disturb progress");

    pid_t cli_resume = start_printerctl_args("resume", NULL, NULL, config_home, &out_fd);
    expect_child_exit(cli_resume, PRINTERCTL_EXIT_OK);
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "host print resume requested") != NULL,
          "CLI resume renders daemon acknowledgement");

    mock_expect_command(&mock, "G1 X2\n");
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X3\n");
    mock_reply_ok(&mock);

    printerctl_plp1_status_response_t completed =
        query_print_status_at_least(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, 502, 4);
    check(completed.commands_acknowledged == 4,
          "'printerctl resume' continues the print from 2, never restarting from 0");

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

/* M23: `printerctl cancel` reaches the daemon-owned host stream the
 * same way; unlike pause/resume the print never reaches its own
 * COMPLETED state, so this is verified with its own daemon instance. */
static void test_cli_cancel_uses_daemon_print_control(void)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 4);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port_and_state(mock.slave_path, state_home);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    send_print_start(ready, 600, gcode_path);
    check(read_response_frame(ready, PRINTERCTL_PLP1_MSG_PRINT_START, 600).status ==
              PRINTERCTL_PLP1_SUCCESS,
          "PRINT_START accepted before CLI cancel");
    close(ready);

    /* Ack-window technique: G1 X0 is sent but held un-acked so
     * `printerctl cancel` is guaranteed to commit (via
     * poll_control_commands()'s ack-wait polling) before the held ACK
     * is released, exactly like test_print_cancel_ack_window_blocks_
     * next_command() above. */
    mock_expect_command(&mock, "G1 X0\n");

    int out_fd = -1;
    pid_t cli_cancel = start_printerctl_args("cancel", NULL, NULL, config_home, &out_fd);
    expect_child_exit(cli_cancel, PRINTERCTL_EXIT_OK);
    char output[256];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "host print cancel requested") != NULL,
          "CLI cancel renders daemon acknowledgement");

    mock_reply_ok(&mock);
    mock_expect_no_command(&mock); /* G1 X1 must not be transmitted after cancel */
    check(!active_marker_exists(state_home), "active marker removed after CLI cancel");

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(gcode_path);
    remove_tree(state_home);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

/* M23 / docs/phase-2-testing.md's "Ctrl+C observer": the CLI's print
 * progress observer (PRINT-OBS-001) installs no signal handler of its
 * own -- client signals are a UI concern, never an execution-ownership
 * one (docs/design/m23-host-control-ux.md: "Do not restore PID-based
 * host execution control through the client"). Sending SIGINT to the
 * observer while it is attached must kill only the observer process;
 * the daemon-owned print continues exactly as if the client had
 * disconnected for any other reason, all the way to completion. */
static void test_cli_print_sigint_kills_observer_not_the_print(void)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");
    char gcode_path[128];
    write_linear_gcode_file(gcode_path, sizeof(gcode_path), 4);

    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port(mock.slave_path);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd = -1;
    pid_t observer = start_printerctl_args("print", gcode_path, NULL, config_home, &out_fd);

    mock_expect_command(&mock, "G1 X0\n");
    mock_reply_ok(&mock);

    /* Ack-window technique: G1 X1 is sent but held un-acked so the
     * print stays blocked mid-stream while SIGINT is delivered below.
     * Unlike the other CLI-level tests above, the observer's own
     * connection stays open the whole time here (printerctld serves
     * one client connection at a time -- see
     * printerctl_daemon_socket_serve_one_interruptible()), so this test
     * deliberately never opens a second, overlapping connection of its
     * own while the observer is still attached; it only reads STATUS
     * afterwards, once the observer's connection is gone. */
    mock_expect_command(&mock, "G1 X1\n");

    /* Give the observer several poll intervals (200ms each, per cli.c's
     * PRINTERCTL_CLI_PRINT_POLL_INTERVAL_MS) to actually connect, accept
     * PRINT_START and start polling STATUS before it is interrupted --
     * otherwise a SIGINT delivered before it even connects would not
     * exercise the intended path. */
    sleep_ms(600);
    expect_child_still_running(observer);

    check(kill(observer, SIGINT) == 0, "SIGINT the print observer");
    int status = 0;
    check(waitpid(observer, &status, 0) == observer, "wait for observer");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT,
          "the observer dies of the unhandled SIGINT itself (no client-side cancellation logic)");
    close(out_fd);

    mock_reply_ok(&mock); /* releases G1 X1's ack: 2 of 4 now acknowledged */
    mock_expect_command(&mock, "G1 X2\n");
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X3\n");
    mock_reply_ok(&mock);

    printerctl_plp1_status_response_t completed =
        query_print_status_at_least(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, 701, 4);
    check(completed.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED,
          "the daemon-owned print reaches COMPLETED despite the observer's SIGINT death");
    check(completed.commands_acknowledged == 4,
          "the print streams to completion unaffected by the observer dying");

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(gcode_path);
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
    write_gcode_file(gcode_path, sizeof(gcode_path), "G21\nG28\nG28 X\nG28 Y\nG28 Z\nM105\n");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port_and_state(mock.slave_path, state_home);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd = -1;
    pid_t cli = start_printerctl_args("print", gcode_path, NULL, config_home, &out_fd);

    /* PRINT-OBS-001: the CLI now remains attached, polling STATUS until
     * a terminal execution_state -- it no longer exits right after
     * acceptance -- so the mock exchange must happen before waiting for
     * the CLI to exit (same ordering test_home_quick_completion_
     * remains_synchronous() above already uses for `home`). */
    mock_expect_command(&mock, "G21\n");
    mock_reply_ok(&mock);
    const char *homing_commands[] = {"G28\n", "G28 X\n", "G28 Y\n", "G28 Z\n"};
    for (size_t i = 0; i < sizeof(homing_commands) / sizeof(homing_commands[0]); ++i) {
        mock_expect_command(&mock, homing_commands[i]);
        mock_expect_no_command(&mock);
        mock_reply_ok(&mock);
    }
    mock_expect_command(&mock, "M105\n");
    mock_reply_ok(&mock);

    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    char output[512];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "print accepted by printerctld") != NULL,
          "CLI print reports daemon acceptance");
    check(strstr(output, "Completed") != NULL,
          "CLI print reports the terminal Completed state before exiting");
    check(strstr(output, "(6/6)") != NULL,
          "source command count includes homing commands");

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

static void test_home_quick_completion_remains_synchronous(void)
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
    pid_t cli = start_printerctl_args("home", NULL, NULL, config_home, &out_fd);
    mock_expect_command(&mock, "G28\n");
    mock_reply_ok(&mock);
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    char output[512];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "home complete: all axes") != NULL,
          "quick HOME reports terminal completion");

    int ping = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ping);
    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

static void test_physical_command_waits_beyond_short_plp_deadline(bool raw_send)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    char config_home[128];
    snprintf(config_home, sizeof(config_home), "/tmp/printerctld-config-home-XXXXXX");
    check(mkdtemp(config_home) != NULL, "config home mkdtemp");

    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port_timeout(mock.slave_path, "8000");
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd = -1;
    pid_t cli = start_printerctl_args(raw_send ? "send" : "home",
                                    raw_send ? "M303 E0 S200 C6" : NULL,
                                    NULL, config_home, &out_fd);
    mock_expect_command(&mock, raw_send ? "M303 E0 S200 C6\n" : "G28\n");
    sleep_ms(5500);
    expect_child_still_running(cli);
    mock_reply_ok(&mock);
    expect_child_exit(cli, PRINTERCTL_EXIT_OK);
    char output[512];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, raw_send ? "< ok" : "home complete: all axes") != NULL,
          "delayed physical command reports terminal completion after ok");

    int ping = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ping);
    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

static void test_physical_command_marlin_timeout_is_daemon_authoritative(bool raw_send)
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
    pid_t cli = start_printerctl_args_combined(raw_send ? "send" : "home",
                                             raw_send ? "M303 E0 S200 C6" : NULL,
                                             NULL, config_home, &out_fd);
    mock_expect_command(&mock, raw_send ? "M303 E0 S200 C6\n" : "G28\n");
    expect_child_exit(cli, PRINTERCTL_EXIT_COMM_ERROR);
    char output[1024];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "wire_status=0x0403") != NULL ||
              strstr(output, "remote status 0x0403") != NULL,
          "physical command timeout is reported as PLP PRINTER_TIMEOUT");

    int ping = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ping);
    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

static void test_home_printer_error_is_propagated(void)
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
    pid_t cli = start_printerctl_args_combined("home", NULL, NULL, config_home, &out_fd);
    mock_expect_command(&mock, "G28\n");
    const char *error = "Error: simulated homing failure\n";
    check(write(mock.master, error, strlen(error)) == (ssize_t)strlen(error),
          "mock writes HOME printer error");
    expect_child_exit(cli, PRINTERCTL_EXIT_PRINTER_ERROR);
    char output[1024];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "wire_status=0x0401") != NULL ||
              strstr(output, "remote status 0x0401") != NULL,
          "HOME printer error is reported as PLP PRINTER_ERROR");

    int ping = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ping);
    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    rmdir(config_home);
}

static void run_home_disconnect_case(bool reset)
{
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port(mock.slave_path);
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int client = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    send_home(client, 330, PRINTERCTL_PLP1_HOME_ALL);
    mock_expect_command(&mock, "G28\n");
    if (reset) {
        struct linger linger_opt = {.l_onoff = 1, .l_linger = 0};
        check(setsockopt(client, SOL_SOCKET, SO_LINGER, &linger_opt,
                         sizeof(linger_opt)) == 0,
              "enable reset-on-close");
    }
    close(client);
    sleep_ms(100);
    mock_expect_no_command(&mock);
    mock_reply_ok(&mock);
    sleep_ms(250);
    mock_expect_no_command(&mock);

    int ping = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    uint8_t request[PRINTERCTL_PLP1_HEADER_SIZE];
    size_t request_length = encode_ping(request, sizeof(request));
    write_full(ping, request, request_length);
    printerctl_plp1_frame_t response =
        read_response_frame(ping, PRINTERCTL_PLP1_MSG_PING, 123);
    check(response.status == PRINTERCTL_PLP1_SUCCESS,
          "daemon remains alive after undeliverable HOME response");
    close(ping);

    stop_printerctld(daemon);
    mock_close(&mock);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
}

static void test_home_client_disconnect_after_g28_is_not_fatal(void)
{
    run_home_disconnect_case(false);
    run_home_disconnect_case(true);
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
    test_cli_power_low_battery_signals_daemon_owned_print();
    test_sigpwr_while_paused_cancels_with_power_failure_semantics();
    test_print_cancel_ack_window_blocks_next_command();
    test_print_advances_after_m109_without_secondary_request();
    test_print_progress_event_backlog_does_not_stall_execution();
    test_status_during_print_has_no_serial_side_effects();
    test_temp_during_print_is_busy_and_has_no_serial_side_effects();
    test_print_progress_advances_with_acknowledgements();
    test_print_pause_resume_preserves_progress();
    test_print_pause_resume_repeated_and_second_client();
    test_print_progress_client_independence();
    test_repeated_status_polling_has_no_serial_side_effects();
    test_cli_read_only_queries_use_daemon();
    test_cli_explicit_physical_commands_use_daemon();
    test_cli_pause_resume_use_daemon_print_control();
    test_cli_cancel_uses_daemon_print_control();
    test_cli_print_sigint_kills_observer_not_the_print();
    test_cli_print_uses_daemon_print_start();
    test_cli_print_source_error_is_daemon_reported();
    test_home_quick_completion_remains_synchronous();
    test_physical_command_waits_beyond_short_plp_deadline(false);
    test_physical_command_waits_beyond_short_plp_deadline(true);
    test_physical_command_marlin_timeout_is_daemon_authoritative(false);
    test_physical_command_marlin_timeout_is_daemon_authoritative(true);
    test_home_printer_error_is_propagated();
    test_home_client_disconnect_after_g28_is_not_fatal();
    return 0;
}
