#define main original_test_main
#include "../../../tests/test_printerctld.c"
#undef main

int main(int argc, char **argv)
{
    check(argc == 2, "provide preload library path");
    alarm(15);
    char state_home[128];
    make_temp_state_home(state_home, sizeof(state_home));
    char source_a[128], source_b[128];
    write_gcode_file(source_a, sizeof(source_a), "G1 X10\n");
    write_gcode_file(source_b, sizeof(source_b), "G1 X20\nG1 X21\n");
    int entered[2], release[2];
    check(pipe(entered) == 0 && pipe(release) == 0, "startup gate pipes");
    char fd_text[32];
    setenv("LD_PRELOAD", argv[1], 1);
    setenv("UX001_HOLD_SOURCE", source_b, 1);
    snprintf(fd_text, sizeof(fd_text), "%d", entered[1]);
    setenv("UX001_ENTERED_FD", fd_text, 1);
    snprintf(fd_text, sizeof(fd_text), "%d", release[0]);
    setenv("UX001_RELEASE_FD", fd_text, 1);
    mock_printer_t mock;
    mock_open(&mock);
    pid_t daemon = start_printerctld_default_with_port_and_state(mock.slave_path, state_home);
    unsetenv("LD_PRELOAD");
    unsetenv("UX001_HOLD_SOURCE");
    unsetenv("UX001_ENTERED_FD");
    unsetenv("UX001_RELEASE_FD");
    int ready = connect_with_retry(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    close(ready);

    int out_fd;
    pid_t cli_a = start_printerctl_args("print", source_a, NULL, NULL, &out_fd);
    mock_expect_command(&mock, "G1 X10\n");
    mock_reply_ok(&mock);
    expect_child_exit(cli_a, PRINTERCTL_EXIT_OK);
    char output[2048];
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "Completed: 100% (1/1)") != NULL, "A completed");
    printf("Job A: completed with 1/1 acknowledged.\n");

    pid_t cli_b = start_printerctl_args("print", source_b, NULL, NULL, &out_fd);
    struct pollfd gate = {.fd = entered[0], .events = POLLIN};
    check(poll(&gate, 1, 3000) > 0, "B is held inside pre-scan after acceptance");
    char byte;
    check(read(entered[0], &byte, 1) == 1, "pre-scan gate entered");
    int status = 0;
    pid_t result = 0;
    for (int attempt = 0; attempt < 100 && result == 0; ++attempt) {
        result = waitpid(cli_b, &status, WNOHANG);
        check(result >= 0, "check B observer lifetime");
        if (result == 0) sleep_ms(10);
    }
    check(result == cli_b && WIFEXITED(status) && WEXITSTATUS(status) == PRINTERCTL_EXIT_OK,
          "reproduce: B observer falsely exits successfully before B transmits");
    read_output(out_fd, output, sizeof(output));
    check(strstr(output, "print accepted by printerctld") != NULL &&
              strstr(output, "Completed: 100% (1/1)") != NULL,
          "B reports A's completed counters");
    mock_expect_no_command(&mock);
    printf("Job B observer output while pre-scan is held:\n%s", output);
    printf("REPRODUCED: CLI B exited 0; B has transmitted zero commands.\n");

    check(write(release[1], "x", 1) == 1, "release pre-scan");
    mock_expect_command(&mock, "G1 X20\n");
    printerctl_plp1_status_response_t running =
        query_print_status(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, 410);
    check(running.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING &&
              running.commands_acknowledged == 0 && running.commands_total == 2,
          "B really runs only after its observer has exited");
    mock_expect_no_command(&mock);
    mock_reply_ok(&mock);
    mock_expect_command(&mock, "G1 X21\n");
    mock_reply_ok(&mock);
    printerctl_plp1_status_response_t completed =
        query_print_status_at_least(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH, 411, 2);
    check(completed.execution_state == PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED,
          "B eventually completes independently");
    puts("Job B: later RUNNING 0/2, then genuinely COMPLETED 2/2.");
    stop_printerctld(daemon);
    mock_close(&mock);
    close(entered[0]);
    close(entered[1]);
    close(release[0]);
    close(release[1]);
    unlink(source_a);
    unlink(source_b);
    remove_tree(state_home);
    unlink(PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH);
    return 0;
}
