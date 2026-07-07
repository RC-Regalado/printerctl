#ifndef PRINTERCTL_GCODE_H
#define PRINTERCTL_GCODE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Milestone 3 G-code file reader.
 *
 * Streams one logical command at a time out of a G-code file, without ever
 * opening a serial device: a leading UTF-8 BOM is skipped, full-line and
 * inline `;` comments are stripped, blank (and now-empty, comment-only)
 * lines are skipped, and each returned command is a normalized, trimmed
 * line with source line number and cumulative byte progress attached. A
 * raw source line (excluding its newline) longer than
 * PRINTERCTL_GCODE_LINE_MAX bytes is rejected as
 * PRINTERCTL_GCODE_ERR_LINE_TOO_LONG before any normalization is attempted.
 *
 * The reader is a plain stack value: there is no heap allocation anywhere
 * in this module, including for the normalized command text, which lives
 * in a fixed-size buffer embedded in printerctl_gcode_result_t. Callers own
 * that struct's storage exactly like any other stack value;
 * printerctl_gcode_reader_next() only ever writes into the buffer the
 * caller already provided.
 *
 * Ownership: printerctl_gcode_reader_open() stats `path` first to reject
 * anything that is not a regular file before ever calling fopen(), then
 * opens it and takes ownership of the resulting FILE* for the lifetime of
 * the reader. That FILE* is always owned by the reader, never merely
 * borrowed: from a successful open() onward, the reader (not the caller)
 * is responsible for it, and printerctl_gcode_reader_close() is the only
 * correct way to release it; callers must not fclose() reader->file
 * themselves. `path` itself is only borrowed for the duration of the
 * open() call and is never retained.
 *
 * Immediately after fopen() succeeds, open() calls fstat() on the open
 * file descriptor and records its identity (device, inode), confirms it is
 * still a regular file, and snapshots its size and modification time; this
 * closes the race between the earlier path-based stat() and the fopen()
 * call. That snapshot is re-checked via fstat() before every subsequent
 * line read so a file replaced or rewritten out from under the reader (by
 * another process, e.g. a slicer re-exporting mid-print) is caught as
 * PRINTERCTL_GCODE_ERR_FILE_CHANGED instead of silently streaming
 * inconsistent or foreign data.
 *
 * Terminal state: once printerctl_gcode_reader_next() returns
 * PRINTERCTL_GCODE_EOF or any PRINTERCTL_GCODE_ERR_* status, the reader
 * saves that status as its terminal status and has reached a terminal
 * state: every subsequent call to next() returns the same terminal status
 * again immediately, without reading further from the file. This makes it
 * safe for a caller to keep calling next() in a loop until it stops
 * returning PRINTERCTL_GCODE_OK, without special-casing the first non-OK
 * result. The only way out of a terminal state is
 * printerctl_gcode_reader_close().
 *
 * Idempotent close: printerctl_gcode_reader_close() may be called on a
 * reader that is already closed, or that was never successfully opened,
 * and is a no-op returning PRINTERCTL_GCODE_OK in both cases. Calling it
 * again after a successful close is always safe.
 */

/*
 * Maximum number of content bytes accepted for a single source line,
 * excluding its newline. A raw line longer than this is rejected as
 * PRINTERCTL_GCODE_ERR_LINE_TOO_LONG before any normalization is
 * attempted.
 */
#define PRINTERCTL_GCODE_LINE_MAX 4096

/*
 * Size, in bytes, of the fixed buffer holding one normalized command, i.e.
 * PRINTERCTL_GCODE_LINE_MAX plus one byte for the terminating '\0'. This is
 * also the size of the internal raw-line buffer used while reading, since
 * stripping comments and trailing whitespace can only shrink a line, never
 * grow it.
 */
#define PRINTERCTL_GCODE_COMMAND_MAX (PRINTERCTL_GCODE_LINE_MAX + 1)

#define PRINTERCTL_GCODE_ERROR_MAX 256

