#include "printerctl/cli.h"
#include "printerctl/gcode.h"
#include "printerctl/logging.h"
#include "printerctl/printer.h"
#include "printerctl/version.h"

#include <stdarg.h>
#include <string.h>

static void set_error(printerctl_cli_error_t *out_error, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static void print_usage_hint(FILE *err)
{
    fprintf(err, "Try 'printerctl --help' for more information.\n");
}

static bool starts_with_equals_form(const char *arg, const char *option_name)
{
    size_t name_length = strlen(option_name);
    return strncmp(arg, option_name, name_length) == 0 && arg[name_length] == '=';
}

/* Reports a clearer error when the unsupported "--option=VALUE" form is used. */
static bool reject_equals_form(const char *arg, printerctl_cli_error_t *out_error)
{
    static const char *const option_names[] = {
        "--port", "--baud", "--timeout", "--dry-run", "--verbose", "--config",
    };

    for (size_t i = 0; i < sizeof(option_names) / sizeof(option_names[0]); i++) {
        if (starts_with_equals_form(arg, option_names[i])) {
            set_error(out_error,
                       "option '%s' does not support '=' syntax; use '%s VALUE'",
                       option_names[i], option_names[i]);
            return true;
        }
    }

    return false;
}

static const char *command_name(printerctl_command_t command)
{
    switch (command) {
    case PRINTERCTL_COMMAND_PRINT: return "print";
    case PRINTERCTL_COMMAND_SEND: return "send";
    case PRINTERCTL_COMMAND_HOME: return "home";
    case PRINTERCTL_COMMAND_PAUSE: return "pause";
    case PRINTERCTL_COMMAND_RESUME: return "resume";
    case PRINTERCTL_COMMAND_CANCEL: return "cancel";
    case PRINTERCTL_COMMAND_TEMP: return "temp";
    case PRINTERCTL_COMMAND_POSITION: return "position";
    case PRINTERCTL_COMMAND_INFO: return "info";
    }
    return "unknown";
}

bool printerctl_cli_parse(int argc, char *const argv[],
                           printerctl_cli_request_t *out_request,
                           printerctl_cli_error_t *out_error)
{
    memset(out_request, 0, sizeof(*out_request));
    memset(out_error, 0, sizeof(*out_error));
    printerctl_config_overrides_init(&out_request->overrides);

    if (argc < 2) {
        set_error(out_error, "missing command");
        return false;
    }

    if (strcmp(argv[1], "--help") == 0) {
        if (argc != 2) {
            set_error(out_error, "too many arguments");
            return false;
        }
        out_request->action = PRINTERCTL_CLI_ACTION_HELP;
        return true;
    }

    if (strcmp(argv[1], "--version") == 0) {
        if (argc != 2) {
            set_error(out_error, "too many arguments");
            return false;
        }
        out_request->action = PRINTERCTL_CLI_ACTION_VERSION;
        return true;
    }

    printerctl_config_overrides_t *overrides = &out_request->overrides;
    int i = 1;

    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        const char *opt = argv[i];

        if (strcmp(opt, "--port") == 0) {
            if (overrides->has_port) {
                set_error(out_error, "duplicate option '--port'");
                return false;
            }
            if (i + 1 >= argc) {
                set_error(out_error, "option '--port' requires a value");
                return false;
            }
            const char *value = argv[i + 1];
            if (value[0] == '\0') {
                set_error(out_error, "option '--port' requires a non-empty value");
                return false;
            }
            if (strlen(value) >= sizeof(overrides->port)) {
                set_error(out_error, "value for option '--port' exceeds the maximum length of %d characters",
                           (int)sizeof(overrides->port) - 1);
                return false;
            }
            snprintf(overrides->port, sizeof(overrides->port), "%s", value);
            overrides->has_port = true;
            i += 2;
        } else if (strcmp(opt, "--baud") == 0) {
            if (overrides->has_baud) {
                set_error(out_error, "duplicate option '--baud'");
                return false;
            }
            if (i + 1 >= argc) {
                set_error(out_error, "option '--baud' requires a value");
                return false;
            }
            int parsed;
            if (!printerctl_config_parse_baud(argv[i + 1], &parsed)) {
                set_error(out_error,
                           "invalid value '%s' for option '--baud' (expected an integer between %d and %d)",
                           argv[i + 1], PRINTERCTL_CONFIG_MIN_BAUD, PRINTERCTL_CONFIG_MAX_BAUD);
                return false;
            }
            overrides->baud = parsed;
            overrides->has_baud = true;
            i += 2;
        } else if (strcmp(opt, "--timeout") == 0) {
            if (overrides->has_timeout) {
                set_error(out_error, "duplicate option '--timeout'");
                return false;
            }
            if (i + 1 >= argc) {
                set_error(out_error, "option '--timeout' requires a value");
                return false;
            }
            int parsed;
            if (!printerctl_config_parse_timeout(argv[i + 1], &parsed)) {
                set_error(out_error,
                           "invalid value '%s' for option '--timeout' (expected an integer between %d and %d)",
                           argv[i + 1], PRINTERCTL_CONFIG_MIN_TIMEOUT_MS, PRINTERCTL_CONFIG_MAX_TIMEOUT_MS);
                return false;
            }
            overrides->timeout_ms = parsed;
            overrides->has_timeout = true;
            i += 2;
        } else if (strcmp(opt, "--dry-run") == 0) {
            if (overrides->has_dry_run) {
                set_error(out_error, "duplicate option '--dry-run'");
                return false;
            }
            overrides->dry_run = true;
            overrides->has_dry_run = true;
            i += 1;
        } else if (strcmp(opt, "--verbose") == 0) {
            if (overrides->has_verbose) {
                set_error(out_error, "duplicate option '--verbose'");
                return false;
            }
            overrides->verbose = true;
            overrides->has_verbose = true;
            i += 1;
        } else if (strcmp(opt, "--config") == 0) {
            if (overrides->has_config_path) {
                set_error(out_error, "duplicate option '--config'");
                return false;
            }
            if (i + 1 >= argc) {
                set_error(out_error, "option '--config' requires a value");
                return false;
            }
            const char *value = argv[i + 1];
            if (strlen(value) >= sizeof(overrides->config_path)) {
                set_error(out_error, "value for option '--config' exceeds the maximum length of %d characters",
                           (int)sizeof(overrides->config_path) - 1);
                return false;
            }
            snprintf(overrides->config_path, sizeof(overrides->config_path), "%s", value);
            overrides->has_config_path = true;
            i += 2;
        } else if (reject_equals_form(opt, out_error)) {
            return false;
        } else {
            set_error(out_error, "unrecognized option '%s'", opt);
            return false;
        }
    }

    if (i >= argc) {
        set_error(out_error, "missing command");
        return false;
    }

    const char *requested_command = argv[i];
    i++;

    printerctl_command_t command;
    if (strcmp(requested_command, "print") == 0) {
        command = PRINTERCTL_COMMAND_PRINT;
    } else if (strcmp(requested_command, "send") == 0) {
        command = PRINTERCTL_COMMAND_SEND;
    } else if (strcmp(requested_command, "home") == 0) {
        command = PRINTERCTL_COMMAND_HOME;
    } else if (strcmp(requested_command, "pause") == 0) {
        command = PRINTERCTL_COMMAND_PAUSE;
    } else if (strcmp(requested_command, "resume") == 0) {
        command = PRINTERCTL_COMMAND_RESUME;
    } else if (strcmp(requested_command, "cancel") == 0) {
        command = PRINTERCTL_COMMAND_CANCEL;
    } else if (strcmp(requested_command, "temp") == 0) {
        command = PRINTERCTL_COMMAND_TEMP;
    } else if (strcmp(requested_command, "position") == 0) {
        command = PRINTERCTL_COMMAND_POSITION;
    } else if (strcmp(requested_command, "info") == 0) {
        command = PRINTERCTL_COMMAND_INFO;
    } else {
        set_error(out_error, "unrecognized command '%s'", requested_command);
        return false;
    }

    out_request->command_args.command = command;

    switch (command) {
    case PRINTERCTL_COMMAND_PRINT:
        if (i >= argc) {
            set_error(out_error, "command 'print' requires a FILE argument");
            return false;
        }
        if (argv[i][0] == '\0') {
            set_error(out_error, "command 'print' requires a non-empty FILE argument");
            return false;
        }
        out_request->command_args.print_file = argv[i];
        i++;
        if (i < argc) {
            set_error(out_error, "too many arguments");
            return false;
        }
        break;

    case PRINTERCTL_COMMAND_SEND:
        if (i >= argc) {
            set_error(out_error, "command 'send' requires a COMMAND argument");
            return false;
        }
        if (argv[i][0] == '\0') {
            set_error(out_error, "command 'send' requires a non-empty COMMAND argument");
            return false;
        }
        out_request->command_args.send_command = argv[i];
        i++;
        if (i < argc) {
            set_error(out_error, "too many arguments");
            return false;
        }
        break;

    case PRINTERCTL_COMMAND_HOME:
        if (i < argc) {
            if (strcmp(argv[i], "--i-understand-no-endstops") != 0) {
                set_error(out_error, "unrecognized argument '%s'", argv[i]);
                return false;
            }
            out_request->command_args.home_confirmed = true;
            i++;
            if (i < argc) {
                set_error(out_error, "too many arguments");
                return false;
            }
        }
        break;

    case PRINTERCTL_COMMAND_PAUSE:
    case PRINTERCTL_COMMAND_RESUME:
    case PRINTERCTL_COMMAND_CANCEL:
    case PRINTERCTL_COMMAND_TEMP:
    case PRINTERCTL_COMMAND_POSITION:
    case PRINTERCTL_COMMAND_INFO:
        if (i < argc) {
            set_error(out_error, "too many arguments");
            return false;
        }
        break;
    }

    out_request->action = PRINTERCTL_CLI_ACTION_COMMAND;
    return true;
}

