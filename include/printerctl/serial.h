#ifndef PRINTERCTL_SERIAL_H
#define PRINTERCTL_SERIAL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Milestone 5 Linux USB serial transport.
 *
 * Line-oriented, timeout-bounded I/O over a POSIX terminal device,
 * configured in raw mode suitable for Marlin: 8N1, no echo, no canonical
 * processing, no CR/LF translation, no flow control, modem-control lines
 * ignored (CLOCAL). Every read and write is bounded by the timeout given
 * at open time; nothing here ever blocks indefinitely.
 *
 * The port is a plain stack value, following the same discipline as
 * printerctl_gcode_reader_t: no heap allocation anywhere in this module,
 * output parameters are always fully initialized on success and failure,
 * a terminal-style close is idempotent, and every failure mode has an
 * explicit status plus an operator-facing message in the error struct.
 *
 * Ownership: printerctl_serial_open() owns the file descriptor it opens
 * for the lifetime of the port; printerctl_serial_close() is the only
 * correct way to release it. `path` is borrowed only for the duration of
 * the open call.
 *
 * This module knows nothing about Marlin semantics (that is marlin.h's
 * job), nothing about G-code, and nothing about dry-run mode: dry-run is
 * enforced one layer up, in the CLI/printer session, which simply never
 * opens a serial port when dry_run is set. It also does not log; callers
 * log TX/RX lines themselves (Milestone 6).
 *
 * Timeout semantics: the timeout is a per-call deadline, measured with
 * CLOCK_MONOTONIC. printerctl_serial_read_line() has the whole timeout
 * to produce one complete line; printerctl_serial_write_line() has the
 * whole timeout to flush one complete line. A timeout is always reported
 * explicitly as PRINTERCTL_SERIAL_ERR_TIMEOUT, never silently retried.
 */

#define PRINTERCTL_SERIAL_ERROR_MAX 256

typedef enum {
    PRINTERCTL_SERIAL_OK = 0,
    PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
    PRINTERCTL_SERIAL_ERR_NOT_OPEN,
    PRINTERCTL_SERIAL_ERR_OPEN_FAILED,
    PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
    PRINTERCTL_SERIAL_ERR_UNSUPPORTED_BAUD,
    PRINTERCTL_SERIAL_ERR_TIMEOUT,
    PRINTERCTL_SERIAL_ERR_IO,
    PRINTERCTL_SERIAL_ERR_DISCONNECTED,
    PRINTERCTL_SERIAL_ERR_LINE_TOO_LONG
} printerctl_serial_status_t;

/*
 * Stack-only port state. `fd` is owned by the port between a successful
 * open() and close(). Callers must not read or write the descriptor
 * directly, and must not close() it themselves.
 */
typedef struct {
    int fd;
    bool open;
    int baud;
    int timeout_ms;
} printerctl_serial_port_t;

/* `status` mirrors the function's return value; `message` is operator-facing. */
typedef struct {
    printerctl_serial_status_t status;
    char message[PRINTERCTL_SERIAL_ERROR_MAX];
} printerctl_serial_error_t;

/*
 * Opens the terminal device at `path` and configures it for Marlin
 * communication: raw mode (no echo, no canonical input, no CR/LF
 * translation, no XON/XOFF or RTS/CTS flow control), 8 data bits, no
 * parity, one stop bit, receiver enabled, modem-control lines ignored,
 * and the requested baud rate applied to both directions. Any bytes
 * already pending in either direction are flushed so a session never
 * starts by reading a stale buffer.
 *
 * Always overwrites the full contents of *port, on both success and
 * failure, so callers never need to zero it first.
 *
 * `port`, `path` and `out_error` must be non-NULL and `timeout_ms` must
 * be positive (PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT otherwise).
 * `baud` must be one of the standard rates the Linux termios interface
 * supports (9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000,
 * 576000, 921600, 1000000); any other value — including Marlin's
 * nonstandard 250000, which needs termios2/BOTHER and has no consumer
 * yet — fails with PRINTERCTL_SERIAL_ERR_UNSUPPORTED_BAUD before the
 * device is touched. Returns PRINTERCTL_SERIAL_ERR_OPEN_FAILED when the
 * device cannot be opened, or PRINTERCTL_SERIAL_ERR_CONFIG_FAILED when
 * it is not a terminal or raw mode / the baud rate cannot be applied and
 * verified; in every failure case the descriptor is closed again and
 * *port is left not-open.
 */
printerctl_serial_status_t printerctl_serial_open(
    printerctl_serial_port_t *port,
    const char *path,
    int baud,
    int timeout_ms,
    printerctl_serial_error_t *out_error);

/*
 * Writes `line` followed by a single '\n' terminator, handling partial
 * writes until the whole line is flushed or the timeout expires. `line`
 * must not contain '\n' or '\r' (PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT;
 * the G-code reader and the M6 commands never produce embedded line
 * endings, so one call is always exactly one Marlin command line).
 *
 * Returns PRINTERCTL_SERIAL_ERR_TIMEOUT if the line cannot be fully
 * written within the port's timeout (the line may then be partially
 * transmitted — the caller must treat the exchange as failed, not
 * retry the same line blindly), PRINTERCTL_SERIAL_ERR_DISCONNECTED when
 * the device has gone away (EPIPE/EIO or hangup), or
 * PRINTERCTL_SERIAL_ERR_IO for any other write failure.
 */
printerctl_serial_status_t printerctl_serial_write_line(
    printerctl_serial_port_t *port,
    const char *line,
    printerctl_serial_error_t *out_error);

/*
 * Reads one line into `buffer` (capacity `buffer_size`, which must be at
 * least 2), consuming bytes up to and including the next '\n', which is
 * not stored. Trailing '\r' bytes before the '\n' are stripped, so both
 * of Marlin's line endings ('\n' and '\r\n') yield the same result. The
 * stored line is always NUL-terminated.
 *
 * Returns PRINTERCTL_SERIAL_ERR_TIMEOUT if no complete line arrives
 * within the port's timeout — bytes received so far remain in *buffer
 * (NUL-terminated) for diagnostics, but the call has failed and those
 * bytes must not be interpreted as a response.
 * PRINTERCTL_SERIAL_ERR_LINE_TOO_LONG is returned when a line does not
 * fit the buffer (the overlong line's remainder is left unread; the
 * exchange should be treated as failed).
 * PRINTERCTL_SERIAL_ERR_DISCONNECTED is returned when the device
 * reports end-of-file or hangup (USB unplugged, pseudo-terminal peer
 * closed), and PRINTERCTL_SERIAL_ERR_IO for any other read failure.
 */
printerctl_serial_status_t printerctl_serial_read_line(
    printerctl_serial_port_t *port,
    char *buffer,
    size_t buffer_size,
    printerctl_serial_error_t *out_error);

/*
 * Closes the port's descriptor and marks *port not-open. Idempotent:
 * calling this again on an already-closed (or never-opened) port is a
 * no-op returning PRINTERCTL_SERIAL_OK. Returns
 * PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT for a NULL port or out_error,
 * or PRINTERCTL_SERIAL_ERR_IO if close() itself fails; either way *port
 * is left marked not-open, so a failed close can never be retried into
 * a double close().
 */
printerctl_serial_status_t printerctl_serial_close(
    printerctl_serial_port_t *port,
    printerctl_serial_error_t *out_error);

#endif
