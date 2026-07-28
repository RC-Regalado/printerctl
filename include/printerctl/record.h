#ifndef PRINTERCTL_RECORD_H
#define PRINTERCTL_RECORD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* pid_t */

/*
 * Milestone 9 session records: durable, operator-facing metadata about
 * a print session that did not complete (failed or cancelled), so the
 * operator can decide how to restart the job manually. Milestone 10's
 * power-event records reuse this module.
 *
 * This module is deliberately write-only in the MVP: nothing in
 * printerctl ever reads a record back to resume a print. Automatic
 * resume is forbidden by the Agents.md safety policy, and every record
 * says so in its header comment. Records are data *for the operator*,
 * not program state.
 *
 * Records live in the state directory: $XDG_STATE_HOME/printerctl when
 * XDG_STATE_HOME is set to an absolute path, otherwise
 * $HOME/.local/state/printerctl (per the XDG Base Directory spec's
 * default). One file per interrupted session, named
 * session-<UTC timestamp>.record, created exclusively (a name collision
 * retries with a numeric suffix). The format is the same root-level
 * key=value with full-line '#' comments the config file uses.
 *
 * Unlike the structured log stream, a record intentionally contains the
 * G-code file path — it is a private file (0600, in the user's own
 * state directory), and a recovery record without the file name would
 * be useless. The Milestone 2 privacy rules still apply to the log:
 * callers must not log the record's path or contents.
 *
 * Same discipline as every other module: no heap, stack-value structs,
 * output parameters fully initialized on success and failure, explicit
 * statuses, no dependency on any other printerctl module.
 */

#define PRINTERCTL_RECORD_ERROR_MAX 256
#define PRINTERCTL_RECORD_DIR_MAX 512
#define PRINTERCTL_RECORD_PATH_MAX 640
#define PRINTERCTL_RECORD_FILE_MAX 1024
#define PRINTERCTL_RECORD_JOB_ID_MAX 32
#define PRINTERCTL_RECORD_DEVICE_MAX 256
#define PRINTERCTL_RECORD_PRINTER_STATE_MAX 32

typedef enum {
    PRINTERCTL_RECORD_OK = 0,
    PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
    PRINTERCTL_RECORD_ERR_NO_STATE_DIR,   /* neither XDG_STATE_HOME nor HOME is usable */
    PRINTERCTL_RECORD_ERR_IO,
    PRINTERCTL_RECORD_ERR_NOT_FOUND       /* Milestone 10: no active-print marker present
                                            * (the expected, non-error outcome when no
                                            * print is currently streaming) */
} printerctl_record_status_t;

typedef struct {
    printerctl_record_status_t status;
    char message[PRINTERCTL_RECORD_ERROR_MAX];
} printerctl_record_error_t;

/*
 * One interrupted print session. `state` is the final state name
 * ("failed" or "cancelled") and `cause` the status name of what stopped
 * the print (e.g. "timeout", "serial", "printer_error", "cancelled");
 * both must be non-empty. `file` is the G-code file path as the
 * operator passed it. The counters mirror the print summary.
 */
/*
 * `job_id`, `serial_device` and `printer_state` were added in Milestone
 * 10. There is no job queue yet (that is Phase II's Milestone 13), so
 * `job_id` is a placeholder scoped to this pre-daemon, one-print-at-a-
 * time architecture: callers set it to "job-<pid>" of the streaming
 * process, which is a stable, locally unique handle for exactly as long
 * as printerctl has any queue at all. `serial_device` is the configured
 * port, included for the same reason `file` is: this is private
 * operator data (0600), not the structured log (see record.c's header
 * comment and architecture.md §6.9). `printer_state` is not populated
 * from a live query -- sending one more command to ask the printer
 * anything, right when a fault or a power event just stopped the
 * stream, is exactly the kind of extra interaction the safety policy
 * avoids -- so it is always "unknown" in this MVP.
 */
