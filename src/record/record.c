/* POSIX.1-2008 for mkdir(), open() flags, fsync() and gmtime_r(). */
#define _POSIX_C_SOURCE 200809L

#include "printerctl/record.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void set_error(printerctl_record_error_t *out_error,
                       printerctl_record_status_t status,
                       const char *format, ...)
{
    out_error->status = status;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

/* Non-empty absolute paths only, per the XDG Base Directory spec. */
static const char *usable_env_dir(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] != '/') {
        return NULL;
    }
    return value;
}

printerctl_record_status_t printerctl_record_resolve_state_dir(
    char *out_dir,
    size_t out_dir_size,
    printerctl_record_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_RECORD_OK;

    if (out_dir == NULL || out_dir_size < 2) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "out_dir must not be NULL and out_dir_size must be at least 2");
        return out_error->status;
    }

    out_dir[0] = '\0';

    const char *xdg_state = usable_env_dir("XDG_STATE_HOME");
    const char *home = usable_env_dir("HOME");

    int written;
    if (xdg_state != NULL) {
        written = snprintf(out_dir, out_dir_size, "%s/printerctl", xdg_state);
    } else if (home != NULL) {
        written = snprintf(out_dir, out_dir_size, "%s/.local/state/printerctl", home);
    } else {
        set_error(out_error, PRINTERCTL_RECORD_ERR_NO_STATE_DIR,
                   "cannot resolve a state directory: neither XDG_STATE_HOME nor HOME "
                   "is set to an absolute path");
        return out_error->status;
    }

    if (written < 0 || (size_t)written >= out_dir_size) {
        out_dir[0] = '\0';
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "the state directory path exceeds the %zu-byte buffer", out_dir_size);
        return out_error->status;
    }

    return PRINTERCTL_RECORD_OK;
}

/*
 * mkdir -p for an absolute path, mode 0700 (records are private
 * operator data). Existing components — including ones owned by other
 * software, like $HOME/.local — are left untouched.
 */
static bool create_dir_tree(const char *dir, printerctl_record_error_t *out_error)
{
    char partial[PRINTERCTL_RECORD_DIR_MAX];
    size_t length = strlen(dir);
    if (length >= sizeof(partial)) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "the record directory path exceeds %zu bytes", sizeof(partial) - 1);
        return false;
    }

    for (size_t i = 1; i <= length; i++) {
        if (dir[i] != '/' && dir[i] != '\0') {
            continue;
        }
        memcpy(partial, dir, i);
        partial[i] = '\0';
        if (mkdir(partial, 0700) != 0 && errno != EEXIST) {
            set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                       "cannot create directory '%s': %s", partial, strerror(errno));
            return false;
        }
    }

    return true;
}

/* RFC3339 UTC with milliseconds, and a filename-safe compact variant. */
static bool format_timestamps(char *rfc3339, size_t rfc3339_size,
                               char *compact, size_t compact_size)
{
    struct timespec now;
    struct tm utc;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        gmtime_r(&now.tv_sec, &utc) == NULL) {
        return false;
    }

    unsigned milliseconds = (unsigned)(now.tv_nsec / 1000000L);
    int a = snprintf(rfc3339, rfc3339_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                     utc.tm_hour, utc.tm_min, utc.tm_sec, milliseconds);
    int b = snprintf(compact, compact_size, "%04d%02d%02dT%02d%02d%02d.%03uZ",
                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                     utc.tm_hour, utc.tm_min, utc.tm_sec, milliseconds);
    return a > 0 && (size_t)a < rfc3339_size && b > 0 && (size_t)b < compact_size;
}

