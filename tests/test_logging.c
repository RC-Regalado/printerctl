#define _POSIX_C_SOURCE 200809L

#include "printerctl/logging.h"

#include <ctype.h>
#include <stdbool.h>
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

/* Counts occurrences of `needle` in `haystack`; both NUL-terminated. */
static int count_occurrences(const char *haystack, const char *needle)
{
    if (haystack == NULL) {
        return 0;
    }

    int count = 0;
    const char *p = haystack;
    size_t needle_len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }

    return count;
}

/*
 * Validates the RFC3339-with-milliseconds UTC timestamp shape this module
 * promises: "YYYY-MM-DDTHH:MM:SS.mmmZ", 24 characters, fixed separators,
 * digits everywhere else.
 */
static bool timestamp_looks_valid(const char *ts)
{
    if (strlen(ts) != 24) {
        return false;
    }

    static const char pattern[] = "DDDD-DD-DDTDD:DD:DD.DDDZ";
    for (int i = 0; i < 24; i++) {
        if (pattern[i] == 'D') {
            if (!isdigit((unsigned char)ts[i])) {
                return false;
            }
        } else if (ts[i] != pattern[i]) {
            return false;
        }
    }

    return true;
}

static void test_timestamp_format_and_structure(void)
{
    char *out = NULL;
    size_t out_len = 0;
    FILE *stream = open_memstream(&out, &out_len);

    printerctl_logger_t logger;
    check(printerctl_log_init(&logger, stream, false) == PRINTERCTL_LOG_OK, "init succeeds with a valid stream");
    check(printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_START, "hello") == PRINTERCTL_LOG_OK,
          "recording a session_start line succeeds");
    check(printerctl_log_shutdown(&logger) == PRINTERCTL_LOG_OK, "shutdown flushes cleanly");
    fclose(stream);

    check(out != NULL, "memstream produced output");
    check(count_occurrences(out, "\n") == 1, "exactly one physical line was written");

    char timestamp[32] = {0};
    char level[32] = {0};
    char event[32] = {0};
    char detail[32] = {0};
    int fields = sscanf(out, "%31[^\t]\t%31[^\t]\t%31[^\t]\t%31[^\n]", timestamp, level, event, detail);

    check(fields == 4, "line splits into exactly four tab-separated fields");
    check(timestamp_looks_valid(timestamp), "timestamp matches RFC3339 UTC with milliseconds");
    check(strcmp(level, "INFO") == 0, "level field is INFO");
    check(strcmp(event, "session_start") == 0, "event field is session_start");
    check(strcmp(detail, "hello") == 0, "detail field is preserved");

    free(out);
}

static void test_all_levels_and_error_flush(void)
{
    char *out = NULL;
    size_t out_len = 0;
    FILE *stream = open_memstream(&out, &out_len);

    printerctl_logger_t logger;
    printerctl_log_init(&logger, stream, true);

    check(printerctl_log_record(&logger, PRINTERCTL_LOG_DEBUG, PRINTERCTL_LOG_EVENT_COMMAND_PARSED, "d") == PRINTERCTL_LOG_OK,
          "DEBUG record succeeds when verbose");
    check(printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_START, "i") == PRINTERCTL_LOG_OK,
          "INFO record succeeds");
    check(printerctl_log_record(&logger, PRINTERCTL_LOG_WARNING, PRINTERCTL_LOG_EVENT_WARNING, "w") == PRINTERCTL_LOG_OK,
          "WARNING record succeeds");

    /* Non-ERROR records are not flushed immediately; memstream should still be empty. */
    fflush(NULL);
    check(out_len == 0, "non-ERROR records are not flushed before shutdown");

    check(printerctl_log_record(&logger, PRINTERCTL_LOG_ERROR, PRINTERCTL_LOG_EVENT_ERROR, "e") == PRINTERCTL_LOG_OK,
          "ERROR record succeeds");
    check(out_len > 0, "an ERROR record triggers an immediate flush");
    check(count_occurrences(out, "DEBUG") == 1, "one DEBUG line present after flush");
    check(count_occurrences(out, "INFO") == 1, "one INFO line present after flush");
    check(count_occurrences(out, "WARNING") == 1, "one WARNING line present after flush");
    check(count_occurrences(out, "ERROR") == 1, "one ERROR line present after flush");

    printerctl_log_shutdown(&logger);
    fclose(stream);
    free(out);
}

