/*
 * Milestone 12 tests for the print engine core, over the same
 * pseudo-terminal mock-Marlin pattern test_printer.c uses. Deliberately
 * does NOT include printerctl/cli.h or anything from src/cli: this file
 * is the proof that the engine runs a full print lifecycle -- connect,
 * stream, pause/resume, cancel, an interrupted-session record, the
 * active-print marker -- without any CLI argument parsing or terminal
 * output, per docs/design/milestone-12-print-engine.md §7.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "printerctl/core/print_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
}

/* One scripted mock printer: a pty pair plus the config that reaches it.
 * Mirrors tests/test_printer.c's mock_printer_t. */
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

static void mock_reply(mock_printer_t *mock, const char *text)
{
    size_t length = strlen(text);
    if (write(mock->master, text, length) != (ssize_t)length) {
        fprintf(stderr, "FAIL: cannot script mock reply\n");
        g_failures++;
    }
}

static void mock_close(mock_printer_t *mock)
{
    close(mock->master);
}

/* Writes `content` to a fresh temp G-code file and returns its path via `path`. */
static void write_temp_gcode(const char *content, char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/printerctl_print_engine_gcode_XXXXXX");
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

/*
 * Captures every event the engine emits (by type) and the last payload
 * of each shape, and doubles as the "signal handler" side of pause/
 * cancel/power-failure tests: setting `*_after_progress` or
 * `*_after_pause` makes the callback call the matching
 * printerctl_print_engine_request_*() the moment the triggering event
 * is observed -- exactly like a real signal handler would, just called
 * synchronously from inside the same blocking printerctl_print_engine_run()
 * instead of asynchronously from a POSIX signal.
 */
typedef struct {
    printerctl_print_engine_t *engine;
    int counts[PRINTERCTL_PRINT_EVENT_RECORD_FAILED + 1];
    printerctl_print_summary_t last_summary;
    char last_record_path[PRINTERCTL_RECORD_PATH_MAX];
    char last_message[256];

    int cancel_after_progress;         /* 0 disables (progress counts start at 1) */
    int pause_after_progress;          /* 0 disables */
    bool resume_after_pause;
    bool cancel_power_failure_after_pause;
} event_probe_t;

static void record_event(const printerctl_print_event_t *event, void *context)
{
    event_probe_t *probe = context;
    probe->counts[event->type]++;

    if (event->type == PRINTERCTL_PRINT_EVENT_PROGRESS) {
        int seen = probe->counts[PRINTERCTL_PRINT_EVENT_PROGRESS];
        if (probe->pause_after_progress != 0 && seen == probe->pause_after_progress) {
            printerctl_print_engine_request_pause(probe->engine);
        }
        if (probe->cancel_after_progress != 0 && seen == probe->cancel_after_progress) {
            printerctl_print_engine_request_cancel(probe->engine);
        }
    } else if (event->type == PRINTERCTL_PRINT_EVENT_PAUSED) {
        if (probe->cancel_power_failure_after_pause) {
            printerctl_print_engine_request_cancel_power_failure(probe->engine);
        } else if (probe->resume_after_pause) {
            printerctl_print_engine_request_resume(probe->engine);
        }
    }

    if (event->summary != NULL) {
        probe->last_summary = *event->summary;
    }
    if (event->path != NULL) {
        snprintf(probe->last_record_path, sizeof(probe->last_record_path), "%s", event->path);
    }
    if (event->message != NULL) {
        snprintf(probe->last_message, sizeof(probe->last_message), "%s", event->message);
    }
}

/* Reads the whole file at `path` into `out` (best-effort, NUL-terminated). */
static void read_whole_file(const char *path, char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }
    size_t read_count = fread(out, 1, out_size - 1, file);
    out[read_count] = '\0';
    fclose(file);
}

