#include "printerctl/cli.h"
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
        "This build parses commands and resolves configuration, but does not\n"
        "yet perform any serial or hardware action (see roadmap.md, Milestone 1).\n"
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
        "  print FILE                          Stream a G-code file\n"
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
        "\n"
        "Exit codes:\n"
        "  0  success (including --help/--version)\n"
        "  2  usage error\n"
        "  3  configuration error\n"
        "  4  safety policy rejection\n"
        "  5  parsed successfully but not yet available in this build\n",
        PRINTERCTL_CONFIG_MIN_BAUD, PRINTERCTL_CONFIG_MAX_BAUD, PRINTERCTL_CONFIG_DEFAULT_BAUD,
        PRINTERCTL_CONFIG_MIN_TIMEOUT_MS, PRINTERCTL_CONFIG_MAX_TIMEOUT_MS, PRINTERCTL_CONFIG_DEFAULT_TIMEOUT_MS);
}

int printerctl_cli_run(int argc, char *const argv[], FILE *out, FILE *err)
{
    printerctl_cli_request_t request;
    printerctl_cli_error_t error;

    if (!printerctl_cli_parse(argc, argv, &request, &error)) {
        fprintf(err, "printerctl: %s\n", error.message);
        print_usage_hint(err);
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
        return PRINTERCTL_EXIT_CONFIG_ERROR;
    }

    if (request.command_args.command == PRINTERCTL_COMMAND_HOME && !request.command_args.home_confirmed) {
        fprintf(err,
            "printerctl: refusing to home without --i-understand-no-endstops "
            "(endstop sensors are not available; see Agents.md safety policy)\n");
        return PRINTERCTL_EXIT_SAFETY_ERROR;
    }

    fprintf(out,
        "printerctl: command '%s' parsed successfully but is not yet implemented "
        "(port=%s baud=%d timeout=%dms dry_run=%s verbose=%s)\n",
        command_name(request.command_args.command),
        config.port[0] != '\0' ? config.port : "(none)",
        config.baud, config.timeout_ms,
        config.dry_run ? "true" : "false",
        config.verbose ? "true" : "false");
    return PRINTERCTL_EXIT_UNAVAILABLE;
}
