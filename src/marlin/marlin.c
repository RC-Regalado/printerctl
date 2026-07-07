#include "printerctl/marlin.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool has_prefix(const char *line, const char *prefix)
{
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void reset_response(printerctl_marlin_response_t *out)
{
    out->type = PRINTERCTL_MARLIN_RESPONSE_INFO;
    out->terminal = false;
    out->payload = "";
    out->has_resend_line = false;
    out->resend_line = 0;
}

printerctl_marlin_status_t printerctl_marlin_classify(const char *line,
                                                        printerctl_marlin_response_t *out)
{
    if (out != NULL) {
        reset_response(out);
    }
    if (line == NULL || out == NULL) {
        return PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT;
    }

    if (strcmp(line, "ok") == 0 || has_prefix(line, "ok ")) {
        out->type = PRINTERCTL_MARLIN_RESPONSE_ACK;
        out->terminal = true;
        out->payload = skip_spaces(line + 2);
        return PRINTERCTL_MARLIN_OK;
    }

    /* echo:busy: must win over the plain echo: prefix. */
    if (has_prefix(line, "echo:busy:")) {
        out->type = PRINTERCTL_MARLIN_RESPONSE_BUSY;
        out->payload = skip_spaces(line + strlen("echo:busy:"));
        return PRINTERCTL_MARLIN_OK;
    }

    if (has_prefix(line, "busy:")) {
        out->type = PRINTERCTL_MARLIN_RESPONSE_BUSY;
        out->payload = skip_spaces(line + strlen("busy:"));
        return PRINTERCTL_MARLIN_OK;
    }

    if (has_prefix(line, "echo:")) {
        out->type = PRINTERCTL_MARLIN_RESPONSE_ECHO;
        out->payload = skip_spaces(line + strlen("echo:"));
        return PRINTERCTL_MARLIN_OK;
    }

    if (has_prefix(line, "Error:")) {
        out->type = PRINTERCTL_MARLIN_RESPONSE_ERROR;
        out->terminal = true;
        out->payload = skip_spaces(line + strlen("Error:"));
        return PRINTERCTL_MARLIN_OK;
    }

    if (has_prefix(line, "Resend:")) {
        out->type = PRINTERCTL_MARLIN_RESPONSE_RESEND;
        out->terminal = true;
        out->payload = skip_spaces(line + strlen("Resend:"));

        const char *digits = out->payload;
        if (isdigit((unsigned char)*digits)) {
            uint64_t value = 0;
            const char *p = digits;
            while (isdigit((unsigned char)*p)) {
                value = value * 10 + (uint64_t)(*p - '0');
                p++;
            }
            /* Only a clean number (optionally followed by whitespace)
             * counts; `Resend: 5x` reports no usable line number. */
            if (*skip_spaces(p) == '\0') {
                out->has_resend_line = true;
                out->resend_line = value;
            }
        }
        return PRINTERCTL_MARLIN_OK;
    }

    out->payload = line;
    return PRINTERCTL_MARLIN_OK;
}

/*
 * Parses `actual/target` (optional spaces around `/`) starting at `s`,
 * which must point just past a sensor key like `T:` or `B:`. Returns the
 * position after the target on success, NULL when the shape does not
 * match.
 */
static const char *parse_actual_target(const char *s, double *out_actual, double *out_target)
{
    char *end;

    s = skip_spaces(s);
    double actual = strtod(s, &end);
    if (end == s) {
        return NULL;
    }

    s = skip_spaces(end);
    if (*s != '/') {
        return NULL;
    }
    s = skip_spaces(s + 1);

    double target = strtod(s, &end);
    if (end == s) {
        return NULL;
    }

    *out_actual = actual;
    *out_target = target;
    return end;
}

/*
 * True when line[pos] starts the token `key` at a token boundary: the
 * start of the line or right after whitespace. Prevents `B@:` (bed power)
 * or `X:` inside `Count X:` from being misread — callers additionally
 * bound their scan where needed.
 */
static bool at_token(const char *line, size_t pos, const char *key)
{
    if (pos > 0 && line[pos - 1] != ' ' && line[pos - 1] != '\t') {
        return false;
    }
    return strncmp(line + pos, key, strlen(key)) == 0;
}

/* Finds the first `key` at a token boundary within line[0, limit); returns its index or -1. */
static long find_token(const char *line, size_t limit, const char *key)
{
    size_t key_length = strlen(key);
    for (size_t pos = 0; pos + key_length <= limit; pos++) {
        if (at_token(line, pos, key)) {
            return (long)pos;
        }
    }
    return -1;
}

printerctl_marlin_status_t printerctl_marlin_parse_temperature(
    const char *line,
    printerctl_marlin_temperature_report_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (line == NULL || out == NULL) {
        return PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT;
    }

    size_t length = strlen(line);

    long t_pos = find_token(line, length, "T:");
    if (t_pos >= 0) {
        double actual;
        double target;
        if (parse_actual_target(line + t_pos + 2, &actual, &target) != NULL) {
            out->has_hotend = true;
            out->hotend_actual = actual;
            out->hotend_target = target;
        }
    }

    long b_pos = find_token(line, length, "B:");
    if (b_pos >= 0) {
        double actual;
        double target;
        if (parse_actual_target(line + b_pos + 2, &actual, &target) != NULL) {
            out->has_bed = true;
            out->bed_actual = actual;
            out->bed_target = target;
        }
    }

    if (!out->has_hotend && !out->has_bed) {
        memset(out, 0, sizeof(*out));
        return PRINTERCTL_MARLIN_ERR_NO_MATCH;
    }

    return PRINTERCTL_MARLIN_OK;
}

/* Parses one `KEY:number` axis value at a token boundary within line[0, limit). */
static bool parse_axis(const char *line, size_t limit, const char *key, double *out_value)
{
    long pos = find_token(line, limit, key);
    if (pos < 0) {
        return false;
    }

    const char *s = line + (size_t)pos + strlen(key);
    char *end;
    double value = strtod(s, &end);
    if (end == s) {
        return false;
    }

    *out_value = value;
    return true;
}

printerctl_marlin_status_t printerctl_marlin_parse_position(
    const char *line,
    printerctl_marlin_position_report_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (line == NULL || out == NULL) {
        return PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT;
    }

    /* Only the logical part before `Count` is decoded; the stepper counts
     * after it repeat X:/Y:/Z: with different units. */
    size_t limit = strlen(line);
    long count_pos = find_token(line, limit, "Count");
    if (count_pos >= 0) {
        limit = (size_t)count_pos;
    }

    double x;
    double y;
    double z;
    if (!parse_axis(line, limit, "X:", &x) ||
        !parse_axis(line, limit, "Y:", &y) ||
        !parse_axis(line, limit, "Z:", &z)) {
        memset(out, 0, sizeof(*out));
        return PRINTERCTL_MARLIN_ERR_NO_MATCH;
    }

    out->x = x;
    out->y = y;
    out->z = z;

    double e;
    if (parse_axis(line, limit, "E:", &e)) {
        out->has_e = true;
        out->e = e;
    }

    return PRINTERCTL_MARLIN_OK;
}

static const char *const info_keys[] = {
    "FIRMWARE_NAME:",
    "SOURCE_CODE_URL:",
    "PROTOCOL_VERSION:",
    "MACHINE_TYPE:",
    "EXTRUDER_COUNT:",
    "UUID:",
};

/*
 * Copies the value of `key` out of an M115 line into `field`: the text
 * from just past the key up to the next known key (or end of line), with
 * surrounding whitespace trimmed and silent truncation to the field size.
 * Returns false when the key is absent, leaving `field` empty.
 */
static bool extract_info_field(const char *line, const char *key,
                                char *field, size_t field_size)
{
    field[0] = '\0';

    size_t length = strlen(line);
    long pos = find_token(line, length, key);
    if (pos < 0) {
        return false;
    }

    const char *start = skip_spaces(line + (size_t)pos + strlen(key));

    size_t end = length;
    for (size_t i = 0; i < sizeof(info_keys) / sizeof(info_keys[0]); i++) {
        long next = find_token(line, length, info_keys[i]);
        if (next >= 0 && (size_t)next > (size_t)pos && (size_t)next < end) {
            end = (size_t)next;
        }
    }

    size_t value_length = (size_t)(line + end - start);
    while (value_length > 0 &&
           (start[value_length - 1] == ' ' || start[value_length - 1] == '\t')) {
        value_length--;
    }

    if (value_length >= field_size) {
        value_length = field_size - 1;
    }
    memcpy(field, start, value_length);
    field[value_length] = '\0';
    return true;
}

printerctl_marlin_status_t printerctl_marlin_parse_firmware_info(
    const char *line,
    printerctl_marlin_firmware_info_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (line == NULL || out == NULL) {
        return PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT;
    }

    if (!extract_info_field(line, "FIRMWARE_NAME:",
                             out->firmware_name, sizeof(out->firmware_name)) ||
        out->firmware_name[0] == '\0') {
        memset(out, 0, sizeof(*out));
        return PRINTERCTL_MARLIN_ERR_NO_MATCH;
    }

    extract_info_field(line, "MACHINE_TYPE:", out->machine_type, sizeof(out->machine_type));
    extract_info_field(line, "PROTOCOL_VERSION:",
                        out->protocol_version, sizeof(out->protocol_version));
    extract_info_field(line, "UUID:", out->uuid, sizeof(out->uuid));

    char count_text[16];
    if (extract_info_field(line, "EXTRUDER_COUNT:", count_text, sizeof(count_text)) &&
        count_text[0] != '\0') {
        char *end;
        long value = strtol(count_text, &end, 10);
        /* upper bound guards the int cast; no real printer is near it */
        if (end != count_text && *end == '\0' && value >= 0 && value <= 1000) {
            out->has_extruder_count = true;
            out->extruder_count = (int)value;
        }
    }

    return PRINTERCTL_MARLIN_OK;
}
