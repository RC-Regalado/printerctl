#define _POSIX_C_SOURCE 200809L

#include "printerctl/cli.h"
#include "printerctl/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
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

static void test_help(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--help"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_OK, "help exits with success status");
    check(out != NULL && strstr(out, "Usage: printerctl") != NULL, "help output includes usage line");
    check(out != NULL && strstr(out, "--help") != NULL, "help output documents --help");
    check(out != NULL && strstr(out, "--version") != NULL, "help output documents --version");
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

static void test_unknown_argument(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--bogus"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(2, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "unknown argument exits with usage error");
    check(err != NULL && strstr(err, "--bogus") != NULL, "unknown argument error names the argument");
    check(out != NULL && out[0] == '\0', "unknown argument produces no stdout output");

    free(out);
    free(err);
}

static void test_too_many_arguments(void)
{
    char *argv[] = {(char *)"printerctl", (char *)"--help", (char *)"extra"};
    char *out = NULL;
    char *err = NULL;

    int status = run_cli(3, argv, &out, &err);

    check(status == PRINTERCTL_EXIT_USAGE_ERROR, "extra arguments exit with usage error");
    check(err != NULL && strstr(err, "too many arguments") != NULL, "extra arguments error is explicit");

    free(out);
    free(err);
}

int main(void)
{
    test_help();
    test_version();
    test_missing_command();
    test_unknown_argument();
    test_too_many_arguments();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All CLI checks passed\n");
    return EXIT_SUCCESS;
}
