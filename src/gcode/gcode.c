#define _POSIX_C_SOURCE 200809L

#include "printerctl/gcode.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

static void set_error(printerctl_gcode_error_t *out_error,
                       printerctl_gcode_status_t status,
                       uint64_t line_number,
                       const char *format, ...)
{
    out_error->status = status;
    out_error->line_number = line_number;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static void reset_reader(printerctl_gcode_reader_t *reader)
{
    memset(reader, 0, sizeof(*reader));
    reader->terminal_status = PRINTERCTL_GCODE_OK;
}

static bool stat_is_regular(const struct stat *st)
{
    return S_ISREG(st->st_mode);
}

/*
 * Records `status` and a formatted message into *out_error and, whenever
 * `reader` is non-NULL, saves `status` as the reader's terminal_status so
 * every later call to next() replays it without touching the file again.
 * Used for every failure path in next(), including EOF, which is terminal
 * exactly like an ERR_* status.
 */
static printerctl_gcode_status_t fail(printerctl_gcode_reader_t *reader,
                                        printerctl_gcode_error_t *out_error,
                                        printerctl_gcode_status_t status,
                                        uint64_t line_number,
                                        const char *format, ...)
{
    out_error->status = status;
    out_error->line_number = line_number;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);

    if (reader != NULL) {
        reader->terminal_status = status;
    }

    return status;
}

/* Re-fstat()s the reader's open file and compares identity/size/mtime against the open-time snapshot. */
static printerctl_gcode_status_t check_file_unchanged(const printerctl_gcode_reader_t *reader)
{
    int fd = fileno(reader->file);
    if (fd == -1) {
        return PRINTERCTL_GCODE_ERR_IO;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        return PRINTERCTL_GCODE_ERR_IO;
    }

    if (!stat_is_regular(&st)) {
        return PRINTERCTL_GCODE_ERR_FILE_CHANGED;
    }

    if ((uint64_t)st.st_dev != reader->device ||
        (uint64_t)st.st_ino != reader->inode ||
        (uint64_t)st.st_size != reader->size ||
        (uint64_t)st.st_mtim.tv_sec != reader->mtime_sec ||
        (uint64_t)st.st_mtim.tv_nsec != reader->mtime_nsec) {
        return PRINTERCTL_GCODE_ERR_FILE_CHANGED;
    }

    return PRINTERCTL_GCODE_OK;
}

/* Whole-percent progress, floored, with an empty file (`total` == 0) always reporting 100. */
static unsigned compute_progress(uint64_t consumed, uint64_t total)
{
    if (total == 0) {
        return 100;
    }

    double percent = floor((double)consumed / (double)total * 100.0);
    if (percent < 0.0) {
        percent = 0.0;
    } else if (percent > 100.0) {
        percent = 100.0;
    }

    return (unsigned)percent;
}

/* Strips a UTF-8 BOM (EF BB BF) if it occupies the first three bytes of `buffer`. */
static void strip_leading_bom(char *buffer, size_t *length)
{
    if (*length >= 3 &&
        (unsigned char)buffer[0] == 0xEF &&
        (unsigned char)buffer[1] == 0xBB &&
        (unsigned char)buffer[2] == 0xBF) {
        memmove(buffer, buffer + 3, *length - 3);
        *length -= 3;
    }
}

/* True if the UTF-8 BOM byte sequence (EF BB BF) occurs anywhere in `buffer`. */
static bool contains_bom_sequence(const char *buffer, size_t length)
{
    if (length < 3) {
        return false;
    }

    for (size_t i = 0; i + 3 <= length; i++) {
        if ((unsigned char)buffer[i] == 0xEF &&
            (unsigned char)buffer[i + 1] == 0xBB &&
            (unsigned char)buffer[i + 2] == 0xBF) {
            return true;
        }
    }

    return false;
}

/* Trims leading and trailing spaces/tabs from `buffer` in place and NUL-terminates it. */
static void trim_spaces_tabs(char *buffer, size_t *length)
{
    size_t len = *length;

    size_t start = 0;
    while (start < len && (buffer[start] == ' ' || buffer[start] == '\t')) {
        start++;
    }

    size_t end = len;
    while (end > start && (buffer[end - 1] == ' ' || buffer[end - 1] == '\t')) {
        end--;
    }

    size_t trimmed = end - start;
    if (start > 0 && trimmed > 0) {
        memmove(buffer, buffer + start, trimmed);
    }
    buffer[trimmed] = '\0';
    *length = trimmed;
}

