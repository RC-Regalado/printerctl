/* POSIX.1-2008 for sigaction(), used by the Milestone 8 print session controls. */
#define _POSIX_C_SOURCE 200809L

#include "printerctl/cli.h"
#include "printerctl/core/print_engine.h"
#include "printerctl/gcode.h"
#include "printerctl/logging.h"
#include "printerctl/printer.h"
#include "printerctl/record.h"
#include "printerctl/version.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

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
    case PRINTERCTL_COMMAND_POWER_LOW_BATTERY: return "power";
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
    } else if (strcmp(requested_command, "power") == 0) {
        command = PRINTERCTL_COMMAND_POWER_LOW_BATTERY;
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
        out_request->command_args.home_axis = PRINTERCTL_HOME_ALL;
        if (i < argc) {
            if (strcmp(argv[i], "X") == 0) {
                out_request->command_args.home_axis = PRINTERCTL_HOME_X;
            } else if (strcmp(argv[i], "Y") == 0) {
                out_request->command_args.home_axis = PRINTERCTL_HOME_Y;
            } else if (strcmp(argv[i], "Z") == 0) {
                out_request->command_args.home_axis = PRINTERCTL_HOME_Z;
            } else {
                set_error(out_error,
                           "unrecognized argument '%s' for 'home' (expected 'X', 'Y' or 'Z')", argv[i]);
                return false;
            }
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

    case PRINTERCTL_COMMAND_POWER_LOW_BATTERY:
        if (i >= argc) {
            set_error(out_error, "command 'power' requires a subcommand (expected 'low-battery')");
            return false;
        }
        if (strcmp(argv[i], "low-battery") != 0) {
            set_error(out_error, "unrecognized subcommand '%s' for 'power' (expected 'low-battery')",
                       argv[i]);
            return false;
        }
        i++;
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
        "each (Milestone 6). 'pause', 'resume' and 'cancel' control a print\n"
        "running from the printer's SD card (Milestone 8; see Print session\n"
        "control below for host streams). 'power low-battery' notifies a\n"
        "running print of an external low-battery condition (Milestone 10;\n"
        "meant to be run by an APCUPSD script, see scripts/apcupsd/). 'home'\n"
        "(and its single-axis variants) homes the printer now that min\n"
        "end-stops are enabled on all three axes (see Safety below).\n"
        "'--dry-run print FILE' parses and normalizes the file without opening\n"
        "any serial device (Milestone 3).\n"
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
        "  home [X|Y|Z]                        Home all axes, or a single one\n"
        "  pause                                Pause the printer's SD print (M25)\n"
        "  resume                               Resume the printer's SD print (M24)\n"
        "  cancel                               Abort the printer's SD print (M524)\n"
        "  temp                                 Query temperatures\n"
        "  position                             Query current position\n"
        "  info                                 Query printer information\n"
        "  power low-battery                    Notify a running print of a power event\n"
        "\n"
        "Print session control:\n"
        "  While 'print FILE' streams, signal the printerctl process to control\n"
        "  the session (e.g. 'kill -USR1 PID' from another terminal):\n"
        "    SIGUSR1          pause after the current command is acknowledged\n"
        "    SIGUSR2          resume from the next unsent command\n"
        "    SIGINT/SIGTERM   cancel gracefully (Ctrl+C; summary still printed)\n"
        "    SIGPWR           cancel gracefully as a power-failure/low-battery\n"
        "                     event, not an operator cancel (see 'power' below)\n"
        "  Nothing is sent to the printer on pause/resume/cancel/power-cancel of\n"
        "  a host stream: it never moves, heats or parks on its own. The\n"
        "  'pause', 'resume' and 'cancel' commands instead talk to a print\n"
        "  running from the printer's SD card; they cannot reach a host stream,\n"
        "  whose serial port is held exclusively while it runs.\n"
        "\n",
        PRINTERCTL_CONFIG_MIN_BAUD, PRINTERCTL_CONFIG_MAX_BAUD, PRINTERCTL_CONFIG_DEFAULT_BAUD,
        PRINTERCTL_CONFIG_MIN_TIMEOUT_MS, PRINTERCTL_CONFIG_MAX_TIMEOUT_MS, PRINTERCTL_CONFIG_DEFAULT_TIMEOUT_MS);

    fprintf(out,
        "Power events:\n"
        "  'power low-battery' is meant to be run by an APCUPSD script (see\n"
        "  scripts/apcupsd/printerctl-low-battery.sh) when the UPS reports a\n"
        "  low battery. It never opens a serial device itself: it looks up the\n"
        "  process currently streaming a print (if any) and delivers SIGPWR to\n"
        "  it. No active print is not an error, and running it more than once\n"
        "  is safe. Power restoration never triggers automatic resume; a\n"
        "  cancelled session record is the operator's cue to restart manually.\n"
        "\n"
        "Other:\n"
        "  --help     Show this help message and exit\n"
        "  --version  Show version information and exit\n"
        "\n"
        "Safety:\n"
        "  Min end-stops are enabled on all three axes, so 'home' (and 'home\n"
        "  X'/'home Y'/'home Z') needs no confirmation flag and runs like\n"
        "  'temp'/'position'/'info': only a configured --port is required.\n"
        "  G28 remains disabled everywhere else: a G-code file containing G28\n"
        "  always fails 'print' (exit code 4; 'print' has no confirmation\n"
        "  flag), and 'send' refuses G28 the same way, with no bypass --\n"
        "  homing is only ever available through the dedicated 'home' command,\n"
        "  never an unexpected side effect of a raw command or a sliced file.\n"
        "  Every other command passes through 'send', including the\n"
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
        "  8  printer error (Error:/Resend: reply, or missing report)\n"
        "  9  print cancelled by the operator (SIGINT/SIGTERM during 'print')\n"
        " 10  print cancelled by a power-failure/low-battery signal (SIGPWR)\n");
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
    case PRINTERCTL_PRINTER_CANCELLED: return "cancelled";
    case PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE: return "cancelled_power_failure";
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
    case PRINTERCTL_PRINTER_CANCELLED:
        return PRINTERCTL_EXIT_PRINT_CANCELLED;
    case PRINTERCTL_PRINTER_CANCELLED_POWER_FAILURE:
        return PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE;
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
/*
 * Min end-stops are enabled on all three axes, so `home` maps to a
 * fixed, safe G28 variant exactly like temp/position/info map to their
 * fixed queries -- never operator-typed text, so it does not go through
 * printerctl_printer_send()'s G28 refusal (which still applies to
 * `send` and to G-code files, the only remaining paths where an
 * unconfirmed G28 could be unexpected).
 */
static const char *home_axis_command(printerctl_home_axis_t axis)
{
    switch (axis) {
    case PRINTERCTL_HOME_X: return "G28 X";
    case PRINTERCTL_HOME_Y: return "G28 Y";
    case PRINTERCTL_HOME_Z: return "G28 Z";
    case PRINTERCTL_HOME_ALL:
    default: return "G28";
    }
}

static const char *home_axis_name(printerctl_home_axis_t axis)
{
    switch (axis) {
    case PRINTERCTL_HOME_X: return "X axis";
    case PRINTERCTL_HOME_Y: return "Y axis";
    case PRINTERCTL_HOME_Z: return "Z axis";
    case PRINTERCTL_HOME_ALL:
    default: return "all axes";
    }
}

static int run_single_command(const printerctl_command_args_t *args,
                               const printerctl_config_t *config,
                               printerctl_logger_t *logger, bool logger_ready,
                               FILE *out, FILE *err)
{
    const char *name = command_name(args->command);

    if (args->command == PRINTERCTL_COMMAND_SEND &&
        printerctl_gcode_command_is_unsafe_g28(args->send_command)) {
        static const char *const safety_message =
            "refusing to send G28: homing is only available through the "
            "'home' command, not 'send' (see the safety policy in Agents.md)";
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

    case PRINTERCTL_COMMAND_HOME: {
        const char *command = home_axis_command(args->home_axis);
        fprintf(out, "> %s\n", command);
        status = printerctl_printer_execute(&session, command, render_received_line, out, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            fprintf(out, "printerctl: home complete: %s (%s acknowledged)\n",
                    home_axis_name(args->home_axis), command);
        }
        break;
    }

    /* Milestone 8 SD-print controls: explicit operator commands sent to
     * a print running from the printer's SD card. A host stream is
     * controlled with signals inside its own process, never from here —
     * its serial port is held exclusively while it runs. */
    case PRINTERCTL_COMMAND_PAUSE:
        fprintf(out, "> M25\n");
        status = printerctl_printer_pause_sd_print(&session, render_received_line, out, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            fprintf(out, "printerctl: SD print pause requested (M25 acknowledged)\n");
        }
        break;

    case PRINTERCTL_COMMAND_RESUME:
        fprintf(out, "> M24\n");
        status = printerctl_printer_resume_sd_print(&session, render_received_line, out, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            fprintf(out, "printerctl: SD print resume requested (M24 acknowledged)\n");
        }
        break;

    case PRINTERCTL_COMMAND_CANCEL:
        fprintf(out, "> M524\n");
        status = printerctl_printer_cancel_sd_print(&session, render_received_line, out, &error);
        if (status == PRINTERCTL_PRINTER_OK) {
            fprintf(out, "printerctl: SD print abort requested (M524 acknowledged)\n");
        }
        break;

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

/*
 * Milestone 12: the CLI's client adapter over printerctl_print_engine_t.
 * Every operator-facing message the engine used to fprintf() directly
 * (progress, pause/resume, the power-failure notice, the marker
 * warning, the record outcome, the terminal summary) now arrives as a
 * printerctl_print_event_t; this is the one place that turns those back
 * into the exact same text run_print_command() used to print inline, so
 * `print`'s output is unchanged even though the lifecycle that produces
 * it now lives in `core/print_engine`.
 */
typedef struct {
    FILE *out;
    FILE *err;
    printerctl_logger_t *logger;
    bool logger_ready;
    const char *file;
    unsigned last_percent; /* PROGRESS dedup, same rule the old renderer used. */
} print_event_context_t;

static void render_print_event(const printerctl_print_event_t *event, void *context)
{
    print_event_context_t *ctx = context;

    switch (event->type) {
    case PRINTERCTL_PRINT_EVENT_STARTED:
        fprintf(ctx->out, "printerctl: printing '%s' — SIGUSR1 pauses, SIGINT cancels\n",
                event->path);
        fflush(ctx->out);
        break;

    case PRINTERCTL_PRINT_EVENT_PROGRESS:
        if (event->progress->progress_percent != ctx->last_percent) {
            fprintf(ctx->out,
                    "printerctl: printing: %u%% (line %llu, %llu command(s) acknowledged)\n",
                    event->progress->progress_percent,
                    (unsigned long long)event->progress->line_number,
                    (unsigned long long)event->progress->command_number);
            fflush(ctx->out);
            ctx->last_percent = event->progress->progress_percent;
        }
        break;

    case PRINTERCTL_PRINT_EVENT_PAUSED:
        fprintf(ctx->out, "printerctl: print paused — SIGUSR2 resumes, SIGINT cancels\n");
        fflush(ctx->out);
        break;

    case PRINTERCTL_PRINT_EVENT_RESUMED:
        fprintf(ctx->out, "printerctl: print resumed\n");
        fflush(ctx->out);
        break;

    case PRINTERCTL_PRINT_EVENT_CANCELLING_POWER_FAILURE:
        fprintf(ctx->out,
                "printerctl: print cancelling: power-failure/low-battery signal received (SIGPWR)\n");
        fflush(ctx->out);
        break;

    case PRINTERCTL_PRINT_EVENT_MARKER_WARNING:
        fprintf(ctx->err,
            "printerctl: warning: cannot record the active-print marker (%s); an "
            "APCUPSD low-battery notification will not be able to find this print\n",
            event->message);
        break;

    case PRINTERCTL_PRINT_EVENT_RECORD_WRITTEN:
        fprintf(ctx->out, "printerctl: session record written to '%s'\n", event->path);
        fflush(ctx->out);
        break;

    case PRINTERCTL_PRINT_EVENT_RECORD_FAILED:
        fprintf(ctx->err, "printerctl: warning: no session record written: %s\n", event->message);
        break;

    case PRINTERCTL_PRINT_EVENT_COMPLETED:
    case PRINTERCTL_PRINT_EVENT_CANCELLED:
    case PRINTERCTL_PRINT_EVENT_FAILED: {
        /* A cancel is the operator's (or, for a power failure, the
         * APCUPSD script's) own request carried out, not a failure: no
         * error report, no ERROR record -- the summary and the engine's
         * own state_transition logging (inherited from
         * printerctl_printer_print_file()) tell the story. */
        if (event->type == PRINTERCTL_PRINT_EVENT_FAILED) {
            report_printer_failure(ctx->err, ctx->logger, ctx->logger_ready, "print",
                                    event->summary->status, event->error);
        }

        const char *state_name = "failed";
        if (event->summary->state == PRINTERCTL_PRINT_COMPLETED) {
            state_name = "completed";
        } else if (event->summary->state == PRINTERCTL_PRINT_CANCELLED) {
            state_name = "cancelled";
        } else if (event->summary->state == PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE) {
            state_name = "cancelled_power_failure";
        }

        fprintf(ctx->out,
            "printerctl: print %s: '%s' — %llu command(s) sent, %llu acknowledged, "
            "last acknowledged line %llu, %u%%\n",
            state_name,
            ctx->file,
            (unsigned long long)event->summary->commands_sent,
            (unsigned long long)event->summary->commands_acknowledged,
            (unsigned long long)event->summary->last_acknowledged_line,
            event->summary->progress_percent);
        fflush(ctx->out);
        break;
    }
    }
}

/*
 * Milestone 12: `print FILE` control signals now feed
 * printerctl_print_engine_t directly instead of file-scope flags that a
 * CLI-owned poll function used to translate. Each handler is a single
 * call to a printerctl_print_engine_request_*() setter -- itself a
 * single sig_atomic_t assignment (see print_engine.h) -- so this stays
 * exactly as async-signal-safe as the flags it replaces. `g_active_engine`
 * is only non-NULL for the duration of run_print_command()'s call to
 * printerctl_print_engine_run(), the same window the old sigaction
 * install/restore pair used to bound.
 *
 *   SIGUSR1          pause after the current command's acknowledgement
 *   SIGUSR2          resume from the next unsent command
 *   SIGINT/SIGTERM   cancel gracefully (summary rendered, exit code 9)
 *   SIGPWR           power-failure cancel (delivered by `printerctl
 *                    power low-battery`, not the operator directly);
 *                    takes priority over an unresolved pause -- see
 *                    print_engine.c's on_control().
 */
static printerctl_print_engine_t *g_active_engine;

static void handle_print_cancel_signal(int signum)
{
    (void)signum;
    if (g_active_engine != NULL) {
        printerctl_print_engine_request_cancel(g_active_engine);
    }
}

static void handle_print_pause_signal(int signum)
{
    (void)signum;
    if (g_active_engine != NULL) {
        printerctl_print_engine_request_pause(g_active_engine);
    }
}

static void handle_print_resume_signal(int signum)
{
    (void)signum;
    if (g_active_engine != NULL) {
        printerctl_print_engine_request_resume(g_active_engine);
    }
}

static void handle_print_power_failure_signal(int signum)
{
    (void)signum;
    if (g_active_engine != NULL) {
        printerctl_print_engine_request_cancel_power_failure(g_active_engine);
    }
}

/*
 * Milestone 7/8/12 execution of `print FILE` (without --dry-run): the
 * CLI is now a thin client adapter over printerctl_print_engine_t,
 * which owns the actual lifecycle (connect, publish the active-print
 * marker, stream, interrupted-session record, disconnect) -- see
 * core/print_engine.c and docs/design/milestone-12-print-engine.md.
 * This function only wires argv/config to the engine, installs the
 * operator's session-control signal handlers for the duration of the
 * stream (restored before returning), and renders every event the
 * engine emits through render_print_event() so `print`'s output stays
 * exactly what it was before the extraction.
 */
static int run_print_command(const printerctl_command_args_t *args,
                              const printerctl_config_t *config,
                              printerctl_logger_t *logger, bool logger_ready,
                              FILE *out, FILE *err)
{
    printerctl_print_engine_t engine;
    printerctl_printer_error_t error;

    print_event_context_t event_context = {
        .out = out,
        .err = err,
        .logger = logger,
        .logger_ready = logger_ready,
        .file = args->print_file,
        .last_percent = (unsigned)-1,
    };

    printerctl_printer_status_t status = printerctl_print_engine_connect(
        &engine, config, logger, logger_ready, render_print_event, &event_context, &error);
    if (status != PRINTERCTL_PRINTER_OK) {
        report_printer_failure(err, logger, logger_ready, "print", status, &error);
        return printer_status_to_exit_code(status);
    }

    g_active_engine = &engine;

    struct sigaction control_action;
    memset(&control_action, 0, sizeof(control_action));
    sigemptyset(&control_action.sa_mask);

    struct sigaction old_int, old_term, old_usr1, old_usr2, old_pwr;
    control_action.sa_handler = handle_print_cancel_signal;
    sigaction(SIGINT, &control_action, &old_int);
    sigaction(SIGTERM, &control_action, &old_term);
    control_action.sa_handler = handle_print_pause_signal;
    sigaction(SIGUSR1, &control_action, &old_usr1);
    control_action.sa_handler = handle_print_resume_signal;
    sigaction(SIGUSR2, &control_action, &old_usr2);
    control_action.sa_handler = handle_print_power_failure_signal;
    sigaction(SIGPWR, &control_action, &old_pwr);

    printerctl_print_summary_t summary;
    status = printerctl_print_engine_run(&engine, args->print_file, &summary, &error);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGUSR1, &old_usr1, NULL);
    sigaction(SIGUSR2, &old_usr2, NULL);
    sigaction(SIGPWR, &old_pwr, NULL);
    g_active_engine = NULL;

    printerctl_printer_error_t disconnect_error;
    if (printerctl_print_engine_disconnect(&engine, &disconnect_error) != PRINTERCTL_PRINTER_OK) {
        fprintf(err, "printerctl: warning: failed to close the serial port cleanly\n");
    }

    return printer_status_to_exit_code(status);
}

/*
 * Milestone 10: `power low-battery` is the command an external notifier
 * (the APCUPSD low-battery script) runs to tell printerctl that the
 * host may be about to lose power. It never touches the serial port
 * itself: it looks up the PID of the printerctl process currently
 * streaming a print (published by printerctl_print_engine_connect() to
 * the active-print marker) and sends it SIGPWR, which that process's
 * handle_print_power_failure_signal() turns into a graceful cancel via
 * printerctl_print_engine_request_cancel_power_failure() -- no new
 * G-code transmitted, file and serial port closed normally, a session
 * record written with state=cancelled_power_failure.
 *
 * No print running is not an error: this command must be safely
 * idempotent for a script that may run with no print active, or run
 * more than once (the roadmap requires exactly this for the later
 * Milestone 19 socket-backed version, and there is no reason for the
 * pre-daemon version to behave any differently). A stale marker whose
 * process is gone (`kill(pid, 0)` fails) is cleaned up rather than
 * treated as a fatal error. A stale marker whose PID has since been
 * reused by some unrelated live process cannot be distinguished from a
 * legitimate one this way; see the Milestone 10 design note for why
 * this residual risk is accepted in the pre-daemon MVP.
 */
static int run_power_low_battery_command(printerctl_logger_t *logger, bool logger_ready,
                                          FILE *out, FILE *err)
{
    char dir[PRINTERCTL_RECORD_DIR_MAX];
    printerctl_record_error_t record_error;
    printerctl_record_status_t status =
        printerctl_record_resolve_state_dir(dir, sizeof(dir), &record_error);

    pid_t pid = 0;
    if (status == PRINTERCTL_RECORD_OK) {
        status = printerctl_record_read_active_pid(dir, &pid, &record_error);
    }

    if (status == PRINTERCTL_RECORD_ERR_NOT_FOUND) {
        fprintf(out, "printerctl: power low-battery: no active print session; nothing to cancel\n");
        if (logger_ready) {
            warn_on_log_failure(err,
                printerctl_log_state_transition(logger, "state=power_low_battery_no_active_print"),
                "failed to record state_transition log");
        }
        return PRINTERCTL_EXIT_OK;
    }

    if (status != PRINTERCTL_RECORD_OK) {
        fprintf(err, "printerctl: power low-battery: %s\n", record_error.message);
        if (logger_ready) {
            warn_on_log_failure(err,
                printerctl_log_record(logger, PRINTERCTL_LOG_WARNING, PRINTERCTL_LOG_EVENT_WARNING,
                                       "power low-battery: cannot determine the active print"),
                "failed to record warning log");
        }
        return PRINTERCTL_EXIT_COMM_ERROR;
    }

    if (kill(pid, 0) != 0) {
        fprintf(out,
            "printerctl: power low-battery: stale marker for process %ld; nothing to cancel\n",
            (long)pid);
        printerctl_record_remove_active_pid(dir);
        if (logger_ready) {
            warn_on_log_failure(err,
                printerctl_log_state_transition(logger, "state=power_low_battery_stale_marker"),
                "failed to record state_transition log");
        }
        return PRINTERCTL_EXIT_OK;
    }

    if (kill(pid, SIGPWR) != 0) {
        fprintf(err, "printerctl: power low-battery: failed to signal process %ld: %s\n",
                (long)pid, strerror(errno));
        if (logger_ready) {
            warn_on_log_failure(err,
                printerctl_log_record(logger, PRINTERCTL_LOG_WARNING, PRINTERCTL_LOG_EVENT_WARNING,
                                       "power low-battery: signal delivery failed"),
                "failed to record warning log");
        }
        return PRINTERCTL_EXIT_COMM_ERROR;
    }

    fprintf(out, "printerctl: power low-battery: signalled the active print (process %ld)\n",
            (long)pid);
    if (logger_ready) {
        warn_on_log_failure(err,
            printerctl_log_state_transition(logger, "state=power_low_battery_signalled"),
            "failed to record state_transition log");
    }
    return PRINTERCTL_EXIT_OK;
}

/* Marlin command a query maps to, for the dry-run "would send" report. */
static const char *single_command_tx_line(const printerctl_command_args_t *args)
{
    switch (args->command) {
    case PRINTERCTL_COMMAND_TEMP: return "M105";
    case PRINTERCTL_COMMAND_POSITION: return "M114";
    case PRINTERCTL_COMMAND_INFO: return "M115";
    case PRINTERCTL_COMMAND_HOME: return home_axis_command(args->home_axis);
    case PRINTERCTL_COMMAND_PAUSE: return "M25";
    case PRINTERCTL_COMMAND_RESUME: return "M24";
    case PRINTERCTL_COMMAND_CANCEL: return "M524";
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
    /* M18: direct serial CLI commands are a transitional legacy path.
     * Migrated PLP commands must switch to the client view and stop
     * consuming port/baud/timeout; this compatibility view is removed in M25. */
    printerctl_config_status_t status = printerctl_config_resolve_for_view(
        &request.overrides, PRINTERCTL_CONFIG_VIEW_LEGACY_CLI, &config, &config_error);
    if (status != PRINTERCTL_CONFIG_OK) {
        fprintf(err, "printerctl: configuration error: %s\n", config_error.message);
        /* config_error.message may embed --config's path; the structured log
         * uses a generic detail so the path is never written to the TSV
         * stream, only to the human-readable line above. */
        log_transient_error(err, "configuration error");
        return PRINTERCTL_EXIT_CONFIG_ERROR;
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
        request.command_args.command == PRINTERCTL_COMMAND_SEND ||
        request.command_args.command == PRINTERCTL_COMMAND_HOME ||
        request.command_args.command == PRINTERCTL_COMMAND_PAUSE ||
        request.command_args.command == PRINTERCTL_COMMAND_RESUME ||
        request.command_args.command == PRINTERCTL_COMMAND_CANCEL;

    int exit_code;

    if (request.command_args.command == PRINTERCTL_COMMAND_PRINT && config.dry_run) {
        exit_code = run_dry_run_print(request.command_args.print_file, &logger, logger_ready, out, err);
    } else if (request.command_args.command == PRINTERCTL_COMMAND_PRINT) {
        exit_code = run_print_command(&request.command_args, &config, &logger, logger_ready, out, err);
    } else if (is_single_command && config.dry_run) {
        const char *tx_line = single_command_tx_line(&request.command_args);

        /* The safety policy holds even when nothing would be sent: a
         * G28 through `send` is rejected in dry-run mode too. `home`'s
         * own G28 is not rejected here -- it is the one command G28 is
         * always available through. */
        if (request.command_args.command == PRINTERCTL_COMMAND_SEND &&
            printerctl_gcode_command_is_unsafe_g28(tx_line)) {
            static const char *const safety_message =
                "refusing to send G28: homing is only available through the "
                "'home' command, not 'send' (see the safety policy in Agents.md)";
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
    } else if (request.command_args.command == PRINTERCTL_COMMAND_POWER_LOW_BATTERY && config.dry_run) {
        /* No serial device is ever opened by this command in the first
         * place, but --dry-run still reports rather than acts, exactly
         * like every other command's dry-run branch. */
        fprintf(out,
            "printerctl: dry run: would check for an active print and signal it if present "
            "(no serial device is opened; no other process is signalled)\n");
        exit_code = PRINTERCTL_EXIT_OK;
    } else if (request.command_args.command == PRINTERCTL_COMMAND_POWER_LOW_BATTERY) {
        exit_code = run_power_low_battery_command(&logger, logger_ready, out, err);
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
