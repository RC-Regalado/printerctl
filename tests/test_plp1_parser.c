/*
 * Milestone 14 incremental-parser tests for PLP/1: fragmentation at
 * arbitrary boundaries, coalesced frames, partial trailing frames, a
 * structural framing fault, and buffer-full partial consumption.
 */
#include "printerctl/protocol/plp1_parser.h"

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

/* Builds one PING request frame's exact wire bytes into `out`, returning its length. */
static size_t build_ping_request(uint32_t request_id, uint8_t *out, size_t out_size)
{
    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    frame.message_type = PRINTERCTL_PLP1_MSG_PING;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = request_id;
    frame.status = PRINTERCTL_PLP1_SUCCESS;

    size_t length = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_encode_frame(&frame, out, out_size, &length, &error);
    return length;
}

static void test_byte_at_a_time(void)
{
    uint8_t wire[64];
    size_t wire_length = build_ping_request(1, wire, sizeof(wire));

    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));

    printerctl_plp1_frame_t frame;
    printerctl_plp1_error_t error;
    bool got_frame = false;
    for (size_t i = 0; i < wire_length; i++) {
        size_t consumed = 0;
        check(printerctl_plp1_parser_feed(&parser, wire + i, 1, &consumed, &error) ==
                  PRINTERCTL_PLP1_OK,
              "feeding one byte succeeds");
        check(consumed == 1, "feeding one byte consumes exactly one byte");

        printerctl_plp1_parser_take_status_t status =
            printerctl_plp1_parser_take_frame(&parser, &frame, &error);
        check(status == PRINTERCTL_PLP1_PARSER_NEED_MORE ||
                  (i == wire_length - 1 && status == PRINTERCTL_PLP1_PARSER_FRAME_READY),
              "take_frame() reports NEED_MORE until the very last byte");
        if (status == PRINTERCTL_PLP1_PARSER_FRAME_READY) {
            got_frame = true;
        }
    }
    check(got_frame, "a frame fed one byte at a time is eventually produced");
    check(frame.request_id == 1, "the byte-at-a-time frame has the correct request_id");
}

static void test_first_four_bytes_then_rest(void)
{
    uint8_t wire[64];
    size_t wire_length = build_ping_request(2, wire, sizeof(wire));

    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));

    size_t consumed = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_parser_feed(&parser, wire, 4, &consumed, &error);

    printerctl_plp1_frame_t frame;
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
              PRINTERCTL_PLP1_PARSER_NEED_MORE,
          "only the 4-byte length prefix is not enough for a frame");

    printerctl_plp1_parser_feed(&parser, wire + 4, wire_length - 4, &consumed, &error);
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
              PRINTERCTL_PLP1_PARSER_FRAME_READY,
          "feeding the rest completes the frame");
    check(frame.request_id == 2, "the completed frame has the correct request_id");
}

static void test_arbitrary_cut_points(void)
{
    uint8_t wire[64];
    size_t wire_length = build_ping_request(3, wire, sizeof(wire));

    /* A handful of deterministic split offsets covering: mid-length-prefix,
     * header split mid-field, exactly at the header boundary, and one
     * byte before the end -- not randomized, so results are reproducible. */
    size_t cut_points[] = {1, 2, 3, 5, 7, 9, 13, wire_length - 1};

    for (size_t c = 0; c < sizeof(cut_points) / sizeof(cut_points[0]); c++) {
        size_t cut = cut_points[c];
        if (cut == 0 || cut >= wire_length) {
            continue;
        }

        uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
        printerctl_plp1_parser_t parser;
        printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));

        size_t consumed = 0;
        printerctl_plp1_error_t error;
        printerctl_plp1_parser_feed(&parser, wire, cut, &consumed, &error);

        printerctl_plp1_frame_t frame;
        printerctl_plp1_parser_take_status_t status =
            printerctl_plp1_parser_take_frame(&parser, &frame, &error);
        check(status == PRINTERCTL_PLP1_PARSER_NEED_MORE,
              "a frame split before its end is never ready early, regardless of cut point");

        printerctl_plp1_parser_feed(&parser, wire + cut, wire_length - cut, &consumed, &error);
        status = printerctl_plp1_parser_take_frame(&parser, &frame, &error);
        check(status == PRINTERCTL_PLP1_PARSER_FRAME_READY,
              "the frame completes once the remainder arrives, for every cut point tried");
        check(frame.request_id == 3, "the reassembled frame has the correct request_id");
    }
}

