/* tests/cleaning_belt_test.c -- Partner 2: timing-based test of the belt <-> cleaning link.
 *
 * Forks the real cleaning process (same pipes / signal mask as the supervisor) and checks:
 *   T1  belt STOP while a lot is in progress: the lot is NOT finished while stopped
 *       (cleaning really stops in the middle of a lot), it finishes after RUN
 *   T2  the time spent waiting is excluded from the processing time sent to QC
 *   T3  a lot that arrives while the belt is stopped is held until RUN
 *   T4  SIGTERM while waiting for the belt ends the process promptly (no hang)
 *   T5  SIGTERM while idle (blocked in read) ends the process promptly
 *   T6  EOF on the input pipe ends cleaning and closes its output (drain rule)
 *   T7  belt-signal storm (USR1/USR2 every millisecond): no signal is lost, no lot is lost,
 *       no hang - this also guards against OpenMP worker threads swallowing signals
 *   T8  SIGTERM after OpenMP worker threads exist (must still reach the main thread)
 *
 * Build: gcc -std=gnu11 -Wall -Wextra -g -fopenmp -Iinclude tests/cleaning_belt_test.c \
 *            src/ipc_util.c src/stage_support.c src/cleaning.c -lm -o cleaning_belt_test
 * Exit code: 0 = all checks passed, 1 = at least one failed.                          */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOT_SIM_MS     400.0 /* fixed processing time per lot (simulated ms) */
#define TIME_SCALE     2.0   /* -> 200 real ms per lot, 25 real ms per slice  */
#define SLICE_REAL_MS  25.0
#define HOLD_MS        1200  /* how long the belt stays stopped in T1         */

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) {                                          \
            g_pass++;                                        \
            printf("  PASS: ");                              \
        } else {                                             \
            g_fail++;                                        \
            printf("  FAIL: ");                              \
        }                                                    \
        printf(__VA_ARGS__);                                 \
        printf("\n");                                        \
    } while (0)

typedef struct {
    pid_t pid;
    int in_w, out_r, tel_r, evt_r;
} Run;

static void make_cfg(Config *cfg)
{
    config_set_defaults(cfg);
    cfg->time_scale = TIME_SCALE;
    cfg->sim_elements_per_lot = 20000;
    cfg->proc_time_ms[STAGE_CLEANING] = make_range(LOT_SIM_MS, LOT_SIM_MS);
    cfg->fault_prob_pct = 0.0;
}

static Run spawn_cleaning(const Config *cfg)
{
    Run r;
    int in_p[2], out_p[2], tel_p[2], evt_p[2];
    sigset_t blk;

    sigemptyset(&blk); /* like the supervisor: children start with USR1/USR2 blocked */
    sigaddset(&blk, SIGUSR1);
    sigaddset(&blk, SIGUSR2);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    if (pipe(in_p) || pipe(out_p) || pipe(tel_p) || pipe(evt_p)) {
        perror("pipe");
        exit(2);
    }
    r.pid = fork();
    if (r.pid < 0) {
        perror("fork");
        exit(2);
    }
    if (r.pid == 0) {
        StageIO io;
        close(in_p[1]); close(out_p[0]); close(tel_p[0]); close(evt_p[0]);
        io.in_fd = in_p[0];
        io.out_fd = out_p[1];
        io.telemetry_fd = tel_p[1];
        io.event_fd = evt_p[1];
        io.peer_pid = -1;
        cleaning_main(cfg, &io);
        _exit(0);
    }
    close(in_p[0]); close(out_p[1]); close(tel_p[1]); close(evt_p[1]);
    r.in_w = in_p[1];
    r.out_r = out_p[0];
    r.tel_r = tel_p[0];
    r.evt_r = evt_p[0];
    return r;
}

static void feed(const Run *r, uint32_t id)
{
    Lot l;
    memset(&l, 0, sizeof l);
    l.lot_id = id;
    l.batch_id = 1;
    l.mass_kg = 48.0;
    l.debris_pct = 5.0;
    l.impurity_pct = 6.0;
    l.t_created = now_s();
    write_full(r->in_w, &l, sizeof l);
}

/* 1 = lot read, 0 = timeout, -1 = EOF/error */
static int wait_lot(const Run *r, int timeout_ms, Lot *l)
{
    struct pollfd p;
    int pr;
    p.fd = r->out_r;
    p.events = POLLIN;
    p.revents = 0;
    do {
        pr = poll(&p, 1, timeout_ms);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0)
        return 0;
    return read_msg(r->out_r, l, sizeof *l) == 1 ? 1 : -1;
}

/* waits (polling) until the child has exited; returns 1 if it exited within timeout_ms */
static int wait_exit(Run *r, int timeout_ms, int *status)
{
    double t0 = now_s();
    while ((now_s() - t0) * 1000.0 < timeout_ms) {
        pid_t w = waitpid(r->pid, status, WNOHANG);
        if (w == r->pid)
            return 1;
        usleep(10000);
    }
    return 0;
}

