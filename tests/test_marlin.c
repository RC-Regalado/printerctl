#include "printerctl/marlin.h"

#include <math.h>
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

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

/* One-call classification assertion: type, terminal flag and payload text. */
static void expect_classified(const char *line,
                               printerctl_marlin_response_type_t expected_type,
                               bool expected_terminal,
                               const char *expected_payload,
                               const char *description)
{
    printerctl_marlin_response_t response;
    printerctl_marlin_status_t status = printerctl_marlin_classify(line, &response);

    check(status == PRINTERCTL_MARLIN_OK, description);
    check(response.type == expected_type, description);
    check(response.terminal == expected_terminal, description);
    check(response.payload != NULL && strcmp(response.payload, expected_payload) == 0, description);
}

static void test_classify_ack(void)
{
    expect_classified("ok", PRINTERCTL_MARLIN_RESPONSE_ACK, true, "",
                      "bare 'ok' is a terminal acknowledgement with an empty payload");
    expect_classified("ok T:210.00 /210.00 B:60.00 /60.00 @:127 B@:0",
                      PRINTERCTL_MARLIN_RESPONSE_ACK, true,
                      "T:210.00 /210.00 B:60.00 /60.00 @:127 B@:0",
                      "'ok <payload>' is a terminal acknowledgement carrying its payload");
    expect_classified("ok N5 P15 B3", PRINTERCTL_MARLIN_RESPONSE_ACK, true, "N5 P15 B3",
                      "ADVANCED_OK style 'ok N.. P.. B..' is still an acknowledgement");

    /* Lookalikes must not be acknowledgements. */
    expect_classified("okay", PRINTERCTL_MARLIN_RESPONSE_INFO, false, "okay",
                      "'okay' is not an acknowledgement");
    expect_classified("look", PRINTERCTL_MARLIN_RESPONSE_INFO, false, "look",
                      "'look' is not an acknowledgement");
    expect_classified("OK", PRINTERCTL_MARLIN_RESPONSE_INFO, false, "OK",
                      "uppercase 'OK' is not something Marlin emits and stays informational");
}

static void test_classify_error(void)
{
    expect_classified("Error:Printer halted. kill() called!",
                      PRINTERCTL_MARLIN_RESPONSE_ERROR, true,
                      "Printer halted. kill() called!",
                      "'Error:' is a terminal failure carrying its message");
    expect_classified("Error: checksum mismatch, Last Line: 4",
                      PRINTERCTL_MARLIN_RESPONSE_ERROR, true,
                      "checksum mismatch, Last Line: 4",
                      "'Error: <text>' skips the space before the message");
}

static void test_classify_busy_and_echo(void)
{
    expect_classified("echo:busy: processing", PRINTERCTL_MARLIN_RESPONSE_BUSY, false,
                      "processing",
                      "'echo:busy:' is busy, not echo");
    expect_classified("busy: paused for user", PRINTERCTL_MARLIN_RESPONSE_BUSY, false,
                      "paused for user",
                      "'busy:' is a non-terminal busy response");
    expect_classified("echo:Unknown command: \"M999123\"",
                      PRINTERCTL_MARLIN_RESPONSE_ECHO, false,
                      "Unknown command: \"M999123\"",
                      "'echo:' is non-terminal informational output");
    expect_classified("echo:  cold extrusion prevented",
                      PRINTERCTL_MARLIN_RESPONSE_ECHO, false,
                      "cold extrusion prevented",
                      "'echo:' payload has leading spaces skipped");
}

static void test_classify_resend(void)
{
    printerctl_marlin_response_t response;

    check(printerctl_marlin_classify("Resend: 5", &response) == PRINTERCTL_MARLIN_OK &&
              response.type == PRINTERCTL_MARLIN_RESPONSE_RESEND,
          "'Resend: 5' classifies as a resend request");
    check(response.terminal, "a resend request is terminal (unsupported in the MVP)");
    check(response.has_resend_line && response.resend_line == 5,
          "'Resend: 5' decodes its line number");

    check(printerctl_marlin_classify("Resend:12345", &response) == PRINTERCTL_MARLIN_OK &&
              response.has_resend_line && response.resend_line == 12345,
          "'Resend:12345' (no space) decodes its line number");

    check(printerctl_marlin_classify("Resend: garbage", &response) == PRINTERCTL_MARLIN_OK &&
              response.type == PRINTERCTL_MARLIN_RESPONSE_RESEND && !response.has_resend_line,
          "a resend with an unparsable value stays a resend but reports no line number");
}

