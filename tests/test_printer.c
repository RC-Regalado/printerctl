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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

/* Writes `content` to a fresh temp G-code file and returns its path via `path`. */
static void write_temp_gcode(const char *content, char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/printerctl_printer_gcode_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "FAIL: mkstemp() failed for temp G-code fixture\n");
        g_failures++;
        return;
    }

    FILE *file = fdopen(fd, "w");
    fputs(content, file);
    fclose(file);
}

/* Collects per-acknowledgement progress callbacks for assertions. */
typedef struct {
    uint64_t calls;
    uint64_t last_line;
    unsigned last_percent;
    bool monotonic;
} progress_probe_t;

static void collect_progress(const printerctl_gcode_result_t *result, void *context)
{
    progress_probe_t *probe = context;
    if (probe->calls > 0 && result->progress_percent < probe->last_percent) {
        probe->monotonic = false;
    }
    probe->calls++;
    probe->last_line = result->line_number;
    probe->last_percent = result->progress_percent;
}

static void test_print_file_completes_and_reports_progress(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    char path[64];
    write_temp_gcode("; header\nG21\nG90 ; absolute\n\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\nok\nok\n");

    progress_probe_t probe = {.monotonic = true};
    printerctl_print_summary_t summary;
    printerctl_printer_status_t status = printerctl_printer_print_file(
        &session, path, collect_progress, &probe, NULL, NULL, &summary, &error);

    check(status == PRINTERCTL_PRINTER_OK, "a clean print completes");
    check(summary.state == PRINTERCTL_PRINT_COMPLETED, "the summary reports the completed state");
    check(summary.commands_sent == 3 && summary.commands_acknowledged == 3,
          "the summary counts sent and acknowledged commands");
    check(summary.last_acknowledged_line == 5, "the summary names the last acknowledged source line");
    check(summary.progress_percent == 100, "a completed print reports 100%");
    check(summary.file_size > 0 && summary.bytes_consumed == summary.file_size,
          "a completed print consumed the whole file");
    check(probe.calls == 3, "the progress callback ran once per acknowledged command");
    check(probe.monotonic && probe.last_percent == 100,
          "progress is monotonic and ends at 100%");

    char transmitted[128];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "G21\nG90\nM105\n") == 0,
          "exactly the normalized commands were transmitted, in order");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
    remove(path);
}

/*
 * The no-flooding acceptance test: a strict mock that, after reading
 * each complete command line, verifies nothing else arrives before it
 * sends its `ok`. If the session ever transmits command N+1 before
 * command N was acknowledged, the child exits with status 2.
 */
static pid_t spawn_strict_mock(int master, int expected_commands)
{
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }

    for (int i = 0; i < expected_commands; i++) {
        char c;
        for (;;) {
            if (read(master, &c, 1) <= 0) {
                _exit(1);
            }
            if (c == '\n') {
                break;
            }
        }

        struct pollfd pfd = {.fd = master, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 150) != 0) {
            _exit(2); /* flooding: more data arrived before our ok */
        }

        if (write(master, "ok\n", 3) != 3) {
            _exit(1);
        }
    }
    _exit(0);
}

static void test_print_file_never_floods_the_serial_buffer(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 2000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\nM400\n", path, sizeof(path));

    pid_t mock_pid = spawn_strict_mock(mock.master, 4);

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status =
        printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

    check(status == PRINTERCTL_PRINTER_OK, "the strictly paced print completes");

    if (status != PRINTERCTL_PRINTER_OK) {
        kill(mock_pid, SIGKILL); /* avoid a blocked child on failure */
    }
    int child_status = 0;
    waitpid(mock_pid, &child_status, 0);
    check(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "the strict mock observed no command before the previous acknowledgement");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
    remove(path);
}

static void test_print_file_stops_on_error(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\nError:Thermal Runaway\n");

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status =
        printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

    check(status == PRINTERCTL_PRINTER_ERR_PRINTER_ERROR,
          "an Error: reply stops the print with the printer-error status");
    check(summary.state == PRINTERCTL_PRINT_FAILED, "the summary reports the failed state");
    check(summary.commands_sent == 2 && summary.commands_acknowledged == 1,
          "the summary shows the failing command was the second one");
    check(summary.last_acknowledged_line == 1, "the last acknowledged line is the first command's");
    check(strstr(error.message, "Thermal Runaway") != NULL,
          "the printer's error message is carried through");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "G21\nG90\n") == 0,
          "no command after the failing one was transmitted");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
    remove(path);
}

