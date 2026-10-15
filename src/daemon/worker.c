#define _GNU_SOURCE

#include "printerctl/daemon/worker.h"

#include "printerctl/core/print_engine.h"
#include "printerctl/daemon/ipc.h"
#include "printerctl/macro.h"
#include "printerctl/protocol/plp1.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int cmd_read_fd;
    int event_write_fd;
    printerctl_config_t config;
    printerctl_print_engine_t engine;
    bool print_active;
    bool macro_active;
    printerctl_print_control_t macro_cancel;
    bool shutdown_after_macro;
    uint32_t macro_shutdown_token;
    bool shutdown_after_print;
    uint32_t shutdown_token;
    printerctl_print_summary_t last_progress; /* most recent PROGRESS snapshot; reused for
                                               * PAUSED/RESUMED so those transitions don't
                                               * reset counters to zero (PRINT-OBS-001) */
} worker_context_t;

static atomic_int g_signal_cancel_requested;
static atomic_int g_signal_power_failure_requested;

void printerctl_daemon_worker_reset_signal_requests(void)
{
    atomic_store_explicit(&g_signal_cancel_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&g_signal_power_failure_requested, 0, memory_order_relaxed);
}

void printerctl_daemon_worker_request_cancel_from_signal(void)
{
    atomic_store_explicit(&g_signal_cancel_requested, 1, memory_order_relaxed);
}

void printerctl_daemon_worker_request_power_failure_from_signal(void)
{
    atomic_store_explicit(&g_signal_power_failure_requested, 1, memory_order_relaxed);
}

static void emit_raw(int event_write_fd, printerctl_daemon_event_type_t type, uint32_t token,
                     printerctl_daemon_result_t result, const char *detail,
                     uint8_t execution_state, uint8_t progress_percent,
                     uint32_t commands_sent, uint32_t commands_acknowledged,
                     uint32_t commands_total, uint32_t source_position,
    uint16_t failure_reason, uint16_t wire_status)
{
    printerctl_daemon_event_t event;
    memset(&event, 0, sizeof(event));
    event.wire_size = sizeof(event);
    event.type = type;
    event.token = token;
    event.result = result;
    event.execution_kind = type == PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE
        ? PRINTERCTL_DAEMON_EXECUTION_KIND_PRINT
        : PRINTERCTL_DAEMON_EXECUTION_KIND_NONE;
    event.execution_state = execution_state;
    event.progress_percent = progress_percent;
    event.commands_sent = commands_sent;
    event.commands_acknowledged = commands_acknowledged;
    event.commands_total = commands_total;
    event.source_position = source_position;
    event.failure_reason = failure_reason;
    event.wire_status = wire_status;
    if (detail != NULL) {
        snprintf(event.detail, sizeof(event.detail), "%s", detail);
    }
    (void)printerctl_daemon_ipc_write_event(event_write_fd, &event);
}

static void emit(int event_write_fd, printerctl_daemon_event_type_t type, uint32_t token,
                 printerctl_daemon_result_t result, const char *detail)
{
    emit_raw(event_write_fd, type, token, result, detail, 0, 0, 0, 0, 0, 0, 0, 0);
}

static uint16_t map_printer_status(printerctl_printer_status_t status)
{
    switch (status) {
    case PRINTERCTL_PRINTER_OK: return PRINTERCTL_PLP1_SUCCESS;
    case PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT: return PRINTERCTL_PLP1_INVALID_ARGUMENT;
    case PRINTERCTL_PRINTER_ERR_NOT_CONNECTED:
    case PRINTERCTL_PRINTER_ERR_NO_PORT: return PRINTERCTL_PLP1_INVALID_STATE;
    case PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND: return PRINTERCTL_PLP1_SAFETY_REJECTED;
    case PRINTERCTL_PRINTER_ERR_GCODE: return PRINTERCTL_PLP1_INVALID_GCODE_SOURCE;
    case PRINTERCTL_PRINTER_ERR_SERIAL: return PRINTERCTL_PLP1_SERIAL_ERROR;
    case PRINTERCTL_PRINTER_ERR_TIMEOUT: return PRINTERCTL_PLP1_PRINTER_TIMEOUT;
    case PRINTERCTL_PRINTER_ERR_PRINTER_ERROR:
    case PRINTERCTL_PRINTER_ERR_RESEND:
    case PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE: return PRINTERCTL_PLP1_PRINTER_ERROR;
    case PRINTERCTL_PRINTER_CANCELLED: return PRINTERCTL_PLP1_SUCCESS;
    case PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE: return PRINTERCTL_PLP1_SHUTTING_DOWN;
    }
    return PRINTERCTL_PLP1_INTERNAL_ERROR;
}

