/* POSIX.1-2008 for mkdtemp(), setenv() and stat(). */
#define _POSIX_C_SOURCE 200809L

#include "printerctl/record.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
}

/* Creates a fresh temp directory for record fixtures. */
static bool make_temp_dir(char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/printerctl_record_XXXXXX");
    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp() failed for record fixture\n");
        g_failures++;
        return false;
    }
    return true;
}

/* Reads a whole file into `buffer`; returns false when it cannot be read. */
static bool read_file(const char *path, char *buffer, size_t buffer_size)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    size_t n = fread(buffer, 1, buffer_size - 1, file);
    buffer[n] = '\0';
    fclose(file);
    return true;
}

/* Removes every regular file in `dir`, then the directory itself. */
static void remove_dir_recursive(const char *dir)
{
    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    if (system(command) != 0) {
        fprintf(stderr, "FAIL: cannot remove fixture directory\n");
        g_failures++;
    }
}

static printerctl_session_record_t sample_record(void)
{
    printerctl_session_record_t record;
    memset(&record, 0, sizeof(record));
    snprintf(record.state, sizeof(record.state), "failed");
    snprintf(record.cause, sizeof(record.cause), "timeout");
    snprintf(record.file, sizeof(record.file), "/tmp/job.gcode");
    record.commands_sent = 42;
    record.commands_acknowledged = 41;
    record.last_acknowledged_line = 57;
    record.bytes_consumed = 1234;
    record.file_size = 5678;
    record.progress_percent = 21;
    return record;
}

static void test_resolve_state_dir_prefers_xdg_state_home(void)
{
    printerctl_record_error_t error;
    char dir[PRINTERCTL_RECORD_DIR_MAX];

    setenv("XDG_STATE_HOME", "/tmp/printerctl-xdg-state", 1);
    check(printerctl_record_resolve_state_dir(dir, sizeof(dir), &error) ==
              PRINTERCTL_RECORD_OK,
          "the state directory resolves with XDG_STATE_HOME set");
    check(strcmp(dir, "/tmp/printerctl-xdg-state/printerctl") == 0,
          "XDG_STATE_HOME/printerctl is preferred");

    /* A relative XDG_STATE_HOME is ignored per the XDG spec. */
    setenv("XDG_STATE_HOME", "relative/path", 1);
    setenv("HOME", "/tmp/printerctl-home", 1);
    check(printerctl_record_resolve_state_dir(dir, sizeof(dir), &error) ==
              PRINTERCTL_RECORD_OK,
          "the state directory still resolves with a relative XDG_STATE_HOME");
    check(strcmp(dir, "/tmp/printerctl-home/.local/state/printerctl") == 0,
          "a relative XDG_STATE_HOME falls back to HOME/.local/state/printerctl");

    unsetenv("XDG_STATE_HOME");
    check(printerctl_record_resolve_state_dir(dir, sizeof(dir), &error) ==
              PRINTERCTL_RECORD_OK,
          "the state directory resolves from HOME alone");
    check(strcmp(dir, "/tmp/printerctl-home/.local/state/printerctl") == 0,
          "HOME/.local/state/printerctl is the fallback");

    unsetenv("HOME");
    check(printerctl_record_resolve_state_dir(dir, sizeof(dir), &error) ==
              PRINTERCTL_RECORD_ERR_NO_STATE_DIR,
          "no usable environment reports NO_STATE_DIR");
    check(dir[0] == '\0', "the failed resolution empties the output");
    check(error.message[0] != '\0', "NO_STATE_DIR carries a message");
}

