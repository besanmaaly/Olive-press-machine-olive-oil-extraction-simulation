/* tests/stage_harness.c -- shared stand-alone test driver for ONE stage process.
 * Usage: ./stage_harness <stage> [lots=10] [time_scale=20] [fault_pct=0]
 * It builds the pipes exactly like the supervisor will, forks the stage, feeds fake
 * lots, and prints everything the stage writes (lots, telemetry, events).
 *  - reception : a dummy "cleaning" child is forked and reports belt signals
 *  - cleaning  : the harness sends SIGUSR2 at start, SIGUSR1 after 1/3 of the lots,
 *                SIGUSR2 after 2/3 (belt stop / resume test)
 * Link with: src/ipc_util.c src/stage_support.c <the stage .c files> (any stage
 * mains you do not own can be provided by tests/mock_stages.c).             */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void (*MainFn)(const Config *, const StageIO *);
static const MainFn MAINS[NUM_STAGES] = {reception_main, cleaning_main, washing_main,
                                         crushing_main,  malaxation_main, pressing_main};

static void dummy_stop(int s)
{
    static const char m[] = "[dummy-cleaning] BELT STOP signal (SIGUSR1)\n";
    ssize_t r = write(1, m, sizeof m - 1);
    (void)s;
    (void)r;
}
static void dummy_run(int s)
{
    static const char m[] = "[dummy-cleaning] BELT RUN signal (SIGUSR2)\n";
    ssize_t r = write(1, m, sizeof m - 1);
    (void)s;
    (void)r;
}

static void run_dummy_cleaning(int rfd)
{
    Lot l;
    int n = 0, r;
    stage_signals_init();
    install_handler(SIGUSR1, dummy_stop, 0);
    install_handler(SIGUSR2, dummy_run, 0);
    stage_signals_unblock();
    while ((r = read_msg(rfd, &l, sizeof l)) != 0) {
        if (r == 1) {
            n++;
            printf("[dummy-cleaning] lot %u mass=%.1f\n", l.lot_id, l.mass_kg);
            fflush(stdout);
        } else if (r == -1)
            break;
    }
    printf("[dummy-cleaning] received %d lots\n", n);
    fflush(stdout);
}

