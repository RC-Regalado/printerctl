/*
 * Mock-Marlin integration tests for the printer session, over
 * pseudo-terminals. The test owns the pty master (the "printer") and
 * scripts its behavior by writing response lines into the master after
 * the session connects (printerctl_serial_open() flushes stale input by
 * design, so scripting must follow the connect) — the pty buffers them,
 * the session reads them after transmitting its command, and the test
 * then reads the master side to assert exactly what was transmitted.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "printerctl/printer.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
}

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

/* One scripted mock printer: a pty pair plus the config that reaches it. */
typedef struct {
    int master;
    printerctl_config_t config;
} mock_printer_t;

static bool mock_open(mock_printer_t *mock, int timeout_ms)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        fprintf(stderr, "FAIL: cannot create pty: %s\n", strerror(errno));
        g_failures++;
        if (master >= 0) {
            close(master);
        }
        return false;
    }

    const char *name = ptsname(master);
    if (name == NULL) {
        fprintf(stderr, "FAIL: ptsname\n");
        g_failures++;
        close(master);
        return false;
    }

    mock->master = master;
    printerctl_config_set_defaults(&mock->config);
    snprintf(mock->config.port, sizeof(mock->config.port), "%s", name);
    mock->config.timeout_ms = timeout_ms;
    return true;
}

/* Scripts the printer's next response lines (already '\n'-terminated text). */
static void mock_reply(mock_printer_t *mock, const char *text)
{
    size_t length = strlen(text);
    if (write(mock->master, text, length) != (ssize_t)length) {
        fprintf(stderr, "FAIL: cannot script mock reply\n");
        g_failures++;
    }
}

/* Reads everything transmitted to the printer so far (up to ~200 ms of silence). */
static size_t mock_received(mock_printer_t *mock, char *buffer, size_t buffer_size)
{
    size_t received = 0;
    for (;;) {
        struct pollfd pfd = {.fd = mock->master, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 200) <= 0) {
            break;
        }
        ssize_t n = read(mock->master, buffer + received, buffer_size - 1 - received);
        if (n <= 0) {
            break;
        }
        received += (size_t)n;
        if (received >= buffer_size - 1) {
            break;
        }
    }
    buffer[received] = '\0';
    return received;
}

static void mock_close(mock_printer_t *mock)
{
    close(mock->master);
}

/* Collects callback lines into a fixed transcript for assertions. */
typedef struct {
    char lines[8][PRINTERCTL_PRINTER_LINE_MAX];
    size_t count;
} transcript_t;

static void collect_line(const char *line, void *context)
{
    transcript_t *transcript = context;
    if (transcript->count < 8) {
        snprintf(transcript->lines[transcript->count], PRINTERCTL_PRINTER_LINE_MAX,
                 "%s", line);
        transcript->count++;
    }
}

static void test_query_temperature_from_ok_payload(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    check(printerctl_printer_connect(&session, &mock.config, NULL, &error) ==
              PRINTERCTL_PRINTER_OK,
          "session connects to the mock printer");

    mock_reply(&mock, "ok T:210.00 /210.00 B:60.00 /60.00 @:127 B@:0\n");

    printerctl_marlin_temperature_report_t report;
    printerctl_printer_status_t status =
        printerctl_printer_query_temperature(&session, &report, &error);
    check(status == PRINTERCTL_PRINTER_OK, "temperature query succeeds");
    check(report.has_hotend && near(report.hotend_actual, 210.0) &&
              near(report.hotend_target, 210.0),
          "hotend values decode from the ok payload");
    check(report.has_bed && near(report.bed_actual, 60.0), "bed values decode");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "M105\n") == 0, "temp transmits exactly 'M105\\n'");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
}

static void test_query_temperature_from_report_line_before_ok(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    mock_reply(&mock, "T:23.5 /0.0 B:22.9 /0.0 @:0 B@:0\nok\n");

    printerctl_marlin_temperature_report_t report;
    check(printerctl_printer_query_temperature(&session, &report, &error) ==
              PRINTERCTL_PRINTER_OK,
          "temperature query succeeds when the report precedes the ok");
    check(report.has_hotend && near(report.hotend_actual, 23.5),
          "hotend values decode from the standalone report line");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
}

