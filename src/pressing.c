#include "common.h"
#include "p3_stage_util.h"

static const double DECANTER_YIELD_FACTOR = 1.05;
static const double DECANTER_TIME_FACTOR = 0.80;
static const double BASE_EXTRACTION = 0.90;
static const double DROPLET_BONUS = 0.10;

static double pressure_mean(const Config *cfg, uint32_t lot_id)
{
    double total = 0.0;
    long n = cfg->sim_elements_per_lot;
    #pragma omp parallel for reduction(+:total) if(cfg->use_openmp) num_threads(cfg->omp_threads) schedule(static)
    for (long i = 0; i < n; i++) {
        double u = hash_unit((UINT64_C(5) << 56) ^ ((uint64_t)lot_id << 32) ^ (uint64_t)i);
        total += u * u * (1.0 - u);
    }
    return total / (double)n;
}

void pressing_process(const Config *cfg, Lot *lot, unsigned *seed)
{
    double t0 = now_s(), k, yield, water_pct, oil, water, remaining;
    if (!p3_input_valid(cfg, lot, seed, STAGE_PRESSING)) return;
    if (!p3_range_valid(cfg->oil_yield_pct) || cfg->oil_yield_pct.max > 100 ||
        !p3_range_valid(cfg->water_pct) || cfg->water_pct.max > 100 ||
        !p3_range_valid(cfg->droplet_final_um) || cfg->droplet_final_um.max <= 0 ||
        !isfinite(lot->oil_droplet_um) || lot->oil_droplet_um < 0 ||
        (cfg->press_method != 0 && cfg->press_method != 1)) {
        fprintf(stderr, "pressing: invalid separation settings or droplet\n");
        return;
    }
    k = pressure_mean(cfg, lot->lot_id);
    yield = rand_range(seed, cfg->oil_yield_pct);
    /* Let the distribution move yield slightly within its configured range. */
    yield += (k - 1.0 / 12.0) * (cfg->oil_yield_pct.max - cfg->oil_yield_pct.min);
    yield = fmax(cfg->oil_yield_pct.min, fmin(yield, cfg->oil_yield_pct.max));
    yield *= BASE_EXTRACTION + DROPLET_BONUS *
             fmin(1.0, lot->oil_droplet_um / cfg->droplet_final_um.max);
    if (cfg->press_method == 1) yield *= DECANTER_YIELD_FACTOR;
    yield = fmin(yield, 100.0);
    water_pct = rand_range(seed, cfg->water_pct);
    /* Allocate oil first, then limit water to the mass still available. */
    oil = lot->mass_kg * (yield / 100.0);
    remaining = lot->mass_kg - oil;
    water = fmin(lot->mass_kg * (water_pct / 100.0), remaining);
    lot->oil_kg = oil;
    lot->water_kg = water;
    lot->pomace_kg = remaining - water;

    if (g_terminate) return;
    if (cfg->press_method == 0) {
        pad_processing_time(cfg, STAGE_PRESSING, t0, seed);
    } else {
        double target = DECANTER_TIME_FACTOR * rand_range(seed, cfg->proc_time_ms[STAGE_PRESSING]);
        double spent = (now_s() - t0) * 1000.0 * cfg->time_scale;
        if (target > spent) sleep_ms_scaled(target - spent, cfg->time_scale, &g_terminate);
    }
}

void pressing_main(const Config *cfg, const StageIO *io)
{
    stage_signals_init();
    stage_signals_unblock();
    /* out_fd is -1: the shared loop reports the three products as an event. */
    stage_run(cfg, io, STAGE_PRESSING, pressing_process);
}