static void print_lot(const Lot *l)
{
    printf("[lot] id=%u batch=%u mass=%.2f debris=%.2f imp=%.2f paste=%.2f drop=%.1f "
           "oil=%.2f water=%.2f pomace=%.2f\n",
           l->lot_id, l->batch_id, l->mass_kg, l->debris_pct, l->impurity_pct,
           l->paste_particle_mm, l->oil_droplet_um, l->oil_kg, l->water_kg, l->pomace_kg);
}
static void print_event(const EventMsg *e, const char *tag)
{
    printf("[%s] type=%d stage=%s status=%d lot=%u v=(%.2f, %.2f, %.2f) %s\n", tag, e->type,
           stage_name(e->stage), e->status, e->lot_id, e->v1, e->v2, e->v3, e->text);
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "cleaning";
    int nlots = argc > 2 ? atoi(argv[2]) : 10;
    int id = -1, i, in_p[2], out_p[2], tel_p[2], evt_p[2];
    Config cfg;
    sigset_t blk;
    pid_t dummy = -1, child;

    for (i = 0; i < NUM_STAGES; i++)
        if (strcmp(name, stage_name(i)) == 0)
            id = i;
    if (id < 0) {
        fprintf(stderr, "unknown stage '%s'\n", name);
        return 2;
    }
    config_set_defaults(&cfg);
    cfg.time_scale = argc > 3 ? atof(argv[3]) : 20.0;
    cfg.fault_prob_pct = argc > 4 ? atof(argv[4]) : 0.0;

    sigemptyset(&blk); /* same as the supervisor: children start with USR1/USR2 blocked */
    sigaddset(&blk, SIGUSR1);
    sigaddset(&blk, SIGUSR2);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    if (pipe(in_p) || pipe(out_p) || pipe(tel_p) || pipe(evt_p)) {
        perror("pipe");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (id == STAGE_RECEPTION) {
        dummy = fork();
        if (dummy == 0) {
            close(in_p[0]); close(in_p[1]); close(out_p[1]);
            close(tel_p[0]); close(tel_p[1]); close(evt_p[0]); close(evt_p[1]);
            run_dummy_cleaning(out_p[0]);
            _exit(0);
        }
    }
    child = fork();
    if (child == 0) {
        StageIO io;
        close(in_p[1]); close(out_p[0]); close(tel_p[0]); close(evt_p[0]);
        io.in_fd = (id == STAGE_RECEPTION) ? -1 : in_p[0];
        io.out_fd = (id == STAGE_PRESSING) ? -1 : out_p[1];
        io.telemetry_fd = tel_p[1];
        io.event_fd = evt_p[1];
        io.peer_pid = dummy;
        MAINS[id](&cfg, &io);
        _exit(0);
    }
    close(in_p[0]); close(out_p[1]); close(tel_p[1]); close(evt_p[1]);
    if (id == STAGE_PRESSING) { close(out_p[0]); out_p[0] = -1; }
    if (id == STAGE_RECEPTION) { close(in_p[1]); in_p[1] = -1; close(out_p[0]); out_p[0] = -1; }

    if (id != STAGE_RECEPTION) {
        if (id == STAGE_CLEANING)
            kill(child, SIGUSR2);
        for (i = 0; i < nlots; i++) {
            Lot l;
            memset(&l, 0, sizeof l);
            l.lot_id = (uint32_t)i;
            l.batch_id = 1;
            l.mass_kg = 48.0;
            l.debris_pct = (id == STAGE_CLEANING) ? 5.0 : 0.5;
            l.impurity_pct = (id <= STAGE_WASHING) ? 6.0 : 0.3;
            l.paste_particle_mm = 2.5;
            l.oil_droplet_um = 45.0;
            l.t_created = now_s();
            if (id == STAGE_CLEANING && i == nlots / 3) kill(child, SIGUSR1);
            if (id == STAGE_CLEANING && i == 2 * nlots / 3) kill(child, SIGUSR2);
            write_full(in_p[1], &l, sizeof l);
            usleep(50000);
        }
        close(in_p[1]);
    }

    {
        int fds[3] = {out_p[0], tel_p[0], evt_p[0]}, open_n = 0, n_out = 0, n_tel = 0;
        for (i = 0; i < 3; i++) if (fds[i] >= 0) open_n++;
        while (open_n > 0) {
            struct pollfd p[3];
            for (i = 0; i < 3; i++) { p[i].fd = fds[i]; p[i].events = POLLIN; p[i].revents = 0; }
            if (poll(p, 3, -1) < 0) { if (errno == EINTR) continue; break; }
            for (i = 0; i < 3; i++) {
                int r;
                if (fds[i] < 0 || !(p[i].revents & (POLLIN | POLLHUP))) continue;
                if (i == 0) {
                    Lot l;
                    r = read_msg(fds[i], &l, sizeof l);
                    if (r == 1) { print_lot(&l); n_out++; }
                } else {
                    EventMsg e;
                    r = read_msg(fds[i], &e, sizeof e);
                    if (r == 1) { print_event(&e, i == 1 ? "tel" : "evt"); if (i == 1) n_tel++; }
                }
                if (r == 0 || r == -1) { close(fds[i]); fds[i] = -1; open_n--; }
            }
        }
        {
            int st = 0;
            waitpid(child, &st, 0);
            if (dummy > 0) waitpid(dummy, NULL, 0);
            printf("== %s: fed %d lots, %d lots out, %d telemetry msgs, exit=%d\n", name,
                   id == STAGE_RECEPTION ? 0 : nlots, n_out, n_tel, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        }
    }
    return 0;
}
