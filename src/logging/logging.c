#define _POSIX_C_SOURCE 200809L

#include "printerctl/logging.h"

#include <time.h>

static const char *level_name(printerctl_log_level_t level)
{
    switch (level) {
    case PRINTERCTL_LOG_DEBUG: return "DEBUG";
    case PRINTERCTL_LOG_INFO: return "INFO";
    case PRINTERCTL_LOG_WARNING: return "WARNING";
    case PRINTERCTL_LOG_ERROR: return "ERROR";
    }
    return NULL;
}

static const char *event_name(printerctl_log_event_t event)
{
    switch (event) {
    case PRINTERCTL_LOG_EVENT_SESSION_START: return "session_start";
    case PRINTERCTL_LOG_EVENT_SESSION_END: return "session_end";
    case PRINTERCTL_LOG_EVENT_CONFIG_SUMMARY: return "config_summary";
    case PRINTERCTL_LOG_EVENT_COMMAND_PARSED: return "command_parsed";
    case PRINTERCTL_LOG_EVENT_COMMAND_UNAVAILABLE: return "command_unavailable";
    case PRINTERCTL_LOG_EVENT_SERIAL_TX: return "serial_tx";
    case PRINTERCTL_LOG_EVENT_SERIAL_RX: return "serial_rx";
    case PRINTERCTL_LOG_EVENT_STATE_TRANSITION: return "state_transition";
    case PRINTERCTL_LOG_EVENT_PROGRESS: return "progress";
    case PRINTERCTL_LOG_EVENT_ERROR: return "error";
    case PRINTERCTL_LOG_EVENT_WARNING: return "warning";
    }
    return NULL;
}

printerctl_log_status_t printerctl_log_init(printerctl_logger_t *logger, FILE *stream, bool verbose)
{
    if (logger == NULL) {
        return PRINTERCTL_LOG_ERR_INVALID_ARGUMENT;
    }

    if (stream == NULL) {
        logger->stream = NULL;
        logger->verbose = false;
        logger->initialized = false;
        return PRINTERCTL_LOG_ERR_INVALID_ARGUMENT;
    }

    logger->stream = stream;
    logger->verbose = verbose;
    logger->initialized = true;
    return PRINTERCTL_LOG_OK;
}

printerctl_log_status_t printerctl_log_shutdown(printerctl_logger_t *logger)
{
    if (logger == NULL) {
        return PRINTERCTL_LOG_ERR_INVALID_ARGUMENT;
    }

    if (!logger->initialized) {
        return PRINTERCTL_LOG_OK;
    }

    FILE *stream = logger->stream;
    logger->initialized = false;
    logger->stream = NULL;

    if (fflush(stream) != 0) {
        return PRINTERCTL_LOG_ERR_FLUSH;
    }

    return PRINTERCTL_LOG_OK;
}

/* Escapes and writes `detail` field-by-field directly to `stream`; NULL writes nothing. */
static bool write_escaped_detail(FILE *stream, const char *detail)
{
    if (detail == NULL) {
        return true;
    }

    for (const unsigned char *p = (const unsigned char *)detail; *p != '\0'; p++) {
        unsigned char c = *p;
        int result;

        switch (c) {
        case '\\':
            result = fputs("\\\\", stream);
            break;
        case '\t':
            result = fputs("\\t", stream);
            break;
        case '\r':
            result = fputs("\\r", stream);
            break;
        case '\n':
            result = fputs("\\n", stream);
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                result = fprintf(stream, "\\x%02X", c);
            } else {
                result = fputc(c, stream);
            }
            break;
        }

        if (result < 0) {
            return false;
        }
    }

    return true;
}

printerctl_log_status_t printerctl_log_record(printerctl_logger_t *logger,
                                                printerctl_log_level_t level,
                                                printerctl_log_event_t event,
                                                const char *detail)
{
    if (logger == NULL) {
        return PRINTERCTL_LOG_ERR_INVALID_ARGUMENT;
    }

    if (level_name(level) == NULL || event_name(event) == NULL) {
        return PRINTERCTL_LOG_ERR_INVALID_ARGUMENT;
    }

    if (!logger->initialized) {
        return PRINTERCTL_LOG_ERR_NOT_INITIALIZED;
    }

    if (logger->stream == NULL) {
        return PRINTERCTL_LOG_ERR_INVALID_ARGUMENT;
    }

    if (level == PRINTERCTL_LOG_DEBUG && !logger->verbose) {
        return PRINTERCTL_LOG_OK;
    }

    FILE *stream = logger->stream;

    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return PRINTERCTL_LOG_ERR_CLOCK;
    }

    struct tm tm_utc;
    if (gmtime_r(&ts.tv_sec, &tm_utc) == NULL) {
        return PRINTERCTL_LOG_ERR_CLOCK;
    }

    long milliseconds = ts.tv_nsec / 1000000L;
    if (milliseconds < 0) {
        milliseconds = 0;
    } else if (milliseconds > 999) {
        milliseconds = 999;
    }

    if (fprintf(stream, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ\t%s\t%s\t",
                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, milliseconds,
                level_name(level), event_name(event)) < 0) {
        return PRINTERCTL_LOG_ERR_WRITE;
    }

    if (!write_escaped_detail(stream, detail)) {
        return PRINTERCTL_LOG_ERR_WRITE;
    }

    if (fputc('\n', stream) == EOF) {
        return PRINTERCTL_LOG_ERR_WRITE;
    }

    if (level == PRINTERCTL_LOG_ERROR) {
        if (fflush(stream) != 0) {
            return PRINTERCTL_LOG_ERR_FLUSH;
        }
    }

    return PRINTERCTL_LOG_OK;
}

printerctl_log_status_t printerctl_log_serial_tx(printerctl_logger_t *logger, const char *detail)
{
    return printerctl_log_record(logger, PRINTERCTL_LOG_DEBUG, PRINTERCTL_LOG_EVENT_SERIAL_TX, detail);
}

printerctl_log_status_t printerctl_log_serial_rx(printerctl_logger_t *logger, const char *detail)
{
    return printerctl_log_record(logger, PRINTERCTL_LOG_DEBUG, PRINTERCTL_LOG_EVENT_SERIAL_RX, detail);
}

printerctl_log_status_t printerctl_log_state_transition(printerctl_logger_t *logger, const char *detail)
{
    return printerctl_log_record(logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_STATE_TRANSITION, detail);
}

printerctl_log_status_t printerctl_log_progress(printerctl_logger_t *logger, const char *detail)
{
    return printerctl_log_record(logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_PROGRESS, detail);
}

printerctl_log_status_t printerctl_log_error(printerctl_logger_t *logger, const char *detail)
{
    return printerctl_log_record(logger, PRINTERCTL_LOG_ERROR, PRINTERCTL_LOG_EVENT_ERROR, detail);
}
