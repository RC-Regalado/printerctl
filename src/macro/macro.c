#include "printerctl/macro.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_error(printerctl_macro_error_t *out_error,
                      printerctl_macro_status_t status,
                      const char *format, ...)
{
    if (out_error == NULL) {
        return;
    }
    out_error->status = status;
    va_list args;
    va_start(args, format);
    vsnprintf(out_error->message, sizeof(out_error->message), format, args);
    va_end(args);
}

static int valid_category(printerctl_macro_category_t category)
{
    return category >= PRINTERCTL_MACRO_CATEGORY_CALIBRATION &&
           category <= PRINTERCTL_MACRO_CATEGORY_DIAGNOSTIC;
}

static int valid_required_state(printerctl_macro_required_state_t state)
{
    return state == PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY;
}

static int valid_id(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; id[i] != '\0'; ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return 0;
        }
    }
    return strlen(id) < PRINTERCTL_MACRO_ID_MAX;
}

static int move_step_type(printerctl_macro_step_type_t type)
{
    return type == PRINTERCTL_MACRO_STEP_MOVE_ABSOLUTE ||
           type == PRINTERCTL_MACRO_STEP_MOVE_RELATIVE;
}

printerctl_macro_status_t printerctl_macro_validate_definition(
    const printerctl_macro_definition_t *definition,
    printerctl_macro_error_t *out_error)
{
    set_error(out_error, PRINTERCTL_MACRO_OK, "");
    if (definition == NULL) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT, "definition must not be NULL");
        return PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT;
    }
    if (!valid_id(definition->id) || definition->display_name[0] == '\0' ||
        definition->description[0] == '\0') {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "macro id, display name and description must be present");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    if (!valid_category(definition->category)) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION, "invalid category");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    if (!valid_required_state(definition->required_state)) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION, "invalid required state");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    if (definition->capabilities == 0 ||
        (definition->capabilities & ~((uint32_t)PRINTERCTL_MACRO_CAP_KNOWN_MASK)) != 0) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "invalid capability mask");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    if (definition->step_count == 0 || definition->step_count > PRINTERCTL_MACRO_STEPS_MAX ||
        definition->steps == NULL) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "macro must have 1..%u steps", PRINTERCTL_MACRO_STEPS_MAX);
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }

    for (size_t i = 0; i < definition->step_count; ++i) {
        const printerctl_macro_step_t *step = &definition->steps[i];
        if (!move_step_type(step->type)) {
            set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                      "unknown step type at index %zu", i);
            return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
        }
        if ((definition->capabilities & PRINTERCTL_MACRO_CAP_MOVE) == 0) {
            set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                      "MOVE step requires MOVE capability");
            return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
        }
        if (step->axes == 0 || (step->axes & ~((uint8_t)PRINTERCTL_MACRO_AXIS_KNOWN_MASK)) != 0) {
            set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                      "invalid axes at index %zu", i);
            return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
        }
        if (!isfinite(step->x) || !isfinite(step->y) || !isfinite(step->z) ||
            !isfinite(step->feedrate_mm_min) || step->feedrate_mm_min <= 0.0) {
            set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                      "invalid numeric value at index %zu", i);
            return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
        }
    }

    return PRINTERCTL_MACRO_OK;
}

printerctl_macro_status_t printerctl_macro_validate_registry(
    const printerctl_macro_definition_t *definitions,
    size_t count,
    printerctl_macro_error_t *out_error)
{
    set_error(out_error, PRINTERCTL_MACRO_OK, "");
    if (count > 0 && definitions == NULL) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT,
                  "definitions must not be NULL");
        return PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!valid_id(definitions[i].id)) {
            set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                      "invalid id at index %zu", i);
            return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
        }
        for (size_t j = i + 1; j < count; ++j) {
            if (strcmp(definitions[i].id, definitions[j].id) == 0) {
                set_error(out_error, PRINTERCTL_MACRO_ERR_DUPLICATE_ID,
                          "duplicate macro id '%s'", definitions[i].id);
                return PRINTERCTL_MACRO_ERR_DUPLICATE_ID;
            }
        }
    }
    return PRINTERCTL_MACRO_OK;
}

