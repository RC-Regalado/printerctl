#define _POSIX_C_SOURCE 200809L

#include "printerctl/prusa_import.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define PRUSA_IMPORT_LINE_MAX 4096
#define PRUSA_IMPORT_VALUE_MAX 1024

static void set_error(printerctl_prusa_import_error_t *out_error,
                      printerctl_prusa_import_status_t status, const char *format, ...)
{
    out_error->status = status;
    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

void printerctl_prusa_import_options_init(printerctl_prusa_import_options_t *options)
{
    if (options == NULL) {
        return;
    }
    options->inset_mm = PRINTERCTL_PRUSA_IMPORT_DEFAULT_INSET_MM;
    options->safe_z_mm = PRINTERCTL_PRUSA_IMPORT_DEFAULT_SAFE_Z_MM;
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[--length] = '\0';
    }
    return text;
}

/* Parses one finite number occupying the whole of `text` (surrounding
 * blanks already trimmed). */
static bool parse_number(const char *text, double *out_value)
{
    if (text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value)) {
        return false;
    }
    *out_value = value;
    return true;
}

/* First element of a PrusaSlicer "normal,silent" machine-limit pair. */
static bool parse_first_number(const char *text, double *out_value)
{
    char buffer[64];
    size_t length = strcspn(text, ",");
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    return parse_number(trim(buffer), out_value);
}

/* bed_shape = 0x0,235x0,235x235,0x235 -> bounding box of the polygon. */
static bool parse_bed_shape(const char *text, printerctl_prusa_import_result_t *result)
{
    char buffer[PRUSA_IMPORT_VALUE_MAX];
    if (strlen(text) >= sizeof(buffer)) {
        return false;
    }
    snprintf(buffer, sizeof(buffer), "%s", text);

    size_t points = 0;
    char *save = NULL;
    for (char *point = strtok_r(buffer, ",", &save); point != NULL;
         point = strtok_r(NULL, ",", &save)) {
        char *separator = strchr(point, 'x');
        if (separator == NULL) {
            return false;
        }
        *separator = '\0';
        double x = 0.0;
        double y = 0.0;
        if (!parse_number(trim(point), &x) || !parse_number(trim(separator + 1), &y)) {
            return false;
        }
        if (points == 0) {
            result->bed_min_x = result->bed_max_x = x;
            result->bed_min_y = result->bed_max_y = y;
        } else {
            result->bed_min_x = fmin(result->bed_min_x, x);
            result->bed_max_x = fmax(result->bed_max_x, x);
            result->bed_min_y = fmin(result->bed_min_y, y);
            result->bed_max_y = fmax(result->bed_max_y, y);
        }
        points++;
    }
    return points >= 3 && result->bed_min_x < result->bed_max_x &&
           result->bed_min_y < result->bed_max_y;
}

typedef struct {
    bool seen_bed_shape;
    bool seen_travel_speed;
    bool seen_travel_speed_z;
    bool seen_max_x;
    bool seen_max_y;
    bool seen_max_z;
    bool seen_max_height;
} seen_keys_t;

/* Optional positive speed/height: absent and zero both mean unavailable. */
static printerctl_prusa_import_status_t apply_optional(
    const char *key, const char *value, bool first_of_pair, bool *seen, bool *has,
    double *target, printerctl_prusa_import_error_t *out_error)
{
    if (*seen) {
        return PRINTERCTL_PRUSA_IMPORT_OK; /* first occurrence wins */
    }
    *seen = true;
    double parsed = 0.0;
    bool ok = first_of_pair ? parse_first_number(value, &parsed) : parse_number(value, &parsed);
    if (!ok || parsed < 0.0) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_VALUE,
                  "invalid value for '%s': '%s'", key, value);
        return out_error->status;
    }
    if (parsed > 0.0) {
        *has = true;
        *target = parsed;
    }
    return PRINTERCTL_PRUSA_IMPORT_OK;
}

