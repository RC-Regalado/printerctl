#include "printerctl/printer.h"

#include "printerctl/gcode.h"

#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* How often a paused print re-polls its control callback. */
#define PRINTERCTL_PRINTER_PAUSE_POLL_MS 100

static void set_error(printerctl_printer_error_t *out_error,
                       printerctl_printer_status_t status,
                       const char *format, ...)
{
    out_error->status = status;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static void reset_session(printerctl_printer_session_t *session)
{
    memset(session, 0, sizeof(*session));
    session->port.fd = -1;
}

/* Best-effort logging: a failed record never affects the exchange. */
static void log_detail(printerctl_logger_t *logger,
                        printerctl_log_status_t (*helper)(printerctl_logger_t *, const char *),
                        const char *detail)
{
    if (logger != NULL) {
        (void)helper(logger, detail);
    }
}

printerctl_printer_status_t printerctl_printer_connect(
    printerctl_printer_session_t *session,
    const printerctl_config_t *config,
    printerctl_logger_t *logger,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_PRINTER_OK;

    if (session == NULL) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "session must not be NULL");
        return out_error->status;
    }

    reset_session(session);

    if (config == NULL) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "config must not be NULL");
        return out_error->status;
    }

    /* Last line of defense for architecture.md §8 rule 7: a dry-run
     * configuration must never reach a serial connection. */
    if (config->dry_run) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "a dry-run session must never open a serial connection");
        return out_error->status;
    }

    if (config->port[0] == '\0') {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_NO_PORT,
                   "no serial port configured; pass --port or set port in the config file");
        return out_error->status;
    }

    printerctl_serial_error_t serial_error;
    printerctl_serial_status_t serial_status = printerctl_serial_open(
        &session->port, config->port, config->baud, config->timeout_ms, &serial_error);
    if (serial_status != PRINTERCTL_SERIAL_OK) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_SERIAL, "%s", serial_error.message);
        return out_error->status;
    }

    session->logger = logger;
    session->connected = true;

    char detail[96];
    snprintf(detail, sizeof(detail), "state=connected baud=%d timeout_ms=%d",
             config->baud, config->timeout_ms);
    log_detail(session->logger, printerctl_log_state_transition, detail);

    return PRINTERCTL_PRINTER_OK;
}

printerctl_printer_status_t printerctl_printer_execute(
    printerctl_printer_session_t *session,
    const char *command,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_PRINTER_OK;

    if (session == NULL || command == NULL) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "session and command must not be NULL");
        return out_error->status;
    }

    if (!session->connected) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_NOT_CONNECTED,
                   "session is not connected");
        return out_error->status;
    }

    printerctl_serial_error_t serial_error;
    printerctl_serial_status_t serial_status =
        printerctl_serial_write_line(&session->port, command, &serial_error);
    if (serial_status != PRINTERCTL_SERIAL_OK) {
        printerctl_printer_status_t status;
        switch (serial_status) {
        case PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT:
            status = PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
            break;
        case PRINTERCTL_SERIAL_ERR_TIMEOUT:
            status = PRINTERCTL_PRINTER_ERR_TIMEOUT;
            break;
        default:
            status = PRINTERCTL_PRINTER_ERR_SERIAL;
            break;
        }
        set_error(out_error, status, "%s", serial_error.message);
        return out_error->status;
    }

    log_detail(session->logger, printerctl_log_serial_tx, command);

    for (;;) {
        char line[PRINTERCTL_PRINTER_LINE_MAX];
        serial_status = printerctl_serial_read_line(&session->port, line, sizeof(line),
                                                     &serial_error);
        if (serial_status != PRINTERCTL_SERIAL_OK) {
            printerctl_printer_status_t status =
                serial_status == PRINTERCTL_SERIAL_ERR_TIMEOUT
                    ? PRINTERCTL_PRINTER_ERR_TIMEOUT
                    : PRINTERCTL_PRINTER_ERR_SERIAL;
            set_error(out_error, status, "%s", serial_error.message);
            log_detail(session->logger, printerctl_log_error,
                        status == PRINTERCTL_PRINTER_ERR_TIMEOUT
                            ? "response timeout"
                            : "serial failure while waiting for a response");
            return out_error->status;
        }

        log_detail(session->logger, printerctl_log_serial_rx, line);

        if (on_line != NULL) {
            on_line(line, context);
        }

        printerctl_marlin_response_t response;
        if (printerctl_marlin_classify(line, &response) != PRINTERCTL_MARLIN_OK) {
            set_error(out_error, PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE,
                       "internal error classifying a response line");
            return out_error->status;
        }

        if (!response.terminal) {
            continue;
        }

        switch (response.type) {
        case PRINTERCTL_MARLIN_RESPONSE_ACK:
            return PRINTERCTL_PRINTER_OK;
        case PRINTERCTL_MARLIN_RESPONSE_ERROR:
            set_error(out_error, PRINTERCTL_PRINTER_ERR_PRINTER_ERROR,
                       "printer reported an error: %s", response.payload);
            log_detail(session->logger, printerctl_log_error, "printer reported an error");
            return out_error->status;
        case PRINTERCTL_MARLIN_RESPONSE_RESEND:
        default:
            set_error(out_error, PRINTERCTL_PRINTER_ERR_RESEND,
                       "printer requested a resend, which is not supported");
            log_detail(session->logger, printerctl_log_error, "printer requested a resend");
            return out_error->status;
        }
    }
}