static void test_run_completes_without_intervention(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    event_probe_t probe = {0};
    printerctl_print_engine_t engine;
    printerctl_printer_error_t error;
    probe.engine = &engine;

    check(printerctl_print_engine_connect(&engine, &mock.config, NULL, false, record_event, &probe,
                                           &error) == PRINTERCTL_PRINTER_OK,
          "the engine connects");

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\nok\nok\n");

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status =
        printerctl_print_engine_run(&engine, path, &summary, &error);

    check(status == PRINTERCTL_PRINTER_OK, "an uninterrupted print completes");
    check(summary.state == PRINTERCTL_PRINT_COMPLETED, "the summary reports completion");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_STARTED] == 1, "STARTED fires exactly once");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_PROGRESS] == 3,
          "PROGRESS fires once per acknowledged command");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_COMPLETED] == 1, "COMPLETED fires exactly once");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_CANCELLED] == 0, "no CANCELLED on a clean completion");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_RECORD_WRITTEN] == 0,
          "a completed print leaves no interrupted-session record");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_MARKER_WARNING] == 0,
          "the active-print marker publishes cleanly in this fixture");

    pid_t marker_pid = 0;
    printerctl_record_error_t marker_error;
    check(printerctl_record_read_active_pid(engine.state_dir, &marker_pid, &marker_error) ==
              PRINTERCTL_RECORD_ERR_NOT_FOUND,
          "the active-print marker is removed once the print finishes");

    printerctl_print_engine_disconnect(&engine, &error);
    mock_close(&mock);
    remove(path);
}

static void test_run_cancel_writes_record(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    event_probe_t probe = {0};
    printerctl_print_engine_t engine;
    printerctl_printer_error_t error;
    probe.engine = &engine;
    probe.cancel_after_progress = 1;

    printerctl_print_engine_connect(&engine, &mock.config, NULL, false, record_event, &probe,
                                     &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\n");

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status =
        printerctl_print_engine_run(&engine, path, &summary, &error);

    check(status == PRINTERCTL_PRINTER_CANCELLED, "a mid-stream cancel returns the cancelled status");
    check(summary.state == PRINTERCTL_PRINT_CANCELLED, "the summary reports the cancelled state");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_CANCELLED] == 1, "CANCELLED fires exactly once");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_RECORD_WRITTEN] == 1,
          "an interrupted session writes a record");
    check(probe.last_record_path[0] != '\0', "the RECORD_WRITTEN event names the record's path");

    struct stat record_stat;
    check(stat(probe.last_record_path, &record_stat) == 0, "the record file actually exists on disk");

    char content[1024];
    read_whole_file(probe.last_record_path, content, sizeof(content));
    check(strstr(content, "state=cancelled\n") != NULL, "the record's state is 'cancelled'");

    pid_t marker_pid = 0;
    printerctl_record_error_t marker_error;
    check(printerctl_record_read_active_pid(engine.state_dir, &marker_pid, &marker_error) ==
              PRINTERCTL_RECORD_ERR_NOT_FOUND,
          "the active-print marker is removed after a cancel");

    printerctl_print_engine_disconnect(&engine, &error);
    mock_close(&mock);
    remove(path);
    remove(probe.last_record_path);
}

static void test_cancel_power_failure_takes_priority_over_pause(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    event_probe_t probe = {0};
    printerctl_print_engine_t engine;
    printerctl_printer_error_t error;
    probe.engine = &engine;
    probe.pause_after_progress = 1;
    probe.cancel_power_failure_after_pause = true;

    printerctl_print_engine_connect(&engine, &mock.config, NULL, false, record_event, &probe,
                                     &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\n");

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status =
        printerctl_print_engine_run(&engine, path, &summary, &error);

    check(status == PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE,
          "a power-failure cancel wins over an unresolved pause");
    check(summary.state == PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE,
          "the summary reports the power-failure cancellation");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_PAUSED] == 1, "PAUSED still fires once before the cancel");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_RESUMED] == 0, "a power-failure cancel never resumes first");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_CANCELLING_POWER_FAILURE] == 1,
          "the power-failure notice fires exactly once (edge-triggered)");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_CANCELLED] == 1, "CANCELLED fires exactly once");

    char content[1024];
    read_whole_file(probe.last_record_path, content, sizeof(content));
    check(strstr(content, "state=cancelled_power_failure\n") != NULL,
          "the record's state is 'cancelled_power_failure'");
    check(strstr(content, "cause=apcupsd_low_battery\n") != NULL,
          "the record's cause names the power event, not a printer status");

    printerctl_print_engine_disconnect(&engine, &error);
    mock_close(&mock);
    remove(path);
    remove(probe.last_record_path);
}

