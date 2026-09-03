/*
 * Milestone 13 lifecycle, IPC-round-trip, fault and repeated-cycle
 * tests for the daemon runtime (the "control context") and its one
 * print worker thread. Everything here is hardware-independent: no
 * serial, no print_engine, no CLI -- pure runtime/IPC/concurrency
 * validation, per docs/design/milestone-13-daemon-runtime.md.
 *
 * Uses the same `check()` harness and /proc/self/fd descriptor-leak
 * technique as tests/test_printer.c's count_open_fds() /
 * test_failure_paths_release_all_descriptors().
 */
#define _POSIX_C_SOURCE 200809L

#include "printerctl/daemon/runtime.h"
#include "printerctl/protocol/plp1.h"

#include <dirent.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
}

static int count_open_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL) {
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static void test_startup_reaches_running(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;

    check(printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error) == PRINTERCTL_DAEMON_OK,
          "start() succeeds with no worker-init fault injected");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_RUNNING,
          "daemon_state reaches RUNNING only after the startup handshake");
    check(snapshot.worker_state == PRINTERCTL_DAEMON_WORKER_READY,
          "worker_state is READY once the handshake completes");

    check(runtime.cmd_pipe[0] >= 0 && runtime.cmd_pipe[1] >= 0, "cmd_pipe exists");
    check(runtime.event_pipe[0] >= 0 && runtime.event_pipe[1] >= 0, "event_pipe exists");

    check(printerctl_daemon_runtime_shutdown(&runtime, &error) == PRINTERCTL_DAEMON_OK,
          "shutdown() succeeds");
}

static void test_nop_round_trip(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error);

    uint32_t token = 0;
    check(printerctl_daemon_runtime_send_nop(&runtime, &token, &error) == PRINTERCTL_DAEMON_OK,
          "send_nop() succeeds");
    check(token != 0, "send_nop() assigns a non-zero correlation token");

    printerctl_daemon_event_t event;
    check(printerctl_daemon_runtime_wait_event(&runtime, &event, &error) == PRINTERCTL_DAEMON_OK,
          "wait_event() observes the NOP's completion");
    check(event.type == PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED,
          "the observed event is COMMAND_COMPLETED");
    check(event.token == token, "the completion event correlates to the exact token sent");
    check(event.result == PRINTERCTL_DAEMON_RESULT_OK, "the NOP completes successfully");

    printerctl_daemon_runtime_shutdown(&runtime, &error);
}

static void test_normal_shutdown(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error);

    /* shutdown() itself drains STOPPING/STOPPED internally and only
     * returns once pthread_join() has completed -- its return is
     * therefore already proof the worker was joined, not merely asked
     * to stop. */
    check(printerctl_daemon_runtime_shutdown(&runtime, &error) == PRINTERCTL_DAEMON_OK,
          "a normal shutdown succeeds");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_STOPPED,
          "daemon_state ends at STOPPED after a normal shutdown");
    check(snapshot.worker_state == PRINTERCTL_DAEMON_WORKER_STOPPED,
          "worker_state ends at STOPPED (observed via the STOPPED event during the drain)");
}

static bool failing_init_hook(void *context, char *out_message, size_t out_message_size)
{
    (void)context;
    snprintf(out_message, out_message_size, "synthetic worker init failure");
    return false;
}

static void test_worker_init_fault(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;

    printerctl_daemon_status_t status =
        printerctl_daemon_runtime_start(&runtime, failing_init_hook, NULL, &error);

    check(status == PRINTERCTL_DAEMON_ERR_WORKER_FAULT,
          "start() reports ERR_WORKER_FAULT when the worker never reaches READY");
    check(strstr(error.message, "synthetic worker init failure") != NULL,
          "the reported error carries the worker's own fault detail");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_FAILED,
          "daemon_state never reaches RUNNING when the worker faults during startup");

    /* A failed start() already joined the thread and closed every fd
     * (see runtime.c) -- shutdown() must refuse a second cleanup pass. */
    check(printerctl_daemon_runtime_shutdown(&runtime, &error) == PRINTERCTL_DAEMON_ERR_ALREADY,
          "shutdown() after a failed start() is rejected rather than double-cleaning-up");
}

