#define _POSIX_C_SOURCE 200809L

#include "printerctl/config.h"

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

/* Writes `content` to a fresh temp file and returns its path via `path`. */
static void write_temp_config(const char *content, char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/printerctl_config_test_XXXXXX");
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

static void test_defaults(void)
{
    printerctl_config_t config;
    printerctl_config_set_defaults(&config);

    check(config.port[0] == '\0', "default port is empty");
    check(config.baud == PRINTERCTL_CONFIG_DEFAULT_BAUD, "default baud matches constant");
    check(config.timeout_ms == PRINTERCTL_CONFIG_DEFAULT_TIMEOUT_MS, "default timeout matches constant");
    check(config.dry_run == false, "default dry_run is false");
    check(config.verbose == false, "default verbose is false");
}

static void test_load_valid_file(void)
{
    char path[64];
    write_temp_config(
        "# comment line\n"
        "\n"
        "  port = /dev/ttyACM0  \n"
        "baud=250000\n"
        "timeout=10000\n"
        "dry_run=true\n"
        "verbose=true\n",
        path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);

    check(status == PRINTERCTL_CONFIG_OK, "valid file loads without error");
    check(overrides.has_port && strcmp(overrides.port, "/dev/ttyACM0") == 0, "port is parsed and trimmed");
    check(overrides.has_baud && overrides.baud == 250000, "baud is parsed");
    check(overrides.has_timeout && overrides.timeout_ms == 10000, "timeout is parsed");
    check(overrides.has_dry_run && overrides.dry_run == true, "dry_run is parsed");
    check(overrides.has_verbose && overrides.verbose == true, "verbose is parsed");

    remove(path);
}

static void test_load_file_not_found(void)
{
    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status =
        printerctl_config_load_file("/nonexistent/printerctl-config-test.conf", &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_FILE_NOT_FOUND, "missing file reports FILE_NOT_FOUND");
    check(error.message[0] != '\0', "missing file error has a message");
}

static void test_load_file_unknown_key(void)
{
    char path[64];
    write_temp_config("bogus=1\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_UNKNOWN_KEY, "unknown key is rejected");
    check(error.line_number == 1, "unknown key error names the line number");

    remove(path);
}

static void test_load_file_duplicate_key(void)
{
    char path[64];
    write_temp_config("port=/dev/ttyACM0\nport=/dev/ttyACM1\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_DUPLICATE_KEY, "duplicate key is rejected");
    check(error.line_number == 2, "duplicate key error names the second line");

    remove(path);
}

static void test_load_file_malformed_line(void)
{
    char path[64];
    write_temp_config("this line has no separator\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_MALFORMED_LINE, "line without '=' is rejected");

    remove(path);
}

static void test_load_file_empty_key(void)
{
    char path[64];
    write_temp_config("=value\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_MALFORMED_LINE, "empty key is rejected");

    remove(path);
}

static void test_load_file_invalid_baud(void)
{
    char path[64];
    write_temp_config("baud=not-a-number\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);
    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "non-numeric baud is rejected");

    remove(path);

    char path2[64];
    write_temp_config("baud=0\n", path2, sizeof(path2));
    printerctl_config_overrides_init(&overrides);
    status = printerctl_config_load_file(path2, &overrides, &error);
    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "out-of-range baud (0) is rejected");
    remove(path2);

    char path3[64];
    write_temp_config("baud=5000000\n", path3, sizeof(path3));
    printerctl_config_overrides_init(&overrides);
    status = printerctl_config_load_file(path3, &overrides, &error);
    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "out-of-range baud (5000000) is rejected");
    remove(path3);
}

static void test_load_file_invalid_timeout(void)
{
    char path[64];
    write_temp_config("timeout=700000\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);
    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "out-of-range timeout is rejected");

    remove(path);
}

static void test_load_file_invalid_bool(void)
{
    char path[64];
    write_temp_config("dry_run=yes\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);
    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "non-'true'/'false' boolean is rejected");

    remove(path);
}

static void test_load_file_line_too_long(void)
{
    char path[64];
    char content[PRINTERCTL_CONFIG_LINE_MAX + 64];
    size_t i = 0;
    memcpy(content, "port=", 5);
    i = 5;
    for (; i < sizeof(content) - 2; i++) {
        content[i] = 'a';
    }
    content[i] = '\n';
    content[i + 1] = '\0';

    write_temp_config(content, path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);
    check(status == PRINTERCTL_CONFIG_ERR_LINE_TOO_LONG, "oversized line is rejected");

    remove(path);
}

static void test_load_file_port_value_too_long(void)
{
    /* A value longer than PRINTERCTL_CONFIG_PORT_MAX but still well under
     * PRINTERCTL_CONFIG_LINE_MAX, so this isolates VALUE_TOO_LONG from
     * LINE_TOO_LONG. */
    char path[64];
    char content[PRINTERCTL_CONFIG_PORT_MAX + 32];
    memcpy(content, "port=", 5);
    size_t i = 5;
    for (; i < sizeof(content) - 2; i++) {
        content[i] = 'x';
    }
    content[i] = '\n';
    content[i + 1] = '\0';

    write_temp_config(content, path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);
    check(status == PRINTERCTL_CONFIG_ERR_VALUE_TOO_LONG, "oversized port value is rejected, never truncated");

    remove(path);
}

static void test_load_file_empty_port_value(void)
{
    char path[64];
    write_temp_config("port=\n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "empty 'port' value is rejected");
    check(error.line_number == 1, "empty 'port' error names the line number");

    remove(path);
}

/* A trailing-whitespace-only value trims to empty and must be rejected the
 * same way as a literally empty value. */
static void test_load_file_whitespace_only_port_value(void)
{
    char path[64];
    write_temp_config("port=   \n", path, sizeof(path));

    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "whitespace-only 'port' value is rejected");

    remove(path);
}

/* Opening a directory as the config path is a deterministic way to make a
 * read fail after fopen() already succeeded, without mocking stdio. */
static void test_load_file_read_error(void)
{
    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);
    printerctl_config_error_t error;

    printerctl_config_status_t status = printerctl_config_load_file("/tmp", &overrides, &error);

    check(status == PRINTERCTL_CONFIG_ERR_READ_FAILED, "a read failure after fopen() is reported, not silently OK");
    check(error.message[0] != '\0', "read failure error has a message");
}

/* The line buffer holds up to PRINTERCTL_CONFIG_LINE_MAX bytes including the
 * newline and the terminating '\0', so the longest accepted line content is
 * exactly PRINTERCTL_CONFIG_LINE_MAX - 2 characters. Uses full-line comments
 * (rather than "port=...") so the boundary is isolated from
 * PRINTERCTL_CONFIG_PORT_MAX, which is far smaller than the line limit. */
static void test_load_file_line_length_boundary(void)
{
    {
        char path[64];
        char content[PRINTERCTL_CONFIG_LINE_MAX + 8];
        content[0] = '#';
        size_t fill = PRINTERCTL_CONFIG_LINE_MAX - 2 - 1;
        for (size_t i = 0; i < fill; i++) {
            content[1 + i] = 'a';
        }
        content[1 + fill] = '\n';
        content[1 + fill + 1] = '\0';

        write_temp_config(content, path, sizeof(path));

        printerctl_config_overrides_t overrides;
        printerctl_config_overrides_init(&overrides);
        printerctl_config_error_t error;

        printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);
        check(status == PRINTERCTL_CONFIG_OK, "a line at exactly the maximum content length is accepted");

        remove(path);
    }
    {
        char path[64];
        char content[PRINTERCTL_CONFIG_LINE_MAX + 8];
        content[0] = '#';
        size_t fill = PRINTERCTL_CONFIG_LINE_MAX - 2 - 1 + 1;
        for (size_t i = 0; i < fill; i++) {
            content[1 + i] = 'a';
        }
        content[1 + fill] = '\n';
        content[1 + fill + 1] = '\0';

        write_temp_config(content, path, sizeof(path));

        printerctl_config_overrides_t overrides;
        printerctl_config_overrides_init(&overrides);
        printerctl_config_error_t error;

        printerctl_config_status_t status = printerctl_config_load_file(path, &overrides, &error);
        check(status == PRINTERCTL_CONFIG_ERR_LINE_TOO_LONG,
              "a line one character past the maximum content length is rejected");

        remove(path);
    }
}

static void test_resolve_precedence(void)
{
    char path[64];
    write_temp_config("port=/dev/from-file\nbaud=57600\n", path, sizeof(path));

    printerctl_config_overrides_t cli_overrides;
    printerctl_config_overrides_init(&cli_overrides);
    snprintf(cli_overrides.config_path, sizeof(cli_overrides.config_path), "%s", path);
    cli_overrides.has_config_path = true;
    snprintf(cli_overrides.port, sizeof(cli_overrides.port), "%s", "/dev/from-cli");
    cli_overrides.has_port = true;

    printerctl_config_t config;
    printerctl_config_error_t error;
    printerctl_config_status_t status = printerctl_config_resolve(&cli_overrides, &config, &error);

    check(status == PRINTERCTL_CONFIG_OK, "resolve succeeds with valid file and CLI overrides");
    check(strcmp(config.port, "/dev/from-cli") == 0, "CLI port overrides file port");
    check(config.baud == 57600, "file baud is kept when CLI does not override it");
    check(config.timeout_ms == PRINTERCTL_CONFIG_DEFAULT_TIMEOUT_MS, "unset fields keep the default");

    remove(path);
}

static void test_resolve_cli_value_equal_to_default(void)
{
    printerctl_config_overrides_t cli_overrides;
    printerctl_config_overrides_init(&cli_overrides);
    cli_overrides.has_baud = true;
    cli_overrides.baud = PRINTERCTL_CONFIG_DEFAULT_BAUD;

    printerctl_config_t config;
    printerctl_config_error_t error;
    printerctl_config_status_t status = printerctl_config_resolve(&cli_overrides, &config, &error);

    check(status == PRINTERCTL_CONFIG_OK, "resolve succeeds when CLI value equals the default");
    check(config.baud == PRINTERCTL_CONFIG_DEFAULT_BAUD, "explicit CLI value equal to default is applied");
}

static void test_resolve_invalid_config_file(void)
{
    char path[64];
    write_temp_config("baud=not-a-number\n", path, sizeof(path));

    printerctl_config_overrides_t cli_overrides;
    printerctl_config_overrides_init(&cli_overrides);
    snprintf(cli_overrides.config_path, sizeof(cli_overrides.config_path), "%s", path);
    cli_overrides.has_config_path = true;

    printerctl_config_t config;
    printerctl_config_error_t error;
    printerctl_config_status_t status = printerctl_config_resolve(&cli_overrides, &config, &error);

    check(status == PRINTERCTL_CONFIG_ERR_INVALID_VALUE, "resolve surfaces the file's error status");
    check(error.message[0] != '\0', "resolve surfaces a descriptive error message");

    remove(path);
}

static void test_resolve_file_boolean_survives_without_cli_override(void)
{
    char path[64];
    write_temp_config("verbose=true\n", path, sizeof(path));

    printerctl_config_overrides_t cli_overrides;
    printerctl_config_overrides_init(&cli_overrides);
    snprintf(cli_overrides.config_path, sizeof(cli_overrides.config_path), "%s", path);
    cli_overrides.has_config_path = true;
    /* CLI does not set --verbose at all. */

    printerctl_config_t config;
    printerctl_config_error_t error;
    printerctl_config_status_t status = printerctl_config_resolve(&cli_overrides, &config, &error);

    check(status == PRINTERCTL_CONFIG_OK, "resolve succeeds");
    check(config.verbose == true, "file-set boolean is not reset by an absent CLI flag");
    check(config.dry_run == false, "fields absent from both file and CLI keep their default");

    remove(path);
}

int main(void)
{
    test_defaults();
    test_load_valid_file();
    test_load_file_not_found();
    test_load_file_unknown_key();
    test_load_file_duplicate_key();
    test_load_file_malformed_line();
    test_load_file_empty_key();
    test_load_file_invalid_baud();
    test_load_file_invalid_timeout();
    test_load_file_invalid_bool();
    test_load_file_line_too_long();
    test_load_file_port_value_too_long();
    test_load_file_empty_port_value();
    test_load_file_whitespace_only_port_value();
    test_load_file_read_error();
    test_load_file_line_length_boundary();
    test_resolve_precedence();
    test_resolve_cli_value_equal_to_default();
    test_resolve_invalid_config_file();
    test_resolve_file_boolean_survives_without_cli_override();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All config checks passed\n");
    return EXIT_SUCCESS;
}
