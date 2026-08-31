#ifndef PRINTERCTL_CONFIG_H
#define PRINTERCTL_CONFIG_H

#include <stdbool.h>

/*
 * Milestone 1 configuration model.
 *
 * Fields are intentionally limited to what Milestone 1 needs: port, baud,
 * timeout, dry_run, verbose. `architecture.md`'s original sketch also lists
 * `apcupsd_enabled`/`apcupsd_auto_resume`; those belong to the APCUPSD
 * config section introduced in Milestone 10 and are deliberately omitted
 * here to avoid speculative, untested fields. The root `key=value` format
 * defined below stays valid once Milestone 10 adds `[apcupsd]`-style
 * sections, since a section header is simply a line this parser does not
 * yet recognize as a key.
 *
 * `port` uses a fixed-size buffer instead of a pointer so the resolved
 * config never depends on the lifetime of argv or a file-read buffer, and
 * so overflow can be rejected explicitly rather than silently truncated.
 */

#define PRINTERCTL_CONFIG_PORT_MAX 256
#define PRINTERCTL_CONFIG_PATH_MAX 4096
#define PRINTERCTL_CONFIG_ERROR_MAX 256

/*
 * Size, in bytes, of the internal line buffer used by
 * printerctl_config_load_file(), including room for the trailing '\n' and
 * the terminating '\0'. The longest line content accepted, excluding its
 * newline, is therefore PRINTERCTL_CONFIG_LINE_MAX - 2 characters; a line
 * one byte longer is rejected as PRINTERCTL_CONFIG_ERR_LINE_TOO_LONG.
 */
#define PRINTERCTL_CONFIG_LINE_MAX 512

#define PRINTERCTL_CONFIG_DEFAULT_BAUD 115200
#define PRINTERCTL_CONFIG_DEFAULT_TIMEOUT_MS 5000

#define PRINTERCTL_CONFIG_MIN_BAUD 1
#define PRINTERCTL_CONFIG_MAX_BAUD 4000000
#define PRINTERCTL_CONFIG_MIN_TIMEOUT_MS 1
#define PRINTERCTL_CONFIG_MAX_TIMEOUT_MS 600000

typedef struct {
    char port[PRINTERCTL_CONFIG_PORT_MAX];
    int baud;
    int timeout_ms;
    bool dry_run;
    bool verbose;
} printerctl_config_t;

/*
 * Sparse override set with one has_* flag per field, used both for
 * `--config FILE` contents and for CLI global options. A field is only
 * applied onto a printerctl_config_t when its has_* flag is set, which is
 * what makes "CLI value equal to the default" behave identically to
 * "CLI value overriding a different default": the flag records that the
 * field was set explicitly, independently of the value chosen.
 */
typedef struct {
    bool has_port;
    char port[PRINTERCTL_CONFIG_PORT_MAX];

    bool has_baud;
    int baud;

    bool has_timeout;
    int timeout_ms;

    bool has_dry_run;
    bool dry_run;

    bool has_verbose;
    bool verbose;

    bool has_config_path;
    char config_path[PRINTERCTL_CONFIG_PATH_MAX];
} printerctl_config_overrides_t;

typedef enum {
    PRINTERCTL_CONFIG_VIEW_ALL = 0,
    PRINTERCTL_CONFIG_VIEW_CLIENT,
    PRINTERCTL_CONFIG_VIEW_DAEMON,
    PRINTERCTL_CONFIG_VIEW_LEGACY_CLI
} printerctl_config_view_t;

typedef enum {
    PRINTERCTL_CONFIG_OK = 0,
    PRINTERCTL_CONFIG_ERR_FILE_NOT_FOUND,
    PRINTERCTL_CONFIG_ERR_LINE_TOO_LONG,
    PRINTERCTL_CONFIG_ERR_MALFORMED_LINE,
    PRINTERCTL_CONFIG_ERR_UNKNOWN_KEY,
    PRINTERCTL_CONFIG_ERR_DUPLICATE_KEY,
    PRINTERCTL_CONFIG_ERR_INVALID_VALUE,
    PRINTERCTL_CONFIG_ERR_VALUE_TOO_LONG,
    PRINTERCTL_CONFIG_ERR_READ_FAILED
} printerctl_config_status_t;