typedef enum {
    PRINTERCTL_GCODE_OK = 0,
    PRINTERCTL_GCODE_EOF,
    PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT,
    PRINTERCTL_GCODE_ERR_NOT_OPEN,
    PRINTERCTL_GCODE_ERR_OPEN_FAILED,
    PRINTERCTL_GCODE_ERR_NOT_REGULAR,
    PRINTERCTL_GCODE_ERR_IO,
    PRINTERCTL_GCODE_ERR_LINE_TOO_LONG,
    PRINTERCTL_GCODE_ERR_MALFORMED,
    PRINTERCTL_GCODE_ERR_UNSAFE_G28,
    PRINTERCTL_GCODE_ERR_FILE_CHANGED
} printerctl_gcode_status_t;

/*
 * Stack-only reader state: one owned FILE*, the open-time safety flag,
 * progress counters and `terminal_status`, which makes next() safe to call
 * repeatedly past EOF or an error by replaying the same status instead of
 * touching the file again. `terminal_status` is PRINTERCTL_GCODE_OK while
 * the reader is still readable and is set exactly once, to the EOF or
 * ERR_* status returned by next(), when the reader becomes terminal.
 *
 * `device` and `inode` identify the specific file that was open()ed (from
 * fstat()'s st_dev/st_ino), `size` and `mtime_sec`/`mtime_nsec` are its
 * size and modification time at that moment. All five are re-read via
 * fstat() before every line read and compared against this snapshot so a
 * file swapped or rewritten underneath the reader is caught as
 * PRINTERCTL_GCODE_ERR_FILE_CHANGED.
 */
typedef struct {
    FILE *file;
    bool open;
    bool allow_g28;
    printerctl_gcode_status_t terminal_status;

    uint64_t device;
    uint64_t inode;
    uint64_t size;
    uint64_t mtime_sec;
    uint64_t mtime_nsec;

    uint64_t bytes_consumed;
    uint64_t line_number;    /* source line number of the last line read; 0 before the first read */
    uint64_t command_count;  /* number of commands successfully returned so far */
} printerctl_gcode_reader_t;

/*
 * One normalized command plus the progress snapshot at the moment it was
 * read. `command` is always NUL-terminated and holds neither a trailing
 * comment, trailing whitespace nor the line's newline. `file_size` is the
 * size in bytes captured at open() time; `bytes_consumed` is the
 * cumulative count of source bytes read through the end of this command's
 * line, so bytes_consumed / (double)file_size is a monotonic progress
 * fraction and `progress_percent` is that same fraction expressed as a
 * whole percentage in [0, 100]. `command_number` is the 1-based ordinal of
 * this command among all commands returned so far by this reader.
 */
typedef struct {
    char command[PRINTERCTL_GCODE_COMMAND_MAX];
    uint64_t line_number;
    uint64_t command_number;
    uint64_t bytes_consumed;
    uint64_t file_size;
    unsigned progress_percent;
} printerctl_gcode_result_t;

/* `line_number` is 0 when the error is not tied to a specific source line. */
typedef struct {
    printerctl_gcode_status_t status;
    uint64_t line_number;
    char message[PRINTERCTL_GCODE_ERROR_MAX];
} printerctl_gcode_error_t;