static void test_busy_lines_are_waited_out(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    mock_reply(&mock,
               "echo:busy: processing\n"
               "echo:busy: processing\n"
               "ok T:100.0 /200.0\n");

    printerctl_marlin_temperature_report_t report;
    check(printerctl_printer_query_temperature(&session, &report, &error) ==
              PRINTERCTL_PRINTER_OK,
          "busy keepalives are waited out until the terminal ok");
    check(near(report.hotend_actual, 100.0), "the report after the busy lines decodes");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
}

static void test_query_position(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    mock_reply(&mock, "X:12.50 Y:-3.25 Z:0.40 E:120.75 Count X:1000 Y:-260 Z:160\nok\n");

    printerctl_marlin_position_report_t report;
    check(printerctl_printer_query_position(&session, &report, &error) ==
              PRINTERCTL_PRINTER_OK,
          "position query succeeds");
    check(near(report.x, 12.5) && near(report.y, -3.25) && near(report.z, 0.4) &&
              report.has_e && near(report.e, 120.75),
          "position values decode");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "M114\n") == 0, "position transmits exactly 'M114\\n'");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
}

static void test_query_info(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    mock_reply(&mock,
               "FIRMWARE_NAME:Marlin 2.1.2 MACHINE_TYPE:EasyThreed K9 EXTRUDER_COUNT:1 "
               "UUID:cede2a2f-41a2-4748-9b12-c55c62f367ff\n"
               "ok\n");

    printerctl_marlin_firmware_info_t info;
    check(printerctl_printer_query_info(&session, &info, &error) == PRINTERCTL_PRINTER_OK,
          "info query succeeds");
    check(strcmp(info.firmware_name, "Marlin 2.1.2") == 0, "firmware name decodes");
    check(strcmp(info.machine_type, "EasyThreed K9") == 0, "machine type decodes");
    check(info.has_extruder_count && info.extruder_count == 1, "extruder count decodes");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "M115\n") == 0, "info transmits exactly 'M115\\n'");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
}

static void test_send_collects_full_exchange(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    mock_reply(&mock, "echo:Settings Stored\nok\n");

    transcript_t transcript = {.count = 0};
    check(printerctl_printer_send(&session, "M500", collect_line, &transcript, &error) ==
              PRINTERCTL_PRINTER_OK,
          "send succeeds with acknowledgement handling");
    check(transcript.count == 2, "send passes every received line to the callback");
    check(transcript.count == 2 &&
              strcmp(transcript.lines[0], "echo:Settings Stored") == 0 &&
              strcmp(transcript.lines[1], "ok") == 0,
          "the callback sees the lines in arrival order, terminal included");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "M500\n") == 0, "send transmits exactly the raw command");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
}

static void test_send_blocks_g28_before_transmission(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    const char *variants[] = {"G28", "g28", "G28 X0", "  G28", "N10 G28", "G028.1"};
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        printerctl_printer_status_t status =
            printerctl_printer_send(&session, variants[i], NULL, NULL, &error);
        check(status == PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND,
              "a G28 variant through send is rejected as unsafe");
        check(error.message[0] != '\0', "the G28 rejection carries a message");
    }

    /* M112 (explicit emergency stop) is the operator speaking: allowed. */
    mock_reply(&mock, "ok\n");
    check(printerctl_printer_send(&session, "M112", NULL, NULL, &error) ==
              PRINTERCTL_PRINTER_OK,
          "the operator's explicit M112 passes through send");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "M112\n") == 0,
          "nothing was transmitted for the rejected G28 variants");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
}

static void test_printer_error_and_resend(void)
{
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        mock_reply(&mock, "Error:Printer halted. kill() called!\n");

        printerctl_marlin_temperature_report_t report;
        printerctl_printer_status_t status =
            printerctl_printer_query_temperature(&session, &report, &error);
        check(status == PRINTERCTL_PRINTER_ERR_PRINTER_ERROR,
              "an Error: reply becomes ERR_PRINTER_ERROR");
        check(strstr(error.message, "Printer halted") != NULL,
              "the printer's error message is carried through");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
    }
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        mock_reply(&mock, "Resend: 5\n");

        printerctl_printer_status_t status =
            printerctl_printer_send(&session, "M400", NULL, NULL, &error);
        check(status == PRINTERCTL_PRINTER_ERR_RESEND,
              "a Resend: reply becomes the explicit unsupported-resend status");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
    }
}

