#include "printerctl/protocol/plp1_parser.h"

#include <stdarg.h>
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

void printerctl_plp1_parser_init(
    printerctl_plp1_parser_t *parser, uint8_t *buffer, size_t buffer_capacity)
{
    parser->buffer = buffer;
    parser->buffer_capacity = buffer_capacity;
    parser->buffered_length = 0;
    parser->consumed_offset = 0;
}

printerctl_plp1_result_t printerctl_plp1_parser_feed(
    printerctl_plp1_parser_t *parser, const uint8_t *data, size_t length,
    size_t *out_consumed, printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (parser == NULL || out_consumed == NULL || (data == NULL && length > 0)) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "parser and out_consumed must not be NULL, and data must not be NULL when "
                  "length > 0");
        return out_error->result;
    }
    *out_consumed = 0;

    /* Compact any unconsumed tail to the front before appending, so a
     * borrowed frame from the *previous* take_frame() call is only
     * ever invalidated here -- never inside take_frame() itself, which
     * is the documented contract. */
    if (parser->consumed_offset > 0) {
        size_t remaining = parser->buffered_length - parser->consumed_offset;
        if (remaining > 0) {
            memmove(parser->buffer, parser->buffer + parser->consumed_offset, remaining);
        }
        parser->buffered_length = remaining;
        parser->consumed_offset = 0;
    }

    size_t space = parser->buffer_capacity - parser->buffered_length;
    size_t take = length < space ? length : space;
    if (take > 0) {
        memcpy(parser->buffer + parser->buffered_length, data, take);
        parser->buffered_length += take;
    }
    *out_consumed = take;
    return PRINTERCTL_PLP1_OK;
}

printerctl_plp1_parser_take_status_t printerctl_plp1_parser_take_frame(
    printerctl_plp1_parser_t *parser, printerctl_plp1_frame_t *out_frame,
    printerctl_plp1_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PLP1_PARSER_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));

    if (parser == NULL || out_frame == NULL) {
        set_error(out_error, PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT, PRINTERCTL_PLP1_SUCCESS,
                  "parser and out_frame must not be NULL");
        return PRINTERCTL_PLP1_PARSER_ERR_INVALID_ARGUMENT;
    }
    memset(out_frame, 0, sizeof(*out_frame));

    const uint8_t *available = parser->buffer + parser->consumed_offset;
    size_t available_length = parser->buffered_length - parser->consumed_offset;

    if (available_length < PRINTERCTL_PLP1_FRAME_PREFIX_SIZE) {
        return PRINTERCTL_PLP1_PARSER_NEED_MORE;
    }

    uint32_t frame_length = ((uint32_t)available[0] << 24) | ((uint32_t)available[1] << 16) |
                             ((uint32_t)available[2] << 8) | (uint32_t)available[3];

    printerctl_plp1_result_t length_status =
        printerctl_plp1_validate_frame_length(frame_length, out_error);
    if (length_status != PRINTERCTL_PLP1_OK) {
        /* The stream is no longer trustworthy for framing -- do not
         * advance consumed_offset; there is nothing sane left to skip
         * to a "next frame" with an untrustworthy length. */
        return PRINTERCTL_PLP1_PARSER_ERR_MALFORMED;
    }

    size_t total_frame_size = (size_t)PRINTERCTL_PLP1_FRAME_PREFIX_SIZE + (size_t)frame_length;
    if (available_length < total_frame_size) {
        return PRINTERCTL_PLP1_PARSER_NEED_MORE;
    }

    printerctl_plp1_result_t decode_status =
        printerctl_plp1_decode_frame(available, total_frame_size, out_frame, out_error);
    parser->consumed_offset += total_frame_size;
    if (decode_status != PRINTERCTL_PLP1_OK) {
        /* A semantic failure (unknown type, bad flags, ...): the frame
         * boundary is trusted, so it is consumed either way -- the
         * caller can keep parsing the next frame even though this one
         * was rejected, matching the task's structural-vs-semantic
         * split. out_error (already filled by decode_frame()) carries
         * the specific wire_status. */
        memset(out_frame, 0, sizeof(*out_frame));
        return PRINTERCTL_PLP1_PARSER_FRAME_REJECTED;
    }

    return PRINTERCTL_PLP1_PARSER_FRAME_READY;
}