static void test_print_file_stops_on_resend_and_timeout(void)
{
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG90\n", path, sizeof(path));
        mock_reply(&mock, "ok\nResend: 2\n");

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status =
            printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

        check(status == PRINTERCTL_PRINTER_ERR_RESEND,
              "a Resend: reply stops the print with the explicit unsupported status");
        check(summary.state == PRINTERCTL_PRINT_FAILED && summary.commands_acknowledged == 1,
              "the resend summary shows where streaming stopped");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 150)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG90\n", path, sizeof(path));
        mock_reply(&mock, "ok\n"); /* then silence */

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status =
            printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

        check(status == PRINTERCTL_PRINTER_ERR_TIMEOUT,
              "acknowledgement silence stops the print with an explicit timeout");
        check(summary.state == PRINTERCTL_PRINT_FAILED &&
                  summary.commands_sent == 2 && summary.commands_acknowledged == 1,
              "the timeout summary shows where streaming stopped");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
}

static void test_print_file_blocks_g28_and_file_errors(void)
{
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG28\nG90\n", path, sizeof(path));
        mock_reply(&mock, "ok\n");

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status =
            printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

        check(status == PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND,
              "a G28 in the file stops the print as an unsafe command");
        check(strstr(error.message, "line 2") != NULL, "the G28 rejection names its line");
        check(summary.commands_sent == 1, "the G28 line itself was never transmitted");

        char transmitted[64];
        mock_received(&mock, transmitted, sizeof(transmitted));
        check(strcmp(transmitted, "G21\n") == 0, "only the command before the G28 went out");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG1 (never closed\n", path, sizeof(path));
        mock_reply(&mock, "ok\n");

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status =
            printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

        check(status == PRINTERCTL_PRINTER_ERR_GCODE,
              "a malformed line mid-stream stops the print with the G-code status");
        check(summary.state == PRINTERCTL_PRINT_FAILED && summary.commands_acknowledged == 1,
              "the file-error summary shows where streaming stopped");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
    {
        /* Unopenable file: fails before anything is transmitted. */
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status = printerctl_printer_print_file(
            &session, "/nonexistent/job.gcode", NULL, NULL, NULL, NULL, &summary, &error);

        check(status == PRINTERCTL_PRINTER_ERR_GCODE,
              "an unopenable file fails with the G-code status");
        check(summary.commands_sent == 0, "nothing was transmitted for an unopenable file");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
    }
}

static void test_print_file_waits_out_busy_and_handles_empty_file(void)
{
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG90\n", path, sizeof(path));
        mock_reply(&mock, "echo:busy: processing\nok\nok\n");

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status =
            printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

        check(status == PRINTERCTL_PRINTER_OK,
              "busy keepalives during a print are waited out");
        check(summary.commands_acknowledged == 2, "both commands were acknowledged");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("", path, sizeof(path));

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status =
            printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL, &summary, &error);

        check(status == PRINTERCTL_PRINTER_OK, "an empty file completes trivially");
        check(summary.state == PRINTERCTL_PRINT_COMPLETED && summary.commands_sent == 0 &&
                  summary.progress_percent == 100,
              "the empty-file summary reports completion with zero commands");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
}

/*
 * Scripted session control: a fixed sequence of decisions, one per
 * poll, then CONTINUE forever. While paused it also watches the mock's
 * master side: anything transmitted between pause polls is flooding.
 */
typedef struct {
    const printerctl_print_control_t *decisions;
    size_t count;
    size_t polls;
    int master;              /* -1 disables the paused-TX watch */
    bool was_paused;
    bool tx_while_paused;
} control_script_t;

static printerctl_print_control_t scripted_control(void *context)
{
    control_script_t *script = context;
    printerctl_print_control_t decision =
        script->polls < script->count ? script->decisions[script->polls]
                                      : PRINTERCTL_PRINT_CONTROL_CONTINUE;
    script->polls++;

    bool paused = decision == PRINTERCTL_PRINT_CONTROL_PAUSE;
    if (paused && script->master >= 0) {
        struct pollfd pfd = {.fd = script->master, .events = POLLIN, .revents = 0};
        if (!script->was_paused) {
            /* Drain what was legitimately transmitted before the pause,
             * so later polls only see pause-time traffic. */
            char sink[256];
            while (poll(&pfd, 1, 0) > 0 && read(script->master, sink, sizeof(sink)) > 0) {
            }
        } else if (poll(&pfd, 1, 0) != 0) {
            script->tx_while_paused = true;
        }
    }
    script->was_paused = paused;

    return decision;
}

