#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "printerctl/daemon/runtime.h"
#include "printerctl/printer.h"
#include "printerctl/protocol/plp1.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

typedef struct {
    int master;
    printerctl_config_t config;
} mock_printer_t;

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

static void mock_reply(mock_printer_t *mock, const char *text)
{
    size_t length = strlen(text);
    check(write(mock->master, text, length) == (ssize_t)length, "mock writes reply");
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

static void write_file(char *path, size_t path_size, const char *contents)
{
    snprintf(path, path_size, "/tmp/printerctl-daemon-print-XXXXXX");
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    size_t length = strlen(contents);
    check(write(fd, contents, length) == (ssize_t)length, "write gcode file");
    close(fd);
}

static void start_runtime(printerctl_daemon_runtime_t *runtime, const printerctl_config_t *config)
{
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_start_with_config(runtime, config, NULL, NULL, &error) ==
              PRINTERCTL_DAEMON_OK,
          "runtime starts with config");
}

static printerctl_daemon_event_t send_and_wait_start(printerctl_daemon_runtime_t *runtime,
                                                     const char *path)
{
    uint32_t token = 0;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_send_print_start(runtime, path, &token, &error) ==
              PRINTERCTL_DAEMON_OK,
          "send PRINT_START");
    printerctl_daemon_event_t event;
    check(printerctl_daemon_runtime_wait_command(runtime, token, &event, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait PRINT_START result");
    return event;
}

static void wait_control(printerctl_daemon_runtime_t *runtime,
                         printerctl_daemon_status_t (*send_fn)(
                             printerctl_daemon_runtime_t *, uint32_t *, printerctl_daemon_error_t *),
                         const char *message)
{
    uint32_t token = 0;
    printerctl_daemon_error_t error;
    check(send_fn(runtime, &token, &error) == PRINTERCTL_DAEMON_OK, message);
    printerctl_daemon_event_t event;
    check(printerctl_daemon_runtime_wait_command(runtime, token, &event, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait control result");
    check(event.result == PRINTERCTL_DAEMON_RESULT_OK, "control accepted");
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

static void test_print_completes_and_updates_status(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    char path[128];
    write_file(path, sizeof(path), "M105\nM114\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");

    mock_expect_command(&mock, "M105\n");
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "M114\n");
    mock_reply_ok(&mock);
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.progress_percent == 100, "progress reaches 100");
    check(snapshot.commands_acknowledged == 2, "two commands acknowledged");

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

static void test_pause_resume_and_second_start_busy(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    char path[128];
    write_file(path, sizeof(path), "M105\nM114\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");

    mock_expect_command(&mock, "M105\n");
    uint32_t pause_token = 0;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_send_print_pause(&runtime, &pause_token, &error) ==
              PRINTERCTL_DAEMON_OK,
          "send PAUSE");
    mock_reply_ok(&mock);
    printerctl_daemon_event_t pause_event;
    check(printerctl_daemon_runtime_wait_command(&runtime, pause_token, &pause_event, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait PAUSE result");
    check(pause_event.result == PRINTERCTL_DAEMON_RESULT_OK, "PAUSE accepted");

    printerctl_daemon_event_t second = send_and_wait_start(&runtime, path);
    check(second.result == PRINTERCTL_DAEMON_RESULT_ERROR, "second PRINT_START rejected");

    wait_control(&runtime, printerctl_daemon_runtime_send_print_resume, "send RESUME");
    mock_expect_command(&mock, "M114\n");
    mock_reply_ok(&mock);
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

static void test_cancel_active_print(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    char path[128];
    write_file(path, sizeof(path), "M105\nM114\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");

    mock_expect_command(&mock, "M105\n");
    uint32_t cancel_token = 0;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_send_print_cancel(&runtime, &cancel_token, &error) ==
              PRINTERCTL_DAEMON_OK,
          "send CANCEL");
    mock_reply_ok(&mock);
    printerctl_daemon_event_t cancel_event;
    check(printerctl_daemon_runtime_wait_command(&runtime, cancel_token, &cancel_event, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait CANCEL result");
    check(cancel_event.result == PRINTERCTL_DAEMON_RESULT_OK, "CANCEL accepted");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED);

    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

static void test_power_failure_cancel_is_distinct(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    char path[128];
    write_file(path, sizeof(path), "M105\nM114\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");

    mock_expect_command(&mock, "M105\n");
    uint32_t token = 0;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_send_print_cancel_power_failure(&runtime, &token, &error) ==
              PRINTERCTL_DAEMON_OK,
          "send power-failure cancel");
    mock_reply_ok(&mock);
    printerctl_daemon_event_t cancel_event;
    check(printerctl_daemon_runtime_wait_command(&runtime, token, &cancel_event, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait power-failure cancel result");
    check(cancel_event.result == PRINTERCTL_DAEMON_RESULT_OK, "power-failure cancel accepted");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED);

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.failure_reason == PRINTERCTL_PLP1_SHUTTING_DOWN,
          "power-failure cancellation remains distinguishable");

    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

static void test_legacy_serial_open_rejected_while_daemon_prints(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    char path[128];
    write_file(path, sizeof(path), "M105\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");
    mock_expect_command(&mock, "M105\n");

    printerctl_printer_session_t legacy;
    printerctl_printer_error_t legacy_error;
    check(printerctl_printer_connect(&legacy, &mock.config, NULL, &legacy_error) ==
              PRINTERCTL_PRINTER_ERR_SERIAL,
          "legacy serial open is rejected while daemon owns serial");

    mock_reply_ok(&mock);
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

typedef struct {
    printerctl_daemon_runtime_t *runtime;
    printerctl_daemon_status_t status;
} shutdown_thread_t;

static void *shutdown_thread_main(void *arg)
{
    shutdown_thread_t *ctx = (shutdown_thread_t *)arg;
    printerctl_daemon_error_t error;
    ctx->status = printerctl_daemon_runtime_shutdown(ctx->runtime, &error);
    return NULL;
}

static void test_shutdown_during_active_print_is_cooperative(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    char path[128];
    write_file(path, sizeof(path), "M105\nM114\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");
    mock_expect_command(&mock, "M105\n");

    shutdown_thread_t shutdown_ctx;
    shutdown_ctx.runtime = &runtime;
    shutdown_ctx.status = PRINTERCTL_DAEMON_ERR_IO;
    pthread_t thread;
    check(pthread_create(&thread, NULL, shutdown_thread_main, &shutdown_ctx) == 0,
          "shutdown thread starts");

    poll(NULL, 0, 50);

    mock_reply_ok(&mock);
    check(pthread_join(thread, NULL) == 0, "shutdown thread joins");
    check(shutdown_ctx.status == PRINTERCTL_DAEMON_OK, "shutdown succeeds");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_STOPPED, "daemon stopped after active shutdown");
    check(snapshot.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED,
          "active shutdown cancels through engine lifecycle");

    unlink(path);
    mock_close(&mock);
}

static void run_fault_case(const char *reply, uint16_t expected_reason)
{
    mock_printer_t mock;
    mock_open(&mock);
    mock.config.timeout_ms = 200;
    char path[128];
    write_file(path, sizeof(path), "M105\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");

    mock_expect_command(&mock, "M105\n");
    if (reply != NULL) {
        mock_reply(&mock, reply);
    }
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED);

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.failure_reason == expected_reason, "failure reason matches expected protocol status");

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

static void test_printer_faults_surface_failed_state(void)
{
    run_fault_case("Error: simulated\n", PRINTERCTL_PLP1_PRINTER_ERROR);
    run_fault_case("Resend: 1\n", PRINTERCTL_PLP1_PRINTER_ERROR);
    run_fault_case(NULL, PRINTERCTL_PLP1_PRINTER_TIMEOUT);
}

int main(void)
{
    test_print_completes_and_updates_status();
    test_pause_resume_and_second_start_busy();
    test_cancel_active_print();
    test_power_failure_cancel_is_distinct();
    test_legacy_serial_open_rejected_while_daemon_prints();
    test_shutdown_during_active_print_is_cooperative();
    test_printer_faults_surface_failed_state();
    return 0;
}
