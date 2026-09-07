/*
 * M25: deterministic build/dependency check. Confirms the final
 * `printerctl` executable contains no symbols from the legacy direct-
 * hardware modules (serial, printer, marlin, print_engine) it must
 * never own again per docs/design/m25-remove-legacy-serial.md's
 * forbidden-dependency list. This inspects the actual linked binary,
 * not just CMakeLists.txt's target_link_libraries() graph: a static
 * library only contributes to the final binary if something actually
 * references it, so an empty grep here is a real, physical guarantee
 * that no object file from those modules was pulled in -- not merely
 * that nobody wrote a forbidden target_link_libraries() line today.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#ifndef TEST_PRINTERCTL_PATH
#error TEST_PRINTERCTL_PATH must be defined
#endif

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
}

/*
 * Runs `command` and reads its stdout into `buffer` (NUL-terminated,
 * truncated with a trailing marker if it would overflow). Returns the
 * command's exit status via pclose(), or -1 if popen() itself failed.
 */
static int run_capture(const char *command, char *buffer, size_t buffer_size, int *out_truncated)
{
    *out_truncated = 0;
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        buffer[0] = '\0';
        return -1;
    }

    size_t used = 0;
    for (;;) {
        if (used + 1 >= buffer_size) {
            *out_truncated = 1;
            break;
        }
        size_t n = fread(buffer + used, 1, buffer_size - used - 1, pipe);
        if (n == 0) {
            break;
        }
        used += n;
    }
    buffer[used] = '\0';
    return pclose(pipe);
}

int main(void)
{
    static char nm_output[262144];
    char command[512];
    int written = snprintf(command, sizeof(command), "nm '%s' 2>/dev/null", TEST_PRINTERCTL_PATH);
    check(written > 0 && (size_t)written < sizeof(command), "nm command line fits");

    int truncated = 0;
    int status = run_capture(command, nm_output, sizeof(nm_output), &truncated);
    check(!truncated, "nm output fits the capture buffer");
    check(status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "nm runs successfully against the printerctl binary");

    /* Sanity check: an absence-of-forbidden-symbols result below is
     * meaningless if `nm` did not actually read real symbols (e.g. a
     * missing `nm` on PATH, or a stripped binary) -- confirm a
     * known-present symbol shows up first. */
    check(strstr(nm_output, "printerctl_cli_run") != NULL,
          "nm output contains a known-present symbol (printerctl_cli_run), "
          "confirming nm actually read the binary's real symbol table");

    static const char *const forbidden_prefixes[] = {
        "printerctl_serial_",
        "printerctl_printer_",
        "printerctl_marlin_",
        "printerctl_print_engine_",
    };

    for (size_t i = 0; i < sizeof(forbidden_prefixes) / sizeof(forbidden_prefixes[0]); i++) {
        char message[192];
        snprintf(message, sizeof(message),
                 "printerctl binary has no '%s*' symbols (M25: no legacy serial/printer/"
                 "marlin/print_engine ownership)",
                 forbidden_prefixes[i]);
        check(strstr(nm_output, forbidden_prefixes[i]) == NULL, message);
    }

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All build-boundary checks passed\n");
    return EXIT_SUCCESS;
}