static printerctl_prusa_import_status_t apply_key(const char *key, const char *value,
                                                  seen_keys_t *seen,
                                                  printerctl_prusa_import_result_t *result,
                                                  printerctl_prusa_import_error_t *out_error)
{
    if (strcmp(key, "bed_shape") == 0) {
        if (seen->seen_bed_shape) {
            return PRINTERCTL_PRUSA_IMPORT_OK;
        }
        seen->seen_bed_shape = true;
        if (!parse_bed_shape(value, result)) {
            set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_VALUE,
                      "invalid bed_shape '%s' (expected at least three XxY points)", value);
            return out_error->status;
        }
        return PRINTERCTL_PRUSA_IMPORT_OK;
    }
    if (strcmp(key, "travel_speed") == 0) {
        return apply_optional(key, value, false, &seen->seen_travel_speed,
                              &result->has_travel_speed, &result->travel_speed, out_error);
    }
    if (strcmp(key, "travel_speed_z") == 0) {
        return apply_optional(key, value, false, &seen->seen_travel_speed_z,
                              &result->has_travel_speed_z, &result->travel_speed_z, out_error);
    }
    if (strcmp(key, "machine_max_feedrate_x") == 0) {
        return apply_optional(key, value, true, &seen->seen_max_x, &result->has_max_feedrate_x,
                              &result->max_feedrate_x, out_error);
    }
    if (strcmp(key, "machine_max_feedrate_y") == 0) {
        return apply_optional(key, value, true, &seen->seen_max_y, &result->has_max_feedrate_y,
                              &result->max_feedrate_y, out_error);
    }
    if (strcmp(key, "machine_max_feedrate_z") == 0) {
        return apply_optional(key, value, true, &seen->seen_max_z, &result->has_max_feedrate_z,
                              &result->max_feedrate_z, out_error);
    }
    if (strcmp(key, "max_print_height") == 0) {
        return apply_optional(key, value, false, &seen->seen_max_height,
                              &result->has_max_print_height, &result->max_print_height,
                              out_error);
    }
    return PRINTERCTL_PRUSA_IMPORT_OK;
}

/* Splits "key = value" in place. Returns false for anything else. */
static bool split_key_value(char *line, char **out_key, char **out_value)
{
    char *equals = strchr(line, '=');
    if (equals == NULL) {
        return false;
    }
    *equals = '\0';
    *out_key = trim(line);
    *out_value = trim(equals + 1);
    return (*out_key)[0] != '\0' && strchr(*out_key, ' ') == NULL;
}

static printerctl_prusa_import_status_t read_settings(FILE *file,
                                                      printerctl_prusa_import_result_t *result,
                                                      seen_keys_t *seen,
                                                      printerctl_prusa_import_error_t *out_error)
{
    char line[PRUSA_IMPORT_LINE_MAX];
    bool first_line = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        bool complete = length > 0 && line[length - 1] == '\n';
        if (!complete && !feof(file)) {
            /* Overlong lines (thumbnails, custom G-code) carry none of the
             * keys this importer reads: discard the remainder. */
            int c;
            while ((c = fgetc(file)) != EOF && c != '\n') {
            }
            first_line = false;
            continue;
        }

        if (first_line && length >= 4 && memcmp(line, "GCDE", 4) == 0) {
            set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_UNSUPPORTED_FORMAT,
                      "binary G-code (.bgcode) is not supported; export ASCII G-code "
                      "or the printer profile (.ini)");
            return out_error->status;
        }
        first_line = false;

        char *text = trim(line);
        bool comment = false;
        if (text[0] == '[') {
            set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_UNSUPPORTED_FORMAT,
                      "config bundles with [sections] are not supported; export a single "
                      "printer profile (.ini) or use a sliced .gcode file");
            return out_error->status;
        }
        if (text[0] == ';') {
            comment = true;
            text = trim(text + 1);
        } else if (text[0] == '#') {
            continue;
        }

        char *key = NULL;
        char *value = NULL;
        if (!split_key_value(text, &key, &value)) {
            continue;
        }
        bool was_bed_shape_seen = seen->seen_bed_shape;
        printerctl_prusa_import_status_t status = apply_key(key, value, seen, result, out_error);
        if (status != PRINTERCTL_PRUSA_IMPORT_OK) {
            return status;
        }
        if (comment && !was_bed_shape_seen && seen->seen_bed_shape) {
            result->from_gcode = true;
        }
    }
    if (ferror(file)) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_READ, "read failed: %s",
                  strerror(errno));
        return out_error->status;
    }
    return PRINTERCTL_PRUSA_IMPORT_OK;
}

