/*
 * Milestone 14 header-level tests for PLP/1: frame_length bounds,
 * version, reserved field, flags validation, status/ERROR-flag
 * consistency, message type registry, and byte-exact golden vectors.
 * Pure in-memory buffers -- no socket, no fd, no thread.
 */
#include "printerctl/protocol/plp1.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        g_failures++;
    }
}

static void check_bytes(const uint8_t *actual, const uint8_t *expected, size_t length,
                         const char *description)
{
    check(memcmp(actual, expected, length) == 0, description);
}

/* Builds a minimal valid PING request frame with the given request_id. */
static printerctl_plp1_frame_t make_ping_request(uint32_t request_id)
{
    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_PING;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = request_id;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    return frame;
}

/* ---- Golden vectors (exact bytes from the task, not round-trip-only) ---- */

static void test_golden_ping_request(void)
{
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x01,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00,
        0x00, 0x00,
    };

    printerctl_plp1_frame_t frame = make_ping_request(42);
    uint8_t buffer[64];
    size_t length = 0;
    printerctl_plp1_error_t error;

    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding a PING request succeeds");
    check(length == sizeof(expected), "the encoded PING request has the expected length");
    check_bytes(buffer, expected, sizeof(expected), "the encoded PING request matches the golden bytes exactly");

    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(expected, sizeof(expected), &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding the golden PING request bytes succeeds");
    check(decoded.version == 1 && decoded.message_type == PRINTERCTL_PLP1_MSG_PING &&
              decoded.flags == PRINTERCTL_PLP1_FLAG_REQUEST && decoded.request_id == 42 &&
              decoded.status == PRINTERCTL_PLP1_SUCCESS && decoded.payload_length == 0,
          "decoding the golden PING request recovers the exact field values");
}

static void test_golden_ping_response(void)
{
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x01,
        0x00, 0x02,
        0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00,
        0x00, 0x00,
    };

    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_PING;
    frame.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
    frame.request_id = 42;
    frame.status = PRINTERCTL_PLP1_SUCCESS;

    uint8_t buffer[64];
    size_t length = 0;
    printerctl_plp1_error_t error;

    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding a PING response succeeds");
    check(length == sizeof(expected), "the encoded PING response has the expected length");
    check_bytes(buffer, expected, sizeof(expected), "the encoded PING response matches the golden bytes exactly");

    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(expected, sizeof(expected), &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding the golden PING response bytes succeeds");
    check(decoded.flags == PRINTERCTL_PLP1_FLAG_RESPONSE && decoded.request_id == 42,
          "decoding the golden PING response recovers the exact field values");
}

static void test_golden_query_temp_request(void)
{
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x30,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00,
        0x00, 0x00,
    };

    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_QUERY_TEMP;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = 42;
    frame.status = PRINTERCTL_PLP1_SUCCESS;

    uint8_t buffer[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding a QUERY_TEMP request succeeds");
    check(length == sizeof(expected), "the encoded QUERY_TEMP request has the expected length");
    check_bytes(buffer, expected, sizeof(expected),
                "the encoded QUERY_TEMP request matches the golden bytes exactly");
}

static void test_golden_raw_send_request(void)
{
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x12,
        0x01,
        0x33,
        0x00, 0x09,
        0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x04,
        'M', '1', '1', '5'
    };

    uint8_t payload[16];
    size_t payload_length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_raw_send_request("M115", payload, sizeof(payload),
                                                  &payload_length, &error) == PRINTERCTL_PLP1_OK,
          "encoding RAW_SEND payload succeeds");

    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_RAW_SEND;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = 42;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;

    uint8_t buffer[64];
    size_t length = 0;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding RAW_SEND request succeeds");
    check_bytes(buffer, expected, sizeof(expected),
                "RAW_SEND request matches the golden bytes exactly");
}

static void test_golden_home_x_request(void)
{
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x0D,
        0x01,
        0x34,
        0x00, 0x09,
        0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00,
        0x00, 0x00,
        0x01
    };

    uint8_t payload[1];
    size_t payload_length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_home_request(PRINTERCTL_PLP1_HOME_X, payload, sizeof(payload),
                                              &payload_length, &error) == PRINTERCTL_PLP1_OK,
          "encoding HOME payload succeeds");

    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_HOME;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = 42;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;

    uint8_t buffer[64];
    size_t length = 0;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding HOME request succeeds");
    check_bytes(buffer, expected, sizeof(expected),
                "HOME X request matches the golden bytes exactly");
}