static void test_pause_then_resume_completes(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    event_probe_t probe = {0};
    printerctl_print_engine_t engine;
    printerctl_printer_error_t error;
    probe.engine = &engine;
    probe.pause_after_progress = 1;
    probe.resume_after_pause = true;

    printerctl_print_engine_connect(&engine, &mock.config, NULL, false, record_event, &probe,
                                     &error);

    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));
    mock_reply(&mock, "ok\nok\nok\n");

    printerctl_print_summary_t summary;
    printerctl_printer_status_t status =
        printerctl_print_engine_run(&engine, path, &summary, &error);

    check(status == PRINTERCTL_PRINTER_OK, "a paused-then-resumed print still completes");
    check(summary.state == PRINTERCTL_PRINT_COMPLETED && summary.commands_acknowledged == 3,
          "resume continued streaming to the end");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_PAUSED] == 1, "PAUSED fires exactly once");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_RESUMED] == 1, "RESUMED fires exactly once");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_RECORD_WRITTEN] == 0,
          "a print that completes after a pause leaves no record");

    printerctl_print_engine_disconnect(&engine, &error);
    mock_close(&mock);
    remove(path);
}

static void test_connect_failure_leaves_no_marker(void)
{
    event_probe_t probe = {0};
    printerctl_print_engine_t engine;
    printerctl_printer_error_t error;
    probe.engine = &engine;

    printerctl_config_t config;
    printerctl_config_set_defaults(&config);
    config.port[0] = '\0'; /* no port configured -> ERR_NO_PORT before any state-dir/marker work */

    printerctl_printer_status_t status = printerctl_print_engine_connect(
        &engine, &config, NULL, false, record_event, &probe, &error);

    check(status == PRINTERCTL_PRINTER_ERR_NO_PORT, "connect fails fast without a configured port");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_MARKER_WARNING] == 0,
          "a connect failure never reaches the marker-publish step");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_STARTED] == 0, "run() was never called, so STARTED never fires");
}

static void test_marker_warning_when_state_dir_unusable(void)
{
    mock_printer_t mock;
    if (!mock_open(&mock, 1000)) {
        return;
    }

    char *saved_xdg = getenv("XDG_STATE_HOME");
    char *saved_xdg_copy = saved_xdg != NULL ? strdup(saved_xdg) : NULL;
    char *saved_home = getenv("HOME");
    char *saved_home_copy = saved_home != NULL ? strdup(saved_home) : NULL;
    unsetenv("XDG_STATE_HOME");
    unsetenv("HOME");

    event_probe_t probe = {0};
    printerctl_print_engine_t engine;
    printerctl_printer_error_t error;
    probe.engine = &engine;

    printerctl_printer_status_t status = printerctl_print_engine_connect(
        &engine, &mock.config, NULL, false, record_event, &probe, &error);

    check(status == PRINTERCTL_PRINTER_OK,
          "an unusable state directory still lets the print connect");
    check(probe.counts[PRINTERCTL_PRINT_EVENT_MARKER_WARNING] == 1,
          "a marker-publish failure is surfaced as a warning event, not a connect failure");
    check(probe.last_message[0] != '\0', "the marker warning carries the underlying reason");

    if (saved_xdg_copy != NULL) {
        setenv("XDG_STATE_HOME", saved_xdg_copy, 1);
        free(saved_xdg_copy);
    }
    if (saved_home_copy != NULL) {
        setenv("HOME", saved_home_copy, 1);
        free(saved_home_copy);
    }

    printerctl_print_engine_disconnect(&engine, &error);
    mock_close(&mock);
}

int main(void)
{
    char state_dir_template[] = "/tmp/printerctl_print_engine_state_XXXXXX";
    if (mkdtemp(state_dir_template) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp() failed for the state-dir fixture\n");
        return EXIT_FAILURE;
    }
    setenv("XDG_STATE_HOME", state_dir_template, 1);

    test_run_completes_without_intervention();
    test_run_cancel_writes_record();
    test_cancel_power_failure_takes_priority_over_pause();
    test_pause_then_resume_completes();
    test_connect_failure_leaves_no_marker();
    test_marker_warning_when_state_dir_unusable();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All print engine checks passed\n");
    return EXIT_SUCCESS;
}