/* Shared shape of the three typed queries: probe every response line with a marlin parser. */

typedef struct {
    printerctl_marlin_temperature_report_t *report;
    bool found;
} temperature_probe_t;

static void probe_temperature(const char *line, void *context)
{
    temperature_probe_t *probe = context;
    if (probe->found) {
        return;
    }

    /* The report may ride on the ok line ("ok T:...") or arrive as its
     * own informational line before the ok; classify first so the "ok "
     * prefix never reaches the report parser. */
    printerctl_marlin_response_t response;
    if (printerctl_marlin_classify(line, &response) != PRINTERCTL_MARLIN_OK) {
        return;
    }

    const char *text = response.type == PRINTERCTL_MARLIN_RESPONSE_ACK
                           ? response.payload
                           : line;

    printerctl_marlin_temperature_report_t report;
    if (printerctl_marlin_parse_temperature(text, &report) == PRINTERCTL_MARLIN_OK) {
        *probe->report = report;
        probe->found = true;
    }
}

printerctl_printer_status_t printerctl_printer_query_temperature(
    printerctl_printer_session_t *session,
    printerctl_marlin_temperature_report_t *out_report,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    if (out_report == NULL) {
        memset(out_error, 0, sizeof(*out_error));
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "out_report must not be NULL");
        return out_error->status;
    }
    memset(out_report, 0, sizeof(*out_report));

    temperature_probe_t probe = {.report = out_report, .found = false};
    printerctl_printer_status_t status =
        printerctl_printer_execute(session, "M105", probe_temperature, &probe, out_error);
    if (status != PRINTERCTL_PRINTER_OK) {
        return status;
    }

    if (!probe.found) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE,
                   "printer acknowledged M105 but sent no temperature report");
        return out_error->status;
    }

    return PRINTERCTL_PRINTER_OK;
}

typedef struct {
    printerctl_marlin_position_report_t *report;
    bool found;
} position_probe_t;

static void probe_position(const char *line, void *context)
{
    position_probe_t *probe = context;
    if (probe->found) {
        return;
    }

    printerctl_marlin_position_report_t report;
    if (printerctl_marlin_parse_position(line, &report) == PRINTERCTL_MARLIN_OK) {
        *probe->report = report;
        probe->found = true;
    }
}

