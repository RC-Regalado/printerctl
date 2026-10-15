#include "printerctl/macro.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static printerctl_macro_step_t move_step(void)
{
    printerctl_macro_step_t step = {
        .type = PRINTERCTL_MACRO_STEP_MOVE_ABSOLUTE,
        .axes = PRINTERCTL_MACRO_AXIS_X | PRINTERCTL_MACRO_AXIS_Y,
        .x = 10.0,
        .y = 20.0,
        .z = 0.0,
        .feedrate_mm_min = 1200.0,
    };
    return step;
}

static printerctl_macro_definition_t valid_macro(const printerctl_macro_step_t *steps,
                                                 size_t step_count)
{
    printerctl_macro_definition_t definition = {
        .id = "test-move",
        .display_name = "Test move",
        .description = "Valid movement macro used by unit tests.",
        .category = PRINTERCTL_MACRO_CATEGORY_MOTION,
        .required_state = PRINTERCTL_MACRO_REQUIRED_STATE_IDLE_ONLY,
        .capabilities = PRINTERCTL_MACRO_CAP_MOVE,
        .step_count = step_count,
        .steps = steps,
    };
    return definition;
}

static void test_valid_movement_macro_passes(void)
{
    printerctl_macro_step_t step = move_step();
    printerctl_macro_definition_t definition = valid_macro(&step, 1);
    printerctl_macro_error_t error;
    check(printerctl_macro_validate_definition(&definition, &error) == PRINTERCTL_MACRO_OK,
          "valid movement macro passes validation");
}

static void test_empty_macro_rejected(void)
{
    printerctl_macro_definition_t definition = valid_macro(NULL, 0);
    printerctl_macro_error_t error;
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "empty executable macro is rejected");
}

static void test_step_count_overflow_rejected(void)
{
    printerctl_macro_step_t step = move_step();
    printerctl_macro_definition_t definition =
        valid_macro(&step, PRINTERCTL_MACRO_STEPS_MAX + 1);
    printerctl_macro_error_t error;
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "step-count overflow is rejected");
}

static void test_invalid_step_type_rejected(void)
{
    printerctl_macro_step_t step = move_step();
    step.type = (printerctl_macro_step_type_t)99;
    printerctl_macro_definition_t definition = valid_macro(&step, 1);
    printerctl_macro_error_t error;
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "invalid step type is rejected");
}

static void test_nan_inf_coordinate_rejected(void)
{
    printerctl_macro_step_t step = move_step();
    step.x = NAN;
    printerctl_macro_definition_t definition = valid_macro(&step, 1);
    printerctl_macro_error_t error;
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "NaN coordinate is rejected");

    step = move_step();
    step.y = INFINITY;
    definition = valid_macro(&step, 1);
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "Inf coordinate is rejected");
}

static void test_invalid_feedrate_rejected(void)
{
    printerctl_macro_step_t step = move_step();
    printerctl_macro_definition_t definition;
    printerctl_macro_error_t error;

    step.feedrate_mm_min = 0.0;
    definition = valid_macro(&step, 1);
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "zero feedrate is rejected");

    step = move_step();
    step.feedrate_mm_min = -1.0;
    definition = valid_macro(&step, 1);
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "negative feedrate is rejected");

    step = move_step();
    step.feedrate_mm_min = INFINITY;
    definition = valid_macro(&step, 1);
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "infinite feedrate is rejected");
}

static void test_move_step_without_move_capability_rejected(void)
{
    printerctl_macro_step_t step = move_step();
    printerctl_macro_definition_t definition = valid_macro(&step, 1);
    definition.capabilities = PRINTERCTL_MACRO_CAP_FAN;
    printerctl_macro_error_t error;
    check(printerctl_macro_validate_definition(&definition, &error) ==
              PRINTERCTL_MACRO_ERR_INVALID_DEFINITION,
          "MOVE step without MOVE capability is rejected");
}

static void test_duplicate_ids_rejected(void)
{
    printerctl_macro_definition_t definitions[2];
    definitions[0] = valid_macro(NULL, 0);
    definitions[1] = valid_macro(NULL, 0);
    printerctl_macro_error_t error;
    check(printerctl_macro_validate_registry(definitions, 2, &error) ==
              PRINTERCTL_MACRO_ERR_DUPLICATE_ID,
          "duplicate stable macro IDs are rejected");
}

static void test_unknown_registry_id_returns_not_found(void)
{
    const printerctl_macro_definition_t *definition = NULL;
    check(printerctl_macro_builtin_find("does-not-exist", &definition) ==
              PRINTERCTL_MACRO_NOT_FOUND,
          "unknown registry id returns not found");
    check(definition == NULL, "not-found lookup clears output definition");
}

static void test_registry_iteration_is_deterministic(void)
{
    const char *expected[] = {
        "maintenance-z-lift",
        "calibrate-front-left",
        "calibrate-front-right",
        "calibrate-center",
        "calibrate-back-left",
        "calibrate-back-right",
        "park",
    };
    size_t count = printerctl_macro_builtin_count();
    check(count == sizeof(expected) / sizeof(expected[0]), "builtin count is stable");
    for (size_t i = 0; i < count; ++i) {
        const printerctl_macro_definition_t *definition = printerctl_macro_builtin_at(i);
        check(definition != NULL, "builtin at index exists");
        check(strcmp(definition->id, expected[i]) == 0, "builtin iteration order is stable");
    }

    printerctl_macro_error_t error;
    check(printerctl_macro_validate_registry(printerctl_macro_builtin_at(0), count, &error) ==
              PRINTERCTL_MACRO_OK,
          "builtin registry has unique valid IDs");
}

