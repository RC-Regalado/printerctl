/* posix_openpt/grantpt/unlockpt/ptsname need XOPEN; high baud constants need DEFAULT. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "printerctl/serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*
 * Opens a pseudo-terminal pair: *out_master receives the master
 * descriptor, `slave_path` the slave device path the serial module will
 * open as if it were a USB serial device. Returns false on failure.
 */
static bool open_pty(int *out_master, char *slave_path, size_t slave_path_size)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        fprintf(stderr, "FAIL: posix_openpt: %s\n", strerror(errno));
        g_failures++;
        return false;
    }

    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        fprintf(stderr, "FAIL: grantpt/unlockpt: %s\n", strerror(errno));
        g_failures++;
        close(master);
        return false;
    }

    const char *name = ptsname(master);
    if (name == NULL || strlen(name) >= slave_path_size) {
        fprintf(stderr, "FAIL: ptsname\n");
        g_failures++;
        close(master);
        return false;
    }

    strcpy(slave_path, name);
    *out_master = master;
    return true;
}

/* Reads from the master side until `expected` bytes arrive or ~1s passes. */
static bool read_from_master(int master, char *buffer, size_t expected)
{
    size_t received = 0;
    uint64_t deadline = monotonic_ms() + 1000;

    while (received < expected) {
        struct pollfd pfd = {.fd = master, .events = POLLIN, .revents = 0};
        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            return false;
        }
        int rc = poll(&pfd, 1, (int)(deadline - now));
        if (rc <= 0) {
            return false;
        }
        ssize_t n = read(master, buffer + received, expected - received);
        if (n <= 0) {
            return false;
        }
        received += (size_t)n;
    }
    return true;
}

/* True when nothing arrives on the master side within `wait_ms`. */
static bool master_stays_silent(int master, int wait_ms)
{
    struct pollfd pfd = {.fd = master, .events = POLLIN, .revents = 0};
    return poll(&pfd, 1, wait_ms) == 0;
}

static void test_open_configures_raw_mode(void)
{
    int master;
    char slave_path[64];
    if (!open_pty(&master, slave_path, sizeof(slave_path))) {
        return;
    }

    printerctl_serial_port_t port;
    printerctl_serial_error_t error;

    printerctl_serial_status_t status =
        printerctl_serial_open(&port, slave_path, 115200, 1000, &error);
    check(status == PRINTERCTL_SERIAL_OK, "a pseudo-terminal slave opens");
    check(port.open && port.fd >= 0, "the port records its open descriptor");
    check(port.baud == 115200 && port.timeout_ms == 1000,
          "the port records its baud and timeout");

    struct termios tio;
    check(tcgetattr(port.fd, &tio) == 0, "the configured port is a terminal");
    check((tio.c_lflag & (ICANON | ECHO | ISIG)) == 0,
          "canonical mode, echo and signals are disabled");
    check((tio.c_iflag & (ICRNL | INLCR | IXON | IXOFF)) == 0,
          "CR/LF translation and software flow control are disabled");
    check((tio.c_oflag & OPOST) == 0, "output post-processing is disabled");
    check((tio.c_cflag & CSIZE) == CS8 && (tio.c_cflag & PARENB) == 0,
          "8 data bits, no parity");
    check((tio.c_cflag & (CREAD | CLOCAL)) == (CREAD | CLOCAL),
          "receiver enabled, modem-control lines ignored");
    check(cfgetispeed(&tio) == B115200 && cfgetospeed(&tio) == B115200,
          "the requested baud rate is applied in both directions");

    check(printerctl_serial_close(&port, &error) == PRINTERCTL_SERIAL_OK, "close succeeds");
    check(!port.open, "close marks the port not-open");
    check(printerctl_serial_close(&port, &error) == PRINTERCTL_SERIAL_OK,
          "a second close is a no-op");

    close(master);
}