static const printerctl_macro_definition_t builtins[] = {
    {
        .id = "maintenance-z-lift",
        .display_name = "Maintenance Z lift",
        .description = "Raise Z by the configured calibration safe-Z distance.",
        .category = PRINTERCTL_MACRO_CATEGORY_MAINTENANCE,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = 0,
        .steps = NULL,
    },
    {
        .id = "calibrate-front-left",
        .display_name = "Calibrate front-left",
        .description = "Built-in calibration placeholder; coordinates require machine geometry.",
        .category = PRINTERCTL_MACRO_CATEGORY_CALIBRATION,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = 0,
        .steps = NULL,
    },
    {
        .id = "calibrate-front-right",
        .display_name = "Calibrate front-right",
        .description = "Built-in calibration placeholder; coordinates require machine geometry.",
        .category = PRINTERCTL_MACRO_CATEGORY_CALIBRATION,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = 0,
        .steps = NULL,
    },
    {
        .id = "calibrate-center",
        .display_name = "Calibrate center",
        .description = "Built-in calibration placeholder; coordinates require machine geometry.",
        .category = PRINTERCTL_MACRO_CATEGORY_CALIBRATION,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = 0,
        .steps = NULL,
    },
    {
        .id = "calibrate-back-left",
        .display_name = "Calibrate back-left",
        .description = "Built-in calibration placeholder; coordinates require machine geometry.",
        .category = PRINTERCTL_MACRO_CATEGORY_CALIBRATION,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = 0,
        .steps = NULL,
    },
    {
        .id = "calibrate-back-right",
        .display_name = "Calibrate back-right",
        .description = "Built-in calibration placeholder; coordinates require machine geometry.",
        .category = PRINTERCTL_MACRO_CATEGORY_CALIBRATION,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = 0,
        .steps = NULL,
    },
    {
        .id = "park",
        .display_name = "Park",
        .description = "Built-in motion placeholder; coordinates require machine geometry.",
        .category = PRINTERCTL_MACRO_CATEGORY_MOTION,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = 0,
        .steps = NULL,
    },
};

size_t printerctl_macro_builtin_count(void)
{
    return sizeof(builtins) / sizeof(builtins[0]);
}

const printerctl_macro_definition_t *printerctl_macro_builtin_at(size_t index)
{
    if (index >= printerctl_macro_builtin_count()) {
        return NULL;
    }
    return &builtins[index];
}

printerctl_macro_status_t printerctl_macro_builtin_find(
    const char *id,
    const printerctl_macro_definition_t **out_definition)
{
    if (out_definition != NULL) {
        *out_definition = NULL;
    }
    if (id == NULL || out_definition == NULL) {
        return PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < printerctl_macro_builtin_count(); ++i) {
        if (strcmp(builtins[i].id, id) == 0) {
            *out_definition = &builtins[i];
            return PRINTERCTL_MACRO_OK;
        }
    }
    return PRINTERCTL_MACRO_NOT_FOUND;
}

static int valid_positive(double value)
{
    return isfinite(value) && value > 0.0;
}

static printerctl_macro_status_t require_z_config(
    const printerctl_macro_calibration_config_t *config,
    printerctl_macro_error_t *out_error)
{
    if (config == NULL || !config->has_safe_z || !config->has_z_feedrate) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "calibration.safe_z and calibration.z_feedrate are required");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    if (!valid_positive(config->safe_z) || !valid_positive(config->z_feedrate)) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "calibration.safe_z and calibration.z_feedrate must be positive finite values");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    return PRINTERCTL_MACRO_OK;
}

static printerctl_macro_status_t require_calibration_config(
    const printerctl_macro_calibration_config_t *config,
    printerctl_macro_error_t *out_error)
{
    printerctl_macro_status_t status = require_z_config(config, out_error);
    if (status != PRINTERCTL_MACRO_OK) {
        return status;
    }
    if (!config->has_min_x || !config->has_max_x || !config->has_min_y ||
        !config->has_max_y || !config->has_xy_feedrate) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "complete calibration geometry is required");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    if (!isfinite(config->min_x) || !isfinite(config->max_x) ||
        !isfinite(config->min_y) || !isfinite(config->max_y) ||
        config->min_x >= config->max_x || config->min_y >= config->max_y ||
        !valid_positive(config->xy_feedrate)) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "invalid calibration geometry");
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }
    return PRINTERCTL_MACRO_OK;
}

