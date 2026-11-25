/* tests/mock_stages.c -- placeholder implementations of EVERY process entry point and
 * kernel, so the supervisor can be built and run before the real stages exist.
 * Real files replace the mocks one by one: compile with -DREAL_RECEPTION, -DREAL_CLEANING,
 * -DREAL_WASHING, -DREAL_CRUSHING, -DREAL_MALAXATION, -DREAL_PRESSING, -DREAL_QC, -DREAL_GUI
 * for every piece that is provided by a real source file (never define both).
 * Env var MOCK_CRITICAL=1 makes the mock QC raise one CRITICAL alert on 'crushing'. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void mock_work(const Config *cfg, Lot *lot, unsigned *seed, StageId id)
{
    sleep_ms_scaled(rand_range(seed, cfg->proc_time_ms[id]) * 0.5, cfg->time_scale, &g_terminate);
    if (id == STAGE_PRESSING) {
        lot->oil_kg = lot->mass_kg * 0.15;
        lot->water_kg = lot->mass_kg * 0.40;
        lot->pomace_kg = lot->mass_kg - lot->oil_kg - lot->water_kg;
    }
}

#define MOCK_STAGE(NAME, ID)                                                          \
    void NAME##_process(const Config *cfg, Lot *lot, unsigned *seed)                  \
    {                                                                                 \
        mock_work(cfg, lot, seed, ID);                                                \
    }                                                                                 \
    void NAME##_main(const Config *cfg, const StageIO *io)                            \
    {                                                                                 \
        stage_signals_init();                                                         \
        stage_signals_unblock();                                                      \
        stage_run(cfg, io, ID, NAME##_process);                                       \
    }

#ifndef REAL_CLEANING
MOCK_STAGE(cleaning, STAGE_CLEANING)
#endif
#ifndef REAL_WASHING
MOCK_STAGE(washing, STAGE_WASHING)
#endif
#ifndef REAL_CRUSHING
MOCK_STAGE(crushing, STAGE_CRUSHING)
#endif
#ifndef REAL_MALAXATION
MOCK_STAGE(malaxation, STAGE_MALAXATION)
#endif
#ifndef REAL_PRESSING
MOCK_STAGE(pressing, STAGE_PRESSING)
#endif

#ifndef REAL_RECEPTION
/* MOCK_RECEPTION: 4 lots per batch, ignores the belt but signals the cleaning process */
void reception_main(const Config *cfg, const StageIO *io)
{
    unsigned seed = cfg->seed;
    int total = cfg->num_batches * 4, i;
    stage_signals_init();
    stage_signals_unblock();
    if (io->peer_pid > 0)
        kill(io->peer_pid, SIGUSR2);
    report_status(io, STAGE_RECEPTION, ST_RUNNING);
    for (i = 0; i < total && !g_terminate; i++) {
        Lot l;
        memset(&l, 0, sizeof l);
        l.lot_id = (uint32_t)i;
        l.batch_id = (uint32_t)(i / 4 + 1);
        l.mass_kg = cfg->conveyor_chunk_kg;
        l.debris_pct = rand_range(&seed, cfg->initial_debris_pct);
        l.impurity_pct = rand_range(&seed, cfg->initial_impurity_pct);
        l.t_created = now_s();
        report_event(io, EV_CONTAINER, STAGE_RECEPTION, l.lot_id,
                     cfg->conveyor_chunk_kg * (double)(3 - i % 4), cfg->container_capacity_kg, 0, NULL);
        if (write_full(io->out_fd, &l, sizeof l) < 0)
            break;
        report_telemetry(cfg, io, STAGE_RECEPTION, l.lot_id, cfg->conveyor_tick_ms, &seed);
        sleep_ms_scaled(cfg->conveyor_tick_ms, cfg->time_scale, &g_terminate);
    }
    /* no belt-stop after the last batch: cleaning must drain its backlog */
    report_event(io, EV_STAGE_DONE, STAGE_RECEPTION, 0, 0, 0, 0, NULL);
    close(io->out_fd);
}
#endif

#ifndef REAL_QC
/* MOCK_QC: forwards telemetry to the supervisor, optionally raises one critical alert */
void quality_control_main(const Config *cfg, const StageIO *io)
{
    EventMsg e;
    int r, n = 0;
    (void)cfg;
    stage_signals_init();
    stage_signals_unblock();
    while (!g_terminate && (r = read_msg(io->in_fd, &e, sizeof e)) != 0) {
        if (r == -2) continue;
        if (r < 0) break;
        send_event(io->event_fd, &e);
        if (++n == 5 && getenv("MOCK_CRITICAL")) {
            EventMsg a = make_event(EV_ALERT_CRITICAL, STAGE_CRUSHING, 0, e.lot_id, 40.0, 10.0, 35.0, "temperature");
            send_event(io->event_fd, &a);
        }
    }
    close(io->in_fd);
}
#endif

#ifndef REAL_GUI
/* MOCK_GUI: prints every event it receives */
void gui_main(const Config *cfg, const StageIO *io)
{
    EventMsg e;
    int r;
    (void)cfg;
    stage_signals_init();
    stage_signals_unblock();
    while (!g_terminate && (r = read_msg(io->in_fd, &e, sizeof e)) != 0) {
        if (r == -2) continue;
        if (r < 0) break;
        printf("[mock-gui] type=%d stage=%s status=%d v=(%.1f,%.1f,%.1f) %s\n", e.type,
               stage_name(e.stage), e.status, e.v1, e.v2, e.v3, e.text);
        fflush(stdout);
    }
    close(io->in_fd);
}
#endif
