#ifndef PRINTERCTL_RECORD_H
#define PRINTERCTL_RECORD_H

#include <stddef.h>
#include <stdint.h>

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

typedef enum {
    PRINTERCTL_RECORD_OK = 0,
    PRINTERCTL_RECORD_ERR_INVALID_ARGUMENT,
    PRINTERCTL_RECORD_ERR_NO_STATE_DIR,   /* neither XDG_STATE_HOME nor HOME is usable */
    PRINTERCTL_RECORD_ERR_IO
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
typedef struct {
    char state[32];
    char cause[64];
    char file[PRINTERCTL_RECORD_FILE_MAX];
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

#endif
