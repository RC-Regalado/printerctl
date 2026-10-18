/* XOPEN 700 for posix_openpt/grantpt/unlockpt/ptsname (and POSIX.1-2008). */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "printerctl/cli.h"
#include "printerctl/protocol/plp1.h"
#include "printerctl/config.h"
#include "printerctl/version.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures = 0;

/*
 * Milestone 9 session records land in $XDG_STATE_HOME/printerctl. The
 * whole suite runs against a disposable state home so no test ever
 * touches the developer's real ~/.local/state; record-specific tests
 * create their own and restore this default afterwards.
 */
static char g_default_state_home[64];

static void write_temp_gcode(const char *content, char *path, size_t path_size);

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

static int run_cli(int argc, char *argv[], char **out_buf, char **err_buf)
{
    size_t out_len = 0;
    size_t err_len = 0;
    FILE *out = open_memstream(out_buf, &out_len);
    FILE *err = open_memstream(err_buf, &err_len);

    int status = printerctl_cli_run(argc, argv, out, err);

    fclose(out);
    fclose(err);

    return status;
}

/* Writes `content` to a fresh temp file and returns its path via `path`. */
static void write_temp_config(const char *content, char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/printerctl_cli_test_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "FAIL: mkstemp() failed for temp config fixture\n");
        g_failures++;
        return;
    }

    FILE *file = fdopen(fd, "w");
    fputs(content, file);
    fclose(file);
}

static void test_help(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--help"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "help exits with success status");
    check(out != NULL && strstr(out, "Usage: printerctl") != NULL, "help output includes usage line");
    check(out != NULL && strstr(out, "--port") != NULL, "help documents --port");
    check(out != NULL && strstr(out, "--baud") != NULL, "help documents --baud");
    check(out != NULL && strstr(out, "--timeout") != NULL, "help documents --timeout");
    check(out != NULL && strstr(out, "--dry-run") != NULL, "help documents --dry-run");
    check(out != NULL && strstr(out, "--verbose") != NULL, "help documents --verbose");
    check(out != NULL && strstr(out, "--config") != NULL, "help documents --config");
    check(out != NULL && strstr(out, "print FILE") != NULL, "help documents print");
    check(out != NULL && strstr(out, "send COMMAND") != NULL, "help documents send");
    check(out != NULL && strstr(out, "home [X|Y|Z]") != NULL, "help documents home and its single-axis variants");
    check(out != NULL && strstr(out, "--i-understand-no-endstops") == NULL,
          "help no longer documents the retired home confirmation flag");
    check(out != NULL && strstr(out, "pause") != NULL, "help documents pause");
    check(out != NULL && strstr(out, "resume") != NULL, "help documents resume");
    check(out != NULL && strstr(out, "cancel") != NULL, "help documents cancel");
    check(out != NULL && strstr(out, "temp") != NULL, "help documents temp");
    check(out != NULL && strstr(out, "position") != NULL, "help documents position");
    check(out != NULL && strstr(out, "info") != NULL, "help documents info");
    check(out != NULL && strstr(out, "power low-battery") != NULL, "help documents power low-battery");
    check(out != NULL && strstr(out, "SIGPWR") != NULL, "help documents the SIGPWR power-failure signal");
    check(out != NULL && strstr(out, "Safety:") != NULL, "help documents the home safety policy");
    /* A trailing space distinguishes the old "Milestone 1 ..." tag this
     * checks for from later, legitimate "(Milestone 10; ...)" mentions,
     * which also start with the substring "Milestone 1". */
    check(out != NULL && strstr(out, "Milestone 1 ") == NULL, "help no longer claims to be Milestone 1");
    check(err != NULL && err[0] == '\0', "help produces no error output");

    free(out);
    free(err);
}

static void test_version(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--version"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    char expected[64];
    snprintf(expected, sizeof(expected), "printerctl %s", PRINTERCTL_VERSION_STRING);

    check(status == PRINTERCTL_EXIT_OK, "version exits with success status");
    check(out != NULL && strstr(out, expected) != NULL, "version output includes version string");
    check(err != NULL && err[0] == '\0', "version produces no error output");

    free(out);
    free(err);
}

static void test_missing_command(void)
{
    char *argv[] = {(char *)"printerctl"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(1, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "missing command exits with usage error");
    check(err != NULL && strstr(err, "missing command") != NULL, "missing command error is explicit");
    check(out != NULL && out[0] == '\0', "missing command produces no stdout output");

    free(out);
    free(err);
}

static void test_unknown_global_option(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--bogus", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "unknown global option exits with usage error");
    check(err != NULL && strstr(err, "--bogus") != NULL, "unknown option error names the option");

    free(out);
    free(err);
}

static void test_help_rejects_extra_arguments(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--help", (char *)"extra"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'--help extra' exits with usage error");
    check(err != NULL && strstr(err, "too many arguments") != NULL, "'--help extra' error is explicit");

    free(out);
    free(err);
}

static void test_equals_form_rejected(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--port=/dev/ttyACM0", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'--option=VALUE' form exits with usage error");
    check(err != NULL && strstr(err, "--port") != NULL, "'--option=VALUE' error names the option");
    check(err != NULL && strstr(err, "=") != NULL, "'--option=VALUE' error mentions the unsupported syntax");

    free(out);
    free(err);
}

static void test_duplicate_global_option(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--port", (char *)"/dev/ttyACM0",
                     (char *)"--port", (char *)"/dev/ttyACM1", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(6, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "duplicate --port exits with usage error");
    check(err != NULL && strstr(err, "duplicate") != NULL, "duplicate --port error says 'duplicate'");

    free(out);
    free(err);
}

static void test_duplicate_boolean_flag(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"--dry-run", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "duplicate --dry-run exits with usage error");
    check(err != NULL && strstr(err, "duplicate") != NULL, "duplicate --dry-run error says 'duplicate'");

    free(out);
    free(err);
}

static void test_missing_option_value(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--port"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'--port' with no value exits with usage error");
    check(err != NULL && strstr(err, "--port") != NULL, "missing value error names the option");

    free(out);
    free(err);
}