/* reads all remaining messages of a pipe (writer must be gone); returns the count */
static int drain_events(int fd, EventMsg *out, int max)
{
    int n = 0;
    EventMsg e;
    while (read_msg(fd, &e, sizeof e) == 1)
        if (n < max)
            out[n++] = e;
    return n;
}

static void test_stop_mid_lot(void)
{
    Config cfg;
    Run r;
    Lot l;
    double t_stop, t_run, t_l1 = 0, tmp;
    int got, i, n_evt, seen_stopped = 0, seen_running_after = 0, st;
    EventMsg ev[256], tel[64];
    int n_tel, tel_ok = 1;

    printf("T1/T2/T6: belt STOP in the middle of a lot, RUN later, then EOF\n");
    make_cfg(&cfg);
    r = spawn_cleaning(&cfg);
    kill(r.pid, SIGUSR2); /* belt running */
    for (i = 0; i < 4; i++)
        feed(&r, (uint32_t)i);

    got = wait_lot(&r, 3000, &l);
    CHECK(got == 1 && l.lot_id == 0, "lot 0 arrives while the belt runs");
    usleep(60 * 1000); /* now cleaning is inside lot 1 (200 ms long) */
    t_stop = now_s();
    kill(r.pid, SIGUSR1); /* belt STOP */

    got = wait_lot(&r, HOLD_MS, &l);
    CHECK(got == 0, "no lot finished during %d ms of stopped belt (lot 1 was interrupted mid-way)", HOLD_MS);

    t_run = now_s();
    kill(r.pid, SIGUSR2); /* belt RUN */
    got = wait_lot(&r, 3000, &l);
    if (got == 1 && l.lot_id == 1)
        t_l1 = now_s();
    CHECK(got == 1 && l.lot_id == 1, "lot 1 completes after RUN");
    tmp = (t_l1 - t_run) * 1000.0;
    CHECK(got == 1 && tmp < LOT_SIM_MS / TIME_SCALE + 150.0,
          "lot 1 needed only its remaining time after RUN (%.0f ms <= %.0f ms)", tmp,
          LOT_SIM_MS / TIME_SCALE + 150.0);
    CHECK(((t_run - t_stop) * 1000.0) >= HOLD_MS - 50, "belt really stayed stopped %.0f ms",
          (t_run - t_stop) * 1000.0);
    for (i = 2; i < 4; i++) {
        got = wait_lot(&r, 3000, &l);
        CHECK(got == 1 && l.lot_id == (uint32_t)i, "lot %d arrives", i);
    }
    CHECK(l.mass_kg < 48.0 && l.mass_kg > 0.0 && l.debris_pct < 5.0 && l.debris_pct >= 0.0 &&
              l.impurity_pct == 6.0,
          "cleaning: mass %.3f < 48, debris %.3f%% < 5%%, impurity untouched", l.mass_kg, l.debris_pct);

    /* T6: close input -> cleaning must finish and close its output */
    close(r.in_w);
    got = wait_lot(&r, 3000, &l);
    CHECK(got == -1, "EOF on the input pipe closes the output pipe (drain rule)");
    CHECK(wait_exit(&r, 3000, &st) && WIFEXITED(st) && WEXITSTATUS(st) == 0, "process exits with status 0");

    n_evt = drain_events(r.evt_r, ev, 256);
    n_tel = drain_events(r.tel_r, tel, 64);
    for (i = 0; i < n_evt; i++) {
        if (ev[i].type == EV_STAGE_STATUS && ev[i].status == ST_STOPPED)
            seen_stopped = 1;
        if (seen_stopped && ev[i].type == EV_STAGE_STATUS && ev[i].status == ST_RUNNING)
            seen_running_after = 1;
    }
    CHECK(seen_stopped && seen_running_after, "status events: ST_STOPPED then ST_RUNNING were reported");
    CHECK(n_tel == 4, "4 telemetry messages (got %d)", n_tel);
    for (i = 0; i < n_tel; i++)
        if (tel[i].v3 > LOT_SIM_MS * 1.75) {
            tel_ok = 0;
            printf("    lot %u reported %.0f simulated ms\n", tel[i].lot_id, tel[i].v3);
        }
    CHECK(tel_ok, "T2: waiting time is excluded: every lot reports <= %.0f simulated ms", LOT_SIM_MS * 1.75);
    close(r.out_r); close(r.tel_r); close(r.evt_r);
    (void)SLICE_REAL_MS;
}

