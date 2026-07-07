#include "printerctl/cli.h"
#include "printerctl/gcode.h"
#include "printerctl/logging.h"
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
        "This build parses commands, resolves configuration, and logs activity.\n"
        "'--dry-run print FILE' parses and normalizes the G-code file without\n"
        "opening any serial device (Milestone 3); no other command performs a\n"
        "serial or hardware action yet (see roadmap.md).\n"
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
        "  print FILE                          Stream a G-code file (with --dry-run:\n"
        "                                       parse and report, no serial access)\n"
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
        "  'print' has no confirmation flag.\n"
        "\n"
        "Exit codes:\n"
        "  0  success (including --help/--version and a clean dry run)\n"
        "  2  usage error\n"
        "  3  configuration error\n"
        "  4  safety policy rejection (including G28 found in a G-code file)\n"
        "  5  parsed successfully but not yet available in this build\n"
        "  6  G-code file error (unreadable, malformed, oversized line, ...)\n",
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

    int exit_code;

    if (request.command_args.command == PRINTERCTL_COMMAND_PRINT && config.dry_run) {
        exit_code = run_dry_run_print(request.command_args.print_file, &logger, logger_ready, out, err);
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
