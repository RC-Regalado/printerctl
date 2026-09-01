#include "printerctl/protocol/plp1.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void set_error(printerctl_plp1_error_t *out_error, printerctl_plp1_result_t result,
                       printerctl_plp1_wire_status_t wire_status, const char *format, ...)
{
    out_error->result = result;
    out_error->wire_status = wire_status;

    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static void write_u16be(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)(value & 0xFFu);
}

static void write_u32be(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value >> 24);
    buffer[1] = (uint8_t)(value >> 16);
    buffer[2] = (uint8_t)(value >> 8);
    buffer[3] = (uint8_t)(value & 0xFFu);
}

static void write_i32be(uint8_t *buffer, int32_t value)
{
    write_u32be(buffer, (uint32_t)value);
}

static uint16_t read_u16be(const uint8_t *buffer)
{
    return (uint16_t)(((uint16_t)buffer[0] << 8) | (uint16_t)buffer[1]);
}

static uint32_t read_u32be(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
}

static int32_t read_i32be(const uint8_t *buffer)
{
    return (int32_t)read_u32be(buffer);
}

static bool message_type_known(uint8_t type)
{
    switch (type) {
    case PRINTERCTL_PLP1_MSG_PING:
    case PRINTERCTL_PLP1_MSG_VERSION:
    case PRINTERCTL_PLP1_MSG_STATUS:
    case PRINTERCTL_PLP1_MSG_PRINT_START:
    case PRINTERCTL_PLP1_MSG_PRINT_PAUSE:
    case PRINTERCTL_PLP1_MSG_PRINT_RESUME:
    case PRINTERCTL_PLP1_MSG_PRINT_CANCEL:
    case PRINTERCTL_PLP1_MSG_QUERY_TEMP:
    case PRINTERCTL_PLP1_MSG_QUERY_POSITION:
    case PRINTERCTL_PLP1_MSG_QUERY_INFO:
        return true;
    default:
        return false;
    }
}

/*
 * Which of REQUEST/RESPONSE/EVENT a known message type may legitimately
 * carry. Every Phase 1 message type is a plain request/response
 * exchange -- none of them is an event (the 0x40-0x4F "events" range is
 * reserved but empty in M14), so EVENT combined with any message type
 * defined today is a semantic error, not just a structural one. Only
 * called for types message_type_known() already accepted; an unknown
 * type has no role constraint applied by this codec (that is decode's
 * separate UNKNOWN_MESSAGE_TYPE check).
 */
static uint16_t message_type_allowed_roles(uint8_t type)
{
    switch (type) {
    case PRINTERCTL_PLP1_MSG_PING:
    case PRINTERCTL_PLP1_MSG_VERSION:
    case PRINTERCTL_PLP1_MSG_STATUS:
    case PRINTERCTL_PLP1_MSG_PRINT_START:
    case PRINTERCTL_PLP1_MSG_PRINT_PAUSE:
    case PRINTERCTL_PLP1_MSG_PRINT_RESUME:
    case PRINTERCTL_PLP1_MSG_PRINT_CANCEL:
    case PRINTERCTL_PLP1_MSG_QUERY_TEMP:
    case PRINTERCTL_PLP1_MSG_QUERY_POSITION:
    case PRINTERCTL_PLP1_MSG_QUERY_INFO:
        return PRINTERCTL_PLP1_FLAG_REQUEST | PRINTERCTL_PLP1_FLAG_RESPONSE;
    default:
        return 0;
    }
}

typedef enum {
    PLP1_PAYLOAD_FORBIDDEN = 0,
    PLP1_PAYLOAD_REQUIRED,
    PLP1_PAYLOAD_OPTIONAL
} plp1_payload_requirement_t;

/*
 * Whether a (message_type, flags) combination must carry a payload,
 * must not, or may either way -- enforced structurally by
 * decode_frame() itself (§16/§19 of docs/design/protocol-v1.md) rather
 * than left to whichever payload-specific decoder or
 * printerctl_plp1_expect_no_payload() call a caller happens to remember
 * to invoke afterward. An ERROR-flagged response of *any* message type
 * may optionally carry the generic error payload instead of its normal
 * success shape (§19), which takes priority over the per-message rule
 * below. Only called for types message_type_known() already accepted.
 */