static void test_port_empty_value(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--port", (char *)"", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "empty '--port' value exits with usage error");
    check(err != NULL && strstr(err, "--port") != NULL, "empty '--port' error names the option");

    free(out);
    free(err);
}

static void test_print_empty_file(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"print", (char *)""};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'print' with an empty FILE argument exits with usage error");

    free(out);
    free(err);
}

static void test_send_empty_command(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"send", (char *)""};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'send' with an empty COMMAND argument exits with usage error");

    free(out);
    free(err);
}

static void test_invalid_baud_value(void)
{
    {
        char *argv[] = {(char *)"printerctl", (char *)"--baud", (char *)"abc", (char *)"pause"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(4, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "non-numeric --baud exits with usage error");
        free(out);
        free(err);
    }
    {
        char *argv[] = {(char *)"printerctl", (char *)"--baud", (char *)"0", (char *)"pause"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(4, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "out-of-range --baud (0) exits with usage error");
        free(out);
        free(err);
    }
    {
        char *argv[] = {(char *)"printerctl", (char *)"--baud", (char *)"5000000", (char *)"pause"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(4, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "out-of-range --baud (5000000) exits with usage error");
        free(out);
        free(err);
    }
}

static void test_invalid_timeout_value(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--timeout", (char *)"700000", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);
    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "out-of-range --timeout exits with usage error");

    free(out);
    free(err);
}

static void test_unknown_command(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"levitate"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "unknown command exits with usage error");
    check(err != NULL && strstr(err, "levitate") != NULL, "unknown command error names the command");

    free(out);
    free(err);
}

static void test_print_command_needs_daemon(void)
{
    /* M22 migrated 'print' to printerctld; without the daemon it fails
     * as unavailable/communication, not as a missing serial port. */
    char *argv[] = {(char *)"printerctl", (char *)"print", (char *)"file.gcode"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR, "valid 'print' without a daemon is a communication error");
    check(err != NULL && strstr(err, "connect") != NULL, "the daemon connection error is reported");

    free(out);
    free(err);
}

static void test_print_missing_file(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"print"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'print' with no FILE exits with usage error");

    free(out);
    free(err);
}

static void test_print_extra_arguments(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"print", (char *)"a.gcode", (char *)"b.gcode"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'print' with two files exits with usage error");

    free(out);
    free(err);
}

static void test_send_command(void)
{
    /* M21 migrates 'send' to printerctld; without the daemon it fails
     * as unavailable/communication, not as a missing serial port. */
    char *argv[] = {(char *)"printerctl", (char *)"send", (char *)"M115 extra words"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR, "valid 'send' without a daemon is a communication error");
    check(err != NULL && strstr(err, "connect") != NULL, "the daemon connection error is reported");

    free(out);
    free(err);
}

static void test_send_missing_argument(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"send"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'send' with no COMMAND exits with usage error");

    free(out);
    free(err);
}

/*
 * Min end-stops are now enabled on all three axes: 'home' needs no
 * confirmation flag and runs like temp/position/info, requiring only a
 * printerctld; without a daemon it is a communication error, not a
 * safety rejection.
 */
static void test_home_no_longer_mentions_retired_confirmation_flag(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"home"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR, "'home' without a daemon is a communication error, not a safety rejection");
    check(err != NULL && strstr(err, "i-understand-no-endstops") == NULL,
          "the retired confirmation flag is never mentioned");

    free(out);
    free(err);
}

