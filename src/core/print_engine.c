#include "printerctl/core/print_engine.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void emit(printerctl_print_engine_t *engine, const printerctl_print_event_t *event)
{
    if (engine->on_event != NULL) {
        engine->on_event(event, engine->event_context);
    }
}

static void emit_simple(printerctl_print_engine_t *engine, printerctl_print_event_type_t type)
{
    printerctl_print_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    emit(engine, &event);
}

static printerctl_print_control_t tx_gate_close_if_requested(printerctl_print_engine_t *engine)
{
    if (engine == NULL || !engine->tx_gate_ready) {
        return PRINTERCTL_PRINT_CONTROL_CONTINUE;
    }

    pthread_mutex_lock(&engine->tx_gate);
    if (engine->cancel_power_failure_requested) {
        engine->tx_gate_closed = true;
        engine->tx_gate_power_failure = true;
    } else if (engine->cancel_requested && !engine->tx_gate_closed) {
        engine->tx_gate_closed = true;
        engine->tx_gate_power_failure = false;
    }

    printerctl_print_control_t control = PRINTERCTL_PRINT_CONTROL_CONTINUE;
    if (engine->tx_gate_closed) {
        control = engine->tx_gate_power_failure
            ? PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE
            : PRINTERCTL_PRINT_CONTROL_CANCEL;
    }
    pthread_mutex_unlock(&engine->tx_gate);
    return control;
}

printerctl_print_control_t printerctl_print_engine_commit_cancellation(
    printerctl_print_engine_t *engine)
{
    return tx_gate_close_if_requested(engine);
}

static printerctl_print_control_t tx_gate_begin(void *context)
{
    printerctl_print_engine_t *engine = context;
    pthread_mutex_lock(&engine->tx_gate);
    if (engine->cancel_power_failure_requested) {
        engine->tx_gate_closed = true;
        engine->tx_gate_power_failure = true;
    } else if (engine->cancel_requested && !engine->tx_gate_closed) {
        engine->tx_gate_closed = true;
        engine->tx_gate_power_failure = false;
    }
    if (engine->tx_gate_closed) {
        printerctl_print_control_t control = engine->tx_gate_power_failure
            ? PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE
            : PRINTERCTL_PRINT_CONTROL_CANCEL;
        pthread_mutex_unlock(&engine->tx_gate);
        return control;
    }
    return PRINTERCTL_PRINT_CONTROL_CONTINUE;
}

static void tx_gate_end(void *context)
{
    printerctl_print_engine_t *engine = context;
    pthread_mutex_unlock(&engine->tx_gate);
}

static void on_ack_wait(void *context)
{
    printerctl_print_engine_t *engine = context;
    if (engine->on_control_poll != NULL) {
        engine->on_control_poll(engine, engine->control_poll_context);
    }
    (void)tx_gate_close_if_requested(engine);
}

/* Mirrors printer.h's printerctl_print_control_t vocabulary; used only to
 * build the interrupted-session record's `cause` field. */
static const char *printer_status_name(printerctl_printer_status_t status)
{
    switch (status) {
    case PRINTERCTL_PRINTER_OK: return "ok";
    case PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT: return "invalid_argument";
    case PRINTERCTL_PRINTER_ERR_NOT_CONNECTED: return "not_connected";
    case PRINTERCTL_PRINTER_ERR_NO_PORT: return "no_port";
    case PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND: return "unsafe_command";
    case PRINTERCTL_PRINTER_ERR_SERIAL: return "serial";
    case PRINTERCTL_PRINTER_ERR_TIMEOUT: return "timeout";
    case PRINTERCTL_PRINTER_ERR_PRINTER_ERROR: return "printer_error";
    case PRINTERCTL_PRINTER_ERR_RESEND: return "resend";
    case PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE: return "unexpected_response";
    case PRINTERCTL_PRINTER_ERR_GCODE: return "gcode";
    case PRINTERCTL_PRINTER_CANCELLED: return "cancelled";
    case PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE: return "cancelled_power_failure";
    }
    return "unknown";
}

