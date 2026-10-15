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
#define PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE 10

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
    PRINTERCTL_COMMAND_INFO,
    PRINTERCTL_COMMAND_SD_PAUSE,
    PRINTERCTL_COMMAND_SD_RESUME,
    PRINTERCTL_COMMAND_SD_CANCEL,
    PRINTERCTL_COMMAND_POWER_LOW_BATTERY /* `power low-battery` (Milestone 10) */
} printerctl_command_t;

/*
 * `home` targets all axes by default; `home X`/`home Y`/`home Z` (added
 * once min end-stops were enabled on all three axes, 2026-07-21) home a
 * single one, sending `G28`/`G28 X`/`G28 Y`/`G28 Z` respectively.
 */
typedef enum {
    PRINTERCTL_HOME_ALL = 0,
    PRINTERCTL_HOME_X,
    PRINTERCTL_HOME_Y,
    PRINTERCTL_HOME_Z
} printerctl_home_axis_t;

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
    printerctl_home_axis_t home_axis;
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
 * renders human-readable output to `out`/`err` and returns the process
 * exit code.
 *
 * As of Milestone 3, `print FILE` under --dry-run (from the CLI flag or
 * the config file) is the first command with a real execution path: the
 * G-code file is parsed and normalized via printerctl_gcode, each command
 * that would be sent is written to `out` with its source line number and
 * progress, and no serial device is ever opened. A clean dry run exits
 * PRINTERCTL_EXIT_OK, including files containing G28; a G-code file
 * problem (unreadable, not a regular
 * file, oversized line, malformed line, I/O error, file changed while
 * being read) exits PRINTERCTL_EXIT_GCODE_ERROR.
 *
 * As of Milestones 20-21, `temp`, `position`, `info`, `send COMMAND`,
 * `home [X|Y|Z]` and `sd pause`/`sd resume`/`sd cancel` execute through
 * printerctld over PLP/1 and no longer open serial from the CLI.
 * `pause`, `resume` and `cancel` are not SD aliases: they control
 * daemon-owned host-print execution instead (see Milestone 23 below).
 * Under --dry-run the migrated physical commands print the line they
 * would transmit and exit PRINTERCTL_EXIT_OK without requiring daemon
 * or serial access.
 *
 * As of Milestone 7, `print FILE` without --dry-run streams the file
 * for real via printerctl_printer_print_file(): one normalized command
 * at a time, waiting for `ok` before the next, rendering progress on
 * every whole-percent change and a session summary (final state, file,
 * commands sent/acknowledged, last acknowledged line) on every outcome.
 * Exit codes follow the same mapping as the single commands, plus
 * PRINTERCTL_EXIT_GCODE_ERROR for a file that fails mid-stream.
 *
 * As of Milestone 8, a streaming `print` is an operator-controllable
 * session: SIGUSR1 pauses after the current command's acknowledgement,
 * SIGUSR2 resumes from the next unsent command, and SIGINT/SIGTERM
 * cancels gracefully — the summary reports the cancelled state and the
 * process exits PRINTERCTL_EXIT_PRINT_CANCELLED. The `sd pause`,
 * `sd resume` and `sd cancel` commands execute for real as daemon-owned
 * Marlin SD-print controls (M25, M24, M524); they target a print
 * running from the printer's SD card, never a host stream.
 *
 * As of Milestone 10, a streaming `print` also reacts to SIGPWR as a
 * distinct, non-operator cancellation: the summary reports
 * PRINTERCTL_PRINT_CANCELLED_POWER_FAILURE and the process exits
 * PRINTERCTL_EXIT_PRINT_CANCELLED_POWER_FAILURE. `power low-battery`
 * is the command an external notifier (the APCUPSD low-battery script)
 * runs to deliver that signal: it never opens a serial device itself --
 * it looks up the PID of the printerctl process currently streaming a
 * print (if any) and sends it SIGPWR. No active print is not an error
 * (PRINTERCTL_EXIT_OK): the command is idempotent, safe to run whether
 * or not a print happens to be in progress, and safe to run more than
 * once.
 *
 * Revised 2026-07-21: min end-stops are now enabled on all three axes,
 * so `home` executes for real as an explicit daemon-owned command
 * sending `G28`.
 * `home X`/`home Y`/`home Z` send `G28 X`/`G28 Y`/`G28 Z` to home a
 * single axis. Revised 2026-10-12: G28 is also allowed in files explicitly
 * selected with `print FILE`, after operator verification of minimum
 * end-stops. `send G28` remains refused with PRINTERCTL_EXIT_SAFETY_ERROR.
 * With `home` now fully implemented, no command reaches
 * PRINTERCTL_EXIT_UNAVAILABLE in ordinary use; the fallback path
 * remains only as a defensive default.
 *
 * As of Milestone 22 (daemon-owned `print FILE`), the streaming session
 * described above under Milestones 7-10 (client-side print_engine,
 * SIGUSR1/SIGUSR2/SIGINT/SIGTERM/SIGPWR handling inside `printerctl`
 * itself) is superseded: the CLI no longer owns execution and installs
 * no signal handlers of its own. `printerctl print FILE` returns after
 * printerctld accepts the request and then only observes progress by
 * polling PLP STATUS (PRINT-OBS-001); an unhandled SIGINT/SIGTERM to
 * that observer terminates the CLI process without affecting the
 * daemon-owned print, which continues exactly as if the client had
 * disconnected for any other reason. As of Milestone 23, `pause`,
 * `resume` and `cancel` send PLP PRINT_PAUSE/PRINT_RESUME/PRINT_CANCEL
 * to control that same daemon-owned stream from any client -- they
 * are not the SIGUSR1/SIGUSR2/SIGINT signals described above, and no
 * PID-based control path exists any more. `sd pause`/`sd resume`/
 * `sd cancel` are unaffected: they still address a print running from
 * the printer's own SD card.
 */
int printerctl_cli_run(int argc, char *const argv[], FILE *out, FILE *err);

#endif
