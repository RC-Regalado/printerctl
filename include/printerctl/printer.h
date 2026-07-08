#ifndef PRINTERCTL_PRINTER_H
#define PRINTERCTL_PRINTER_H

#include "printerctl/config.h"
#include "printerctl/logging.h"
#include "printerctl/marlin.h"
#include "printerctl/serial.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Milestone 6 printer session: the first composition of `serial` +
 * `marlin` + `logging`. Implements the single-command exchanges behind
 * `temp` (M105), `position` (M114), `info` (M115) and `send`, following
 * architecture.md §7.2: send one command line, then read response lines
 * until a terminal Marlin response (`ok`, `Error:` or `Resend:`)
 * arrives, treating `busy:`/`echo:`/unknown lines as non-terminal.
 *
 * Safety:
 *   - printerctl_printer_connect() refuses a config with dry_run set
 *     (PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT): a dry-run invocation
 *     must never construct a serial connection, and this module is the
 *     last line of defense for architecture.md §8 rule 7 — the CLI
 *     never calls connect in dry-run mode in the first place.
 *   - printerctl_printer_send() rejects a G28 command
 *     (PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND) before anything is
 *     written, using the same detection the G-code reader applies to
 *     files (printerctl_gcode_command_is_unsafe_g28()). There is no
 *     bypass through `send`; homing has its own confirmed command.
 *     Every other command — including M112, the operator's explicit
 *     emergency stop — passes through: `send` is the operator speaking,
 *     not the software acting automatically.
 *
 * Logging (best-effort, never a gate): with a non-NULL logger, every
 * transmitted line is logged via the `serial_tx` helper and every
 * received line via `serial_rx` (both DEBUG — visible with --verbose),
 * and connect/disconnect record INFO `state_transition` events (baud
 * and timeout only; never the port path, per the Milestone 2 privacy
 * rules). A failure to write a log record never affects the exchange
 * itself. `logger` may be NULL to disable logging entirely.
 *
 * Same output-parameter discipline as every other module: no heap,
 * stack-value session, error struct always fully initialized,
 * idempotent disconnect.
 */

/* Maximum length of one received response line, including the '\0'. */
#define PRINTERCTL_PRINTER_LINE_MAX 1024

#define PRINTERCTL_PRINTER_ERROR_MAX 256

typedef enum {
    PRINTERCTL_PRINTER_OK = 0,
    PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT,
    PRINTERCTL_PRINTER_ERR_NOT_CONNECTED,
    PRINTERCTL_PRINTER_ERR_NO_PORT,
    PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND,
    PRINTERCTL_PRINTER_ERR_SERIAL,
    PRINTERCTL_PRINTER_ERR_TIMEOUT,
    PRINTERCTL_PRINTER_ERR_PRINTER_ERROR,   /* Marlin replied Error: */
    PRINTERCTL_PRINTER_ERR_RESEND,          /* Marlin requested a resend (unsupported in MVP) */
    PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE
} printerctl_printer_status_t;

typedef struct {
    printerctl_printer_status_t status;
    char message[PRINTERCTL_PRINTER_ERROR_MAX];
} printerctl_printer_error_t;

/*
 * Stack-value session: an owned serial port plus a borrowed logger.
 * `logger` must stay valid between connect() and disconnect(); the
 * session never initializes or shuts it down.
 */
typedef struct {
    printerctl_serial_port_t port;
    printerctl_logger_t *logger;
    bool connected;
} printerctl_printer_session_t;

/*
 * Called once per received response line during an exchange, in arrival
 * order, including the terminal line itself. `line` borrows the
 * session's internal buffer and is valid only for the duration of the
 * call; copy it if it must outlive the callback.
 */
typedef void (*printerctl_printer_line_fn)(const char *line, void *context);

/*
 * Opens the serial port named by `config` and binds `logger` (which may
 * be NULL) to the session. Always overwrites the full contents of
 * *session, on both success and failure.
 *
 * `session`, `config` and `out_error` must be non-NULL, and
 * config->dry_run must be false (PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT
 * otherwise — a dry-run config must never reach a connect call).
 * Returns PRINTERCTL_PRINTER_ERR_NO_PORT when config->port is empty,
 * or PRINTERCTL_PRINTER_ERR_SERIAL when the port cannot be opened or
 * configured (the serial module's message is passed through).
 */
