/* tests/config_test.c -- Partner 2: parser test program.
 * Usage: ./config_test <file.cfg> [--check-defaults]
 * Loads the file with config_load(), prints every field of the Config and returns
 *   0  file accepted (warnings are allowed),
 *   1  config_load() reported an error,
 *   3  --check-defaults was given and the loaded values differ from config_set_defaults().
 * Build: gcc -std=gnu11 -Wall -Wextra -g -Iinclude tests/config_test.c src/config.c -lm -o config_test */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pr(FILE *f, const char *name, const Range *r)
{
    fprintf(f, "%s = %.17g %.17g\n", name, r->min, r->max);
}

static void pr_stage_range(FILE *f, const char *base, const Range *arr)
{
    char name[64];
    int i;
    for (i = 0; i < NUM_STAGES; i++) {
        snprintf(name, sizeof name, "%s.%s", base, stage_name(i));
        pr(f, name, &arr[i]);
    }
}

static void print_config(FILE *f, const Config *c)
{
    int i;
    fprintf(f, "seed = %u\n", c->seed);
    fprintf(f, "time_scale = %.17g\n", c->time_scale);
    fprintf(f, "num_batches = %d\n", c->num_batches);
    pr(f, "batch_mass_kg", &c->batch_mass_kg);
    pr(f, "batch_interval_ms", &c->batch_interval_ms);
    fprintf(f, "container_capacity_kg = %.17g\n", c->container_capacity_kg);
    fprintf(f, "conveyor_chunk_kg = %.17g\n", c->conveyor_chunk_kg);
    fprintf(f, "conveyor_tick_ms = %d\n", c->conveyor_tick_ms);
    pr(f, "initial_debris_pct", &c->initial_debris_pct);
    pr(f, "initial_impurity_pct", &c->initial_impurity_pct);
    pr(f, "cleaning_removal_pct", &c->cleaning_removal_pct);
    pr(f, "washing_removal_pct", &c->washing_removal_pct);
    pr(f, "paste_particle_mm", &c->paste_particle_mm);
    pr(f, "droplet_final_um", &c->droplet_final_um);
    pr(f, "oil_yield_pct", &c->oil_yield_pct);
    pr(f, "water_pct", &c->water_pct);
    fprintf(f, "press_method = %d\n", c->press_method);
    pr_stage_range(f, "proc_time_ms", c->proc_time_ms);
    pr_stage_range(f, "temp_nominal_c", c->temp_nominal_c);
    pr_stage_range(f, "temp_limit_c", c->temp_limit_c);
    pr_stage_range(f, "pressure_nominal_bar", c->pressure_nominal_bar);
    pr_stage_range(f, "pressure_limit_bar", c->pressure_limit_bar);
    for (i = 0; i < NUM_STAGES; i++)
        fprintf(f, "max_proc_time_ms.%s = %.17g\n", stage_name(i), c->max_proc_time_ms[i]);
    fprintf(f, "fault_prob_pct = %.17g\n", c->fault_prob_pct);
    fprintf(f, "warn_margin_pct = %.17g\n", c->warn_margin_pct);
    fprintf(f, "qc_report_interval_ms = %d\n", c->qc_report_interval_ms);
    fprintf(f, "use_openmp = %d\n", c->use_openmp);
    fprintf(f, "omp_threads = %d\n", c->omp_threads);
    fprintf(f, "sim_elements_per_lot = %d\n", c->sim_elements_per_lot);
    fprintf(f, "critical_pause_ms = %d\n", c->critical_pause_ms);
    fprintf(f, "max_critical_events = %d\n", c->max_critical_events);
    fprintf(f, "drain_timeout_ms = %d\n", c->drain_timeout_ms);
    fprintf(f, "log_file = %s\n", c->log_file);
    fprintf(f, "enable_gui = %d\n", c->enable_gui);
    fprintf(f, "gui_auto_close_ms = %d\n", c->gui_auto_close_ms);
    fprintf(f, "window_w = %d\n", c->window_w);
    fprintf(f, "window_h = %d\n", c->window_h);
}

/* Compares the printed form of two configs; prints the differing lines. Returns 1 if equal. */
static int same_as_defaults(const Config *loaded)
{
    Config def;
    char *a = NULL, *b = NULL;
    size_t la = 0, lb = 0;
    FILE *fa, *fb;
    int equal;

    config_set_defaults(&def);
    fa = open_memstream(&a, &la);
    fb = open_memstream(&b, &lb);
    if (!fa || !fb) {
        perror("open_memstream");
        return 0;
    }
    print_config(fa, loaded);
    print_config(fb, &def);
    fclose(fa);
    fclose(fb);
    equal = (la == lb && memcmp(a, b, la) == 0);
    if (!equal) {
        char *pa = a, *pb = b;
        while (*pa && *pb) {
            char *ea = strchr(pa, '\n'), *eb = strchr(pb, '\n');
            size_t na = ea ? (size_t)(ea - pa) : strlen(pa), nb = eb ? (size_t)(eb - pb) : strlen(pb);
            if (na != nb || memcmp(pa, pb, na) != 0)
                printf("DIFF loaded: %.*s | default: %.*s\n", (int)na, pa, (int)nb, pb);
            pa += na + (ea ? 1 : 0);
            pb += nb + (eb ? 1 : 0);
        }
    }
    free(a);
    free(b);
    return equal;
}

int main(int argc, char **argv)
{
    Config cfg;
    int check = 0, i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.cfg> [--check-defaults]\n", argv[0]);
        return 2;
    }
    for (i = 2; i < argc; i++)
        if (strcmp(argv[i], "--check-defaults") == 0)
            check = 1;

    if (config_load(argv[1], &cfg) != 0) {
        printf("config_load: ERROR (see messages above)\n");
        return 1;
    }
    print_config(stdout, &cfg);
    if (check) {
        if (same_as_defaults(&cfg)) {
            printf("== MATCH: loaded values are exactly the built-in defaults\n");
        } else {
            printf("== MISMATCH: loaded values differ from config_set_defaults()\n");
            return 3;
        }
    }
    return 0;
}
