/* tests/pipeline_p2_test.c -- Partner 2: mini pipeline  reception -> cleaning -> washing -> sink
 * built exactly like the supervisor will build the real one (anonymous pipes, USR1/USR2/TERM...
 * blocked before fork, fork order washing, cleaning, reception, belt signals reception->cleaning).
 * It checks the belt protocol with the REAL stages:
 *   - every lot created by reception reaches the sink, in order, none lost, no hang
 *   - number of lots = sum over batches of ceil(batch kg / chunk kg)
 *   - EV_BELT: one RUN per batch, one STOP between batches, NO STOP after the last batch
 *   - EV_STAGE_DONE from all three stages, all processes exit with status 0 (no zombies)
 *   - optional SIGTERM mode: all three stages stop promptly when terminated mid-run
 *
 * Usage: ./pipeline_p2_test <config.cfg> [time_scale] [tick_ms] [zero_interval 0/1] [term_after_ms]
 * Build: gcc -std=gnu11 -Wall -Wextra -g -fopenmp -Iinclude tests/pipeline_p2_test.c src/config.c \
 *            src/ipc_util.c src/stage_support.c src/reception.c src/cleaning.c src/washing.c -lm -o pipeline_p2_test */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "common.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NPIPES    5 /* lot0, lot1, lot2, tel, evt */
#define MAX_BATCH 64
#define MAX_BELT  256
#define RUN_TIMEOUT_S 90

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                      \
    do {                                      \
        if (cond) { g_pass++; printf("  PASS: "); } \
        else { g_fail++; printf("  FAIL: "); }     \
        printf(__VA_ARGS__);                  \
        printf("\n");                         \
    } while (0)

static int p[NPIPES][2];

static pid_t start_stage(void (*fn)(const Config *, const StageIO *), const Config *cfg, StageIO io)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        int i, j;
        for (i = 0; i < NPIPES; i++)
            for (j = 0; j < 2; j++)
                if (p[i][j] != io.in_fd && p[i][j] != io.out_fd && p[i][j] != io.telemetry_fd &&
                    p[i][j] != io.event_fd)
                    close(p[i][j]);
        fn(cfg, &io);
        _exit(0);
    }
    return pid;
}

