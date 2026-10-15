/*
 * _DEFAULT_SOURCE (not just _POSIX_C_SOURCE) because the Linux-only
 * target (Agents.md) uses cfsetspeed() and the high baud-rate constants
 * (B460800 and up), which POSIX does not define.
 */
#define _DEFAULT_SOURCE

#include "printerctl/serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PRINTERCTL_SERIAL_WAIT_HOOK_POLL_MS 20

static void set_error(printerctl_serial_error_t *out_error,
                       printerctl_serial_status_t status,
                       const char *format, ...)
{
    out_error->status = status;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static void reset_port(printerctl_serial_port_t *port)
{
    port->fd = -1;
    port->open = false;
    port->baud = 0;
    port->timeout_ms = 0;
}

/*
 * Maps a numeric baud rate to its termios constant. Only the standard
 * rates Linux exposes as Bxxx constants are supported; anything else
 * (notably Marlin's nonstandard 250000, which needs termios2/BOTHER) is
 * rejected before the device is touched.
 */
static bool baud_to_speed(int baud, speed_t *out_speed)
{
    switch (baud) {
    case 9600: *out_speed = B9600; return true;
    case 19200: *out_speed = B19200; return true;
    case 38400: *out_speed = B38400; return true;
    case 57600: *out_speed = B57600; return true;
    case 115200: *out_speed = B115200; return true;
    case 230400: *out_speed = B230400; return true;
    case 460800: *out_speed = B460800; return true;
    case 500000: *out_speed = B500000; return true;
    case 576000: *out_speed = B576000; return true;
    case 921600: *out_speed = B921600; return true;
    case 1000000: *out_speed = B1000000; return true;
    default: return false;
    }
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*
 * Milliseconds left before `deadline_ms`, clamped to 0 and to INT_MAX-safe
 * values for poll(). A zero result means the deadline has already passed.
 */
static int remaining_ms(uint64_t deadline_ms)
{
    uint64_t now = monotonic_ms();
    if (now >= deadline_ms) {
        return 0;
    }
    uint64_t remaining = deadline_ms - now;
    if (remaining > 60000u) {
        remaining = 60000u;
    }
    return (int)remaining;
}

/*
 * Waits until the descriptor is ready for `events` (POLLIN or POLLOUT) or
 * the deadline passes. Returns PRINTERCTL_SERIAL_OK when ready,
 * ERR_TIMEOUT when the deadline expires first, ERR_DISCONNECTED on
 * hangup/error conditions reported by poll(), or ERR_IO if poll() itself
 * fails. EINTR restarts the wait with the remaining time.
 */
static printerctl_serial_status_t wait_ready_with_hook(
    int fd,
    short events,
    uint64_t deadline_ms,
    printerctl_serial_wait_hook_fn on_wait,
    void *wait_context,
    printerctl_serial_interrupt_fn should_interrupt,
    void *interrupt_context)
{
    for (;;) {
        if (should_interrupt != NULL && should_interrupt(interrupt_context)) {
            return PRINTERCTL_SERIAL_INTERRUPTED;
        }
        struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};

        int timeout = remaining_ms(deadline_ms);
        if (timeout == 0) {
            return PRINTERCTL_SERIAL_ERR_TIMEOUT;
        }
        if ((on_wait != NULL || should_interrupt != NULL) &&
            timeout > PRINTERCTL_SERIAL_WAIT_HOOK_POLL_MS) {
            timeout = PRINTERCTL_SERIAL_WAIT_HOOK_POLL_MS;
        }

        int rc = poll(&pfd, 1, timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PRINTERCTL_SERIAL_ERR_IO;
        }
        if (rc == 0) {
            if (on_wait != NULL) {
                on_wait(wait_context);
                continue;
            }
            if (should_interrupt != NULL) continue;
            return PRINTERCTL_SERIAL_ERR_TIMEOUT;
        }

        /* Readable data may accompany a hangup; drain it before
         * reporting the disconnect, so a final response is not lost. */
        if (pfd.revents & events) {
            return PRINTERCTL_SERIAL_OK;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return PRINTERCTL_SERIAL_ERR_DISCONNECTED;
        }
    }
}

static printerctl_serial_status_t wait_ready(int fd, short events, uint64_t deadline_ms)
{
    return wait_ready_with_hook(fd, events, deadline_ms, NULL, NULL, NULL, NULL);
}

printerctl_serial_status_t printerctl_serial_open(
    printerctl_serial_port_t *port,
    const char *path,
    int baud,
    int timeout_ms,
    printerctl_serial_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_SERIAL_OK;

    if (port == NULL) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT, "port must not be NULL");
        return out_error->status;
    }

    reset_port(port);

    if (path == NULL) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT, "path must not be NULL");
        return out_error->status;
    }

    if (timeout_ms <= 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
                   "timeout must be positive, got %d ms", timeout_ms);
        return out_error->status;
    }

    speed_t speed;
    if (!baud_to_speed(baud, &speed)) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_UNSUPPORTED_BAUD,
                   "baud rate %d is not a supported standard rate", baud);
        return out_error->status;
    }

    /* O_NONBLOCK so open() never hangs on modem-control lines and so all
     * later I/O is bounded purely by our own poll() deadlines. O_NOCTTY
     * so the device can never become this process's controlling
     * terminal. */
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_OPEN_FAILED,
                   "cannot open '%s': %s", path, strerror(errno));
        return out_error->status;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
                   "'%s' is not a terminal device: %s", path, strerror(errno));
        close(fd);
        return out_error->status;
    }

    /* Raw 8N1: no input translation or flow control, no output
     * post-processing, no echo/canonical/signal handling, receiver on,
     * modem-control lines ignored. Spelled out rather than via
     * cfmakeraw() so the exact configuration is visible and testable. */
    tio.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tio.c_oflag &= ~(tcflag_t)OPOST;
    tio.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= (tcflag_t)(CS8 | CREAD | CLOCAL);

    /* Non-blocking descriptor + poll() own all timing; VMIN/VTIME must
     * not add a second timeout layer. */
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
                   "cannot set baud rate %d on '%s': %s", baud, path, strerror(errno));
        close(fd);
        return out_error->status;
    }

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
                   "cannot configure '%s': %s", path, strerror(errno));
        close(fd);
        return out_error->status;
    }

    /* tcsetattr() succeeds if *any* requested change was applied; read
     * the configuration back and verify the parts that matter. */
    struct termios applied;
    if (tcgetattr(fd, &applied) != 0 ||
        cfgetispeed(&applied) != speed || cfgetospeed(&applied) != speed ||
        (applied.c_lflag & (ICANON | ECHO)) != 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
                   "configuration of '%s' did not fully apply", path);
        close(fd);
        return out_error->status;
    }

    /* Never start a session by reading stale bytes from before this
     * open. (Marlin's post-reset 'start' banner arrives later, after the
     * DTR-triggered reboot, and is not affected.) */
    if (tcflush(fd, TCIOFLUSH) != 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
                   "cannot flush '%s': %s", path, strerror(errno));
        close(fd);
        return out_error->status;
    }

    /* Exclusive mode (Milestone 8): while this port is held open, any
     * other open() of the device fails with EBUSY, so a concurrent
     * printerctl invocation cannot interleave bytes into an active
     * exchange or stream. Cleared again on close. (A CAP_SYS_ADMIN
     * process can still bypass this — kernel semantics.) */
    if (ioctl(fd, TIOCEXCL) != 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
                   "cannot take exclusive access to '%s': %s", path, strerror(errno));
        close(fd);
        return out_error->status;
    }

    port->fd = fd;
    port->open = true;
    port->baud = baud;
    port->timeout_ms = timeout_ms;

    return PRINTERCTL_SERIAL_OK;
}