static void emit_command_result(worker_context_t *ctx, uint32_t token,
                                printerctl_daemon_result_t result, uint16_t wire_status,
                                const char *detail)
{
    emit_raw(ctx->event_write_fd, PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED, token, result,
             detail, 0, 0, 0, 0, 0, 0, 0, wire_status);
}

static printerctl_daemon_event_t command_result_event(uint32_t token,
                                                      printerctl_daemon_result_t result,
                                                      uint16_t wire_status,
                                                      const char *detail)
{
    printerctl_daemon_event_t event;
    memset(&event, 0, sizeof(event));
    event.wire_size = sizeof(event);
    event.type = PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED;
    event.token = token;
    event.result = result;
    event.wire_status = wire_status;
    if (detail != NULL) {
        snprintf(event.detail, sizeof(event.detail), "%s", detail);
    }
    return event;
}

static void emit_temp_result(worker_context_t *ctx, uint32_t token,
                             const printerctl_marlin_temperature_report_t *report)
{
    printerctl_daemon_event_t event =
        command_result_event(token, PRINTERCTL_DAEMON_RESULT_OK, PRINTERCTL_PLP1_SUCCESS, NULL);
    event.query_has_hotend = report->has_hotend;
    event.query_hotend_actual = report->hotend_actual;
    event.query_hotend_target = report->hotend_target;
    event.query_has_bed = report->has_bed;
    event.query_bed_actual = report->bed_actual;
    event.query_bed_target = report->bed_target;
    (void)printerctl_daemon_ipc_write_event(ctx->event_write_fd, &event);
}

static void emit_position_result(worker_context_t *ctx, uint32_t token,
                                 const printerctl_marlin_position_report_t *report)
{
    printerctl_daemon_event_t event =
        command_result_event(token, PRINTERCTL_DAEMON_RESULT_OK, PRINTERCTL_PLP1_SUCCESS, NULL);
    event.query_x = report->x;
    event.query_y = report->y;
    event.query_z = report->z;
    event.query_has_e = report->has_e;
    event.query_e = report->e;
    (void)printerctl_daemon_ipc_write_event(ctx->event_write_fd, &event);
}

static void emit_info_result(worker_context_t *ctx, uint32_t token,
                             const printerctl_marlin_firmware_info_t *info)
{
    printerctl_daemon_event_t event =
        command_result_event(token, PRINTERCTL_DAEMON_RESULT_OK, PRINTERCTL_PLP1_SUCCESS, NULL);
    snprintf(event.query_firmware_name, sizeof(event.query_firmware_name), "%s",
             info->firmware_name);
    snprintf(event.query_machine_type, sizeof(event.query_machine_type), "%s",
             info->machine_type);
    snprintf(event.query_protocol_version, sizeof(event.query_protocol_version), "%s",
             info->protocol_version);
    snprintf(event.query_uuid, sizeof(event.query_uuid), "%s", info->uuid);
    event.query_has_extruder_count = info->has_extruder_count;
    event.query_extruder_count = info->extruder_count;
    (void)printerctl_daemon_ipc_write_event(ctx->event_write_fd, &event);
}

typedef struct {
    char *buffer;
    size_t size;
    size_t used;
} transcript_t;

static void collect_line(const char *line, void *context)
{
    transcript_t *tx = context;
    if (tx->used + 1 >= tx->size) {
        return;
    }
    int written = snprintf(tx->buffer + tx->used, tx->size - tx->used, "%s\n", line);
    if (written > 0) {
        size_t n = (size_t)written;
        tx->used += n < tx->size - tx->used ? n : tx->size - tx->used - 1u;
    }
}

static const char *home_command(uint8_t axis)
{
    switch (axis) {
    case PRINTERCTL_PLP1_HOME_X: return "G28 X";
    case PRINTERCTL_PLP1_HOME_Y: return "G28 Y";
    case PRINTERCTL_PLP1_HOME_Z: return "G28 Z";
    case PRINTERCTL_PLP1_HOME_ALL:
    default: return "G28";
    }
}