static void test_normal_line_exchange(void)
{
    int master;
    char slave_path[64];
    if (!open_pty(&master, slave_path, sizeof(slave_path))) {
        return;
    }

    printerctl_serial_port_t port;
    printerctl_serial_error_t error;
    check(printerctl_serial_open(&port, slave_path, 115200, 1000, &error) ==
              PRINTERCTL_SERIAL_OK,
          "port opens for the exchange test");

    /* TX: one command becomes exactly the line plus '\n', nothing else. */
    check(printerctl_serial_write_line(&port, "M105", &error) == PRINTERCTL_SERIAL_OK,
          "write_line succeeds");
    char received[8] = {0};
    check(read_from_master(master, received, 5) && memcmp(received, "M105\n", 5) == 0,
          "the mock printer receives the command terminated by a single newline");
    check(master_stays_silent(master, 50),
          "nothing beyond the terminated command is transmitted (and no echo)");

    /* RX: a plain '\n'-terminated response. */
    const char *reply = "ok T:210.00 /210.00 B:60.00 /60.00\n";
    check(write(master, reply, strlen(reply)) == (ssize_t)strlen(reply),
          "mock reply is written");
    char line[128];
    check(printerctl_serial_read_line(&port, line, sizeof(line), &error) ==
              PRINTERCTL_SERIAL_OK,
          "read_line returns the reply");
    check(strcmp(line, "ok T:210.00 /210.00 B:60.00 /60.00") == 0,
          "the reply arrives without its newline");

    /* RX: '\r\n' yields the same text as '\n'. */
    check(write(master, "ok\r\n", 4) == 4, "CRLF reply is written");
    check(printerctl_serial_read_line(&port, line, sizeof(line), &error) ==
              PRINTERCTL_SERIAL_OK &&
              strcmp(line, "ok") == 0,
          "a CRLF-terminated reply is stripped to the same text");

    /* RX: two lines in one burst are returned one call at a time. */
    const char *burst = "echo:busy: processing\nok\n";
    check(write(master, burst, strlen(burst)) == (ssize_t)strlen(burst),
          "burst of two lines is written");
    check(printerctl_serial_read_line(&port, line, sizeof(line), &error) ==
              PRINTERCTL_SERIAL_OK &&
              strcmp(line, "echo:busy: processing") == 0,
          "the first line of a burst is returned first");
    check(printerctl_serial_read_line(&port, line, sizeof(line), &error) ==
              PRINTERCTL_SERIAL_OK &&
              strcmp(line, "ok") == 0,
          "the second line of a burst is returned second");

    /* RX: an empty line is a valid, empty result. */
    check(write(master, "\n", 1) == 1, "empty line is written");
    check(printerctl_serial_read_line(&port, line, sizeof(line), &error) ==
              PRINTERCTL_SERIAL_OK &&
              line[0] == '\0',
          "an empty line reads as an empty string");

    printerctl_serial_close(&port, &error);
    close(master);
}

static void test_read_timeout_is_explicit_and_bounded(void)
{
    int master;
    char slave_path[64];
    if (!open_pty(&master, slave_path, sizeof(slave_path))) {
        return;
    }

    printerctl_serial_port_t port;
    printerctl_serial_error_t error;
    check(printerctl_serial_open(&port, slave_path, 115200, 200, &error) ==
              PRINTERCTL_SERIAL_OK,
          "port opens for the timeout test");

    /* Nothing sent at all. */
    char line[64];
    uint64_t started = monotonic_ms();
    printerctl_serial_status_t status =
        printerctl_serial_read_line(&port, line, sizeof(line), &error);
    uint64_t elapsed = monotonic_ms() - started;

    check(status == PRINTERCTL_SERIAL_ERR_TIMEOUT, "silence reports an explicit timeout");
    check(elapsed >= 150, "the timeout waits close to the configured 200 ms");
    check(elapsed < 2000, "the timeout does not wait far beyond the configured 200 ms");
    check(error.message[0] != '\0', "the timeout carries an operator-facing message");

    /* A partial line without a terminator also times out; the partial
     * bytes are visible for diagnostics but the call failed. */
    check(write(master, "ok", 2) == 2, "partial reply is written");
    status = printerctl_serial_read_line(&port, line, sizeof(line), &error);
    check(status == PRINTERCTL_SERIAL_ERR_TIMEOUT,
          "an unterminated partial line reports a timeout");
    check(strcmp(line, "ok") == 0, "the partial bytes are left in the buffer, NUL-terminated");

    printerctl_serial_close(&port, &error);
    close(master);
}

