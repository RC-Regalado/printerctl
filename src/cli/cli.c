#define _DEFAULT_SOURCE

#include "printerctl/client/plp_client.h"
#include "printerctl/cli.h"
#include "printerctl/gcode.h"
#include "printerctl/logging.h"
#include "printerctl/record.h"
#include "printerctl/version.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PRINTERCTL_CLI_PLP_TIMEOUT_MS 5000
#define PRINTERCTL_CLI_PLP_WAIT_FOREVER_MS 0

/* PRINT-OBS-001: interval between STATUS polls while printerctl print FILE
 * remains attached as an observer after PRINT_START acceptance. */
#define PRINTERCTL_CLI_PRINT_POLL_INTERVAL_MS 200
#define PRINTERCTL_CLI_PRINT_STATUS_FIELDS                                                     \
    (PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE | PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS |    \
     PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED |                                      \
     PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_TOTAL | PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON)

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
    case PRINTERCTL_COMMAND_SD_PAUSE: return "sd pause";
    case PRINTERCTL_COMMAND_SD_RESUME: return "sd resume";
    case PRINTERCTL_COMMAND_SD_CANCEL: return "sd cancel";
    case PRINTERCTL_COMMAND_POWER_LOW_BATTERY: return "power";
    }
    return "unknown";
}

static bool is_migrated_query_command(printerctl_command_t command)
{
    return command == PRINTERCTL_COMMAND_TEMP ||
           command == PRINTERCTL_COMMAND_POSITION ||
           command == PRINTERCTL_COMMAND_INFO;
}

static bool is_migrated_physical_command(printerctl_command_t command)
{
    return command == PRINTERCTL_COMMAND_SEND ||
           command == PRINTERCTL_COMMAND_HOME ||
           command == PRINTERCTL_COMMAND_SD_PAUSE ||
           command == PRINTERCTL_COMMAND_SD_RESUME ||
           command == PRINTERCTL_COMMAND_SD_CANCEL;
}

static bool is_migrated_daemon_command(printerctl_command_t command)
{
    return command == PRINTERCTL_COMMAND_PRINT ||
           is_migrated_query_command(command) ||
           is_migrated_physical_command(command);
}