static void test_write_session_creates_dir_and_record(void)
{
    char base[64];
    if (!make_temp_dir(base, sizeof(base))) {
        return;
    }

    /* Two levels below the fixture: the directory tree is created on demand. */
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/state/printerctl", base);

    printerctl_session_record_t record = sample_record();
    printerctl_record_error_t error;
    char path[PRINTERCTL_RECORD_PATH_MAX];

    check(printerctl_record_write_session(dir, &record, path, sizeof(path), &error) ==
              PRINTERCTL_RECORD_OK,
          "a session record writes into a directory that did not exist yet");
    check(strncmp(path, dir, strlen(dir)) == 0, "the returned path is inside the record directory");
    check(strstr(path, "session-") != NULL && strstr(path, ".record") != NULL,
          "the record file name is timestamped");

    struct stat info;
    check(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600,
          "the record file is private (mode 0600)");

    char content[2048];
    check(read_file(path, content, sizeof(content)), "the record file can be read back");
    check(strstr(content, "version=1\n") != NULL, "the record carries a format version");
    check(strstr(content, "event=print_interrupted\n") != NULL, "the record names its event");
    check(strstr(content, "state=failed\n") != NULL, "the record carries the final state");
    check(strstr(content, "cause=timeout\n") != NULL, "the record carries the cause");
    check(strstr(content, "file=/tmp/job.gcode\n") != NULL, "the record carries the G-code file path");
    check(strstr(content, "commands_sent=42\n") != NULL, "the record carries commands_sent");
    check(strstr(content, "commands_acknowledged=41\n") != NULL,
          "the record carries commands_acknowledged");
    check(strstr(content, "last_acknowledged_line=57\n") != NULL,
          "the record carries the last acknowledged line");
    check(strstr(content, "bytes_consumed=1234\n") != NULL, "the record carries bytes_consumed");
    check(strstr(content, "file_size=5678\n") != NULL, "the record carries the file size");
    check(strstr(content, "progress_percent=21\n") != NULL, "the record carries the progress");
    check(strstr(content, "timestamp=") != NULL && strstr(content, "Z\n") != NULL,
          "the record carries a UTC timestamp");
    check(strstr(content, "never resumes") != NULL,
          "the record states the no-automatic-resume policy");

    remove_dir_recursive(base);
}

static void test_write_session_never_overwrites(void)
{
    char dir[64];
    if (!make_temp_dir(dir, sizeof(dir))) {
        return;
    }

    printerctl_session_record_t record = sample_record();
    printerctl_record_error_t error;
    char first[PRINTERCTL_RECORD_PATH_MAX];
    char second[PRINTERCTL_RECORD_PATH_MAX];

    /* Two records in quick succession (likely the same millisecond)
     * must land in two distinct files. */
    check(printerctl_record_write_session(dir, &record, first, sizeof(first), &error) ==
              PRINTERCTL_RECORD_OK,
          "the first record writes");
    check(printerctl_record_write_session(dir, &record, second, sizeof(second), &error) ==
              PRINTERCTL_RECORD_OK,
          "the second record writes");
    check(strcmp(first, second) != 0, "the two records have distinct paths");

    char content[2048];
    check(read_file(first, content, sizeof(content)) &&
              read_file(second, content, sizeof(content)),
          "both record files exist");

    remove_dir_recursive(dir);
}

static void test_write_session_failures(void)
{
    printerctl_session_record_t record = sample_record();
    printerctl_record_error_t error;
    char path[PRINTERCTL_RECORD_PATH_MAX];

    /* An uncreatable directory (a path through a regular file). */
    check(printerctl_record_write_session("/dev/null/printerctl", &record,
                                           path, sizeof(path), &error) ==
              PRINTERCTL_RECORD_ERR_IO,
          "an uncreatable record directory reports an I/O error");
    check(path[0] == '\0', "the failed write empties the output path");
    check(error.message[0] != '\0', "the I/O error carries a message");

    /* Invalid arguments. */
    check(printerctl_record_write_session(NULL, &record, path, sizeof(path), &error) ==
              PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
          "a NULL directory is rejected");
    check(printerctl_record_write_session("/tmp", NULL, path, sizeof(path), &error) ==
              PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
          "a NULL record is rejected");
    check(printerctl_record_write_session("/tmp", &record, NULL, 0, &error) ==
              PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
          "a NULL output path is rejected");

    printerctl_session_record_t empty = sample_record();
    empty.state[0] = '\0';
    check(printerctl_record_write_session("/tmp", &empty, path, sizeof(path), &error) ==
              PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
          "an empty state is rejected");
}

int main(void)
{
    test_resolve_state_dir_prefers_xdg_state_home();
    test_write_session_creates_dir_and_record();
    test_write_session_never_overwrites();
    test_write_session_failures();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All record checks passed\n");
    return EXIT_SUCCESS;
}