static void test_hold_and_term(void)
{
    Config cfg;
    Run r;
    Lot l;
    int got, st;

    printf("T3/T4: lot arrives while belt is stopped; SIGTERM while waiting\n");
    make_cfg(&cfg);
    r = spawn_cleaning(&cfg);
    feed(&r, 0); /* belt has never been started -> must be held */
    got = wait_lot(&r, 600, &l);
    CHECK(got == 0, "T3: lot held for 600 ms while the belt is stopped");
    kill(r.pid, SIGUSR2);
    got = wait_lot(&r, 3000, &l);
    CHECK(got == 1 && l.lot_id == 0, "T3: lot released after RUN");

    kill(r.pid, SIGUSR1); /* stop again */
    usleep(100 * 1000);
    feed(&r, 1);          /* cleaning blocks in wait_for_belt() */
    usleep(200 * 1000);
    kill(r.pid, SIGTERM);
    CHECK(wait_exit(&r, 2000, &st) && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "T4: SIGTERM while waiting for the belt -> process exits (status 0) within 2 s");
    got = wait_lot(&r, 200, &l);
    CHECK(got != 1 || (l.mass_kg < 48.0 && l.debris_pct < 5.0),
          "T4: a lot forwarded during shutdown is fully cleaned, never half-processed");
    close(r.in_w); close(r.out_r); close(r.tel_r); close(r.evt_r);
}

static void test_term_idle(void)
{
    Config cfg;
    Run r;
    int st;

    printf("T5: SIGTERM while idle (blocked in read)\n");
    make_cfg(&cfg);
    r = spawn_cleaning(&cfg);
    kill(r.pid, SIGUSR2);
    usleep(100 * 1000);
    kill(r.pid, SIGTERM);
    CHECK(wait_exit(&r, 2000, &st) && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "T5: idle process exits (status 0) within 2 s after SIGTERM");
    close(r.in_w); close(r.out_r); close(r.tel_r); close(r.evt_r);
}


static void test_belt_storm(void)
{
    Config cfg;
    int rep, k, i, all_ok = 1, exit_ok = 1;
    const int lots = 30, reps = 6;

    printf("T7: belt-signal storm, %d repetitions\n", reps);
    make_cfg(&cfg);
    cfg.time_scale = 20.0; /* 20 real ms per lot */
    for (rep = 0; rep < reps; rep++) {
        Run r = spawn_cleaning(&cfg);
        Lot l;
        int got_n = 0, st;
        kill(r.pid, SIGUSR2);
        for (i = 0; i < lots; i++)
            feed(&r, (uint32_t)i);
        for (k = 0; k < 300; k++) {
            /* STOP immediately followed by RUN: both are pending at the same time while the
             * target is not scheduled (standard signals are not queued).  The handlers must
             * not run nested/reordered, otherwise the belt stays "stopped" although the last
             * signal sent was RUN. */
            kill(r.pid, SIGUSR1);
            kill(r.pid, SIGUSR2);
            if (k % 3 == 0)
                usleep(500);
        }
        /* the last signal sent was RUN: every lot must complete */
        while (got_n < lots && wait_lot(&r, 5000, &l) == 1)
            got_n++;
        if (got_n != lots) {
            all_ok = 0;
            printf("    repetition %d: only %d of %d lots arrived\n", rep, got_n, lots);
        }
        close(r.in_w);
        if (!wait_exit(&r, 3000, &st) || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            exit_ok = 0;
            kill(r.pid, SIGKILL);
            waitpid(r.pid, &st, 0);
        }
        close(r.out_r); close(r.tel_r); close(r.evt_r);
    }
    CHECK(all_ok, "T7: all %d lots arrived in every repetition (no lost signal, no hang)", lots);
    CHECK(exit_ok, "T7: cleaning exited cleanly after every storm");
}

static void test_term_after_workers(void)
{
    Config cfg;
    int rep, ok = 1;
    const int reps = 10;

    printf("T8: SIGTERM while idle, after the OpenMP workers were created (%d repetitions)\n", reps);
    make_cfg(&cfg);
    cfg.time_scale = 20.0;
    for (rep = 0; rep < reps; rep++) {
        Run r = spawn_cleaning(&cfg);
        Lot l;
        int st;
        kill(r.pid, SIGUSR2);
        feed(&r, 0);
        feed(&r, 1);
        if (wait_lot(&r, 3000, &l) != 1 || wait_lot(&r, 3000, &l) != 1)
            ok = 0;
        usleep(20 * 1000);
        kill(r.pid, SIGTERM);
        if (!wait_exit(&r, 2000, &st) || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            ok = 0;
            kill(r.pid, SIGKILL);
            waitpid(r.pid, &st, 0);
        }
        close(r.in_w); close(r.out_r); close(r.tel_r); close(r.evt_r);
    }
    CHECK(ok, "T8: SIGTERM ends the process within 2 s in all %d repetitions", reps);
}

int main(void)
{
    alarm(60); /* safety net: a hang is a failure */
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_stop_mid_lot();
    test_hold_and_term();
    test_term_idle();
    test_belt_storm();
    test_term_after_workers();
    printf("== cleaning_belt_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