static void test_timeout_and_missing_report(void)
{
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 150)) {
            return;
        }
        /* Silence: no scripted reply at all. */
        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        printerctl_marlin_temperature_report_t report;
        check(printerctl_printer_query_temperature(&session, &report, &error) ==
                  PRINTERCTL_PRINTER_ERR_TIMEOUT,
              "a silent printer reports an explicit timeout");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
    }
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        mock_reply(&mock, "ok\n"); /* acknowledged, but no report */

        printerctl_marlin_temperature_report_t report;
        check(printerctl_printer_query_temperature(&session, &report, &error) ==
                  PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE,
              "an ok without a report is an explicit unexpected-response failure");
        check(!report.has_hotend && !report.has_bed,
              "the missing report leaves the output zeroed");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
    }
}

static void test_disconnected_printer(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 500)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    mock_close(&mock); /* the printer vanishes mid-session */

    transcript_t transcript = {.count = 0};
    printerctl_printer_status_t status =
        printerctl_printer_send(&session, "M105", collect_line, &transcript, &error);
    check(status == PRINTERCTL_PRINTER_ERR_SERIAL || status == PRINTERCTL_PRINTER_ERR_TIMEOUT,
          "a vanished printer fails the exchange explicitly");
    check(status != PRINTERCTL_PRINTER_OK, "a vanished printer never acknowledges");

    printerctl_printer_disconnect(&session, &error);
}

static void test_connect_guards(void)
{
    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_config_t config;

    /* Empty port. */
    printerctl_config_set_defaults(&config);
    check(printerctl_printer_connect(&session, &config, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_NO_PORT,
          "connect without a configured port reports NO_PORT");
    check(strstr(error.message, "--port") != NULL,
          "the NO_PORT message tells the operator what to pass");

    /* Dry-run must never reach a connection. */
    printerctl_config_set_defaults(&config);
    snprintf(config.port, sizeof(config.port), "/dev/null");
    config.dry_run = true;
    check(printerctl_printer_connect(&session, &config, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
          "connect refuses a dry-run configuration");

    /* Unopenable device. */
    printerctl_config_set_defaults(&config);
    snprintf(config.port, sizeof(config.port), "/nonexistent/ttyACM99");
    check(printerctl_printer_connect(&session, &config, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_SERIAL,
          "connect maps an unopenable device to ERR_SERIAL");

    /* NULL arguments. */
    check(printerctl_printer_connect(NULL, &config, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
          "connect with a NULL session is rejected");
    check(printerctl_printer_connect(&session, NULL, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
          "connect with a NULL config is rejected");
}

static void test_lifecycle(void)
{
    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_config_t config;
    printerctl_config_set_defaults(&config);

    /* Not connected (rejected connect resets the session). */
    printerctl_printer_connect(&session, &config, NULL, &error);
    check(printerctl_printer_execute(&session, "M105", NULL, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_NOT_CONNECTED,
          "execute on an unconnected session reports NOT_CONNECTED");
    check(printerctl_printer_disconnect(&session, &error) == PRINTERCTL_PRINTER_OK,
          "disconnect on an unconnected session is a no-op");

    /* Connected lifecycle. */
    mock_printer_t mock;
    if (!mock_open(&mock, 500)) {
        return;
    }
    check(printerctl_printer_connect(&session, &mock.config, NULL, &error) ==
              PRINTERCTL_PRINTER_OK,
          "session connects for the lifecycle test");
    check(printerctl_printer_disconnect(&session, &error) == PRINTERCTL_PRINTER_OK,
          "disconnect succeeds");
    check(!session.connected, "disconnect marks the session unconnected");
    check(printerctl_printer_disconnect(&session, &error) == PRINTERCTL_PRINTER_OK,
          "a second disconnect is a no-op");
    check(printerctl_printer_execute(&session, "M105", NULL, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_NOT_CONNECTED,
          "execute after disconnect reports NOT_CONNECTED");

    mock_close(&mock);
}

int main(void)
{
    test_query_temperature_from_ok_payload();
    test_query_temperature_from_report_line_before_ok();
    test_busy_lines_are_waited_out();
    test_query_position();
    test_query_info();
    test_send_collects_full_exchange();
    test_send_blocks_g28_before_transmission();
    test_printer_error_and_resend();
    test_timeout_and_missing_report();
    test_disconnected_printer();
    test_connect_guards();
    test_lifecycle();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All printer checks passed\n");
    return EXIT_SUCCESS;
}
