#include "common.h"
#include "p3_stage_util.h"

/* Fixed coefficients of the team's proposed model, not config keys. */
static const double FRESH_DROPLET_UM = 5.0;
static const double PARTICLE_ADJUSTMENT = 0.10;

static double fragment_mean(const Config *cfg, uint32_t lot_id)
{
    double total = 0.0;
    long n = cfg->sim_elements_per_lot;
    #pragma omp parallel for reduction(+:total) if(cfg->use_openmp) num_threads(cfg->omp_threads) schedule(static)
    for (long i = 0; i < n; i++) {
        double u = hash_unit((UINT64_C(3) << 56) ^ ((uint64_t)lot_id << 32) ^ (uint64_t)i);
        total += u;
    }
    return total / (double)n;
}

void crushing_process(const Config *cfg, Lot *lot, unsigned *seed)
{
    double t0 = now_s(), size, mean;
    if (!p3_input_valid(cfg, lot, seed, STAGE_CRUSHING)) return;
    if (!p3_range_valid(cfg->paste_particle_mm)) {
        fprintf(stderr, "crushing: invalid particle range\n");
        return;
    }
    mean = fragment_mean(cfg, lot->lot_id);
    size = rand_range(seed, cfg->paste_particle_mm);
    size += (mean - 0.5) * PARTICLE_ADJUSTMENT *
            (cfg->paste_particle_mm.max - cfg->paste_particle_mm.min);
    /* Crushing changes the size of olives and pits, not their total mass. */
    lot->paste_particle_mm = fmax(cfg->paste_particle_mm.min,
                                 fmin(size, cfg->paste_particle_mm.max));
    lot->oil_droplet_um = FRESH_DROPLET_UM;
    if (!g_terminate) pad_processing_time(cfg, STAGE_CRUSHING, t0, seed);
}

void crushing_main(const Config *cfg, const StageIO *io)
{
    stage_signals_init();
    stage_signals_unblock();
    stage_run(cfg, io, STAGE_CRUSHING, crushing_process);
}