static printerctl_macro_calibration_config_t valid_geometry(void)
{
    printerctl_macro_calibration_config_t config = {
        .has_min_x = true,
        .has_max_x = true,
        .has_min_y = true,
        .has_max_y = true,
        .has_safe_z = true,
        .has_xy_feedrate = true,
        .has_z_feedrate = true,
        .min_x = 10.0,
        .max_x = 90.0,
        .min_y = 20.0,
        .max_y = 120.0,
        .safe_z = 5.0,
        .xy_feedrate = 1500.0,
        .z_feedrate = 300.0,
    };
    return config;
}

static void test_resolve_executable_builtins(void)
{
    printerctl_macro_calibration_config_t config = valid_geometry();
    printerctl_macro_definition_t definition;
    printerctl_macro_step_t steps[PRINTERCTL_MACRO_STEPS_MAX];
    printerctl_macro_error_t error;

    check(printerctl_macro_resolve_builtin("maintenance-z-lift", &config, &definition, steps,
                                           PRINTERCTL_MACRO_STEPS_MAX, &error) ==
              PRINTERCTL_MACRO_OK,
          "maintenance-z-lift resolves");
    check(definition.step_count == 1 && steps[0].type == PRINTERCTL_MACRO_STEP_MOVE_RELATIVE &&
              steps[0].axes == PRINTERCTL_MACRO_AXIS_Z && steps[0].z == 5.0,
          "maintenance-z-lift is relative safe-Z");

    const struct {
        const char *id;
        double x;
        double y;
    } points[] = {
        {"calibrate-front-left", 10.0, 20.0},
        {"calibrate-front-right", 90.0, 20.0},
        {"calibrate-center", 50.0, 70.0},
        {"calibrate-back-left", 10.0, 120.0},
        {"calibrate-back-right", 90.0, 120.0},
    };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        check(printerctl_macro_resolve_builtin(points[i].id, &config, &definition, steps,
                                               PRINTERCTL_MACRO_STEPS_MAX, &error) ==
                  PRINTERCTL_MACRO_OK,
              "calibration macro resolves");
        check(definition.step_count == 3, "calibration macro has three steps");
        check(steps[0].type == PRINTERCTL_MACRO_STEP_MOVE_RELATIVE && steps[0].z == 5.0,
              "calibration first raises relative Z");
        check(steps[1].type == PRINTERCTL_MACRO_STEP_MOVE_ABSOLUTE &&
                  steps[1].x == points[i].x && steps[1].y == points[i].y,
              "calibration resolves XY");
        check(steps[2].type == PRINTERCTL_MACRO_STEP_MOVE_RELATIVE && steps[2].z == -5.0,
              "calibration lowers relative Z");
    }
}

static void test_invalid_geometry_rejected(void)
{
    printerctl_macro_calibration_config_t config = valid_geometry();
    printerctl_macro_definition_t definition;
    printerctl_macro_step_t steps[PRINTERCTL_MACRO_STEPS_MAX];
    printerctl_macro_error_t error;

    config.min_x = config.max_x;
    check(printerctl_macro_resolve_builtin("calibrate-front-left", &config, &definition, steps,
                                           PRINTERCTL_MACRO_STEPS_MAX, &error) !=
              PRINTERCTL_MACRO_OK,
          "min_x >= max_x rejected");
    config = valid_geometry();
    config.min_y = config.max_y;
    check(printerctl_macro_resolve_builtin("calibrate-front-left", &config, &definition, steps,
                                           PRINTERCTL_MACRO_STEPS_MAX, &error) !=
              PRINTERCTL_MACRO_OK,
          "min_y >= max_y rejected");
    config = valid_geometry();
    config.safe_z = 0.0;
    check(printerctl_macro_resolve_builtin("maintenance-z-lift", &config, &definition, steps,
                                           PRINTERCTL_MACRO_STEPS_MAX, &error) !=
              PRINTERCTL_MACRO_OK,
          "safe_z <= 0 rejected");
    config = valid_geometry();
    config.xy_feedrate = -1.0;
    check(printerctl_macro_resolve_builtin("calibrate-front-left", &config, &definition, steps,
                                           PRINTERCTL_MACRO_STEPS_MAX, &error) !=
              PRINTERCTL_MACRO_OK,
          "xy feedrate <= 0 rejected");
    config = valid_geometry();
    config.z_feedrate = INFINITY;
    check(printerctl_macro_resolve_builtin("maintenance-z-lift", &config, &definition, steps,
                                           PRINTERCTL_MACRO_STEPS_MAX, &error) !=
              PRINTERCTL_MACRO_OK,
          "Inf feedrate rejected");
}

int main(void)
{
    test_valid_movement_macro_passes();
    test_empty_macro_rejected();
    test_step_count_overflow_rejected();
    test_invalid_step_type_rejected();
    test_nan_inf_coordinate_rejected();
    test_invalid_feedrate_rejected();
    test_move_step_without_move_capability_rejected();
    test_duplicate_ids_rejected();
    test_unknown_registry_id_returns_not_found();
    test_registry_iteration_is_deterministic();
    test_resolve_executable_builtins();
    test_invalid_geometry_rejected();
    return failures == 0 ? 0 : 1;
}