static void print_help(FILE *out)
{
    fprintf(out,
        "Usage: printerctl [GLOBAL_OPTIONS] COMMAND [COMMAND_ARGS]\n"
        "       printerctl --help\n"
        "       printerctl --version\n"
        "\n"
        "Command-line controller for Marlin-based 3D printers over USB serial.\n"
        "'print FILE' streams a G-code file one acknowledged command at a time\n"
        "(Milestone 7); 'temp', 'position', 'info' and 'send' run one command\n"
        "each (Milestone 6). '--dry-run print FILE' parses and normalizes the\n"
        "file without opening any serial device (Milestone 3). 'home', 'pause',\n"
        "'resume' and 'cancel' are not implemented yet (see roadmap.md).\n"
        "\n"
        "Global options (must appear before COMMAND, at most once each):\n"
        "  --port VALUE      Serial device path (default: none)\n"
        "  --baud VALUE      Baud rate, integer %d-%d (default: %d)\n"
        "  --timeout VALUE   Response timeout in milliseconds, integer %d-%d (default: %d)\n"
        "  --dry-run         Never open or write to the serial device\n"
        "  --verbose         Enable verbose output\n"
        "  --config PATH     Load key=value configuration from PATH\n"
        "\n"
        "Commands:\n"
        "  print FILE                          Stream a G-code file, waiting for each\n"
        "                                       command's 'ok' (with --dry-run: parse\n"
        "                                       and report only, no serial access)\n"
        "  send COMMAND                        Send one raw command\n"
        "  home [--i-understand-no-endstops]   Home all axes (see Safety below)\n"
        "  pause                                Pause an active print\n"
        "  resume                               Resume a paused print\n"
        "  cancel                               Cancel an active print\n"
        "  temp                                 Query temperatures\n"
        "  position                             Query current position\n"
        "  info                                 Query printer information\n"
        "\n"
        "Other:\n"
        "  --help     Show this help message and exit\n"
        "  --version  Show version information and exit\n"
        "\n"
        "Safety:\n"
        "  'home' is refused unless --i-understand-no-endstops is given explicitly,\n"
        "  because endstop sensors are not available on this printer. This applies\n"
        "  even in --dry-run mode: home is never confirmed implicitly.\n"
        "  A G-code file containing G28 always fails 'print' (exit code 4);\n"
        "  'print' has no confirmation flag. 'send' refuses G28 the same way,\n"
        "  with no bypass; every other command passes through, including the\n"
        "  operator's explicit emergency stop (M112).\n"
        "\n"
        "Exit codes:\n"
        "  0  success (including --help/--version and a clean dry run)\n"
        "  2  usage error\n"
        "  3  configuration error (including no --port for a printer command)\n"
        "  4  safety policy rejection (G28 in a G-code file or via send)\n"
        "  5  parsed successfully but not yet available in this build\n"
        "  6  G-code file error (unreadable, malformed, oversized line, ...)\n"
        "  7  communication error (cannot open port, I/O failure, timeout)\n"
        "  8  printer error (Error:/Resend: reply, or missing report)\n",
        PRINTERCTL_CONFIG_MIN_BAUD, PRINTERCTL_CONFIG_MAX_BAUD, PRINTERCTL_CONFIG_DEFAULT_BAUD,
        PRINTERCTL_CONFIG_MIN_TIMEOUT_MS, PRINTERCTL_CONFIG_MAX_TIMEOUT_MS, PRINTERCTL_CONFIG_DEFAULT_TIMEOUT_MS);
}