/* Parses a run of decimal digits at *pos, advancing *pos past them; false if there are none. */
static bool parse_leading_digits(const char *s, size_t len, size_t *pos, unsigned long *out_value)
{
    bool any = false;
    unsigned long value = 0;

    while (*pos < len && isdigit((unsigned char)s[*pos])) {
        value = value * 10 + (unsigned long)(s[*pos] - '0');
        (*pos)++;
        any = true;
    }

    if (!any) {
        return false;
    }

    *out_value = value;
    return true;
}

/*
 * True if `command` starts with an initial G28, i.e. an optional `N<digits>`
 * word and whitespace, then `G`/`g`, optional spaces, then decimal digits
 * (leading zeros allowed) whose value is 28. Any suffix at all after that
 * numeric value, including a subcode (`.1`) or parameters, still counts.
 */
static bool is_unsafe_g28(const char *command, size_t length)
{
    size_t pos = 0;

    if (pos < length && (command[pos] == 'N' || command[pos] == 'n')) {
        pos++;

        unsigned long line_word;
        if (!parse_leading_digits(command, length, &pos, &line_word)) {
            return false;
        }

        size_t ws_start = pos;
        while (pos < length && (command[pos] == ' ' || command[pos] == '\t')) {
            pos++;
        }
        if (pos == ws_start) {
            return false;
        }
    }

    if (pos >= length || (command[pos] != 'G' && command[pos] != 'g')) {
        return false;
    }
    pos++;

    while (pos < length && (command[pos] == ' ' || command[pos] == '\t')) {
        pos++;
    }

    unsigned long code;
    if (!parse_leading_digits(command, length, &pos, &code)) {
        return false;
    }

    return code == 28;
}

/*
 * Copies `raw` into `out`, dropping a `;` line comment (outside parentheses)
 * and any `(...)` comment, which may nest up to 8 levels deep. Fails with
 * PRINTERCTL_GCODE_ERR_MALFORMED on nesting beyond that depth, a `)` with no
 * matching `(`, or an unterminated `(` still open at end of line.
 */
static printerctl_gcode_status_t strip_comments(printerctl_gcode_reader_t *reader,
                                                  printerctl_gcode_error_t *out_error,
                                                  uint64_t line_number,
                                                  const char *raw,
                                                  size_t raw_length,
                                                  char *out,
                                                  size_t *out_length)
{
    size_t depth = 0;
    size_t len = 0;

    for (size_t i = 0; i < raw_length; i++) {
        char c = raw[i];

        if (c == ';' && depth == 0) {
            break;
        }

        if (c == '(') {
            depth++;
            if (depth > 8) {
                return fail(reader, out_error, PRINTERCTL_GCODE_ERR_MALFORMED, line_number,
                             "comment nested more than 8 levels deep");
            }
            continue;
        }

        if (c == ')') {
            if (depth == 0) {
                return fail(reader, out_error, PRINTERCTL_GCODE_ERR_MALFORMED, line_number,
                             "unmatched ')' with no matching '('");
            }
            depth--;
            continue;
        }

        if (depth == 0) {
            out[len++] = c;
        }
    }

    if (depth != 0) {
        return fail(reader, out_error, PRINTERCTL_GCODE_ERR_MALFORMED, line_number,
                     "unterminated '(' comment");
    }

    *out_length = len;
    return PRINTERCTL_GCODE_OK;
}

/*
 * Reads one raw source line via fgetc(), byte by byte, into `buffer` (whose
 * capacity is PRINTERCTL_GCODE_LINE_MAX), stopping at a bare `\n` or a `\r\n`
 * pair, neither of which is included in `buffer`. A `\r` not immediately
 * followed by `\n` fails as PRINTERCTL_GCODE_ERR_MALFORMED, as does any NUL
 * or other control byte besides tab. A final line with no trailing newline
 * at all is accepted. Content beyond PRINTERCTL_GCODE_LINE_MAX bytes is
 * drained (read and discarded, to keep the file position and byte count
 * correct) rather than stored, and fails as
 * PRINTERCTL_GCODE_ERR_LINE_TOO_LONG once the line's end is reached.
 * `*out_bytes_read` is always set to the number of bytes physically
 * consumed from the file for this line, including any newline; a value of 0
 * means the file had nothing left to read (true end of file).
 */