typedef struct {
    printerctl_config_status_t status;
    int line_number; /* 0 when the error is not tied to a specific line */
    char message[PRINTERCTL_CONFIG_ERROR_MAX];
} printerctl_config_error_t;

/* Zeroes *overrides and clears every has_* flag. */
void printerctl_config_overrides_init(printerctl_config_overrides_t *overrides);

/* Fills *config with the built-in safe defaults. */
void printerctl_config_set_defaults(printerctl_config_t *config);

/*
 * Parses "key=value" lines from `value` as a baud rate / timeout / boolean,
 * shared by both the config-file reader and CLI option parsing so the two
 * entry points agree exactly on valid syntax and ranges.
 */
bool printerctl_config_parse_baud(const char *value, int *out_baud);
bool printerctl_config_parse_timeout(const char *value, int *out_timeout_ms);
bool printerctl_config_parse_bool(const char *value, bool *out_value);

/*
 * Reads a Milestone 1 config file: root-level "key=value" lines, blank
 * lines and full-line "#" comments ignored, whitespace trimmed around keys
 * and values. Recognized keys: port, baud, timeout, dry_run, verbose.
 * Booleans must be exactly "true" or "false".
 *
 * On success, sets the has_* flag and value for every key found (does not
 * touch fields for keys absent from the file) and returns
 * PRINTERCTL_CONFIG_OK. On failure, returns the specific error and fills
 * *out_error; *overrides may hold already-applied keys from lines read
 * before the failing line.
 *
 * Errors: file does not exist or cannot be opened, a line's content
 * (excluding its newline) exceeds PRINTERCTL_CONFIG_LINE_MAX - 2 characters,
 * a non-comment/non-blank line has no '=' or an empty key, an unknown key, a
 * key repeated within the file, a value that fails validation (including an
 * empty value for 'port'), a value too long for its fixed buffer, or a read
 * error on the underlying file handle after it was successfully opened
 * (PRINTERCTL_CONFIG_ERR_READ_FAILED; any parse error found before the read
 * error is reported instead, since it was detected first).
 */
printerctl_config_status_t printerctl_config_load_file(
    const char *path,
    printerctl_config_overrides_t *overrides,
    printerctl_config_error_t *out_error);

/* Applies every has_* field present in *overrides onto *config. */
void printerctl_config_apply_overrides(
    printerctl_config_t *config,
    const printerctl_config_overrides_t *overrides);

/*
 * Applies only the fields owned by `view`.
 *
 * CLIENT: dry_run, verbose.
 * DAEMON: port, baud, timeout.
 * LEGACY_CLI: transitional Phase 2 compatibility view for direct serial
 * commands not yet migrated to PLP/daemon execution; removed in M25.
 * ALL: historical unfiltered behavior.
 */
void printerctl_config_apply_overrides_for_view(
    printerctl_config_t *config,
    const printerctl_config_overrides_t *overrides,
    printerctl_config_view_t view);

/*
 * Resolves the final configuration in precedence order: built-in defaults,
 * then the file named by cli_overrides->config_path (only when
 * has_config_path is set), then cli_overrides itself. On failure, returns
 * the config-file error and fills *out_error; *out_config is left
 * unspecified.
 */
printerctl_config_status_t printerctl_config_resolve(
    const printerctl_config_overrides_t *cli_overrides,
    printerctl_config_t *out_config,
    printerctl_config_error_t *out_error);

printerctl_config_status_t printerctl_config_resolve_for_view(
    const printerctl_config_overrides_t *cli_overrides,
    printerctl_config_view_t view,
    printerctl_config_t *out_config,
    printerctl_config_error_t *out_error);

#endif