/*
 * Records one ERROR log line for a human-facing error that is reported
 * before configuration (and therefore `config.verbose`) is known. Uses a
 * transient logger scoped to `err` because ERROR records are never
 * filtered by verbosity, so the verbose flag's value cannot matter here.
 * Best-effort: if logging itself cannot be set up, or the record or the
 * shutdown flush fails, the human-readable message already printed by the
 * caller is left as the only record of the failure, and a single brief
 * warning is emitted so the gap is not silent. Shutdown is always attempted,
 * even when recording the error failed, so the stream is still flushed.
 */
static void log_transient_error(FILE *err, const char *detail)
{
    printerctl_logger_t logger;
    if (printerctl_log_init(&logger, err, false) != PRINTERCTL_LOG_OK) {
        fprintf(err, "printerctl: warning: failed to record error log\n");
        return;
    }

    printerctl_log_status_t record_status = printerctl_log_error(&logger, detail);
    printerctl_log_status_t shutdown_status = printerctl_log_shutdown(&logger);

    if (record_status != PRINTERCTL_LOG_OK || shutdown_status != PRINTERCTL_LOG_OK) {
        fprintf(err, "printerctl: warning: failed to record error log\n");
    }
}

/* Emits a brief human warning for a non-OK logging status without hiding the caller's primary result. */
static void warn_on_log_failure(FILE *err, printerctl_log_status_t status, const char *what)
{
    if (status != PRINTERCTL_LOG_OK) {
        fprintf(err, "printerctl: warning: %s\n", what);
    }
}

