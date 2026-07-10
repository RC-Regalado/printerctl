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
#define PRINTERCTL_EXIT_GCODE_ERROR 6
#define PRINTERCTL_EXIT_COMM_ERROR 7
#define PRINTERCTL_EXIT_PRINTER_ERROR 8
#define PRINTERCTL_EXIT_PRINT_CANCELLED 9

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
 * `out`/`err` and returns the process exit code.
 *
 * As of Milestone 3, `print FILE` under --dry-run (from the CLI flag or
 * the config file) is the first command with a real execution path: the
 * G-code file is parsed and normalized via printerctl_gcode, each command
 * that would be sent is written to `out` with its source line number and
 * progress, and no serial device is ever opened. A clean dry run exits
 * PRINTERCTL_EXIT_OK; a file containing G28 exits
 * PRINTERCTL_EXIT_SAFETY_ERROR (never confirmable from `print` in this
 * milestone); any other G-code file problem (unreadable, not a regular
 * file, oversized line, malformed line, I/O error, file changed while
 * being read) exits PRINTERCTL_EXIT_GCODE_ERROR.
 *
 * As of Milestone 6, `temp`, `position`, `info` and `send COMMAND`
 * execute a real single-command exchange via printerctl_printer (serial
 * + marlin + logging): PRINTERCTL_EXIT_OK with human-readable output on
 * success; PRINTERCTL_EXIT_CONFIG_ERROR when no port is configured;
 * PRINTERCTL_EXIT_SAFETY_ERROR when `send` carries a G28;
 * PRINTERCTL_EXIT_COMM_ERROR when the port cannot be opened or the
 * exchange fails at the transport level (I/O, disconnect, timeout);
 * PRINTERCTL_EXIT_PRINTER_ERROR when the printer replies `Error:` or
 * `Resend:`, or acknowledges without the expected report. Under
 * --dry-run these four commands print the line they would transmit and
 * exit PRINTERCTL_EXIT_OK without touching any serial device.
 *
 * As of Milestone 7, `print FILE` without --dry-run streams the file
 * for real via printerctl_printer_print_file(): one normalized command
 * at a time, waiting for `ok` before the next, rendering progress on
 * every whole-percent change and a session summary (final state, file,
 * commands sent/acknowledged, last acknowledged line) on every outcome.
 * Exit codes follow the same mapping as the single commands, plus
 * PRINTERCTL_EXIT_GCODE_ERROR for a file that fails mid-stream and
 * PRINTERCTL_EXIT_SAFETY_ERROR for a G28 found in the file.
 *
 * As of Milestone 8, a streaming `print` is an operator-controllable
 * session: SIGUSR1 pauses after the current command's acknowledgement,
 * SIGUSR2 resumes from the next unsent command, and SIGINT/SIGTERM
 * cancels gracefully — the summary reports the cancelled state and the
 * process exits PRINTERCTL_EXIT_PRINT_CANCELLED. The `pause`, `resume`
 * and `cancel` commands execute for real as direct Marlin SD-print
 * controls (M25, M24, M524) with the same exit-code mapping as `send`;
 * they target a print running from the printer's SD card, never a host
 * stream (the serial port of an active stream is held exclusively).
 * Only `home` still ends in PRINTERCTL_EXIT_UNAVAILABLE.
 */
int printerctl_cli_run(int argc, char *const argv[], FILE *out, FILE *err);

#endif