static printerctl_gcode_status_t read_raw_line(printerctl_gcode_reader_t *reader,
                                                 printerctl_gcode_error_t *out_error,
                                                 uint64_t line_number,
                                                 char *buffer,
                                                 size_t *out_length,
                                                 uint64_t *out_bytes_read)
{
    size_t length = 0;
    uint64_t bytes_read = 0;
    bool draining = false;

    for (;;) {
        int c = fgetc(reader->file);
        if (c == EOF) {
            if (ferror(reader->file)) {
                *out_bytes_read = bytes_read;
                return fail(reader, out_error, PRINTERCTL_GCODE_ERR_IO, line_number,
                             "error reading file: %s", strerror(errno));
            }
            break;
        }
        bytes_read++;

        if (c == '\n') {
            break;
        }

        if (draining) {
            continue;
        }

        if (c == '\r') {
            int c2 = fgetc(reader->file);
            if (c2 == EOF) {
                if (ferror(reader->file)) {
                    *out_bytes_read = bytes_read;
                    return fail(reader, out_error, PRINTERCTL_GCODE_ERR_IO, line_number,
                                 "error reading file: %s", strerror(errno));
                }
                *out_bytes_read = bytes_read;
                return fail(reader, out_error, PRINTERCTL_GCODE_ERR_MALFORMED, line_number,
                             "line ends with a stray carriage return");
            }
            bytes_read++;
            if (c2 == '\n') {
                break;
            }
            *out_bytes_read = bytes_read;
            return fail(reader, out_error, PRINTERCTL_GCODE_ERR_MALFORMED, line_number,
                         "carriage return not immediately followed by a line feed");
        }

        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            *out_bytes_read = bytes_read;
            return fail(reader, out_error, PRINTERCTL_GCODE_ERR_MALFORMED, line_number,
                         "control character 0x%02X is not allowed", (unsigned)c);
        }

        if (length < PRINTERCTL_GCODE_LINE_MAX) {
            buffer[length++] = (char)c;
        } else {
            draining = true;
        }
    }

    *out_length = length;
    *out_bytes_read = bytes_read;

    if (draining) {
        return fail(reader, out_error, PRINTERCTL_GCODE_ERR_LINE_TOO_LONG, line_number,
                     "line exceeds the maximum of %d content bytes", PRINTERCTL_GCODE_LINE_MAX);
    }

    return PRINTERCTL_GCODE_OK;
}

bool printerctl_gcode_command_is_unsafe_g28(const char *command)
{
    if (command == NULL) {
        return false;
    }

    while (*command == ' ' || *command == '\t') {
        command++;
    }

    return is_unsafe_g28(command, strlen(command));
}

printerctl_gcode_status_t printerctl_gcode_reader_open(
    printerctl_gcode_reader_t *reader,
    const char *path,
    bool allow_g28,
    printerctl_gcode_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_GCODE_OK;

    if (reader == NULL) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT, 0, "reader must not be NULL");
        return out_error->status;
    }

    reset_reader(reader);

    if (path == NULL) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT, 0, "path must not be NULL");
        return out_error->status;
    }

    struct stat pre_stat;
    if (stat(path, &pre_stat) != 0) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_OPEN_FAILED, 0,
                   "cannot stat '%s': %s", path, strerror(errno));
        return out_error->status;
    }

    if (!stat_is_regular(&pre_stat)) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_NOT_REGULAR, 0,
                   "'%s' is not a regular file", path);
        return out_error->status;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_OPEN_FAILED, 0,
                   "cannot open '%s': %s", path, strerror(errno));
        return out_error->status;
    }

    int fd = fileno(file);
    if (fd == -1) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_OPEN_FAILED, 0,
                   "cannot obtain file descriptor for '%s': %s", path, strerror(errno));
        fclose(file);
        return out_error->status;
    }

    struct stat live_stat;
    if (fstat(fd, &live_stat) != 0) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_OPEN_FAILED, 0,
                   "cannot fstat '%s': %s", path, strerror(errno));
        fclose(file);
        return out_error->status;
    }

    if (!stat_is_regular(&live_stat)) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_NOT_REGULAR, 0,
                   "'%s' is not a regular file", path);
        fclose(file);
        return out_error->status;
    }

    if (live_stat.st_dev != pre_stat.st_dev ||
        live_stat.st_ino != pre_stat.st_ino ||
        live_stat.st_size != pre_stat.st_size) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_FILE_CHANGED, 0,
                   "'%s' changed between stat and open", path);
        fclose(file);
        return out_error->status;
    }

    reader->file = file;
    reader->open = true;
    reader->allow_g28 = allow_g28;
    reader->terminal_status = PRINTERCTL_GCODE_OK;

    reader->device = (uint64_t)live_stat.st_dev;
    reader->inode = (uint64_t)live_stat.st_ino;
    reader->size = (uint64_t)live_stat.st_size;
    reader->mtime_sec = (uint64_t)live_stat.st_mtim.tv_sec;
    reader->mtime_nsec = (uint64_t)live_stat.st_mtim.tv_nsec;

    return PRINTERCTL_GCODE_OK;
}