static const char *gcode_status_name(printerctl_gcode_status_t status)
{
    switch (status) {
    case PRINTERCTL_GCODE_OK: return "ok";
    case PRINTERCTL_GCODE_EOF: return "eof";
    case PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT: return "invalid_argument";
    case PRINTERCTL_GCODE_ERR_NOT_OPEN: return "not_open";
    case PRINTERCTL_GCODE_ERR_OPEN_FAILED: return "open_failed";
    case PRINTERCTL_GCODE_ERR_NOT_REGULAR: return "not_regular";
    case PRINTERCTL_GCODE_ERR_IO: return "io";
    case PRINTERCTL_GCODE_ERR_LINE_TOO_LONG: return "line_too_long";
    case PRINTERCTL_GCODE_ERR_MALFORMED: return "malformed";
    case PRINTERCTL_GCODE_ERR_UNSAFE_G28: return "unsafe_g28";
    case PRINTERCTL_GCODE_ERR_FILE_CHANGED: return "file_changed";
    }
    return "unknown";
}

/* Best-effort close; a close failure is reported but never changes the dry run's primary result. */
static void close_dry_run_reader(printerctl_gcode_reader_t *reader, FILE *err)
{
    printerctl_gcode_error_t close_error;
    if (printerctl_gcode_reader_close(reader, &close_error) != PRINTERCTL_GCODE_OK) {
        fprintf(err, "printerctl: warning: failed to close G-code file cleanly\n");
    }
}

/*
 * Milestone 3 dry-run execution of `print FILE`: parses and normalizes the
 * G-code file via printerctl_gcode without ever opening a serial device,
 * writing each command that would be transmitted to `out` with its source
 * line number and whole-percent progress. The reader is always opened with
 * allow_g28 = false: `print` has no confirmation flag in this milestone,
 * so a file containing G28 always fails the dry run with the safety exit
 * code.
 *
 * Logging follows the Milestone 2 privacy rules: the file path and the
 * G-code command text appear only in human-readable output, never in the
 * structured log stream (a gcode error message may embed the path, so the
 * ERROR record gets a generic status/line detail instead). Per-command
 * `progress` records are DEBUG (verbose-only); one summarizing INFO
 * `progress` record and INFO `state_transition` records for
 * start/completion/failure are always written.
 */