static printerctl_printer_status_t run_physical_operation(
    printerctl_printer_session_t *session,
    const printerctl_daemon_command_t *cmd,
    transcript_t *transcript,
    printerctl_printer_error_t *error)
{
    switch (cmd->type) {
    case PRINTERCTL_DAEMON_CMD_RAW_SEND:
        return printerctl_printer_send(session, cmd->text, collect_line, transcript, error);
    case PRINTERCTL_DAEMON_CMD_HOME:
        return printerctl_printer_execute(session, home_command(cmd->axis), collect_line,
                                          transcript, error);
    case PRINTERCTL_DAEMON_CMD_SD_PAUSE:
        return printerctl_printer_pause_sd_print(session, collect_line, transcript, error);
    case PRINTERCTL_DAEMON_CMD_SD_RESUME:
        return printerctl_printer_resume_sd_print(session, collect_line, transcript, error);
    case PRINTERCTL_DAEMON_CMD_SD_CANCEL:
        return printerctl_printer_cancel_sd_print(session, collect_line, transcript, error);
    default:
        memset(error, 0, sizeof(*error));
        error->status = PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT;
        snprintf(error->message, sizeof(error->message), "%s", "unsupported physical command");
        return error->status;
    }
}

static void run_physical(worker_context_t *ctx, const printerctl_daemon_command_t *cmd)
{
    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_status_t status =
        printerctl_printer_connect(&session, &ctx->config, NULL, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                            map_printer_status(status), error.message);
        return;
    }

    printerctl_daemon_event_t event =
        command_result_event(cmd->token, PRINTERCTL_DAEMON_RESULT_OK,
                             PRINTERCTL_PLP1_SUCCESS, NULL);
    transcript_t transcript = {
        .buffer = event.response_text,
        .size = sizeof(event.response_text),
        .used = 0,
    };

    status = run_physical_operation(&session, cmd, &transcript, &error);
    printerctl_printer_error_t disconnect_error;
    printerctl_printer_disconnect(&session, &disconnect_error);
    if (status == PRINTERCTL_PRINTER_OK) {
        (void)printerctl_daemon_ipc_write_event(ctx->event_write_fd, &event);
    } else {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                            map_printer_status(status), error.message);
    }
}

static void emit_print_update(worker_context_t *ctx, uint8_t execution_state,
                              const printerctl_print_summary_t *summary,
                              const char *detail, uint16_t failure_reason)
{
    uint8_t progress = 0;
    uint32_t sent = 0;
    uint32_t acknowledged = 0;
    uint32_t source_position = 0;
    if (summary != NULL) {
        progress = summary->progress_percent > 100u ? 100u : (uint8_t)summary->progress_percent;
        sent = summary->commands_sent > UINT32_MAX ? UINT32_MAX : (uint32_t)summary->commands_sent;
        acknowledged = summary->commands_acknowledged > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)summary->commands_acknowledged;
        source_position =
            summary->bytes_consumed > UINT32_MAX ? UINT32_MAX : (uint32_t)summary->bytes_consumed;
    }
    uint64_t total64 = printerctl_print_engine_commands_total(&ctx->engine);
    uint32_t total = total64 > UINT32_MAX ? UINT32_MAX : (uint32_t)total64;
    emit_raw(ctx->event_write_fd, PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE, 0,
             PRINTERCTL_DAEMON_RESULT_OK, detail, execution_state, progress, sent, acknowledged,
             total, source_position, failure_reason, 0);
}

static void emit_macro_update(worker_context_t *ctx, uint8_t execution_state, const char *id,
                              uint32_t current_step, uint32_t total_steps,
                              uint16_t terminal_result, const char *detail)
{
    printerctl_daemon_event_t event;
    memset(&event, 0, sizeof(event));
    event.wire_size = sizeof(event);
    event.type = PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE;
    event.result = PRINTERCTL_DAEMON_RESULT_OK;
    event.execution_kind = PRINTERCTL_DAEMON_EXECUTION_KIND_MACRO;
    event.execution_state = execution_state;
    event.progress_percent =
        total_steps == 0 ? 0 : (uint8_t)((current_step * 100u) / total_steps);
    snprintf(event.macro_id, sizeof(event.macro_id), "%s", id != NULL ? id : "");
    event.macro_current_step = current_step;
    event.macro_total_steps = total_steps;
    event.macro_terminal_result = terminal_result;
    event.failure_reason = terminal_result;
    if (detail != NULL) {
        snprintf(event.detail, sizeof(event.detail), "%s", detail);
    }
    (void)printerctl_daemon_ipc_write_event(ctx->event_write_fd, &event);
}

static uint16_t validate_print_source(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? PRINTERCTL_PLP1_FILE_NOT_FOUND
                               : PRINTERCTL_PLP1_FILE_NOT_READABLE;
    }
    if (!S_ISREG(st.st_mode)) {
        return PRINTERCTL_PLP1_INVALID_GCODE_SOURCE;
    }
    if (access(path, R_OK) != 0) {
        return PRINTERCTL_PLP1_FILE_NOT_READABLE;
    }
    return PRINTERCTL_PLP1_SUCCESS;
}