static plp1_payload_requirement_t message_role_payload_requirement(uint8_t type, uint16_t flags)
{
    if (flags & PRINTERCTL_PLP1_FLAG_ERROR) {
        return PLP1_PAYLOAD_OPTIONAL;
    }

    bool is_response = (flags & PRINTERCTL_PLP1_FLAG_RESPONSE) != 0;

    switch (type) {
    case PRINTERCTL_PLP1_MSG_PING:
    case PRINTERCTL_PLP1_MSG_PRINT_PAUSE:
    case PRINTERCTL_PLP1_MSG_PRINT_RESUME:
    case PRINTERCTL_PLP1_MSG_PRINT_CANCEL:
        return PLP1_PAYLOAD_FORBIDDEN;
    case PRINTERCTL_PLP1_MSG_VERSION:
        return is_response ? PLP1_PAYLOAD_REQUIRED : PLP1_PAYLOAD_FORBIDDEN;
    case PRINTERCTL_PLP1_MSG_STATUS:
        /* Both directions always carry at least their bitmap
         * (requested_fields / present_fields). */
        return PLP1_PAYLOAD_REQUIRED;
    case PRINTERCTL_PLP1_MSG_PRINT_START:
        /* The request always carries source_path, which is at least 2
         * bytes (the length prefix) even for an empty string -- a
         * request with no payload at all has no length prefix to read
         * and is therefore invalid, not merely "an empty string". The
         * success response has no payload. */
        return is_response ? PLP1_PAYLOAD_FORBIDDEN : PLP1_PAYLOAD_REQUIRED;
    case PRINTERCTL_PLP1_MSG_QUERY_TEMP:
    case PRINTERCTL_PLP1_MSG_QUERY_POSITION:
    case PRINTERCTL_PLP1_MSG_QUERY_INFO:
        return is_response ? PLP1_PAYLOAD_REQUIRED : PLP1_PAYLOAD_FORBIDDEN;
    default:
        return PLP1_PAYLOAD_OPTIONAL; /* unreachable: message_type_known()
            already rejected anything else before this is ever called;
            optional here is the conservative default for defense in depth. */
    }
}