printerctl_serial_status_t printerctl_serial_write_line(
    printerctl_serial_port_t *port,
    const char *line,
    printerctl_serial_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_SERIAL_OK;

    if (port == NULL || line == NULL) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
                   "port and line must not be NULL");
        return out_error->status;
    }

    if (!port->open) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_NOT_OPEN, "port is not open");
        return out_error->status;
    }

    if (strchr(line, '\n') != NULL || strchr(line, '\r') != NULL) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
                   "line must not contain embedded line endings");
        return out_error->status;
    }

    uint64_t deadline = monotonic_ms() + (uint64_t)port->timeout_ms;
    size_t line_length = strlen(line);

    /* Two segments — the line and its '\n' terminator — written through
     * one partial-write loop, so no assembled copy of the line is needed. */
    const char *segments[2] = {line, "\n"};
    size_t lengths[2] = {line_length, 1};

    for (int segment = 0; segment < 2; segment++) {
        size_t written = 0;
        while (written < lengths[segment]) {
            printerctl_serial_status_t ready = wait_ready(port->fd, POLLOUT, deadline);
            if (ready == PRINTERCTL_SERIAL_ERR_TIMEOUT) {
                set_error(out_error, ready,
                           "timed out after %d ms writing a %zu-byte line",
                           port->timeout_ms, line_length + 1);
                return out_error->status;
            }
            if (ready == PRINTERCTL_SERIAL_ERR_DISCONNECTED) {
                set_error(out_error, ready, "device disconnected during write");
                return out_error->status;
            }
            if (ready != PRINTERCTL_SERIAL_OK) {
                set_error(out_error, ready, "error waiting to write: %s", strerror(errno));
                return out_error->status;
            }

            ssize_t rc = write(port->fd, segments[segment] + written,
                                lengths[segment] - written);
            if (rc < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                if (errno == EPIPE || errno == EIO) {
                    set_error(out_error, PRINTERCTL_SERIAL_ERR_DISCONNECTED,
                               "device disconnected during write: %s", strerror(errno));
                    return out_error->status;
                }
                set_error(out_error, PRINTERCTL_SERIAL_ERR_IO,
                           "write failed: %s", strerror(errno));
                return out_error->status;
            }
            written += (size_t)rc;
        }
    }

    return PRINTERCTL_SERIAL_OK;
}

printerctl_serial_status_t printerctl_serial_read_line(
    printerctl_serial_port_t *port,
    char *buffer,
    size_t buffer_size,
    printerctl_serial_error_t *out_error)
{
    return printerctl_serial_read_line_with_hook(port, buffer, buffer_size, NULL, NULL,
                                                out_error);
}

