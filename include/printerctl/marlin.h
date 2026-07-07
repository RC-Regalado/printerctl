#ifndef PRINTERCTL_MARLIN_H
#define PRINTERCTL_MARLIN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Milestone 4 Marlin response parser.
 *
 * Pure text parsing, completely independent of the serial transport: every
 * function here takes one already-received response line (NUL-terminated,
 * without its trailing newline — stripping line endings is the future
 * serial module's job) and classifies or decodes it. No I/O, no heap
 * allocation, no global state; all storage lives in caller-provided
 * structs, following the same output-parameter discipline as cli.h,
 * config.h and gcode.h (output structs are always fully initialized, on
 * both success and failure).
 *
 * Classification contract (see printerctl_marlin_classify()):
 *
 *   - `ok` (alone or as `ok <payload>`) is the acknowledgement that
 *     terminates a command exchange successfully.
 *   - `Error:<text>` is a terminal failure.
 *   - `busy:<text>` and `echo:busy:<text>` are non-terminal: the printer
 *     is still working and the caller must keep waiting.
 *   - `echo:<text>` is non-terminal informational output.
 *   - `Resend:<n>` requests retransmission from line <n>. The MVP does
 *     not implement resend (roadmap.md backlog items 1-3), so callers
 *     must treat it as terminal-unsupported: stop streaming with an
 *     explicit "resend required but unsupported" failure, never ignore it
 *     and never blindly continue.
 *   - Anything else (e.g. `start`, `wait`, an M105/M114/M115 report line
 *     arriving without an `ok ` prefix) is a non-terminal informational
 *     line. It is never an error by itself; callers that expect a typed
 *     report run the matching parse function below on it.
 *
 * Prefix matching is exact-case (`ok`, `Error:`, `busy:`, `echo:`,
 * `Resend:`), matching what Marlin actually emits; this parser does not
 * guess at case variants real firmware never produces.
 */

typedef enum {
    PRINTERCTL_MARLIN_OK = 0,
    PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
    PRINTERCTL_MARLIN_ERR_NO_MATCH
} printerctl_marlin_status_t;

typedef enum {
    PRINTERCTL_MARLIN_RESPONSE_ACK = 0,   /* `ok`, terminal success */
    PRINTERCTL_MARLIN_RESPONSE_ERROR,     /* `Error:`, terminal failure */
    PRINTERCTL_MARLIN_RESPONSE_BUSY,      /* `busy:` / `echo:busy:`, keep waiting */
    PRINTERCTL_MARLIN_RESPONSE_ECHO,      /* `echo:`, informational */
    PRINTERCTL_MARLIN_RESPONSE_RESEND,    /* `Resend:`, unsupported in the MVP */
    PRINTERCTL_MARLIN_RESPONSE_INFO      /* any other line, informational */
} printerctl_marlin_response_type_t;

/*
 * One classified response line. `payload` is a borrowed pointer into the
 * `line` given to printerctl_marlin_classify() — the text after the
 * classified prefix with leading spaces/tabs skipped, or the whole line
 * for PRINTERCTL_MARLIN_RESPONSE_INFO. It is valid only as long as that
 * `line` buffer is; callers must not retain it past the buffer's
 * lifetime. It is never NULL after a successful classify (an `ok` with no
 * payload yields an empty string).
 *
 * `terminal` is the streaming decision predigested: true exactly for ACK,
 * ERROR and RESEND (the three types after which a caller stops waiting
 * for more lines for the current command), false for BUSY, ECHO and INFO.
 *
 * `has_resend_line`/`resend_line` are meaningful only for RESEND:
 * has_resend_line is true when a decimal line number followed the
 * `Resend:` prefix (Marlin always sends one, but a malformed value is
 * reported as has_resend_line == false rather than a guessed number).
 */
typedef struct {
    printerctl_marlin_response_type_t type;
    bool terminal;
    const char *payload;
    bool has_resend_line;
    uint64_t resend_line;
} printerctl_marlin_response_t;

/*
 * Classifies one response line into *out. `line` and `out` must be
 * non-NULL (PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT otherwise; *out is
 * reset to a well-defined INFO/empty state whenever out itself is
 * non-NULL). Never fails for any actual text: every line classifies as
 * one of the six types, with unknown lines becoming
 * PRINTERCTL_MARLIN_RESPONSE_INFO. Returns PRINTERCTL_MARLIN_OK.
 */
printerctl_marlin_status_t printerctl_marlin_classify(const char *line,
                                                        printerctl_marlin_response_t *out);

/*
 * Decoded M105-style temperature report. `has_hotend`/`has_bed` say which
 * sensors appeared in the line; their actual/target pairs are meaningful
 * only when the corresponding flag is true. Targets are 0.0 when the
 * heater is off, exactly as Marlin reports them.
 */
typedef struct {
    bool has_hotend;
    double hotend_actual;
    double hotend_target;
    bool has_bed;
    double bed_actual;
    double bed_target;
} printerctl_marlin_temperature_report_t;

/*
 * Parses a temperature report of the shape Marlin emits for M105 and for
 * automatic temperature reporting:
 *
 *   T:210.00 /210.00 B:60.00 /60.00 @:127 B@:0
 *   T:23.5/0.0 B:22.9/0.0 @:0
 *
 * `line` may be either a full response line or the payload of an
 * `ok T:...` acknowledgement (callers typically classify first and pass
 * response.payload). The optional space before `/` and the `@:`/`B@:`
 * power fields are tolerated; `B@:` is never confused with `B:`. `line`
 * and `out` must be non-NULL (PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT).
 * Returns PRINTERCTL_MARLIN_ERR_NO_MATCH when the line contains neither a
 * `T:actual/target` nor a `B:actual/target` pair; *out is zeroed in every
 * non-OK case.
 */
printerctl_marlin_status_t printerctl_marlin_parse_temperature(
    const char *line,
    printerctl_marlin_temperature_report_t *out);

/* Decoded M114-style position report, logical coordinates only. */
typedef struct {
    double x;
    double y;
    double z;
    bool has_e;
    double e;
} printerctl_marlin_position_report_t;

/*
 * Parses a position report of the shape Marlin emits for M114:
 *
 *   X:0.00 Y:0.00 Z:0.00 E:0.00 Count X:0 Y:0 Z:0
 *
 * Only the logical coordinates before `Count` are decoded; the stepper
 * counts after it are ignored. X, Y and Z are all required for the line
 * to count as a position report (PRINTERCTL_MARLIN_ERR_NO_MATCH
 * otherwise, so arbitrary lines that merely contain an `X:` somewhere are
 * not misread); E is optional and reported via `has_e`. `line` and `out`
 * must be non-NULL (PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT). *out is
 * zeroed in every non-OK case.
 */
printerctl_marlin_status_t printerctl_marlin_parse_position(
    const char *line,
    printerctl_marlin_position_report_t *out);

/*
 * Size of each decoded M115 text field, including the terminating '\0'.
 * A longer value in the response is truncated to fit, never an error:
 * these fields are rendered for humans, and Marlin's own values are far
 * shorter in practice.
 */
#define PRINTERCTL_MARLIN_INFO_FIELD_MAX 128

/*
 * Decoded M115 firmware info. Fields that did not appear in the line are
 * empty strings (or has_extruder_count == false); `firmware_name` is the
 * only field required for the line to match at all.
 */
typedef struct {
    char firmware_name[PRINTERCTL_MARLIN_INFO_FIELD_MAX];
    char machine_type[PRINTERCTL_MARLIN_INFO_FIELD_MAX];
    char protocol_version[PRINTERCTL_MARLIN_INFO_FIELD_MAX];
    char uuid[PRINTERCTL_MARLIN_INFO_FIELD_MAX];
    bool has_extruder_count;
    int extruder_count;
} printerctl_marlin_firmware_info_t;

/*
 * Parses an M115 firmware information line:
 *
 *   FIRMWARE_NAME:Marlin 2.1.2 (...) SOURCE_CODE_URL:... PROTOCOL_VERSION:1.0
 *   MACHINE_TYPE:EasyThreed K9 EXTRUDER_COUNT:1 UUID:...
 *
 * M115 values may themselves contain spaces (firmware and machine names
 * do), so each value runs from its key to the start of the next known
 * key (`FIRMWARE_NAME:`, `SOURCE_CODE_URL:`, `PROTOCOL_VERSION:`,
 * `MACHINE_TYPE:`, `EXTRUDER_COUNT:`, `UUID:`) or the end of the line,
 * with surrounding whitespace trimmed. A line without `FIRMWARE_NAME:`
 * is PRINTERCTL_MARLIN_ERR_NO_MATCH. An unparsable EXTRUDER_COUNT value
 * yields has_extruder_count == false rather than a guess. `line` and
 * `out` must be non-NULL (PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT). *out
 * is zeroed in every non-OK case.
 */
printerctl_marlin_status_t printerctl_marlin_parse_firmware_info(
    const char *line,
    printerctl_marlin_firmware_info_t *out);

#endif