printerctl_plp1_result_t printerctl_plp1_validate_frame_length(
    uint32_t frame_length, printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (frame_length < PRINTERCTL_PLP1_MIN_FRAME_LENGTH) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_MALFORMED_FRAME,
                  "frame_length %u is below the minimum %u", frame_length,
                  PRINTERCTL_PLP1_MIN_FRAME_LENGTH);
        return out_error->result;
    }
    if (frame_length > PRINTERCTL_PLP1_MAX_FRAME_LENGTH) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_FRAME_TOO_LARGE,
                  "frame_length %u exceeds the maximum %u", frame_length,
                  PRINTERCTL_PLP1_MAX_FRAME_LENGTH);
        return out_error->result;
    }
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_frame(
    const printerctl_plp1_frame_t *frame,
    uint8_t *out_buffer, size_t out_buffer_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (frame == NULL || out_buffer == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "frame, out_buffer and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;

    if (frame->version != PRINTERCTL_PLP1_VERSION_1) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "unsupported version %u", frame->version);
        return out_error->result;
    }

    if (frame->payload_length > 0 && frame->payload == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "payload_length > 0 requires a non-NULL payload");
        return out_error->result;
    }

    if ((uint32_t)frame->payload_length >
        PRINTERCTL_PLP1_MAX_FRAME_LENGTH - PRINTERCTL_PLP1_MIN_FRAME_LENGTH) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "payload_length %u is too large to fit in one frame", frame->payload_length);
        return out_error->result;
    }

    if ((frame->flags & PRINTERCTL_PLP1_FLAG_HAS_PAYLOAD) && frame->payload_length == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "HAS_PAYLOAD is set but payload_length is 0");
        return out_error->result;
    }

    uint16_t flags = frame->flags;
    if (frame->payload_length > 0) {
        flags |= PRINTERCTL_PLP1_FLAG_HAS_PAYLOAD;
    }

    if (flags & ~(uint16_t)PRINTERCTL_PLP1_FLAG_KNOWN_MASK) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "unknown flag bits set: 0x%04X", flags);
        return out_error->result;
    }

    int role_count = ((flags & PRINTERCTL_PLP1_FLAG_REQUEST) != 0) +
                      ((flags & PRINTERCTL_PLP1_FLAG_RESPONSE) != 0) +
                      ((flags & PRINTERCTL_PLP1_FLAG_EVENT) != 0);
    if (role_count != 1) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "exactly one of REQUEST/RESPONSE/EVENT must be set, found %d", role_count);
        return out_error->result;
    }

    if (message_type_known(frame->message_type) &&
        (flags & message_type_allowed_roles(frame->message_type)) == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "message type 0x%02X does not support the role flags 0x%04X",
                  frame->message_type, flags);
        return out_error->result;
    }

    if ((flags & (PRINTERCTL_PLP1_FLAG_REQUEST | PRINTERCTL_PLP1_FLAG_RESPONSE)) &&
        frame->request_id == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "request_id 0 is reserved for uncorrelated messages; a REQUEST/RESPONSE must "
                  "use 1..UINT32_MAX");
        return out_error->result;
    }

    if (flags & PRINTERCTL_PLP1_FLAG_REQUEST) {
        if (frame->status != PRINTERCTL_PLP1_SUCCESS) {
            set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                      "a request's status must be SUCCESS, got 0x%04X", frame->status);
            return out_error->result;
        }
    }
    if (flags & PRINTERCTL_PLP1_FLAG_RESPONSE) {
        bool has_error_flag = (flags & PRINTERCTL_PLP1_FLAG_ERROR) != 0;
        bool status_is_error = frame->status != PRINTERCTL_PLP1_SUCCESS;
        if (has_error_flag != status_is_error) {
            set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                      "ERROR flag (%d) inconsistent with status 0x%04X", (int)has_error_flag,
                      frame->status);
            return out_error->result;
        }
    }
    if (message_type_known(frame->message_type)) {
        plp1_payload_requirement_t payload_requirement =
            message_role_payload_requirement(frame->message_type, flags);
        if (payload_requirement == PLP1_PAYLOAD_FORBIDDEN && frame->payload_length != 0) {
            set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                      "message type 0x%02X (flags 0x%04X) does not accept a payload, got %u "
                      "byte(s)",
                      frame->message_type, flags, frame->payload_length);
            return out_error->result;
        }
        if (payload_requirement == PLP1_PAYLOAD_REQUIRED && frame->payload_length == 0) {
            set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                      "message type 0x%02X (flags 0x%04X) requires a payload, got none",
                      frame->message_type, flags);
            return out_error->result;
        }
    }

    size_t frame_length = (size_t)PRINTERCTL_PLP1_MIN_FRAME_LENGTH + frame->payload_length;
    size_t total_size = (size_t)PRINTERCTL_PLP1_FRAME_PREFIX_SIZE + frame_length;

    if (out_buffer_size < total_size) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_buffer_size %zu is smaller than the %zu bytes this frame needs",
                  out_buffer_size, total_size);
        return out_error->result;
    }

    write_u32be(out_buffer, (uint32_t)frame_length);
    out_buffer[4] = frame->version;
    out_buffer[5] = frame->message_type;
    write_u16be(out_buffer + 6, flags);
    write_u32be(out_buffer + 8, frame->request_id);
    write_u16be(out_buffer + 12, frame->status);
    write_u16be(out_buffer + 14, 0); /* reserved */
    if (frame->payload_length > 0) {
        memcpy(out_buffer + PRINTERCTL_PLP1_HEADER_SIZE, frame->payload, frame->payload_length);
    }

    *out_length = total_size;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_frame(
    const uint8_t *buffer, size_t buffer_length,
    printerctl_plp1_frame_t *out_frame, printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (buffer == NULL || out_frame == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "buffer and out_frame must not be NULL");
        return out_error->result;
    }
    memset(out_frame, 0, sizeof(*out_frame));

    if (buffer_length < PRINTERCTL_PLP1_FRAME_PREFIX_SIZE) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_MALFORMED_FRAME,
                  "buffer of %zu byte(s) is shorter than the frame_length prefix", buffer_length);
        return out_error->result;
    }

    uint32_t frame_length = read_u32be(buffer);
    printerctl_plp1_result_t length_status =
        printerctl_plp1_validate_frame_length(frame_length, out_error);
    if (length_status != PRINTERCTL_PLP1_OK) {
        return length_status;
    }

    size_t expected_total = (size_t)PRINTERCTL_PLP1_FRAME_PREFIX_SIZE + (size_t)frame_length;
    if (buffer_length != expected_total) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_MALFORMED_FRAME,
                  "buffer_length %zu does not match the declared frame (expected %zu)",
                  buffer_length, expected_total);
        return out_error->result;
    }

    uint8_t version = buffer[4];
    uint8_t message_type = buffer[5];
    uint16_t flags = read_u16be(buffer + 6);
    uint32_t request_id = read_u32be(buffer + 8);
    uint16_t status = read_u16be(buffer + 12);
    uint16_t reserved = read_u16be(buffer + 14);
    uint16_t payload_length = (uint16_t)(frame_length - PRINTERCTL_PLP1_MIN_FRAME_LENGTH);

    if (reserved != 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_MALFORMED_FRAME,
                  "reserved header field must be 0, got 0x%04X", reserved);
        return out_error->result;
    }
    if (version != PRINTERCTL_PLP1_VERSION_1) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_UNSUPPORTED_VERSION,
                  "unsupported protocol version %u", version);
        return out_error->result;
    }
    if (flags & ~(uint16_t)PRINTERCTL_PLP1_FLAG_KNOWN_MASK) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_FLAGS,
                  "unknown flag bits set: 0x%04X", flags);
        return out_error->result;
    }
    int role_count = ((flags & PRINTERCTL_PLP1_FLAG_REQUEST) != 0) +
                      ((flags & PRINTERCTL_PLP1_FLAG_RESPONSE) != 0) +
                      ((flags & PRINTERCTL_PLP1_FLAG_EVENT) != 0);
    if (role_count != 1) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_FLAGS,
                  "exactly one of REQUEST/RESPONSE/EVENT must be set, found %d", role_count);
        return out_error->result;
    }
    bool has_payload_flag = (flags & PRINTERCTL_PLP1_FLAG_HAS_PAYLOAD) != 0;
    bool has_payload_actual = payload_length > 0;
    if (has_payload_flag != has_payload_actual) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_FLAGS,
                  "HAS_PAYLOAD flag (%d) inconsistent with an actual payload of %u byte(s)",
                  (int)has_payload_flag, payload_length);
        return out_error->result;
    }
    if ((flags & (PRINTERCTL_PLP1_FLAG_REQUEST | PRINTERCTL_PLP1_FLAG_RESPONSE)) &&
        request_id == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_MALFORMED_FRAME,
                  "request_id 0 is reserved for uncorrelated messages; a REQUEST/RESPONSE must "
                  "use 1..UINT32_MAX");
        return out_error->result;
    }
    if (flags & PRINTERCTL_PLP1_FLAG_REQUEST) {
        if (status != PRINTERCTL_PLP1_SUCCESS) {
            set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_MALFORMED_FRAME,
                      "a request's status must be SUCCESS, got 0x%04X", status);
            return out_error->result;
        }
    }
    if (flags & PRINTERCTL_PLP1_FLAG_RESPONSE) {
        bool has_error_flag = (flags & PRINTERCTL_PLP1_FLAG_ERROR) != 0;
        bool status_is_error = status != PRINTERCTL_PLP1_SUCCESS;
        if (has_error_flag != status_is_error) {
            set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_MALFORMED_FRAME,
                      "ERROR flag (%d) inconsistent with status 0x%04X", (int)has_error_flag,
                      status);
            return out_error->result;
        }
    }
    if (!message_type_known(message_type)) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_UNKNOWN_MESSAGE_TYPE,
                  "unknown message type 0x%02X", message_type);
        return out_error->result;
    }
    if ((flags & message_type_allowed_roles(message_type)) == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_FLAGS,
                  "message type 0x%02X does not support the role flags 0x%04X", message_type,
                  flags);
        return out_error->result;
    }
    plp1_payload_requirement_t payload_requirement =
        message_role_payload_requirement(message_type, flags);
    if (payload_requirement == PLP1_PAYLOAD_FORBIDDEN && payload_length != 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "message type 0x%02X (flags 0x%04X) does not accept a payload, got %u byte(s)",
                  message_type, flags, payload_length);
        return out_error->result;
    }
    if (payload_requirement == PLP1_PAYLOAD_REQUIRED && payload_length == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "message type 0x%02X (flags 0x%04X) requires a payload, got none", message_type,
                  flags);
        return out_error->result;
    }

    out_frame->version = version;
    out_frame->message_type = message_type;
    out_frame->flags = flags;
    out_frame->request_id = request_id;
    out_frame->status = status;
    out_frame->payload = payload_length > 0 ? buffer + PRINTERCTL_PLP1_HEADER_SIZE : NULL;
    out_frame->payload_length = payload_length;

    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_expect_no_payload(
    const printerctl_plp1_frame_t *frame, printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (frame == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "frame must not be NULL");
        return out_error->result;
    }
    if (frame->payload_length != 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "message type 0x%02X does not accept a payload (got %u byte(s))",
                  frame->message_type, frame->payload_length);
        return out_error->result;
    }
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_string(
    const char *value, uint8_t *out_buffer, size_t out_buffer_size,
    size_t *out_length, printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (value == NULL || out_buffer == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "value, out_buffer and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;

    size_t value_length = strlen(value);
    if (value_length > 0xFFFFu) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "string of %zu byte(s) does not fit a uint16 length prefix", value_length);
        return out_error->result;
    }

    size_t total = 2 + value_length;
    if (out_buffer_size < total) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_buffer_size %zu is smaller than the %zu bytes this string needs",
                  out_buffer_size, total);
        return out_error->result;
    }

    write_u16be(out_buffer, (uint16_t)value_length);
    if (value_length > 0) {
        memcpy(out_buffer + 2, value, value_length);
    }
    *out_length = total;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_string(
    const uint8_t *buffer, size_t buffer_length,
    char *out_value, size_t out_value_size, size_t *out_consumed,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    /* buffer may be NULL when buffer_length is 0 -- that is a
     * legitimate "no bytes available" payload (e.g. a required string
     * field that was omitted entirely), not caller misuse; it falls
     * through to the length check below and is reported as
     * INVALID_PAYLOAD, not ERR_INVALID_ARGUMENT. */
    if ((buffer == NULL && buffer_length > 0) || out_value == NULL || out_consumed == NULL ||
        out_value_size == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_value and out_consumed must not be NULL, out_value_size must be > 0, and "
                  "buffer must not be NULL when buffer_length > 0");
        return out_error->result;
    }
    out_value[0] = '\0';
    *out_consumed = 0;

    if (buffer_length < 2) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "truncated string length prefix (%zu byte(s) available)", buffer_length);
        return out_error->result;
    }

    uint16_t length = read_u16be(buffer);
    if (buffer_length < 2u + (size_t)length) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "declared string length %u exceeds the %zu byte(s) available", length,
                  buffer_length - 2);
        return out_error->result;
    }
    if ((size_t)length >= out_value_size) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "string of %u byte(s) does not fit the %zu-byte destination buffer", length,
                  out_value_size);
        return out_error->result;
    }

    if (length > 0) {
        memcpy(out_value, buffer + 2, length);
    }
    out_value[length] = '\0';
    *out_consumed = 2u + (size_t)length;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_version_response(
    const printerctl_plp1_version_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (value == NULL || out_payload == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "value, out_payload and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;

    const size_t total = 1 + 4;
    if (out_payload_size < total) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_payload_size %zu is smaller than the %zu bytes this payload needs",
                  out_payload_size, total);
        return out_error->result;
    }

    out_payload[0] = value->protocol_version;
    write_u32be(out_payload + 1, value->capability_bitmap);
    *out_length = total;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_version_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_version_response_t *out_value,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if ((payload == NULL && payload_length > 0) || out_value == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_value must not be NULL, and payload must not be NULL when payload_length > 0");
        return out_error->result;
    }
    memset(out_value, 0, sizeof(*out_value));

    if (payload_length != 1 + 4) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "VERSION response payload of %zu byte(s), expected exactly 5", payload_length);
        return out_error->result;
    }

    out_value->protocol_version = payload[0];
    out_value->capability_bitmap = read_u32be(payload + 1);
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_status_request(
    uint32_t requested_fields,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (out_payload == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_payload and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;

    if (requested_fields & ~(uint32_t)PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "requested_fields has reserved bit(s) set: 0x%08X", requested_fields);
        return out_error->result;
    }

    if (out_payload_size < 4) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_payload_size %zu is smaller than the 4 bytes this payload needs",
                  out_payload_size);
        return out_error->result;
    }

    write_u32be(out_payload, requested_fields);
    *out_length = 4;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_status_request(
    const uint8_t *payload, size_t payload_length,
    uint32_t *out_requested_fields,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if ((payload == NULL && payload_length > 0) || out_requested_fields == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_requested_fields must not be NULL, and payload must not be NULL when "
                  "payload_length > 0");
        return out_error->result;
    }
    *out_requested_fields = 0;

    if (payload_length != 4) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "STATUS request payload of %zu byte(s), expected exactly 4", payload_length);
        return out_error->result;
    }

    uint32_t requested = read_u32be(payload);
    if (requested & ~(uint32_t)PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "STATUS request requested_fields has reserved bit(s) set: 0x%08X", requested);
        return out_error->result;
    }

    *out_requested_fields = requested;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_status_response(
    const printerctl_plp1_status_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (value == NULL || out_payload == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "value, out_payload and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;

    uint32_t present = value->present_fields;
    if (present & ~(uint32_t)PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "present_fields has reserved bit(s) set: 0x%08X", present);
        return out_error->result;
    }
    size_t needed = 4;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE) needed += 1;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE) needed += 1;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE) needed += 1;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS) needed += 1;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_ELAPSED_TIME) needed += 4;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_SENT) needed += 4;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED) needed += 4;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_SOURCE_POSITION) needed += 4;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON) needed += 2;

    if (out_payload_size < needed) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_payload_size %zu is smaller than the %zu bytes this payload needs",
                  out_payload_size, needed);
        return out_error->result;
    }

    size_t offset = 0;
    write_u32be(out_payload + offset, present);
    offset += 4;
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE) {
        out_payload[offset++] = value->daemon_state;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE) {
        out_payload[offset++] = value->worker_state;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE) {
        out_payload[offset++] = value->execution_state;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS) {
        out_payload[offset++] = value->progress;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_ELAPSED_TIME) {
        write_u32be(out_payload + offset, value->elapsed_time);
        offset += 4;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_SENT) {
        write_u32be(out_payload + offset, value->commands_sent);
        offset += 4;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED) {
        write_u32be(out_payload + offset, value->commands_acknowledged);
        offset += 4;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_SOURCE_POSITION) {
        write_u32be(out_payload + offset, value->source_position);
        offset += 4;
    }
    if (present & PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON) {
        write_u16be(out_payload + offset, value->failure_reason);
        offset += 2;
    }

    *out_length = offset;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_status_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_status_response_t *out_value,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if ((payload == NULL && payload_length > 0) || out_value == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_value must not be NULL, and payload must not be NULL when payload_length > 0");
        return out_error->result;
    }
    memset(out_value, 0, sizeof(*out_value));

    if (payload_length < 4) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "STATUS response payload of %zu byte(s) is shorter than the required 4",
                  payload_length);
        return out_error->result;
    }

    uint32_t present = read_u32be(payload);
    if (present & ~(uint32_t)PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "STATUS response present_fields has reserved bit(s) set: 0x%08X", present);
        return out_error->result;
    }
    size_t offset = 4;

