/* src/stage_support.c -- FROZEN shared stage helpers. Do NOT edit locally. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <signal.h>
#include <unistd.h>

static void on_term(int s)
{
    (void)s;
    g_terminate = 1;
}

void stage_signals_init(void)
{
    install_handler(SIGINT, SIG_IGN, 0); /* Ctrl-C is handled by the supervisor only */
    install_handler(SIGPIPE, SIG_IGN, 0);
    install_handler(SIGTERM, on_term, 0);
}

void stage_signals_unblock(void)
{
    sigset_t s;
    int i;
    const int sigs[2] = {SIGUSR1, SIGUSR2};
    /* A stage that did not install its own SIGUSR1/SIGUSR2 handler ignores them.
     * Done here (not in init) so a pending belt signal is never discarded. */
    for (i = 0; i < 2; i++) {
        struct sigaction old;
        if (sigaction(sigs[i], NULL, &old) == 0 && old.sa_handler == SIG_DFL)
            install_handler(sigs[i], SIG_IGN, 0);
    }
    sigemptyset(&s);
    sigprocmask(SIG_SETMASK, &s, NULL);
}

void report_event(const StageIO *io, int type, StageId id, uint32_t lot_id,
                  double v1, double v2, double v3, const char *text)
{
    EventMsg e;
    if (io->event_fd < 0)
        return;
    e = make_event(type, id, 0, lot_id, v1, v2, v3, text);
    send_event(io->event_fd, &e);
}

void report_status(const StageIO *io, StageId id, StageStatus st)
{
    EventMsg e;
    if (io->event_fd < 0)
        return;
    e = make_event(EV_STAGE_STATUS, id, st, 0, 0, 0, 0, NULL);
    send_event(io->event_fd, &e);
}

void report_telemetry(const Config *cfg, const StageIO *io, StageId id,
                      uint32_t lot_id, double sim_ms, unsigned *seed)
{
    EventMsg e;
    double temp, pres;
    if (io->telemetry_fd < 0)
        return;
    temp = rand_range(seed, cfg->temp_nominal_c[id]);
    pres = rand_range(seed, cfg->pressure_nominal_bar[id]);
    if (rand_unit(seed) * 100.0 < cfg->fault_prob_pct) { /* fault injection */
        int on_temp = rand_unit(seed) < 0.5;
        int critical = rand_unit(seed) < 0.3;
        Range lim = on_temp ? cfg->temp_limit_c[id] : cfg->pressure_limit_bar[id];
        double span = lim.max - lim.min;
        double margin = span * cfg->warn_margin_pct / 100.0;
        double v = critical ? lim.max + 0.05 * span + rand_unit(seed) * 0.10 * span
                            : lim.max - 0.5 * margin; /* inside the warning band */
        if (on_temp)
            temp = v;
        else
            pres = v;
    }
    e = make_event(EV_TELEMETRY, id, ST_RUNNING, lot_id, temp, pres, sim_ms, NULL);
    send_event(io->telemetry_fd, &e);
}

void pad_processing_time(const Config *cfg, StageId id, double t0_s, unsigned *seed)
{
    double target = rand_range(seed, cfg->proc_time_ms[id]);
    double spent = (now_s() - t0_s) * 1000.0 * (cfg->time_scale > 0 ? cfg->time_scale : 1.0);
    if (target > spent)
        sleep_ms_scaled(target - spent, cfg->time_scale, &g_terminate);
}

int stage_run(const Config *cfg, const StageIO *io, StageId id, ProcessFn fn)
{
    unsigned seed = cfg->seed + 7919u * (unsigned)(id + 1);
    Lot lot;

    report_status(io, id, ST_IDLE);
    while (!g_terminate) {
        double t0, real_ms, sim_ms;
        int r = read_msg(io->in_fd, &lot, sizeof lot);
        if (r == -2)
            continue;
        if (r <= 0)
            break;
        report_status(io, id, ST_RUNNING);
        g_excluded_ms = 0.0;
        t0 = now_s();
        fn(cfg, &lot, &seed);
        real_ms = (now_s() - t0) * 1000.0 - g_excluded_ms;
        if (real_ms < 0.0)
            real_ms = 0.0;
        sim_ms = real_ms * (cfg->time_scale > 0 ? cfg->time_scale : 1.0);
        report_telemetry(cfg, io, id, lot.lot_id, sim_ms, &seed);
        if (io->out_fd >= 0) {
            if (write_full(io->out_fd, &lot, sizeof lot) < 0)
                break;
        } else {
            report_event(io, EV_LOT_FINISHED, id, lot.lot_id, lot.oil_kg, lot.water_kg,
                         lot.pomace_kg, NULL);
        }
        report_status(io, id, ST_IDLE);
    }
    report_event(io, EV_STAGE_DONE, id, 0, 0, 0, 0, NULL);
    if (io->in_fd >= 0)
        close(io->in_fd);
    if (io->out_fd >= 0)
        close(io->out_fd);
    return 0;
}
