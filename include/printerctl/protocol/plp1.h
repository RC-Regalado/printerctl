#ifndef PRINTERCTL_PROTOCOL_PLP1_H
#define PRINTERCTL_PROTOCOL_PLP1_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Milestone 14: Printerctl Local Protocol v1 (PLP/1) -- the binary,
 * length-prefixed, big-endian wire contract local clients will use to
 * talk to the future `printerctld` over AF_UNIX/SOCK_STREAM (Milestone
 * 15). This module is a pure codec: it never opens a socket, never
 * knows a file descriptor, thread or pipe, and never touches
 * `print_engine`, `printer` or `serial` -- it can be exercised entirely
 * against in-memory buffers, which is exactly what its test suite does.
 * See docs/design/protocol-v1.md for the full wire specification this
 * header implements, including the handful of wire-width decisions
 * (STATUS payload field sizes) this milestone had to make that the
 * task did not fully pin down itself.
 *
 * Frame layout (16-byte header, all multi-byte fields big-endian):
 *
 *   Offset  Size  Field
 *   0       4     frame_length  (bytes following this field itself)
 *   4       1     version
 *   5       1     message_type
 *   6       2     flags
 *   8       4     request_id
 *   12      2     status
 *   14      2     reserved      (MUST be 0)
 *   16      N     payload       (N = frame_length - 12)
 *
 * Nothing here is a packed C struct memcpy'd to the wire: every
 * multi-byte field is written/read byte-by-byte through the pack/unpack
 * helpers in plp1.c, because printerctl_plp1_frame_t (the decoded, C-side
 * view) has no wire-compatible memory layout guarantee across compilers
 * or platforms -- "no serializar structs C directamente al wire" is a
 * structural requirement here, not just a style preference.
 */

#define PRINTERCTL_PLP1_FRAME_PREFIX_SIZE 4u
#define PRINTERCTL_PLP1_HEADER_SIZE 16u
#define PRINTERCTL_PLP1_MIN_FRAME_LENGTH 12u
#define PRINTERCTL_PLP1_MAX_FRAME_LENGTH 65536u
#define PRINTERCTL_PLP1_MAX_FRAME_SIZE \
    (PRINTERCTL_PLP1_FRAME_PREFIX_SIZE + PRINTERCTL_PLP1_MAX_FRAME_LENGTH)

#define PRINTERCTL_PLP1_VERSION_1 0x01u

/*
 * Message type registry (docs/design/protocol-v1.md's canonical copy is
 * authoritative). IDs are part of the wire contract and must never be
 * reused for an unrelated operation once published, per the roadmap.
 */
#define PRINTERCTL_PLP1_MSG_PING 0x01u
#define PRINTERCTL_PLP1_MSG_VERSION 0x02u
#define PRINTERCTL_PLP1_MSG_STATUS 0x03u
#define PRINTERCTL_PLP1_MSG_PRINT_START 0x20u
#define PRINTERCTL_PLP1_MSG_PRINT_PAUSE 0x21u
#define PRINTERCTL_PLP1_MSG_PRINT_RESUME 0x22u
#define PRINTERCTL_PLP1_MSG_PRINT_CANCEL 0x23u
#define PRINTERCTL_PLP1_MSG_QUERY_TEMP 0x30u
#define PRINTERCTL_PLP1_MSG_QUERY_POSITION 0x31u
#define PRINTERCTL_PLP1_MSG_QUERY_INFO 0x32u
#define PRINTERCTL_PLP1_MSG_RAW_SEND 0x33u
#define PRINTERCTL_PLP1_MSG_HOME 0x34u
#define PRINTERCTL_PLP1_MSG_SD_PAUSE 0x35u
#define PRINTERCTL_PLP1_MSG_SD_RESUME 0x36u
#define PRINTERCTL_PLP1_MSG_SD_CANCEL 0x37u

/* Flags (uint16 bitmap). Exactly one of REQUEST/RESPONSE/EVENT must be
 * set on any valid frame; bits 6-15 are reserved and must be 0. */
