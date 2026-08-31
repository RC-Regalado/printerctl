#include "printerctl/config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void set_error(printerctl_config_error_t *out_error,
                       printerctl_config_status_t status,
                       int line_number,
                       const char *format, ...)
{
    out_error->status = status;
    out_error->line_number = line_number;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }

    size_t length = strlen(text);
    while (length > 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t' ||
            text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[length - 1] = '\0';
        length--;
    }

    return text;
}

static bool parse_ranged_int(const char *value, long min_value, long max_value, int *out)
{
    if (value[0] == '\0') {
        return false;
    }

    errno = 0;
    char *end_ptr = NULL;
    long parsed = strtol(value, &end_ptr, 10);

    if (errno != 0 || end_ptr == value || *end_ptr != '\0') {
        return false;
    }

    if (parsed < min_value || parsed > max_value) {
        return false;
    }

    *out = (int)parsed;
    return true;
}

bool printerctl_config_parse_baud(const char *value, int *out_baud)
{
    return parse_ranged_int(value, PRINTERCTL_CONFIG_MIN_BAUD, PRINTERCTL_CONFIG_MAX_BAUD, out_baud);
}

bool printerctl_config_parse_timeout(const char *value, int *out_timeout_ms)
{
    return parse_ranged_int(value, PRINTERCTL_CONFIG_MIN_TIMEOUT_MS, PRINTERCTL_CONFIG_MAX_TIMEOUT_MS, out_timeout_ms);
}

bool printerctl_config_parse_bool(const char *value, bool *out_value)
{
    if (strcmp(value, "true") == 0) {
        *out_value = true;
        return true;
    }

    if (strcmp(value, "false") == 0) {
        *out_value = false;
        return true;
    }

    return false;
}

void printerctl_config_overrides_init(printerctl_config_overrides_t *overrides)
{
    memset(overrides, 0, sizeof(*overrides));
}

void printerctl_config_set_defaults(printerctl_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->port[0] = '\0';
    config->baud = PRINTERCTL_CONFIG_DEFAULT_BAUD;
    config->timeout_ms = PRINTERCTL_CONFIG_DEFAULT_TIMEOUT_MS;
    config->dry_run = false;
    config->verbose = false;
}

printerctl_config_status_t printerctl_config_load_file(
    const char *path,
    printerctl_config_overrides_t *overrides,
    printerctl_config_error_t *out_error)
{
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_CONFIG_OK;

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        set_error(out_error, PRINTERCTL_CONFIG_ERR_FILE_NOT_FOUND, 0,
                   "cannot open config file '%s': %s", path, strerror(errno));
        return out_error->status;
    }

    bool seen_port = false;
    bool seen_baud = false;
    bool seen_timeout = false;
    bool seen_dry_run = false;
    bool seen_verbose = false;

    char line[PRINTERCTL_CONFIG_LINE_MAX];
    int line_number = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        size_t length = strlen(line);
        bool has_newline = length > 0 && line[length - 1] == '\n';
        if (!has_newline && !feof(file)) {
            set_error(out_error, PRINTERCTL_CONFIG_ERR_LINE_TOO_LONG, line_number,
                       "line %d exceeds the maximum length of %d characters",
                       line_number, PRINTERCTL_CONFIG_LINE_MAX - 2);
            break;
        }

        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        char *separator = strchr(trimmed, '=');
        if (separator == NULL) {
            set_error(out_error, PRINTERCTL_CONFIG_ERR_MALFORMED_LINE, line_number,
                       "line %d is not a valid key=value pair", line_number);
            break;
        }

        *separator = '\0';
        char *key = trim(trimmed);
        char *value = trim(separator + 1);

        if (key[0] == '\0') {
            set_error(out_error, PRINTERCTL_CONFIG_ERR_MALFORMED_LINE, line_number,
                       "line %d has an empty key", line_number);
            break;
        }

        if (strcmp(key, "port") == 0) {
            if (seen_port) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_DUPLICATE_KEY, line_number,
                           "line %d: key 'port' is duplicated", line_number);
                break;
            }
            if (value[0] == '\0') {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_INVALID_VALUE, line_number,
                           "line %d: value for 'port' must not be empty", line_number);
                break;
            }
            if (strlen(value) >= sizeof(overrides->port)) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_VALUE_TOO_LONG, line_number,
                           "line %d: value for 'port' exceeds the maximum length of %d characters",
                           line_number, (int)sizeof(overrides->port) - 1);
                break;
            }
            snprintf(overrides->port, sizeof(overrides->port), "%s", value);
            overrides->has_port = true;
            seen_port = true;
        } else if (strcmp(key, "baud") == 0) {
            if (seen_baud) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_DUPLICATE_KEY, line_number,
                           "line %d: key 'baud' is duplicated", line_number);
                break;
            }
            int parsed;
            if (!printerctl_config_parse_baud(value, &parsed)) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_INVALID_VALUE, line_number,
                           "line %d: invalid value '%s' for 'baud' (expected an integer between %d and %d)",
                           line_number, value, PRINTERCTL_CONFIG_MIN_BAUD, PRINTERCTL_CONFIG_MAX_BAUD);
                break;
            }
            overrides->baud = parsed;
            overrides->has_baud = true;
            seen_baud = true;
        } else if (strcmp(key, "timeout") == 0) {
            if (seen_timeout) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_DUPLICATE_KEY, line_number,
                           "line %d: key 'timeout' is duplicated", line_number);
                break;
            }
            int parsed;
            if (!printerctl_config_parse_timeout(value, &parsed)) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_INVALID_VALUE, line_number,
                           "line %d: invalid value '%s' for 'timeout' (expected an integer between %d and %d)",
                           line_number, value, PRINTERCTL_CONFIG_MIN_TIMEOUT_MS, PRINTERCTL_CONFIG_MAX_TIMEOUT_MS);
                break;
            }
            overrides->timeout_ms = parsed;
            overrides->has_timeout = true;
            seen_timeout = true;
        } else if (strcmp(key, "dry_run") == 0) {
            if (seen_dry_run) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_DUPLICATE_KEY, line_number,
                           "line %d: key 'dry_run' is duplicated", line_number);
                break;
            }
            bool parsed;
            if (!printerctl_config_parse_bool(value, &parsed)) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_INVALID_VALUE, line_number,
                           "line %d: invalid value '%s' for 'dry_run' (expected 'true' or 'false')",
                           line_number, value);
                break;
            }
            overrides->dry_run = parsed;
            overrides->has_dry_run = true;
            seen_dry_run = true;
        } else if (strcmp(key, "verbose") == 0) {
            if (seen_verbose) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_DUPLICATE_KEY, line_number,
                           "line %d: key 'verbose' is duplicated", line_number);
                break;
            }
            bool parsed;
            if (!printerctl_config_parse_bool(value, &parsed)) {
                set_error(out_error, PRINTERCTL_CONFIG_ERR_INVALID_VALUE, line_number,
                           "line %d: invalid value '%s' for 'verbose' (expected 'true' or 'false')",
                           line_number, value);
                break;
            }
            overrides->verbose = parsed;
            overrides->has_verbose = true;
            seen_verbose = true;
        } else {
            set_error(out_error, PRINTERCTL_CONFIG_ERR_UNKNOWN_KEY, line_number,
                       "line %d: unknown configuration key '%s'", line_number, key);
            break;
        }
    }

    if (out_error->status == PRINTERCTL_CONFIG_OK && ferror(file)) {
        set_error(out_error, PRINTERCTL_CONFIG_ERR_READ_FAILED, 0,
                   "error reading config file '%s': %s", path, strerror(errno));
    }

    fclose(file);
    return out_error->status;
}

