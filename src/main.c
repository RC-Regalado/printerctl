#include "printerctl/cli.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    return printerctl_cli_run(argc, argv, stdout, stderr);
}