static void test_verbose_filtering(void)
{
    {
        char *out = NULL;
        size_t out_len = 0;
        FILE *stream = open_memstream(&out, &out_len);

        printerctl_logger_t logger;
        printerctl_log_init(&logger, stream, false);
        check(printerctl_log_record(&logger, PRINTERCTL_LOG_DEBUG, PRINTERCTL_LOG_EVENT_COMMAND_PARSED, "x") == PRINTERCTL_LOG_OK,
              "filtered DEBUG record still returns OK");
        printerctl_log_shutdown(&logger);
        fclose(stream);

        check(out != NULL && out[0] == '\0', "DEBUG record produces no output when verbose is false");
        free(out);
    }
    {
        char *out = NULL;
        size_t out_len = 0;
        FILE *stream = open_memstream(&out, &out_len);

        printerctl_logger_t logger;
        printerctl_log_init(&logger, stream, true);
        printerctl_log_record(&logger, PRINTERCTL_LOG_DEBUG, PRINTERCTL_LOG_EVENT_COMMAND_PARSED, "x");
        printerctl_log_shutdown(&logger);
        fclose(stream);

        check(out != NULL && strstr(out, "DEBUG") != NULL, "DEBUG record is written when verbose is true");
        free(out);
    }
}

static void test_escaping_and_injection(void)
{
    char *out = NULL;
    size_t out_len = 0;
    FILE *stream = open_memstream(&out, &out_len);

    printerctl_logger_t logger;
    printerctl_log_init(&logger, stream, false);

    /* backslash, tab, CR, LF, a C0 control byte, DEL, and a raw >=0x80 byte. */
    const char detail[] = "back\\slash\ttab\rcr\nlf\x01""ctrl\x7F""del\xC3\xA9""hi";
    check(printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_STATE_TRANSITION, detail) == PRINTERCTL_LOG_OK,
          "record with injectable detail succeeds");
    printerctl_log_shutdown(&logger);
    fclose(stream);

    check(count_occurrences(out, "\n") == 1, "escaped detail never introduces a second physical line");
    check(count_occurrences(out, "\t") == 3, "escaped detail never introduces a structural tab beyond the three separators");
    check(strstr(out, "back\\\\slash") != NULL, "backslash is escaped");
    check(strstr(out, "\\ttab") != NULL, "tab is escaped");
    check(strstr(out, "\\rcr") != NULL, "carriage return is escaped");
    check(strstr(out, "\\nlf") != NULL, "line feed is escaped");
    check(strstr(out, "\\x01ctrl") != NULL, "C0 control byte is escaped as \\xHH");
    check(strstr(out, "\\x7Fdel") != NULL, "DEL is escaped as \\x7F");
    check(strstr(out, "\xC3\xA9hi") != NULL, "bytes >= 0x80 are preserved unchanged");

    free(out);
}

static void test_helpers(void)
{
    char *out = NULL;
    size_t out_len = 0;
    FILE *stream = open_memstream(&out, &out_len);

    printerctl_logger_t logger;
    printerctl_log_init(&logger, stream, true);

    check(printerctl_log_serial_tx(&logger, "G1 X10") == PRINTERCTL_LOG_OK, "serial_tx helper succeeds");
    check(printerctl_log_serial_rx(&logger, "ok") == PRINTERCTL_LOG_OK, "serial_rx helper succeeds");
    check(printerctl_log_state_transition(&logger, "idle->printing") == PRINTERCTL_LOG_OK, "state_transition helper succeeds");
    check(printerctl_log_progress(&logger, "50%") == PRINTERCTL_LOG_OK, "progress helper succeeds");
    check(printerctl_log_error(&logger, "boom") == PRINTERCTL_LOG_OK, "error helper succeeds");

    printerctl_log_shutdown(&logger);
    fclose(stream);

    check(strstr(out, "\tDEBUG\tserial_tx\tG1 X10\n") != NULL, "serial_tx logs DEBUG level with the given detail");
    check(strstr(out, "\tDEBUG\tserial_rx\tok\n") != NULL, "serial_rx logs DEBUG level with the given detail");
    check(strstr(out, "\tINFO\tstate_transition\tidle->printing\n") != NULL, "state_transition logs INFO level");
    check(strstr(out, "\tINFO\tprogress\t50%\n") != NULL, "progress logs INFO level");
    check(strstr(out, "\tERROR\terror\tboom\n") != NULL, "error helper logs ERROR level with event 'error'");

    free(out);
}

