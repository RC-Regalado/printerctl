#ifndef PRINTERCTL_CLI_H
#define PRINTERCTL_CLI_H

#include "printerctl/config.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * Exit codes are part of the public, stable contract described in
 * docs/design/milestone-1-cli-config.md. Values are fixed and never reused
 * for a different meaning; there are no reserved ranges.
 */
#define PRINTERCTL_EXIT_OK 0
#define PRINTERCTL_EXIT_USAGE_ERROR 2
#define PRINTERCTL_EXIT_CONFIG_ERROR 3
#define PRINTERCTL_EXIT_SAFETY_ERROR 4
#define PRINTERCTL_EXIT_UNAVAILABLE 5

#define PRINTERCTL_CLI_ERROR_MAX 256

typedef enum {
    PRINTERCTL_CLI_ACTION_HELP,
    PRINTERCTL_CLI_ACTION_VERSION,
    PRINTERCTL_CLI_ACTION_COMMAND
} printerctl_cli_action_t;

typedef enum {
    PRINTERCTL_COMMAND_PRINT,
    PRINTERCTL_COMMAND_SEND,
    PRINTERCTL_COMMAND_HOME,
    PRINTERCTL_COMMAND_PAUSE,
    PRINTERCTL_COMMAND_RESUME,
    PRINTERCTL_COMMAND_CANCEL,
    PRINTERCTL_COMMAND_TEMP,
    PRINTERCTL_COMMAND_POSITION,
    PRINTERCTL_COMMAND_INFO
} printerctl_command_t;

/*
 * `print_file` and `send_command` point directly into the argv array given
 * to printerctl_cli_parse(); no copy is made. They are valid only for as
 * long as that argv array is valid, i.e. for the duration of the single
 * invocation that parsed them. Callers must not retain these pointers past
 * that invocation, and must not call printerctl_cli_parse() again with a
 * different argv while still relying on a request from an earlier call.
 */
typedef struct {
    printerctl_command_t command;
    const char *print_file;
    const char *send_command;
    bool home_confirmed;
} printerctl_command_args_t;

typedef struct {
    printerctl_cli_action_t action;
    printerctl_config_overrides_t overrides;
    printerctl_command_args_t command_args; /* valid when action == PRINTERCTL_CLI_ACTION_COMMAND */
} printerctl_cli_request_t;

typedef struct {
    char message[PRINTERCTL_CLI_ERROR_MAX];
} printerctl_cli_error_t;

/*
 * Pure parser: argv -> request or error. Performs no I/O (no printing, no
 * file access, no config resolution) and allocates no heap memory; all
 * storage lives in *out_request and *out_error. Both output parameters are
 * fully initialized on every call, whether it succeeds or fails.
 *
 * Grammar: printerctl [GLOBAL_OPTIONS] COMMAND [COMMAND_ARGS]
 *          printerctl --help
 *          printerctl --version
 *
 * Global options accept only the "--option VALUE" form (never
 * "--option=VALUE") and must appear before COMMAND; each may appear at most
 * once. --help and --version are standalone invocations and cannot be
 * combined with global options or a command.
 *
 * Returns true and sets out_request->action on success. Returns false and
 * fills out_error->message with an operator-facing description on any
 * usage error (unknown option, missing/duplicate/oversized/empty value,
 * unknown command, wrong number of command arguments, an empty FILE/COMMAND
 * argument, etc.).
 */
bool printerctl_cli_parse(int argc, char *const argv[],
                           printerctl_cli_request_t *out_request,
                           printerctl_cli_error_t *out_error);

/*
 * Composition entry point used by main(): parses argv, resolves
 * configuration (defaults, optional --config file, then CLI overrides),
 * enforces the `home` safety confirmation, renders human-readable output to
 * `out`/`err` and returns the process exit code. No printer command is
 * actually executed in Milestone 1; a successfully parsed and resolved
 * printer command always ends in PRINTERCTL_EXIT_UNAVAILABLE.
 */
int printerctl_cli_run(int argc, char *const argv[], FILE *out, FILE *err);

#endif