static int run_dry_run_print(const char *path, printerctl_logger_t *logger,
                              bool logger_ready, FILE *out, FILE *err)
{
    printerctl_gcode_reader_t reader;
    printerctl_gcode_error_t gcode_error;

    if (logger_ready) {
        warn_on_log_failure(err,
            printerctl_log_state_transition(logger, "state=dry_run_print_started"),
            "failed to record state_transition log");
    }

    printerctl_gcode_status_t status =
        printerctl_gcode_reader_open(&reader, path, false, &gcode_error);

    uint64_t commands = 0;
    uint64_t lines = 0;
    uint64_t bytes = 0;

    if (status == PRINTERCTL_GCODE_OK) {
        fprintf(out, "printerctl: dry run: parsing '%s' (no serial device is opened)\n", path);

        printerctl_gcode_result_t result;
        for (;;) {
            status = printerctl_gcode_reader_next(&reader, &result, &gcode_error);
            if (status != PRINTERCTL_GCODE_OK) {
                break;
            }

            fprintf(out, "[%3u%%] line %llu: %s\n",
                    result.progress_percent,
                    (unsigned long long)result.line_number,
                    result.command);

            if (logger_ready) {
                char detail[96];
                snprintf(detail, sizeof(detail), "line=%llu command=%llu percent=%u",
                         (unsigned long long)result.line_number,
                         (unsigned long long)result.command_number,
                         result.progress_percent);
                warn_on_log_failure(err,
                    printerctl_log_record(logger, PRINTERCTL_LOG_DEBUG, PRINTERCTL_LOG_EVENT_PROGRESS, detail),
                    "failed to record progress log");
            }
        }

        commands = reader.command_count;
        lines = reader.line_number;
        bytes = reader.bytes_consumed;
    }

    close_dry_run_reader(&reader, err);

    if (status == PRINTERCTL_GCODE_EOF) {
        fprintf(out, "printerctl: dry run complete: %llu command(s) from %llu source line(s), %llu byte(s)\n",
                (unsigned long long)commands,
                (unsigned long long)lines,
                (unsigned long long)bytes);

        if (logger_ready) {
            char detail[96];
            snprintf(detail, sizeof(detail), "commands=%llu lines=%llu percent=100",
                     (unsigned long long)commands,
                     (unsigned long long)lines);
            warn_on_log_failure(err, printerctl_log_progress(logger, detail),
                "failed to record progress log");
            warn_on_log_failure(err,
                printerctl_log_state_transition(logger, "state=dry_run_print_completed status=ok"),
                "failed to record state_transition log");
        }

        return PRINTERCTL_EXIT_OK;
    }

    if (gcode_error.line_number != 0) {
        fprintf(err, "printerctl: dry run failed at line %llu: %s\n",
                (unsigned long long)gcode_error.line_number, gcode_error.message);
    } else {
        fprintf(err, "printerctl: dry run failed: %s\n", gcode_error.message);
    }

    if (logger_ready) {
        char detail[96];
        snprintf(detail, sizeof(detail), "dry_run_print status=%s line=%llu",
                 gcode_status_name(status),
                 (unsigned long long)gcode_error.line_number);
        warn_on_log_failure(err, printerctl_log_error(logger, detail),
            "failed to record error log");
        warn_on_log_failure(err,
            printerctl_log_state_transition(logger, "state=dry_run_print_failed"),
            "failed to record state_transition log");
    }

    return status == PRINTERCTL_GCODE_ERR_UNSAFE_G28
        ? PRINTERCTL_EXIT_SAFETY_ERROR
        : PRINTERCTL_EXIT_GCODE_ERROR;
}

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
    }
    return "unknown";
}

static int printer_status_to_exit_code(printerctl_printer_status_t status)
{
    switch (status) {
    case PRINTERCTL_PRINTER_OK:
        return PRINTERCTL_EXIT_OK;
    case PRINTERCTL_PRINTER_ERR_NO_PORT:
        return PRINTERCTL_EXIT_CONFIG_ERROR;
    case PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND:
        return PRINTERCTL_EXIT_SAFETY_ERROR;
    case PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT:
    case PRINTERCTL_PRINTER_ERR_NOT_CONNECTED:
        return PRINTERCTL_EXIT_USAGE_ERROR;
    case PRINTERCTL_PRINTER_ERR_SERIAL:
    case PRINTERCTL_PRINTER_ERR_TIMEOUT:
        return PRINTERCTL_EXIT_COMM_ERROR;
    case PRINTERCTL_PRINTER_ERR_PRINTER_ERROR:
    case PRINTERCTL_PRINTER_ERR_RESEND:
    case PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE:
        return PRINTERCTL_EXIT_PRINTER_ERROR;
    case PRINTERCTL_PRINTER_ERR_GCODE:
        return PRINTERCTL_EXIT_GCODE_ERROR;
    }
    return PRINTERCTL_EXIT_COMM_ERROR;
}

/*
 * Reports a failed printer exchange: the human-readable message (which
 * may embed the port path, so it stays out of the structured log) to
 * `err`, and one generic ERROR record naming only the command and the
 * status, matching the Milestone 3 pattern for gcode failures.
 */
static void report_printer_failure(FILE *err, printerctl_logger_t *logger, bool logger_ready,
                                    const char *command_name_text,
                                    printerctl_printer_status_t status,
                                    const printerctl_printer_error_t *error)
{
    fprintf(err, "printerctl: %s\n", error->message);

    if (logger_ready) {
        char detail[96];
        snprintf(detail, sizeof(detail), "command=%s status=%s",
                 command_name_text, printer_status_name(status));
        warn_on_log_failure(err, printerctl_log_error(logger, detail),
            "failed to record error log");
    }
}