static void test_print_file_pause_transmits_nothing_and_resume_continues(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\nok\nok\n");

    /* Stream one command, pause for three polls, then resume. */
    static const printerctl_print_control_t decisions[] = {
        PRINTERCTL_PRINT_CONTROL_CONTINUE,
        PRINTERCTL_PRINT_CONTROL_PAUSE,
        PRINTERCTL_PRINT_CONTROL_PAUSE,
        PRINTERCTL_PRINT_CONTROL_PAUSE,
        PRINTERCTL_PRINT_CONTROL_CONTINUE,
    };
    control_script_t script = {
        .decisions = decisions,
        .count = sizeof(decisions) / sizeof(decisions[0]),
        .master = mock.master,
    };

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status = printerctl_printer_print_file(
        &session, path, NULL, NULL, scripted_control, &script, &summary, &error);

    check(status == PRINTERCTL_PRINTER_OK, "a paused-then-resumed print completes");
    check(summary.state == PRINTERCTL_PRINT_COMPLETED &&
              summary.commands_sent == 3 && summary.commands_acknowledged == 3,
          "resume continued from the next unsent command to the end");
    check(!script.tx_while_paused, "nothing was transmitted while the print was paused");
    check(script.polls >= 6, "the control callback was re-polled while paused");

    /* The pause-time drain consumed 'G21\n'; the rest must follow in order. */
    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "G90\nM105\n") == 0,
          "the commands after the pause went out in order");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
    remove(path);
}

static void test_print_file_cancel_stops_after_current_acknowledgement(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\n");

    static const printerctl_print_control_t decisions[] = {
        PRINTERCTL_PRINT_CONTROL_CONTINUE,
        PRINTERCTL_PRINT_CONTROL_CANCEL,
    };
    control_script_t script = {
        .decisions = decisions,
        .count = sizeof(decisions) / sizeof(decisions[0]),
        .master = -1,
    };

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status = printerctl_printer_print_file(
        &session, path, NULL, NULL, scripted_control, &script, &summary, &error);

    check(status == PRINTERCTL_PRINTER_CANCELLED,
          "an operator cancel returns the explicit cancelled status");
    check(summary.state == PRINTERCTL_PRINT_CANCELLED &&
              summary.status == PRINTERCTL_PRINTER_CANCELLED,
          "the summary reports the cancelled state and status");
    check(summary.commands_sent == 1 && summary.commands_acknowledged == 1 &&
              summary.last_acknowledged_line == 1,
          "the cancel took effect after the current acknowledgement");
    check(strstr(error.message, "cancel") != NULL, "the cancel carries an operator-facing message");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "G21\n") == 0, "nothing after the cancel point was transmitted");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
    remove(path);
}

/*
 * Milestone 10: an external power-failure/low-battery notification
 * cancels exactly like an operator cancel structurally, but must be
 * distinguishable afterwards -- a different status and a different
 * final state -- so the CLI's summary and session record can say which
 * one actually happened.
 */
static void test_print_file_cancel_power_failure_stops_after_current_acknowledgement(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\n");

    static const printerctl_print_control_t decisions[] = {
        PRINTERCTL_PRINT_CONTROL_CONTINUE,
        PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE,
    };
    control_script_t script = {
        .decisions = decisions,
        .count = sizeof(decisions) / sizeof(decisions[0]),
        .master = -1,
    };

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status = printerctl_printer_print_file(
        &session, path, NULL, NULL, scripted_control, &script, &summary, &error);

    check(status == PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE,
          "a power-failure cancel returns its own distinct status, not the operator one");
    check(summary.state == PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE &&
              summary.status == PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE,
          "the summary reports the power-failure cancelled state and status");
    check(summary.commands_sent == 1 && summary.commands_acknowledged == 1 &&
              summary.last_acknowledged_line == 1,
          "the power-failure cancel took effect after the current acknowledgement");
    check(strstr(error.message, "power") != NULL,
          "the power-failure cancel carries a message naming the real cause");

    char transmitted[64];
    mock_received(&mock, transmitted, sizeof(transmitted));
    check(strcmp(transmitted, "G21\n") == 0, "nothing after the cancel point was transmitted");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
    remove(path);
}

