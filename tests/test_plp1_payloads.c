/*
 * Milestone 14 payload-codec tests for PLP/1: VERSION, STATUS,
 * PRINT_START and the generic error payload, exercised directly against
 * their own encode/decode functions (not through a full frame).
 */
#include "printerctl/protocol/plp1.h"

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

/* ---- VERSION response ---- */

static void test_version_response(void)
{
    printerctl_plp1_version_response_t value;
    value.protocol_version = PRINTERCTL_PLP1_VERSION_1;
    value.capability_bitmap = PRINTERCTL_PLP1_CAP_PRINT_CONTROL | PRINTERCTL_PLP1_CAP_PRINTER_STATUS;

    uint8_t payload[16];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_version_response(&value, payload, sizeof(payload), &length,
                                                   &error) == PRINTERCTL_PLP1_OK,
          "encoding a VERSION response succeeds");
    check(length == 5, "the VERSION response payload is exactly 5 bytes");
    check(payload[0] == PRINTERCTL_PLP1_VERSION_1, "the protocol_version byte is correct");

    printerctl_plp1_version_response_t decoded;
    check(printerctl_plp1_decode_version_response(payload, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding a VERSION response succeeds");
    check(decoded.protocol_version == value.protocol_version &&
              decoded.capability_bitmap == value.capability_bitmap,
          "decoding a VERSION response recovers the exact fields");

    /* An unset capability bit stays unset through the round trip. */
    check((decoded.capability_bitmap & PRINTERCTL_PLP1_CAP_JOB_QUEUE) == 0,
          "a capability bit that was never set decodes as unset, not as fictitiously present");

    check(printerctl_plp1_decode_version_response(payload, 4, &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a truncated VERSION response payload is rejected as INVALID_PAYLOAD");
}

/* ---- STATUS request ---- */

static void test_status_request(void)
{
    uint8_t payload[8];
    size_t length = 0;
    printerctl_plp1_error_t error;
    uint32_t requested = PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE |
                          PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS;

    check(printerctl_plp1_encode_status_request(requested, payload, sizeof(payload), &length,
                                                 &error) == PRINTERCTL_PLP1_OK,
          "encoding a STATUS request succeeds");
    check(length == 4, "the STATUS request payload is exactly 4 bytes");

    uint32_t decoded = 0;
    check(printerctl_plp1_decode_status_request(payload, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding a STATUS request succeeds");
    check(decoded == requested, "decoding a STATUS request recovers the exact bitmap");
}

/* ---- STATUS response ---- */

static void test_status_response_partial_fields(void)
{
    printerctl_plp1_status_response_t value;
    memset(&value, 0, sizeof(value));
    value.present_fields = PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE |
                            PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS |
                            PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_ACKNOWLEDGED;
    value.daemon_state = PRINTERCTL_PLP1_DAEMON_STATE_RUNNING;
    value.progress = 42;
    value.commands_acknowledged = 12345;
    /* Fields NOT in present_fields are set here to prove they are not
     * accidentally serialized. */
    value.worker_state = PRINTERCTL_PLP1_WORKER_STATE_READY;
    value.elapsed_time = 999;

    uint8_t payload[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_status_response(&value, payload, sizeof(payload), &length,
                                                  &error) == PRINTERCTL_PLP1_OK,
          "encoding a partial STATUS response succeeds");
    /* present_fields(4) + daemon_state(1) + progress(1) + commands_acknowledged(4) */
    check(length == 4 + 1 + 1 + 4, "the encoded STATUS response has exactly the expected length");

    printerctl_plp1_status_response_t decoded;
    check(printerctl_plp1_decode_status_response(payload, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding a partial STATUS response succeeds");
    check(decoded.present_fields == value.present_fields, "present_fields round-trips exactly");
    check(decoded.daemon_state == PRINTERCTL_PLP1_DAEMON_STATE_RUNNING &&
              decoded.progress == 42 && decoded.commands_acknowledged == 12345,
          "the present fields decode to their exact values");
    check(decoded.worker_state == 0 && decoded.elapsed_time == 0,
          "fields absent from present_fields decode as zero, never as fictitious values, "
          "even though the encoder's input struct had non-zero garbage in them");
}

static void test_status_response_canonical_ordering(void)
{
    /* Request the fields in reverse-of-canonical order; the wire bytes
     * must still come out in canonical order regardless. */
    printerctl_plp1_status_response_t value;
    memset(&value, 0, sizeof(value));
    value.present_fields = PRINTERCTL_PLP1_STATUS_FIELD_FAILURE_REASON |
                            PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE;
    value.daemon_state = PRINTERCTL_PLP1_DAEMON_STATE_FAILED;
    value.failure_reason = PRINTERCTL_PLP1_PRINTER_ERROR;

    uint8_t payload[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_encode_status_response(&value, payload, sizeof(payload), &length, &error);

    /* Canonical order places daemon_state before failure_reason
     * regardless of which bit is numerically higher: present_fields (4
     * bytes) at offset 0, daemon_state (1 byte) at offset 4,
     * failure_reason (2 bytes) at offset 5. */
    check(payload[4] == PRINTERCTL_PLP1_DAEMON_STATE_FAILED,
          "daemon_state is written before failure_reason on the wire (canonical order)");
    uint16_t wire_failure_reason = (uint16_t)((payload[5] << 8) | payload[6]);
    check(wire_failure_reason == PRINTERCTL_PLP1_PRINTER_ERROR,
          "failure_reason follows daemon_state at the canonical offset");

    printerctl_plp1_status_response_t decoded;
    check(printerctl_plp1_decode_status_response(payload, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding the reordered-request STATUS response still succeeds");
    check(decoded.daemon_state == PRINTERCTL_PLP1_DAEMON_STATE_FAILED &&
              decoded.failure_reason == PRINTERCTL_PLP1_PRINTER_ERROR,
          "both fields decode correctly regardless of request order");
}

static void test_status_response_truncated(void)
{
    printerctl_plp1_status_response_t value;
    memset(&value, 0, sizeof(value));
    value.present_fields = PRINTERCTL_PLP1_STATUS_FIELD_ELAPSED_TIME;
    value.elapsed_time = 100;

    uint8_t payload[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_encode_status_response(&value, payload, sizeof(payload), &length, &error);

    printerctl_plp1_status_response_t decoded;
    check(printerctl_plp1_decode_status_response(payload, length - 1, &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a STATUS response payload shorter than present_fields implies is rejected as "
          "INVALID_PAYLOAD");
}

/* ---- PRINT_START ---- */

static void test_print_start_empty_path(void)
{
    uint8_t payload[8];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_print_start_request("", payload, sizeof(payload), &length,
                                                       &error) == PRINTERCTL_PLP1_OK,
          "encoding an empty PRINT_START path succeeds");
    check(length == 2, "an empty path's payload is just the 2-byte zero length prefix");

    printerctl_plp1_print_start_request_t decoded;
    check(printerctl_plp1_decode_print_start_request(payload, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding an empty PRINT_START path succeeds -- an empty path is a codec-valid string, "
          "not a codec error; whether it is an acceptable print request is Milestone 16 domain "
          "logic");
    check(decoded.source_path[0] == '\0', "the decoded path is the empty string");
}

static void test_print_start_valid_path(void)
{
    uint8_t payload[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_encode_print_start_request("/tmp/a.gcode", payload, sizeof(payload), &length,
                                                &error);

    printerctl_plp1_print_start_request_t decoded;
    check(printerctl_plp1_decode_print_start_request(payload, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding a normal PRINT_START path succeeds");
    check(strcmp(decoded.source_path, "/tmp/a.gcode") == 0, "the decoded path matches exactly");
}

static void test_print_start_path_at_max(void)
{
    char long_path[PRINTERCTL_PLP1_PRINT_START_PATH_MAX];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    uint8_t payload[PRINTERCTL_PLP1_PRINT_START_PATH_MAX + 8];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_print_start_request(long_path, payload, sizeof(payload), &length,
                                                       &error) == PRINTERCTL_PLP1_OK,
          "encoding a path of PRINT_START_PATH_MAX - 1 bytes succeeds");

    printerctl_plp1_print_start_request_t decoded;
    check(printerctl_plp1_decode_print_start_request(payload, length, &decoded, &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding a path of exactly PRINT_START_PATH_MAX - 1 bytes (fits + NUL) succeeds");
    check(strcmp(decoded.source_path, long_path) == 0, "the maximum-length path round-trips exactly");
}

static void test_print_start_over_length_path(void)
{
    /* One byte longer than the destination buffer can hold (including
     * the NUL) -- must be rejected, not silently truncated (unlike the
     * generic error message). */
    char over_path[PRINTERCTL_PLP1_PRINT_START_PATH_MAX + 1];
    memset(over_path, 'b', sizeof(over_path) - 1);
    over_path[sizeof(over_path) - 1] = '\0';

    uint8_t payload[PRINTERCTL_PLP1_PRINT_START_PATH_MAX + 16];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_string(over_path, payload, sizeof(payload), &length, &error) ==
              PRINTERCTL_PLP1_OK,
          "the wire encoder itself has no PRINT_START-specific length limit");

    printerctl_plp1_print_start_request_t decoded;
    check(printerctl_plp1_decode_print_start_request(payload, length, &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "decoding a path that does not fit PRINT_START_PATH_MAX is rejected as INVALID_PAYLOAD, "
          "not truncated");
}

static void test_truncated_string(void)
{
    /* Declared length exceeds the bytes actually available. */
    uint8_t payload[] = {0x00, 0x05, 'a', 'b'}; /* declares 5 bytes, only 2 present */
    printerctl_plp1_print_start_request_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_print_start_request(payload, sizeof(payload), &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a declared string length exceeding the available payload is rejected as INVALID_PAYLOAD");

    /* Not even enough bytes for the 2-byte length prefix itself. */
    uint8_t one_byte[] = {0x00};
    check(printerctl_plp1_decode_print_start_request(one_byte, sizeof(one_byte), &decoded,
                                                       &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a payload too short even for the string length prefix is rejected as INVALID_PAYLOAD");
}

/* ---- required payload entirely absent (payload == NULL, length == 0) ----
 *
 * decode_frame() hands back payload == NULL / payload_length == 0 for
 * a frame with no payload at all -- a legitimate, if insufficient,
 * decode input (not caller misuse). Every payload decoder must report
 * this as PRINTERCTL_PLP1_ERR_MALFORMED / INVALID_PAYLOAD, the same as
 * any other too-short payload, not PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT
 * (which would incorrectly suggest the *caller* passed bad arguments).
 */

static void test_required_payload_absent(void)
{
    printerctl_plp1_error_t error;

    printerctl_plp1_version_response_t version_value;
    check(printerctl_plp1_decode_version_response(NULL, 0, &version_value, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a completely absent VERSION response payload is INVALID_PAYLOAD, not caller misuse");

    uint32_t requested_fields;
    check(printerctl_plp1_decode_status_request(NULL, 0, &requested_fields, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a completely absent STATUS request payload is INVALID_PAYLOAD, not caller misuse");

    printerctl_plp1_status_response_t status_value;
    check(printerctl_plp1_decode_status_response(NULL, 0, &status_value, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a completely absent STATUS response payload is INVALID_PAYLOAD, not caller misuse");

    printerctl_plp1_print_start_request_t print_start_value;
    check(printerctl_plp1_decode_print_start_request(NULL, 0, &print_start_value, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a completely absent PRINT_START payload is INVALID_PAYLOAD, not caller misuse");

    char message[32];
    check(printerctl_plp1_decode_error_payload(NULL, 0, message, sizeof(message), &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a completely absent error payload is INVALID_PAYLOAD, not caller misuse");
}

/* ---- trailing bytes: a payload longer than the codec actually consumes ---- */

static void test_trailing_bytes_rejected(void)
{
    printerctl_plp1_error_t error;

    /* VERSION response: 5 bytes needed, 1 extra. */
    {
        printerctl_plp1_version_response_t value = {PRINTERCTL_PLP1_VERSION_1, 0};
        uint8_t payload[6];
        size_t length = 0;
        printerctl_plp1_encode_version_response(&value, payload, sizeof(payload), &length, &error);
        printerctl_plp1_version_response_t decoded;
        check(printerctl_plp1_decode_version_response(payload, length + 1, &decoded, &error) ==
                      PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "a VERSION response payload with one trailing byte is rejected as INVALID_PAYLOAD");
    }

    /* STATUS request: 4 bytes needed, 1 extra. */
    {
        uint8_t payload[5] = {0, 0, 0, 1, 0xFF};
        uint32_t requested_fields;
        check(printerctl_plp1_decode_status_request(payload, sizeof(payload), &requested_fields,
                                                      &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "a STATUS request payload with one trailing byte is rejected as INVALID_PAYLOAD");
    }

    /* STATUS response: present_fields declares one field, payload has one extra byte after it. */
    {
        printerctl_plp1_status_response_t value;
        memset(&value, 0, sizeof(value));
        value.present_fields = PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS;
        value.progress = 50;
        uint8_t payload[6]; /* 4 (present_fields) + 1 (progress) + 1 trailing */
        size_t length = 0;
        printerctl_plp1_encode_status_response(&value, payload, sizeof(payload) - 1, &length,
                                                &error);
        payload[length] = 0xFF; /* one trailing byte the encoder never wrote */
        printerctl_plp1_status_response_t decoded;
        check(printerctl_plp1_decode_status_response(payload, length + 1, &decoded, &error) ==
                      PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "a STATUS response payload with one trailing byte beyond present_fields is "
              "rejected as INVALID_PAYLOAD");
    }

    /* PRINT_START: string consumed, one extra byte left over. */
    {
        uint8_t payload[64];
        size_t length = 0;
        printerctl_plp1_encode_print_start_request("a.gcode", payload, sizeof(payload), &length,
                                                     &error);
        payload[length] = 0xFF;
        printerctl_plp1_print_start_request_t decoded;
        check(printerctl_plp1_decode_print_start_request(payload, length + 1, &decoded, &error) ==
                      PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "a PRINT_START payload with one trailing byte after the path is rejected as "
              "INVALID_PAYLOAD");
    }

    /* Generic error payload: message consumed, one extra byte left over. */
    {
        uint8_t payload[64];
        size_t length = 0;
        printerctl_plp1_encode_error_payload("oops", payload, sizeof(payload), &length, &error);
        payload[length] = 0xFF;
        char message[32];
        check(printerctl_plp1_decode_error_payload(payload, length + 1, message, sizeof(message),
                                                    &error) == PRINTERCTL_PLP1_ERR_MALFORMED &&
                  error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
              "an error payload with one trailing byte after the message is rejected as "
              "INVALID_PAYLOAD");
    }
}

/* ---- STATUS reserved bits ---- */

static void test_status_response_reserved_bits(void)
{
    /* A present_fields value with a bit outside PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK
     * (0x01FF) set must be rejected -- the decoder cannot safely size or
     * skip a field it does not know about. */
    uint8_t payload[4];
    uint32_t present = PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE | 0x8000u; /* bit 15: reserved */
    payload[0] = (uint8_t)(present >> 24);
    payload[1] = (uint8_t)(present >> 16);
    payload[2] = (uint8_t)(present >> 8);
    payload[3] = (uint8_t)(present & 0xFFu);

    printerctl_plp1_status_response_t decoded;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_decode_status_response(payload, sizeof(payload), &decoded, &error) ==
                  PRINTERCTL_PLP1_ERR_MALFORMED &&
              error.wire_status == PRINTERCTL_PLP1_INVALID_PAYLOAD,
          "a STATUS response present_fields with a reserved bit set is rejected as "
          "INVALID_PAYLOAD");
}

/* The two STATUS encoders must reject reserved bits themselves, as
 * caller-input validation -- otherwise a caller could construct a
 * frame this same codec's own decoder would then refuse to read back,
 * an asymmetry that let garbage bits reach the wire silently. */

static void test_encode_status_request_rejects_reserved_bits(void)
{
    uint8_t payload[8];
    size_t length = 0;
    printerctl_plp1_error_t error;
    uint32_t requested = PRINTERCTL_PLP1_STATUS_FIELD_PROGRESS | 0x0200u; /* bit 9: reserved */

    check(printerctl_plp1_encode_status_request(requested, payload, sizeof(payload), &length,
                                                 &error) == PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_status_request() rejects a requested_fields with a reserved bit set");
    check(length == 0, "a rejected encode_status_request() reports zero bytes written");
}

static void test_encode_status_response_rejects_reserved_bits(void)
{
    printerctl_plp1_status_response_t value;
    memset(&value, 0, sizeof(value));
    value.present_fields = PRINTERCTL_PLP1_STATUS_FIELD_DAEMON_STATE | 0x8000u; /* bit 15: reserved */
    value.daemon_state = PRINTERCTL_PLP1_DAEMON_STATE_RUNNING;

    uint8_t payload[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_status_response(&value, payload, sizeof(payload), &length,
                                                  &error) == PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT,
          "encode_status_response() rejects a present_fields with a reserved bit set");
    check(length == 0, "a rejected encode_status_response() reports zero bytes written");
}

/* ---- generic error payload ---- */

static void test_error_payload_round_trip(void)
{
    uint8_t payload[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    check(printerctl_plp1_encode_error_payload("printer disconnected", payload, sizeof(payload),
                                                &length, &error) == PRINTERCTL_PLP1_OK,
          "encoding a generic error message succeeds");

    char message[64];
    check(printerctl_plp1_decode_error_payload(payload, length, message, sizeof(message),
                                                &error) == PRINTERCTL_PLP1_OK,
          "decoding a generic error message succeeds");
    check(strcmp(message, "printer disconnected") == 0, "the decoded message matches exactly");
}

static void test_error_payload_truncating_decode(void)
{
    uint8_t payload[64];
    size_t length = 0;
    printerctl_plp1_error_t error;
    printerctl_plp1_encode_error_payload("a message longer than the tiny buffer", payload,
                                          sizeof(payload), &length, &error);

    char tiny[8]; /* room for 7 chars + NUL */
    check(printerctl_plp1_decode_error_payload(payload, length, tiny, sizeof(tiny), &error) ==
              PRINTERCTL_PLP1_OK,
          "decoding an oversized error message into a small buffer still succeeds (truncates)");
    check(strlen(tiny) == sizeof(tiny) - 1, "the truncated message fills exactly the caller's buffer");
    check(tiny[sizeof(tiny) - 1] == '\0', "the truncated message is still NUL-terminated");
}

int main(void)
{
    test_version_response();
    test_status_request();
    test_status_response_partial_fields();
    test_status_response_canonical_ordering();
    test_status_response_truncated();
    test_status_response_reserved_bits();
    test_encode_status_request_rejects_reserved_bits();
    test_encode_status_response_rejects_reserved_bits();
    test_required_payload_absent();
    test_trailing_bytes_rejected();
    test_print_start_empty_path();
    test_print_start_valid_path();
    test_print_start_path_at_max();
    test_print_start_over_length_path();
    test_truncated_string();
    test_error_payload_round_trip();
    test_error_payload_truncating_decode();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All PLP/1 payload checks passed\n");
    return EXIT_SUCCESS;
}
