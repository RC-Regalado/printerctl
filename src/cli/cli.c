#include "printerctl/cli.h"
#include "printerctl/version.h"

#include <string.h>

static void print_usage_hint(FILE *err)
{
    fprintf(err, "Try 'printerctl --help' for more information.\n");
}

int printerctl_cli_run(int argc, char *const argv[], FILE *out, FILE *err)
{
    if (argc != 2) {
        if (argc < 2) {
            fprintf(err, "printerctl: missing command\n");
        } else {
            fprintf(err, "printerctl: too many arguments\n");
        }
        print_usage_hint(err);
        return PRINTERCTL_EXIT_USAGE_ERROR;
    }

    const char *arg = argv[1];

    if (strcmp(arg, "--help") == 0) {
        fprintf(out,
            "Usage: printerctl [OPTION]\n"
            "\n"
            "Command-line controller for Marlin-based 3D printers over USB serial.\n"
            "\n"
            "Options:\n"
            "  --help     Show this help message and exit\n"
            "  --version  Show version information and exit\n"
            "\n"
            "Printer commands (print, pause, resume, cancel, temp, position, info,\n"
            "send, home) are not yet implemented in this build.\n");
        return PRINTERCTL_EXIT_OK;
    }

    if (strcmp(arg, "--version") == 0) {
        fprintf(out, "printerctl %s\n", PRINTERCTL_VERSION_STRING);
        return PRINTERCTL_EXIT_OK;
    }

    fprintf(err, "printerctl: unrecognized argument '%s'\n", arg);
    print_usage_hint(err);
    return PRINTERCTL_EXIT_USAGE_ERROR;
}