static bool min_available(double *out_value, size_t count, const bool *has, const double *values)
{
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (has[i] && (!found || values[i] < *out_value)) {
            *out_value = values[i];
            found = true;
        }
    }
    return found;
}

static printerctl_prusa_import_status_t derive(const printerctl_prusa_import_options_t *options,
                                               printerctl_prusa_import_result_t *result,
                                               printerctl_prusa_import_error_t *out_error)
{
    double inset = options->inset_mm;
    double safe_z = options->safe_z_mm;
    if (!isfinite(inset) || inset < 0.0) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_ARGUMENT,
                  "inset must be a finite value >= 0 mm");
        return out_error->status;
    }
    if (!isfinite(safe_z) || safe_z <= 0.0) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_ARGUMENT,
                  "safe_z must be a finite value > 0 mm");
        return out_error->status;
    }

    result->inset_mm = inset;
    result->min_x = result->bed_min_x + inset;
    result->max_x = result->bed_max_x - inset;
    result->min_y = result->bed_min_y + inset;
    result->max_y = result->bed_max_y - inset;
    if (!(result->min_x < result->max_x) || !(result->min_y < result->max_y)) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_ARGUMENT,
                  "inset %.3f mm leaves no usable area on a %.3f x %.3f mm bed", inset,
                  result->bed_max_x - result->bed_min_x, result->bed_max_y - result->bed_min_y);
        return out_error->status;
    }

    if (result->has_max_print_height && safe_z >= result->max_print_height) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_ARGUMENT,
                  "safe_z %.3f mm is not below max_print_height %.3f mm", safe_z,
                  result->max_print_height);
        return out_error->status;
    }
    result->safe_z = safe_z;

    const bool xy_has[] = {result->has_travel_speed, result->has_max_feedrate_x,
                           result->has_max_feedrate_y};
    const double xy_values[] = {result->travel_speed, result->max_feedrate_x,
                                result->max_feedrate_y};
    double xy = 0.0;
    if (!min_available(&xy, 3, xy_has, xy_values)) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_MISSING_KEY,
                  "cannot derive an XY feedrate: need travel_speed or "
                  "machine_max_feedrate_x/machine_max_feedrate_y");
        return out_error->status;
    }

    const bool z_has[] = {result->has_travel_speed_z, result->has_max_feedrate_z};
    const double z_values[] = {result->travel_speed_z, result->max_feedrate_z};
    double z = 0.0;
    if (!min_available(&z, 2, z_has, z_values)) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_MISSING_KEY,
                  "cannot derive a Z feedrate: need machine_max_feedrate_z or travel_speed_z");
        return out_error->status;
    }

    result->xy_feedrate = xy * 60.0;
    result->z_feedrate = z * 60.0;
    return PRINTERCTL_PRUSA_IMPORT_OK;
}