/* stdout renderer for `send`: every received line, prefixed to read as an exchange. */
static void render_received_line(const char *line, void *context)
{
    FILE *out = context;
    fprintf(out, "< %s\n", line);
}

/*
 * Milestone 6 execution of temp/position/info/send: connect, run the
 * one exchange, render, disconnect. The session lives entirely inside
 * this call; the port is always closed before returning, on success and
 * on failure.
 */
static int run_single_command(const printerctl_command_args_t *args,
                               const printerctl_config_t *config,
                               printerctl_logger_t *logger, bool logger_ready,
                               FILE *out, FILE *err)
{
    const char *name = command_name(args->command);

    if (args->command == PRINTERCTL_COMMAND_SEND &&
        printerctl_gcode_command_is_unsafe_g28(args->send_command)) {
        static const char *const safety_message =
            "refusing to send G28: homing is disabled because endstop sensors "
            "are not available (see the safety policy in Agents.md)";
        fprintf(err, "printerctl: %s\n", safety_message);
        if (logger_ready) {
            warn_on_log_failure(err, printerctl_log_error(logger, safety_message),
                "failed to record error log");
        }
        return PRINTERCTL_EXIT_SAFETY_ERROR;
    }

    printerctl_printer_session_t session;
    printerctl_printer_error_t error;

    printerctl_printer_status_t status =
        printerctl_printer_connect(&session, config, logger_ready ? logger : NULL, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        report_printer_failure(err, logger, logger_ready, name, status, &error);
        return printer_status_to_exit_code(status);
    }

    switch (args->command) {
    case PRINTERCTL_COMMAND_TEMP: {
        printerctl_marlin_temperature_report_t report;
        status = printerctl_printer_query_temperature(&session, &report, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            fprintf(out, "printerctl: hotend %.2f C (target %.2f C)",
                    report.hotend_actual, report.hotend_target);
            if (report.has_bed) {
                fprintf(out, ", bed %.2f C (target %.2f C)",
                        report.bed_actual, report.bed_target);
            }
            fprintf(out, "\n");
        }
        break;
    }

    case PRINTERCTL_COMMAND_POSITION: {
        printerctl_marlin_position_report_t report;
        status = printerctl_printer_query_position(&session, &report, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            fprintf(out, "printerctl: position X=%.2f Y=%.2f Z=%.2f",
                    report.x, report.y, report.z);
            if (report.has_e) {
                fprintf(out, " E=%.2f", report.e);
            }
            fprintf(out, "\n");
        }
        break;
    }

    case PRINTERCTL_COMMAND_INFO: {
        printerctl_marlin_firmware_info_t info;
        status = printerctl_printer_query_info(&session, &info, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            fprintf(out, "printerctl: firmware: %s\n", info.firmware_name);
            if (info.machine_type[0] != '\0') {
                fprintf(out, "printerctl: machine: %s\n", info.machine_type);
            }
            if (info.protocol_version[0] != '\0') {
                fprintf(out, "printerctl: protocol: %s\n", info.protocol_version);
            }
            if (info.has_extruder_count) {
                fprintf(out, "printerctl: extruders: %d\n", info.extruder_count);
            }
            if (info.uuid[0] != '\0') {
                fprintf(out, "printerctl: uuid: %s\n", info.uuid);
            }
        }
        break;
    }

    case PRINTERCTL_COMMAND_SEND:
    default:
        fprintf(out, "> %s\n", args->send_command);
        status = printerctl_printer_send(&session, args->send_command,
                                          render_received_line, out, &error);
        break;
    }

    if (status != PRINTERCTL_PRINTER_OK) {
        report_printer_failure(err, logger, logger_ready, name, status, &error);
    }

    printerctl_printer_error_t disconnect_error;
    if (printerctl_printer_disconnect(&session, &disconnect_error) != PRINTERCTL_PRINTER_OK) {
        fprintf(err, "printerctl: warning: failed to close the serial port cleanly\n");
    }

    return printer_status_to_exit_code(status);
}

/* Renders a progress line whenever the whole-percent value changes. */
typedef struct {
    FILE *out;
    unsigned last_percent;
} print_progress_renderer_t;

static void render_print_progress(const printerctl_gcode_result_t *result, void *context)
{
    print_progress_renderer_t *renderer = context;
    if (result->progress_percent != renderer->last_percent) {
        fprintf(renderer->out, "printerctl: printing: %u%% (line %llu, %llu command(s) acknowledged)\n",
                result->progress_percent,
                (unsigned long long)result->line_number,
                (unsigned long long)result->command_number);
        renderer->last_percent = result->progress_percent;
    }
}