/*
 * A power-failure signal must win even while paused: it must not have
 * to wait for a resume that respects the operator's pause first.
 */
static void test_print_file_cancel_power_failure_takes_priority_while_paused(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_connect(&session, &mock.config, NULL, &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\n");

    static const printerctl_print_control_t decisions[] = {
        PRINTERCTL_PRINT_CONTROL_CONTINUE,
        PRINTERCTL_PRINT_CONTROL_PAUSE,
        PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE,
    };
    control_script_t script = {
        .decisions = decisions,
        .count = sizeof(decisions) / sizeof(decisions[0]),
        .master = -1,
    };

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status = printerctl_printer_print_file(
        &session, path, NULL, NULL, scripted_control, &script, &summary, &error);

    check(status == PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE,
          "a power-failure signal cancels a paused print instead of waiting for resume");
    check(summary.state == PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE,
          "the summary reports the power-failure cancelled state");

    printerctl_printer_disconnect(&session, &error);
    mock_close(&mock);
    remove(path);
}

static void test_print_file_cancel_before_first_command_and_while_paused(void)
{
    {
        /* Cancel before anything is streamed. */
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG90\n", path, sizeof(path));

        static const printerctl_print_control_t decisions[] = {
            PRINTERCTL_PRINT_CONTROL_CANCEL,
        };
        control_script_t script = {.decisions = decisions, .count = 1, .master = -1};

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status = printerctl_printer_print_file(
            &session, path, NULL, NULL, scripted_control, &script, &summary, &error);

        check(status == PRINTERCTL_PRINTER_CANCELLED,
              "cancelling before the first command reports cancelled");
        check(summary.state == PRINTERCTL_PRINT_CANCELLED && summary.commands_sent == 0,
              "nothing was transmitted for a cancel before the first command");

        char transmitted[64];
        check(mock_received(&mock, transmitted, sizeof(transmitted)) == 0,
              "the mock received no bytes at all");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
    {
        /* Cancel while paused. */
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG90\n", path, sizeof(path));
        mock_reply(&mock, "ok\n");

        static const printerctl_print_control_t decisions[] = {
            PRINTERCTL_PRINT_CONTROL_CONTINUE,
            PRINTERCTL_PRINT_CONTROL_PAUSE,
            PRINTERCTL_PRINT_CONTROL_CANCEL,
        };
        control_script_t script = {.decisions = decisions, .count = 3, .master = -1};

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status = printerctl_printer_print_file(
            &session, path, NULL, NULL, scripted_control, &script, &summary, &error);

        check(status == PRINTERCTL_PRINTER_CANCELLED, "cancelling while paused reports cancelled");
        check(summary.state == PRINTERCTL_PRINT_CANCELLED && summary.commands_sent == 1,
              "the paused print stopped where the pause left it");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }
}

static void test_sd_print_controls_send_marlin_commands(void)
{
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        mock_reply(&mock, "ok\nok\nok\n");

        check(printerctl_printer_pause_sd_print(&session, NULL, NULL, &error) ==
                  PRINTERCTL_PRINTER_OK,
              "the SD pause exchange succeeds");
        check(printerctl_printer_resume_sd_print(&session, NULL, NULL, &error) ==
                  PRINTERCTL_PRINTER_OK,
              "the SD resume exchange succeeds");
        check(printerctl_printer_cancel_sd_print(&session, NULL, NULL, &error) ==
                  PRINTERCTL_PRINTER_OK,
              "the SD cancel exchange succeeds");

        char transmitted[64];
        mock_received(&mock, transmitted, sizeof(transmitted));
        check(strcmp(transmitted, "M25\nM24\nM524\n") == 0,
              "pause/resume/cancel map to exactly M25, M24 and M524");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
    }
    {
        /* An Error: reply fails the control like any other exchange. */
        mock_printer_t mock;
        if (!mock_open(&mock, 1000)) {
            return;
        }

        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        mock_reply(&mock, "Error:Printer halted. kill() called!\n");

        check(printerctl_printer_pause_sd_print(&session, NULL, NULL, &error) ==
                  PRINTERCTL_PRINTER_ERR_PRINTER_ERROR,
              "an Error: reply to M25 becomes ERR_PRINTER_ERROR");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
    }
    {
        /* Session-lifecycle guard: not connected. */
        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_config_t config;
        printerctl_config_set_defaults(&config);
        printerctl_printer_connect(&session, &config, NULL, &error); /* rejected: no port */

        check(printerctl_printer_pause_sd_print(&session, NULL, NULL, &error) ==
                  PRINTERCTL_PRINTER_ERR_NOT_CONNECTED,
              "an SD control on an unconnected session reports NOT_CONNECTED");
    }
}

/* Counts this process's open descriptors via /proc/self/fd. */
static int count_open_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL) {
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/*
 * Milestone 9 resource-cleanup acceptance: every failure path must
 * close both the G-code file and the serial port. Each scenario runs a
 * full session — connect, failed print, disconnect — and the process's
 * descriptor count must end exactly where it started.
 */
static void test_failure_paths_release_all_descriptors(void)
{
    int before = count_open_fds();
    if (before < 0) {
        return; /* no /proc: nothing to measure */
    }

    /* Timeout mid-stream. */
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 150)) {
            return;
        }
        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG90\n", path, sizeof(path));
        mock_reply(&mock, "ok\n"); /* then silence */

        printerctl_print_summary_t summary;
        check(printerctl_printer_print_file(&session, path, NULL, NULL, NULL, NULL,
                                             &summary, &error) ==
                  PRINTERCTL_PRINTER_ERR_TIMEOUT,
              "the cleanup scenario fails with the expected timeout");

        printerctl_printer_disconnect(&session, &error);
        mock_close(&mock);
        remove(path);
    }

    /* Disconnect mid-stream (the printer vanishes). */
    {
        mock_printer_t mock;
        if (!mock_open(&mock, 500)) {
            return;
        }
        printerctl_printer_session_t session;
        printerctl_printer_error_t error;
        printerctl_printer_connect(&session, &mock.config, NULL, &error);

        char path[64];
        write_temp_gcode("G21\nG90\n", path, sizeof(path));
        mock_close(&mock);

        printerctl_print_summary_t summary;
        printerctl_printer_status_t status = printerctl_printer_print_file(
            &session, path, NULL, NULL, NULL, NULL, &summary, &error);
        check(status != PRINTERCTL_PRINTER_OK,
              "the vanished-printer scenario fails explicitly");
        check(summary.state == PRINTERCTL_PRINT_FAILED,
              "the vanished-printer summary reports the failed state");

        printerctl_printer_disconnect(&session, &error);
        remove(path);
    }

    int after = count_open_fds();
    check(after == before,
          "every failure path released its file and serial descriptors");
}