static void on_print_event(const printerctl_print_event_t *event, void *context)
{
    worker_context_t *ctx = context;

    if (event->type == PRINTERCTL_PRINT_EVENT_STARTED) {
        memset(&ctx->last_progress, 0, sizeof(ctx->last_progress));
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING, NULL, NULL, 0);
    } else if (event->type == PRINTERCTL_PRINT_EVENT_PROGRESS && event->progress != NULL) {
        printerctl_print_summary_t summary;
        memset(&summary, 0, sizeof(summary));
        summary.commands_sent = event->progress->command_number;
        summary.commands_acknowledged = event->progress->command_number;
        summary.bytes_consumed = event->progress->bytes_consumed;
        summary.progress_percent = event->progress->progress_percent;
        ctx->last_progress = summary;
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING, &summary, NULL, 0);
    } else if (event->type == PRINTERCTL_PRINT_EVENT_PAUSED) {
        /* Reuse the last known progress -- see worker_context_t.last_progress's
         * doc comment; passing NULL here would zero progress on pause. */
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_PAUSED, &ctx->last_progress, NULL,
                          0);
    } else if (event->type == PRINTERCTL_PRINT_EVENT_RESUMED) {
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING, &ctx->last_progress, NULL,
                          0);
    } else if (event->type == PRINTERCTL_PRINT_EVENT_COMPLETED && event->summary != NULL) {
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED, event->summary, NULL, 0);
    } else if (event->type == PRINTERCTL_PRINT_EVENT_CANCELLED && event->summary != NULL) {
        uint16_t reason = event->summary->state == PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE
            ? PRINTERCTL_PLP1_SHUTTING_DOWN
            : 0;
        const char *detail = reason != 0 ? "print cancelled: power failure" : NULL;
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED, event->summary, detail,
                          reason);
    } else if (event->type == PRINTERCTL_PRINT_EVENT_FAILED && event->summary != NULL) {
        const char *detail = event->error != NULL ? event->error->message : "print failed";
        uint16_t reason = event->error != NULL ? map_printer_status(event->error->status)
                                               : PRINTERCTL_PLP1_INTERNAL_ERROR;
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED, event->summary, detail,
                          reason);
    }
}

static bool command_available(int fd)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

static void poll_control_commands(printerctl_print_engine_t *engine, void *context)
{
    worker_context_t *ctx = context;

    if (atomic_load_explicit(&g_signal_power_failure_requested, memory_order_relaxed)) {
        printerctl_print_engine_request_cancel_power_failure(engine);
        (void)printerctl_print_engine_commit_cancellation(engine);
    } else if (atomic_load_explicit(&g_signal_cancel_requested, memory_order_relaxed)) {
        printerctl_print_engine_request_cancel(engine);
        (void)printerctl_print_engine_commit_cancellation(engine);
    }

    while (command_available(ctx->cmd_read_fd)) {
        printerctl_daemon_command_t cmd;
        printerctl_daemon_ipc_status_t status =
            printerctl_daemon_ipc_read_command(ctx->cmd_read_fd, &cmd);
        if (status != PRINTERCTL_DAEMON_IPC_OK) {
            printerctl_print_engine_request_cancel(engine);
            (void)printerctl_print_engine_commit_cancellation(engine);
            return;
        }

        switch (cmd.type) {
        case PRINTERCTL_DAEMON_CMD_PRINT_PAUSE:
            printerctl_print_engine_request_pause(&ctx->engine);
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_RESUME:
            printerctl_print_engine_request_resume(&ctx->engine);
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL:
            atomic_store_explicit(&g_signal_cancel_requested, 0, memory_order_relaxed);
            printerctl_print_engine_request_cancel(&ctx->engine);
            (void)printerctl_print_engine_commit_cancellation(&ctx->engine);
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL_POWER_FAILURE:
            atomic_store_explicit(&g_signal_power_failure_requested, 0, memory_order_relaxed);
            printerctl_print_engine_request_cancel_power_failure(&ctx->engine);
            (void)printerctl_print_engine_commit_cancellation(&ctx->engine);
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_START:
        case PRINTERCTL_DAEMON_CMD_MACRO_EXECUTE:
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_ERROR,
                                PRINTERCTL_PLP1_BUSY, "print already active");
            break;
        case PRINTERCTL_DAEMON_CMD_QUERY_TEMP:
        case PRINTERCTL_DAEMON_CMD_QUERY_POSITION:
        case PRINTERCTL_DAEMON_CMD_QUERY_INFO:
        case PRINTERCTL_DAEMON_CMD_RAW_SEND:
        case PRINTERCTL_DAEMON_CMD_HOME:
        case PRINTERCTL_DAEMON_CMD_SD_PAUSE:
        case PRINTERCTL_DAEMON_CMD_SD_RESUME:
        case PRINTERCTL_DAEMON_CMD_SD_CANCEL:
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_ERROR,
                                PRINTERCTL_PLP1_BUSY, "print already active");
            break;
        case PRINTERCTL_DAEMON_CMD_SHUTDOWN:
            atomic_store_explicit(&g_signal_cancel_requested, 0, memory_order_relaxed);
            ctx->shutdown_after_print = true;
            ctx->shutdown_token = cmd.token;
            printerctl_print_engine_request_cancel(&ctx->engine);
            (void)printerctl_print_engine_commit_cancellation(&ctx->engine);
            emit(ctx->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPING, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            break;
        case PRINTERCTL_DAEMON_CMD_NOP:
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        }
    }

    if (ctx->shutdown_after_print) {
        printerctl_print_engine_request_cancel(engine);
        (void)printerctl_print_engine_commit_cancellation(engine);
    }
}