printerctl_printer_status_t printerctl_print_engine_connect(
    printerctl_print_engine_t *engine,
    const printerctl_config_t *config,
    printerctl_logger_t *logger,
    bool logger_ready,
    printerctl_print_engine_event_fn on_event,
    void *event_context,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (engine == NULL) {
        out_error->status = PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
        snprintf(out_error->message, sizeof(out_error->message), "engine must not be NULL");
        return out_error->status;
    }

    memset(engine, 0, sizeof(*engine));
    engine->logger = logger;
    engine->logger_ready = logger_ready;
    engine->on_event = on_event;
    engine->event_context = event_context;

    if (pthread_mutex_init(&engine->tx_gate, NULL) != 0) {
        out_error->status = PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
        snprintf(out_error->message, sizeof(out_error->message), "cannot initialize TX gate");
        return out_error->status;
    }
    engine->tx_gate_ready = true;

    printerctl_printer_status_t status =
        printerctl_printer_connect(&engine->printer, config, logger_ready ? logger : NULL, out_error);
    if (status != PRINTERCTL_PRINTER_OK) {
        pthread_mutex_destroy(&engine->tx_gate);
        engine->tx_gate_ready = false;
        return status;
    }

    if (config != NULL) {
        snprintf(engine->serial_device, sizeof(engine->serial_device), "%s", config->port);
    }

    /* Best-effort: an external low-battery notifier will not find this
     * print without the marker, but that is not a reason to refuse the
     * print itself -- same trade-off run_print_command() made. */
    printerctl_record_error_t marker_error;
    if (printerctl_record_resolve_state_dir(engine->state_dir, sizeof(engine->state_dir),
                                             &marker_error) == PRINTERCTL_RECORD_OK) {
        engine->state_dir_resolved = true;
        if (printerctl_record_write_active_pid(engine->state_dir, &marker_error) ==
            PRINTERCTL_RECORD_OK) {
            engine->marker_active = true;
        }
    }

    if (!engine->marker_active) {
        printerctl_print_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = PRINTERCTL_PRINT_EVENT_MARKER_WARNING;
        event.message = marker_error.message;
        emit(engine, &event);
    }

    return PRINTERCTL_PRINTER_OK;
}

/* Forwards every acknowledged-command callback as a PROGRESS event,
 * undeduplicated -- see the event enum's doc comment for why. */
static void on_progress(const printerctl_gcode_result_t *result, void *context)
{
    printerctl_print_engine_t *engine = context;
    printerctl_print_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = PRINTERCTL_PRINT_EVENT_PROGRESS;
    event.progress = result;
    emit(engine, &event);
}

/* Same priority order run_print_command()'s print_control_poll() used to
 * enforce: an unresolved power-failure cancellation wins over cancel,
 * which wins over an unresolved pause. */
static printerctl_print_control_t on_control(void *context)
{
    printerctl_print_engine_t *engine = context;

    if (engine->on_control_poll != NULL) {
        engine->on_control_poll(engine, engine->control_poll_context);
    }

    printerctl_print_control_t cancel_control = tx_gate_close_if_requested(engine);
    if (cancel_control == PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE) {
        if (!engine->cancel_power_failure_notified) {
            engine->cancel_power_failure_notified = true;
            emit_simple(engine, PRINTERCTL_PRINT_EVENT_CANCELLING_POWER_FAILURE);
        }
        return PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE;
    }

    if (cancel_control == PRINTERCTL_PRINT_CONTROL_CANCEL) {
        return PRINTERCTL_PRINT_CONTROL_CANCEL;
    }

    /* A resume answers a pause; both flags set means the pair resolved.
     * A stray resume with nothing paused must not neutralize a later
     * pause, so it is dropped here. */
    if (engine->resume_requested) {
        engine->resume_requested = 0;
        engine->pause_requested = 0;
    }

    if (engine->pause_requested) {
        if (!engine->paused) {
            engine->paused = true;
            emit_simple(engine, PRINTERCTL_PRINT_EVENT_PAUSED);
        }
        return PRINTERCTL_PRINT_CONTROL_PAUSE;
    }

    if (engine->paused) {
        engine->paused = false;
        emit_simple(engine, PRINTERCTL_PRINT_EVENT_RESUMED);
    }

    return PRINTERCTL_PRINT_CONTROL_CONTINUE;
}