#define PLP1_STATUS_READ_U8(FIELD_BIT, FIELD)                                                  \
    if (present & (FIELD_BIT)) {                                                               \
        if (payload_length < offset + 1) {                                                     \
            set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD, \
                      "STATUS response payload too short for its present_fields bitmap");       \
            memset(out_value, 0, sizeof(*out_value));                                           \
            return out_error->result;                                                          \
        }                                                                                        \
        out_value->FIELD = payload[offset];                                                    \
        offset += 1;                                                                            \
    }

#define PLP1_STATUS_READ_U16(FIELD_BIT, FIELD)                                                 \
    if (present & (FIELD_BIT)) {                                                               \
        if (payload_length < offset + 2) {                                                     \
            set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD, \
                      "STATUS response payload too short for its present_fields bitmap");       \
            memset(out_value, 0, sizeof(*out_value));                                           \
            return out_error->result;                                                          \
        }                                                                                        \
        out_value->FIELD = read_u16be(payload + offset);                                        \
        offset += 2;                                                                             \
    }

#define PLP1_STATUS_READ_U32(FIELD_BIT, FIELD)                                                 \
    if (present & (FIELD_BIT)) {                                                               \
        if (payload_length < offset + 4) {                                                     \
            set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD, \
                      "STATUS response payload too short for its present_fields bitmap");       \
            memset(out_value, 0, sizeof(*out_value));                                           \
            return out_error->result;                                                          \
        }                                                                                        \
        out_value->FIELD = read_u32be(payload + offset);                                         \
        offset += 4;                                                                             \
    }

    PLP1_STATUS_READ_U8(PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE, daemon_state)
    PLP1_STATUS_READ_U8(PRINTERCTL_PLP1_STATUS_FIELD_WORKER_STATE, worker_state)
    PLP1_STATUS_READ_U8(PRINTERCTL_PLP1_STATUS_FIELD_EXECUTION_STATE, execution_state)
    PLP1_STATUS_READ_U8(PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS, progress)
    PLP1_STATUS_READ_U32(PRINTERCTL_PLP1_STATUS_FIELD_ELAPSED_TIME, elapsed_time)
    PLP1_STATUS_READ_U32(PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_SENT, commands_sent)
    PLP1_STATUS_READ_U32(PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED, commands_acknowledged)
    PLP1_STATUS_READ_U32(PRINTERCTL_PLP1_STATUS_FIELD_SOURCE_POSITION, source_position)
    PLP1_STATUS_READ_U16(PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON, failure_reason)

