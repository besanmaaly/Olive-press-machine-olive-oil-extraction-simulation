/* src/washing.c -- Partner 2: Washing process (removes soil and remaining impurities).
 *
 * Lot physics (physics contract):
 *     impurity_pct *= (1 - removal / 100),  removal in washing_removal_pct
 *     mass_kg unchanged
 * The OpenMP kernel simulates the water-jet effect on every item of the lot and
 * modulates the removal by a few percent of its configured range.                 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <signal.h>

#define KERNEL_SALT      3u           /* different salt per stage */
#define KERNEL_FIXED_ONE 4294967296.0 /* 2^32: fixed-point scale of the kernel sum */
#define KERNEL_MEAN_INV  12.0         /* E[u*u*(1-u)] = 1/12 -> effect is about 1.0 */
#define EFFECT_GAIN      0.05         /* kernel may shift removal by 5% of the range */

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* OpenMP kernel: stand-in for the "water-jet effect" per item.  Fixed-point integer
 * accumulation makes the result independent of the thread count / OpenMP on-off. */
static double kernel(const Config *cfg, uint32_t lot_id, uint64_t salt)
{
    long n = cfg->sim_elements_per_lot;
    unsigned long long acc = 0;
    long i;
    sigset_t block, old;

    if (n < 1)
        return 1.0 / KERNEL_MEAN_INV;
    /* Signals are process-directed: any thread that does not block them may receive them.
     * If an OpenMP worker caught SIGTERM / a belt signal, the main thread (blocked in read(),
     * nanosleep() or sigsuspend()) would never wake up.  The workers are created inside this
     * window and inherit the blocked mask, so every signal is always delivered to the main
     * thread (pending signals are delivered when the mask is restored below). */
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, &old);
#pragma omp parallel for reduction(+ : acc) if (cfg->use_openmp) num_threads(cfg->omp_threads)
    for (i = 0; i < n; i++) {
        double u = hash_unit((salt << 56) ^ ((uint64_t)lot_id << 32) ^ (uint64_t)i);
        acc += (unsigned long long)(u * u * (1.0 - u) * KERNEL_FIXED_ONE);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    return (double)acc / KERNEL_FIXED_ONE / (double)n;
}

void washing_main(const Config *cfg, const StageIO *io)
{
    stage_signals_init();
    stage_signals_unblock();
    stage_run(cfg, io, STAGE_WASHING, washing_process);
}

/* Called by stage_run() once per lot. */
void washing_process(const Config *cfg, Lot *lot, unsigned *seed)
{
    Range r = cfg->washing_removal_pct;
    double t0 = now_s();
    double effect = kernel(cfg, lot->lot_id, KERNEL_SALT) * KERNEL_MEAN_INV;
    double removal = rand_range(seed, r);

    removal = clamp(removal + (effect - 1.0) * EFFECT_GAIN * (r.max - r.min), r.min, r.max);
    lot->impurity_pct *= (1.0 - removal / 100.0); /* mass_kg is unchanged */

    pad_processing_time(cfg, STAGE_WASHING, t0, seed);
}