typedef struct {
    char state[32];
    char cause[64];
    char file[PRINTERCTL_RECORD_FILE_MAX];
    char job_id[PRINTERCTL_RECORD_JOB_ID_MAX];
    char serial_device[PRINTERCTL_RECORD_DEVICE_MAX];
    char printer_state[PRINTERCTL_RECORD_PRINTER_STATE_MAX];
    uint64_t commands_sent;
    uint64_t commands_acknowledged;
    uint64_t last_acknowledged_line;
    uint64_t bytes_consumed;
    uint64_t file_size;
    unsigned progress_percent;
} printerctl_session_record_t;

/*
 * Resolves the record directory into `out_dir` (always NUL-terminated;
 * emptied on failure): $XDG_STATE_HOME/printerctl when XDG_STATE_HOME
 * is a non-empty absolute path, else $HOME/.local/state/printerctl when
 * HOME is a non-empty absolute path, else
 * PRINTERCTL_RECORD_ERR_NO_STATE_DIR. The directory is not created
 * here; printerctl_record_write_session() creates it on demand.
 */
printerctl_record_status_t printerctl_record_resolve_state_dir(
    char *out_dir,
    size_t out_dir_size,
    printerctl_record_error_t *out_error);

/*
 * Writes one session record into `dir`, creating the directory (and
 * missing parents, mode 0700) on demand. The record file (mode 0600) is
 * created exclusively and fsync'd before close — a recovery record that
 * does not survive a power failure would miss its own point. On success
 * `out_path` (always NUL-terminated; emptied on failure) names the
 * file written, so the caller can show the operator where it is.
 *
 * `dir`, `record`, `out_path` and `out_error` must be non-NULL, and
 * record->state / record->cause non-empty
 * (PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT otherwise). Any create/write/
 * sync failure is PRINTERCTL_RECORD_ERR_IO with the reason in the error
 * message; a partially written file is removed.
 */
printerctl_record_status_t printerctl_record_write_session(
    const char *dir,
    const printerctl_session_record_t *record,
    char *out_path,
    size_t out_path_size,
    printerctl_record_error_t *out_error);

/*
 * Milestone 10 addition: a live marker naming the process ID of the
 * printerctl invocation currently streaming a print, at
 * <dir>/active-print.pid (mode 0600, a decimal PID followed by a
 * newline, silently overwritten if one is already there). This is NOT
 * an interrupted-session record -- it exists only while a print
 * streams and is removed when the stream ends, on every outcome. It is
 * the "temporary internal interface" the roadmap allows ahead of
 * Milestone 17's daemon and Unix socket: an external notifier (the
 * APCUPSD low-battery script, through `printerctl power low-battery`)
 * reads it to find the right process to signal (SIGPWR) without
 * touching the serial port or any other internal file itself.
 *
 * Creates the directory (mode 0700) on demand, exactly like
 * printerctl_record_write_session(). `dir` and `out_error` must be
 * non-NULL.
 */
printerctl_record_status_t printerctl_record_write_active_pid(
    const char *dir,
    printerctl_record_error_t *out_error);

/*
 * Reads back the marker written by printerctl_record_write_active_pid().
 * Returns PRINTERCTL_RECORD_ERR_NOT_FOUND when no marker file exists --
 * the expected, non-error outcome when no print is currently streaming
 * -- or PRINTERCTL_RECORD_ERR_IO when it exists but cannot be read or
 * does not contain a valid process id. `dir`, `out_pid` and `out_error`
 * must be non-NULL; `*out_pid` is 0 on any non-OK return.
 */
printerctl_record_status_t printerctl_record_read_active_pid(
    const char *dir,
    pid_t *out_pid,
    printerctl_record_error_t *out_error);

/*
 * Removes the active-print marker. Best-effort and idempotent: a
 * missing marker, or a directory that itself does not exist, is not an
 * error. `dir` may be NULL, in which case this is a no-op.
 */
void printerctl_record_remove_active_pid(const char *dir);

#endif
