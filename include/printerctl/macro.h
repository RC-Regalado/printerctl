#ifndef PRINTERCTL_MACRO_H
#define PRINTERCTL_MACRO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define PRINTERCTL_MACRO_ID_MAX 64
#define PRINTERCTL_MACRO_DISPLAY_NAME_MAX 96
#define PRINTERCTL_MACRO_DESCRIPTION_MAX 256
#define PRINTERCTL_MACRO_STEPS_MAX 32
#define PRINTERCTL_MACRO_ERROR_MAX 256

typedef enum {
    PRINTERCTL_MACRO_OK = 0,
    PRINTERCTL_MACRO_NOT_FOUND,
    PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT,
    PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
    PRINTERCTL_MACRO_ERR_DUPLICATE_ID
} printerctl_macro_status_t;

typedef enum {
    PRINTERCTL_MACRO_CATEGORY_CALIBRATION = 1,
    PRINTERCTL_MACRO_CATEGORY_MAINTENANCE,
    PRINTERCTL_MACRO_CATEGORY_MOTION,
    PRINTERCTL_MACRO_CATEGORY_THERMAL,
    PRINTERCTL_MACRO_CATEGORY_FILAMENT,
    PRINTERCTL_MACRO_CATEGORY_DIAGNOSTIC
} printerctl_macro_category_t;

typedef enum {
    PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY = 1
} printerctl_macro_required_state_t;

enum {
    PRINTERCTL_MACRO_CAP_MOVE = 1u << 0,
    PRINTERCTL_MACRO_CAP_HEAT = 1u << 1,
    PRINTERCTL_MACRO_CAP_EXTRUDE = 1u << 2,
    PRINTERCTL_MACRO_CAP_FAN = 1u << 3
};

#define PRINTERCTL_MACRO_CAP_KNOWN_MASK \
    (PRINTERCTL_MACRO_CAP_MOVE | PRINTERCTL_MACRO_CAP_HEAT | \
     PRINTERCTL_MACRO_CAP_EXTRUDE | PRINTERCTL_MACRO_CAP_FAN)

typedef enum {
    PRINTERCTL_MACRO_STEP_MOVE_ABSOLUTE = 1,
    PRINTERCTL_MACRO_STEP_MOVE_RELATIVE
} printerctl_macro_step_type_t;

enum {
    PRINTERCTL_MACRO_AXIS_X = 1u << 0,
    PRINTERCTL_MACRO_AXIS_Y = 1u << 1,
    PRINTERCTL_MACRO_AXIS_Z = 1u << 2
};

#define PRINTERCTL_MACRO_AXIS_KNOWN_MASK \
    (PRINTERCTL_MACRO_AXIS_X | PRINTERCTL_MACRO_AXIS_Y | PRINTERCTL_MACRO_AXIS_Z)

typedef struct {
    printerctl_macro_step_type_t type;
    uint8_t axes;
    double x;
    double y;
    double z;
    double feedrate_mm_min;
} printerctl_macro_step_t;

typedef struct {
    char id[PRINTERCTL_MACRO_ID_MAX];
    char display_name[PRINTERCTL_MACRO_DISPLAY_NAME_MAX];
    char description[PRINTERCTL_MACRO_DESCRIPTION_MAX];
    printerctl_macro_category_t category;
    printerctl_macro_required_state_t required_state;
    uint32_t capabilities;
    size_t step_count;
    const printerctl_macro_step_t *steps;
} printerctl_macro_definition_t;

typedef struct {
    printerctl_macro_status_t status;
    char message[PRINTERCTL_MACRO_ERROR_MAX];
} printerctl_macro_error_t;

typedef struct {
    bool has_min_x;
    bool has_max_x;
    bool has_min_y;
    bool has_max_y;
    bool has_safe_z;
    bool has_xy_feedrate;
    bool has_z_feedrate;
    double min_x;
    double max_x;
    double min_y;
    double max_y;
    double safe_z;
    double xy_feedrate;
    double z_feedrate;
} printerctl_macro_calibration_config_t;

printerctl_macro_status_t printerctl_macro_validate_definition(
    const printerctl_macro_definition_t *definition,
    printerctl_macro_error_t *out_error);

printerctl_macro_status_t printerctl_macro_validate_registry(
    const printerctl_macro_definition_t *definitions,
    size_t count,
    printerctl_macro_error_t *out_error);

size_t printerctl_macro_builtin_count(void);

const printerctl_macro_definition_t *printerctl_macro_builtin_at(size_t index);

printerctl_macro_status_t printerctl_macro_builtin_find(
    const char *id,
    const printerctl_macro_definition_t **out_definition);

printerctl_macro_status_t printerctl_macro_resolve_builtin(
    const char *id,
    const printerctl_macro_calibration_config_t *config,
    printerctl_macro_definition_t *out_definition,
    printerctl_macro_step_t *out_steps,
    size_t out_step_capacity,
    printerctl_macro_error_t *out_error);

#endif
