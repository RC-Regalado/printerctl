#define _POSIX_C_SOURCE 200809L

#include "printerctl/cli.h"
#include "printerctl/config.h"
#include "printerctl/version.h"

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
    check(out != NULL && strstr(out, "--i-understand-no-endstops") != NULL, "help documents home confirmation flag");
    check(out != NULL && strstr(out, "pause") != NULL, "help documents pause");
    check(out != NULL && strstr(out, "resume") != NULL, "help documents resume");
    check(out != NULL && strstr(out, "cancel") != NULL, "help documents cancel");
    check(out != NULL && strstr(out, "temp") != NULL, "help documents temp");
    check(out != NULL && strstr(out, "position") != NULL, "help documents position");
    check(out != NULL && strstr(out, "info") != NULL, "help documents info");
    check(out != NULL && strstr(out, "Safety:") != NULL, "help documents the home safety policy");
    check(out != NULL && strstr(out, "Milestone 1") == NULL, "help no longer claims to be Milestone 1");
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

    int status = run_cli(3, argv, &out, &err);

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

    int status = run_cli(3, argv, &out, &err);

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

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'print' with an empty FILE argument exits with usage error");

    free(out);
    free(err);
}

static void test_send_empty_command(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"send", (char *)""};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

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

static void test_print_command_unavailable(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"print", (char *)"file.gcode"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "valid 'print' parses and reports unavailable");
    check(out != NULL && strstr(out, "print") != NULL, "unavailable message names the command");
    check(out != NULL && strstr(out, "(none)") != NULL, "unavailable message shows the default (empty) port");

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
    char *argv[] = {(char *)"printerctl", (char *)"send", (char *)"M115 extra words"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "valid 'send' parses and reports unavailable");
    check(out != NULL && strstr(out, "send") != NULL, "unavailable message names the command");

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

static void test_home_without_confirmation(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"home"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_SAFETY_ERROR, "'home' without confirmation exits with safety error");
    check(err != NULL && strstr(err, "i-understand-no-endstops") != NULL,
          "safety error mentions the confirmation flag");

    free(out);
    free(err);
}

static void test_home_with_confirmation(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"home", (char *)"--i-understand-no-endstops"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "confirmed 'home' parses and reports unavailable");

    free(out);
    free(err);
}

static void test_home_with_wrong_flag(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"home", (char *)"--yes"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "'home --yes' exits with usage error");

    free(out);
    free(err);
}

static void test_dry_run_home_without_confirmation_still_fails(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--dry-run", (char *)"home"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_SAFETY_ERROR, "'--dry-run home' without confirmation still exits with safety error");

    free(out);
    free(err);
}

static void test_simple_commands_unavailable(void)
{
    const char *commands[] = {"pause", "resume", "cancel", "temp", "position", "info"};

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char *argv[] = {(char *)"printerctl", (char *)commands[i]};
        char *out = NULL;
        char *err = NULL;

        int status = run_cli(2, argv, &out, &err);

        check(status == PRINTERCTL_EXIT_UNAVAILABLE, "simple command parses and reports unavailable");
        check(out != NULL && strstr(out, commands[i]) != NULL, "unavailable message names the command");

        free(out);
        free(err);
    }
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
    char *argv[] = {(char *)"printerctl", (char *)"--baud", (char *)"115200", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(4, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "explicit --baud equal to the default still parses successfully");
    check(out != NULL && strstr(out, "baud=115200") != NULL, "resolved config reflects the explicit default value");

    free(out);
    free(err);
}

static void test_config_file_precedence(void)
{
    char path[64];
    write_temp_config("port=/dev/from-file\nbaud=57600\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--config", path,
                     (char *)"--port", (char *)"/dev/from-cli", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(6, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "valid --config with CLI override parses successfully");
    check(out != NULL && strstr(out, "port=/dev/from-cli") != NULL, "CLI port overrides the config file port");
    check(out != NULL && strstr(out, "baud=57600") != NULL, "config file baud is applied when CLI does not override it");

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
    char *argv[] = {(char *)"printerctl", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "non-verbose 'pause' still resolves and reports unavailable");
    check(err != NULL && strstr(err, "\tINFO\tsession_start\t") != NULL, "log includes a session_start line");
    check(err != NULL && strstr(err, "\tINFO\tconfig_summary\t") != NULL, "log includes a config_summary line");
    check(err != NULL && strstr(err, "\tWARNING\tcommand_unavailable\tcommand=pause") != NULL,
          "log includes a command_unavailable line naming the command");
    check(err != NULL && strstr(err, "\tINFO\tsession_end\t") != NULL, "log includes a session_end line");
    check(err != NULL && strstr(err, "command_parsed") == NULL,
          "non-verbose run does not log the DEBUG command_parsed line");
    check(err != NULL && count_occurrences(err, "\n") == 4, "non-verbose run logs exactly four lines");

    free(out);
    free(err);
}

static void test_verbose_adds_command_parsed_log(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--verbose", (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "verbose 'pause' still resolves and reports unavailable");
    check(err != NULL && strstr(err, "\tDEBUG\tcommand_parsed\tcommand=pause") != NULL,
          "verbose run logs the DEBUG command_parsed line naming the command");
    check(err != NULL && count_occurrences(err, "\n") == 5, "verbose run logs exactly five lines");

    free(out);
    free(err);
}

static void test_config_summary_omits_raw_port_and_config_path(void)
{
    char path[64];
    write_temp_config("baud=57600\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"--port", (char *)"/dev/ttyACM0",
                     (char *)"--config", path, (char *)"pause"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(6, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE, "port+config 'pause' still resolves and reports unavailable");
    check(err != NULL && strstr(err, "port_set=true") != NULL, "config_summary reports port_set=true as a boolean");
    check(err != NULL && strstr(err, "config_file_set=true") != NULL,
          "config_summary reports config_file_set=true as a boolean");
    check(err != NULL && strstr(err, "/dev/ttyACM0") == NULL, "config_summary never contains the raw port value");
    check(err != NULL && strstr(err, path) == NULL, "config_summary never contains the config file path");

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

static void test_print_without_dry_run_is_still_unavailable(void)
{
    char path[64];
    write_temp_gcode("G21\n", path, sizeof(path));

    char *argv[] = {(char *)"printerctl", (char *)"print", path};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_UNAVAILABLE,
          "'print' without --dry-run still reports unavailable, even for a readable file");
    check(out != NULL && strstr(out, "not yet implemented") != NULL,
          "non-dry-run print keeps the unavailable message");
    check(out != NULL && strstr(out, "G21") == NULL, "non-dry-run print does not parse the file");

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

static void test_safety_error_adds_single_error_log_line(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"home"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_SAFETY_ERROR, "unconfirmed home still exits with safety error");
    check(err != NULL && count_occurrences(err, "\tERROR\terror\t") == 1,
          "safety rejection adds exactly one ERROR log line alongside the human-readable message");
    check(err != NULL && strstr(err, "i-understand-no-endstops") != NULL,
          "human-readable safety message is preserved");

    free(out);
    free(err);
}

int main(void)
{
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
    test_print_command_unavailable();
    test_print_missing_file();
    test_print_extra_arguments();
    test_send_command();
    test_send_missing_argument();
    test_home_without_confirmation();
    test_home_with_confirmation();
    test_home_with_wrong_flag();
    test_dry_run_home_without_confirmation_still_fails();
    test_simple_commands_unavailable();
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
    test_print_without_dry_run_is_still_unavailable();
    test_dry_run_from_config_file_enables_dry_run_print();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All CLI checks passed\n");
    return EXIT_SUCCESS;
}