static void test_write_timeout_on_full_buffer(void)
{
    int master;
    char slave_path[64];
    if (!open_pty(&master, slave_path, sizeof(slave_path))) {
        return;
    }

    printerctl_serial_port_t port;
    printerctl_serial_error_t error;
    check(printerctl_serial_open(&port, slave_path, 115200, 200, &error) ==
              PRINTERCTL_SERIAL_OK,
          "port opens for the write-timeout test");

    /* A line far larger than any pty buffer, with nobody reading the
     * master side: the partial-write loop must hit the deadline. */
    size_t huge = 1 << 20;
    char *line = malloc(huge + 1);
    memset(line, 'G', huge);
    line[huge] = '\0';

    printerctl_serial_status_t status = printerctl_serial_write_line(&port, line, &error);
    check(status == PRINTERCTL_SERIAL_ERR_TIMEOUT,
          "a write nobody drains reports an explicit timeout");

    free(line);
    printerctl_serial_close(&port, &error);
    close(master);
}

static void test_disconnect_is_explicit(void)
{
    int master;
    char slave_path[64];
    if (!open_pty(&master, slave_path, sizeof(slave_path))) {
        return;
    }

    printerctl_serial_port_t port;
    printerctl_serial_error_t error;
    check(printerctl_serial_open(&port, slave_path, 115200, 500, &error) ==
              PRINTERCTL_SERIAL_OK,
          "port opens for the disconnect test");

    close(master); /* the "printer" goes away */

    char line[64];
    printerctl_serial_status_t status =
        printerctl_serial_read_line(&port, line, sizeof(line), &error);
    check(status == PRINTERCTL_SERIAL_ERR_DISCONNECTED,
          "reading a vanished device reports DISCONNECTED, not a timeout");

    status = printerctl_serial_write_line(&port, "M105", &error);
    check(status == PRINTERCTL_SERIAL_ERR_DISCONNECTED || status == PRINTERCTL_SERIAL_ERR_IO,
          "writing to a vanished device fails explicitly");
    check(status != PRINTERCTL_SERIAL_ERR_TIMEOUT,
          "writing to a vanished device is not misreported as a timeout");

    printerctl_serial_close(&port, &error);
}

static void test_line_too_long_for_buffer(void)
{
    int master;
    char slave_path[64];
    if (!open_pty(&master, slave_path, sizeof(slave_path))) {
        return;
    }

    printerctl_serial_port_t port;
    printerctl_serial_error_t error;
    check(printerctl_serial_open(&port, slave_path, 115200, 500, &error) ==
              PRINTERCTL_SERIAL_OK,
          "port opens for the overlong-line test");

    const char *long_line = "0123456789ABCDEF\n";
    check(write(master, long_line, strlen(long_line)) == (ssize_t)strlen(long_line),
          "overlong reply is written");

    char line[8];
    printerctl_serial_status_t status =
        printerctl_serial_read_line(&port, line, sizeof(line), &error);
    check(status == PRINTERCTL_SERIAL_ERR_LINE_TOO_LONG,
          "a line that does not fit the buffer is rejected explicitly");
    check(strcmp(line, "0123456") == 0, "the buffer holds the NUL-terminated prefix");

    printerctl_serial_close(&port, &error);
    close(master);
}