int main(int argc, char **argv)
{
    Config cfg;
    sigset_t blk;
    pid_t pid_w, pid_c, pid_r;
    StageIO io;
    int i, st, term_ms;
    int lot_fd, tel_fd, evt_fd, open_n = 3;
    double expected_kg[MAX_BATCH + 1] = {0};
    int batches_seen = 0, n_lots = 0, in_order = 1, expected_lots = 0;
    int belt[MAX_BELT], n_belt = 0, done_mask = 0;
    uint32_t next_id = 0;
    double t_start, max_chunk = 0;

    if (argc < 2 || config_load(argv[1], &cfg) != 0) {
        fprintf(stderr, "usage: %s <config.cfg> [time_scale] [tick_ms] [zero_interval] [term_after_ms]\n", argv[0]);
        return 2;
    }
    if (argc > 2) cfg.time_scale = atof(argv[2]);
    if (argc > 3) cfg.conveyor_tick_ms = atoi(argv[3]);
    if (argc > 4 && atoi(argv[4])) cfg.batch_interval_ms = make_range(0, 0);
    term_ms = argc > 5 ? atoi(argv[5]) : 0;
    cfg.fault_prob_pct = 0.0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("pipeline: batches=%d time_scale=%g tick=%d ms interval=[%g,%g] chunk=%g kg%s\n", cfg.num_batches,
           cfg.time_scale, cfg.conveyor_tick_ms, cfg.batch_interval_ms.min, cfg.batch_interval_ms.max,
           cfg.conveyor_chunk_kg, term_ms ? " (SIGTERM mode)" : "");

    sigemptyset(&blk); /* the supervisor blocks these before any fork */
    sigaddset(&blk, SIGUSR1); sigaddset(&blk, SIGUSR2);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    for (i = 0; i < NPIPES; i++)
        if (pipe(p[i])) { perror("pipe"); return 2; }

    io.in_fd = p[1][0]; io.out_fd = p[2][1]; io.telemetry_fd = p[3][1]; io.event_fd = p[4][1]; io.peer_pid = -1;
    pid_w = start_stage(washing_main, &cfg, io);
    io.in_fd = p[0][0]; io.out_fd = p[1][1];
    pid_c = start_stage(cleaning_main, &cfg, io);
    io.in_fd = -1; io.out_fd = p[0][1]; io.peer_pid = pid_c;
    pid_r = start_stage(reception_main, &cfg, io);

    lot_fd = p[2][0]; tel_fd = p[3][0]; evt_fd = p[4][0];
    close(p[0][0]); close(p[0][1]); close(p[1][0]); close(p[1][1]); close(p[2][1]); close(p[3][1]); close(p[4][1]);

    t_start = now_s();
    while (open_n > 0 && now_s() - t_start < RUN_TIMEOUT_S) {
        struct pollfd pf[3] = {{lot_fd, POLLIN, 0}, {tel_fd, POLLIN, 0}, {evt_fd, POLLIN, 0}};
        int k;
        if (term_ms && now_s() - t_start > term_ms / 1000.0) {
            kill(pid_r, SIGTERM); kill(pid_c, SIGTERM); kill(pid_w, SIGTERM);
            term_ms = 0;
        }
        if (poll(pf, 3, 100) < 0 && errno != EINTR) break;
        for (k = 0; k < 3; k++) {
            int fd = k == 0 ? lot_fd : (k == 1 ? tel_fd : evt_fd), r;
            if (fd < 0 || !(pf[k].revents & (POLLIN | POLLHUP))) continue;
            if (k == 0) {
                Lot l;
                r = read_msg(fd, &l, sizeof l);
                if (r == 1) {
                    n_lots++;
                    if (l.lot_id != next_id++) in_order = 0;
                    if (l.mass_kg > max_chunk) max_chunk = l.mass_kg;
                }
            } else {
                EventMsg e;
                r = read_msg(fd, &e, sizeof e);
                if (r == 1 && k == 2) {
                    if (e.type == EV_CONTAINER && e.lot_id >= 1 && e.lot_id <= MAX_BATCH && expected_kg[e.lot_id] == 0.0) {
                        expected_kg[e.lot_id] = e.v1; /* first event of a batch = full container */
                        if (e.lot_id > (uint32_t)batches_seen) batches_seen = (int)e.lot_id;
                    }
                    if (e.type == EV_BELT && n_belt < MAX_BELT) belt[n_belt++] = (int)e.v1;
                    if (e.type == EV_STAGE_DONE) done_mask |= 1 << e.stage;
                }
            }
            if (r == 0 || r == -1) {
                close(fd); open_n--;
                if (k == 0) lot_fd = -1; else if (k == 1) tel_fd = -1; else evt_fd = -1;
            }
        }
    }
    printf("finished in %.2f s, %d lots reached the sink\n", now_s() - t_start, n_lots);
    CHECK(open_n == 0, "no hang: all pipes reached EOF (open=%d)", open_n);

    for (i = 1; i <= batches_seen; i++)
        expected_lots += (int)ceil(expected_kg[i] / cfg.conveyor_chunk_kg - 1e-9);
    {
        int pids[3] = {pid_r, pid_c, pid_w}, ok = 1;
        for (i = 0; i < 3; i++) {
            int tries = 0, w = 0;
            while (tries++ < 300 && (w = waitpid(pids[i], &st, WNOHANG)) == 0) usleep(10000);
            if (w != pids[i] || !WIFEXITED(st) || WEXITSTATUS(st) != 0) ok = 0;
        }
        CHECK(ok, "reception, cleaning and washing all exited with status 0 and were reaped (no zombies)");
    }
    if (term_ms == 0 && argc > 5 && atoi(argv[5]) > 0) {
        CHECK(now_s() - t_start < atoi(argv[5]) / 1000.0 + 3.0, "SIGTERM stopped the pipeline promptly (< 3 s)");
        CHECK(in_order, "lots that arrived are in order");
    } else {
        int runs = 0, stops = 0, alternate = 1;
        for (i = 0; i < n_belt; i++) {
            if (belt[i]) runs++; else stops++;
            if (i > 0 && belt[i] == belt[i - 1]) alternate = 0;
        }
        CHECK(batches_seen == cfg.num_batches, "%d batches were received", batches_seen);
        CHECK(n_lots == expected_lots, "all lots arrived: %d == sum of ceil(batch/chunk) = %d", n_lots, expected_lots);
        CHECK(in_order, "lots arrived in order 0..%d, none lost", n_lots - 1);
        CHECK(max_chunk <= cfg.conveyor_chunk_kg + 1e-9 && max_chunk > 0, "chunk size never exceeds %g kg", cfg.conveyor_chunk_kg);
        CHECK(runs == cfg.num_batches, "belt RUN events = %d (one per batch)", runs);
        CHECK(stops == cfg.num_batches - 1, "belt STOP events = %d (between batches, none after the last)", stops);
        CHECK(alternate && n_belt > 0 && belt[0] == 1 && belt[n_belt - 1] == 1, "belt events alternate RUN/STOP, start and end with RUN");
        CHECK((done_mask & 7) == 7, "EV_STAGE_DONE received from reception, cleaning, washing");
    }
    printf("== pipeline_p2_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