static void test_pipe_creation_failure_leaks_nothing(void)
{
    int before = count_open_fds();
    if (before < 0) {
        return; /* no /proc: nothing to measure */
    }

    struct rlimit original;
    if (getrlimit(RLIMIT_NOFILE, &original) != 0) {
        check(false, "getrlimit() for the pipe-exhaustion fixture");
        return;
    }

    /* Leave room for essentially nothing beyond what's already open, so
     * the very first pipe() call inside start() fails deterministically
     * regardless of exact fd numbering. */
    struct rlimit tight = original;
    tight.rlim_cur = (rlim_t)(before + 1);
    if (setrlimit(RLIMIT_NOFILE, &tight) != 0) {
        check(false, "setrlimit() to exhaust descriptors for the pipe-failure fixture");
        return;
    }

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    printerctl_daemon_status_t status = printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error);

    setrlimit(RLIMIT_NOFILE, &original);

    check(status == PRINTERCTL_DAEMON_ERR_PIPE,
          "a pipe() failure during start() is reported as ERR_PIPE");
    check(runtime.snapshot.daemon_state == PRINTERCTL_DAEMON_FAILED,
          "daemon_state reflects the failed startup");

    int after = count_open_fds();
    check(after == before, "a failed pipe() creation leaks no descriptors");
}

static void test_cmd_pipe_eof_is_orderly(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error);

    /* Simulate control going away without an explicit SHUTDOWN: close
     * control's write end directly (transparent struct, same technique
     * tests already use elsewhere in this project). The worker is
     * guaranteed to be blocked in read(cmd_read_fd) at this point (it
     * already emitted READY and start() already observed it), so this
     * cannot race a write. */
    check(close(runtime.cmd_pipe[1]) == 0, "closing cmd_pipe's write end directly succeeds");
    runtime.cmd_pipe[1] = -1;

    printerctl_daemon_event_t event;
    check(printerctl_daemon_runtime_wait_event(&runtime, &event, &error) == PRINTERCTL_DAEMON_OK &&
              event.type == PRINTERCTL_DAEMON_EVENT_STOPPING,
          "cmd_pipe EOF makes the worker emit STOPPING (orderly, not a fault)");
    check(printerctl_daemon_runtime_wait_event(&runtime, &event, &error) == PRINTERCTL_DAEMON_OK &&
              event.type == PRINTERCTL_DAEMON_EVENT_STOPPED,
          "cmd_pipe EOF is followed by STOPPED");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.worker_state == PRINTERCTL_DAEMON_WORKER_STOPPED,
          "worker_state ends at STOPPED, not FAULTED, on a plain cmd_pipe EOF");

    /* This test bypassed shutdown() on purpose (it owns closing
     * cmd_pipe[1] itself, which shutdown() cannot know about) -- clean
     * up the remaining three fds and join manually. */
    pthread_join(runtime.worker_thread, NULL);
    close(runtime.cmd_pipe[0]);
    close(runtime.event_pipe[0]);
    close(runtime.event_pipe[1]);
}

static void test_event_pipe_failure_is_reported(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error);

    /* Same reasoning as above: the worker is blocked in read(), not
     * mid-write, so closing its write end here is race-free. */
    check(close(runtime.event_pipe[1]) == 0, "closing event_pipe's write end directly succeeds");
    runtime.event_pipe[1] = -1;

    printerctl_daemon_event_t event;
    printerctl_daemon_status_t status = printerctl_daemon_runtime_wait_event(&runtime, &event, &error);
    check(status == PRINTERCTL_DAEMON_ERR_IO,
          "losing the event channel is reported as ERR_IO, not silently ignored");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_FAILED,
          "total loss of the event channel fails the daemon, distinct from a worker FAULT event");

    /* Wake the worker so the test process can join it cleanly: send a
     * real SHUTDOWN directly (control's own send_nop()/shutdown() would
     * try to wait_event() again, which would just fail the same way).
     * The worker's own attempt to emit STOPPING/STOPPED will fail
     * silently (best-effort, event_pipe[1] is gone) but it still
     * returns either way. */
    printerctl_daemon_command_t shutdown_cmd;
    memset(&shutdown_cmd, 0, sizeof(shutdown_cmd));
    shutdown_cmd.wire_size = sizeof(shutdown_cmd);
    shutdown_cmd.type = PRINTERCTL_DAEMON_CMD_SHUTDOWN;
    printerctl_daemon_ipc_write_command(runtime.cmd_pipe[1], &shutdown_cmd);

    pthread_join(runtime.worker_thread, NULL);
    close(runtime.cmd_pipe[0]);
    close(runtime.cmd_pipe[1]);
}