static printerctl_macro_calibration_config_t macro_config_from_runtime(
    const printerctl_config_t *config)
{
    printerctl_macro_calibration_config_t macro_config;
    memset(&macro_config, 0, sizeof(macro_config));
    macro_config.has_min_x = config->has_calibration_min_x;
    macro_config.has_max_x = config->has_calibration_max_x;
    macro_config.has_min_y = config->has_calibration_min_y;
    macro_config.has_max_y = config->has_calibration_max_y;
    macro_config.has_safe_z = config->has_calibration_safe_z;
    macro_config.has_xy_feedrate = config->has_calibration_xy_feedrate;
    macro_config.has_z_feedrate = config->has_calibration_z_feedrate;
    macro_config.min_x = config->calibration_min_x;
    macro_config.max_x = config->calibration_max_x;
    macro_config.min_y = config->calibration_min_y;
    macro_config.max_y = config->calibration_max_y;
    macro_config.safe_z = config->calibration_safe_z;
    macro_config.xy_feedrate = config->calibration_xy_feedrate;
    macro_config.z_feedrate = config->calibration_z_feedrate;
    return macro_config;
}

static printerctl_print_control_t poll_macro_control_commands(void *context)
{
    worker_context_t *ctx = context;
    if (atomic_exchange_explicit(&g_signal_power_failure_requested, 0, memory_order_relaxed)) {
        ctx->macro_cancel = PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE;
    } else if (atomic_exchange_explicit(&g_signal_cancel_requested, 0, memory_order_relaxed) &&
               ctx->macro_cancel == PRINTERCTL_PRINT_CONTROL_CONTINUE) {
        ctx->macro_cancel = PRINTERCTL_PRINT_CONTROL_CANCEL;
    }
    while (command_available(ctx->cmd_read_fd)) {
        printerctl_daemon_command_t cmd;
        printerctl_daemon_ipc_status_t status =
            printerctl_daemon_ipc_read_command(ctx->cmd_read_fd, &cmd);
        if (status != PRINTERCTL_DAEMON_IPC_OK) {
            ctx->macro_cancel = PRINTERCTL_PRINT_CONTROL_CANCEL;
            return ctx->macro_cancel;
        }

        switch (cmd.type) {
        case PRINTERCTL_DAEMON_CMD_SHUTDOWN:
            ctx->shutdown_after_macro = true;
            ctx->macro_shutdown_token = cmd.token;
            emit(ctx->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPING, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            if (ctx->macro_cancel == PRINTERCTL_PRINT_CONTROL_CONTINUE)
                ctx->macro_cancel = PRINTERCTL_PRINT_CONTROL_CANCEL;
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_START:
        case PRINTERCTL_DAEMON_CMD_MACRO_EXECUTE:
        case PRINTERCTL_DAEMON_CMD_QUERY_TEMP:
        case PRINTERCTL_DAEMON_CMD_QUERY_POSITION:
        case PRINTERCTL_DAEMON_CMD_QUERY_INFO:
        case PRINTERCTL_DAEMON_CMD_RAW_SEND:
        case PRINTERCTL_DAEMON_CMD_HOME:
        case PRINTERCTL_DAEMON_CMD_SD_PAUSE:
        case PRINTERCTL_DAEMON_CMD_SD_RESUME:
        case PRINTERCTL_DAEMON_CMD_SD_CANCEL:
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_ERROR,
                                PRINTERCTL_PLP1_BUSY, "macro already active");
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_PAUSE:
        case PRINTERCTL_DAEMON_CMD_PRINT_RESUME:
        case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL:
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_ERROR,
                                PRINTERCTL_PLP1_NOT_PRINTING, "no active print");
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL_POWER_FAILURE:
            ctx->macro_cancel = PRINTERCTL_PRINT_CONTROL_CANCEL_POWER_FAILURE;
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        case PRINTERCTL_DAEMON_CMD_NOP:
            emit_command_result(ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        }
    }
    return ctx->macro_cancel;
}