printerctl_printer_status_t printerctl_printer_connect(
    printerctl_printer_session_t *session,
    const printerctl_config_t *config,
    printerctl_logger_t *logger,
    printerctl_printer_error_t *out_error);

/*
 * Executes one command exchange: transmits `command` as a single line,
 * then reads response lines until a terminal Marlin response arrives.
 * Every received line (terminal included) is passed to `on_line` when
 * it is non-NULL. `busy:`, `echo:` and unknown informational lines are
 * non-terminal; each received line restarts the configured timeout, so
 * a long-running command that emits `busy:` keepalives never times out
 * while the printer is still talking.
 *
 * Returns PRINTERCTL_PRINTER_OK on `ok`;
 * PRINTERCTL_PRINTER_ERR_PRINTER_ERROR on `Error:` (with Marlin's
 * message in *out_error); PRINTERCTL_PRINTER_ERR_RESEND on `Resend:`;
 * PRINTERCTL_PRINTER_ERR_TIMEOUT when a response line does not arrive
 * in time; PRINTERCTL_PRINTER_ERR_SERIAL on write/read failures
 * (including disconnects). The command must not contain embedded line
 * endings (PRINTERCTL_PRINTER_ERR_INVALID_ARGUMENT, from the serial
 * module's contract).
 *
 * This function applies no safety filtering: callers that accept
 * operator-typed commands must gate them first (printerctl_printer_send()
 * does exactly that). The typed queries below pass fixed, safe commands.
 */
printerctl_printer_status_t printerctl_printer_execute(
    printerctl_printer_session_t *session,
    const char *command,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error);

/*
 * `temp`: sends M105 and decodes the temperature report from the
 * acknowledgement payload (`ok T:...`) or from an informational report
 * line preceding the `ok`. Returns
 * PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE if the exchange succeeds
 * but no temperature pair can be decoded; *out_report is zeroed then.
 */
printerctl_printer_status_t printerctl_printer_query_temperature(
    printerctl_printer_session_t *session,
    printerctl_marlin_temperature_report_t *out_report,
    printerctl_printer_error_t *out_error);

/*
 * `position`: sends M114 and decodes the position report from the
 * response lines. Returns PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE if
 * the exchange succeeds but no X/Y/Z report can be decoded.
 */
printerctl_printer_status_t printerctl_printer_query_position(
    printerctl_printer_session_t *session,
    printerctl_marlin_position_report_t *out_report,
    printerctl_printer_error_t *out_error);

/*
 * `info`: sends M115 and decodes the firmware information from the
 * response lines. Returns PRINTERCTL_PRINTER_ERR_UNEXPECTED_RESPONSE if
 * the exchange succeeds but no FIRMWARE_NAME line can be decoded.
 */
printerctl_printer_status_t printerctl_printer_query_info(
    printerctl_printer_session_t *session,
    printerctl_marlin_firmware_info_t *out_info,
    printerctl_printer_error_t *out_error);

/*
 * `send`: one raw operator command with acknowledgement handling.
 * Rejects G28 (PRINTERCTL_PRINTER_ERR_UNSAFE_COMMAND) before anything
 * is transmitted; otherwise identical to printerctl_printer_execute(),
 * with every received line passed to `on_line` so the caller can render
 * the full exchange.
 */
printerctl_printer_status_t printerctl_printer_send(
    printerctl_printer_session_t *session,
    const char *command,
    printerctl_printer_line_fn on_line,
    void *context,
    printerctl_printer_error_t *out_error);

/*
 * Closes the session's serial port. Idempotent: a second disconnect, or
 * a disconnect on a never-connected session, is a no-op returning
 * PRINTERCTL_PRINTER_OK. Returns PRINTERCTL_PRINTER_ERR_SERIAL if the
 * underlying close fails; either way the session is left disconnected.
 */
printerctl_printer_status_t printerctl_printer_disconnect(
    printerctl_printer_session_t *session,
    printerctl_printer_error_t *out_error);

#endif
