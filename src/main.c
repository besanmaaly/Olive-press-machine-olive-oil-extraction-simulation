#include "common.h"
#include "supervisor.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    Config cfg;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        return 2;
    }
    config_set_defaults(&cfg);
    if (config_load(argv[1], &cfg) != 0) {
        fprintf(stderr, "Cannot load configuration: %s\n", argv[1]);
        return 2;
    }
    return supervisor_run(&cfg, argv[1]);
}