printerctl_gcode_status_t printerctl_gcode_reader_next(
    printerctl_gcode_reader_t *reader,
    printerctl_gcode_result_t *result,
    printerctl_gcode_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_GCODE_OK;

    if (reader == NULL) {
        return fail(NULL, out_error, PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT, 0,
                     "reader must not be NULL");
    }

    if (result == NULL) {
        return fail(reader, out_error, PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT, 0,
                     "result must not be NULL");
    }

    if (!reader->open) {
        return fail(reader, out_error, PRINTERCTL_GCODE_ERR_NOT_OPEN, 0,
                     "reader is not open");
    }

    if (reader->terminal_status != PRINTERCTL_GCODE_OK) {
        out_error->status = reader->terminal_status;
        out_error->line_number = 0;
        snprintf(out_error->message, sizeof(out_error->message),
                  "reader already reached terminal status %d", (int)reader->terminal_status);
        return reader->terminal_status;
    }

    for (;;) {
        printerctl_gcode_status_t fs_status = check_file_unchanged(reader);
        if (fs_status != PRINTERCTL_GCODE_OK) {
            return fail(reader, out_error, fs_status, 0,
                         fs_status == PRINTERCTL_GCODE_ERR_FILE_CHANGED
                             ? "underlying file changed since it was opened"
                             : "cannot fstat underlying file");
        }

        uint64_t line_number = reader->line_number + 1;
        char raw[PRINTERCTL_GCODE_COMMAND_MAX];
        size_t raw_length = 0;
        uint64_t bytes_read = 0;

        printerctl_gcode_status_t read_status =
            read_raw_line(reader, out_error, line_number, raw, &raw_length, &bytes_read);
        if (read_status != PRINTERCTL_GCODE_OK) {
            return read_status;
        }

        if (bytes_read == 0) {
            if (reader->bytes_consumed != reader->size) {
                return fail(reader, out_error, PRINTERCTL_GCODE_ERR_IO, 0,
                             "reached end of file after %llu bytes but expected %llu",
                             (unsigned long long)reader->bytes_consumed,
                             (unsigned long long)reader->size);
            }
            return fail(reader, out_error, PRINTERCTL_GCODE_EOF, 0, "end of file reached");
        }

        reader->line_number = line_number;
        reader->bytes_consumed += bytes_read;

        if (line_number == 1) {
            strip_leading_bom(raw, &raw_length);
        }

        if (contains_bom_sequence(raw, raw_length)) {
            return fail(reader, out_error, PRINTERCTL_GCODE_ERR_MALFORMED, line_number,
                         "unexpected byte order mark");
        }

        char command[PRINTERCTL_GCODE_COMMAND_MAX];
        size_t command_length = 0;
        printerctl_gcode_status_t comment_status =
            strip_comments(reader, out_error, line_number, raw, raw_length, command, &command_length);
        if (comment_status != PRINTERCTL_GCODE_OK) {
            return comment_status;
        }

        trim_spaces_tabs(command, &command_length);

        if (command_length == 0) {
            continue;
        }

        if (!reader->allow_g28 && is_unsafe_g28(command, command_length)) {
            return fail(reader, out_error, PRINTERCTL_GCODE_ERR_UNSAFE_G28, line_number,
                         "G28 homing command is not allowed");
        }

        reader->command_count++;

        memcpy(result->command, command, command_length + 1);
        result->line_number = line_number;
        result->command_number = reader->command_count;
        result->bytes_consumed = reader->bytes_consumed;
        result->file_size = reader->size;
        result->progress_percent = compute_progress(reader->bytes_consumed, reader->size);

        out_error->status = PRINTERCTL_GCODE_OK;
        return PRINTERCTL_GCODE_OK;
    }
}

printerctl_gcode_status_t printerctl_gcode_reader_close(
    printerctl_gcode_reader_t *reader,
    printerctl_gcode_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = PRINTERCTL_GCODE_OK;

    if (reader == NULL) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_INVALID_ARGUMENT, 0, "reader must not be NULL");
        return out_error->status;
    }

    if (!reader->open) {
        return PRINTERCTL_GCODE_OK;
    }

    FILE *file = reader->file;
    reset_reader(reader);

    if (fclose(file) != 0) {
        set_error(out_error, PRINTERCTL_GCODE_ERR_IO, 0,
                   "error closing file: %s", strerror(errno));
        return out_error->status;
    }

    return PRINTERCTL_GCODE_OK;
}