printerctl_printer_status_t printerctl_printer_query_position(
    printerctl_printer_session_t *session,
    printerctl_marlin_position_report_t *out_report,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    if (out_report == NULL) {
        memset(out_error, 0, sizeof(*out_error));
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "out_report must not be NULL");
        return out_error->status;
    }
    memset(out_report, 0, sizeof(*out_report));

    position_probe_t probe = {.report = out_report, .found = false};
    printerctl_printer_status_t status =
        printerctl_printer_execute(session, "M114", probe_position, &probe, out_error);
    if (status != PRINTERCTL_PRINTER_OK) {
        return status;
    }

    if (!probe.found) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE,
                   "printer acknowledged M114 but sent no position report");
        return out_error->status;
    }

    return PRINTERCTL_PRINTER_OK;
}

typedef struct {
    printerctl_marlin_firmware_info_t *info;
    bool found;
} info_probe_t;

static void probe_info(const char *line, void *context)
{
    info_probe_t *probe = context;
    if (probe->found) {
        return;
    }

    printerctl_marlin_firmware_info_t info;
    if (printerctl_marlin_parse_firmware_info(line, &info) == PRINTERCTL_MARLIN_OK) {
        *probe->info = info;
        probe->found = true;
    }
}

printerctl_printer_status_t printerctl_printer_query_info(
    printerctl_printer_session_t *session,
    printerctl_marlin_firmware_info_t *out_info,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    if (out_info == NULL) {
        memset(out_error, 0, sizeof(*out_error));
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "out_info must not be NULL");
        return out_error->status;
    }
    memset(out_info, 0, sizeof(*out_info));

    info_probe_t probe = {.info = out_info, .found = false};
    printerctl_printer_status_t status =
        printerctl_printer_execute(session, "M115", probe_info, &probe, out_error);
    if (status != PRINTERCTL_PRINTER_OK) {
        return status;
    }

    if (!probe.found) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE,
                   "printer acknowledged M115 but sent no firmware information");
        return out_error->status;
    }

    return PRINTERCTL_PRINTER_OK;
}

printerctl_printer_status_t printerctl_printer_send(
    printerctl_printer_session_t *session,
    const char *command,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_PRINTER_OK;

    if (command == NULL) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "command must not be NULL");
        return out_error->status;
    }

    /* Safety gate before anything touches the wire: the same G28
     * detection the G-code reader applies to files (Milestone 2's
     * design already required this check to live in the execution path,
     * not be inferred from logs). */
    if (printerctl_gcode_command_is_unsafe_g28(command)) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND,
                   "refusing to send G28: homing is disabled because endstop sensors "
                   "are not available (see the safety policy in Agents.md)");
        if (session != NULL) {
            log_detail(session->logger, printerctl_log_error, "send rejected: unsafe G28");
        }
        return out_error->status;
    }

    return printerctl_printer_execute(session, command, on_line, context, out_error);
}

/*
 * Shared shape of the three SD-print controls: one explicit operator
 * command with send-and-wait-for-terminal semantics, plus an INFO
 * state_transition record naming the requested control on success.
 */
static printerctl_printer_status_t request_sd_control(
    printerctl_printer_session_t *session,
    const char *command,
    const char *state_detail,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error)
{
    printerctl_printer_status_t status =
        printerctl_printer_execute(session, command, on_line, context, out_error);
    if (status == PRINTERCTL_PRINTER_OK) {
        log_detail(session->logger, printerctl_log_state_transition, state_detail);
    }
    return status;
}

printerctl_printer_status_t printerctl_printer_pause_sd_print(
    printerctl_printer_session_t *session,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error)
{
    return request_sd_control(session, "M25", "state=sd_pause_requested",
                               on_line, context, out_error);
}

printerctl_printer_status_t printerctl_printer_resume_sd_print(
    printerctl_printer_session_t *session,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error)
{
    return request_sd_control(session, "M24", "state=sd_resume_requested",
                               on_line, context, out_error);
}

printerctl_printer_status_t printerctl_printer_cancel_sd_print(
    printerctl_printer_session_t *session,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error)
{
    return request_sd_control(session, "M524", "state=sd_cancel_requested",
                               on_line, context, out_error);
}

