/* src/reception.c -- Partner 2: Reception process (container + conveyor belt + container sensor).
 *
 * Olive batches arrive one after the other in a large container.  A conveyor belt
 * moves small chunks ("lots") from the container to the cleaning stage.  A container
 * sensor decides whether the belt keeps rotating (olives left) or stops (empty).
 *
 * Belt control uses signals to the cleaning process (io->peer_pid):
 *     SIGUSR2 = belt RUNNING, SIGUSR1 = belt STOPPED.
 * Lots travel through the output pipe (io->out_fd).
 *
 * Drain rule: after the LAST batch no stop signal is sent, so cleaning can finish
 * every lot already in the pipeline; the end of the run is signalled by closing
 * out_fd (EOF ripples down the pipeline).                                          */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SENSOR_EMPTY_EPS_KG 1e-9 /* below this the container counts as empty */

/* state private to this process */
static int s_belt_running = 0;
static double s_last_signal_s = 0.0; /* now_s() of the last belt signal, 0 = none yet */
static int s_kill_warned = 0;

/* The single function that decides whether the belt keeps rotating. */
static int container_sensor_has_olives(double kg)
{
    return kg > SENSOR_EMPTY_EPS_KG;
}

/* Starts (on = 1) or stops (on = 0) the belt: signal to cleaning + event + status.
 * Standard signals are not queued, so two belt signals must never be sent closer
 * than one conveyor tick.  The main loop already sleeps one tick per chunk; the
 * guard below also covers batch_interval_ms = 0.                                  */
static void set_belt(const Config *cfg, const StageIO *io, int on)
{
    double ts = cfg->time_scale > 0.0 ? cfg->time_scale : 1.0;

    if (on == s_belt_running)
        return; /* nothing changes: no redundant signal */

    if (s_last_signal_s > 0.0) {
        double min_gap_s = (double)cfg->conveyor_tick_ms / 1000.0 / ts;
        double gap_s = now_s() - s_last_signal_s;
        if (gap_s < min_gap_s)
            sleep_ms_scaled((min_gap_s - gap_s) * 1000.0 * ts, cfg->time_scale, &g_terminate);
    }

    if (io->peer_pid > 0) {
        if (kill(io->peer_pid, on ? SIGUSR2 : SIGUSR1) < 0 && !s_kill_warned) {
            s_kill_warned = 1;
            fprintf(stderr, "reception: cannot signal cleaning (pid %d): %s\n", (int)io->peer_pid,
                    strerror(errno));
        }
    }
    s_last_signal_s = now_s();
    s_belt_running = on;
    report_event(io, EV_BELT, STAGE_RECEPTION, 0, on, 0, 0, NULL);
    report_status(io, STAGE_RECEPTION, on ? ST_RUNNING : ST_STOPPED);
}

void reception_main(const Config *cfg, const StageIO *io)
{
    double container_kg = 0.0;
    uint32_t lot_counter = 0;
    unsigned seed = cfg->seed;
    int batch, failed = 0;

    s_belt_running = 0;
    s_last_signal_s = 0.0;

    stage_signals_init(); /* reception only sends signals, it installs no handler */
    stage_signals_unblock();
    report_status(io, STAGE_RECEPTION, ST_IDLE);

    for (batch = 1; batch <= cfg->num_batches && !g_terminate && !failed; batch++) {
        if (batch > 1) { /* the next truck arrives after a random interval */
            report_status(io, STAGE_RECEPTION, ST_STOPPED);
            sleep_ms_scaled(rand_range(&seed, cfg->batch_interval_ms), cfg->time_scale, &g_terminate);
            if (g_terminate)
                break;
        }

        /* a new batch is emptied into the container */
        container_kg = rand_range(&seed, cfg->batch_mass_kg);
        if (container_kg > cfg->container_capacity_kg)
            container_kg = cfg->container_capacity_kg;
        report_event(io, EV_CONTAINER, STAGE_RECEPTION, (uint32_t)batch, container_kg,
                     cfg->container_capacity_kg, 0, NULL);

        /* the container sensor keeps the belt rotating while there are olives */
        while (!g_terminate && container_sensor_has_olives(container_kg)) {
            Lot lot;
            double chunk;

            set_belt(cfg, io, 1);

            chunk = cfg->conveyor_chunk_kg < container_kg ? cfg->conveyor_chunk_kg : container_kg;
            memset(&lot, 0, sizeof lot);
            lot.lot_id = lot_counter++;
            lot.batch_id = (uint32_t)batch;
            lot.mass_kg = chunk;
            lot.debris_pct = rand_range(&seed, cfg->initial_debris_pct);
            lot.impurity_pct = rand_range(&seed, cfg->initial_impurity_pct);
            lot.t_created = now_s();

            if (write_full(io->out_fd, &lot, sizeof lot) < 0) {
                /* EPIPE: the cleaning process is gone, nobody to feed any more */
                failed = 1;
                break;
            }
            container_kg -= chunk;
            report_telemetry(cfg, io, STAGE_RECEPTION, lot.lot_id, cfg->conveyor_tick_ms, &seed);
            report_event(io, EV_CONTAINER, STAGE_RECEPTION, (uint32_t)batch, container_kg,
                         cfg->container_capacity_kg, 0, NULL);
            sleep_ms_scaled(cfg->conveyor_tick_ms, cfg->time_scale, &g_terminate);
        }

        /* container empty: stop the belt, except after the last batch (drain rule) */
        if (!failed && !g_terminate && batch < cfg->num_batches)
            set_belt(cfg, io, 0);
    }

    if (io->out_fd >= 0)
        close(io->out_fd); /* EOF ripples downstream */
    report_event(io, EV_STAGE_DONE, STAGE_RECEPTION, 0, 0, 0, 0, NULL);
    report_status(io, STAGE_RECEPTION, ST_DONE);
}