static bool format_move_command(const printerctl_macro_step_t *step, char *out, size_t out_size)
{
    if (out_size < 3 || step->feedrate_mm_min < 0.001) return false;
    memcpy(out, "G1", 3);
    size_t used = 2;
    const char axes[] = "XYZ";
    const double values[] = {step->x, step->y, step->z};
    for (size_t i = 0; i < 3; ++i) {
        if ((step->axes & (1u << i)) == 0) continue;
        if (values[i] != 0 && fabs(values[i]) < 0.001) return false;
        int n = snprintf(out + used, out_size - used, " %c%.3f", axes[i], values[i]);
        if (n < 0 || (size_t)n >= out_size - used) return false;
        used += (size_t)n;
    }
    int n = snprintf(out + used, out_size - used, " F%.3f", step->feedrate_mm_min);
    return n >= 0 && (size_t)n < out_size - used;
}

static printerctl_printer_status_t execute_macro_command(worker_context_t *ctx,
                                                         printerctl_printer_session_t *session,
                                                         const char *command,
                                                         printerctl_printer_error_t *error)
{
    return printerctl_printer_execute_controlled(session, command,
                                                  poll_macro_control_commands, ctx, error);
}

static void run_macro(worker_context_t *ctx, const printerctl_daemon_command_t *cmd)
{
    if (ctx->print_active || ctx->macro_active) {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                            PRINTERCTL_PLP1_BUSY, "execution already active");
        return;
    }

    printerctl_macro_step_t steps[PRINTERCTL_MACRO_STEPS_MAX];
    printerctl_macro_definition_t definition;
    printerctl_macro_error_t macro_error;
    printerctl_macro_calibration_config_t macro_config = macro_config_from_runtime(&ctx->config);
    printerctl_macro_status_t macro_status =
        printerctl_macro_resolve_builtin(cmd->macro_id, &macro_config, &definition, steps,
                                         sizeof(steps) / sizeof(steps[0]), &macro_error);
    if (macro_status != PRINTERCTL_MACRO_OK) {
        uint16_t wire_status = macro_status == PRINTERCTL_MACRO_NOT_FOUND
            ? PRINTERCTL_PLP1_INVALID_ARGUMENT
            : PRINTERCTL_PLP1_INVALID_STATE;
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR, wire_status,
                            macro_error.message);
        return;
    }

    char moves[PRINTERCTL_MACRO_STEPS_MAX][128];
    for (size_t i = 0; i < definition.step_count; ++i) {
        if (!format_move_command(&definition.steps[i], moves[i], sizeof(moves[i]))) {
            emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                                PRINTERCTL_PLP1_INVALID_ARGUMENT, "macro command is not representable");
            return;
        }
    }
    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_status_t status =
        printerctl_printer_connect(&session, &ctx->config, NULL, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                            map_printer_status(status), error.message);
        return;
    }

    ctx->macro_active = true;
    ctx->macro_cancel = PRINTERCTL_PRINT_CONTROL_CONTINUE;
    emit_macro_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING, definition.id, 0,
                      (uint32_t)definition.step_count, 0, NULL);
    emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_OK, PRINTERCTL_PLP1_SUCCESS,
                        NULL);

    uint32_t completed = 0;
    uint16_t terminal = PRINTERCTL_PLP1_SUCCESS;
    status = execute_macro_command(ctx, &session, "G21", &error);
    for (size_t i = 0; status == PRINTERCTL_PRINTER_OK && i < definition.step_count; ++i) {
        const printerctl_macro_step_t *step = &definition.steps[i];
        const char *mode = step->type == PRINTERCTL_MACRO_STEP_MOVE_ABSOLUTE ? "G90" : "G91";
        status = execute_macro_command(ctx, &session, mode, &error);
        if (status != PRINTERCTL_PRINTER_OK) {
            terminal = status == PRINTERCTL_PRINTER_CANCELLED ? PRINTERCTL_PLP1_SUCCESS
                                                              : map_printer_status(status);
            break;
        }
        status = execute_macro_command(ctx, &session, moves[i], &error);
        if (status != PRINTERCTL_PRINTER_OK) {
            terminal = map_printer_status(status);
            break;
        }
        completed++;
        emit_macro_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING, definition.id, completed,
                          (uint32_t)definition.step_count, 0, NULL);
    }

    if (status == PRINTERCTL_PRINTER_OK) {
        status = execute_macro_command(ctx, &session, "G90", &error);
        if (status != PRINTERCTL_PRINTER_OK) {
            terminal = map_printer_status(status);
        }
    }

    printerctl_printer_error_t disconnect_error;
    printerctl_printer_disconnect(&session, &disconnect_error);
    ctx->macro_active = false;

    if (status == PRINTERCTL_PRINTER_OK) {
        emit_macro_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED, definition.id, completed,
                          (uint32_t)definition.step_count, PRINTERCTL_PLP1_SUCCESS, NULL);
    } else if (status == PRINTERCTL_PRINTER_CANCELLED ||
               status == PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE) {
        terminal = status == PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE
            ? PRINTERCTL_PLP1_SHUTTING_DOWN : PRINTERCTL_PLP1_SUCCESS;
        emit_macro_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED, definition.id, completed,
                          (uint32_t)definition.step_count, terminal, error.message);
    } else {
        terminal = map_printer_status(status);
        emit_macro_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED, definition.id, completed,
                          (uint32_t)definition.step_count, terminal, error.message);
    }

    if (ctx->shutdown_after_macro) {
        emit(ctx->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPED, ctx->macro_shutdown_token,
             PRINTERCTL_DAEMON_RESULT_OK, NULL);
    }
}

