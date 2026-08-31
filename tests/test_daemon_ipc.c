/*
 * Milestone 13 unit tests for the internal cmd_pipe/event_pipe
 * envelopes: framing, correlation tokens and malformed-message
 * rejection, over plain pipe() fd pairs -- no threads involved, this is
 * the layer below src/daemon/worker.c and src/daemon/runtime.c.
 */
#include "printerctl/daemon/ipc.h"

#include <stdbool.h>
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

static void test_command_round_trip(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        check(false, "pipe() for the command round-trip test");
        return;
    }

    printerctl_daemon_command_t sent;
    memset(&sent, 0, sizeof(sent));
    sent.wire_size = sizeof(sent);
    sent.type = PRINTERCTL_DAEMON_CMD_NOP;
    sent.token = 42;

    check(printerctl_daemon_ipc_write_command(fds[1], &sent) == PRINTERCTL_DAEMON_IPC_OK,
          "a valid command writes successfully");

    printerctl_daemon_command_t received;
    check(printerctl_daemon_ipc_read_command(fds[0], &received) == PRINTERCTL_DAEMON_IPC_OK,
          "the command reads back successfully");
    check(received.type == PRINTERCTL_DAEMON_CMD_NOP, "the command type round-trips");
    check(received.token == 42, "the correlation token round-trips");

    close(fds[0]);
    close(fds[1]);
}

static const char *event_type_name(printerctl_daemon_event_type_t type)
{
    switch (type) {
    case PRINTERCTL_DAEMON_EVENT_READY: return "READY";
    case PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED: return "COMMAND_COMPLETED";
    case PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE: return "PRINT_UPDATE";
    case PRINTERCTL_DAEMON_EVENT_STOPPING: return "STOPPING";
    case PRINTERCTL_DAEMON_EVENT_STOPPED: return "STOPPED";
    case PRINTERCTL_DAEMON_EVENT_FAULT: return "FAULT";
    }
    return "?";
}

static void test_event_round_trip_all_types(void)
{
    static const printerctl_daemon_event_type_t types[] = {
        PRINTERCTL_DAEMON_EVENT_READY,
        PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED,
        PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE,
        PRINTERCTL_DAEMON_EVENT_STOPPING,
        PRINTERCTL_DAEMON_EVENT_STOPPED,
        PRINTERCTL_DAEMON_EVENT_FAULT,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        int fds[2];
        if (pipe(fds) != 0) {
            check(false, "pipe() for an event round-trip test");
            continue;
        }

        printerctl_daemon_event_t sent;
        memset(&sent, 0, sizeof(sent));
        sent.wire_size = sizeof(sent);
        sent.type = types[i];
        sent.token = 7;
        sent.result = types[i] == PRINTERCTL_DAEMON_EVENT_FAULT ? PRINTERCTL_DAEMON_RESULT_ERROR
                                                                  : PRINTERCTL_DAEMON_RESULT_OK;
        if (types[i] == PRINTERCTL_DAEMON_EVENT_FAULT) {
            snprintf(sent.detail, sizeof(sent.detail), "synthetic fault");
        }

        check(printerctl_daemon_ipc_write_event(fds[1], &sent) == PRINTERCTL_DAEMON_IPC_OK,
              "a valid event writes successfully");

        printerctl_daemon_event_t received;
        char description[96];
        snprintf(description, sizeof(description), "the %s event reads back successfully",
                  event_type_name(types[i]));
        check(printerctl_daemon_ipc_read_event(fds[0], &received) == PRINTERCTL_DAEMON_IPC_OK,
              description);
        check(received.type == sent.type, "the event type round-trips");
        check(received.token == 7, "the correlation token round-trips");
        check(received.result == sent.result, "the result round-trips");
        if (types[i] == PRINTERCTL_DAEMON_EVENT_FAULT) {
            check(strcmp(received.detail, "synthetic fault") == 0,
                  "the fault detail text round-trips");
        }

        close(fds[0]);
        close(fds[1]);
    }
}