static void test_multiple_frames_in_one_feed(void)
{
    uint8_t one[64];
    uint8_t two[64];
    uint8_t three[64];
    size_t one_length = build_ping_request(10, one, sizeof(one));
    size_t two_length = build_ping_request(11, two, sizeof(two));
    size_t three_length = build_ping_request(12, three, sizeof(three));

    uint8_t combined[256];
    size_t offset = 0;
    memcpy(combined + offset, one, one_length);
    offset += one_length;
    memcpy(combined + offset, two, two_length);
    offset += two_length;
    memcpy(combined + offset, three, three_length);
    offset += three_length;

    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));

    size_t consumed = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_parser_feed(&parser, combined, offset, &consumed, &error) ==
              PRINTERCTL_PLP1_OK,
          "feeding three concatenated frames succeeds");
    check(consumed == offset, "all bytes of three concatenated frames are consumed by one feed()");

    printerctl_plp1_frame_t frame;
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 10,
          "the first of three coalesced frames is produced first");
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 11,
          "the second of three coalesced frames is produced next");
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 12,
          "the third of three coalesced frames is produced last");
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
              PRINTERCTL_PLP1_PARSER_NEED_MORE,
          "nothing more is ready after draining all three");
}

static void test_complete_plus_partial_next_frame(void)
{
    uint8_t one[64];
    uint8_t two[64];
    size_t one_length = build_ping_request(20, one, sizeof(one));
    size_t two_length = build_ping_request(21, two, sizeof(two));

    uint8_t combined[256];
    memcpy(combined, one, one_length);
    /* Only the first half of the second frame. */
    size_t two_partial = two_length / 2;
    memcpy(combined + one_length, two, two_partial);
    size_t total_first_feed = one_length + two_partial;

    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));

    size_t consumed = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_parser_feed(&parser, combined, total_first_feed, &consumed, &error);

    printerctl_plp1_frame_t frame;
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 20,
          "the complete first frame is ready even though a partial second frame follows it");
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
              PRINTERCTL_PLP1_PARSER_NEED_MORE,
          "the partial second frame is not ready yet");

    printerctl_plp1_parser_feed(&parser, two + two_partial, two_length - two_partial, &consumed,
                                 &error);
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 21,
          "the second frame completes once its remainder is fed");
}

static void test_max_size_frame_boundary(void)
{
    /* A parser buffer sized exactly PRINTERCTL_PLP1_MAX_FRAME_SIZE must
     * still be able to take in and produce one full max-size frame --
     * the boundary the parser's own documentation promises callers. */
    static uint8_t payload[PRINTERCTL_PLP1_MAX_FRAME_LENGTH - PRINTERCTL_PLP1_MIN_FRAME_LENGTH];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    printerctl_plp1_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = PRINTERCTL_PLP1_VERSION_1;
    /* PRINT_START, not PING: PING now structurally forbids a payload
     * (decode_frame()'s mandatory per-message payload validation), and
     * PRINT_START's request is the message type that legitimately
     * carries an arbitrarily large payload. */
    frame.message_type = PRINTERCTL_PLP1_MSG_PRINT_START;
    frame.flags = PRINTERCTL_PLP1_FLAG_REQUEST;
    frame.request_id = 77;
    frame.status = PRINTERCTL_PLP1_SUCCESS;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);

    static uint8_t wire[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    size_t wire_length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_frame(&frame, wire, sizeof(wire), &wire_length, &error) ==
                  PRINTERCTL_PLP1_OK &&
              wire_length == PRINTERCTL_PLP1_MAX_FRAME_SIZE,
          "building the max-size wire fixture succeeds");

    static uint8_t parser_buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, parser_buffer, sizeof(parser_buffer));

    size_t consumed = 0;
    check(printerctl_plp1_parser_feed(&parser, wire, wire_length, &consumed, &error) ==
                  PRINTERCTL_PLP1_OK &&
              consumed == wire_length,
          "a parser buffer of exactly PRINTERCTL_PLP1_MAX_FRAME_SIZE accepts a full max-size "
          "frame in one feed()");

    printerctl_plp1_frame_t decoded;
    check(printerctl_plp1_parser_take_frame(&parser, &decoded, &error) ==
              PRINTERCTL_PLP1_PARSER_FRAME_READY,
          "the max-size frame is produced by take_frame()");
    check(decoded.payload_length == sizeof(payload) &&
              memcmp(decoded.payload, payload, sizeof(payload)) == 0,
          "the parsed max-size frame's payload matches exactly, byte for byte");
}

