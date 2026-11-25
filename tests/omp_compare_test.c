/* tests/omp_compare_test.c -- Partner 2: OpenMP on/off comparison + physics checks for
 * the cleaning and washing stages.
 *
 *  1. Runs the REAL cleaning_main / washing_main in a forked process (like the supervisor)
 *     with use_openmp = 0, and with use_openmp = 1 for 2 and 4 threads.
 *  2. Compares the output lots bit for bit (the kernels are deterministic, so OpenMP
 *     must not change a single value).
 *  3. Checks the physics contract (mass conserved by washing, removal inside the
 *     configured percentage range, debris / impurity decrease).
 *  4. Measures the wall time of the whole stage (time_scale is huge, so sleeping is
 *     negligible and the time is the computation) -> "better or worse with OpenMP?".
 *
 * Usage: ./omp_compare_test [lots=60] [elements=2000000]
 * Build: gcc -std=gnu11 -Wall -Wextra -g -O2 -fopenmp -Iinclude tests/omp_compare_test.c \
 *            src/ipc_util.c src/stage_support.c src/cleaning.c src/washing.c -lm -o omp_compare_test */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LOTS 512
#define FAST_SCALE 1.0e6 /* sleeping becomes negligible -> we time the computation only */
#define EPS 1e-9

typedef void (*MainFn)(const Config *, const StageIO *);

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                      \
    do {                                      \
        if (cond) { g_pass++; printf("  PASS: "); } \
        else { g_fail++; printf("  FAIL: "); }     \
        printf(__VA_ARGS__);                  \
        printf("\n");                         \
    } while (0)

static void make_input(Lot *in, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        memset(&in[i], 0, sizeof in[i]);
        in[i].lot_id = (uint32_t)i;
        in[i].batch_id = 1;
        in[i].mass_kg = 40.0 + (double)(i % 11); /* varied masses */
        in[i].debris_pct = 2.0 + (double)(i % 7);
        in[i].impurity_pct = 3.0 + (double)(i % 8);
        in[i].t_created = 0.0; /* fixed so that lots can be compared bit for bit */
    }
}

/* Runs one stage process, feeds n lots, collects the output. Returns elapsed seconds, <0 on error. */
static double run_stage(MainFn fn, const Config *cfg, int send_run, const Lot *in, int n, Lot *out, int *n_out)
{
    int in_p[2], out_p[2], tel_p[2], evt_p[2], st, k = 0;
    sigset_t blk;
    pid_t pid;
    double t0, t1;
    Lot l;

    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigaddset(&blk, SIGUSR2);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    if (pipe(in_p) || pipe(out_p) || pipe(tel_p) || pipe(evt_p))
        return -1.0;
    pid = fork();
    if (pid < 0)
        return -1.0;
    if (pid == 0) {
        StageIO io;
        close(in_p[1]); close(out_p[0]); close(tel_p[0]); close(evt_p[0]);
        io.in_fd = in_p[0];
        io.out_fd = out_p[1];
        io.telemetry_fd = tel_p[1];
        io.event_fd = evt_p[1];
        io.peer_pid = -1;
        fn(cfg, &io);
        _exit(0);
    }
    close(in_p[0]); close(out_p[1]); close(tel_p[1]); close(evt_p[1]);
    if (send_run)
        kill(pid, SIGUSR2);
    t0 = now_s();
    if (write_full(in_p[1], in, (size_t)n * sizeof(Lot)) < 0)
        return -1.0;
    close(in_p[1]);
    while (k < MAX_LOTS && read_msg(out_p[0], &l, sizeof l) == 1)
        out[k++] = l;
    t1 = now_s();
    waitpid(pid, &st, 0);
    close(out_p[0]); close(tel_p[0]); close(evt_p[0]);
    *n_out = k;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? t1 - t0 : -1.0;
}

static void set_omp(Config *c, int on, int threads)
{
    c->use_openmp = on;
    c->omp_threads = threads;
}

static int lots_equal(const Lot *a, const Lot *b, int n)
{
    return memcmp(a, b, (size_t)n * sizeof(Lot)) == 0;
}

