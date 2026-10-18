#ifndef PRINTERCTL_PRUSA_IMPORT_H
#define PRINTERCTL_PRUSA_IMPORT_H

/*
 * M29 PrusaSlicer configuration import: derives the daemon-owned
 * `calibration.*` keys (M28) from a PrusaSlicer printer profile (.ini)
 * or from the configuration block PrusaSlicer appends to an ASCII
 * .gcode file. Pure, local and hardware-independent: it never opens a
 * serial device, never talks to printerctld and never writes a file --
 * the caller decides where the generated text goes.
 *
 * Mapping (every derived value is printed as a comment by
 * printerctl_prusa_import_write_config() so the operator can audit it):
 *
 *   bed_shape                    -> bed bounding box; calibration
 *                                   min/max X/Y = box shrunk by inset_mm
 *   travel_speed,
 *   machine_max_feedrate_x/_y    -> xy_feedrate = 60 * min(available) mm/min
 *   travel_speed_z (> 0),
 *   machine_max_feedrate_z       -> z_feedrate  = 60 * min(available) mm/min
 *   max_print_height (> 0)       -> upper bound check for safe_z
 *
 * `safe_z` is not a PrusaSlicer setting; it comes from options.safe_z_mm.
 * Machine limits use the first (normal-mode) value of a "normal,silent"
 * pair. Values that are absent or zero are treated as unavailable, never
 * as "unlimited". Config bundles (files with [section] headers) and
 * binary G-code are rejected rather than guessed.
 */

#include <stdbool.h>
#include <stdio.h>

#define PRINTERCTL_PRUSA_IMPORT_ERROR_MAX 256
#define PRINTERCTL_PRUSA_IMPORT_DEFAULT_INSET_MM 30.0
#define PRINTERCTL_PRUSA_IMPORT_DEFAULT_SAFE_Z_MM 5.0

typedef enum {
    PRINTERCTL_PRUSA_IMPORT_OK = 0,
    PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_ARGUMENT,
    PRINTERCTL_PRUSA_IMPORT_ERR_OPEN,
    PRINTERCTL_PRUSA_IMPORT_ERR_READ,
    PRINTERCTL_PRUSA_IMPORT_ERR_UNSUPPORTED_FORMAT,
    PRINTERCTL_PRUSA_IMPORT_ERR_MISSING_KEY,
    PRINTERCTL_PRUSA_IMPORT_ERR_INVALID_VALUE
} printerctl_prusa_import_status_t;

typedef struct {
    printerctl_prusa_import_status_t status;
    char message[PRINTERCTL_PRUSA_IMPORT_ERROR_MAX];
} printerctl_prusa_import_error_t;

typedef struct {
    double inset_mm;  /* >= 0; distance kept from every bed edge */
    double safe_z_mm; /* > 0; relative Z lift used by the macros */
} printerctl_prusa_import_options_t;

typedef struct {
    /* Source values (mm, mm/s). has_* is false when absent or zero. */
    double bed_min_x;
    double bed_max_x;
    double bed_min_y;
    double bed_max_y;
    bool has_travel_speed;
    double travel_speed;
    bool has_travel_speed_z;
    double travel_speed_z;
    bool has_max_feedrate_x;
    double max_feedrate_x;
    bool has_max_feedrate_y;
    double max_feedrate_y;
    bool has_max_feedrate_z;
    double max_feedrate_z;
    bool has_max_print_height;
    double max_print_height;
    bool from_gcode; /* true when read from a .gcode configuration block */

    /* Derived printerctld calibration.* values (mm, mm/min). */
    double inset_mm;
    double min_x;
    double max_x;
    double min_y;
    double max_y;
    double safe_z;
    double xy_feedrate;
    double z_feedrate;
} printerctl_prusa_import_result_t;

void printerctl_prusa_import_options_init(printerctl_prusa_import_options_t *options);

/*
 * Reads `path`, derives the calibration geometry and validates it with
 * the same rules the daemon applies before executing a macro (finite,
 * min < max, positive safe_z and feedrates). *out_result is fully
 * overwritten on success and zeroed on failure.
 */
printerctl_prusa_import_status_t printerctl_prusa_import_file(
    const char *path,
    const printerctl_prusa_import_options_t *options,
    printerctl_prusa_import_result_t *out_result,
    printerctl_prusa_import_error_t *out_error);

/*
 * Writes a printerctld key=value configuration (calibration.* keys plus
 * explanatory comments; serial keys are emitted commented out because a
 * PrusaSlicer printer profile does not carry them). Returns false if the
 * stream reports an error.
 */
bool printerctl_prusa_import_write_config(const printerctl_prusa_import_result_t *result,
                                          const char *source_label, FILE *out);

#endif