static void test_structural_fault(void)
{
    uint8_t bad_length_frame[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, /* frame_length far beyond MAX */
        0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    };

    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));

    size_t consumed = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_parser_feed(&parser, bad_length_frame, sizeof(bad_length_frame), &consumed,
                                 &error);

    printerctl_plp1_frame_t frame;
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_FRAME_TOO_LARGE,
          "an out-of-range frame_length is reported as a structural ERR_MALFORMED, not a "
          "semantic FRAME_REJECTED");
}

static void test_semantic_rejection_allows_continued_parsing(void)
{
    /* A frame with a known-bad message type: the boundary is trusted
     * (frame_length is valid), so the parser should consume it and let
     * the caller keep going with the next frame. */
    uint8_t bad_type_frame[16] = {
        0x00, 0x00, 0x00, 0x0C,
        0x01,
        0x7F, /* unknown message type */
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
    };
    uint8_t good_frame[64];
    size_t good_length = build_ping_request(99, good_frame, sizeof(good_frame));

    uint8_t combined[128];
    memcpy(combined, bad_type_frame, sizeof(bad_type_frame));
    memcpy(combined + sizeof(bad_type_frame), good_frame, good_length);

    uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));

    size_t consumed = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_parser_feed(&parser, combined, sizeof(bad_type_frame) + good_length, &consumed,
                                 &error);

    printerctl_plp1_frame_t frame;
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_REJECTED &&
              error.wire_status == PRINTERCTL_PLP1_UNKNOWN_MESSAGE_TYPE,
          "a frame with an unknown message type is FRAME_REJECTED (semantic), not ERR_MALFORMED");
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 99,
          "parsing continues normally after a semantically rejected frame");
}

static void test_feed_partial_consumption_when_buffer_full(void)
{
    /* A buffer capacity of exactly one frame's size: feeding two frames
     * worth of bytes in one call must consume only the first frame's
     * worth and report the rest as unconsumed, never fail outright. */
    uint8_t one[64];
    size_t one_length = build_ping_request(30, one, sizeof(one));

    uint8_t two[64];
    size_t two_length = build_ping_request(31, two, sizeof(two));

    uint8_t combined[256];
    memcpy(combined, one, one_length);
    memcpy(combined + one_length, two, two_length);

    uint8_t small_buffer[64];
    printerctl_plp1_parser_t parser;
    printerctl_plp1_parser_init(&parser, small_buffer, one_length); /* room for exactly one frame */

    size_t consumed = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_parser_feed(&parser, combined, one_length + two_length, &consumed,
                                       &error) == PRINTERCTL_PLP1_OK,
          "feed() succeeds even when not all bytes fit");
    check(consumed == one_length,
          "feed() consumes only as many bytes as fit, reporting the rest as unconsumed rather "
          "than failing");

    printerctl_plp1_frame_t frame;
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 30,
          "the frame that did fit is ready");

    /* After draining, the rest can be fed. */
    check(printerctl_plp1_parser_feed(&parser, combined + consumed, two_length, &consumed,
                                       &error) == PRINTERCTL_PLP1_OK &&
              consumed == two_length,
          "feeding the remainder after draining succeeds fully");
    check(printerctl_plp1_parser_take_frame(&parser, &frame, &error) ==
                  PRINTERCTL_PLP1_PARSER_FRAME_READY &&
              frame.request_id == 31,
          "the second frame is ready after the remainder is fed");
}

int main(void)
{
    test_byte_at_a_time();
    test_first_four_bytes_then_rest();
    test_arbitrary_cut_points();
    test_multiple_frames_in_one_feed();
    test_complete_plus_partial_next_frame();
    test_max_size_frame_boundary();
    test_structural_fault();
    test_semantic_rejection_allows_continued_parsing();
    test_feed_partial_consumption_when_buffer_full();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All PLP/1 parser checks passed\n");
    return EXIT_SUCCESS;
}