static void run_print(worker_context_t *ctx, const printerctl_daemon_command_t *cmd)
{
    printerctl_printer_error_t error;
    uint16_t source_status = validate_print_source(cmd->path);
    if (source_status != PRINTERCTL_PLP1_SUCCESS) {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR, source_status,
                            "print source is not usable");
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED, NULL,
                          "print source is not usable", source_status);
        return;
    }

    printerctl_printer_status_t status = printerctl_print_engine_connect(
        &ctx->engine, &ctx->config, NULL, false, on_print_event, ctx, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                            map_printer_status(status), error.message);
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED, NULL, error.message,
                          map_printer_status(status));
        return;
    }

    ctx->print_active = true;
    printerctl_print_engine_set_control_poll(&ctx->engine, poll_control_commands, ctx);
    memset(&ctx->last_progress, 0, sizeof(ctx->last_progress));
    emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING, NULL, NULL, 0);
    emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_OK, PRINTERCTL_PLP1_SUCCESS,
                        NULL);

    printerctl_print_summary_t summary;
    status = printerctl_print_engine_run(&ctx->engine, cmd->path, &summary, &error);
    if (status != PRINTERCTL_PRINTER_OK && status != PRINTERCTL_PRINTER_CANCELLED &&
        status != PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE) {
        emit_print_update(ctx, PRINTERCTL_PLP1_EXECUTION_STATE_FAILED, &summary, error.message,
                          map_printer_status(status));
    }

    printerctl_print_engine_disconnect(&ctx->engine, &error);
    ctx->print_active = false;

    if (ctx->shutdown_after_print) {
        emit(ctx->event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPED, ctx->shutdown_token,
             PRINTERCTL_DAEMON_RESULT_OK, NULL);
    }
}

static void run_query(worker_context_t *ctx, const printerctl_daemon_command_t *cmd)
{
    printerctl_printer_session_t session;
    printerctl_printer_error_t error;
    printerctl_printer_status_t status =
        printerctl_printer_connect(&session, &ctx->config, NULL, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                            map_printer_status(status), error.message);
        return;
    }

    if (cmd->type == PRINTERCTL_DAEMON_CMD_QUERY_TEMP) {
        printerctl_marlin_temperature_report_t report;
        status = printerctl_printer_query_temperature(&session, &report, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            emit_temp_result(ctx, cmd->token, &report);
        }
    } else if (cmd->type == PRINTERCTL_DAEMON_CMD_QUERY_POSITION) {
        printerctl_marlin_position_report_t report;
        status = printerctl_printer_query_position(&session, &report, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            emit_position_result(ctx, cmd->token, &report);
        }
    } else {
        printerctl_marlin_firmware_info_t info;
        status = printerctl_printer_query_info(&session, &info, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            emit_info_result(ctx, cmd->token, &info);
        }
    }

    printerctl_printer_disconnect(&session, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        emit_command_result(ctx, cmd->token, PRINTERCTL_DAEMON_RESULT_ERROR,
                            map_printer_status(status), error.message);
    }
}

