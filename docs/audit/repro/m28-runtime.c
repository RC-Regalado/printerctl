#define main original_test_main
#include "../../../tests/test_daemon_print.c"
#undef main
#include "printerctl/daemon/worker.h"
#include <stdatomic.h>

typedef struct {
    printerctl_daemon_runtime_t *runtime;
    atomic_bool finished;
} shutdown_context_t;

static void *shutdown_thread(void *arg)
{
    shutdown_context_t *context = arg;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_shutdown(context->runtime, &error) == 0,
          "shutdown completes");
    atomic_store(&context->finished, true);
    return NULL;
}

int main(int argc, char **argv)
{
    alarm(10);
    check(argc == 2, "select units, power or shutdown");
    check(strcmp(argv[1], "units") == 0 || strcmp(argv[1], "power") == 0 ||
              strcmp(argv[1], "shutdown") == 0, "known review scenario");
    mock_printer_t mock;
    mock_open(&mock);
    configure_calibration(&mock.config);
    printerctl_daemon_runtime_t runtime;
    start_runtime(&runtime, &mock.config);
    printerctl_daemon_error_t error;
    if (strcmp(argv[1], "units") == 0) {
        uint32_t token;
        check(printerctl_daemon_runtime_send_raw_send(&runtime, "G20", &token, &error) == 0,
              "request inches");
        expect_ack(&mock, "G20\n");
        printerctl_daemon_event_t event;
        check(printerctl_daemon_runtime_wait_command(&runtime, token, &event, &error) == 0,
              "wait inches");
        check(event.wire_status == PRINTERCTL_PLP1_SUCCESS, "inches accepted");
    }
    printerctl_daemon_event_t accepted = send_and_wait_macro(&runtime, "maintenance-z-lift");
    check(accepted.result == PRINTERCTL_DAEMON_RESULT_OK, "macro accepted");
    mock_expect_command(&mock, "G91\n");
    if (strcmp(argv[1], "shutdown") == 0) {
        shutdown_context_t context = {.runtime = &runtime};
        atomic_init(&context.finished, false);
        pthread_t thread;
        check(pthread_create(&thread, NULL, shutdown_thread, &context) == 0,
              "start shutdown");
        for (int i = 0; i < 20; ++i) {
            mock_reply(&mock, "busy: processing\n");
            usleep(50000);
        }
        check(!atomic_load(&context.finished), "shutdown blocked throughout chatter");
        puts("REPRODUCED: shutdown remains blocked until ACK; chatter keeps it pending");
        mock_reply_ok(&mock);
        check(pthread_join(thread, NULL) == 0, "join shutdown");
        mock_expect_no_command(&mock, "shutdown has no cleanup movement");
        mock_close(&mock);
        return 0;
    }
    if (strcmp(argv[1], "power") == 0) {
        printerctl_daemon_worker_request_power_failure_from_signal();
        uint32_t token;
        check(printerctl_daemon_runtime_send_print_cancel_power_failure(&runtime, &token, &error) == 0,
              "request power cancellation as daemon main does");
    }
    mock_reply_ok(&mock);
    expect_ack(&mock, "G1 Z5.000 F300.000\n");
    expect_ack(&mock, "G90\n");
    wait_until_execution(&runtime, PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);
    puts("REPRODUCED: macro moved and completed despite prior G20 or power request");
    printerctl_daemon_runtime_shutdown(&runtime, &error);
    mock_close(&mock);
    return 0;
}