#undef PLP1_STATUS_READ_U8
#undef PLP1_STATUS_READ_U16
#undef PLP1_STATUS_READ_U32

    if (offset != payload_length) {
        memset(out_value, 0, sizeof(*out_value));
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "STATUS response payload has %zu trailing byte(s) beyond what present_fields "
                  "declares",
                  payload_length - offset);
        return out_error->result;
    }

    out_value->present_fields = present;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_print_start_request(
    const char *source_path,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    return printerctl_plp1_encode_string(source_path, out_payload, out_payload_size, out_length,
                                          out_error);
}

printerctl_plp1_result_t printerctl_plp1_decode_print_start_request(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_print_start_request_t *out_value,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (out_value == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_value must not be NULL");
        return out_error->result;
    }
    memset(out_value, 0, sizeof(*out_value));

    size_t consumed = 0;
    printerctl_plp1_result_t status = printerctl_plp1_decode_string(
        payload, payload_length, out_value->source_path, sizeof(out_value->source_path),
        &consumed, out_error);
    if (status != PRINTERCTL_PLP1_OK) {
        return status;
    }

    if (consumed != payload_length) {
        memset(out_value, 0, sizeof(*out_value));
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "PRINT_START payload has %zu trailing byte(s) after the source_path string",
                  payload_length - consumed);
        return out_error->result;
    }

    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_query_temp_response(
    const printerctl_plp1_query_temp_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if (value == NULL || out_payload == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "value, out_payload and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;
    if (value->present_fields & ~(uint8_t)(PRINTERCTL_PLP1_QUERY_TEMP_FIELD_HOTEND |
                                           PRINTERCTL_PLP1_QUERY_TEMP_FIELD_BED)) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "TEMP response present_fields has reserved bits");
        return out_error->result;
    }
    const size_t needed = 1 + 4 + 4 + 4 + 4;
    if (out_payload_size < needed) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_payload_size %zu is smaller than %zu", out_payload_size, needed);
        return out_error->result;
    }
    out_payload[0] = value->present_fields;
    write_i32be(out_payload + 1, value->hotend_actual_milli_c);
    write_i32be(out_payload + 5, value->hotend_target_milli_c);
    write_i32be(out_payload + 9, value->bed_actual_milli_c);
    write_i32be(out_payload + 13, value->bed_target_milli_c);
    *out_length = needed;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_query_temp_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_query_temp_response_t *out_value,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if ((payload == NULL && payload_length > 0) || out_value == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_value must not be NULL, and payload must not be NULL when present");
        return out_error->result;
    }
    memset(out_value, 0, sizeof(*out_value));
    if (payload_length != 17) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "TEMP response payload of %zu byte(s), expected exactly 17", payload_length);
        return out_error->result;
    }
    if (payload[0] & ~(uint8_t)(PRINTERCTL_PLP1_QUERY_TEMP_FIELD_HOTEND |
                                PRINTERCTL_PLP1_QUERY_TEMP_FIELD_BED)) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "TEMP response present_fields has reserved bits");
        return out_error->result;
    }
    out_value->present_fields = payload[0];
    out_value->hotend_actual_milli_c = read_i32be(payload + 1);
    out_value->hotend_target_milli_c = read_i32be(payload + 5);
    out_value->bed_actual_milli_c = read_i32be(payload + 9);
    out_value->bed_target_milli_c = read_i32be(payload + 13);
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_query_position_response(
    const printerctl_plp1_query_position_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if (value == NULL || out_payload == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "value, out_payload and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;
    if (value->present_fields & ~(uint8_t)PRINTERCTL_PLP1_QUERY_POSITION_FIELD_E) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "POSITION response present_fields has reserved bits");
        return out_error->result;
    }
    const size_t needed = 1 + 4 + 4 + 4 + 4;
    if (out_payload_size < needed) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_payload_size %zu is smaller than %zu", out_payload_size, needed);
        return out_error->result;
    }
    out_payload[0] = value->present_fields;
    write_i32be(out_payload + 1, value->x_milli_mm);
    write_i32be(out_payload + 5, value->y_milli_mm);
    write_i32be(out_payload + 9, value->z_milli_mm);
    write_i32be(out_payload + 13, value->e_milli_mm);
    *out_length = needed;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_query_position_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_query_position_response_t *out_value,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if ((payload == NULL && payload_length > 0) || out_value == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_value must not be NULL, and payload must not be NULL when present");
        return out_error->result;
    }
    memset(out_value, 0, sizeof(*out_value));
    if (payload_length != 17) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "POSITION response payload of %zu byte(s), expected exactly 17", payload_length);
        return out_error->result;
    }
    if (payload[0] & ~(uint8_t)PRINTERCTL_PLP1_QUERY_POSITION_FIELD_E) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "POSITION response present_fields has reserved bits");
        return out_error->result;
    }
    out_value->present_fields = payload[0];
    out_value->x_milli_mm = read_i32be(payload + 1);
    out_value->y_milli_mm = read_i32be(payload + 5);
    out_value->z_milli_mm = read_i32be(payload + 9);
    out_value->e_milli_mm = read_i32be(payload + 13);
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_query_info_response(
    const printerctl_plp1_query_info_response_t *value,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if (value == NULL || out_payload == NULL || out_length == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "value, out_payload and out_length must not be NULL");
        return out_error->result;
    }
    *out_length = 0;

    const char *fields[] = {value->firmware_name, value->machine_type,
                            value->protocol_version, value->uuid};
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t written = 0;
        printerctl_plp1_result_t status = printerctl_plp1_encode_string(
            fields[i], out_payload + offset, out_payload_size - offset, &written, out_error);
        if (status != PRINTERCTL_PLP1_OK) {
            *out_length = 0;
            return status;
        }
        offset += written;
    }
    if (out_payload_size < offset + 3) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_BUFFER_TOO_SMALL, PRINTERCTL_PLP1_SUCCESS,
                  "out_payload_size %zu is too small for INFO tail", out_payload_size);
        *out_length = 0;
        return out_error->result;
    }
    out_payload[offset++] = value->has_extruder_count ? 1u : 0u;
    write_u16be(out_payload + offset, value->extruder_count);
    offset += 2;
    *out_length = offset;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_decode_query_info_response(
    const uint8_t *payload, size_t payload_length,
    printerctl_plp1_query_info_response_t *out_value,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if ((payload == NULL && payload_length > 0) || out_value == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_value must not be NULL, and payload must not be NULL when present");
        return out_error->result;
    }
    memset(out_value, 0, sizeof(*out_value));

    char *fields[] = {out_value->firmware_name, out_value->machine_type,
                      out_value->protocol_version, out_value->uuid};
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t consumed = 0;
        printerctl_plp1_result_t status = printerctl_plp1_decode_string(
            payload + offset, payload_length - offset, fields[i],
            sizeof(out_value->firmware_name), &consumed, out_error);
        if (status != PRINTERCTL_PLP1_OK) {
            memset(out_value, 0, sizeof(*out_value));
            return status;
        }
        offset += consumed;
    }
    if (payload_length != offset + 3) {
        memset(out_value, 0, sizeof(*out_value));
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "INFO response payload has invalid length");
        return out_error->result;
    }
    if (payload[offset] > 1u) {
        memset(out_value, 0, sizeof(*out_value));
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "INFO response has invalid extruder_count presence flag");
        return out_error->result;
    }
    out_value->has_extruder_count = payload[offset++] != 0;
    out_value->extruder_count = read_u16be(payload + offset);
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_result_t printerctl_plp1_encode_error_payload(
    const char *message,
    uint8_t *out_payload, size_t out_payload_size, size_t *out_length,
    printerctl_plp1_error_t *out_error)
{
    return printerctl_plp1_encode_string(message, out_payload, out_payload_size, out_length,
                                          out_error);
}

