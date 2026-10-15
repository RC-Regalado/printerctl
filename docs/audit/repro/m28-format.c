#include "../../../src/daemon/worker.c"

int main(void)
{
    printerctl_macro_calibration_config_t config = {
        .has_min_x = true, .has_max_x = true, .has_min_y = true, .has_max_y = true,
        .has_safe_z = true, .has_xy_feedrate = true, .has_z_feedrate = true,
        .min_x = 1e200, .max_x = 2e200, .min_y = 1e200, .max_y = 2e200,
        .safe_z = 5, .xy_feedrate = 1500, .z_feedrate = 300
    };
    printerctl_macro_definition_t definition;
    printerctl_macro_step_t steps[3];
    printerctl_macro_error_t error;
    if (printerctl_macro_resolve_builtin("calibrate-front-left", &config,
            &definition, steps, 3, &error) != PRINTERCTL_MACRO_OK) return 2;
    char command[128];
    format_move_command(&steps[1], command, sizeof(command));
    puts(command);
    return 0;
}