/*
 * SIGALRM watchdog for test_shutdown_after_drained_fault_does_not_deadlock():
 * printerctl_daemon_ipc's read_full() retries EINTR by design, so a
 * plain alarm() would not unblock a regressed shutdown() -- it would
 * just retry the same read(). The handler instead terminates the
 * process outright (_exit() is async-signal-safe), turning a
 * regression into a loud, clearly-labeled test failure instead of a
 * hung test binary/CI job.
 */
static void alarm_handler(int signum)
{
    (void)signum;
    static const char message[] =
        "FAIL: shutdown() after a drained post-READY FAULT did not return "
        "-- deadlock regression\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(EXIT_FAILURE);
}

/*
 * Required review fix: shutdown() must not deadlock when the worker
 * already reached a terminal state (FAULTED here) *before* shutdown()
 * was ever called -- i.e. an earlier wait_event() call already drained
 * the worker's only FAULT event, so nothing is left in event_pipe for
 * a naive "send SHUTDOWN, then wait for one more terminal event" drain
 * loop to read, and the worker thread body has already returned so
 * nothing more will ever be written either.
 */
static void test_shutdown_after_drained_fault_does_not_deadlock(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error) == PRINTERCTL_DAEMON_OK,
          "start() succeeds");

    /* Force a fault after READY: write a malformed command directly to
     * cmd_pipe, bypassing send_nop() (which can only ever send a valid
     * NOP), so the worker's read loop rejects it and emits FAULT before
     * returning. */
    printerctl_daemon_command_t malformed;
    memset(&malformed, 0, sizeof(malformed));
    malformed.wire_size = sizeof(malformed);
    malformed.type = (printerctl_daemon_command_type_t)99; /* out of range on purpose */
    check(printerctl_daemon_ipc_write_command(runtime.cmd_pipe[1], &malformed) ==
              PRINTERCTL_DAEMON_IPC_OK,
          "the malformed command writes successfully");

    /* Observe -- and thereby drain -- the FAULT before shutdown() is
     * ever called, exactly the sequence that used to deadlock. */
    printerctl_daemon_event_t event;
    check(printerctl_daemon_runtime_wait_event(&runtime, &event, &error) == PRINTERCTL_DAEMON_OK &&
              event.type == PRINTERCTL_DAEMON_EVENT_FAULT,
          "the malformed command makes the worker fault, observed here before shutdown()");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.worker_state == PRINTERCTL_DAEMON_WORKER_FAULTED,
          "worker_state is already FAULTED before shutdown() is called");
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_RUNNING,
          "a mid-run worker FAULT leaves daemon_state at RUNNING (the two dimensions stay separate)");

    struct sigaction alarm_action;
    memset(&alarm_action, 0, sizeof(alarm_action));
    alarm_action.sa_handler = alarm_handler;
    sigaction(SIGALRM, &alarm_action, NULL);
    alarm(5);

    check(printerctl_daemon_runtime_shutdown(&runtime, &error) == PRINTERCTL_DAEMON_OK,
          "shutdown() after an already-drained post-READY FAULT returns promptly, without deadlocking");

    alarm(0);
    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigaction(SIGALRM, &default_action, NULL);

    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.daemon_state == PRINTERCTL_DAEMON_STOPPED,
          "daemon_state still reaches STOPPED even though the worker's last observed state was FAULTED");
}