printerctl_record_status_t printerctl_record_write_session(
    const char *dir,
    const printerctl_session_record_t *record,
    char *out_path,
    size_t out_path_size,
    printerctl_record_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_RECORD_OK;

    if (out_path != NULL && out_path_size > 0) {
        out_path[0] = '\0';
    }

    if (dir == NULL || record == NULL || out_path == NULL || out_path_size < 2) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "dir, record and out_path must not be NULL");
        return out_error->status;
    }

    if (record->state[0] == '\0' || record->cause[0] == '\0') {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "record state and cause must not be empty");
        return out_error->status;
    }

    if (!create_dir_tree(dir, out_error)) {
        return out_error->status;
    }

    char timestamp[40];
    char compact[32];
    if (!format_timestamps(timestamp, sizeof(timestamp), compact, sizeof(compact))) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "cannot read the system clock: %s", strerror(errno));
        return out_error->status;
    }

    /* Exclusive create; on a same-millisecond collision, retry with a
     * numeric suffix rather than overwriting an existing record. */
    char path[PRINTERCTL_RECORD_PATH_MAX];
    int fd = -1;
    for (int attempt = 0; attempt < 10; attempt++) {
        int written;
        if (attempt == 0) {
            written = snprintf(path, sizeof(path), "%s/session-%s.record", dir, compact);
        } else {
            written = snprintf(path, sizeof(path), "%s/session-%s-%d.record",
                               dir, compact, attempt);
        }
        if (written < 0 || (size_t)written >= sizeof(path)) {
            set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                       "the record path exceeds the %zu-byte buffer", sizeof(path) - 1);
            return out_error->status;
        }

        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                       "cannot create record '%s': %s", path, strerror(errno));
            return out_error->status;
        }
    }
    if (fd < 0) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "cannot create a record in '%s': too many name collisions", dir);
        return out_error->status;
    }

    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "cannot open record '%s' for writing: %s", path, strerror(errno));
        close(fd);
        (void)remove(path);
        return out_error->status;
    }

    int printed = fprintf(file,
        "# printerctl interrupted print session record\n"
        "# printerctl never resumes a print automatically (safety policy);\n"
        "# use this record to decide how to restart the job manually.\n"
        "version=1\n"
        "timestamp=%s\n"
        "event=print_interrupted\n"
        "job_id=%s\n"
        "state=%s\n"
        "cause=%s\n"
        "file=%s\n"
        "serial_device=%s\n"
        "commands_sent=%llu\n"
        "commands_acknowledged=%llu\n"
        "last_acknowledged_line=%llu\n"
        "bytes_consumed=%llu\n"
        "file_size=%llu\n"
        "progress_percent=%u\n"
        "printer_state=%s\n",
        timestamp,
        record->job_id,
        record->state,
        record->cause,
        record->file,
        record->serial_device,
        (unsigned long long)record->commands_sent,
        (unsigned long long)record->commands_acknowledged,
        (unsigned long long)record->last_acknowledged_line,
        (unsigned long long)record->bytes_consumed,
        (unsigned long long)record->file_size,
        record->progress_percent,
        record->printer_state);

    /* The record exists to survive exactly the failures it describes:
     * flush it to the kernel and the kernel's copy to the device before
     * declaring it written. */
    bool ok = printed > 0 && fflush(file) == 0 && fsync(fd) == 0;
    if (fclose(file) != 0) {
        ok = false;
    }

    if (!ok) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "cannot write record '%s': %s", path, strerror(errno));
        (void)remove(path);
        return out_error->status;
    }

    snprintf(out_path, out_path_size, "%s", path);
    return PRINTERCTL_RECORD_OK;
}

#define ACTIVE_PID_FILENAME "active-print.pid"

/* Builds <dir>/active-print.pid into `path`; false if it does not fit. */
static bool active_pid_path(const char *dir, char *path, size_t path_size)
{
    int written = snprintf(path, path_size, "%s/" ACTIVE_PID_FILENAME, dir);
    return written > 0 && (size_t)written < path_size;
}

printerctl_record_status_t printerctl_record_write_active_pid(
    const char *dir,
    printerctl_record_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_RECORD_OK;

    if (dir == NULL) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT, "dir must not be NULL");
        return out_error->status;
    }

    if (!create_dir_tree(dir, out_error)) {
        return out_error->status;
    }

    char path[PRINTERCTL_RECORD_PATH_MAX];
    if (!active_pid_path(dir, path, sizeof(path))) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "the active-print marker path exceeds the %zu-byte buffer", sizeof(path) - 1);
        return out_error->status;
    }

    /* Overwritten, not exclusive: a marker left behind by a crashed
     * earlier run must not block the current one from publishing its
     * own PID. printerctl_cli_run() reads it back only through
     * printerctl_record_read_active_pid(), which validates liveness. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "cannot create active-print marker '%s': %s", path, strerror(errno));
        return out_error->status;
    }

    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "cannot open active-print marker '%s' for writing: %s", path, strerror(errno));
        close(fd);
        return out_error->status;
    }

    bool ok = fprintf(file, "%ld\n", (long)getpid()) > 0 && fflush(file) == 0;
    if (fclose(file) != 0) {
        ok = false;
    }

    if (!ok) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "cannot write active-print marker '%s': %s", path, strerror(errno));
        return out_error->status;
    }

    return PRINTERCTL_RECORD_OK;
}

printerctl_record_status_t printerctl_record_read_active_pid(
    const char *dir,
    pid_t *out_pid,
    printerctl_record_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_RECORD_OK;

    if (out_pid != NULL) {
        *out_pid = 0;
    }

    if (dir == NULL || out_pid == NULL) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "dir and out_pid must not be NULL");
        return out_error->status;
    }

    char path[PRINTERCTL_RECORD_PATH_MAX];
    if (!active_pid_path(dir, path, sizeof(path))) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
                   "the active-print marker path exceeds the %zu-byte buffer", sizeof(path) - 1);
        return out_error->status;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            set_error(out_error, PRINTERCTL_RECORD_ERR_NOT_FOUND,
                       "no active print: no marker at '%s'", path);
        } else {
            set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                       "cannot open active-print marker '%s': %s", path, strerror(errno));
        }
        return out_error->status;
    }

    long value = -1;
    int scanned = fscanf(file, "%ld", &value);
    fclose(file);

    if (scanned != 1 || value <= 0) {
        set_error(out_error, PRINTERCTL_RECORD_ERR_IO,
                   "active-print marker '%s' does not contain a valid process id", path);
        return out_error->status;
    }

    *out_pid = (pid_t)value;
    return PRINTERCTL_RECORD_OK;
}

void printerctl_record_remove_active_pid(const char *dir)
{
    if (dir == NULL) {
        return;
    }

    char path[PRINTERCTL_RECORD_PATH_MAX];
    if (!active_pid_path(dir, path, sizeof(path))) {
        return;
    }

    (void)remove(path);
}