static void test_init_shutdown_idempotent(void)
{
    printerctl_logger_t logger;
    memset(&logger, 0xAA, sizeof(logger));

    check(printerctl_log_init(&logger, NULL, false) == PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
          "init with a NULL stream is rejected");
    check(logger.initialized == false, "a rejected init leaves the logger marked uninitialized");

    check(printerctl_log_shutdown(&logger) == PRINTERCTL_LOG_OK, "shutdown on a never-initialized logger is a no-op");

    char *out = NULL;
    size_t out_len = 0;
    FILE *stream = open_memstream(&out, &out_len);

    check(printerctl_log_init(&logger, stream, false) == PRINTERCTL_LOG_OK, "init with a valid stream succeeds");
    check(logger.initialized == true, "a successful init marks the logger initialized");

    check(printerctl_log_shutdown(&logger) == PRINTERCTL_LOG_OK, "first shutdown succeeds");
    check(logger.initialized == false, "shutdown marks the logger uninitialized");
    check(printerctl_log_shutdown(&logger) == PRINTERCTL_LOG_OK, "second shutdown is idempotent and still succeeds");

    check(printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_END, NULL) == PRINTERCTL_LOG_ERR_NOT_INITIALIZED,
          "recording after shutdown is rejected as not initialized");

    fclose(stream);
    free(out);
}

static void test_practical_errors(void)
{
    check(printerctl_log_init(NULL, stdout, false) == PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
          "init with a NULL logger is rejected");
    check(printerctl_log_shutdown(NULL) == PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
          "shutdown with a NULL logger is rejected");
    check(printerctl_log_record(NULL, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_START, "x") == PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
          "record with a NULL logger is rejected");

    {
        char *out = NULL;
        size_t out_len = 0;
        FILE *stream = open_memstream(&out, &out_len);
        printerctl_logger_t logger;
        printerctl_log_init(&logger, stream, false);

        check(printerctl_log_record(&logger, (printerctl_log_level_t)99, PRINTERCTL_LOG_EVENT_SESSION_START, "x") == PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
              "an out-of-range level enum value is rejected");
        check(printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, (printerctl_log_event_t)99, "x") == PRINTERCTL_LOG_ERR_INVALID_ARGUMENT,
              "an out-of-range event enum value is rejected");

        check(printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_START, NULL) == PRINTERCTL_LOG_OK,
              "a NULL detail is treated as empty rather than rejected");

        printerctl_log_shutdown(&logger);
        fclose(stream);
        check(out != NULL && strstr(out, "\tINFO\tsession_start\t\n") != NULL, "a NULL detail renders as an empty field");
        free(out);
    }

    {
        /* A read-only stream cannot be written to: this must surface as a practical write error. */
        char path[] = "/tmp/printerctl_logging_test_XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0, "temp file for read-only stream test is created");
        if (fd >= 0) {
            close(fd);
            FILE *ro_stream = fopen(path, "r");
            check(ro_stream != NULL, "temp file reopens for reading");
            if (ro_stream != NULL) {
                printerctl_logger_t logger;
                printerctl_log_init(&logger, ro_stream, false);
                printerctl_log_status_t status =
                    printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_START, "x");
                check(status == PRINTERCTL_LOG_ERR_WRITE, "writing to a read-only stream reports ERR_WRITE");
                printerctl_log_shutdown(&logger);
                fclose(ro_stream);
            }
            remove(path);
        }
    }
}

int main(void)
{
    test_timestamp_format_and_structure();
    test_all_levels_and_error_flush();
    test_verbose_filtering();
    test_escaping_and_injection();
    test_helpers();
    test_init_shutdown_idempotent();
    test_practical_errors();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All logging checks passed\n");
    return EXIT_SUCCESS;
}
