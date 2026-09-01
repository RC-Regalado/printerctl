/* XOPEN 700 for posix_openpt/grantpt/unlockpt/ptsname (and POSIX.1-2008). */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "printerctl/cli.h"
#include "printerctl/config.h"
#include "printerctl/version.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

static void test_print_command_needs_port(void)
{
    /* Since Milestone 7 'print' streams for real; without a configured
     * port it is a configuration error, not "unavailable". */
    char *argv[] = {(char *)"printerctl", (char *)"print", (char *)"file.gcode"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_CONFIG_ERROR, "valid 'print' without a port is a configuration error");
    check(err != NULL && strstr(err, "--port") != NULL, "the no-port error tells the operator what to pass");

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

static void test_printer_commands_need_port(void)
{
    /* M21 migrated explicit physical single commands to the daemon.
     * Only legacy 'print FILE' keeps direct serial ownership. */
    char path[64];
    write_temp_gcode("M105\n", path, sizeof(path));
    char *argv[] = {(char *)"printerctl", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_CONFIG_ERROR, "legacy print without a port is still a configuration error");
    check(err != NULL && strstr(err, "--port") != NULL, "the no-port error tells the operator what to pass");

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

    /* Use legacy print here because M21 migrated home to daemon/client
     * config and it must not consume hardware config anymore. */
    char gcode_path[64];
    write_temp_gcode("M105\n", gcode_path, sizeof(gcode_path));
    char *argv[] = {(char *)"printerctl", (char *)"--config", path,
                     (char *)"--port", (char *)"/dev/from-cli",
                     (char *)"print", gcode_path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(7, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "'print' with the resolved config attempts to open /dev/from-cli, which does not exist");
    check(err != NULL && strstr(err, "/dev/from-cli") != NULL,
          "CLI port overrides the config file port (the connection failure names the CLI port)");
    check(err != NULL && strstr(err, "/dev/from-file") == NULL, "the config file's port never took effect");
    check(err != NULL && strstr(err, "baud=57600") != NULL,
          "config file baud is applied when CLI does not override it");

    remove(path);
    remove(gcode_path);
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

    /* Legacy print still attempts a direct connection and fails (the port
     * does not exist), so the port legitimately appears in the
     * human-readable failure message elsewhere in `err` -- the
     * assertions below check the config_summary log line specifically,
     * not the whole buffer, matching the Milestone 2 privacy rules
     * (structured log never leaks it; human-readable output may). */
    char gcode_path[64];
    write_temp_gcode("M105\n", gcode_path, sizeof(gcode_path));
    char *argv[] = {(char *)"printerctl", (char *)"--port", (char *)"/dev/ttyACM0",
                     (char *)"--config", path,
                     (char *)"print", gcode_path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(7, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "port+config resolved 'print' attempts a real connection and fails to open the nonexistent port");

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
    check(err != NULL && strstr(err, "/dev/ttyACM0") != NULL,
          "the human-readable connection-failure message may still name the port");

    remove(path);
    remove(gcode_path);
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

/* Finds a session record under `state_home`/printerctl; returns the count. */
static int find_session_records(const char *state_home, char *out_path, size_t out_size)
{
    char record_dir[128];
    snprintf(record_dir, sizeof(record_dir), "%s/printerctl", state_home);

    DIR *dir = opendir(record_dir);
    if (dir == NULL) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "session-", 8) == 0) {
            count++;
            if (out_path != NULL) {
                snprintf(out_path, out_size, "%s/%s", record_dir, entry->d_name);
            }
        }
    }
    closedir(dir);
    return count;
}

/* Reads a whole file into `buffer`; returns false when it cannot be read. */
static bool read_whole_file(const char *path, char *buffer, size_t buffer_size)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    size_t n = fread(buffer, 1, buffer_size - 1, file);
    buffer[n] = '\0';
    fclose(file);
    return true;
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

static void test_dry_run_print_unsafe_g28(void)
{
    char path[64];
    write_temp_gcode("G21\nG28\nG90\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_SAFETY_ERROR, "a G-code file containing G28 exits with the safety error code");
    check(err != NULL && strstr(err, "G28") != NULL, "the G28 rejection message names the command");
    check(err != NULL && strstr(err, "line 2") != NULL, "the G28 rejection message names the source line");
    check(err != NULL && count_occurrences(err, "\tERROR\terror\t") == 1,
          "the G28 rejection adds exactly one ERROR log line");
    check(err != NULL && strstr(err, "status=unsafe_g28") != NULL,
          "the structured ERROR record carries the unsafe_g28 status");
    check(err != NULL && strstr(err, "state=dry_run_print_failed") != NULL,
          "the failed dry run logs its state transition");
    check(err != NULL && strstr(err, path) == NULL, "logs never contain the G-code file path");
    check(out != NULL && strstr(out, "G21") != NULL, "commands before the G28 line were still reported");

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

    check(status == PRINTERCTL_EXIT_CONFIG_ERROR,
          "'print' without a port fails as a configuration error before streaming");
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

/*
 * Scriptable mock Marlin printer for end-to-end CLI tests: a pty whose
 * master side is serviced by a forked child. The child waits for one
 * complete command line, writes the scripted reply (which may be
 * several lines), then keeps draining input until the parent kills it.
 * The child must run concurrently with printerctl_cli_run() because the
 * serial open flushes any input scripted in advance.
 */
typedef struct {
    int master;
    pid_t child;
    char slave_path[64];
} cli_mock_printer_t;

/*
 * Streaming variant of the mock: one scripted reply per received
 * command line, in order, then silence (drain until killed). Used by
 * the Milestone 7 print tests, where each acknowledged command triggers
 * the transmission of the next one.
 */
static bool cli_mock_start_script(cli_mock_printer_t *mock,
                                   const char *const *replies, size_t reply_count)
{
    mock->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (mock->master < 0 || grantpt(mock->master) != 0 || unlockpt(mock->master) != 0) {
        fprintf(stderr, "FAIL: cannot create pty for the scripted mock printer\n");
        g_failures++;
        return false;
    }

    const char *name = ptsname(mock->master);
    if (name == NULL || strlen(name) >= sizeof(mock->slave_path)) {
        fprintf(stderr, "FAIL: ptsname for the scripted mock printer\n");
        g_failures++;
        close(mock->master);
        return false;
    }
    strcpy(mock->slave_path, name);

    mock->child = fork();
    if (mock->child < 0) {
        fprintf(stderr, "FAIL: cannot fork the scripted mock printer\n");
        g_failures++;
        close(mock->master);
        return false;
    }

    if (mock->child == 0) {
        char c;
        for (size_t i = 0; i < reply_count; i++) {
            for (;;) {
                ssize_t n = read(mock->master, &c, 1);
                if (n <= 0) {
                    _exit(1);
                }
                if (c == '\n') {
                    break;
                }
            }
            size_t length = strlen(replies[i]);
            if (write(mock->master, replies[i], length) != (ssize_t)length) {
                _exit(1);
            }
        }
        for (;;) {
            ssize_t n = read(mock->master, &c, 1);
            if (n <= 0) {
                _exit(0);
            }
        }
    }

    return true;
}

static void cli_mock_stop(cli_mock_printer_t *mock)
{
    kill(mock->child, SIGKILL);
    waitpid(mock->child, NULL, 0);
    close(mock->master);
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

static void test_print_end_to_end(void)
{
    char path[64];
    write_temp_gcode("; header\nG21\nG90 ; absolute\nM105\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n", "ok\n", "ok\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script(&mock, replies, 3)) {
        remove(path);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"--verbose", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(6, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "'print' against the mock printer exits with success");
    check(out != NULL && strstr(out, "print completed") != NULL,
          "the session summary reports completion");
    check(out != NULL && strstr(out, "3 command(s) sent, 3 acknowledged") != NULL,
          "the session summary counts sent and acknowledged commands");
    check(out != NULL && strstr(out, "100%") != NULL, "progress reaches 100%");
    check(out != NULL && strstr(out, path) != NULL, "the summary names the file");
    check(err != NULL && strstr(err, "\tDEBUG\tserial_tx\tG21") != NULL,
          "each streamed command is logged as serial_tx");
    check(err != NULL && strstr(err, "\tDEBUG\tserial_tx\tM105") != NULL,
          "the last streamed command is logged as serial_tx");
    check(err != NULL && strstr(err, "state=printing") != NULL,
          "the print start state transition is logged");
    check(err != NULL && strstr(err, "state=print_completed") != NULL,
          "the print completion state transition is logged");
    check(err != NULL && strstr(err, "\tINFO\tprogress\t") != NULL,
          "progress is logged at INFO on percent changes");
    check(err != NULL && strstr(err, path) == NULL, "logs never contain the G-code file path");

    cli_mock_stop(&mock);
    remove(path);
    free(out);
    free(err);
}

static void test_print_error_stops_gracefully_end_to_end(void)
{
    char path[64];
    write_temp_gcode("G21\nG90\nM105\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n", "Error:Thermal Runaway\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script(&mock, replies, 2)) {
        remove(path);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_PRINTER_ERROR,
          "an Error: reply during a print exits with the printer error code");
    check(err != NULL && strstr(err, "Thermal Runaway") != NULL,
          "the printer's error message reaches the operator");
    check(out != NULL && strstr(out, "print failed") != NULL,
          "the session summary reports the failure");
    check(out != NULL && strstr(out, "2 command(s) sent, 1 acknowledged") != NULL,
          "the summary shows the exchange stopped at the failing command");
    check(out != NULL && strstr(out, "last acknowledged line 1") != NULL,
          "the summary names the last acknowledged source line");

    cli_mock_stop(&mock);
    remove(path);
    free(out);
    free(err);
}

static void test_print_g28_in_file_end_to_end(void)
{
    char path[64];
    write_temp_gcode("G21\nG28\nG90\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script(&mock, replies, 1)) {
        remove(path);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_SAFETY_ERROR,
          "a G28 in the streamed file exits with the safety error code");
    check(err != NULL && strstr(err, "G28") != NULL, "the rejection names G28");
    check(out != NULL && strstr(out, "1 command(s) sent, 1 acknowledged") != NULL,
          "commands before the G28 line were streamed; the G28 line was not");

    cli_mock_stop(&mock);
    remove(path);
    free(out);
    free(err);
}

static void test_print_timeout_end_to_end(void)
{
    char path[64];
    write_temp_gcode("G21\nG90\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n"}; /* then silence */
    cli_mock_printer_t mock;
    if (!cli_mock_start_script(&mock, replies, 1)) {
        remove(path);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"--timeout", (char *)"150", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(7, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "a print that stops receiving acknowledgements exits with the communication error code");
    check(err != NULL && strstr(err, "timed out") != NULL, "the timeout message is explicit");
    check(out != NULL && strstr(out, "print failed") != NULL,
          "the session summary reports the failure");
    check(out != NULL && strstr(out, "2 command(s) sent, 1 acknowledged") != NULL,
          "the summary shows where streaming stopped");

    cli_mock_stop(&mock);
    remove(path);
    free(out);
    free(err);
}

static void test_pause_resume_cancel_sd_end_to_end(void)
{
    const char *commands[] = {"pause", "resume", "cancel"};

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char *argv[] = {(char *)"printerctl", (char *)commands[i]};
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(2, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_UNAVAILABLE,
              "bare pause/resume/cancel are reserved for host control until M23");
        check(err != NULL && strstr(err, "use 'sd") != NULL,
              "the operator is pointed at the SD namespace");

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

/*
 * Slow-acknowledgement variant of the scripted mock: one reply per
 * received command line, each delayed by `delay_ms`, so a streaming
 * print stays in flight long enough for the parent to drive it with
 * signals.
 */
static bool cli_mock_start_script_delayed(cli_mock_printer_t *mock,
                                           const char *const *replies, size_t reply_count,
                                           int delay_ms)
{
    mock->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (mock->master < 0 || grantpt(mock->master) != 0 || unlockpt(mock->master) != 0) {
        fprintf(stderr, "FAIL: cannot create pty for the delayed mock printer\n");
        g_failures++;
        return false;
    }

    const char *name = ptsname(mock->master);
    if (name == NULL || strlen(name) >= sizeof(mock->slave_path)) {
        fprintf(stderr, "FAIL: ptsname for the delayed mock printer\n");
        g_failures++;
        close(mock->master);
        return false;
    }
    strcpy(mock->slave_path, name);

    mock->child = fork();
    if (mock->child < 0) {
        fprintf(stderr, "FAIL: cannot fork the delayed mock printer\n");
        g_failures++;
        close(mock->master);
        return false;
    }

    if (mock->child == 0) {
        char c;
        for (size_t i = 0; i < reply_count; i++) {
            for (;;) {
                ssize_t n = read(mock->master, &c, 1);
                if (n <= 0) {
                    _exit(1);
                }
                if (c == '\n') {
                    break;
                }
            }
            poll(NULL, 0, delay_ms);
            size_t length = strlen(replies[i]);
            if (write(mock->master, replies[i], length) != (ssize_t)length) {
                _exit(1);
            }
        }
        for (;;) {
            ssize_t n = read(mock->master, &c, 1);
            if (n <= 0) {
                _exit(0);
            }
        }
    }

    return true;
}

/*
 * Runs printerctl_cli_run() in a forked child with stdout captured to a
 * temp file, so the parent can watch the output live and drive the
 * print with signals — exactly how an operator controls a host stream.
 */
typedef struct {
    pid_t pid;
    char out_path[64];
} cli_print_process_t;

static bool cli_print_start(cli_print_process_t *proc, int argc, char *argv[])
{
    snprintf(proc->out_path, sizeof(proc->out_path), "/tmp/printerctl_cli_out_XXXXXX");
    int fd = mkstemp(proc->out_path);
    if (fd < 0) {
        fprintf(stderr, "FAIL: mkstemp() failed for the CLI output capture\n");
        g_failures++;
        return false;
    }

    proc->pid = fork();
    if (proc->pid < 0) {
        fprintf(stderr, "FAIL: cannot fork the CLI process\n");
        g_failures++;
        close(fd);
        return false;
    }

    if (proc->pid == 0) {
        FILE *out = fdopen(fd, "w");
        FILE *err = fopen("/dev/null", "w");
        if (out == NULL || err == NULL) {
            _exit(127);
        }
        int code = printerctl_cli_run(argc, argv, out, err);
        fflush(out);
        _exit(code);
    }

    close(fd);
    return true;
}

/* Polls the captured output until `needle` appears (or ~3 s pass). */
static bool cli_print_wait_for_output(cli_print_process_t *proc, const char *needle)
{
    char buffer[4096];
    for (int attempt = 0; attempt < 300; attempt++) {
        FILE *file = fopen(proc->out_path, "r");
        if (file != NULL) {
            size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
            buffer[n] = '\0';
            fclose(file);
            if (strstr(buffer, needle) != NULL) {
                return true;
            }
        }
        poll(NULL, 0, 10);
    }
    return false;
}

/* Bounded wait for the CLI child; kills it rather than hanging the suite. */
static bool cli_print_wait_exit(cli_print_process_t *proc, int *out_code)
{
    for (int attempt = 0; attempt < 800; attempt++) {
        int status = 0;
        pid_t done = waitpid(proc->pid, &status, WNOHANG);
        if (done == proc->pid) {
            if (WIFEXITED(status)) {
                *out_code = WEXITSTATUS(status);
                return true;
            }
            return false;
        }
        poll(NULL, 0, 10);
    }
    kill(proc->pid, SIGKILL);
    waitpid(proc->pid, NULL, 0);
    return false;
}

/* Reads the whole captured output into `buffer`. */
static void cli_print_read_output(cli_print_process_t *proc, char *buffer, size_t buffer_size)
{
    buffer[0] = '\0';
    FILE *file = fopen(proc->out_path, "r");
    if (file != NULL) {
        size_t n = fread(buffer, 1, buffer_size - 1, file);
        buffer[n] = '\0';
        fclose(file);
    }
}

static void test_print_pause_and_resume_by_signal_end_to_end(void)
{
    char path[64];
    write_temp_gcode("G21\nG90\nM105\nM400\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n", "ok\n", "ok\n", "ok\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script_delayed(&mock, replies, 4, 150)) {
        remove(path);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"print", path};
    cli_print_process_t proc;
    if (!cli_print_start(&proc, 5, argv)) {
        cli_mock_stop(&mock);
        remove(path);
        return;
    }

    /* The signals land while acknowledgements are still pending, because
     * every mock reply is delayed 150 ms. */
    check(cli_print_wait_for_output(&proc, "printerctl: printing"), "the print announces itself");
    kill(proc.pid, SIGUSR1);
    check(cli_print_wait_for_output(&proc, "print paused"),
          "SIGUSR1 pauses the print after the current acknowledgement");
    kill(proc.pid, SIGUSR2);
    check(cli_print_wait_for_output(&proc, "print resumed"), "SIGUSR2 resumes the print");

    int code = -1;
    check(cli_print_wait_exit(&proc, &code), "the paused-then-resumed print finishes");
    check(code == PRINTERCTL_EXIT_OK, "the resumed print exits with success");

    char output[4096];
    cli_print_read_output(&proc, output, sizeof(output));
    check(strstr(output, "print completed") != NULL, "the summary reports completion");
    check(strstr(output, "4 command(s) sent, 4 acknowledged") != NULL,
          "resume continued from the next unsent command: every command streamed");

    cli_mock_stop(&mock);
    remove(proc.out_path);
    remove(path);
}

static void test_print_cancel_by_signal_end_to_end(void)
{
    char state_home[64];
    if (!set_temp_state_home(state_home, sizeof(state_home))) {
        return;
    }

    char path[64];
    write_temp_gcode("G21\nG90\nM105\nM400\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n", "ok\n", "ok\n", "ok\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script_delayed(&mock, replies, 4, 150)) {
        remove(path);
        drop_temp_state_home(state_home);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"print", path};
    cli_print_process_t proc;
    if (!cli_print_start(&proc, 5, argv)) {
        cli_mock_stop(&mock);
        remove(path);
        return;
    }

    check(cli_print_wait_for_output(&proc, "printerctl: printing"), "the print announces itself");
    kill(proc.pid, SIGINT);

    int code = -1;
    check(cli_print_wait_exit(&proc, &code), "the cancelled print finishes promptly");
    check(code == PRINTERCTL_EXIT_PRINT_CANCELLED,
          "SIGINT during a print exits with the print-cancelled code");

    char output[4096];
    cli_print_read_output(&proc, output, sizeof(output));
    check(strstr(output, "print cancelled") != NULL, "the summary reports the cancelled state");
    check(strstr(output, "print completed") == NULL, "a cancelled print never reports completion");
    check(strstr(output, "4 command(s) sent") == NULL,
          "the cancel stopped the stream before the whole file went out");

    /* A cancelled print is an interrupted session: it leaves a record. */
    char record_path[256];
    check(find_session_records(state_home, record_path, sizeof(record_path)) == 1,
          "the cancelled print left exactly one session record");
    char content[2048];
    check(read_whole_file(record_path, content, sizeof(content)) &&
              strstr(content, "state=cancelled\n") != NULL &&
              strstr(content, "cause=cancelled\n") != NULL,
          "the record carries the cancelled state and cause");

    cli_mock_stop(&mock);
    remove(proc.out_path);
    remove(path);
    drop_temp_state_home(state_home);
}

static void test_print_failure_writes_session_record(void)
{
    char state_home[64];
    if (!set_temp_state_home(state_home, sizeof(state_home))) {
        return;
    }

    char path[64];
    write_temp_gcode("G21\nG90\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n"}; /* then silence */
    cli_mock_printer_t mock;
    if (!cli_mock_start_script(&mock, replies, 1)) {
        remove(path);
        drop_temp_state_home(state_home);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"--timeout", (char *)"150", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(7, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "the session record never changes the print's exit code");
    check(out != NULL && strstr(out, "session record written to") != NULL,
          "the operator is told where the record was written");

    char record_path[256];
    check(find_session_records(state_home, record_path, sizeof(record_path)) == 1,
          "exactly one session record was written");

    char content[2048];
    check(read_whole_file(record_path, content, sizeof(content)),
          "the session record can be read back");
    check(strstr(content, "state=failed\n") != NULL, "the record carries the failed state");
    check(strstr(content, "cause=timeout\n") != NULL, "the record carries the timeout cause");
    check(content[0] != '\0' && strstr(content, path) != NULL,
          "the record names the G-code file");
    check(strstr(content, "commands_sent=2\n") != NULL &&
              strstr(content, "commands_acknowledged=1\n") != NULL &&
              strstr(content, "last_acknowledged_line=1\n") != NULL,
          "the record shows exactly where streaming stopped");
    check(strstr(content, "timestamp=") != NULL, "the record carries a timestamp");
    check(strstr(content, "never resumes") != NULL,
          "the record states the no-automatic-resume policy");

    /* Privacy: the structured log names neither the record path nor the
     * state directory — only the path-free state transition. */
    check(err != NULL && strstr(err, "state=session_recorded") != NULL,
          "the record is logged as a path-free state transition");
    check(err != NULL && strstr(err, state_home) == NULL,
          "logs never contain the state directory or record path");

    cli_mock_stop(&mock);
    remove(path);
    drop_temp_state_home(state_home);
    free(out);
    free(err);
}

static void test_print_completed_writes_no_record(void)
{
    char state_home[64];
    if (!set_temp_state_home(state_home, sizeof(state_home))) {
        return;
    }

    char path[64];
    write_temp_gcode("G21\nG90\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n", "ok\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script(&mock, replies, 2)) {
        remove(path);
        drop_temp_state_home(state_home);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(5, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "the clean print completes");
    check(out != NULL && strstr(out, "session record") == NULL,
          "a completed print mentions no session record");
    check(find_session_records(state_home, NULL, 0) == 0,
          "a completed print writes no session record");

    cli_mock_stop(&mock);
    remove(path);
    drop_temp_state_home(state_home);
    free(out);
    free(err);
}

static void test_print_never_resumes_automatically(void)
{
    char state_home[64];
    if (!set_temp_state_home(state_home, sizeof(state_home))) {
        return;
    }

    char path[64];
    write_temp_gcode("G21\nG90\n", path, sizeof(path));

    /* First run: interrupted by acknowledgement silence, record written. */
    {
        static const char *const replies[] = {"ok\n"};
        cli_mock_printer_t mock;
        if (!cli_mock_start_script(&mock, replies, 1)) {
            remove(path);
            drop_temp_state_home(state_home);
            return;
        }

        char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                         (char *)"--timeout", (char *)"150", (char *)"print", path};
        char *out = NULL;
        char *err = NULL;
        run_cli(7, argv, &out, &err);
        cli_mock_stop(&mock);
        free(out);
        free(err);
    }
    check(find_session_records(state_home, NULL, 0) == 1,
          "the interrupted run left a session record");

    /* Second run of the same file: the record present, the whole file
     * streams again from its first command — printerctl never resumes
     * from a record (power or serial failure must not trigger resume). */
    {
        static const char *const replies[] = {"ok\n", "ok\n"};
        cli_mock_printer_t mock;
        if (!cli_mock_start_script(&mock, replies, 2)) {
            remove(path);
            drop_temp_state_home(state_home);
            return;
        }

        char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                         (char *)"print", path};
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(5, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_OK, "the rerun completes");
        check(out != NULL && strstr(out, "2 command(s) sent, 2 acknowledged") != NULL,
              "the rerun streamed the whole file from its first command, not a suffix");

        cli_mock_stop(&mock);
        free(out);
        free(err);
    }
    check(find_session_records(state_home, NULL, 0) == 1,
          "the completed rerun added no record and consumed none");

    remove(path);
    drop_temp_state_home(state_home);
}

static void test_session_record_failure_is_only_a_warning(void)
{
    /* An absolute but uncreatable state home: the record cannot be
     * written, the print outcome must not change. */
    setenv("XDG_STATE_HOME", "/dev/null/printerctl-state", 1);

    char path[64];
    write_temp_gcode("G21\nG90\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script(&mock, replies, 1)) {
        remove(path);
        setenv("XDG_STATE_HOME", g_default_state_home, 1);
        return;
    }

    char *argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                     (char *)"--timeout", (char *)"150", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(7, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_COMM_ERROR,
          "a failed record write never changes the print's exit code");
    check(err != NULL && strstr(err, "no session record written") != NULL,
          "the operator is warned that no record was written");
    check(out != NULL && strstr(out, "session record written") == NULL,
          "no record path is claimed when the write failed");

    cli_mock_stop(&mock);
    remove(path);
    setenv("XDG_STATE_HOME", g_default_state_home, 1);
    free(out);
    free(err);
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

/*
 * Full Milestone 10 acceptance scenario: a real streaming print, still
 * in flight, is cancelled by `printerctl power low-battery` acting on
 * the same active-print marker the print published -- exactly the path
 * an APCUPSD script drives through scripts/apcupsd/printerctl-low-battery.sh.
 */
static void test_power_low_battery_cancels_active_print_end_to_end(void)
{
    char state_home[64];
    if (!set_temp_state_home(state_home, sizeof(state_home))) {
        return;
    }

    char path[64];
    write_temp_gcode("G21\nG90\nM105\nM400\n", path, sizeof(path));

    static const char *const replies[] = {"ok\n", "ok\n", "ok\n", "ok\n"};
    cli_mock_printer_t mock;
    if (!cli_mock_start_script_delayed(&mock, replies, 4, 150)) {
        remove(path);
        drop_temp_state_home(state_home);
        return;
    }

    char *print_argv[] = {(char *)"printerctl", (char *)"--port", mock.slave_path,
                           (char *)"print", path};
    cli_print_process_t proc;
    if (!cli_print_start(&proc, 5, print_argv)) {
        cli_mock_stop(&mock);
        remove(path);
        drop_temp_state_home(state_home);
        return;
    }

    check(cli_print_wait_for_output(&proc, "printerctl: printing"), "the print announces itself");

    /* The active-print marker is published early in the stream; give it
     * a moment to land before asking for it. */
    char *power_argv[] = {(char *)"printerctl", (char *)"power", (char *)"low-battery"};
    char *out = NULL;
    char *err = NULL;
    int power_status = -1;
    for (int attempt = 0; attempt < 300; attempt++) {
        power_status = run_cli(3, power_argv, &out, &err);
        if (out != NULL && strstr(out, "no active print") == NULL) {
            break;
        }
        free(out);
        free(err);
        out = NULL;
        err = NULL;
        poll(NULL, 0, 10);
    }

    check(power_status == PRINTERCTL_EXIT_OK, "'power low-battery' itself reports success");
    check(out != NULL && strstr(out, "signalled the active print") != NULL,
          "'power low-battery' found and signalled the running print");

    int code = -1;
    check(cli_print_wait_exit(&proc, &code), "the power-cancelled print finishes promptly");
    check(code == PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE,
          "the print exits with the power-failure cancellation code, not the operator one");

    char output[4096];
    cli_print_read_output(&proc, output, sizeof(output));
    check(strstr(output, "print cancelled_power_failure") != NULL,
          "the summary names the power-failure cancellation specifically");
    check(strstr(output, "print completed") == NULL, "a power-cancelled print never reports completion");

    char record_path[256];
    check(find_session_records(state_home, record_path, sizeof(record_path)) == 1,
          "the power-cancelled print left exactly one session record");
    char content[2048];
    check(read_whole_file(record_path, content, sizeof(content)) &&
              strstr(content, "state=cancelled_power_failure\n") != NULL &&
              strstr(content, "cause=apcupsd_low_battery\n") != NULL,
          "the record distinguishes a power-failure cancellation from an operator one");

    /* The marker is removed once the print ends: a second notification
     * now finds nothing to cancel. */
    free(out);
    free(err);
    out = NULL;
    err = NULL;
    int second_status = run_cli(3, power_argv, &out, &err);
    check(second_status == PRINTERCTL_EXIT_OK && out != NULL &&
              strstr(out, "no active print") != NULL,
          "the marker is cleaned up once the print ends, so a later notification is a no-op");

    cli_mock_stop(&mock);
    remove(proc.out_path);
    remove(path);
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
    test_print_command_needs_port();
    test_print_missing_file();
    test_print_extra_arguments();
    test_send_command();
    test_send_missing_argument();
    test_home_no_longer_mentions_retired_confirmation_flag();
    test_home_axis_argument_grammar();
    test_dry_run_home();
    test_printer_commands_need_port();
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
    test_dry_run_print_unsafe_g28();
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
    test_print_end_to_end();
    test_print_error_stops_gracefully_end_to_end();
    test_print_g28_in_file_end_to_end();
    test_print_timeout_end_to_end();
    test_pause_resume_cancel_sd_end_to_end();
    test_dry_run_sd_controls();
    test_print_pause_and_resume_by_signal_end_to_end();
    test_print_cancel_by_signal_end_to_end();
    test_print_failure_writes_session_record();
    test_print_completed_writes_no_record();
    test_print_never_resumes_automatically();
    test_session_record_failure_is_only_a_warning();
    test_dry_run_single_commands();
    test_power_requires_low_battery_subcommand();
    test_power_low_battery_dry_run();
    test_power_low_battery_with_no_active_print();
    test_power_low_battery_cleans_up_a_stale_marker();
    test_power_low_battery_cancels_active_print_end_to_end();

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