static bool is_reserved_host_control_command(printerctl_command_t command)
{
    return command == PRINTERCTL_COMMAND_PAUSE ||
           command == PRINTERCTL_COMMAND_RESUME ||
           command == PRINTERCTL_COMMAND_CANCEL;
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
    } else if (strcmp(requested_command, "sd") == 0) {
        if (i >= argc) {
            set_error(out_error, "command 'sd' requires a subcommand (expected 'pause', 'resume' or 'cancel')");
            return false;
        }
        if (strcmp(argv[i], "pause") == 0) {
            command = PRINTERCTL_COMMAND_SD_PAUSE;
        } else if (strcmp(argv[i], "resume") == 0) {
            command = PRINTERCTL_COMMAND_SD_RESUME;
        } else if (strcmp(argv[i], "cancel") == 0) {
            command = PRINTERCTL_COMMAND_SD_CANCEL;
        } else {
            set_error(out_error, "unrecognized subcommand '%s' for 'sd' (expected 'pause', 'resume' or 'cancel')",
                       argv[i]);
            return false;
        }
        i++;
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
    case PRINTERCTL_COMMAND_SD_PAUSE:
    case PRINTERCTL_COMMAND_SD_RESUME:
    case PRINTERCTL_COMMAND_SD_CANCEL:
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
        "Command-line controller for Marlin-based 3D printers through printerctld.\n"
        "'print FILE' starts daemon-owned host streaming over printerctld\n"
        "(Milestone 22); 'temp', 'position' and 'info' query through printerctld\n"
        "(Milestone 20); 'send', 'home' and 'sd pause/resume/cancel' run through printerctld (M21).\n"
        "'pause', 'resume' and 'cancel' are reserved for host prints (M23; see Print session\n"
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
        "  print FILE                          Ask printerctld to start a host print\n"
        "                                       (with --dry-run: parse and report only,\n"
        "                                       no daemon or serial access)\n"
        "  send COMMAND                        Send one raw command\n"
        "  home [X|Y|Z]                        Home all axes, or a single one\n"
        "  pause                                Host print pause (reserved until M23)\n"
        "  resume                               Host print resume (reserved until M23)\n"
        "  cancel                               Host print cancel (reserved until M23)\n"
        "  sd pause                             Pause the printer's SD print (M25)\n"
        "  sd resume                            Resume the printer's SD print (M24)\n"
        "  sd cancel                            Abort the printer's SD print (M524)\n"
        "  temp                                 Query temperatures\n"
        "  position                             Query current position\n"
        "  info                                 Query printer information\n"
        "  power low-battery                    Notify a running print of a power event\n"
        "\n"
        "Print session control:\n"
        "  'print FILE' starts daemon-owned host streaming and returns after\n"
        "  printerctld accepts the request; the stream continues if the client\n"
        "  disconnects. Host pause/resume/cancel are added in M23.\n"
        "  Nothing is sent to the printer on pause/resume/cancel/power-cancel of\n"
        "  a host stream: it never moves, heats or parks on its own.\n"
        "  The 'sd pause', 'sd resume' and 'sd cancel' commands instead talk\n"
        "  to a print running from the printer's SD card; they cannot reach a\n"
        "  host stream.\n"
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
        "  X'/'home Y'/'home Z') needs no confirmation flag. It remains a\n"
        "  daemon-owned command during Phase 2 migration.\n"
        "  G28 remains disabled everywhere else: daemon-owned print execution\n"
        "  rejects G28 from sliced files with no confirmation flag, and\n"
        "  'send' refuses G28 the same way, with no bypass -- homing is\n"
        "  only ever available through the dedicated 'home' command,\n"
        "  never an unexpected side effect of a raw command or a sliced file.\n"
        "  Every other command passes through 'send', including the\n"
        "  operator's explicit emergency stop (M112).\n"
        "\n"
        "Exit codes:\n"
        "  0  success (including --help/--version and a clean dry run)\n"
        "  2  usage error\n"
        "  3  configuration error\n"
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

/*
 * Min end-stops are enabled on all three axes, so `home` maps to a
 * fixed, safe G28 variant -- never operator-typed text. Generic `send`
 * still rejects G28 before IPC as UX, and the daemon remains the
 * authoritative safety owner for raw-send and print execution.
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

static uint8_t plp_home_axis(printerctl_home_axis_t axis)
{
    switch (axis) {
    case PRINTERCTL_HOME_X: return PRINTERCTL_PLP1_HOME_X;
    case PRINTERCTL_HOME_Y: return PRINTERCTL_PLP1_HOME_Y;
    case PRINTERCTL_HOME_Z: return PRINTERCTL_PLP1_HOME_Z;
    case PRINTERCTL_HOME_ALL:
    default: return PRINTERCTL_PLP1_HOME_ALL;
    }
}

static void render_transcript(FILE *out, const char *transcript)
{
    const char *line = transcript;
    while (line[0] != '\0') {
        const char *end = strchr(line, '\n');
        if (end == NULL) {
            fprintf(out, "< %s\n", line);
            return;
        }
        fprintf(out, "< %.*s\n", (int)(end - line), line);
        line = end + 1;
    }
}

static int wire_status_to_exit_code(uint16_t wire_status)
{
    switch (wire_status) {
    case PRINTERCTL_PLP1_SUCCESS:
        return PRINTERCTL_EXIT_OK;
    case PRINTERCTL_PLP1_SAFETY_REJECTED:
        return PRINTERCTL_EXIT_SAFETY_ERROR;
    case PRINTERCTL_PLP1_FILE_NOT_FOUND:
    case PRINTERCTL_PLP1_FILE_NOT_READABLE:
    case PRINTERCTL_PLP1_INVALID_GCODE_SOURCE:
        return PRINTERCTL_EXIT_GCODE_ERROR;
    case PRINTERCTL_PLP1_INVALID_ARGUMENT:
        return PRINTERCTL_EXIT_USAGE_ERROR;
    case PRINTERCTL_PLP1_PRINTER_ERROR:
        return PRINTERCTL_EXIT_PRINTER_ERROR;
    case PRINTERCTL_PLP1_SERIAL_ERROR:
    case PRINTERCTL_PLP1_PRINTER_TIMEOUT:
    case PRINTERCTL_PLP1_WORKER_UNAVAILABLE:
    case PRINTERCTL_PLP1_INTERNAL_ERROR:
    case PRINTERCTL_PLP1_SHUTTING_DOWN:
    case PRINTERCTL_PLP1_BUSY:
    case PRINTERCTL_PLP1_INVALID_STATE:
    default:
        return PRINTERCTL_EXIT_COMM_ERROR;
    }
}

static int client_status_to_exit_code(printerctl_client_status_t status, uint16_t wire_status)
{
    if (status == PRINTERCTL_CLIENT_ERR_REMOTE) {
        return wire_status_to_exit_code(wire_status);
    }
    return status == PRINTERCTL_CLIENT_OK ? PRINTERCTL_EXIT_OK : PRINTERCTL_EXIT_COMM_ERROR;
}

static void report_client_failure(FILE *err, printerctl_logger_t *logger, bool logger_ready,
                                  const char *command_name_text,
                                  printerctl_client_status_t status,
                                  const printerctl_client_error_t *error)
{
    fprintf(err, "printerctl: %s\n", error->message);
    if (logger_ready) {
        char detail[96];
        snprintf(detail, sizeof(detail), "command=%s client_status=%d wire_status=0x%04X",
                 command_name_text, (int)status, error->wire_status);
        warn_on_log_failure(err, printerctl_log_error(logger, detail),
            "failed to record error log");
    }
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static const char *execution_state_label(uint8_t state)
{
    switch (state) {
    case PRINTERCTL_PLP1_EXECUTION_STATE_IDLE: return "Idle";
    case PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING: return "Printing";
    case PRINTERCTL_PLP1_EXECUTION_STATE_PAUSED: return "Paused";
    case PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLING: return "Cancelling";
    case PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED: return "Completed";
    case PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED: return "Cancelled";
    case PRINTERCTL_PLP1_EXECUTION_STATE_FAILED: return "Failed";
    default: return "Unknown";
    }
}

static bool execution_state_is_terminal(uint8_t state)
{
    return state == PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED ||
           state == PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED ||
           state == PRINTERCTL_PLP1_EXECUTION_STATE_FAILED;
}

/* "Printing: 47% (583/1240)" when commands_total is known, otherwise
 * "Printing: 47% (583 acknowledged)" -- commands_total is best-effort
 * (see docs/design/protocol-v1.md STATUS §16) and may be unavailable. */
static void render_print_progress(FILE *out, const printerctl_plp1_status_response_t *status)
{
    fprintf(out, "printerctl: %s: %u%%", execution_state_label(status->execution_state),
            (unsigned)status->progress);
    if ((status->present_fields & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_TOTAL) != 0) {
        fprintf(out, " (%u/%u)", (unsigned)status->commands_acknowledged,
                (unsigned)status->commands_total);
    } else if ((status->present_fields & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED) !=
               0) {
        fprintf(out, " (%u acknowledged)", (unsigned)status->commands_acknowledged);
    }
    fprintf(out, "\n");
}

/*
 * PRINT-OBS-001: remains attached on `client` after PRINT_START
 * acceptance, polling STATUS (snapshot-only, zero serial I/O) until the
 * daemon reports a terminal execution_state -- never merely because
 * progress reaches 100%, and never on a client-side timer/heuristic.
 * `client` is left open; the caller closes it.
 */
static int run_print_progress_observer(printerctl_client_t *client, FILE *out, FILE *err,
                                       printerctl_logger_t *logger, bool logger_ready)
{
    bool have_previous = false;
    uint8_t previous_state = 0;
    uint8_t previous_progress = 0;
    uint32_t previous_acknowledged = 0;

    for (;;) {
        printerctl_plp1_status_response_t status_response;
        printerctl_client_error_t error;
        printerctl_client_status_t status = printerctl_client_status(
            client, PRINTERCTL_CLI_PRINT_STATUS_FIELDS, PRINTERCTL_CLI_PLP_TIMEOUT_MS,
            &status_response, &error);
        if (status != PRINTERCTL_CLIENT_OK) {
            report_client_failure(err, logger, logger_ready, "print", status, &error);
            return client_status_to_exit_code(status, error.wire_status);
        }

        if (!have_previous || status_response.execution_state != previous_state ||
            status_response.progress != previous_progress ||
            status_response.commands_acknowledged != previous_acknowledged) {
            render_print_progress(out, &status_response);
            have_previous = true;
            previous_state = status_response.execution_state;
            previous_progress = status_response.progress;
            previous_acknowledged = status_response.commands_acknowledged;
        }

        if (execution_state_is_terminal(status_response.execution_state)) {
            if (status_response.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_FAILED) {
                uint16_t reason =
                    (status_response.present_fields &
                     PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON) != 0
                        ? status_response.failure_reason
                        : PRINTERCTL_PLP1_INTERNAL_ERROR;
                fprintf(err, "printerctl: print failed\n");
                return wire_status_to_exit_code(reason);
            }
            return PRINTERCTL_EXIT_OK;
        }

        sleep_ms(PRINTERCTL_CLI_PRINT_POLL_INTERVAL_MS);
    }
}

static int run_print_start_command(const printerctl_command_args_t *args,
                                   printerctl_logger_t *logger, bool logger_ready,
                                   FILE *out, FILE *err)
{
    printerctl_client_t client;
    printerctl_client_error_t error;
    printerctl_client_status_t status =
        printerctl_client_connect(&client, PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, &error);
    if (status != PRINTERCTL_CLIENT_OK) {
        report_client_failure(err, logger, logger_ready, "print", status, &error);
        return client_status_to_exit_code(status, error.wire_status);
    }

    status = printerctl_client_print_start(
        &client, args->print_file, PRINTERCTL_CLI_PLP_TIMEOUT_MS, &error);
    if (status != PRINTERCTL_CLIENT_OK) {
        printerctl_client_close(&client);
        report_client_failure(err, logger, logger_ready, "print", status, &error);
        return client_status_to_exit_code(status, error.wire_status);
    }

    fprintf(out, "printerctl: print accepted by printerctld: '%s'\n", args->print_file);

    int exit_code = run_print_progress_observer(&client, out, err, logger, logger_ready);
    printerctl_client_close(&client);
    return exit_code;
}

static double milli_to_double(int32_t value)
{
    return (double)value / 1000.0;
}

static int run_query_command(const printerctl_command_args_t *args,
                             printerctl_logger_t *logger, bool logger_ready,
                             FILE *out, FILE *err)
{
    printerctl_client_t client;
    printerctl_client_error_t error;
    printerctl_client_status_t status =
        printerctl_client_connect(&client, PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, &error);
    if (status != PRINTERCTL_CLIENT_OK) {
        report_client_failure(err, logger, logger_ready, command_name(args->command), status,
                              &error);
        return client_status_to_exit_code(status, error.wire_status);
    }

    if (args->command == PRINTERCTL_COMMAND_TEMP) {
        printerctl_plp1_query_temp_response_t temp;
        status = printerctl_client_query_temp(&client, PRINTERCTL_CLI_PLP_TIMEOUT_MS, &temp,
                                              &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            fprintf(out, "printerctl: hotend %.2f C (target %.2f C)",
                    milli_to_double(temp.hotend_actual_milli_c),
                    milli_to_double(temp.hotend_target_milli_c));
            if (temp.present_fields & PRINTERCTL_PLP1_QUERY_TEMP_FIELD_BED) {
                fprintf(out, ", bed %.2f C (target %.2f C)",
                        milli_to_double(temp.bed_actual_milli_c),
                        milli_to_double(temp.bed_target_milli_c));
            }
            fprintf(out, "\n");
        }
    } else if (args->command == PRINTERCTL_COMMAND_POSITION) {
        printerctl_plp1_query_position_response_t position;
        status = printerctl_client_query_position(&client, PRINTERCTL_CLI_PLP_TIMEOUT_MS,
                                                  &position, &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            fprintf(out, "printerctl: position X=%.2f Y=%.2f Z=%.2f",
                    milli_to_double(position.x_milli_mm),
                    milli_to_double(position.y_milli_mm),
                    milli_to_double(position.z_milli_mm));
            if (position.present_fields & PRINTERCTL_PLP1_QUERY_POSITION_FIELD_E) {
                fprintf(out, " E=%.2f", milli_to_double(position.e_milli_mm));
            }
            fprintf(out, "\n");
        }
    } else {
        printerctl_plp1_query_info_response_t info;
        status = printerctl_client_query_info(&client, PRINTERCTL_CLI_PLP_TIMEOUT_MS, &info,
                                              &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            fprintf(out, "printerctl: firmware: %s\n", info.firmware_name);
            if (info.machine_type[0] != '\0') {
                fprintf(out, "printerctl: machine: %s\n", info.machine_type);
            }
            if (info.protocol_version[0] != '\0') {
                fprintf(out, "printerctl: protocol: %s\n", info.protocol_version);
            }
            if (info.has_extruder_count) {
                fprintf(out, "printerctl: extruders: %u\n", (unsigned)info.extruder_count);
            }
            if (info.uuid[0] != '\0') {
                fprintf(out, "printerctl: uuid: %s\n", info.uuid);
            }
        }
    }

    printerctl_client_close(&client);
    if (status != PRINTERCTL_CLIENT_OK) {
        report_client_failure(err, logger, logger_ready, command_name(args->command), status,
                              &error);
        return client_status_to_exit_code(status, error.wire_status);
    }
    return PRINTERCTL_EXIT_OK;
}

static int run_physical_command(const printerctl_command_args_t *args,
                                printerctl_logger_t *logger, bool logger_ready,
                                FILE *out, FILE *err)
{
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

    printerctl_client_t client;
    printerctl_client_error_t error;
    printerctl_client_status_t status =
        printerctl_client_connect(&client, PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, &error);
    if (status != PRINTERCTL_CLIENT_OK) {
        report_client_failure(err, logger, logger_ready, command_name(args->command), status,
                              &error);
        return client_status_to_exit_code(status, error.wire_status);
    }

    if (args->command == PRINTERCTL_COMMAND_SEND) {
        printerctl_plp1_raw_send_response_t response;
        fprintf(out, "> %s\n", args->send_command);
        status = printerctl_client_raw_send(&client, args->send_command,
                                            PRINTERCTL_CLI_PLP_TIMEOUT_MS, &response, &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            render_transcript(out, response.transcript);
        }
    } else if (args->command == PRINTERCTL_COMMAND_HOME) {
        const char *command = home_axis_command(args->home_axis);
        fprintf(out, "> %s\n", command);
        status = printerctl_client_home(&client, plp_home_axis(args->home_axis),
                                        PRINTERCTL_CLI_PLP_WAIT_FOREVER_MS, &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            fprintf(out, "printerctl: home complete: %s (%s acknowledged)\n",
                    home_axis_name(args->home_axis), command);
        }
    } else if (args->command == PRINTERCTL_COMMAND_SD_PAUSE) {
        fprintf(out, "> M25\n");
        status = printerctl_client_sd_pause(&client, PRINTERCTL_CLI_PLP_TIMEOUT_MS, &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            fprintf(out, "printerctl: SD print pause requested (M25 acknowledged)\n");
        }
    } else if (args->command == PRINTERCTL_COMMAND_SD_RESUME) {
        fprintf(out, "> M24\n");
        status = printerctl_client_sd_resume(&client, PRINTERCTL_CLI_PLP_TIMEOUT_MS, &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            fprintf(out, "printerctl: SD print resume requested (M24 acknowledged)\n");
        }
    } else {
        fprintf(out, "> M524\n");
        status = printerctl_client_sd_cancel(&client, PRINTERCTL_CLI_PLP_TIMEOUT_MS, &error);
        if (status == PRINTERCTL_CLIENT_OK) {
            fprintf(out, "printerctl: SD print abort requested (M524 acknowledged)\n");
        }
    }

    printerctl_client_close(&client);
    if (status != PRINTERCTL_CLIENT_OK) {
        report_client_failure(err, logger, logger_ready, command_name(args->command), status,
                              &error);
        return client_status_to_exit_code(status, error.wire_status);
    }
    return PRINTERCTL_EXIT_OK;
}

/*
 * Milestone 10: `power low-battery` is the command an external notifier
 * (the APCUPSD low-battery script) runs to tell printerctl that the
 * host may be about to lose power. It never touches the serial port
 * itself: during M22 it still uses the transitional marker path to
 * signal the daemon process that owns an active print. No new cleanup
 * G-code is generated by the CLI.
 *
 * No print running is not an error: this command must be safely
 * idempotent for a script that may run with no print active, or run
 * more than once (the roadmap requires exactly this for the future
 * daemon-backed version, and there is no reason for the pre-daemon
 * version to behave any differently). A stale marker whose
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
    case PRINTERCTL_COMMAND_SD_PAUSE: return "M25";
    case PRINTERCTL_COMMAND_SD_RESUME: return "M24";
    case PRINTERCTL_COMMAND_SD_CANCEL: return "M524";
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
    /* M18: remaining transitional commands use the legacy config view.
     * Migrated PLP commands use the client view and do not consume
     * port/baud/timeout; the compatibility view is removed in M25. */
    printerctl_config_view_t config_view =
        (is_migrated_daemon_command(request.command_args.command) ||
         is_reserved_host_control_command(request.command_args.command))
            ? PRINTERCTL_CONFIG_VIEW_CLIENT
            : PRINTERCTL_CONFIG_VIEW_LEGACY_CLI;
    printerctl_config_status_t status = printerctl_config_resolve_for_view(
        &request.overrides, config_view, &config, &config_error);
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
        request.command_args.command == PRINTERCTL_COMMAND_SEND ||
        request.command_args.command == PRINTERCTL_COMMAND_HOME ||
        request.command_args.command == PRINTERCTL_COMMAND_SD_PAUSE ||
        request.command_args.command == PRINTERCTL_COMMAND_SD_RESUME ||
        request.command_args.command == PRINTERCTL_COMMAND_SD_CANCEL;

    int exit_code;

    if (request.command_args.command == PRINTERCTL_COMMAND_PRINT && config.dry_run) {
        exit_code = run_dry_run_print(request.command_args.print_file, &logger, logger_ready, out, err);
    } else if (request.command_args.command == PRINTERCTL_COMMAND_PRINT) {
        exit_code = run_print_start_command(&request.command_args, &logger, logger_ready, out, err);
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
        exit_code = run_physical_command(&request.command_args, &logger, logger_ready, out, err);
    } else if (is_migrated_query_command(request.command_args.command) && config.dry_run) {
        const char *tx_line = single_command_tx_line(&request.command_args);
        fprintf(out,
            "printerctl: dry run: would send '%s' through printerctld and wait for the response "
            "(no serial device is opened by printerctl)\n", tx_line);
        exit_code = PRINTERCTL_EXIT_OK;
    } else if (is_migrated_query_command(request.command_args.command)) {
        exit_code = run_query_command(&request.command_args, &logger, logger_ready, out, err);
    } else if (is_reserved_host_control_command(request.command_args.command)) {
        fprintf(err,
                "printerctl: command '%s' is reserved for host print control and is not "
                "available until M23; use 'sd %s' for SD-card prints\n",
                command_name(request.command_args.command),
                command_name(request.command_args.command));
        exit_code = PRINTERCTL_EXIT_UNAVAILABLE;
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