/*
 * Moved from run_print_command()'s write_session_record(): persists an
 * interrupted-session record and emits RECORD_WRITTEN/RECORD_FAILED in
 * place of the fprintf()/structured-log pair the CLI used to own.
 */
static void write_session_record(printerctl_print_engine_t *engine, const char *path,
                                  const printerctl_print_summary_t *summary)
{
    printerctl_session_record_t record;
    memset(&record, 0, sizeof(record));
    if (summary->state == PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE) {
        snprintf(record.state, sizeof(record.state), "cancelled_power_failure");
        snprintf(record.cause, sizeof(record.cause), "apcupsd_low_battery");
    } else if (summary->state == PRINTERCTL_PRINT_CANCELLED) {
        snprintf(record.state, sizeof(record.state), "cancelled");
        snprintf(record.cause, sizeof(record.cause), "%s", printer_status_name(summary->status));
    } else {
        snprintf(record.state, sizeof(record.state), "failed");
        snprintf(record.cause, sizeof(record.cause), "%s", printer_status_name(summary->status));
    }
    snprintf(record.file, sizeof(record.file), "%s", path);
    snprintf(record.serial_device, sizeof(record.serial_device), "%s", engine->serial_device);
    snprintf(record.job_id, sizeof(record.job_id), "job-%ld", (long)getpid());
    snprintf(record.printer_state, sizeof(record.printer_state), "unknown");
    record.commands_sent = summary->commands_sent;
    record.commands_acknowledged = summary->commands_acknowledged;
    record.last_acknowledged_line = summary->last_acknowledged_line;
    record.bytes_consumed = summary->bytes_consumed;
    record.file_size = summary->file_size;
    record.progress_percent = summary->progress_percent;

    printerctl_record_error_t record_error;
    memset(&record_error, 0, sizeof(record_error));
    char record_path[PRINTERCTL_RECORD_PATH_MAX];
    printerctl_record_status_t status = PRINTERCTL_RECORD_ERR_NO_STATE_DIR;
    if (engine->state_dir_resolved) {
        status = printerctl_record_write_session(engine->state_dir, &record, record_path,
                                                   sizeof(record_path), &record_error);
    } else {
        snprintf(record_error.message, sizeof(record_error.message),
                 "no usable state directory");
    }

    printerctl_print_event_t event;
    memset(&event, 0, sizeof(event));
    if (status == PRINTERCTL_RECORD_OK) {
        event.type = PRINTERCTL_PRINT_EVENT_RECORD_WRITTEN;
        event.path = record_path;
        if (engine->logger_ready) {
            printerctl_log_state_transition(engine->logger, "state=session_recorded");
        }
    } else {
        event.type = PRINTERCTL_PRINT_EVENT_RECORD_FAILED;
        event.message = record_error.message;
        if (engine->logger_ready) {
            printerctl_log_record(engine->logger, PRINTERCTL_LOG_WARNING,
                                   PRINTERCTL_LOG_EVENT_WARNING, "session record not written");
        }
    }
    emit(engine, &event);
}

/*
 * Best-effort total-command pre-scan (PRINT-OBS-001): a second,
 * throw-away reader pass over the same source, opened with the same
 * allow_g28 policy the real streaming pass uses (printer.c's
 * print_file_controlled() opens with allow_g28 = false), so the count
 * reflects what will actually be streamed. Any non-EOF terminal status
 * (open failure, unsafe G28, malformed line, file changed mid-scan,
 * ...) yields 0/unknown rather than failing the print here -- the real
 * streaming pass independently re-validates the file and is the only
 * one authorized to fail the print.
 */
static uint64_t count_source_commands(const char *path)
{
    printerctl_gcode_reader_t reader;
    printerctl_gcode_error_t error;
    if (printerctl_gcode_reader_open(&reader, path, false, &error) != PRINTERCTL_GCODE_OK) {
        return 0;
    }

    printerctl_gcode_status_t status;
    do {
        printerctl_gcode_result_t result;
        status = printerctl_gcode_reader_next(&reader, &result, &error);
    } while (status == PRINTERCTL_GCODE_OK);

    uint64_t total = status == PRINTERCTL_GCODE_EOF ? reader.command_count : 0;
    printerctl_gcode_error_t close_error;
    printerctl_gcode_reader_close(&reader, &close_error);
    return total;
}