#define PRINTERCTL_PLP1_FLAG_REQUEST 0x0001u
#define PRINTERCTL_PLP1_FLAG_RESPONSE 0x0002u
#define PRINTERCTL_PLP1_FLAG_ERROR 0x0004u
#define PRINTERCTL_PLP1_FLAG_HAS_PAYLOAD 0x0008u
#define PRINTERCTL_PLP1_FLAG_EVENT 0x0010u
#define PRINTERCTL_PLP1_FLAG_MORE 0x0020u
#define PRINTERCTL_PLP1_FLAG_KNOWN_MASK 0x003Fu

/*
 * Wire status vocabulary. Values are fixed to the numbers the wire
 * carries (see docs/design/protocol-v1.md), so encoding a status is a
 * direct write of the enum value and decoding needs no lookup table.
 */
typedef enum {
    PRINTERCTL_PLP1_SUCCESS = 0x0000,

    PRINTERCTL_PLP1_MALFORMED_FRAME = 0x0101,
    PRINTERCTL_PLP1_UNSUPPORTED_VERSION = 0x0102,
    PRINTERCTL_PLP1_UNKNOWN_MESSAGE_TYPE = 0x0103,
    PRINTERCTL_PLP1_INVALID_FLAGS = 0x0104,
    PRINTERCTL_PLP1_INVALID_PAYLOAD = 0x0105,
    PRINTERCTL_PLP1_FRAME_TOO_LARGE = 0x0106,

    PRINTERCTL_PLP1_INVALID_ARGUMENT = 0x0201,
    PRINTERCTL_PLP1_INVALID_STATE = 0x0202,
    PRINTERCTL_PLP1_BUSY = 0x0203,
    PRINTERCTL_PLP1_NOT_PRINTING = 0x0204,
    PRINTERCTL_PLP1_SAFETY_REJECTED = 0x0205,

    PRINTERCTL_PLP1_WORKER_UNAVAILABLE = 0x0301,
    PRINTERCTL_PLP1_INTERNAL_ERROR = 0x0302,
    PRINTERCTL_PLP1_SHUTTING_DOWN = 0x0303,

    PRINTERCTL_PLP1_PRINTER_ERROR = 0x0401,
    PRINTERCTL_PLP1_SERIAL_ERROR = 0x0402,
    PRINTERCTL_PLP1_PRINTER_TIMEOUT = 0x0403,

    PRINTERCTL_PLP1_FILE_NOT_FOUND = 0x0501,
    PRINTERCTL_PLP1_FILE_NOT_READABLE = 0x0502,
    PRINTERCTL_PLP1_INVALID_GCODE_SOURCE = 0x0503
} printerctl_plp1_wire_status_t;

/*
 * C API result: a function's own success/failure, distinct from the
 * wire status vocabulary above (frame *content*, never a codec
 * function's return value). Every module in this codebase keeps a
 * result/status enum private to itself; PLP/1's wire vocabulary is
 * externally frozen by the wire contract, so it does not get to double
 * as this module's function-return type the way e.g.
 * printerctl_gcode_status_t does in gcode.h.
 */
typedef enum {
    PRINTERCTL_PLP1_OK = 0,
    PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, /* NULL/misuse, or an encode() input
                                            * that would violate the wire
                                            * contract (caller's fault, not
                                            * bad wire data) */
    PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, /* an output buffer was too small */
    PRINTERCTL_PLP1_ERR_MALFORMED         /* decode found bad wire bytes; see
                                            * out_error->wire_status for the
                                            * status code a real response
                                            * would carry */
} printerctl_plp1_result_t;

#define PRINTERCTL_PLP1_ERROR_MAX 256

typedef struct {
    printerctl_plp1_result_t result;
    printerctl_plp1_wire_status_t wire_status; /* meaningful only when
                                                 * result == PRINTERCTL_PLP1_ERR_MALFORMED */
    char message[PRINTERCTL_PLP1_ERROR_MAX];
} printerctl_plp1_error_t;