static void test_classify_info(void)
{
    expect_classified("start", PRINTERCTL_MARLIN_RESPONSE_INFO, false, "start",
                      "'start' is informational");
    expect_classified("wait", PRINTERCTL_MARLIN_RESPONSE_INFO, false, "wait",
                      "'wait' is informational");
    expect_classified("T:23.5 /0.0 B:22.9 /0.0 @:0 B@:0",
                      PRINTERCTL_MARLIN_RESPONSE_INFO, false,
                      "T:23.5 /0.0 B:22.9 /0.0 @:0 B@:0",
                      "an auto temperature report line without 'ok' is informational");
    expect_classified("", PRINTERCTL_MARLIN_RESPONSE_INFO, false, "",
                      "an empty line is informational, never an error");
}

static void test_classify_invalid_arguments(void)
{
    printerctl_marlin_response_t response;

    check(printerctl_marlin_classify(NULL, &response) == PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "classify with a NULL line is rejected");
    check(response.type == PRINTERCTL_MARLIN_RESPONSE_INFO && response.payload != NULL,
          "a rejected classify still leaves *out in a well-defined state");
    check(printerctl_marlin_classify("ok", NULL) == PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "classify with a NULL out is rejected");
}

static void test_parse_temperature(void)
{
    printerctl_marlin_temperature_report_t temps;

    check(printerctl_marlin_parse_temperature(
              "T:210.00 /210.00 B:60.00 /60.00 @:127 B@:0", &temps) == PRINTERCTL_MARLIN_OK,
          "a full M105 report parses");
    check(temps.has_hotend && near(temps.hotend_actual, 210.0) && near(temps.hotend_target, 210.0),
          "hotend actual/target decode");
    check(temps.has_bed && near(temps.bed_actual, 60.0) && near(temps.bed_target, 60.0),
          "bed actual/target decode; B@: is not confused with B:");

    check(printerctl_marlin_parse_temperature("T:23.5/0.0 B:22.9/0.0 @:0", &temps) ==
              PRINTERCTL_MARLIN_OK,
          "the no-space actual/target variant parses");
    check(temps.has_hotend && near(temps.hotend_actual, 23.5) && near(temps.hotend_target, 0.0),
          "no-space hotend values decode");
    check(temps.has_bed && near(temps.bed_actual, 22.9), "no-space bed values decode");

    check(printerctl_marlin_parse_temperature("T:150.3 /200.0 @:127", &temps) ==
              PRINTERCTL_MARLIN_OK &&
              temps.has_hotend && !temps.has_bed,
          "a hotend-only report parses with has_bed == false");

    check(printerctl_marlin_parse_temperature("ok", &temps) == PRINTERCTL_MARLIN_ERR_NO_MATCH,
          "a line with no temperature pairs is NO_MATCH");
    check(!temps.has_hotend && !temps.has_bed, "NO_MATCH zeroes the report");
    check(printerctl_marlin_parse_temperature("echo:Bed Leveling", &temps) ==
              PRINTERCTL_MARLIN_ERR_NO_MATCH,
          "arbitrary text is NO_MATCH");

    check(printerctl_marlin_parse_temperature(NULL, &temps) ==
              PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "temperature parse with a NULL line is rejected");
    check(printerctl_marlin_parse_temperature("T:1/2", NULL) ==
              PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "temperature parse with a NULL out is rejected");
}

static void test_parse_temperature_from_classified_ok_payload(void)
{
    printerctl_marlin_response_t response;
    printerctl_marlin_temperature_report_t temps;

    const char *line = "ok T:210.00 /210.00 B:60.00 /60.00 @:127 B@:0";
    check(printerctl_marlin_classify(line, &response) == PRINTERCTL_MARLIN_OK &&
              response.type == PRINTERCTL_MARLIN_RESPONSE_ACK,
          "the M105 'ok' line classifies as an acknowledgement");
    check(printerctl_marlin_parse_temperature(response.payload, &temps) == PRINTERCTL_MARLIN_OK &&
              temps.has_hotend && temps.has_bed,
          "the classified payload feeds straight into the temperature parser");
}

static void test_parse_position(void)
{
    printerctl_marlin_position_report_t position;

    check(printerctl_marlin_parse_position(
              "X:0.00 Y:0.00 Z:0.00 E:0.00 Count X:0 Y:0 Z:0", &position) == PRINTERCTL_MARLIN_OK,
          "a full M114 report parses");
    check(near(position.x, 0.0) && near(position.y, 0.0) && near(position.z, 0.0),
          "logical coordinates decode");
    check(position.has_e && near(position.e, 0.0), "E decodes when present");

    check(printerctl_marlin_parse_position(
              "X:12.50 Y:-3.25 Z:0.40 E:120.75 Count X:1000 Y:-260 Z:160", &position) ==
              PRINTERCTL_MARLIN_OK,
          "a mid-print M114 report parses");
    check(near(position.x, 12.5) && near(position.y, -3.25) && near(position.z, 0.4) &&
              near(position.e, 120.75),
          "negative and fractional coordinates decode; Count values are ignored");

    check(printerctl_marlin_parse_position("X:1.0 Y:2.0 Z:3.0", &position) ==
              PRINTERCTL_MARLIN_OK &&
              !position.has_e,
          "a report without E parses with has_e == false");

    check(printerctl_marlin_parse_position("X:1.0 Y:2.0", &position) ==
              PRINTERCTL_MARLIN_ERR_NO_MATCH,
          "a line missing Z is not a position report");
    check(printerctl_marlin_parse_position("Count X:0 Y:0 Z:0", &position) ==
              PRINTERCTL_MARLIN_ERR_NO_MATCH,
          "a line whose coordinates only appear after Count is not a position report");
    check(printerctl_marlin_parse_position("echo:X marks the spot", &position) ==
              PRINTERCTL_MARLIN_ERR_NO_MATCH,
          "arbitrary text containing X is not a position report");

    check(printerctl_marlin_parse_position(NULL, &position) ==
              PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "position parse with a NULL line is rejected");
    check(printerctl_marlin_parse_position("X:1 Y:2 Z:3", NULL) ==
              PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "position parse with a NULL out is rejected");
}