static void test_unknown_command_type_rejected(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        check(false, "pipe() for the unknown-command-type test");
        return;
    }

    printerctl_daemon_command_t sent;
    memset(&sent, 0, sizeof(sent));
    sent.wire_size = sizeof(sent);
    sent.type = (printerctl_daemon_command_type_t)99; /* out of range on purpose */
    sent.token = 1;

    /* write_command() does not validate -- only the reader does, the
     * same asymmetry printerctl_serial_write_line() has (a writer
     * trusts its own caller; a reader trusts nothing off the wire). */
    check(printerctl_daemon_ipc_write_command(fds[1], &sent) == PRINTERCTL_DAEMON_IPC_OK,
          "an out-of-range type still writes (validation is the reader's job)");

    printerctl_daemon_command_t received;
    check(printerctl_daemon_ipc_read_command(fds[0], &received) ==
              PRINTERCTL_DAEMON_IPC_ERR_MALFORMED,
          "an unknown command type is rejected as malformed");

    close(fds[0]);
    close(fds[1]);
}

static void test_unknown_event_type_and_invalid_result_rejected(void)
{
    {
        int fds[2];
        if (pipe(fds) != 0) {
            check(false, "pipe() for the unknown-event-type test");
            return;
        }
        printerctl_daemon_event_t sent;
        memset(&sent, 0, sizeof(sent));
        sent.wire_size = sizeof(sent);
        sent.type = (printerctl_daemon_event_type_t)99;
        check(printerctl_daemon_ipc_write_event(fds[1], &sent) == PRINTERCTL_DAEMON_IPC_OK,
              "an out-of-range event type still writes");

        printerctl_daemon_event_t received;
        check(printerctl_daemon_ipc_read_event(fds[0], &received) ==
                  PRINTERCTL_DAEMON_IPC_ERR_MALFORMED,
              "an unknown event type is rejected as malformed");
        close(fds[0]);
        close(fds[1]);
    }
    {
        int fds[2];
        if (pipe(fds) != 0) {
            check(false, "pipe() for the invalid-result test");
            return;
        }
        printerctl_daemon_event_t sent;
        memset(&sent, 0, sizeof(sent));
        sent.wire_size = sizeof(sent);
        sent.type = PRINTERCTL_DAEMON_EVENT_READY;
        sent.result = (printerctl_daemon_result_t)5; /* out of range */
        check(printerctl_daemon_ipc_write_event(fds[1], &sent) == PRINTERCTL_DAEMON_IPC_OK,
              "an out-of-range result still writes");

        printerctl_daemon_event_t received;
        check(printerctl_daemon_ipc_read_event(fds[0], &received) ==
                  PRINTERCTL_DAEMON_IPC_ERR_MALFORMED,
              "an invalid result value is rejected as malformed");
        close(fds[0]);
        close(fds[1]);
    }
}

static void test_truncated_message(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        check(false, "pipe() for the truncated-message test");
        return;
    }

    printerctl_daemon_command_t sent;
    memset(&sent, 0, sizeof(sent));
    sent.wire_size = sizeof(sent);
    sent.type = PRINTERCTL_DAEMON_CMD_NOP;

    /* Write fewer raw bytes than a full command, then close the write
     * end -- the reader must see a short read followed by EOF mid-message,
     * not silently succeed on partial/garbage data. */
    size_t short_length = sizeof(sent) / 2;
    check(write(fds[1], &sent, short_length) == (ssize_t)short_length,
          "the short write for the truncation fixture succeeds");
    close(fds[1]);

    printerctl_daemon_command_t received;
    check(printerctl_daemon_ipc_read_command(fds[0], &received) ==
              PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED,
          "a message that ends mid-way through is reported as truncated, not EOF");

    close(fds[0]);
}

static void test_clean_eof(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        check(false, "pipe() for the clean-EOF test");
        return;
    }

    close(fds[1]); /* nothing ever written */

    printerctl_daemon_command_t received;
    check(printerctl_daemon_ipc_read_command(fds[0], &received) == PRINTERCTL_DAEMON_IPC_EOF,
          "a pipe closed before any byte arrives reads as a clean EOF");

    close(fds[0]);
}

int main(void)
{
    test_command_round_trip();
    test_event_round_trip_all_types();
    test_unknown_command_type_rejected();
    test_unknown_event_type_and_invalid_result_rejected();
    test_truncated_message();
    test_clean_eof();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All daemon IPC checks passed\n");
    return EXIT_SUCCESS;
}