static void test_home_axis_argument_grammar(void)
{
    /* An unrecognized axis letter is a usage error. */
    {
        char *argv[] = {(char *)"printerctl", (char *)"home", (char *)"W"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(3, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'home W' (not X/Y/Z) exits with usage error");
        check(err != NULL && strstr(err, "'X', 'Y' or 'Z'") != NULL,
              "the usage error names the accepted axis letters");
        free(out);
        free(err);
    }
    /* The retired confirmation flag is now just an unrecognized argument. */
    {
        char *argv[] = {(char *)"printerctl", (char *)"home", (char *)"--i-understand-no-endstops"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(3, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR,
              "the retired confirmation flag is now an unrecognized argument, not accepted");
        free(out);
        free(err);
    }
    /* Only a single trailing argument is accepted. */
    {
        char *argv[] = {(char *)"printerctl", (char *)"home", (char *)"X", (char *)"Y"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(4, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'home X Y' (two axes) exits with usage error");
        free(out);
        free(err);
    }
}

static void test_dry_run_home(void)
{
    static const struct {
        const char *argument;
        const char *would_send;
    } cases[] = {
        {NULL, "would send 'G28'"},
        {"X", "would send 'G28 X'"},
        {"Y", "would send 'G28 Y'"},
        {"Z", "would send 'G28 Z'"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *argv[4] = {(char *)"printerctl", (char *)"--dry-run", (char *)"home", NULL};
        int argc = 3;
        if (cases[i].argument != NULL) {
            argv[3] = (char *)cases[i].argument;
            argc = 4;
        }
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(argc, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_OK, "'--dry-run home' exits with success without a port");
        check(out != NULL && strstr(out, cases[i].would_send) != NULL,
              "the dry-run home reports the G28 variant it would send");

        free(out);
        free(err);
    }
}

static void test_migrated_print_ignores_hardware_options(void)
{
    char path[64];
    write_temp_gcode("M105\n", path, sizeof(path));
    char *argv[] = {(char *)"printerctl", (char *)"--port", (char *)"/tmp/unused",
                    (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "migrated print uses printerctld even when a legacy --port is present");
    check(err != NULL && strstr(err, "/tmp/unused") == NULL,
          "migrated print does not consume the legacy --port value");

    remove(path);
    free(out);
    free(err);
}

static void test_global_option_after_command_rejected(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"pause", (char *)"--verbose"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "global option placed after the command exits with usage error");

    free(out);
    free(err);
}

static void test_cli_value_equal_to_default(void)
{
    /* Every resolved command logs a config_summary line with the baud
     * value; 'power low-battery' is the simplest one that always
     * succeeds without needing a --port at all (unlike every other
     * command, now that 'home' is fully implemented too). */
    char *argv[] = {(char *)"printerctl", (char *)"--baud", (char *)"115200",
                     (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "explicit --baud equal to the default still parses successfully");
    check(err != NULL && strstr(err, "baud=115200") != NULL, "resolved config reflects the explicit default value");

    free(out);
    free(err);
}

static void test_config_file_precedence(void)
{
    char path[64];
    write_temp_config("port=/dev/from-file\nbaud=57600\n", path, sizeof(path));

    /* power low-battery still uses the transitional legacy config view
     * until M24 moves it to daemon ownership. */
    char *argv[] = {(char *)"printerctl", (char *)"--config", path,
                     (char *)"--port", (char *)"/dev/from-cli",
                     (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(7, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "legacy-view command accepts merged hardware config");
    check(err != NULL && strstr(err, "/dev/from-file") == NULL, "the config file's port never took effect");
    check(err != NULL && strstr(err, "baud=57600") != NULL,
          "config file baud is applied when CLI does not override it");

    remove(path);
    free(out);
    free(err);
}

static void test_config_file_not_found(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--config", (char *)"/nonexistent/printerctl.conf", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_CONFIG_ERROR, "missing --config file exits with configuration error");
    check(err != NULL && strstr(err, "configuration error") != NULL, "configuration error message is explicit");

    free(out);
    free(err);
}

static void test_config_file_invalid(void)
{
    char path[64];
    write_temp_config("baud=not-a-number\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--config", path, (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_CONFIG_ERROR, "invalid --config file exits with configuration error");

    remove(path);
    free(out);
    free(err);
}

static void test_config_file_duplicate_key(void)
{
    char path[64];
    write_temp_config("port=/dev/ttyACM0\nport=/dev/ttyACM1\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--config", path, (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_CONFIG_ERROR, "--config file with a duplicated key exits with configuration error");
    check(err != NULL && strstr(err, "duplicated") != NULL, "configuration error mentions the duplicated key");

    remove(path);
    free(out);
    free(err);
}

static void test_config_missing_value(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--config"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'--config' with no value exits with usage error");

    free(out);
    free(err);
}

/*
 * Exercises printerctl_cli_parse() directly rather than through
 * printerctl_cli_run(), to verify the pure-parser contract documented in
 * cli.h: the resolved action/command, overrides (value plus has_* flag) and
 * that print_file/send_command point straight into argv rather than a copy.
 */
static void test_parse_direct_contract(void)
{
    char *argv[] = {(char *)"printerctl",
                     (char *)"--port", (char *)"/dev/ttyACM0",
                     (char *)"--baud", (char *)"250000",
                     (char *)"--dry-run",
                     (char *)"print", (char *)"file.gcode"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));

    printerctl_cli_request_t request;
    printerctl_cli_error_t error;

    bool ok = printerctl_cli_parse(argc, argv, &request, &error);

    check(ok, "direct parse of a valid invocation succeeds");
    check(request.action == PRINTERCTL_CLI_ACTION_COMMAND, "direct parse sets action to COMMAND");
    check(request.command_args.command == PRINTERCTL_COMMAND_PRINT, "direct parse sets command to PRINT");
    check(request.command_args.print_file == argv[7], "print_file points directly into argv, not a copy");
    check(request.overrides.has_port && strcmp(request.overrides.port, "/dev/ttyACM0") == 0,
          "direct parse records the --port override value");
    check(request.overrides.has_baud && request.overrides.baud == 250000,
          "direct parse records the --baud override value");
    check(request.overrides.has_dry_run && request.overrides.dry_run == true,
          "direct parse records the --dry-run override");
    check(!request.overrides.has_verbose && !request.overrides.has_timeout && !request.overrides.has_config_path,
          "direct parse leaves options that were not passed with has_*== false");
}

static void test_help_and_version_produce_no_logs(void)
{
    {
        char *argv[] = {(char *)"printerctl", (char *)"--help"};
        char *out = NULL;
        char *err = NULL;
        run_cli(2, argv, &out, &err);
        check(err != NULL && err[0] == '\0', "'--help' writes nothing to stderr, including no log lines");
        free(out);
        free(err);
    }
    {
        char *argv[] = {(char *)"printerctl", (char *)"--version"};
        char *out = NULL;
        char *err = NULL;
        run_cli(2, argv, &out, &err);
        check(err != NULL && err[0] == '\0', "'--version' writes nothing to stderr, including no log lines");
        free(out);
        free(err);
    }
}

static void test_successful_command_logs_session_lifecycle(void)
{
    /* 'power low-battery' always succeeds without a --port, making it
     * the simplest real command for a generic "successful command"
     * lifecycle check now that 'home' (like everything else) requires
     * one and no command is ever "unavailable" in ordinary use. */
    char *argv[] = {(char *)"printerctl", (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "non-verbose 'power low-battery' succeeds");
    check(err != NULL && strstr(err, "\tINFO\tsession_start\t") != NULL, "log includes a session_start line");
    check(err != NULL && strstr(err, "\tINFO\tconfig_summary\t") != NULL, "log includes a config_summary line");
    check(err != NULL && strstr(err, "\tINFO\tstate_transition\tstate=power_low_battery_no_active_print") != NULL,
          "log includes the power low-battery state transition");
    check(err != NULL && strstr(err, "\tINFO\tsession_end\t") != NULL, "log includes a session_end line");
    check(err != NULL && strstr(err, "command_parsed") == NULL,
          "non-verbose run does not log the DEBUG command_parsed line");
    check(err != NULL && count_occurrences(err, "\n") == 4, "non-verbose run logs exactly four lines");

    free(out);
    free(err);
}

static void test_verbose_adds_command_parsed_log(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--verbose", (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "verbose 'power low-battery' succeeds");
    check(err != NULL && strstr(err, "\tDEBUG\tcommand_parsed\tcommand=power") != NULL,
          "verbose run logs the DEBUG command_parsed line naming the command");
    check(err != NULL && count_occurrences(err, "\n") == 5, "verbose run logs exactly five lines");

    free(out);
    free(err);
}

static void test_config_summary_omits_raw_port_and_config_path(void)
{
    char path[64];
    write_temp_config("baud=57600\n", path, sizeof(path));

    /* The assertions below check the config_summary log line
     * specifically, matching the Milestone 2 privacy rule that
     * structured logs never expose raw port/config paths. */
    char *argv[] = {(char *)"printerctl", (char *)"--port", (char *)"/dev/ttyACM0",
                     (char *)"--config", path,
                     (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(7, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "port+config resolved command succeeds");

    const char *summary_line = err != NULL ? strstr(err, "\tINFO\tconfig_summary\t") : NULL;
    check(summary_line != NULL, "log includes a config_summary line");

    char summary_text[256];
    summary_text[0] = '\0';
    if (summary_line != NULL) {
        const char *end = strchr(summary_line, '\n');
        size_t length = end != NULL ? (size_t)(end - summary_line) : strlen(summary_line);
        if (length >= sizeof(summary_text)) {
            length = sizeof(summary_text) - 1;
        }
        memcpy(summary_text, summary_line, length);
        summary_text[length] = '\0';
    }

    check(strstr(summary_text, "port_set=true") != NULL, "config_summary reports port_set=true as a boolean");
    check(strstr(summary_text, "config_file_set=true") != NULL,
          "config_summary reports config_file_set=true as a boolean");
    check(strstr(summary_text, "/dev/ttyACM0") == NULL,
          "the config_summary log line itself never contains the raw port value");
    check(strstr(summary_text, path) == NULL,
          "the config_summary log line itself never contains the config file path");

    remove(path);
    free(out);
    free(err);
}

static void test_print_and_send_do_not_leak_content(void)
{
    {
        char *argv[] = {(char *)"printerctl", (char *)"print", (char *)"/tmp/secret-job.gcode"};
        char *out = NULL;
        char *err = NULL;
        run_cli(3, argv, &out, &err);
        check(err != NULL && strstr(err, "secret-job.gcode") == NULL,
              "logs never contain the print file path");
        check(err != NULL && strstr(err, "command=print") != NULL, "logs still name the command as 'print'");
        free(out);
        free(err);
    }
    {
        char *argv[] = {(char *)"printerctl", (char *)"send", (char *)"M117 secret payload"};
        char *out = NULL;
        char *err = NULL;
        run_cli(3, argv, &out, &err);
        check(err != NULL && strstr(err, "secret payload") == NULL, "logs never contain the raw send content");
        check(err != NULL && strstr(err, "command=send") != NULL, "logs still name the command as 'send'");
        free(out);
        free(err);
    }
}

static void test_usage_error_adds_single_error_log_line(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--bogus", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "unknown option still exits with usage error");
    check(err != NULL && count_occurrences(err, "\tERROR\terror\t") == 1,
          "usage error adds exactly one ERROR log line alongside the human-readable message");
    check(err != NULL && strstr(err, "--bogus") != NULL, "human-readable usage message is preserved");

    free(out);
    free(err);
}

static void test_config_error_adds_single_error_log_line(void)
{
    const char *path = "/nonexistent/printerctl.conf";
    char *argv[] = {(char *)"printerctl", (char *)"--config", (char *)path, (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_CONFIG_ERROR, "missing config file still exits with configuration error");
    check(err != NULL && count_occurrences(err, "\tERROR\terror\t") == 1,
          "configuration error adds exactly one ERROR log line alongside the human-readable message");
    check(err != NULL && strstr(err, "configuration error") != NULL, "human-readable configuration message is preserved");
    check(err != NULL && count_occurrences(err, path) == 1,
          "the --config path appears exactly once in stderr, in the human-readable message only");

    if (err != NULL) {
        const char *error_line = strstr(err, "\tERROR\terror\t");
        check(error_line != NULL && strstr(error_line, "configuration error") != NULL,
              "the structured ERROR log line uses the generic 'configuration error' detail");
        check(error_line != NULL && strstr(error_line, path) == NULL,
              "the structured ERROR log line never contains the --config path");
    }

    free(out);
    free(err);
}

/* Writes `content` to a fresh temp G-code file and returns its path via `path`. */
static void write_temp_gcode(const char *content, char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/printerctl_cli_gcode_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "FAIL: mkstemp() failed for temp G-code fixture\n");
        g_failures++;
        return;
    }

    FILE *file = fdopen(fd, "w");
    fputs(content, file);
    fclose(file);
}

/* Points XDG_STATE_HOME at a fresh temp directory; records land there. */
static bool set_temp_state_home(char *dir, size_t dir_size)
{
    snprintf(dir, dir_size, "/tmp/printerctl_cli_state_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp() failed for the state home fixture\n");
        g_failures++;
        return false;
    }
    setenv("XDG_STATE_HOME", dir, 1);
    return true;
}

/* Restores the suite-wide disposable state home and removes `dir`. */
static void drop_temp_state_home(const char *dir)
{
    char command[128];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    if (system(command) != 0) {
        fprintf(stderr, "FAIL: cannot remove the state home fixture\n");
        g_failures++;
    }
    setenv("XDG_STATE_HOME", g_default_state_home, 1);
}

static void test_dry_run_print_success(void)
{
    char path[64];
    write_temp_gcode("; header\nG21\nG90 ; absolute\n\nM104 S200\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "a clean dry-run print exits with success");
    check(out != NULL && strstr(out, "dry run") != NULL, "dry-run output announces the dry run");
    check(out != NULL && strstr(out, "G21") != NULL, "dry-run output lists the first command");
    check(out != NULL && strstr(out, "G90") != NULL, "dry-run output lists a comment-stripped command");
    check(out != NULL && strstr(out, "M104 S200") != NULL, "dry-run output lists the last command");
    check(out != NULL && strstr(out, "absolute") == NULL, "dry-run output strips inline comments");
    check(out != NULL && strstr(out, "line 2") != NULL, "dry-run output reports source line numbers");
    check(out != NULL && strstr(out, "100%") != NULL, "dry-run output reports progress reaching 100%");
    check(out != NULL && strstr(out, "3 command(s)") != NULL, "dry-run summary counts the commands");

    check(err != NULL && strstr(err, "\tINFO\tstate_transition\tstate=dry_run_print_started") != NULL,
          "log includes the dry-run start state transition");
    check(err != NULL && strstr(err, "\tINFO\tprogress\tcommands=3 lines=5 percent=100") != NULL,
          "log includes the summarizing INFO progress record");
    check(err != NULL && strstr(err, "state=dry_run_print_completed status=ok") != NULL,
          "log includes the dry-run completion state transition");
    check(err != NULL && strstr(err, path) == NULL, "logs never contain the G-code file path");
    check(err != NULL && strstr(err, "M104") == NULL, "logs never contain G-code command text");
    check(err != NULL && strstr(err, "command_unavailable") == NULL,
          "a dry-run print is not reported as unavailable");
    check(err != NULL && strstr(err, "\tINFO\tsession_end\t") != NULL, "session_end is still logged");

    remove(path);
    free(out);
    free(err);
}

static void test_dry_run_print_verbose_logs_per_command_progress(void)
{
    char path[64];
    write_temp_gcode("G21\nG90\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"--verbose", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "a verbose dry-run print exits with success");
    check(err != NULL && count_occurrences(err, "\tDEBUG\tprogress\t") == 2,
          "verbose dry run logs one DEBUG progress record per command");

    remove(path);
    free(out);
    free(err);
}

static void test_dry_run_print_non_verbose_omits_per_command_progress(void)
{
    char path[64];
    write_temp_gcode("G21\nG90\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    run_cli(4, argv, &out, &err);

    check(err != NULL && count_occurrences(err, "\tDEBUG\t") == 0,
          "non-verbose dry run logs no DEBUG records");
    check(err != NULL && count_occurrences(err, "\tINFO\tprogress\t") == 1,
          "non-verbose dry run still logs the one summarizing progress record");

    remove(path);
    free(out);
    free(err);
}

static void test_dry_run_print_empty_file(void)
{
    char path[64];
    write_temp_gcode("", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "a dry run of an empty file exits with success");
    check(out != NULL && strstr(out, "0 command(s)") != NULL, "empty-file dry-run summary reports zero commands");

    remove(path);
    free(out);
    free(err);
}

static void test_dry_run_print_allows_g28(void)
{
    char path[64];
    write_temp_gcode("G21\nG28\nG28 X\nG28 Y\nG28 Z\nG90\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "a G-code file containing G28 passes dry run");
    check(out != NULL && strstr(out, "line 2: G28\n") != NULL &&
              strstr(out, "line 3: G28 X\n") != NULL &&
              strstr(out, "line 4: G28 Y\n") != NULL &&
              strstr(out, "line 5: G28 Z\n") != NULL,
          "dry run renders homing commands from the source");
    check(out != NULL && strstr(out, "6 command(s)") != NULL && strstr(out, "G90") != NULL,
          "dry run processes the entire file including commands after homing");
    check(err != NULL && count_occurrences(err, "\tERROR\terror\t") == 0,
          "G28 in a file produces no safety error log");
    check(err != NULL && strstr(err, path) == NULL, "logs never contain the G-code file path");

    remove(path);
    free(out);
    free(err);
}

static void test_dry_run_print_missing_file(void)
{
    const char *path = "/nonexistent/printerctl-job.gcode";
    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"print", (char *)path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_GCODE_ERROR, "a missing G-code file exits with the G-code error code");
    check(err != NULL && strstr(err, "dry run failed") != NULL, "the missing-file message is explicit");
    check(err != NULL && count_occurrences(err, path) == 1,
          "the file path appears exactly once in stderr, in the human-readable message only");

    if (err != NULL) {
        const char *error_line = strstr(err, "\tERROR\terror\t");
        check(error_line != NULL && strstr(error_line, "status=open_failed") != NULL,
              "the structured ERROR record carries the open_failed status");
        check(error_line != NULL && strstr(error_line, path) == NULL,
              "the structured ERROR record never contains the file path");
    }

    free(out);
    free(err);
}

static void test_dry_run_print_malformed_file(void)
{
    char path[64];
    write_temp_gcode("G21\nG1 (never closed\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_GCODE_ERROR, "a malformed G-code file exits with the G-code error code");
    check(err != NULL && strstr(err, "line 2") != NULL, "the malformed-file message names the source line");
    check(err != NULL && strstr(err, "status=malformed") != NULL,
          "the structured ERROR record carries the malformed status");

    remove(path);
    free(out);
    free(err);
}

static void test_print_without_port_does_not_touch_the_file(void)
{
    char path[64];
    write_temp_gcode("G21\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "'print' without a daemon fails as a communication error before local parsing");
    check(out != NULL && strstr(out, "G21") == NULL,
          "no command is parsed or reported when the connection cannot be made");

    remove(path);
    free(out);
    free(err);
}

static void test_dry_run_from_config_file_enables_dry_run_print(void)
{
    char gcode_path[64];
    write_temp_gcode("G21\n", gcode_path, sizeof(gcode_path));

    char config_path[64];
    write_temp_config("dry_run=true\n", config_path, sizeof(config_path));

    char *argv[] = {(char *)"printerctl", (char *)"--config", config_path, (char *)"print", gcode_path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "dry_run=true from the config file enables the dry-run print path");
    check(out != NULL && strstr(out, "G21") != NULL, "config-enabled dry run parses the file");

    remove(gcode_path);
    remove(config_path);
    free(out);
    free(err);
}

static void test_temp_end_to_end(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"temp"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "'temp' without daemon exits with communication error");
    check(err != NULL && strstr(err, "connect(") != NULL, "'temp' reports daemon connection");
    free(out);
    free(err);
}

static void test_position_end_to_end(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"position"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "'position' without daemon exits with communication error");
    free(out);
    free(err);
}

static void test_info_end_to_end(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"info"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "'info' without daemon exits with communication error");
    free(out);
    free(err);
}

static void test_send_end_to_end_logs_full_exchange(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--verbose", (char *)"send", (char *)"M500"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR, "migrated 'send' fails cleanly when daemon is unavailable");
    check(err != NULL && strstr(err, "\tDEBUG\tcommand_parsed\tcommand=send") != NULL,
          "migrated 'send' still logs CLI parsing");
    check(err != NULL && strstr(err, "connect") != NULL, "the daemon connection error is reported");

    free(out);
    free(err);
}

static void test_send_g28_blocked_end_to_end(void)
{
    {
        char *argv[] = {(char *)"printerctl", (char *)"send", (char *)"G28"};
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(3, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_SAFETY_ERROR,
              "'send G28' is rejected before requiring a configured port");
        check(err != NULL && strstr(err, "G28") != NULL, "the no-port G28 rejection names G28");

        free(out);
        free(err);
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", (char *)"/tmp/unused",
                     (char *)"send", (char *)"G28"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_SAFETY_ERROR, "'send G28' exits with the safety error code");
    check(err != NULL && strstr(err, "G28") != NULL, "the rejection names G28");
    check(err != NULL && strstr(err, "'home' command") != NULL, "the rejection explains why");

    free(out);
    free(err);
}

static void test_printer_error_end_to_end(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"send", (char *)"M105"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "migrated 'send' reports daemon unavailable without falling back to serial");

    free(out);
    free(err);
}

static void test_timeout_end_to_end(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--timeout", (char *)"150",
                     (char *)"send", (char *)"M105"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR, "migrated 'send' does not use CLI timeout as serial config");
    check(err != NULL && strstr(err, "connect") != NULL, "the daemon connection error is reported");

    free(out);
    free(err);
}

static void test_pause_resume_cancel_use_daemon_end_to_end(void)
{
    /* M23: bare pause/resume/cancel are daemon-owned host-print controls,
     * migrated exactly like send/temp/home -- with no daemon listening,
     * they fail cleanly the same way those commands do, never falling
     * back to a client-owned execution model. */
    const char *commands[] = {"pause", "resume", "cancel"};

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char *argv[] = {(char *)"printerctl", (char *)commands[i]};
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(2, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_COMM_ERROR,
              "bare pause/resume/cancel report a clean communication error with no daemon");
        check(err != NULL && strstr(err, "connect") != NULL,
              "the daemon connection error is reported");

        free(out);
        free(err);
    }
}

static void test_dry_run_host_controls(void)
{
    const char *commands[] = {"pause", "resume", "cancel"};

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)commands[i]};
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(3, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_OK,
              "a dry-run host-print control exits with success without a daemon");
        check(out != NULL && strstr(out, commands[i]) != NULL,
              "the dry-run host-print control names the requested action");
        check(out != NULL && strstr(out, "no serial device is opened") != NULL,
              "the dry-run host-print control reports no serial access");

        free(out);
        free(err);
    }
}

static void test_dry_run_sd_controls(void)
{
    const struct {
        const char *command;
        const char *would_send;
    } cases[] = {
        {"pause", "would send 'M25'"},
        {"resume", "would send 'M24'"},
        {"cancel", "would send 'M524'"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"sd",
                         (char *)cases[i].command};
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(4, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_OK, "a dry-run SD-print control exits with success without a port");
        check(out != NULL && strstr(out, cases[i].would_send) != NULL,
              "the dry-run SD-print control reports the Marlin command it would send");

        free(out);
        free(err);
    }
}

static void test_dry_run_single_commands(void)
{
    /* --dry-run works without a port: nothing is opened. */
    {
        char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"temp"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(3, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_OK, "'--dry-run temp' exits with success without a port");
        check(out != NULL && strstr(out, "would send 'M105'") != NULL,
              "'--dry-run temp' reports the M105 it would send");
        free(out);
        free(err);
    }
    {
        char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"send", (char *)"M400"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(4, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_OK, "'--dry-run send' exits with success");
        check(out != NULL && strstr(out, "would send 'M400'") != NULL,
              "'--dry-run send' reports the raw command it would send");
        free(out);
        free(err);
    }
    {
        /* The safety policy holds even when nothing would be transmitted. */
        char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"send", (char *)"G28"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(4, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_SAFETY_ERROR,
              "'--dry-run send G28' still exits with the safety error code");
        free(out);
        free(err);
    }
}

/* Milestone 10: `power` grammar -- a required, fixed subcommand. */
static void test_power_requires_low_battery_subcommand(void)
{
    {
        char *argv[] = {(char *)"printerctl", (char *)"power"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(2, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'power' alone is a usage error");
        check(err != NULL && strstr(err, "low-battery") != NULL,
              "the usage error names the expected subcommand");
        free(out);
        free(err);
    }
    {
        char *argv[] = {(char *)"printerctl", (char *)"power", (char *)"nonsense"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(3, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "an unrecognized 'power' subcommand is a usage error");
        free(out);
        free(err);
    }
    {
        char *argv[] = {(char *)"printerctl", (char *)"power", (char *)"low-battery", (char *)"extra"};
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(4, argv, &out, &err);
        check(status == PRINTERCTL_EXIT_USAGE_ERROR, "extra arguments after 'low-battery' are a usage error");
        free(out);
        free(err);
    }
}

/* `power low-battery` never needs a port and never opens a serial device. */
static void test_power_low_battery_dry_run(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "'--dry-run power low-battery' exits with success without a port");
    check(out != NULL && strstr(out, "dry run") != NULL, "the dry run reports itself as such");

    free(out);
    free(err);
}

static void test_power_low_battery_with_no_active_print(void)
{
    char state_home[64];
    if (!set_temp_state_home(state_home, sizeof(state_home))) {
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK,
          "'power low-battery' with no print running is a safe no-op, not an error");
    check(out != NULL && strstr(out, "no active print") != NULL,
          "the operator is told nothing was cancelled");
    check(err != NULL && strstr(err, "\tWARNING\t") == NULL,
          "no warning is produced when there is simply nothing to cancel");

    /* Running it again must behave identically: idempotent. */
    free(out);
    free(err);
    out = NULL;
    err = NULL;
    status = run_cli(3, argv, &out, &err);
    check(status == PRINTERCTL_EXIT_OK, "'power low-battery' is idempotent with no active print");

    free(out);
    free(err);
    drop_temp_state_home(state_home);
}

static void test_power_low_battery_cleans_up_a_stale_marker(void)
{
    char state_home[64];
    if (!set_temp_state_home(state_home, sizeof(state_home))) {
        return;
    }

    /* A marker naming a PID that is guaranteed not to exist: fork a
     * child that exits immediately and reap it, so its PID is free. */
    pid_t dead = fork();
    if (dead == 0) {
        _exit(0);
    }
    int child_status = 0;
    waitpid(dead, &child_status, 0);

    char record_dir[128];
    snprintf(record_dir, sizeof(record_dir), "%s/printerctl", state_home);
    if (mkdir(record_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "FAIL: cannot create the record directory fixture\n");
        g_failures++;
        drop_temp_state_home(state_home);
        return;
    }
    char marker_path[192];
    snprintf(marker_path, sizeof(marker_path), "%s/active-print.pid", record_dir);
    FILE *marker = fopen(marker_path, "w");
    check(marker != NULL, "the stale marker fixture is created");
    if (marker != NULL) {
        fprintf(marker, "%ld\n", (long)dead);
        fclose(marker);
    }

    char *argv[] = {(char *)"printerctl", (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;
    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "a stale marker is not an error");
    check(out != NULL && strstr(out, "stale marker") != NULL,
          "the operator is told the marker was stale");

    struct stat info;
    check(stat(marker_path, &info) != 0, "the stale marker is removed");

    free(out);
    free(err);
    drop_temp_state_home(state_home);
}

static void test_safety_error_adds_single_error_log_line(void)
{
    /* 'home' no longer produces a safety rejection now that min
     * end-stops are enabled; 'send "G28"' is still unconditionally
     * refused (see test_send_g28_blocked_end_to_end) and remains a
     * safety-rejection vehicle. */
    char *argv[] = {(char *)"printerctl", (char *)"send", (char *)"G28"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_SAFETY_ERROR, "sending G28 still exits with safety error");
    check(err != NULL && count_occurrences(err, "\tERROR\terror\t") == 1,
          "safety rejection adds exactly one ERROR log line alongside the human-readable message");
    check(err != NULL && strstr(err, "only available through the 'home' command") != NULL,
          "human-readable safety message is preserved");

    free(out);
    free(err);
}

/* ---- M29: macro and config from-prusa commands ---- */

static void expect_usage_error(int argc, char *argv[], const char *description)
{
    char *out = NULL;
    char *err = NULL;
    int status = run_cli(argc, argv, &out, &err);
    check(status == PRINTERCTL_EXIT_USAGE_ERROR, description);
    check(err != NULL && strstr(err, "printerctl: ") != NULL, "usage error is explained");
    free(out);
    free(err);
}

static void test_macro_and_config_grammar(void)
{
    char long_id[PRINTERCTL_PLP1_MACRO_ID_MAX + 1];
    memset(long_id, 'a', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';

    char *a1[] = {"printerctl", "macro"};
    expect_usage_error(2, a1, "'macro' requires a subcommand");
    char *a2[] = {"printerctl", "macro", "delete"};
    expect_usage_error(3, a2, "unknown 'macro' subcommand is rejected");
    char *a3[] = {"printerctl", "macro", "list", "extra"};
    expect_usage_error(4, a3, "'macro list' takes no arguments");
    char *a4[] = {"printerctl", "macro", "run"};
    expect_usage_error(3, a4, "'macro run' requires an ID");
    char *a5[] = {"printerctl", "macro", "run", ""};
    expect_usage_error(4, a5, "'macro run' rejects an empty ID");
    char *a6[] = {"printerctl", "macro", "run", long_id};
    expect_usage_error(4, a6, "'macro run' rejects an over-length ID");
    char *a7[] = {"printerctl", "macro", "run", "park", "now"};
    expect_usage_error(5, a7, "'macro run' rejects extra arguments");
    char *a8[] = {"printerctl", "config"};
    expect_usage_error(2, a8, "'config' requires a subcommand");
    char *a9[] = {"printerctl", "config", "from-cura", "x.ini"};
    expect_usage_error(4, a9, "unknown 'config' subcommand is rejected");
    char *a10[] = {"printerctl", "config", "from-prusa"};
    expect_usage_error(3, a10, "'config from-prusa' requires a FILE");
    char *a11[] = {"printerctl", "config", "from-prusa", "x.ini", "-1"};
    expect_usage_error(5, a11, "negative INSET_MM is rejected at parse time");
    char *a12[] = {"printerctl", "config", "from-prusa", "x.ini", "30", "0"};
    expect_usage_error(6, a12, "non-positive SAFE_Z_MM is rejected at parse time");
    char *a13[] = {"printerctl", "config", "from-prusa", "x.ini", "30", "5", "9"};
    expect_usage_error(7, a13, "'config from-prusa' rejects extra arguments");
    char *a14[] = {"printerctl", "config", "from-prusa", "x.ini", "wide"};
    expect_usage_error(5, a14, "non-numeric INSET_MM is rejected");
}

static void test_dry_run_macro_commands(void)
{
    char *run_argv[] = {"printerctl", "--dry-run", "macro", "run", "calibrate-center"};
    char *out = NULL;
    char *err = NULL;
    int status = run_cli(5, run_argv, &out, &err);
    check(status == PRINTERCTL_EXIT_OK, "dry-run macro run exits 0 without a daemon");
    check(out != NULL && strstr(out, "calibrate-center") != NULL &&
              strstr(out, "no serial device is opened") != NULL,
          "dry-run macro run names the macro and reports no serial access");
    free(out);
    free(err);

    char *list_argv[] = {"printerctl", "--dry-run", "macro", "list"};
    status = run_cli(4, list_argv, &out, &err);
    check(status == PRINTERCTL_EXIT_OK, "dry-run macro list exits 0 without a daemon");
    free(out);
    free(err);
}

static void test_macro_commands_need_daemon(void)
{
    char *list_argv[] = {"printerctl", "macro", "list"};
    char *run_argv[] = {"printerctl", "macro", "run", "maintenance-z-lift"};
    char **cases[] = {list_argv, run_argv};
    int counts[] = {3, 4};
    for (size_t i = 0; i < 2; i++) {
        char *out = NULL;
        char *err = NULL;
        int status = run_cli(counts[i], cases[i], &out, &err);
        check(status == PRINTERCTL_EXIT_COMM_ERROR,
              "macro commands report a clean communication error with no daemon");
        check(err != NULL && strstr(err, "connect") != NULL,
              "macro commands report the daemon connection error");
        free(out);
        free(err);
    }
}

static void test_config_from_prusa_generates_calibration(void)
{
    char path[64];
    write_temp_config("bed_shape = 0x0,235x0,235x235,0x235\n"
                      "machine_max_feedrate_x = 150,150\n"
                      "machine_max_feedrate_y = 150,150\n"
                      "machine_max_feedrate_z = 5,5\n"
                      "max_print_height = 270\n",
                      path, sizeof(path));

    char *argv[] = {"printerctl", "config", "from-prusa", path};
    char *out = NULL;
    char *err = NULL;
    int status = run_cli(4, argv, &out, &err);
    check(status == PRINTERCTL_EXIT_OK, "config from-prusa succeeds on a printer profile");
    check(out != NULL && strstr(out, "calibration.min_x=30.000\n") != NULL &&
              strstr(out, "calibration.max_y=205.000\n") != NULL &&
              strstr(out, "calibration.safe_z=5.000\n") != NULL &&
              strstr(out, "calibration.xy_feedrate=9000.000\n") != NULL &&
              strstr(out, "calibration.z_feedrate=300.000\n") != NULL,
          "config from-prusa prints the derived calibration keys on stdout");
    check(out != NULL && strstr(out, "\nport=") == NULL,
          "config from-prusa never invents an active serial port");
    check(err != NULL && strstr(err, "calibration.") == NULL,
          "the generated configuration goes to stdout, not the log stream");
    free(out);
    free(err);

    char *argv_custom[] = {"printerctl", "config", "from-prusa", path, "10", "2.5"};
    status = run_cli(6, argv_custom, &out, &err);
    check(status == PRINTERCTL_EXIT_OK && out != NULL &&
              strstr(out, "calibration.min_x=10.000\n") != NULL &&
              strstr(out, "calibration.safe_z=2.500\n") != NULL,
          "config from-prusa applies INSET_MM and SAFE_Z_MM");
    free(out);
    free(err);

    char *argv_wide[] = {"printerctl", "config", "from-prusa", path, "200"};
    status = run_cli(5, argv_wide, &out, &err);
    check(status == PRINTERCTL_EXIT_USAGE_ERROR && err != NULL &&
              strstr(err, "no usable area") != NULL,
          "an inset larger than the bed is a usage error");
    free(out);
    free(err);
    unlink(path);

    char *argv_missing[] = {"printerctl", "config", "from-prusa",
                            "/nonexistent/printerctl/BiquB1.ini"};
    status = run_cli(4, argv_missing, &out, &err);
    check(status == PRINTERCTL_EXIT_CONFIG_ERROR, "a missing PrusaSlicer file is a config error");
    free(out);
    free(err);
}

int main(void)
{
    /* No test may write into the developer's real state directory. */
    snprintf(g_default_state_home, sizeof(g_default_state_home),
             "/tmp/printerctl_cli_state_XXXXXX");
    if (mkdtemp(g_default_state_home) == NULL) {
        fprintf(stderr, "FAIL: cannot create the suite state home\n");
        return EXIT_FAILURE;
    }
    setenv("XDG_STATE_HOME", g_default_state_home, 1);

    test_macro_and_config_grammar();
    test_dry_run_macro_commands();
    test_macro_commands_need_daemon();
    test_config_from_prusa_generates_calibration();

    test_help();
    test_version();
    test_missing_command();
    test_unknown_global_option();
    test_help_rejects_extra_arguments();
    test_equals_form_rejected();
    test_duplicate_global_option();
    test_duplicate_boolean_flag();
    test_missing_option_value();
    test_port_empty_value();
    test_print_empty_file();
    test_send_empty_command();
    test_invalid_baud_value();
    test_invalid_timeout_value();
    test_unknown_command();
    test_print_command_needs_daemon();
    test_print_missing_file();
    test_print_extra_arguments();
    test_send_command();
    test_send_missing_argument();
    test_home_no_longer_mentions_retired_confirmation_flag();
    test_home_axis_argument_grammar();
    test_dry_run_home();
    test_migrated_print_ignores_hardware_options();
    test_global_option_after_command_rejected();
    test_cli_value_equal_to_default();
    test_config_file_precedence();
    test_config_file_not_found();
    test_config_file_invalid();
    test_config_file_duplicate_key();
    test_config_missing_value();
    test_parse_direct_contract();
    test_help_and_version_produce_no_logs();
    test_successful_command_logs_session_lifecycle();
    test_verbose_adds_command_parsed_log();
    test_config_summary_omits_raw_port_and_config_path();
    test_print_and_send_do_not_leak_content();
    test_usage_error_adds_single_error_log_line();
    test_config_error_adds_single_error_log_line();
    test_safety_error_adds_single_error_log_line();
    test_dry_run_print_success();
    test_dry_run_print_verbose_logs_per_command_progress();
    test_dry_run_print_non_verbose_omits_per_command_progress();
    test_dry_run_print_empty_file();
    test_dry_run_print_allows_g28();
    test_dry_run_print_missing_file();
    test_dry_run_print_malformed_file();
    test_print_without_port_does_not_touch_the_file();
    test_dry_run_from_config_file_enables_dry_run_print();
    test_temp_end_to_end();
    test_position_end_to_end();
    test_info_end_to_end();
    test_send_end_to_end_logs_full_exchange();
    test_send_g28_blocked_end_to_end();
    test_printer_error_end_to_end();
    test_timeout_end_to_end();
    test_pause_resume_cancel_use_daemon_end_to_end();
    test_dry_run_host_controls();
    test_dry_run_sd_controls();
    test_dry_run_single_commands();
    test_power_requires_low_battery_subcommand();
    test_power_low_battery_dry_run();
    test_power_low_battery_with_no_active_print();
    test_power_low_battery_cleans_up_a_stale_marker();

    {
        char command[128];
        snprintf(command, sizeof(command), "rm -rf '%s'", g_default_state_home);
        if (system(command) != 0) {
            fprintf(stderr, "FAIL: cannot remove the suite state home\n");
            g_failures++;
        }
    }

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All CLI checks passed\n");
    return EXIT_SUCCESS;
}
