#ifndef PRINTERCTL_CLI_H
#define PRINTERCTL_CLI_H

#include <stdio.h>

#define PRINTERCTL_EXIT_OK 0
#define PRINTERCTL_EXIT_USAGE_ERROR 1

/*
 * Parses command-line arguments and writes help/version/error output to the
 * given streams. Separated from main() so it can be linked directly into the
 * test executable without spawning a subprocess.
 */
int printerctl_cli_run(int argc, char *const argv[], FILE *out, FILE *err);

#endif