static printerctl_serial_status_t read_line_controlled(
    printerctl_serial_port_t *port,
    char *buffer,
    size_t buffer_size,
    printerctl_serial_wait_hook_fn on_wait,
    void *wait_context,
    printerctl_serial_interrupt_fn should_interrupt,
    void *interrupt_context,
    printerctl_serial_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_SERIAL_OK;

    if (port == NULL || buffer == NULL || buffer_size < 2) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
                   "port and buffer must not be NULL and buffer_size must be at least 2");
        return out_error->status;
    }

    buffer[0] = '\0';

    if (!port->open) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_NOT_OPEN, "port is not open");
        return out_error->status;
    }

    uint64_t deadline = monotonic_ms() + (uint64_t)port->timeout_ms;
    size_t length = 0;

    for (;;) {
        printerctl_serial_status_t ready =
            wait_ready_with_hook(port->fd, POLLIN, deadline, on_wait, wait_context,
                                 should_interrupt, interrupt_context);
        if (ready == PRINTERCTL_SERIAL_INTERRUPTED) {
            buffer[length] = '\0';
            set_error(out_error, ready, "read interrupted by caller");
            return ready;
        }
        if (ready == PRINTERCTL_SERIAL_ERR_TIMEOUT) {
            buffer[length] = '\0';
            set_error(out_error, ready,
                       "timed out after %d ms waiting for a line (%zu byte(s) received)",
                       port->timeout_ms, length);
            return out_error->status;
        }
        if (ready == PRINTERCTL_SERIAL_ERR_DISCONNECTED) {
            buffer[length] = '\0';
            set_error(out_error, ready, "device disconnected during read");
            return out_error->status;
        }
        if (ready != PRINTERCTL_SERIAL_OK) {
            buffer[length] = '\0';
            set_error(out_error, ready, "error waiting to read: %s", strerror(errno));
            return out_error->status;
        }

        char c;
        ssize_t rc = read(port->fd, &c, 1);
        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            buffer[length] = '\0';
            /* A pseudo-terminal whose peer closed reports EIO; a real USB
             * device that vanished reports EIO or ENXIO. Both are
             * disconnects, not generic I/O failures. */
            if (errno == EIO || errno == ENXIO || errno == ENODEV) {
                set_error(out_error, PRINTERCTL_SERIAL_ERR_DISCONNECTED,
                           "device disconnected during read: %s", strerror(errno));
            } else {
                set_error(out_error, PRINTERCTL_SERIAL_ERR_IO,
                           "read failed: %s", strerror(errno));
            }
            return out_error->status;
        }
        if (rc == 0) {
            buffer[length] = '\0';
            set_error(out_error, PRINTERCTL_SERIAL_ERR_DISCONNECTED,
                       "device reported end of file");
            return out_error->status;
        }

        if (c == '\n') {
            /* Tolerate Marlin's '\r\n' as well as bare '\n'. */
            while (length > 0 && buffer[length - 1] == '\r') {
                length--;
            }
            buffer[length] = '\0';
            return PRINTERCTL_SERIAL_OK;
        }

        if (length >= buffer_size - 1) {
            buffer[length] = '\0';
            set_error(out_error, PRINTERCTL_SERIAL_ERR_LINE_TOO_LONG,
                       "line exceeds the %zu-byte buffer", buffer_size);
            return out_error->status;
        }

        buffer[length++] = c;
    }
}

printerctl_serial_status_t printerctl_serial_read_line_with_hook(
    printerctl_serial_port_t *port, char *buffer, size_t buffer_size,
    printerctl_serial_wait_hook_fn on_wait, void *context,
    printerctl_serial_error_t *out_error)
{
    return read_line_controlled(port, buffer, buffer_size, on_wait, context,
                                NULL, NULL, out_error);
}

printerctl_serial_status_t printerctl_serial_read_line_interruptible(
    printerctl_serial_port_t *port, char *buffer, size_t buffer_size,
    printerctl_serial_interrupt_fn should_interrupt, void *context,
    printerctl_serial_error_t *out_error)
{
    return read_line_controlled(port, buffer, buffer_size, NULL, NULL,
                                should_interrupt, context, out_error);
}

printerctl_serial_status_t printerctl_serial_close(
    printerctl_serial_port_t *port,
    printerctl_serial_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_SERIAL_OK;

    if (port == NULL) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT, "port must not be NULL");
        return out_error->status;
    }

    if (!port->open) {
        return PRINTERCTL_SERIAL_OK;
    }

    int fd = port->fd;
    reset_port(port);

    /* Best-effort: the kernel also drops exclusive mode on the last
     * close, but releasing it explicitly keeps the port reopenable even
     * if another descriptor to it exists somewhere. */
    (void)ioctl(fd, TIOCNXCL);

    if (close(fd) != 0) {
        set_error(out_error, PRINTERCTL_SERIAL_ERR_IO,
                   "error closing port: %s", strerror(errno));
        return out_error->status;
    }

    return PRINTERCTL_SERIAL_OK;
}