printerctl_prusa_import_status_t printerctl_prusa_import_file(
    const char *path,
    const printerctl_prusa_import_options_t *options,
    printerctl_prusa_import_result_t *out_result,
    printerctl_prusa_import_error_t *out_error)
{
    if (out_error == NULL) {
        return PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_ARGUMENT;
    }
    memset(out_error, 0, sizeof(*out_error));
    if (path == NULL || options == NULL || out_result == NULL) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_ARGUMENT,
                  "path, options and out_result must not be NULL");
        return out_error->status;
    }
    memset(out_result, 0, sizeof(*out_result));

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_OPEN, "cannot open '%s': %s", path,
                  strerror(errno));
        return out_error->status;
    }

    seen_keys_t seen;
    memset(&seen, 0, sizeof(seen));
    printerctl_prusa_import_status_t status = read_settings(file, out_result, &seen, out_error);
    fclose(file);

    if (status == PRINTERCTL_PRUSA_IMPORT_OK && !seen.seen_bed_shape) {
        set_error(out_error, PRINTERCTL_PRUSA_IMPORT_ERR_MISSING_KEY,
                  "no bed_shape found: not a PrusaSlicer printer profile or sliced G-code "
                  "with an embedded configuration");
        status = out_error->status;
    }
    if (status == PRINTERCTL_PRUSA_IMPORT_OK) {
        status = derive(options, out_result, out_error);
    }
    if (status != PRINTERCTL_PRUSA_IMPORT_OK) {
        memset(out_result, 0, sizeof(*out_result));
    }
    return status;
}

static void write_speed_source(FILE *out, bool *first, const char *name, bool has, double value)
{
    if (!has) {
        return;
    }
    fprintf(out, "%s%s=%g", *first ? "" : ", ", name, value);
    *first = false;
}

bool printerctl_prusa_import_write_config(const printerctl_prusa_import_result_t *result,
                                          const char *source_label, FILE *out)
{
    if (result == NULL || out == NULL) {
        return false;
    }

    fprintf(out, "# printerctld configuration generated by 'printerctl config from-prusa'\n");
    fprintf(out, "# source: %s (%s)\n", source_label != NULL ? source_label : "(unknown)",
            result->from_gcode ? "sliced G-code configuration block"
                               : "PrusaSlicer printer profile");
    fprintf(out, "# bed_shape: X %g..%g mm, Y %g..%g mm; calibration inset %g mm\n",
            result->bed_min_x, result->bed_max_x, result->bed_min_y, result->bed_max_y,
            result->inset_mm);

    bool first = true;
    fprintf(out, "# xy_feedrate: 60 * min(");
    write_speed_source(out, &first, "travel_speed", result->has_travel_speed,
                       result->travel_speed);
    write_speed_source(out, &first, "machine_max_feedrate_x", result->has_max_feedrate_x,
                       result->max_feedrate_x);
    write_speed_source(out, &first, "machine_max_feedrate_y", result->has_max_feedrate_y,
                       result->max_feedrate_y);
    fprintf(out, ") mm/s\n");

    first = true;
    fprintf(out, "# z_feedrate: 60 * min(");
    write_speed_source(out, &first, "travel_speed_z", result->has_travel_speed_z,
                       result->travel_speed_z);
    write_speed_source(out, &first, "machine_max_feedrate_z", result->has_max_feedrate_z,
                       result->max_feedrate_z);
    fprintf(out, ") mm/s\n");
    fprintf(out, "# safe_z is not a PrusaSlicer setting (operator-chosen relative lift)\n");
    fprintf(out, "# The macros do not home: run 'printerctl home' before using them.\n");
    fprintf(out, "#\n");
    fprintf(out, "# Serial settings are not part of a PrusaSlicer printer profile:\n");
    fprintf(out, "# port=/dev/ttyUSB0\n");
    fprintf(out, "# baud=115200\n");
    fprintf(out, "calibration.min_x=%.3f\n", result->min_x);
    fprintf(out, "calibration.max_x=%.3f\n", result->max_x);
    fprintf(out, "calibration.min_y=%.3f\n", result->min_y);
    fprintf(out, "calibration.max_y=%.3f\n", result->max_y);
    fprintf(out, "calibration.safe_z=%.3f\n", result->safe_z);
    fprintf(out, "calibration.xy_feedrate=%.3f\n", result->xy_feedrate);
    fprintf(out, "calibration.z_feedrate=%.3f\n", result->z_feedrate);
    return fflush(out) == 0 && !ferror(out);
}