uint64_t printerctl_print_engine_commands_total(const printerctl_print_engine_t *engine)
{
    return engine != NULL ? engine->commands_total : 0;
}

printerctl_printer_status_t printerctl_print_engine_run(
    printerctl_print_engine_t *engine,
    const char *path,
    printerctl_print_summary_t *out_summary,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (engine == NULL || path == NULL || out_summary == NULL) {
        out_error->status = PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
        snprintf(out_error->message, sizeof(out_error->message),
                 "engine, path and out_summary must not be NULL");
        return out_error->status;
    }
    memset(out_summary, 0, sizeof(*out_summary));

    engine->commands_total = count_source_commands(path);

    printerctl_print_event_t started;
    memset(&started, 0, sizeof(started));
    started.type = PRINTERCTL_PRINT_EVENT_STARTED;
    started.path = path;
    emit(engine, &started);

    printerctl_printer_status_t status = printerctl_printer_print_file_controlled(
        &engine->printer, path, on_progress, engine, on_control, engine, tx_gate_begin,
        tx_gate_end, on_ack_wait, engine, out_summary, out_error);

    printerctl_print_event_t terminal;
    memset(&terminal, 0, sizeof(terminal));
    terminal.summary = out_summary;
    terminal.error = out_error;
    switch (out_summary->state) {
    case PRINTERCTL_PRINT_COMPLETED:
        terminal.type = PRINTERCTL_PRINT_EVENT_COMPLETED;
        break;
    case PRINTERCTL_PRINT_CANCELLED:
    case PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE:
        terminal.type = PRINTERCTL_PRINT_EVENT_CANCELLED;
        break;
    case PRINTERCTL_PRINT_FAILED:
    default:
        terminal.type = PRINTERCTL_PRINT_EVENT_FAILED;
        break;
    }
    emit(engine, &terminal);

    /* An interrupted session -- the print started (the file opened) but
     * did not complete -- leaves a durable record. Open failures leave
     * none: nothing was ever streamed, so there is nothing to recover. */
    if (out_summary->state != PRINTERCTL_PRINT_COMPLETED && out_summary->file_size > 0) {
        write_session_record(engine, path, out_summary);
    }

    if (engine->marker_active) {
        printerctl_record_remove_active_pid(engine->state_dir);
        engine->marker_active = false;
    }

    return status;
}

void printerctl_print_engine_request_pause(printerctl_print_engine_t *engine)
{
    engine->pause_requested = 1;
}

void printerctl_print_engine_request_resume(printerctl_print_engine_t *engine)
{
    engine->resume_requested = 1;
}

void printerctl_print_engine_request_cancel(printerctl_print_engine_t *engine)
{
    engine->cancel_requested = 1;
}

void printerctl_print_engine_request_cancel_power_failure(printerctl_print_engine_t *engine)
{
    engine->cancel_power_failure_requested = 1;
}

void printerctl_print_engine_set_control_poll(
    printerctl_print_engine_t *engine,
    printerctl_print_engine_control_poll_fn on_control_poll,
    void *context)
{
    if (engine == NULL) {
        return;
    }
    engine->on_control_poll = on_control_poll;
    engine->control_poll_context = context;
}

printerctl_printer_status_t printerctl_print_engine_disconnect(
    printerctl_print_engine_t *engine,
    printerctl_printer_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (engine == NULL) {
        out_error->status = PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
        snprintf(out_error->message, sizeof(out_error->message), "engine must not be NULL");
        return out_error->status;
    }

    if (engine->marker_active) {
        printerctl_record_remove_active_pid(engine->state_dir);
        engine->marker_active = false;
    }

    printerctl_printer_status_t status = printerctl_printer_disconnect(&engine->printer, out_error);
    if (engine->tx_gate_ready) {
        pthread_mutex_destroy(&engine->tx_gate);
        engine->tx_gate_ready = false;
    }
    return status;
}