/*
 * Opens `path` and initializes *reader to stream commands from it, with
 * `allow_g28` recorded for later use by printerctl_gcode_reader_next().
 * Always overwrites the full contents of *reader, on both success and
 * failure, so callers never need to zero it first.
 *
 * `reader` and `path` must be non-NULL and `out_error` must be non-NULL
 * (returns PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT otherwise; *reader is left
 * untouched if reader itself is NULL, otherwise reset to a well-defined,
 * not-open state). `path` is stat()ed before it is opened, and the result
 * is opened and then fstat()ed again on the live file descriptor to record
 * its device/inode identity, confirm it is still a regular file, and
 * snapshot its size and modification time for later PRINTERCTL_GCODE_ERR_
 * FILE_CHANGED checks. Returns PRINTERCTL_GCODE_ERR_OPEN_FAILED if `path`
 * cannot be stat()ed or opened for reading, or
 * PRINTERCTL_GCODE_ERR_NOT_REGULAR if either stat() call reports something
 * other than a regular file (e.g. a directory, device or FIFO); in either
 * case *out_error is filled and *reader is left not-open. On success,
 * returns PRINTERCTL_GCODE_OK with *reader open, its counters zeroed,
 * terminal_status set to PRINTERCTL_GCODE_OK, and ready for
 * printerctl_gcode_reader_next().
 */
printerctl_gcode_status_t printerctl_gcode_reader_open(
    printerctl_gcode_reader_t *reader,
    const char *path,
    bool allow_g28,
    printerctl_gcode_error_t *out_error);

/*
 * Reads and returns the next normalized command into *result, skipping a
 * leading UTF-8 BOM, blank lines, full-line comments and now-empty
 * comment-only lines, and stripping trailing inline comments and
 * whitespace from the rest.
 *
 * `reader`, `result` and `out_error` must be non-NULL
 * (PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT otherwise). Returns
 * PRINTERCTL_GCODE_ERR_NOT_OPEN if the reader was never opened or has
 * already been closed. If the reader is already terminal (a previous call
 * returned PRINTERCTL_GCODE_EOF or a PRINTERCTL_GCODE_ERR_* status), that
 * same terminal_status is returned again immediately, with *result left
 * unspecified and no further reading of the file. Returns
 * PRINTERCTL_GCODE_EOF once the file is fully consumed, with *result left
 * unspecified.
 *
 * Before each line is read, the file's current device/inode identity,
 * regular-file-ness and size (from fstat()) are compared against the
 * snapshot captured at open() time; any mismatch fails with
 * PRINTERCTL_GCODE_ERR_FILE_CHANGED. Fails with
 * PRINTERCTL_GCODE_ERR_LINE_TOO_LONG if a line's content (excluding its
 * newline) exceeds PRINTERCTL_GCODE_LINE_MAX bytes,
 * PRINTERCTL_GCODE_ERR_MALFORMED for a line that cannot be normalized into
 * a valid command, PRINTERCTL_GCODE_ERR_UNSAFE_G28 for a `G28` command
 * when the reader was opened with allow_g28 = false, or
 * PRINTERCTL_GCODE_ERR_IO on a read error from the underlying file. In
 * every failure case *out_error is filled (with line_number set when the
 * failure is tied to a specific source line), the status is saved as the
 * reader's terminal_status, and the reader becomes terminal: every
 * subsequent call returns that same status again without reading further,
 * until printerctl_gcode_reader_close() is called.
 *
 * On success, returns PRINTERCTL_GCODE_OK, fills *result (including
 * progress_percent, derived from bytes_consumed and the size snapshotted
 * at open() time) and advances the reader's counters (line_number,
 * command_count, bytes_consumed).
 */
printerctl_gcode_status_t printerctl_gcode_reader_next(
    printerctl_gcode_reader_t *reader,
    printerctl_gcode_result_t *result,
    printerctl_gcode_error_t *out_error);

/*
 * Closes the reader's underlying FILE* (fclose) and marks *reader
 * not-open. Idempotent: calling this again on an already-closed (or
 * never-opened) reader is a no-op that returns PRINTERCTL_GCODE_OK
 * without touching any file. Returns PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT
 * for a NULL reader or a NULL out_error, or PRINTERCTL_GCODE_ERR_IO if
 * fclose() fails; either way *reader is left marked not-open, so a failed
 * close can never be retried into a double fclose().
 */
printerctl_gcode_status_t printerctl_gcode_reader_close(
    printerctl_gcode_reader_t *reader,
    printerctl_gcode_error_t *out_error);

#endif
