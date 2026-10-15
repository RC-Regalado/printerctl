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

static void mock_expect_no_command(mock_printer_t *mock, const char *context)
{
    struct pollfd pfd;
    pfd.fd = mock->master;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
        char buffer[128];
        ssize_t n = read(mock->master, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            fprintf(stderr, "unexpected command in %s: %s\n", context, buffer);
        }
        check(0, "printer receives no unexpected command");
    }
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

static void configure_calibration(printerctl_config_t *config)
{
    config->has_calibration_min_x = true;
    config->has_calibration_max_x = true;
    config->has_calibration_min_y = true;
    config->has_calibration_max_y = true;
    config->has_calibration_safe_z = true;
    config->has_calibration_xy_feedrate = true;
    config->has_calibration_z_feedrate = true;
    config->calibration_min_x = 10.0;
    config->calibration_max_x = 90.0;
    config->calibration_min_y = 20.0;
    config->calibration_max_y = 120.0;
    config->calibration_safe_z = 5.0;
    config->calibration_xy_feedrate = 1500.0;
    config->calibration_z_feedrate = 300.0;
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

static printerctl_daemon_event_t send_and_wait_macro(printerctl_daemon_runtime_t *runtime,
                                                     const char *id)
{
    uint32_t token = 0;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_send_macro_execute(runtime, id, &token, &error) ==
              PRINTERCTL_DAEMON_OK,
          "send MACRO_EXECUTE");
    printerctl_daemon_event_t event;
    check(printerctl_daemon_runtime_wait_command(runtime, token, &event, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait MACRO_EXECUTE result");
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

static void expect_ack(mock_printer_t *mock, const char *command)
{
    mock_expect_command(mock, command);
    mock_reply_ok(mock);
}

static void test_macro_z_lift_executes_typed_relative_move(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_macro(&runtime, "maintenance-z-lift");
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "macro accepted");

    expect_ack(&mock, "G91\n");
    expect_ack(&mock, "G1 Z5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);
    mock_expect_no_command(&mock, "z-lift completion");

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    mock_close(&mock);
}

static void test_macro_front_left_calibration_sequence(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_macro(&runtime, "calibrate-front-left");
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "calibration macro accepted");

    expect_ack(&mock, "G91\n");
    expect_ack(&mock, "G1 Z5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    expect_ack(&mock, "G1 X10.000 Y20.000 F1500.000\n");
    expect_ack(&mock, "G91\n");
    expect_ack(&mock, "G1 Z-5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);
    mock_expect_no_command(&mock, "front-left completion");

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    mock_close(&mock);
}

static void run_calibration_point_case(const char *id, const char *expected_xy)
{
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_macro(&runtime, id);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "calibration macro accepted");

    expect_ack(&mock, "G91\n");
    expect_ack(&mock, "G1 Z5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    expect_ack(&mock, expected_xy);
    expect_ack(&mock, "G91\n");
    expect_ack(&mock, "G1 Z-5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    mock_close(&mock);
}

static void test_all_calibration_points_resolve_coordinates(void)
{
    run_calibration_point_case("calibrate-front-left", "G1 X10.000 Y20.000 F1500.000\n");
    run_calibration_point_case("calibrate-front-right", "G1 X90.000 Y20.000 F1500.000\n");
    run_calibration_point_case("calibrate-center", "G1 X50.000 Y70.000 F1500.000\n");
    run_calibration_point_case("calibrate-back-left", "G1 X10.000 Y120.000 F1500.000\n");
    run_calibration_point_case("calibrate-back-right", "G1 X90.000 Y120.000 F1500.000\n");
}

static void test_print_excludes_macro_without_serial_side_effects(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);
    char path[128];
    write_file(path, sizeof(path), "M105\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_start(&runtime, path);
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "PRINT_START accepted");
    mock_expect_command(&mock, "M105\n");

    printerctl_daemon_event_t macro = send_and_wait_macro(&runtime, "maintenance-z-lift");
    check(macro.result == PRINTERCTL_DAEMON_RESULT_ERROR &&
              macro.wire_status == PRINTERCTL_PLP1_BUSY,
          "macro is BUSY while print owns execution");
    mock_expect_no_command(&mock, "print excludes macro");
    mock_reply_ok(&mock);
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

static void test_macro_excludes_print_without_print_serial_side_effects(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);
    char path[128];
    write_file(path, sizeof(path), "M105\n");

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_macro(&runtime, "maintenance-z-lift");
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "macro accepted");
    mock_expect_command(&mock, "G91\n");

    uint32_t token = 0;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_send_print_start(&runtime, path, &token, &error) ==
              PRINTERCTL_DAEMON_OK,
          "send PRINT_START during macro");
    mock_reply_ok(&mock);
    printerctl_daemon_event_t busy;
    check(printerctl_daemon_runtime_wait_command(&runtime, token, &busy, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait busy print result");
    check(busy.result == PRINTERCTL_DAEMON_RESULT_ERROR && busy.wire_status == PRINTERCTL_PLP1_BUSY,
          "PRINT_START is BUSY while macro owns execution");

    expect_ack(&mock, "G1 Z5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);

    printerctl_daemon_runtime_shutdown(&runtime, &error);
    unlink(path);
    mock_close(&mock);
}

static void test_macro_error_stops_future_steps_without_cleanup(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_macro(&runtime, "calibrate-front-left");
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "macro accepted");

    expect_ack(&mock, "G91\n");
    expect_ack(&mock, "G1 Z5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    mock_expect_command(&mock, "G1 X10.000 Y20.000 F1500.000\n");
    mock_reply(&mock, "Error: simulated\n");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED);
    mock_expect_no_command(&mock, "macro error");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.macro_current_step == 1, "only first macro step completed");
    check(snapshot.failure_reason == PRINTERCTL_PLP1_PRINTER_ERROR,
          "macro printer error maps to terminal result");

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    mock_close(&mock);
}

static void test_macro_timeout_stops_future_steps_and_daemon_survives(void)
{
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);
    mock.config.timeout_ms = 200;

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t accepted = send_and_wait_macro(&runtime, "calibrate-front-left");
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "macro accepted");

    expect_ack(&mock, "G91\n");
    expect_ack(&mock, "G1 Z5.000 F300.000\n");
    mock_expect_command(&mock, "G90\n");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED);
    mock_expect_no_command(&mock, "macro timeout");

    uint32_t token = 0;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_send_nop(&runtime, &token, &error) == PRINTERCTL_DAEMON_OK,
          "daemon accepts NOP after macro timeout");
    printerctl_daemon_event_t nop;
    check(printerctl_daemon_runtime_wait_command(&runtime, token, &nop, &error) ==
              PRINTERCTL_DAEMON_OK,
          "NOP completes after macro timeout");
    check(nop.result == PRINTERCTL_DAEMON_RESULT_OK, "daemon remains healthy");

    printerctl_daemon_runtime_shutdown(&runtime, &error);
    mock_close(&mock);
}