printerctl_plp1_result_t printerctl_plp1_decode_error_payload(
    const uint8_t *payload, size_t payload_length,
    char *out_message, size_t out_message_size,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if ((payload == NULL && payload_length > 0) || out_message == NULL ||
        out_message_size == 0) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "out_message must not be NULL, out_message_size must be > 0, and payload must "
                  "not be NULL when payload_length > 0");
        return out_error->result;
    }
    out_message[0] = '\0';

    if (payload_length < 2) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "truncated error message length prefix (%zu byte(s) available)", payload_length);
        return out_error->result;
    }

    uint16_t length = read_u16be(payload);
    if (payload_length != 2u + (size_t)length) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_MALFORMED, PRINTERCTL_PLP1_INVALID_PAYLOAD,
                  "declared error message length %u does not match the %zu byte(s) available "
                  "(too few, or trailing bytes present)",
                  length, payload_length - 2);
        return out_error->result;
    }

    /* Informative text only: truncate into the caller's buffer rather
     * than erroring, unlike printerctl_plp1_decode_string()'s strict
     * "must fit" contract used for identifiers like PRINT_START's path. */
    size_t copy_length = length;
    if (copy_length >= out_message_size) {
        copy_length = out_message_size - 1;
    }
    if (copy_length > 0) {
        memcpy(out_message, payload + 2, copy_length);
    }
    out_message[copy_length] = '\0';
    return PRINTERCTL_PLP1_OK;
}
