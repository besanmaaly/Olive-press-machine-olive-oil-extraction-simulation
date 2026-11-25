#include "common.h"
#include "p3_stage_util.h"

enum { MIXING_STEPS = 10 };
static const double MIXING_RATE = 0.35;

static double coalescence_mean(const Config *cfg, uint32_t lot_id, int step)
{
    double total = 0.0;
    long n = cfg->sim_elements_per_lot;
    #pragma omp parallel for reduction(+:total) if(cfg->use_openmp) num_threads(cfg->omp_threads) schedule(static)
    for (long i = 0; i < n; i++) {
        double u = hash_unit(((uint64_t)(16 + step) << 56) ^
                             ((uint64_t)lot_id << 32) ^ (uint64_t)i);
        /* Keep each merging factor strictly between zero and one. */
        total += 0.5 * (1.0 + u * u * (1.0 - u));
    }
    return total / (double)n;
}

void malaxation_process(const Config *cfg, Lot *lot, unsigned *seed)
{
    double t0 = now_s(), target, droplet;
    if (!p3_input_valid(cfg, lot, seed, STAGE_MALAXATION)) return;
    if (!p3_range_valid(cfg->droplet_final_um) ||
        !isfinite(lot->oil_droplet_um) || lot->oil_droplet_um < 0 ||
        lot->oil_droplet_um > cfg->droplet_final_um.max) {
        fprintf(stderr, "malaxation: invalid droplet or target range\n");
        return;
    }
    droplet = lot->oil_droplet_um;
    target = fmax(droplet, rand_range(seed, cfg->droplet_final_um));
    for (int step = 0; step < MIXING_STEPS && !g_terminate; step++) {
        double factor = coalescence_mean(cfg, lot->lot_id, step);
        droplet += (target - droplet) * MIXING_RATE * (0.5 + factor);
        droplet = fmin(droplet, target);
    }
    lot->oil_droplet_um = droplet;
    if (!g_terminate) pad_processing_time(cfg, STAGE_MALAXATION, t0, seed);
}

void malaxation_main(const Config *cfg, const StageIO *io)
{
    stage_signals_init();
    stage_signals_unblock();
    stage_run(cfg, io, STAGE_MALAXATION, malaxation_process);
}