static void test_invalid_or_missing_macro_config_has_zero_serial_side_effects(void)
{
    mock_printer_t mock;
    mock_open(&mock);

    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t missing = send_and_wait_macro(&runtime, "calibrate-front-left");
    check(missing.result == PRINTERCTL_DAEMON_RESULT_ERROR, "missing geometry rejected");
    mock_expect_no_command(&mock, "missing macro config");

    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_shutdown(&runtime, &error);

    configure_calibration(&mock.config);
    mock.config.calibration_min_x = mock.config.calibration_max_x;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_event_t invalid = send_and_wait_macro(&runtime, "calibrate-front-left");
    check(invalid.result == PRINTERCTL_DAEMON_RESULT_ERROR, "invalid geometry rejected");
    mock_expect_no_command(&mock, "invalid macro config");

    printerctl_daemon_runtime_shutdown(&runtime, &error);
    mock_close(&mock);
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
    test_macro_z_lift_executes_typed_relative_move();
    test_macro_front_left_calibration_sequence();
    test_all_calibration_points_resolve_coordinates();
    test_print_excludes_macro_without_serial_side_effects();
    test_macro_excludes_print_without_print_serial_side_effects();
    test_macro_error_stops_future_steps_without_cleanup();
    test_macro_timeout_stops_future_steps_and_daemon_survives();
    test_invalid_or_missing_macro_config_has_zero_serial_side_effects();
    test_print_completes_and_updates_status();
    test_pause_resume_and_second_start_busy();
    test_cancel_active_print();
    test_power_failure_cancel_is_distinct();
    test_legacy_serial_open_rejected_while_daemon_prints();
    test_shutdown_during_active_print_is_cooperative();
    test_printer_faults_surface_failed_state();
    return 0;
}