void *printerctl_daemon_worker_main(void *arg)
{
    printerctl_daemon_worker_args_t *args = arg;
    worker_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cmd_read_fd = args->cmd_read_fd;
    ctx.event_write_fd = args->event_write_fd;
    ctx.config = args->config;

    if (args->init_hook != NULL) {
        char message[PRINTERCTL_DAEMON_EVENT_DETAIL_MAX];
        message[0] = '\0';
        if (!args->init_hook(args->init_context, message, sizeof(message))) {
            emit(ctx.event_write_fd, PRINTERCTL_DAEMON_EVENT_FAULT, 0,
                 PRINTERCTL_DAEMON_RESULT_ERROR,
                 message[0] != '\0' ? message : "worker initialization failed");
            return NULL;
        }
    }

    emit(ctx.event_write_fd, PRINTERCTL_DAEMON_EVENT_READY, 0, PRINTERCTL_DAEMON_RESULT_OK, NULL);

    for (;;) {
        printerctl_daemon_command_t cmd;
        printerctl_daemon_ipc_status_t status =
            printerctl_daemon_ipc_read_command(ctx.cmd_read_fd, &cmd);

        if (status == PRINTERCTL_DAEMON_IPC_EOF) {
            emit(ctx.event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPING, 0,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            emit(ctx.event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPED, 0,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            return NULL;
        }

        if (status != PRINTERCTL_DAEMON_IPC_OK) {
            const char *detail = status == PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED
                ? "cmd_pipe: truncated message"
                : status == PRINTERCTL_DAEMON_IPC_ERR_MALFORMED
                    ? "cmd_pipe: malformed message"
                    : "cmd_pipe: read failed";
            emit(ctx.event_write_fd, PRINTERCTL_DAEMON_EVENT_FAULT, 0,
                 PRINTERCTL_DAEMON_RESULT_ERROR, detail);
            return NULL;
        }

        switch (cmd.type) {
        case PRINTERCTL_DAEMON_CMD_NOP:
            emit_command_result(&ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_OK,
                                PRINTERCTL_PLP1_SUCCESS, NULL);
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_START:
            run_print(&ctx, &cmd);
            if (ctx.shutdown_after_print) {
                return NULL;
            }
            break;
        case PRINTERCTL_DAEMON_CMD_QUERY_TEMP:
        case PRINTERCTL_DAEMON_CMD_QUERY_POSITION:
        case PRINTERCTL_DAEMON_CMD_QUERY_INFO:
            run_query(&ctx, &cmd);
            break;
        case PRINTERCTL_DAEMON_CMD_RAW_SEND:
        case PRINTERCTL_DAEMON_CMD_HOME:
        case PRINTERCTL_DAEMON_CMD_SD_PAUSE:
        case PRINTERCTL_DAEMON_CMD_SD_RESUME:
        case PRINTERCTL_DAEMON_CMD_SD_CANCEL:
            run_physical(&ctx, &cmd);
            break;
        case PRINTERCTL_DAEMON_CMD_MACRO_EXECUTE:
            run_macro(&ctx, &cmd);
            if (ctx.shutdown_after_macro) {
                return NULL;
            }
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_PAUSE:
        case PRINTERCTL_DAEMON_CMD_PRINT_RESUME:
        case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL:
            atomic_store_explicit(&g_signal_cancel_requested, 0, memory_order_relaxed);
            emit_command_result(&ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_ERROR,
                                PRINTERCTL_PLP1_NOT_PRINTING, "no active print");
            break;
        case PRINTERCTL_DAEMON_CMD_PRINT_CANCEL_POWER_FAILURE:
            atomic_store_explicit(&g_signal_power_failure_requested, 0, memory_order_relaxed);
            emit_command_result(&ctx, cmd.token, PRINTERCTL_DAEMON_RESULT_ERROR,
                                PRINTERCTL_PLP1_NOT_PRINTING, "no active print");
            break;
        case PRINTERCTL_DAEMON_CMD_SHUTDOWN:
            atomic_store_explicit(&g_signal_cancel_requested, 0, memory_order_relaxed);
            emit(ctx.event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPING, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            emit(ctx.event_write_fd, PRINTERCTL_DAEMON_EVENT_STOPPED, cmd.token,
                 PRINTERCTL_DAEMON_RESULT_OK, NULL);
            return NULL;
        }
    }
}