static void test_golden_print_start_request(void)
{
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x1A,
        0x01,
        0x20,
        0x00, 0x09,
        0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x0C,
        '/', 't', 'm', 'p', '/', 'a', '.', 'g', 'c', 'o', 'd', 'e',
    };

    uint8_t payload[32];
    size_t payload_length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_print_start_request("/tmp/a.gcode", payload, sizeof(payload),
                                                       &payload_length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding the PRINT_START payload succeeds");
    check(payload_length == 14, "the PRINT_START payload is exactly 14 bytes");

    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_PRINT_START;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST; /* HAS_PAYLOAD is auto-added by encode_frame() */
    frame.request_id = 42;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;

    uint8_t buffer[64];
    size_t length = 0;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding the full PRINT_START frame succeeds");
    check(length == sizeof(expected), "the encoded PRINT_START frame has the expected length (26 + 4)");
    check_bytes(buffer, expected, sizeof(expected), "the encoded PRINT_START frame matches the golden bytes exactly");

    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(expected, sizeof(expected), &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding the golden PRINT_START bytes succeeds");
    check(decoded.flags == (PRINTERCTL_PLP1_FLAG_REQUEST | PRINTERCTL_PLP1_FLAG_HAS_PAYLOAD),
          "decoding recovers REQUEST | HAS_PAYLOAD");
    check(decoded.payload_length == 14 && decoded.payload != NULL,
          "decoding recovers the 14-byte payload");

    printerctl_plp1_print_start_request_t start;
    check(printerctl_plp1_decode_print_start_request(decoded.payload, decoded.payload_length,
                                                       &start, &error) == PRINTERCTL_PLP1_OK,
          "decoding the PRINT_START payload succeeds");
    check(strcmp(start.source_path, "/tmp/a.gcode") == 0,
          "decoding the PRINT_START payload recovers the exact source path");
}

/* ---- frame_length bounds ---- */

static void test_frame_length_bounds(void)
{
    printerctl_plp1_error_t error;

    check(printerctl_plp1_validate_frame_length(PRINTERCTL_PLP1_MIN_FRAME_LENGTH, &error) ==
              PRINTERCTL_PLP1_OK,
          "the minimum frame_length (12) is valid");
    check(printerctl_plp1_validate_frame_length(PRINTERCTL_PLP1_MAX_FRAME_LENGTH, &error) ==
              PRINTERCTL_PLP1_OK,
          "the maximum frame_length (65536) is valid");

    check(printerctl_plp1_validate_frame_length(PRINTERCTL_PLP1_MIN_FRAME_LENGTH - 1, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_MALFORMED_FRAME,
          "a frame_length of 11 is rejected as MALFORMED_FRAME");
    check(printerctl_plp1_validate_frame_length(PRINTERCTL_PLP1_MAX_FRAME_LENGTH + 1, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_FRAME_TOO_LARGE,
          "a frame_length of 65537 is rejected as FRAME_TOO_LARGE");

    /* An "empty" frame -- no payload -- is exactly the minimum length. */
    printerctl_plp1_frame_t frame = make_ping_request(1);
    uint8_t buffer[64];
    size_t length = 0;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
                  PRINTERCTL_PLP1_OK &&
              length == PRINTERCTL_PLP1_HEADER_SIZE,
          "a payload-less frame encodes to exactly the 16-byte header (the minimum valid frame)");

    /* A too-short buffer for decode_frame() itself. */
    printerctl_plp1_frame_t decoded;
    uint8_t short_buffer[3] = {0, 0, 0};
    check(printerctl_plp1_decode_frame(short_buffer, sizeof(short_buffer), &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_MALFORMED_FRAME,
          "a buffer shorter than the frame_length prefix is rejected as MALFORMED_FRAME");
}

/* ---- version ---- */

static void test_version(void)
{
    printerctl_plp1_error_t error;
    uint8_t buffer[64];
    size_t length;

    printerctl_plp1_frame_t frame = make_ping_request(1);
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "version 1 encodes successfully");

    frame.version = 2;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects an unsupported version as caller misuse");

    static const uint8_t unsupported_version_frame[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x02, /* version 2 */
        0x01,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
    };
    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(unsupported_version_frame,
                                        sizeof(unsupported_version_frame), &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_UNSUPPORTED_VERSION,
          "decoding a version-2 frame is rejected as UNSUPPORTED_VERSION");
}

/* ---- reserved field ---- */

static void test_reserved_nonzero(void)
{
    static const uint8_t frame_with_reserved[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x01,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x01, /* reserved != 0 */
    };
    printerctl_plp1_frame_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_frame(frame_with_reserved, sizeof(frame_with_reserved), &decoded,
                                        &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_MALFORMED_FRAME,
          "a non-zero reserved field is rejected as MALFORMED_FRAME");
}

/* ---- flags ---- */

static void test_flags(void)
{
    printerctl_plp1_error_t error;
    printerctl_plp1_frame_t decoded;
    uint8_t buffer[64];
    size_t length;

    /* Valid: exactly one of REQUEST/RESPONSE/EVENT. */
    printerctl_plp1_frame_t request = make_ping_request(1);
    check(printerctl_plp1_encode_frame(&request, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "REQUEST alone is a valid flag combination");

    printerctl_plp1_frame_t response = request;
    response.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
    check(printerctl_plp1_encode_frame(&response, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "RESPONSE alone is a valid flag combination");

    /* EVENT is structurally a valid *flags* combination (exactly one
     * role bit), but no Phase 1 message type is an event-type message
     * (see test_message_type_role_combinations() below) -- PING is a
     * request/response ping, so EVENT|PING is rejected for message-type
     * reasons, not a flags-shape reason. */
    printerctl_plp1_frame_t event = request;
    event.flags = PRINTERCTL_PLP1_FLAG_EVENT;
    check(printerctl_plp1_encode_frame(&event, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "EVENT is a structurally valid role bit, but PING does not support it "
          "(no Phase 1 message type is an event type yet)");

    /* Invalid: none of REQUEST/RESPONSE/EVENT. */
    printerctl_plp1_frame_t none = request;
    none.flags = 0;
    check(printerctl_plp1_encode_frame(&none, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects a frame with none of REQUEST/RESPONSE/EVENT set");

    /* Invalid: more than one of REQUEST/RESPONSE/EVENT. */
    printerctl_plp1_frame_t both = request;
    both.flags = PRINTERCTL_PLP1_FLAG_REQUEST | PRINTERCTL_PLP1_FLAG_RESPONSE;
    check(printerctl_plp1_encode_frame(&both, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects a frame with more than one of REQUEST/RESPONSE/EVENT set");

    /* Invalid: unknown flag bit, at decode time. */
    static const uint8_t unknown_flag_frame[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x01,
        0x01, 0x01, /* REQUEST | an unknown high bit */
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
    };
    check(printerctl_plp1_decode_frame(unknown_flag_frame, sizeof(unknown_flag_frame), &decoded,
                                        &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_FLAGS,
          "decoding a frame with an unknown flag bit set is rejected as INVALID_FLAGS");

    /* Invalid: none of REQUEST/RESPONSE/EVENT, at decode time. */
    static const uint8_t no_role_frame[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x01,
        0x00, 0x08, /* HAS_PAYLOAD only, no REQUEST/RESPONSE/EVENT */
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
    };
    check(printerctl_plp1_decode_frame(no_role_frame, sizeof(no_role_frame), &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_FLAGS,
          "decoding a frame with none of REQUEST/RESPONSE/EVENT set is rejected as INVALID_FLAGS");
}

/* ---- message type / role combinations ---- */

static void test_message_type_role_combinations(void)
{
    static const uint8_t known_types[] = {
        PRINTERCTL_PLP1_MSG_PING,         PRINTERCTL_PLP1_MSG_VERSION,
        PRINTERCTL_PLP1_MSG_STATUS,       PRINTERCTL_PLP1_MSG_PRINT_START,
        PRINTERCTL_PLP1_MSG_PRINT_PAUSE,  PRINTERCTL_PLP1_MSG_PRINT_RESUME,
        PRINTERCTL_PLP1_MSG_PRINT_CANCEL,
    };

    uint8_t buffer[64];
    size_t length;
    printerctl_plp1_error_t error;
    printerctl_plp1_frame_t decoded;
    static uint8_t dummy_payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};

    for (size_t i = 0; i < sizeof(known_types) / sizeof(known_types[0]); i++) {
        uint8_t type = known_types[i];

        /* This loop is isolating "is this role valid for this message
         * type", not payload shape/presence (that is
         * test_mandatory_payload_validation() below) -- so each frame
         * here is given whatever payload its (type, role) combination
         * actually requires, matching plp1.c's own
         * message_role_payload_requirement() table, so a payload
         * mismatch never masks the role/type result this test cares
         * about. */
        bool request_needs_payload =
            type == PRINTERCTL_PLP1_MSG_STATUS || type == PRINTERCTL_PLP1_MSG_PRINT_START;
        bool response_needs_payload =
            type == PRINTERCTL_PLP1_MSG_STATUS || type == PRINTERCTL_PLP1_MSG_VERSION;

        /* REQUEST and RESPONSE are valid for every Phase 1 message type. */
        printerctl_plp1_frame_t req = make_ping_request(1);
        req.message_type = type;
        if (request_needs_payload) {
            req.payload = dummy_payload;
            req.payload_length = sizeof(dummy_payload);
        }
        check(printerctl_plp1_encode_frame(&req, buffer, sizeof(buffer), &length, &error) ==
                  PRINTERCTL_PLP1_OK,
              "REQUEST is valid for every Phase 1 message type");

        printerctl_plp1_frame_t resp = req;
        resp.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
        resp.payload = response_needs_payload ? dummy_payload : NULL;
        resp.payload_length = response_needs_payload ? sizeof(dummy_payload) : 0;
        check(printerctl_plp1_encode_frame(&resp, buffer, sizeof(buffer), &length, &error) ==
                  PRINTERCTL_PLP1_OK,
              "RESPONSE is valid for every Phase 1 message type");

        /* EVENT is not valid for any Phase 1 message type (none of them
         * is an event-family message yet) -- rejected for the role/type
         * reason regardless of payload, since that check runs before
         * the payload-requirement check in decode_frame()/encode_frame(). */
        printerctl_plp1_frame_t event = req;
        event.flags = PRINTERCTL_PLP1_FLAG_EVENT;
        check(printerctl_plp1_encode_frame(&event, buffer, sizeof(buffer), &length, &error) ==
                  PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
              "encode_frame() rejects EVENT for every currently-defined message type");

        /* Same check at decode time, byte level: build an EVENT|type
         * frame by hand (bypassing encode_frame()'s own validation) and
         * confirm the decoder independently rejects it. */
        uint8_t raw[16] = {
            0x00, 0x00, 0x00, 0x0C,
            0x01,
            type,
            0x00, 0x10, /* EVENT */
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00,
            0x00, 0x00,
        };
        check(printerctl_plp1_decode_frame(raw, sizeof(raw), &decoded, &error) ==
                      PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_FLAGS,
              "decode_frame() independently rejects EVENT for every currently-defined message "
              "type, as INVALID_FLAGS");
    }
}

/* ---- request_id ---- */

static void test_request_id_zero_rejected(void)
{
    /* The protocol document reserves request_id 0 for uncorrelated
     * messages (principally events) and requires clients to use
     * 1..UINT32_MAX for REQUEST/RESPONSE frames. A REQUEST or RESPONSE
     * carrying request_id 0 is therefore rejected, at both encode
     * (caller misuse) and decode (malformed wire content) time. */
    printerctl_plp1_frame_t request = make_ping_request(0);
    uint8_t buffer[64];
    size_t length;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_frame(&request, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects a REQUEST with request_id 0");

    printerctl_plp1_frame_t response = make_ping_request(0);
    response.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
    check(printerctl_plp1_encode_frame(&response, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects a RESPONSE with request_id 0");

    static const uint8_t request_id_zero_frame[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x01,
        0x00, 0x01, /* REQUEST */
        0x00, 0x00, 0x00, 0x00, /* request_id = 0 */
        0x00, 0x00,
        0x00, 0x00,
    };
    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(request_id_zero_frame, sizeof(request_id_zero_frame),
                                        &decoded, &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_MALFORMED_FRAME,
          "decode_frame() rejects a REQUEST with request_id 0 as MALFORMED_FRAME");

    /* EVENT is exempt from the request_id-0 rule (0 is exactly what it
     * is reserved for). No Phase 1 message type actually supports
     * EVENT yet (test_message_type_role_combinations()), so an
     * EVENT-flagged frame is always rejected regardless -- but the
     * *reason* must stay the role/type mismatch (INVALID_FLAGS), never
     * additionally/instead a request_id complaint. Comparing an EVENT
     * frame with request_id 0 against one with a non-zero request_id
     * and observing the identical error proves the request_id-0 check
     * truly does not fire for EVENT. */
    uint8_t event_nonzero_id[16] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        PRINTERCTL_PLP1_MSG_PING,
        0x00, 0x10, /* EVENT */
        0x00, 0x00, 0x00, 0x07, /* request_id = 7 */
        0x00, 0x00,
        0x00, 0x00,
    };
    uint8_t event_zero_id[16] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        PRINTERCTL_PLP1_MSG_PING,
        0x00, 0x10, /* EVENT */
        0x00, 0x00, 0x00, 0x00, /* request_id = 0 */
        0x00, 0x00,
        0x00, 0x00,
    };
    printerctl_plp1_error_t error_nonzero;
    printerctl_plp1_error_t error_zero;
    printerctl_plp1_result_t status_nonzero =
        printerctl_plp1_decode_frame(event_nonzero_id, sizeof(event_nonzero_id), &decoded,
                                      &error_nonzero);
    printerctl_plp1_result_t status_zero = printerctl_plp1_decode_frame(
        event_zero_id, sizeof(event_zero_id), &decoded, &error_zero);
    check(status_nonzero == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error_nonzero.wire_status == PRINTERCTL_PLP1_INVALID_FLAGS,
          "an EVENT frame with a non-zero request_id fails for the role/type mismatch");
    check(status_zero == status_nonzero && error_zero.wire_status == error_nonzero.wire_status,
          "the same EVENT frame with request_id 0 fails identically -- the request_id-0 rule "
          "does not additionally fire for EVENT, confirming the exemption");
}

/* ---- status / ERROR flag consistency ---- */

static void test_status_error_consistency(void)
{
    printerctl_plp1_error_t error;
    uint8_t buffer[64];
    size_t length;

    /* Invalid: request with non-SUCCESS status. */
    printerctl_plp1_frame_t bad_request = make_ping_request(1);
    bad_request.status = PRINTERCTL_PLP1_INTERNAL_ERROR;
    check(printerctl_plp1_encode_frame(&bad_request, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects a request whose status is not SUCCESS");

    /* Invalid: response with ERROR flag but SUCCESS status. */
    printerctl_plp1_frame_t bad_response = make_ping_request(1);
    bad_response.flags = PRINTERCTL_PLP1_FLAG_RESPONSE | PRINTERCTL_PLP1_FLAG_ERROR;
    bad_response.status = PRINTERCTL_PLP1_SUCCESS;
    check(printerctl_plp1_encode_frame(&bad_response, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects a response with ERROR set but status SUCCESS");

    /* Invalid: response with non-SUCCESS status but ERROR flag unset. */
    printerctl_plp1_frame_t bad_response2 = make_ping_request(1);
    bad_response2.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
    bad_response2.status = PRINTERCTL_PLP1_INTERNAL_ERROR;
    check(printerctl_plp1_encode_frame(&bad_response2, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects a response with non-SUCCESS status but ERROR unset");

    /* Valid: response with ERROR set and non-SUCCESS status. */
    printerctl_plp1_frame_t good_error_response = make_ping_request(1);
    good_error_response.flags = PRINTERCTL_PLP1_FLAG_RESPONSE | PRINTERCTL_PLP1_FLAG_ERROR;
    good_error_response.status = PRINTERCTL_PLP1_INTERNAL_ERROR;
    check(printerctl_plp1_encode_frame(&good_error_response, buffer, sizeof(buffer), &length,
                                        &error) == PRINTERCTL_PLP1_OK,
          "encode_frame() accepts a response with ERROR set and a matching non-SUCCESS status");

    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(buffer, length, &decoded, &error) == PRINTERCTL_PLP1_OK,
          "decoding that same error response succeeds");

    /* Same two decode-side checks, at the byte level. */
    static const uint8_t request_bad_status[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x01,
        0x00, 0x01, /* REQUEST */
        0x00, 0x00, 0x00, 0x01,
        0x03, 0x02, /* status = INTERNAL_ERROR (0x0302) */
        0x00, 0x00,
    };
    check(printerctl_plp1_decode_frame(request_bad_status, sizeof(request_bad_status), &decoded,
                                        &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_MALFORMED_FRAME,
          "decoding a request with non-SUCCESS status is rejected as MALFORMED_FRAME");
}

/* ---- HAS_PAYLOAD consistency ---- */

static void test_has_payload_consistency(void)
{
    printerctl_plp1_error_t error;
    uint8_t buffer[64];
    size_t length;

    /* encode-side: caller sets HAS_PAYLOAD by hand with no payload. */
    printerctl_plp1_frame_t frame = make_ping_request(1);
    frame.flags |= PRINTERCTL_PLP1_FLAG_HAS_PAYLOAD;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_frame() rejects HAS_PAYLOAD set with payload_length 0");

    /* decode-side: HAS_PAYLOAD set but frame_length implies no payload. */
    static const uint8_t has_payload_but_empty[] = {
        0x00, 0x00, 0x00, 0x0C, /* frame_length = 12 -> 0 payload bytes */
        0x01,
        0x01,
        0x00, 0x09, /* REQUEST | HAS_PAYLOAD */
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
    };
    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(has_payload_but_empty, sizeof(has_payload_but_empty),
                                        &decoded, &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_FLAGS,
          "decode_frame() rejects HAS_PAYLOAD set with an actually-empty payload");

    /* decode-side: HAS_PAYLOAD not set but the frame carries a payload. */
    static const uint8_t payload_but_flag_unset[] = {
        0x00, 0x00, 0x00, 0x0E, /* frame_length = 14 -> 2 payload bytes */
        0x01,
        0x01,
        0x00, 0x01, /* REQUEST only, no HAS_PAYLOAD */
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
        0xAB, 0xCD, /* 2 payload bytes present anyway */
    };
    check(printerctl_plp1_decode_frame(payload_but_flag_unset, sizeof(payload_but_flag_unset),
                                        &decoded, &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_FLAGS,
          "decode_frame() rejects an actual payload with HAS_PAYLOAD unset");
}

/* ---- unexpected payload on a no-payload message, at the wire level ---- */

/*
 * decode_frame()/encode_frame() enforce, structurally and
 * unconditionally, whether a given (message_type, role) combination
 * must carry a payload, must not, or may either way -- per
 * docs/design/protocol-v1.md's payload-requirement table. This is
 * mandatory validation on every real decode, not merely something a
 * caller opts into by remembering to call
 * printerctl_plp1_expect_no_payload() or a payload-specific decoder
 * afterward (test_expect_no_payload() below still covers that function
 * directly, for a caller constructing a frame_t by hand rather than
 * through decode_frame()).
 */
static void test_mandatory_payload_validation(void)
{
    uint8_t buffer[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_frame_t decoded;
    uint8_t dummy_payload[2] = {0xAA, 0xBB};

    /* FORBIDDEN: PING/PAUSE/RESUME/CANCEL never accept a payload,
     * enforced independent of expect_no_payload(). */
    static const uint8_t forbidden_types[] = {
        PRINTERCTL_PLP1_MSG_PING, PRINTERCTL_PLP1_MSG_PRINT_PAUSE,
        PRINTERCTL_PLP1_MSG_PRINT_RESUME, PRINTERCTL_PLP1_MSG_PRINT_CANCEL,
    };
    for (size_t i = 0; i < sizeof(forbidden_types) / sizeof(forbidden_types[0]); i++) {
        printerctl_plp1_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.version = PRINTERCTL_PLP1_VERSION_1;
        frame.message_type = forbidden_types[i];
        frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
        frame.request_id = 1;
        frame.status = PRINTERCTL_PLP1_SUCCESS;
        frame.payload = dummy_payload;
        frame.payload_length = sizeof(dummy_payload);

        check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
                  PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
              "encode_frame() rejects a payload on a message type that forbids one");

        /* Build the equivalent wire bytes directly (bypassing
         * encode_frame()'s own rejection above) to confirm decode_frame()
         * independently rejects it too, from real wire bytes. */
        uint8_t raw[18] = {
            0x00, 0x00, 0x00, 0x0E, /* frame_length = 14 -> 2 payload bytes */
            0x01,
            forbidden_types[i],
            0x00, 0x09, /* REQUEST | HAS_PAYLOAD */
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00,
            0x00, 0x00,
            0xAA, 0xBB,
        };
        check(printerctl_plp1_decode_frame(raw, sizeof(raw), &decoded, &error) ==
                      PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "decode_frame() rejects a payload on a message type that forbids one, from real "
              "wire bytes");
    }

    /* REQUIRED: VERSION response, STATUS request/response, PRINT_START
     * request never omit their payload. */
    {
        printerctl_plp1_frame_t version_resp;
        memset(&version_resp, 0, sizeof(version_resp));
        version_resp.version = PRINTERCTL_PLP1_VERSION_1;
        version_resp.message_type = PRINTERCTL_PLP1_MSG_VERSION;
        version_resp.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
        version_resp.request_id = 1;
        version_resp.status = PRINTERCTL_PLP1_SUCCESS;
        check(printerctl_plp1_encode_frame(&version_resp, buffer, sizeof(buffer), &length,
                                            &error) == PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
              "encode_frame() rejects a payload-less VERSION response");

        static const uint8_t version_resp_no_payload[16] = {
            0x00, 0x00, 0x00, 0x0C,
            0x01,
            PRINTERCTL_PLP1_MSG_VERSION,
            0x00, 0x02, /* RESPONSE */
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00,
            0x00, 0x00,
        };
        check(printerctl_plp1_decode_frame(version_resp_no_payload,
                                            sizeof(version_resp_no_payload), &decoded, &error) ==
                      PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "decode_frame() rejects a payload-less VERSION response from real wire bytes");
    }
    {
        static const uint8_t status_req_no_payload[16] = {
            0x00, 0x00, 0x00, 0x0C,
            0x01,
            PRINTERCTL_PLP1_MSG_STATUS,
            0x00, 0x01, /* REQUEST */
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00,
            0x00, 0x00,
        };
        check(printerctl_plp1_decode_frame(status_req_no_payload, sizeof(status_req_no_payload),
                                            &decoded, &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "decode_frame() rejects a payload-less STATUS request");
    }
    {
        /* PRINT_START request: no payload at all (not even the 2-byte
         * length prefix an empty string would still need) is rejected
         * at the frame level, before decode_print_start_request() is
         * ever called. */
        static const uint8_t print_start_no_payload[16] = {
            0x00, 0x00, 0x00, 0x0C,
            0x01,
            PRINTERCTL_PLP1_MSG_PRINT_START,
            0x00, 0x01, /* REQUEST */
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00,
            0x00, 0x00,
        };
        check(printerctl_plp1_decode_frame(print_start_no_payload, sizeof(print_start_no_payload),
                                            &decoded, &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "decode_frame() rejects a payload-less PRINT_START request");
    }
    {
        /* PRINT_START success response: a payload is forbidden. */
        printerctl_plp1_frame_t print_start_resp;
        memset(&print_start_resp, 0, sizeof(print_start_resp));
        print_start_resp.version = PRINTERCTL_PLP1_VERSION_1;
        print_start_resp.message_type = PRINTERCTL_PLP1_MSG_PRINT_START;
        print_start_resp.flags = PRINTERCTL_PLP1_FLAG_RESPONSE;
        print_start_resp.request_id = 1;
        print_start_resp.status = PRINTERCTL_PLP1_SUCCESS;
        print_start_resp.payload = dummy_payload;
        print_start_resp.payload_length = sizeof(dummy_payload);
        check(printerctl_plp1_encode_frame(&print_start_resp, buffer, sizeof(buffer), &length,
                                            &error) == PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
              "encode_frame() rejects a payload on a PRINT_START success response");
    }
    {
        static const uint8_t query_resp_no_payload[16] = {
            0x00, 0x00, 0x00, 0x0C,
            0x01,
            PRINTERCTL_PLP1_MSG_QUERY_TEMP,
            0x00, 0x02, /* RESPONSE */
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00,
            0x00, 0x00,
        };
        check(printerctl_plp1_decode_frame(query_resp_no_payload, sizeof(query_resp_no_payload),
                                            &decoded, &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "decode_frame() rejects a payload-less QUERY_TEMP response");
    }

    /* OPTIONAL override: an ERROR-flagged response of ANY message type
     * (including ones that otherwise forbid a payload, like PING) may
     * carry the generic error payload, or may not -- both are valid. */
    {
        printerctl_plp1_frame_t ping_error_with_payload;
        memset(&ping_error_with_payload, 0, sizeof(ping_error_with_payload));
        ping_error_with_payload.version = PRINTERCTL_PLP1_VERSION_1;
        ping_error_with_payload.message_type = PRINTERCTL_PLP1_MSG_PING;
        ping_error_with_payload.flags = PRINTERCTL_PLP1_FLAG_RESPONSE | PRINTERCTL_PLP1_FLAG_ERROR;
        ping_error_with_payload.request_id = 1;
        ping_error_with_payload.status = PRINTERCTL_PLP1_INTERNAL_ERROR;
        ping_error_with_payload.payload = dummy_payload;
        ping_error_with_payload.payload_length = sizeof(dummy_payload);
        check(printerctl_plp1_encode_frame(&ping_error_with_payload, buffer, sizeof(buffer),
                                            &length, &error) == PRINTERCTL_PLP1_OK,
              "an ERROR-flagged PING response may carry a payload (the generic error message), "
              "overriding PING's normal no-payload rule");
        check(printerctl_plp1_decode_frame(buffer, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding that ERROR-flagged PING response with a payload succeeds");

        printerctl_plp1_frame_t ping_error_without_payload = ping_error_with_payload;
        ping_error_without_payload.payload = NULL;
        ping_error_without_payload.payload_length = 0;
        check(printerctl_plp1_encode_frame(&ping_error_without_payload, buffer, sizeof(buffer),
                                            &length, &error) == PRINTERCTL_PLP1_OK,
              "an ERROR-flagged PING response may also carry no payload at all");
    }
}

/* ---- full max-size frame ---- */

static void test_max_size_frame(void)
{
    static uint8_t payload[PRINTERCTL_PLP1_MAX_FRAME_LENGTH - PRINTERCTL_PLP1_MIN_FRAME_LENGTH];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    /* PRINT_START, not PING: PING now structurally forbids a payload
     * (test_mandatory_payload_validation()), and PRINT_START's request
     * is exactly the message type that legitimately carries an
     * arbitrarily large (string) payload. decode_frame() only checks
     * presence/length here, not the string's own shape, so arbitrary
     * bytes are fine for exercising the frame-level size boundary. */
    frame.message_type = PRINTERCTL_PLP1_MSG_PRINT_START;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = 1;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);

    static uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "encoding a frame at exactly PRINTERCTL_PLP1_MAX_FRAME_LENGTH succeeds");
    check(length == PRINTERCTL_PLP1_MAX_FRAME_SIZE,
          "the encoded max-size frame is exactly PRINTERCTL_PLP1_MAX_FRAME_SIZE bytes");

    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(buffer, length, &decoded, &error) == PRINTERCTL_PLP1_OK,
          "decoding a frame at exactly PRINTERCTL_PLP1_MAX_FRAME_LENGTH succeeds");
    check(decoded.payload_length == sizeof(payload), "the decoded payload has the exact max length");
    check(memcmp(decoded.payload, payload, sizeof(payload)) == 0,
          "the decoded max-size payload matches exactly, byte for byte");

    /* One byte over the limit must still be rejected -- the boundary is
     * exact, not merely "large frames generally work". payload_length
     * is a uint16_t (max 65535), so a payload one byte over
     * MAX_FRAME_LENGTH - MIN_FRAME_LENGTH (65524, i.e. 65525) is still
     * representable in the type -- it is encode_frame()'s own bounds
     * check that must catch it, not the type system. */
    static uint8_t over_payload[PRINTERCTL_PLP1_MAX_FRAME_LENGTH - PRINTERCTL_PLP1_MIN_FRAME_LENGTH + 1];
    frame.payload = over_payload;
    frame.payload_length = (uint16_t)sizeof(over_payload);
    check(printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error) ==
              PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encoding a payload one byte over the max-frame boundary is rejected, not silently "
          "accepted or truncated");
}

/* ---- message type registry ---- */

static void test_unknown_message_type(void)
{
    static const uint8_t frame[] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x7F, /* message_type not in the Phase 1 registry */
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
    };
    printerctl_plp1_frame_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_frame(frame, sizeof(frame), &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_UNKNOWN_MESSAGE_TYPE,
          "decoding an unknown message type is rejected as UNKNOWN_MESSAGE_TYPE");
}

/* ---- expect_no_payload ---- */

static void test_expect_no_payload(void)
{
    printerctl_plp1_error_t error;
    printerctl_plp1_frame_t empty = make_ping_request(1);
    check(printerctl_plp1_expect_no_payload(&empty, &error) == PRINTERCTL_PLP1_OK,
          "a payload-less frame passes expect_no_payload()");

    uint8_t some_payload[4] = {1, 2, 3, 4};
    printerctl_plp1_frame_t with_payload = empty;
    with_payload.payload = some_payload;
    with_payload.payload_length = sizeof(some_payload);
    check(printerctl_plp1_expect_no_payload(&with_payload, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a frame carrying a payload fails expect_no_payload() with INVALID_PAYLOAD");
}

/* ---- buffer_length must match the declared frame exactly ---- */

static void test_buffer_length_mismatch(void)
{
    uint8_t buffer[64];
    size_t length;
    printerctl_plp1_error_t error;
    printerctl_plp1_frame_t frame = make_ping_request(1);
    printerctl_plp1_encode_frame(&frame, buffer, sizeof(buffer), &length, &error);

    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_decode_frame(buffer, length + 1, &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_MALFORMED_FRAME,
          "a buffer longer than the declared frame is rejected as MALFORMED_FRAME");
    check(printerctl_plp1_decode_frame(buffer, length - 1, &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_MALFORMED_FRAME,
          "a buffer shorter than the declared frame is rejected as MALFORMED_FRAME");
}

int main(void)
{
    test_golden_ping_request();
    test_golden_ping_response();
    test_golden_query_temp_request();
    test_golden_raw_send_request();
    test_golden_home_x_request();
    test_golden_print_start_request();
    test_frame_length_bounds();
    test_version();
    test_reserved_nonzero();
    test_flags();
    test_message_type_role_combinations();
    test_request_id_zero_rejected();
    test_status_error_consistency();
    test_has_payload_consistency();
    test_mandatory_payload_validation();
    test_max_size_frame();
    test_unknown_message_type();
    test_expect_no_payload();
    test_buffer_length_mismatch();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All PLP/1 frame checks passed\n");
    return EXIT_SUCCESS;
}