void printerctl_config_apply_overrides(
    printerctl_config_t *config,
    const printerctl_config_overrides_t *overrides)
{
    printerctl_config_apply_overrides_for_view(config, overrides, PRINTERCTL_CONFIG_VIEW_ALL);
}

void printerctl_config_apply_overrides_for_view(
    printerctl_config_t *config,
    const printerctl_config_overrides_t *overrides,
    printerctl_config_view_t view)
{
    bool apply_hardware =
        view == PRINTERCTL_CONFIG_VIEW_ALL ||
        view == PRINTERCTL_CONFIG_VIEW_DAEMON ||
        view == PRINTERCTL_CONFIG_VIEW_LEGACY_CLI;
    bool apply_client =
        view == PRINTERCTL_CONFIG_VIEW_ALL ||
        view == PRINTERCTL_CONFIG_VIEW_CLIENT ||
        view == PRINTERCTL_CONFIG_VIEW_LEGACY_CLI;

    if (apply_hardware && overrides->has_port) {
        snprintf(config->port, sizeof(config->port), "%s", overrides->port);
    }
    if (apply_hardware && overrides->has_baud) {
        config->baud = overrides->baud;
    }
    if (apply_hardware && overrides->has_timeout) {
        config->timeout_ms = overrides->timeout_ms;
    }
    if (apply_client && overrides->has_dry_run) {
        config->dry_run = overrides->dry_run;
    }
    if (apply_client && overrides->has_verbose) {
        config->verbose = overrides->verbose;
    }
}

printerctl_config_status_t printerctl_config_resolve(
    const printerctl_config_overrides_t *cli_overrides,
    printerctl_config_t *out_config,
    printerctl_config_error_t *out_error)
{
    return printerctl_config_resolve_for_view(
        cli_overrides, PRINTERCTL_CONFIG_VIEW_ALL, out_config, out_error);
}

printerctl_config_status_t printerctl_config_resolve_for_view(
    const printerctl_config_overrides_t *cli_overrides,
    printerctl_config_view_t view,
    printerctl_config_t *out_config,
    printerctl_config_error_t *out_error)
{
    memset(out_error, 0, sizeof(*out_error));
    printerctl_config_set_defaults(out_config);

    if (cli_overrides->has_config_path) {
        printerctl_config_overrides_t file_overrides;
        printerctl_config_overrides_init(&file_overrides);

        printerctl_config_status_t status =
            printerctl_config_load_file(cli_overrides->config_path, &file_overrides, out_error);
        if (status != PRINTERCTL_CONFIG_OK) {
            return status;
        }

        printerctl_config_apply_overrides_for_view(out_config, &file_overrides, view);
    }

    printerctl_config_apply_overrides_for_view(out_config, cli_overrides, view);
    return PRINTERCTL_CONFIG_OK;
}