static void test_parse_firmware_info(void)
{
    printerctl_marlin_firmware_info_t info;

    const char *line =
        "FIRMWARE_NAME:Marlin 2.1.2 (Sep 30 2023 12:00:00) "
        "SOURCE_CODE_URL:github.com/MarlinFirmware/Marlin "
        "PROTOCOL_VERSION:1.0 MACHINE_TYPE:EasyThreed K9 EXTRUDER_COUNT:1 "
        "UUID:cede2a2f-41a2-4748-9b12-c55c62f367ff";

    check(printerctl_marlin_parse_firmware_info(line, &info) == PRINTERCTL_MARLIN_OK,
          "a full M115 report parses");
    check(strcmp(info.firmware_name, "Marlin 2.1.2 (Sep 30 2023 12:00:00)") == 0,
          "firmware name (containing spaces) decodes up to the next key");
    check(strcmp(info.machine_type, "EasyThreed K9") == 0,
          "machine type (containing spaces) decodes");
    check(strcmp(info.protocol_version, "1.0") == 0, "protocol version decodes");
    check(strcmp(info.uuid, "cede2a2f-41a2-4748-9b12-c55c62f367ff") == 0, "UUID decodes");
    check(info.has_extruder_count && info.extruder_count == 1, "extruder count decodes");

    check(printerctl_marlin_parse_firmware_info("FIRMWARE_NAME:Marlin 1.1.9", &info) ==
              PRINTERCTL_MARLIN_OK,
          "a minimal M115 report with only a firmware name parses");
    check(strcmp(info.firmware_name, "Marlin 1.1.9") == 0, "minimal firmware name decodes");
    check(info.machine_type[0] == '\0' && info.uuid[0] == '\0' && !info.has_extruder_count,
          "absent M115 fields stay empty");

    check(printerctl_marlin_parse_firmware_info(
              "FIRMWARE_NAME:Marlin EXTRUDER_COUNT:banana", &info) == PRINTERCTL_MARLIN_OK &&
              !info.has_extruder_count,
          "an unparsable EXTRUDER_COUNT yields has_extruder_count == false");

    check(printerctl_marlin_parse_firmware_info("ok", &info) == PRINTERCTL_MARLIN_ERR_NO_MATCH,
          "a line without FIRMWARE_NAME is NO_MATCH");
    check(info.firmware_name[0] == '\0', "NO_MATCH zeroes the info struct");

    check(printerctl_marlin_parse_firmware_info(NULL, &info) ==
              PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "firmware info parse with a NULL line is rejected");
    check(printerctl_marlin_parse_firmware_info("FIRMWARE_NAME:x", NULL) ==
              PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT,
          "firmware info parse with a NULL out is rejected");
}

static void test_firmware_info_field_truncation(void)
{
    /* firmware name longer than PRINTERCTL_MARLIN_INFO_FIELD_MAX - 1 */
    char line[600];
    int written = snprintf(line, sizeof(line), "FIRMWARE_NAME:");
    for (int i = 0; i < 300; i++) {
        line[written + i] = 'A';
    }
    line[written + 300] = '\0';

    printerctl_marlin_firmware_info_t info;
    check(printerctl_marlin_parse_firmware_info(line, &info) == PRINTERCTL_MARLIN_OK,
          "an oversized firmware name still parses");
    check(strlen(info.firmware_name) == PRINTERCTL_MARLIN_INFO_FIELD_MAX - 1,
          "an oversized firmware name is truncated to the field size");
}

int main(void)
{
    test_classify_ack();
    test_classify_error();
    test_classify_busy_and_echo();
    test_classify_resend();
    test_classify_info();
    test_classify_invalid_arguments();
    test_parse_temperature();
    test_parse_temperature_from_classified_ok_payload();
    test_parse_position();
    test_parse_firmware_info();
    test_firmware_info_field_truncation();

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All Marlin checks passed\n");
    return EXIT_SUCCESS;
}
