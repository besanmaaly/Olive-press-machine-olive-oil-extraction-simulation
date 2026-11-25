/* src/cleaning.c -- Partner 2: Cleaning process (air-blower simulation).
 *
 * Cleaning is tied to the conveyor belt of the reception process:
 *     SIGUSR2 -> belt running, SIGUSR1 -> belt stopped.
 * While the belt is stopped cleaning does no work at all: it sleeps in sigsuspend()
 * (no busy waiting).  The belt state is also checked between 50 ms slices of the
 * processing time, so cleaning stops in the middle of a lot as well.
 *
 * Lot physics: removal of leaves / twigs / dust, see the "physics contract":
 *     removed    = mass * debris% / 100 * removal / 100
 *     mass      -= removed
 *     debris_pct = remaining debris as % of the new mass                          */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CLEAN_SLICE_SIM_MS 50.0         /* belt state is re-checked every 50 simulated ms */
#define KERNEL_SALT        2u           /* different salt per stage: different "air flow"  */
#define KERNEL_FIXED_ONE   4294967296.0 /* 2^32: fixed-point scale of the kernel sum       */
#define KERNEL_MEAN_INV    12.0         /* E[u*u*(1-u)] = 1/12  -> effect is about 1.0     */
#define EFFECT_GAIN        0.05         /* kernel may shift removal by 5% of the range     */
#define MIN_MASS_FRACTION  1e-9         /* cleaning never removes the whole lot            */

static volatile sig_atomic_t g_belt_running = 0;
static const StageIO *s_io;

static void on_belt_run(int s)  { (void)s; g_belt_running = 1; } /* SIGUSR2 */
static void on_belt_stop(int s) { (void)s; g_belt_running = 0; } /* SIGUSR1 */

/* Installs a belt-signal handler whose sa_mask blocks BOTH belt signals while it runs.
 * Without this, when STOP and RUN are pending together the kernel delivers SIGUSR1 first
 * but SIGUSR2's handler is entered before the first instruction of SIGUSR1's handler, so
 * the bodies run in reverse order and the belt would stay "stopped" although the last
 * signal sent was RUN (found by tests/pipeline_p2_test.c, hang after ~1 run in 10).
 * install_handler() cannot be used: it always passes an empty sa_mask.  No SA_RESTART:
 * blocking calls still return EINTR. */
static int install_belt_handler(int sig, void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sa.sa_flags = 0;
    return sigaction(sig, &sa, NULL);
}

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* OpenMP kernel: stand-in for the "air-flow effect" on every item of the lot.
 * The sum is accumulated in fixed point (integers), so it is exactly the same for any
 * number of threads and with OpenMP switched off.  Returns the mean effect (~1/12). */
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

/* Blocks (sigsuspend, no busy waiting) while the belt is stopped.  Race free: the belt
 * signals and SIGTERM are blocked while the flags are tested, and sigsuspend() unblocks
 * them atomically.  The time spent waiting is added to g_excluded_ms so that Quality
 * Control does not see a fake long processing time. */
static void wait_for_belt(void)
{
    sigset_t block, old;
    double t0;
    int waited = 0;

    if (g_belt_running || g_terminate)
        return;

    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &old);
    t0 = now_s();
    while (!g_belt_running && !g_terminate) {
        if (!waited) {
            report_status(s_io, STAGE_CLEANING, ST_STOPPED);
            waited = 1;
        }
        sigsuspend(&old);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    if (waited) {
        g_excluded_ms += (now_s() - t0) * 1000.0;
        if (!g_terminate)
            report_status(s_io, STAGE_CLEANING, ST_RUNNING);
    }
}

void cleaning_main(const Config *cfg, const StageIO *io)
{
    s_io = io;
    stage_signals_init();
    install_belt_handler(SIGUSR2, on_belt_run); /* AFTER init, BEFORE unblock */
    install_belt_handler(SIGUSR1, on_belt_stop);
    stage_signals_unblock();
    stage_run(cfg, io, STAGE_CLEANING, cleaning_process);
}

/* Called by stage_run() once per lot. */
void cleaning_process(const Config *cfg, Lot *lot, unsigned *seed)
{
    Range r = cfg->cleaning_removal_pct;
    double ts = cfg->time_scale > 0.0 ? cfg->time_scale : 1.0;
    double t0, excluded0, effect, removal, target_ms;

    /* 1. nothing happens while the belt is stopped.  On SIGTERM the wait ends at once; the
     *    (instant) physics below is still applied, because stage_run() forwards the lot
     *    even when we return early, and a forwarded lot must never be half-processed. */
    wait_for_belt();
    t0 = now_s();
    excluded0 = g_excluded_ms; /* waiting before t0 must not be subtracted twice */

    /* 2. compute the lot (OpenMP kernel modulates the removal slightly) */
    effect = kernel(cfg, lot->lot_id, KERNEL_SALT) * KERNEL_MEAN_INV;
    removal = rand_range(seed, r);
    removal = clamp(removal + (effect - 1.0) * EFFECT_GAIN * (r.max - r.min), r.min, r.max);
    if (lot->mass_kg > 0.0) {
        double debris_kg = lot->mass_kg * lot->debris_pct / 100.0;
        double removed = debris_kg * removal / 100.0;
        if (removed > lot->mass_kg * (1.0 - MIN_MASS_FRACTION))
            removed = lot->mass_kg * (1.0 - MIN_MASS_FRACTION);
        lot->mass_kg -= removed;
        lot->debris_pct = clamp(100.0 * (debris_kg - removed) / lot->mass_kg, 0.0, 100.0);
    }

    /* 3. spend the rest of the processing time in slices; the belt is checked before
     *    every slice, so cleaning also stops in the middle of a lot */
    target_ms = rand_range(seed, cfg->proc_time_ms[STAGE_CLEANING]);
    for (;;) {
        double spent_ms, remaining_ms;

        wait_for_belt();
        if (g_terminate)
            return;
        spent_ms = ((now_s() - t0) * 1000.0 - (g_excluded_ms - excluded0)) * ts; /* simulated ms, waiting excluded */
        remaining_ms = target_ms - spent_ms;
        if (remaining_ms <= 0.0)
            break;
        sleep_ms_scaled(remaining_ms < CLEAN_SLICE_SIM_MS ? remaining_ms : CLEAN_SLICE_SIM_MS,
                        cfg->time_scale, &g_terminate);
    }
}