/*
 * Milestone 7 execution of `print FILE` (without --dry-run): connect,
 * stream the file one acknowledged command at a time, render progress
 * on every whole-percent change and a session summary at the end, then
 * disconnect. The summary is rendered on success and on failure alike,
 * naming the file, the final state and the last acknowledged line.
 */
static int run_print_command(const printerctl_command_args_t *args,
                              const printerctl_config_t *config,
                              printerctl_logger_t *logger, bool logger_ready,
                              FILE *out, FILE *err)
{
    printerctl_printer_session_t session;
    printerctl_printer_error_t error;

    printerctl_printer_status_t status =
        printerctl_printer_connect(&session, config, logger_ready ? logger : NULL, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        report_printer_failure(err, logger, logger_ready, "print", status, &error);
        return printer_status_to_exit_code(status);
    }

    fprintf(out, "printerctl: printing '%s'\n", args->print_file);

    print_progress_renderer_t renderer = {.out = out, .last_percent = (unsigned)-1};
    printerctl_print_summary_t summary;
    status = printerctl_printer_print_file(&session, args->print_file,
                                            render_print_progress, &renderer,
                                            &summary, &error);

    if (status != PRINTERCTL_PRINTER_OK) {
        report_printer_failure(err, logger, logger_ready, "print", status, &error);
    }

    fprintf(out,
        "printerctl: print %s: '%s' — %llu command(s) sent, %llu acknowledged, "
        "last acknowledged line %llu, %u%%\n",
        summary.state == PRINTERCTL_PRINT_COMPLETED ? "completed" : "failed",
        args->print_file,
        (unsigned long long)summary.commands_sent,
        (unsigned long long)summary.commands_acknowledged,
        (unsigned long long)summary.last_acknowledged_line,
        summary.progress_percent);

    printerctl_printer_error_t disconnect_error;
    if (printerctl_printer_disconnect(&session, &disconnect_error) != PRINTERCTL_PRINTER_OK) {
        fprintf(err, "printerctl: warning: failed to close the serial port cleanly\n");
    }

    return printer_status_to_exit_code(status);
}

/* Marlin command a query maps to, for the dry-run "would send" report. */
static const char *single_command_tx_line(const printerctl_command_args_t *args)
{
    switch (args->command) {
    case PRINTERCTL_COMMAND_TEMP: return "M105";
    case PRINTERCTL_COMMAND_POSITION: return "M114";
    case PRINTERCTL_COMMAND_INFO: return "M115";
    case PRINTERCTL_COMMAND_SEND: return args->send_command;
    default: return NULL;
    }
}