static void test_open_failures(void)
{
    printerctl_serial_port_t port;
    printerctl_serial_error_t error;

    check(printerctl_serial_open(&port, "/nonexistent/ttyACM99", 115200, 1000, &error) ==
              PRINTERCTL_SERIAL_ERR_OPEN_FAILED,
          "a missing device fails with OPEN_FAILED");
    check(!port.open, "a failed open leaves the port not-open");
    check(error.message[0] != '\0', "OPEN_FAILED carries a message");

    check(printerctl_serial_open(&port, "/dev/null", 115200, 1000, &error) ==
              PRINTERCTL_SERIAL_ERR_CONFIG_FAILED,
          "a non-terminal device fails with CONFIG_FAILED");

    check(printerctl_serial_open(&port, "/dev/null", 250000, 1000, &error) ==
              PRINTERCTL_SERIAL_ERR_UNSUPPORTED_BAUD,
          "the nonstandard 250000 baud is rejected before the device is touched");
    check(printerctl_serial_open(&port, "/dev/null", 123, 1000, &error) ==
              PRINTERCTL_SERIAL_ERR_UNSUPPORTED_BAUD,
          "an arbitrary baud value is rejected");
}

static void test_invalid_arguments_and_lifecycle(void)
{
    printerctl_serial_port_t port;
    printerctl_serial_error_t error;
    char line[16];

    check(printerctl_serial_open(NULL, "/dev/null", 115200, 1000, &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "open with a NULL port is rejected");
    check(printerctl_serial_open(&port, NULL, 115200, 1000, &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "open with a NULL path is rejected");
    check(printerctl_serial_open(&port, "/dev/null", 115200, 0, &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "open with a zero timeout is rejected");
    check(printerctl_serial_open(&port, "/dev/null", 115200, 1000, NULL) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "open with a NULL out_error is rejected");

    /* port was reset to not-open by the rejected opens above. */
    check(printerctl_serial_write_line(&port, "M105", &error) ==
              PRINTERCTL_SERIAL_ERR_NOT_OPEN,
          "write on a never-opened port reports NOT_OPEN");
    check(printerctl_serial_read_line(&port, line, sizeof(line), &error) ==
              PRINTERCTL_SERIAL_ERR_NOT_OPEN,
          "read on a never-opened port reports NOT_OPEN");
    check(printerctl_serial_close(&port, &error) == PRINTERCTL_SERIAL_OK,
          "close on a never-opened port is a no-op");

    /* Argument validation on a live port. */
    int master;
    char slave_path[64];
    if (!open_pty(&master, slave_path, sizeof(slave_path))) {
        return;
    }
    check(printerctl_serial_open(&port, slave_path, 115200, 500, &error) ==
              PRINTERCTL_SERIAL_OK,
          "port opens for argument validation");

    check(printerctl_serial_write_line(&port, NULL, &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "write with a NULL line is rejected");
    check(printerctl_serial_write_line(&port, "M105\nM106", &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "write with an embedded newline is rejected");
    check(printerctl_serial_write_line(&port, "M105\r", &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "write with an embedded carriage return is rejected");
    check(printerctl_serial_read_line(&port, NULL, 16, &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "read with a NULL buffer is rejected");
    check(printerctl_serial_read_line(&port, line, 1, &error) ==
              PRINTERCTL_SERIAL_ERR_INVALID_ARGUMENT,
          "read with a one-byte buffer is rejected");

    check(printerctl_serial_close(&port, &error) == PRINTERCTL_SERIAL_OK,
          "close after validation succeeds");
    check(printerctl_serial_write_line(&port, "M105", &error) ==
              PRINTERCTL_SERIAL_ERR_NOT_OPEN,
          "write after close reports NOT_OPEN");

    close(master);
}

int main(void)
{
    test_open_configures_raw_mode();
    test_normal_line_exchange();
    test_read_timeout_is_explicit_and_bounded();
    test_write_timeout_on_full_buffer();
    test_disconnect_is_explicit();
    test_line_too_long_for_buffer();
    test_open_failures();
    test_invalid_arguments_and_lifecycle();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All serial checks passed\n");
    return EXIT_SUCCESS;
}