static printerctl_macro_step_t relative_z(double z, double feedrate)
{
    printerctl_macro_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PRINTERCTL_MACRO_STEP_MOVE_RELATIVE;
    step.axes = PRINTERCTL_MACRO_AXIS_Z;
    step.z = z;
    step.feedrate_mm_min = feedrate;
    return step;
}

static printerctl_macro_step_t absolute_xy(double x, double y, double feedrate)
{
    printerctl_macro_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PRINTERCTL_MACRO_STEP_MOVE_ABSOLUTE;
    step.axes = PRINTERCTL_MACRO_AXIS_X | PRINTERCTL_MACRO_AXIS_Y;
    step.x = x;
    step.y = y;
    step.feedrate_mm_min = feedrate;
    return step;
}

printerctl_macro_status_t printerctl_macro_resolve_builtin(
    const char *id,
    const printerctl_macro_calibration_config_t *config,
    printerctl_macro_definition_t *out_definition,
    printerctl_macro_step_t *out_steps,
    size_t out_step_capacity,
    printerctl_macro_error_t *out_error)
{
    set_error(out_error, PRINTERCTL_MACRO_OK, "");
    const printerctl_macro_definition_t *base = NULL;
    printerctl_macro_status_t status = printerctl_macro_builtin_find(id, &base);
    if (status != PRINTERCTL_MACRO_OK) {
        set_error(out_error, status, "macro id '%s' not found", id != NULL ? id : "(null)");
        return status;
    }
    if (out_definition == NULL || out_steps == NULL || out_step_capacity < 3) {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT,
                  "output definition and at least 3 step slots are required");
        return PRINTERCTL_MACRO_ERR_INVALID_ARGUMENT;
    }

    *out_definition = *base;
    out_definition->steps = out_steps;

    if (strcmp(id, "maintenance-z-lift") == 0) {
        status = require_z_config(config, out_error);
        if (status != PRINTERCTL_MACRO_OK) {
            return status;
        }
        out_steps[0] = relative_z(config->safe_z, config->z_feedrate);
        out_definition->step_count = 1;
        return printerctl_macro_validate_definition(out_definition, out_error);
    }

    double x = 0.0;
    double y = 0.0;
    if (strcmp(id, "calibrate-front-left") == 0) {
        status = require_calibration_config(config, out_error);
        if (status != PRINTERCTL_MACRO_OK) return status;
        x = config->min_x;
        y = config->min_y;
    } else if (strcmp(id, "calibrate-front-right") == 0) {
        status = require_calibration_config(config, out_error);
        if (status != PRINTERCTL_MACRO_OK) return status;
        x = config->max_x;
        y = config->min_y;
    } else if (strcmp(id, "calibrate-center") == 0) {
        status = require_calibration_config(config, out_error);
        if (status != PRINTERCTL_MACRO_OK) return status;
        x = (config->min_x + config->max_x) / 2.0;
        y = (config->min_y + config->max_y) / 2.0;
    } else if (strcmp(id, "calibrate-back-left") == 0) {
        status = require_calibration_config(config, out_error);
        if (status != PRINTERCTL_MACRO_OK) return status;
        x = config->min_x;
        y = config->max_y;
    } else if (strcmp(id, "calibrate-back-right") == 0) {
        status = require_calibration_config(config, out_error);
        if (status != PRINTERCTL_MACRO_OK) return status;
        x = config->max_x;
        y = config->max_y;
    } else {
        set_error(out_error, PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
                  "macro id '%s' is not executable in M28", id);
        return PRINTERCTL_MACRO_ERR_INVALID_DEFINITION;
    }

    out_steps[0] = relative_z(config->safe_z, config->z_feedrate);
    out_steps[1] = absolute_xy(x, y, config->xy_feedrate);
    out_steps[2] = relative_z(-config->safe_z, config->z_feedrate);
    out_definition->step_count = 3;
    return printerctl_macro_validate_definition(out_definition, out_error);
}