int printerctl_cli_run(int argc, char *const argv[], FILE *out, FILE *err)
{
    printerctl_cli_request_t request;
    printerctl_cli_error_t error;

    if (!printerctl_cli_parse(argc, argv, &request, &error)) {
        fprintf(err, "printerctl: %s\n", error.message);
        print_usage_hint(err);
        log_transient_error(err, error.message);
        return PRINTERCTL_EXIT_USAGE_ERROR;
    }

    if (request.action == PRINTERCTL_CLI_ACTION_HELP) {
        print_help(out);
        return PRINTERCTL_EXIT_OK;
    }

    if (request.action == PRINTERCTL_CLI_ACTION_VERSION) {
        fprintf(out, "printerctl %s\n", PRINTERCTL_VERSION_STRING);
        return PRINTERCTL_EXIT_OK;
    }

    printerctl_config_t config;
    printerctl_config_error_t config_error;
    printerctl_config_status_t status =
        printerctl_config_resolve(&request.overrides, &config, &config_error);
    if (status != PRINTERCTL_CONFIG_OK) {
        fprintf(err, "printerctl: configuration error: %s\n", config_error.message);
        /* config_error.message may embed --config's path; the structured log
         * uses a generic detail so the path is never written to the TSV
         * stream, only to the human-readable line above. */
        log_transient_error(err, "configuration error");
        return PRINTERCTL_EXIT_CONFIG_ERROR;
    }

    if (request.command_args.command == PRINTERCTL_COMMAND_HOME && !request.command_args.home_confirmed) {
        static const char *const safety_message =
            "refusing to home without --i-understand-no-endstops "
            "(endstop sensors are not available; see Agents.md safety policy)";
        fprintf(err, "printerctl: %s\n", safety_message);
        log_transient_error(err, safety_message);
        return PRINTERCTL_EXIT_SAFETY_ERROR;
    }

    char command_detail[64];
    snprintf(command_detail, sizeof(command_detail), "command=%s",
             command_name(request.command_args.command));

    printerctl_logger_t logger;
    printerctl_log_status_t log_init_status = printerctl_log_init(&logger, err, config.verbose);
    bool logger_ready = (log_init_status == PRINTERCTL_LOG_OK);
    if (!logger_ready) {
        fprintf(err, "printerctl: warning: logging unavailable\n");
    }

    if (logger_ready) {
        warn_on_log_failure(err,
            printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_START, NULL),
            "failed to record session_start log");

        char summary[160];
        snprintf(summary, sizeof(summary),
                 "port_set=%s config_file_set=%s baud=%d timeout_ms=%d dry_run=%s verbose=%s",
                 request.overrides.has_port ? "true" : "false",
                 request.overrides.has_config_path ? "true" : "false",
                 config.baud, config.timeout_ms,
                 config.dry_run ? "true" : "false",
                 config.verbose ? "true" : "false");
        warn_on_log_failure(err,
            printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_CONFIG_SUMMARY, summary),
            "failed to record config_summary log");

        warn_on_log_failure(err,
            printerctl_log_record(&logger, PRINTERCTL_LOG_DEBUG, PRINTERCTL_LOG_EVENT_COMMAND_PARSED, command_detail),
            "failed to record command_parsed log");
    }

    bool is_single_command =
        request.command_args.command == PRINTERCTL_COMMAND_TEMP ||
        request.command_args.command == PRINTERCTL_COMMAND_POSITION ||
        request.command_args.command == PRINTERCTL_COMMAND_INFO ||
        request.command_args.command == PRINTERCTL_COMMAND_SEND;

    int exit_code;

    if (request.command_args.command == PRINTERCTL_COMMAND_PRINT && config.dry_run) {
        exit_code = run_dry_run_print(request.command_args.print_file, &logger, logger_ready, out, err);
    } else if (request.command_args.command == PRINTERCTL_COMMAND_PRINT) {
        exit_code = run_print_command(&request.command_args, &config, &logger, logger_ready, out, err);
    } else if (is_single_command && config.dry_run) {
        const char *tx_line = single_command_tx_line(&request.command_args);

        /* The safety policy holds even when nothing would be sent: a
         * G28 through `send` is rejected in dry-run mode too, exactly
         * like `home` is never confirmed implicitly by --dry-run. */
        if (request.command_args.command == PRINTERCTL_COMMAND_SEND &&
            printerctl_gcode_command_is_unsafe_g28(tx_line)) {
            static const char *const safety_message =
                "refusing to send G28: homing is disabled because endstop sensors "
                "are not available (see the safety policy in Agents.md)";
            fprintf(err, "printerctl: %s\n", safety_message);
            if (logger_ready) {
                warn_on_log_failure(err, printerctl_log_error(&logger, safety_message),
                    "failed to record error log");
            }
            exit_code = PRINTERCTL_EXIT_SAFETY_ERROR;
        } else {
            fprintf(out,
                "printerctl: dry run: would send '%s' and wait for the response "
                "(no serial device is opened)\n", tx_line);
            exit_code = PRINTERCTL_EXIT_OK;
        }
    } else if (is_single_command) {
        exit_code = run_single_command(&request.command_args, &config, &logger, logger_ready, out, err);
    } else {
        fprintf(out,
            "printerctl: command '%s' parsed successfully but is not yet implemented "
            "(port=%s baud=%d timeout=%dms dry_run=%s verbose=%s)\n",
            command_name(request.command_args.command),
            config.port[0] != '\0' ? config.port : "(none)",
            config.baud, config.timeout_ms,
            config.dry_run ? "true" : "false",
            config.verbose ? "true" : "false");

        if (logger_ready) {
            warn_on_log_failure(err,
                printerctl_log_record(&logger, PRINTERCTL_LOG_WARNING, PRINTERCTL_LOG_EVENT_COMMAND_UNAVAILABLE, command_detail),
                "failed to record command_unavailable log");
        }

        exit_code = PRINTERCTL_EXIT_UNAVAILABLE;
    }

    if (logger_ready) {
        warn_on_log_failure(err,
            printerctl_log_record(&logger, PRINTERCTL_LOG_INFO, PRINTERCTL_LOG_EVENT_SESSION_END, NULL),
            "failed to record session_end log");

        warn_on_log_failure(err, printerctl_log_shutdown(&logger), "failed to shut down logging cleanly");
    }

    return exit_code;
}
