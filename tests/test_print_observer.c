#define _GNU_SOURCE
#define main original_test_main
#include "test_printerctld.c"
#undef main
#include <dlfcn.h>
#include <sys/stat.h>

static pid_t observer_daemon = -1;
static pid_t observer_cli = -1;

static void cleanup_observer_processes(void)
{
    /* A failed assertion must not leave a daemon holding ctest's output pipe. */
    if (observer_cli > 0) {
        kill(observer_cli, SIGKILL);
        waitpid(observer_cli, NULL, 0);
    }
    if (observer_daemon > 0) {
        kill(observer_daemon, SIGKILL);
        waitpid(observer_daemon, NULL, 0);
    }
}

static void run_observer_startup_case(const char *shim, uint8_t previous_state)
{
    struct stat socket_stat;
    check(lstat(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, &socket_stat) < 0 && errno == ENOENT,
          "observer test requires an unused socket; isolate /tmp");
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char source_a[128], source_b[128];
    write_gcode_file(source_a, sizeof(source_a), "G1 X10\n");
    write_gcode_file(source_b, sizeof(source_b), "G1 X20\nG1 X21\n");
    int entered[2], release[2];
    check(pipe(entered) == 0 && pipe(release) == 0, "startup gate pipes");
    char fd_text[32], preload[4096];
    void *asan = dlsym(RTLD_DEFAULT, "__asan_init");
    Dl_info info;
    /* Keep the sanitizer runtime first while interposing only the test daemon. */
    if (asan != NULL) {
        check(dladdr(asan, &info) != 0 && strstr(info.dli_fname, ".so") != NULL,
              "preload test requires shared ASan runtime when ASan is enabled");
        check(snprintf(preload, sizeof(preload), "%s:%s", info.dli_fname, shim) <
                  (int)sizeof(preload), "preload path fits");
    } else {
        check(snprintf(preload, sizeof(preload), "%s", shim) < (int)sizeof(preload),
              "preload path fits");
    }
    setenv("LD_PRELOAD", preload, 1);
    setenv("UX001_HOLD_SOURCE", source_b, 1);
    snprintf(fd_text, sizeof(fd_text), "%d", entered[1]);
    setenv("UX001_ENTERED_FD", fd_text, 1);
    snprintf(fd_text, sizeof(fd_text), "%d", release[0]);
    setenv("UX001_RELEASE_FD", fd_text, 1);
    mock_printer_t mock;
    mock_open(&mock);
    observer_daemon = start_printerctld_default_with_port_and_state(mock.slave_path, state_home);
    unsetenv("LD_PRELOAD");
    unsetenv("UX001_HOLD_SOURCE");
    unsetenv("UX001_ENTERED_FD");
    unsetenv("UX001_RELEASE_FD");
    close(connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH));

    start_daemon_print_without_waiting_for_command(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, source_a);
    mock_expect_command(&mock, "G1 X10\n");
    if (previous_state == PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED) {
        int control = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
        send_print_cancel(control, 410);
        check(read_response_frame(control, PRINTERCTL_PLP1_MSG_PRINT_CANCEL, 410).status ==
                  PRINTERCTL_PLP1_SUCCESS, "cancel first job");
        close(control);
    }
    if (previous_state == PRINTERCTL_PLP1_EXECUTION_STATE_FAILED)
        mock_write_text(&mock, "Error: staged first-job failure\n");
    else
        mock_reply_ok(&mock);
    query_print_status_until_state(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, 411, previous_state);

    int out_fd;
    observer_cli = start_printerctl_args("print", source_b, NULL, NULL, &out_fd);
    struct pollfd gate = {.fd = entered[0], .events = POLLIN};
    check(poll(&gate, 1, 3000) > 0, "second job reaches held pre-scan");
    char byte;
    check(read(entered[0], &byte, 1) == 1, "pre-scan gate entered");
    for (int attempt = 0; attempt < 40; ++attempt) {
        expect_child_still_running(observer_cli);
        sleep_ms(10);
    }
    mock_expect_no_command(&mock);
    check(write(release[1], "x", 1) == 1, "release pre-scan");
    mock_expect_command(&mock, "G1 X20\n");
    mock_expect_no_command(&mock);
    expect_child_still_running(observer_cli);
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X21\n");
    mock_reply_ok(&mock);
    expect_child_exit(observer_cli, PRINTERCTL_EXIT_OK);
    observer_cli = -1;
    char output[4096];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "Printing: 0% (0 acknowledged)") != NULL,
          "observer renders the new execution while source count is unknown");
    check(strstr(output, "Completed: 100% (2/2)") != NULL &&
              strstr(output, "Completed: 100% (1/1)") == NULL,
          "observer reports only the second job's genuine completion");
    stop_printerctld(observer_daemon);
    observer_daemon = -1;
    mock_close(&mock);
    close(entered[0]);
    close(entered[1]);
    close(release[0]);
    close(release[1]);
    unlink(source_a);
    unlink(source_b);
    remove_tree(state_home);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
}

int main(int argc, char **argv)
{
    check(argc == 2, "provide preload library path");
    check(atexit(cleanup_observer_processes) == 0, "register child cleanup");
    run_observer_startup_case(argv[1], PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED);
    run_observer_startup_case(argv[1], PRINTERCTL_PLP1_EXECUTION_STATE_FAILED);
    run_observer_startup_case(argv[1], PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED);
    puts("Observer startup regression: three previous terminal states PASS");
    return 0;
}