/* Maps a reader failure to the session-level status the CLI renders. */
static printerctl_printer_status_t gcode_failure_status(printerctl_gcode_status_t status)
{
    return status == PRINTERCTL_GCODE_ERR_UNSAFE_G28
               ? PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND
               : PRINTERCTL_PRINTER_ERR_GCODE;
}

/* One INFO state_transition record closing a print session, with the summary attached. */
static void log_print_end(printerctl_logger_t *logger,
                           const printerctl_print_summary_t *summary,
                           const char *state_name)
{
    char detail[160];
    snprintf(detail, sizeof(detail),
             "state=%s commands_sent=%llu commands_acknowledged=%llu "
             "last_acknowledged_line=%llu percent=%u",
             state_name,
             (unsigned long long)summary->commands_sent,
             (unsigned long long)summary->commands_acknowledged,
             (unsigned long long)summary->last_acknowledged_line,
             summary->progress_percent);
    log_detail(logger, printerctl_log_state_transition, detail);
}

printerctl_printer_status_t printerctl_printer_print_file(
    printerctl_printer_session_t *session,
    const char *path,
    printerctl_printer_progress_fn on_progress,
    void *progress_context,
    printerctl_printer_control_fn on_control,
    void *control_context,
    printerctl_print_summary_t *out_summary,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_PRINTER_OK;

    if (out_summary == NULL) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "out_summary must not be NULL");
        return out_error->status;
    }

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->state = PRINTERCTL_PRINT_FAILED;

    if (session == NULL || path == NULL) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "session and path must not be NULL");
        out_summary->status = out_error->status;
        return out_error->status;
    }

    if (!session->connected) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_NOT_CONNECTED,
                   "session is not connected");
        out_summary->status = out_error->status;
        return out_error->status;
    }

    printerctl_gcode_reader_t reader;
    printerctl_gcode_error_t gcode_error;
    printerctl_gcode_status_t gcode_status =
        printerctl_gcode_reader_open(&reader, path, false, &gcode_error);
    if (gcode_status != PRINTERCTL_GCODE_OK) {
        printerctl_printer_status_t status = gcode_failure_status(gcode_status);
        set_error(out_error, status, "%s", gcode_error.message);
        out_summary->status = status;
        log_detail(session->logger, printerctl_log_error, "print failed: cannot open G-code file");
        log_print_end(session->logger, out_summary, "print_failed");
        return out_error->status;
    }

    out_summary->file_size = reader.size;
    log_detail(session->logger, printerctl_log_state_transition, "state=printing");

    unsigned last_logged_percent = (unsigned)-1;
    printerctl_printer_status_t status = PRINTERCTL_PRINTER_OK;

    for (;;) {
        /* Session control sits between acknowledged commands: pause and
         * cancel are honored here, never mid-exchange, so the command
         * in flight always completes its acknowledgement first. */
        if (on_control != NULL) {
            printerctl_print_control_t control = on_control(control_context);
            if (control == PRINTERCTL_PRINT_CONTROL_PAUSE) {
                log_detail(session->logger, printerctl_log_state_transition,
                            "state=print_paused");
                do {
                    (void)poll(NULL, 0, PRINTERCTL_PRINTER_PAUSE_POLL_MS);
                    control = on_control(control_context);
                } while (control == PRINTERCTL_PRINT_CONTROL_PAUSE);
                if (control == PRINTERCTL_PRINT_CONTROL_CONTINUE) {
                    log_detail(session->logger, printerctl_log_state_transition,
                                "state=printing");
                }
            }
            if (control == PRINTERCTL_PRINT_CONTROL_CANCEL) {
                set_error(out_error, PRINTERCTL_PRINTER_CANCELLED,
                           "print cancelled by the operator");
                status = PRINTERCTL_PRINTER_CANCELLED;
                break;
            }
        }

        /* The next command is not even read from the file until the
         * previous one was acknowledged: strictly one in flight. */
        printerctl_gcode_result_t result;
        gcode_status = printerctl_gcode_reader_next(&reader, &result, &gcode_error);
        if (gcode_status == PRINTERCTL_GCODE_EOF) {
            out_summary->state = PRINTERCTL_PRINT_COMPLETED;
            out_summary->status = PRINTERCTL_PRINTER_OK;
            out_summary->bytes_consumed = reader.bytes_consumed;
            out_summary->progress_percent = 100;
            break;
        }
        if (gcode_status != PRINTERCTL_GCODE_OK) {
            status = gcode_failure_status(gcode_status);
            if (gcode_error.line_number != 0) {
                set_error(out_error, status, "%s (line %llu)", gcode_error.message,
                           (unsigned long long)gcode_error.line_number);
            } else {
                set_error(out_error, status, "%s", gcode_error.message);
            }
            log_detail(session->logger, printerctl_log_error,
                        status == PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND
                            ? "print stopped: unsafe G28 in the file"
                            : "print stopped: G-code file error");
            break;
        }

        printerctl_printer_status_t exchange_status =
            printerctl_printer_execute(session, result.command, NULL, NULL, out_error);
        out_summary->commands_sent++;
        if (exchange_status != PRINTERCTL_PRINTER_OK) {
            status = exchange_status;
            break;
        }

        out_summary->commands_acknowledged++;
        out_summary->last_acknowledged_line = result.line_number;
        out_summary->bytes_consumed = result.bytes_consumed;
        out_summary->progress_percent = result.progress_percent;

        if (session->logger != NULL) {
            char detail[128];
            snprintf(detail, sizeof(detail), "line=%llu command=%llu percent=%u",
                     (unsigned long long)result.line_number,
                     (unsigned long long)result.command_number,
                     result.progress_percent);
            (void)printerctl_log_record(session->logger, PRINTERCTL_LOG_DEBUG,
                                         PRINTERCTL_LOG_EVENT_PROGRESS, detail);
            if (result.progress_percent != last_logged_percent) {
                (void)printerctl_log_progress(session->logger, detail);
                last_logged_percent = result.progress_percent;
            }
        }

        if (on_progress != NULL) {
            on_progress(&result, progress_context);
        }
    }

    printerctl_gcode_error_t close_error;
    (void)printerctl_gcode_reader_close(&reader, &close_error);

    if (status == PRINTERCTL_PRINTER_CANCELLED) {
        out_summary->state = PRINTERCTL_PRINT_CANCELLED;
        out_summary->status = status;
        log_print_end(session->logger, out_summary, "print_cancelled");
        return status;
    }

    if (status != PRINTERCTL_PRINTER_OK) {
        out_summary->state = PRINTERCTL_PRINT_FAILED;
        out_summary->status = status;
        log_print_end(session->logger, out_summary, "print_failed");
        return status;
    }

    log_print_end(session->logger, out_summary, "print_completed");
    return PRINTERCTL_PRINTER_OK;
}

printerctl_printer_status_t printerctl_printer_disconnect(
    printerctl_printer_session_t *session,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_PRINTER_OK;

    if (session == NULL) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
                   "session must not be NULL");
        return out_error->status;
    }

    if (!session->connected) {
        return PRINTERCTL_PRINTER_OK;
    }

    printerctl_logger_t *logger = session->logger;

    printerctl_serial_error_t serial_error;
    printerctl_serial_status_t serial_status =
        printerctl_serial_close(&session->port, &serial_error);

    reset_session(session);

    log_detail(logger, printerctl_log_state_transition, "state=disconnected");

    if (serial_status != PRINTERCTL_SERIAL_OK) {
        set_error(out_error, PRINTERCTL_PRINTER_ERR_SERIAL, "%s", serial_error.message);
        return out_error->status;
    }

    return PRINTERCTL_PRINTER_OK;
}