static void check_cleaning_physics(const Config *cfg, const Lot *in, const Lot *out, int n)
{
    int i, ok_mass = 1, ok_range = 1, ok_other = 1;
    for (i = 0; i < n; i++) {
        double debris_kg = in[i].mass_kg * in[i].debris_pct / 100.0;
        double left_kg = out[i].mass_kg * out[i].debris_pct / 100.0;
        double removal = 100.0 * (1.0 - left_kg / debris_kg); /* % of the debris that was removed */
        if (!(out[i].mass_kg > 0.0 && out[i].mass_kg < in[i].mass_kg &&
              out[i].debris_pct >= 0.0 && out[i].debris_pct <= 100.0 &&
              out[i].debris_pct < in[i].debris_pct))
            ok_mass = 0;
        if (removal < cfg->cleaning_removal_pct.min - 1e-6 || removal > cfg->cleaning_removal_pct.max + 1e-6)
            ok_range = 0;
        if (out[i].impurity_pct != in[i].impurity_pct || out[i].lot_id != in[i].lot_id ||
            out[i].batch_id != in[i].batch_id)
            ok_other = 0;
    }
    CHECK(ok_mass, "cleaning: mass decreases (but stays > 0), debris %% decreases and stays in [0,100]");
    CHECK(ok_range, "cleaning: debris removal is inside cleaning_removal_pct [%g, %g]",
          cfg->cleaning_removal_pct.min, cfg->cleaning_removal_pct.max);
    CHECK(ok_other, "cleaning: impurity, lot_id and batch_id are untouched");
}

static void check_washing_physics(const Config *cfg, const Lot *in, const Lot *out, int n)
{
    int i, ok_mass = 1, ok_range = 1, ok_other = 1;
    for (i = 0; i < n; i++) {
        double removal = 100.0 * (1.0 - out[i].impurity_pct / in[i].impurity_pct);
        if (out[i].mass_kg != in[i].mass_kg)
            ok_mass = 0;
        if (removal < cfg->washing_removal_pct.min - 1e-6 || removal > cfg->washing_removal_pct.max + 1e-6)
            ok_range = 0;
        if (out[i].debris_pct != in[i].debris_pct || out[i].lot_id != in[i].lot_id)
            ok_other = 0;
    }
    CHECK(ok_mass, "washing: mass is unchanged");
    CHECK(ok_range, "washing: impurity removal is inside washing_removal_pct [%g, %g]",
          cfg->washing_removal_pct.min, cfg->washing_removal_pct.max);
    CHECK(ok_other, "washing: debris, lot_id are untouched");
}

static void compare_stage(const char *name, MainFn fn, int send_run, int n, int elements)
{
    static Lot in[MAX_LOTS], base[MAX_LOTS], o2[MAX_LOTS], o4[MAX_LOTS], again[MAX_LOTS];
    Config cfg;
    int nb = 0, n2 = 0, n4 = 0, na = 0;
    double t_off, t_on2, t_on4;

    printf("--- %s (%d lots, %d elements per lot)\n", name, n, elements);
    config_set_defaults(&cfg);
    cfg.time_scale = FAST_SCALE;
    cfg.sim_elements_per_lot = elements;
    make_input(in, n);

    set_omp(&cfg, 0, 1);
    t_off = run_stage(fn, &cfg, send_run, in, n, base, &nb);
    set_omp(&cfg, 1, 2);
    t_on2 = run_stage(fn, &cfg, send_run, in, n, o2, &n2);
    set_omp(&cfg, 1, 4);
    t_on4 = run_stage(fn, &cfg, send_run, in, n, o4, &n4);
    CHECK(t_off >= 0 && t_on2 >= 0 && t_on4 >= 0 && nb == n && n2 == n && n4 == n,
          "all runs finished and returned %d lots", n);
    CHECK(lots_equal(base, o2, n), "use_openmp=0 and use_openmp=1/2 threads give IDENTICAL lots (bit for bit)");
    CHECK(lots_equal(base, o4, n), "use_openmp=0 and use_openmp=1/4 threads give IDENTICAL lots (bit for bit)");
    run_stage(fn, &cfg, send_run, in, n, again, &na);
    CHECK(na == n && lots_equal(o4, again, n), "two identical runs give identical lots (deterministic)");

    if (strcmp(name, "cleaning") == 0)
        check_cleaning_physics(&cfg, in, base, nb);
    else
        check_washing_physics(&cfg, in, base, nb);

    printf("  TIME  OpenMP off: %.3f s | on, 2 threads: %.3f s (x%.2f) | on, 4 threads: %.3f s (x%.2f)\n",
           t_off, t_on2, t_off / t_on2, t_on4, t_off / t_on4);
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 60;
    int elements = argc > 2 ? atoi(argv[2]) : 2000000;

    if (n < 1 || n > MAX_LOTS || elements < 1) {
        fprintf(stderr, "usage: %s [lots 1..%d] [elements >= 1]\n", argv[0], MAX_LOTS);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("online CPUs: %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    compare_stage("cleaning", cleaning_main, 1, n, elements);
    compare_stage("washing", washing_main, 0, n, elements);
    printf("== omp_compare_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