static void test_print_file_guards(void)
{
    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_print_summary_t summary;
    printerctl_config_t config;
    printerctl_config_set_defaults(&config);

    printerctl_printer_connect(&session, &config, NULL, &error); /* rejected: no port */

    check(printerctl_printer_print_file(&session, "/tmp/x.gcode", NULL, NULL, NULL, NULL, &summary, &error) ==
              PRINTERCTL_PRINTER_ERR_NOT_CONNECTED,
          "printing on an unconnected session reports NOT_CONNECTED");
    check(printerctl_printer_print_file(&session, NULL, NULL, NULL, NULL, NULL, &summary, &error) ==
              PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
          "printing with a NULL path is rejected");
    check(printerctl_printer_print_file(&session, "/tmp/x.gcode", NULL, NULL, NULL, NULL, NULL, &error) ==
              PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
          "printing with a NULL summary is rejected");
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
    test_print_file_completes_and_reports_progress();
    test_print_file_never_floods_the_serial_buffer();
    test_print_file_stops_on_error();
    test_print_file_stops_on_resend_and_timeout();
    test_print_file_blocks_g28_and_file_errors();
    test_print_file_waits_out_busy_and_handles_empty_file();
    test_print_file_pause_transmits_nothing_and_resume_continues();
    test_print_file_cancel_stops_after_current_acknowledgement();
    test_print_file_cancel_power_failure_stops_after_current_acknowledgement();
    test_print_file_cancel_power_failure_takes_priority_while_paused();
    test_print_file_cancel_before_first_command_and_while_paused();
    test_sd_print_controls_send_marlin_commands();
    test_failure_paths_release_all_descriptors();
    test_print_file_guards();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All printer checks passed\n");
    return EXIT_SUCCESS;
}