/*
 * Decoded frame. `payload` borrows directly from the buffer that was
 * decoded -- valid only for as long as that buffer is valid (the same
 * "borrowed, duration-of-the-call" discipline printer.h's
 * printerctl_printer_line_fn already uses), never copied, never owned.
 * `status` is a raw uint16_t rather than printerctl_plp1_wire_status_t:
 * a frame may legitimately carry a status value this build does not
 * recognize yet (forward compatibility). Only structural/framing rules
 * are enforced by the decoder; comparing `status` against the enum
 * constants is the caller's job.
 */
typedef struct {
    uint8_t version;
    uint8_t message_type;
    uint16_t flags;
    uint32_t request_id;
    uint16_t status;
    const uint8_t *payload; /* NULL when payload_length == 0 */
    uint16_t payload_length;
} printerctl_plp1_frame_t;

/*
 * Structural check only: is `frame_length` within
 * [PRINTERCTL_PLP1_MIN_FRAME_LENGTH, PRINTERCTL_PLP1_MAX_FRAME_LENGTH]?
 * This is the check a stream parser runs on the first 4 bytes, before
 * it decides how many more bytes to wait for -- failing it means the
 * stream itself is no longer trustworthy for framing purposes (see
 * plp1_parser.h). Returns PRINTERCTL_PLP1_ERR_MALFORMED with
 * out_error->wire_status set to PRINTERCTL_PLP1_MALFORMED_FRAME (too
 * small) or PRINTERCTL_PLP1_FRAME_TOO_LARGE (too large).
 */
printerctl_plp1_result_t printerctl_plp1_validate_frame_length(
    uint32_t frame_length, printerctl_plp1_error_t *out_error);

/*
 * Encodes `frame` into `out_buffer`, writing exactly
 * PRINTERCTL_PLP1_HEADER_SIZE + frame->payload_length bytes (reported
 * via *out_length). `frame->flags` is written with
 * PRINTERCTL_PLP1_FLAG_HAS_PAYLOAD automatically OR'd in whenever
 * payload_length > 0 -- callers do not need to set it themselves.
 * `reserved` is always written as 0.
 *
 * Validates the same structural rules printerctl_plp1_decode_frame()
 * enforces (version == 1, exactly one of REQUEST/RESPONSE/EVENT, no
 * unknown flag bits, status/ERROR-flag consistency) as caller-input
 * checks: any violation is PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, never
 * PRINTERCTL_PLP1_ERR_MALFORMED (that status is reserved for decoding
 * untrusted wire bytes). Does NOT validate message_type against the
 * Phase 1 registry -- encode() trusts caller-constructed data; registry
 * membership is decode()'s job of validating untrusted input.
 *
 * Returns PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL if out_buffer_size is
 * insufficient; out_buffer is left untouched in every error case.
 */
