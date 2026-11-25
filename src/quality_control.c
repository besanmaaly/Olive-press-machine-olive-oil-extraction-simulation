#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

/* Quality-control process. The stage telemetry path is separate from
 * the normal event path: QC forwards each telemetry item exactly once. */
static int alert_if_needed(const Config *cfg, const StageIO *io,
                           const EventMsg *e, Range limit, double value,
                           const char *parameter)
{
    const double span = limit.max - limit.min;
    const double margin = span * cfg->warn_margin_pct / 100.0;
    int type = 0;
    if (!isfinite(value) || value < limit.min || value > limit.max)
        type = EV_ALERT_CRITICAL;
    else if (value < limit.min + margin || value > limit.max - margin)
        type = EV_ALERT_WARNING;
    if (type) {
        EventMsg a = make_event(type, e->stage, 0, e->lot_id,
                                value, limit.min, limit.max, parameter);
        return send_event(io->event_fd, &a);
    }
    return 0;
}

static int report_counts(const StageIO *io, const unsigned long counts[NUM_STAGES])
{
    char text[48];
    (void)snprintf(text, sizeof text, "lots R%lu C%lu W%lu Cr%lu M%lu P%lu",
                   counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
    EventMsg summary = make_event(EV_INFO, -1, 0, 0, 0, 0, 0, text);
    return send_event(io->event_fd, &summary);
}

void quality_control_main(const Config *cfg, const StageIO *io)
{
    unsigned long lots[NUM_STAGES] = {0};
    double avg_ms[NUM_STAGES] = {0};
    double next_report, period_s;
    (void)avg_ms;
    if (!cfg || !io || io->in_fd < 0 || io->event_fd < 0 ||
        cfg->time_scale <= 0 || cfg->qc_report_interval_ms <= 0)
        return;
    stage_signals_init();
    stage_signals_unblock();
    period_s = cfg->qc_report_interval_ms / (1000.0 * cfg->time_scale);
    next_report = now_s() + period_s;
    while (!g_terminate) {
        const double remaining_ms = (next_report - now_s()) * 1000.0;
        int timeout_ms = remaining_ms > INT_MAX ? INT_MAX :
                         remaining_ms < 10.0 ? 10 : (int)ceil(remaining_ms);
        struct pollfd pfd = { .fd = io->in_fd, .events = POLLIN | POLLHUP };
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("quality_control: poll");
            break;
        }
        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            EventMsg e;
            int r = read_msg(io->in_fd, &e, sizeof e);
            if (r == -2) continue;
            if (r <= 0) {
                if (r == -1) perror("quality_control: read");
                break;
            }
            if (e.type == EV_TELEMETRY && e.stage >= 0 && e.stage < NUM_STAGES) {
                const int s = e.stage;
                const double old_count = (double)lots[s];
                lots[s]++;
                avg_ms[s] += (e.v3 - avg_ms[s]) / (old_count + 1.0);
                if (send_event(io->event_fd, &e) < 0) break;
                if (alert_if_needed(cfg, io, &e, cfg->temp_limit_c[s],
                                    e.v1, "temperature") < 0) break;
                if (alert_if_needed(cfg, io, &e, cfg->pressure_limit_bar[s],
                                    e.v2, "pressure") < 0) break;
                if (isfinite(e.v3) && e.v3 > cfg->max_proc_time_ms[s]) {
                    EventMsg a = make_event(EV_ALERT_WARNING, s, 0, e.lot_id,
                                            e.v3, 0, cfg->max_proc_time_ms[s], "time");
                    if (send_event(io->event_fd, &a) < 0) break;
                }
            }
        }
        if (now_s() >= next_report) {
            if (report_counts(io, lots) < 0) break;
            next_report = now_s() + period_s;
        }
    }
    EventMsg done = make_event(EV_INFO, -1, 0, 0, 0, 0, 0, "QC done");
    (void)send_event(io->event_fd, &done);
    close(io->in_fd);
    close(io->event_fd);
}