/* PRINT-OBS-001: PRINT_UPDATE's commands_total reaches the published
 * snapshot exactly like the other print-visibility fields already
 * covered above -- a synthetic event injected directly on event_pipe,
 * no worker/print_engine involved. */
static void test_print_update_propagates_commands_total(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    check(printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error) == PRINTERCTL_DAEMON_OK,
          "start() succeeds");

    printerctl_daemon_event_t update;
    memset(&update, 0, sizeof(update));
    update.wire_size = sizeof(update);
    update.type = PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE;
    update.execution_state = PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING;
    update.progress_percent = 47;
    update.commands_sent = 583;
    update.commands_acknowledged = 583;
    update.commands_total = 1240;
    check(printerctl_daemon_ipc_write_event(runtime.event_pipe[1], &update) ==
              PRINTERCTL_DAEMON_IPC_OK,
          "synthetic PRINT_UPDATE write succeeds");

    printerctl_daemon_event_t observed;
    check(printerctl_daemon_runtime_wait_event(&runtime, &observed, &error) ==
              PRINTERCTL_DAEMON_OK,
          "wait_event() observes the synthetic PRINT_UPDATE");

    printerctl_daemon_snapshot_t snapshot;
    printerctl_daemon_runtime_snapshot(&runtime, &snapshot);
    check(snapshot.commands_total == 1240, "commands_total reaches the published snapshot");
    check(snapshot.commands_acknowledged == 583, "commands_acknowledged reaches the snapshot too");

    printerctl_daemon_runtime_shutdown(&runtime, &error);
}

static void test_repeated_shutdown_is_rejected(void)
{
    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t error;
    printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error);

    check(printerctl_daemon_runtime_shutdown(&runtime, &error) == PRINTERCTL_DAEMON_OK,
          "the first shutdown() succeeds");
    check(printerctl_daemon_runtime_shutdown(&runtime, &error) == PRINTERCTL_DAEMON_ERR_ALREADY,
          "a second shutdown() is rejected rather than double-closing/double-joining");
}

/*
 * The primary defense against leaks/deadlocks/races the milestone asks
 * for: repeat the whole create/start/NOP/shutdown cycle many times over
 * fresh runtime instances and assert the descriptor count always
 * returns to baseline.
 */
static void test_repeated_lifecycle_leaks_nothing(void)
{
    int baseline = count_open_fds();
    if (baseline < 0) {
        return; /* no /proc: nothing to measure */
    }

    for (int cycle = 0; cycle < 20; cycle++) {
        printerctl_daemon_runtime_t runtime;
        printerctl_daemon_error_t error;

        if (printerctl_daemon_runtime_start(&runtime, NULL, NULL, &error) != PRINTERCTL_DAEMON_OK) {
            check(false, "start() succeeds on every repeated-lifecycle cycle");
            continue;
        }

        uint32_t token = 0;
        printerctl_daemon_runtime_send_nop(&runtime, &token, &error);
        printerctl_daemon_event_t event;
        printerctl_daemon_runtime_wait_event(&runtime, &event, &error);

        if (printerctl_daemon_runtime_shutdown(&runtime, &error) != PRINTERCTL_DAEMON_OK) {
            check(false, "shutdown() succeeds on every repeated-lifecycle cycle");
            continue;
        }

        int after = count_open_fds();
        if (after != baseline) {
            char description[96];
            snprintf(description, sizeof(description),
                     "cycle %d leaves the descriptor count at baseline (got %d, expected %d)",
                     cycle, after, baseline);
            check(false, description);
        }
    }
}

int main(void)
{
    test_startup_reaches_running();
    test_nop_round_trip();
    test_normal_shutdown();
    test_worker_init_fault();
    test_pipe_creation_failure_leaks_nothing();
    test_cmd_pipe_eof_is_orderly();
    test_event_pipe_failure_is_reported();
    test_print_update_propagates_commands_total();
    test_shutdown_after_drained_fault_does_not_deadlock();
    test_repeated_shutdown_is_rejected();
    test_repeated_lifecycle_leaks_nothing();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All daemon runtime checks passed\n");
    return EXIT_SUCCESS;
}