printerctl_plp1_result_t printerctl_plp1_encode_frame(
    const printerctl_plp1_frame_t *frame,
    uint8_t *out_buffer, size_t out_buffer_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

/*
 * Decodes exactly one complete frame from `buffer`. `buffer_length`
 * must equal exactly 4 + frame_length (the full frame, no more, no
 * less) -- printerctl_plp1_parser_take_frame() is the caller expected
 * to satisfy this in practice, by construction, since it slices exact
 * frame boundaries out of an accumulated byte stream before calling
 * this function. `*out_frame->payload` borrows from `buffer`.
 *
 * Validation order and the exact PRINTERCTL_PLP1_ERR_MALFORMED
 * out_error->wire_status produced by each failure are documented in
 * docs/design/protocol-v1.md; in short: frame_length bounds, exact
 * buffer_length match, reserved == 0, version == 1, flag validity,
 * status/ERROR-flag consistency, then message_type registry membership.
 */
printerctl_plp1_result_t printerctl_plp1_decode_frame(
    const uint8_t *buffer, size_t buffer_length,
    printerctl_plp1_frame_t *out_frame, printerctl_plp1_error_t *out_error);

/*
 * Validates that a decoded frame carries no payload -- the shape PING
 * and the PAUSE/RESUME/CANCEL requests/responses require.
 * Returns PRINTERCTL_PLP1_ERR_MALFORMED / PRINTERCTL_PLP1_INVALID_PAYLOAD
 * when frame->payload_length != 0.
 */
printerctl_plp1_result_t printerctl_plp1_expect_no_payload(
    const printerctl_plp1_frame_t *frame, printerctl_plp1_error_t *out_error);

/*
 * String codec: wire format is a uint16 big-endian length followed by
 * that many raw UTF-8 bytes, no NUL terminator on the wire. `value`
 * (encode) is a NUL-terminated C string; `out_value` (decode) is always
 * NUL-terminated on success. `*out_consumed` (decode) is how many bytes
 * of `buffer` the string field occupied (2 + length), so a caller with
 * more fields after the string can advance correctly -- no Phase 1
 * payload actually has one, but the generic error payload and
 * PRINT_START both reuse these two functions directly.
 */
printerctl_plp1_result_t printerctl_plp1_encode_string(
    const char *value, uint8_t *out_buffer, size_t out_buffer_size,
    size_t *out_length, printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_string(
    const uint8_t *buffer, size_t buffer_length,
    char *out_value, size_t out_value_size, size_t *out_consumed,
    printerctl_plp1_error_t *out_error);

/* VERSION response payload: uint8 protocol_version, uint32 capability_bitmap. */
#define PRINTERCTL_PLP1_CAP_PRINT_CONTROL 0x00000001u
#define PRINTERCTL_PLP1_CAP_PRINTER_STATUS 0x00000002u
#define PRINTERCTL_PLP1_CAP_EVENTS 0x00000004u        /* reserved/future */
#define PRINTERCTL_PLP1_CAP_JOB_QUEUE 0x00000008u     /* reserved/future */
#define PRINTERCTL_PLP1_CAP_HISTORY 0x00000010u       /* reserved/future */
#define PRINTERCTL_PLP1_CAP_MULTI_PRINTER 0x00000020u /* reserved/future */

typedef struct {
    uint8_t protocol_version;
    uint32_t capability_bitmap;
} printerctl_plp1_version_response_t;

printerctl_plp1_result_t printerctl_plp1_encode_version_response(
    const printerctl_plp1_version_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_version_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_version_response_t *out_value,
    printerctl_plp1_error_t *out_error);

/*
 * STATUS request payload: uint32 requested_fields (a single field, not
 * worth a dedicated struct).
 */
printerctl_plp1_result_t printerctl_plp1_encode_status_request(
    uint32_t requested_fields,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_status_request(
    const uint8_t *payload, size_t payload_length,
    uint32_t *out_requested_fields,
    printerctl_plp1_error_t *out_error);

/*
 * STATUS response field bitmap (uint32) and canonical wire order --
 * present fields are always serialized in this order regardless of the
 * order the client requested them in.
 */
#define PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE 0x0001u
#define PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE 0x0002u
#define PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE 0x0004u
#define PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS 0x0008u
#define PRINTERCTL_PLP1_STATUS_FIELD_ELAPSED_TIME 0x0010u
#define PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_SENT 0x0020u
#define PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED 0x0040u
#define PRINTERCTL_PLP1_STATUS_FIELD_SOURCE_POSITION 0x0080u
#define PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON 0x0100u
#define PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK 0x01FFu /* bits 0-8; any other
    bit set in a response's present_fields is rejected -- a decoder cannot
    safely size/skip a field it does not know about (see plp1_parser.h's
    structural-vs-semantic split: this is decode_status_response()'s own
    "malformed content" case, not a framing concern). */

/*
 * daemon_state/worker_state wire enums. Deliberately separate from and
 * independent of include/printerctl/daemon/runtime.h's C enums --
 * protocol/ must not include daemon/ (architectural isolation). A
 * future dispatcher (M15/M16) maps printerctl_daemon_state_t /
 * printerctl_daemon_worker_state_t onto these; UNKNOWN=0 has no
 * equivalent on the daemon side and exists only for a STATUS response
 * describing a field that has no known value yet.
 */
#define PRINTERCTL_PLP1_DAEMON_STATE_UNKNOWN 0u
#define PRINTERCTL_PLP1_DAEMON_STATE_STARTING 1u
#define PRINTERCTL_PLP1_DAEMON_STATE_RUNNING 2u
#define PRINTERCTL_PLP1_DAEMON_STATE_STOPPING 3u
#define PRINTERCTL_PLP1_DAEMON_STATE_STOPPED 4u
#define PRINTERCTL_PLP1_DAEMON_STATE_FAILED 5u

#define PRINTERCTL_PLP1_WORKER_STATE_UNKNOWN 0u
#define PRINTERCTL_PLP1_WORKER_STATE_INITIALIZING 1u
#define PRINTERCTL_PLP1_WORKER_STATE_READY 2u
#define PRINTERCTL_PLP1_WORKER_STATE_STOPPING 3u
#define PRINTERCTL_PLP1_WORKER_STATE_STOPPED 4u
#define PRINTERCTL_PLP1_WORKER_STATE_FAULTED 5u

#define PRINTERCTL_PLP1_EXECUTION_STATE_IDLE 1u
#define PRINTERCTL_PLP1_EXECUTION_STATE_RUNNING 2u
#define PRINTERCTL_PLP1_EXECUTION_STATE_PAUSED 3u
#define PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLING 4u
#define PRINTERCTL_PLP1_EXECUTION_STATE_COMPLETED 5u
#define PRINTERCTL_PLP1_EXECUTION_STATE_CANCELLED 6u
#define PRINTERCTL_PLP1_EXECUTION_STATE_FAILED 7u

/*
 * Wire widths for each optional field (docs/design/protocol-v1.md
 * records the rationale): daemon_state/worker_state/execution_state/
 * progress are 1 byte each; elapsed_time/commands_sent/
 * commands_acknowledged/source_position are 4 bytes each;
 * failure_reason is 2 bytes (reuses printerctl_plp1_wire_status_t's
 * numeric space; 0 means none). `execution_state`'s value set is
 * reserved -- Milestone 16 defines it; M14 only reserves the wire slot.
 */
typedef struct {
    uint32_t present_fields;
    uint8_t daemon_state;
    uint8_t worker_state;
    uint8_t execution_state;
    uint8_t progress;
    uint32_t elapsed_time;
    uint32_t commands_sent;
    uint32_t commands_acknowledged;
    uint32_t source_position;
    uint16_t failure_reason;
} printerctl_plp1_status_response_t;

/*
 * Encodes only the fields whose bit is set in value->present_fields, in
 * canonical order, regardless of field values left in the rest of
 * *value.
 */
printerctl_plp1_result_t printerctl_plp1_encode_status_response(
    const printerctl_plp1_status_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

/*
 * Decodes present_fields, then reads only those fields (canonical
 * order) into *out_value, zeroing every field whose bit is absent.
 * A requested-but-absent field is not an error at this layer -- it
 * simply never appears in present_fields. Returns
 * PRINTERCTL_PLP1_ERR_MALFORMED / PRINTERCTL_PLP1_INVALID_PAYLOAD when
 * payload_length is shorter than present_fields implies.
 */
printerctl_plp1_result_t printerctl_plp1_decode_status_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_status_response_t *out_value,
    printerctl_plp1_error_t *out_error);

/*
 * PRINT_START request payload: a single length-prefixed string, the
 * source path as the operator/client named it. The frame transports the
 * reference, never file contents; M14 never opens this path. An empty
 * path decodes successfully as an empty string -- whether that is an
 * acceptable request is Milestone 16 domain logic, not a codec concern.
 */
#define PRINTERCTL_PLP1_PRINT_START_PATH_MAX 4096u /* matches
    PRINTERCTL_CONFIG_PATH_MAX's precedent in include/printerctl/config.h */

typedef struct {
    char source_path[PRINTERCTL_PLP1_PRINT_START_PATH_MAX];
} printerctl_plp1_print_start_request_t;

printerctl_plp1_result_t printerctl_plp1_encode_print_start_request(
    const char *source_path,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_print_start_request(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_print_start_request_t *out_value,
    printerctl_plp1_error_t *out_error);

/* Read-only query responses (M20). Numeric values are signed fixed-point
 * milli-units on the wire: Celsius for TEMP, millimetres for POSITION. */
#define PRINTERCTL_PLP1_QUERY_TEMP_FIELD_HOTEND 0x01u
#define PRINTERCTL_PLP1_QUERY_TEMP_FIELD_BED 0x02u
#define PRINTERCTL_PLP1_QUERY_POSITION_FIELD_E 0x01u

typedef struct {
    uint8_t present_fields;
    int32_t hotend_actual_milli_c;
    int32_t hotend_target_milli_c;
    int32_t bed_actual_milli_c;
    int32_t bed_target_milli_c;
} printerctl_plp1_query_temp_response_t;

typedef struct {
    uint8_t present_fields;
    int32_t x_milli_mm;
    int32_t y_milli_mm;
    int32_t z_milli_mm;
    int32_t e_milli_mm;
} printerctl_plp1_query_position_response_t;

typedef struct {
    char firmware_name[128];
    char machine_type[128];
    char protocol_version[128];
    char uuid[128];
    bool has_extruder_count;
    uint16_t extruder_count;
} printerctl_plp1_query_info_response_t;

#define PRINTERCTL_PLP1_RAW_COMMAND_MAX 1024u
#define PRINTERCTL_PLP1_RAW_RESPONSE_MAX 4096u

typedef struct {
    char command[PRINTERCTL_PLP1_RAW_COMMAND_MAX];
} printerctl_plp1_raw_send_request_t;

typedef struct {
    char transcript[PRINTERCTL_PLP1_RAW_RESPONSE_MAX];
} printerctl_plp1_raw_send_response_t;

#define PRINTERCTL_PLP1_HOME_ALL 0u
#define PRINTERCTL_PLP1_HOME_X 1u
#define PRINTERCTL_PLP1_HOME_Y 2u
#define PRINTERCTL_PLP1_HOME_Z 3u

typedef struct {
    uint8_t axis;
} printerctl_plp1_home_request_t;

printerctl_plp1_result_t printerctl_plp1_encode_query_temp_response(
    const printerctl_plp1_query_temp_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_query_temp_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_query_temp_response_t *out_value,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_encode_query_position_response(
    const printerctl_plp1_query_position_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_query_position_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_query_position_response_t *out_value,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_encode_query_info_response(
    const printerctl_plp1_query_info_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_query_info_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_query_info_response_t *out_value,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_encode_raw_send_request(
    const char *command,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_raw_send_request(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_raw_send_request_t *out_value,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_encode_raw_send_response(
    const char *transcript,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_raw_send_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_raw_send_response_t *out_value,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_encode_home_request(
    uint8_t axis,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_home_request(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_home_request_t *out_value,
    printerctl_plp1_error_t *out_error);

/*
 * Generic error payload (an ERROR-flagged response may carry one):
 * uint16 message_length + that many UTF-8 bytes, informative only --
 * clients must never depend on parsing this text to determine an error
 * class; that is what `status` is for. The decode side truncates into
 * the caller's buffer rather than requiring it to be large enough for
 * an arbitrary message: unlike PRINT_START's path, truncating a
 * human-readable diagnostic loses nothing safety-relevant.
 */
printerctl_plp1_result_t printerctl_plp1_encode_error_payload(
    const char *message,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error);

printerctl_plp1_result_t printerctl_plp1_decode_error_payload(
    const uint8_t *payload, size_t payload_length,
    char *out_message, size_t out_message_size,
    printerctl_plp1_error_t *out_error);

#endif
