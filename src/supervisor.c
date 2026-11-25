/* Partner 1: Supervisor and process orchestration (Linux). */
#define _GNU_SOURCE
#include "supervisor.h"
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PIPE_READ  0
#define PIPE_WRITE 1
#define NO_FD (-1)
#define POLL_STEP_MS 250

typedef struct {
    const Config *cfg;
    int lot[NUM_STAGES - 1][2];
    int tel[2], evt[2], gui[2];
    int signal_fd;
    pid_t pid[NUM_STAGES], qc_pid, gui_pid;
    int stage_done[NUM_STAGES];
    int stage_reaped[NUM_STAGES];
    int qc_reaped, gui_reaped;
    int stage_status[NUM_STAGES];
    int stage_lots[NUM_STAGES], warnings[NUM_STAGES], criticals[NUM_STAGES];
    double last_telemetry_log[NUM_STAGES];
    double resume_at[NUM_STAGES];
    double shutdown_deadline, gui_deadline;
    double oil, water, pomace;
    double last_container_kg;
    int batch_count, lots_done, warning_total, critical_total;
    int active_children;
    int paused_all, finished, shutting_down, shutdown_sent;
    int exit_code;
} Supervisor;

static double clock_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = NO_FD;
    }
}

static void close_all_pipes(Supervisor *s)
{
    int i, j;
    for (i = 0; i < NUM_STAGES - 1; ++i)
        for (j = 0; j < 2; ++j)
            close_fd(&s->lot[i][j]);
    for (j = 0; j < 2; ++j) {
        close_fd(&s->tel[j]);
        close_fd(&s->evt[j]);
        close_fd(&s->gui[j]);
    }
}

static void init_state(Supervisor *s, const Config *cfg)
{
    int i, j;
    memset(s, 0, sizeof *s);
    s->cfg = cfg;
    s->signal_fd = -1;
    s->qc_pid = s->gui_pid = -1;
    s->last_container_kg = -1;
    for (i = 0; i < NUM_STAGES; ++i) {
        s->pid[i] = -1;
        s->stage_status[i] = ST_IDLE;
        s->last_telemetry_log[i] = -1;
    }
    for (i = 0; i < NUM_STAGES - 1; ++i)
        for (j = 0; j < 2; ++j)
            s->lot[i][j] = -1;
    for (j = 0; j < 2; ++j)
        s->tel[j] = s->evt[j] = s->gui[j] = -1;
}

static int create_pipes(Supervisor *s)
{
    int i, flags;
    for (i = 0; i < NUM_STAGES - 1; ++i)
        if (pipe(s->lot[i]) == -1) return -1;
    if (pipe(s->tel) == -1 || pipe(s->evt) == -1 || pipe(s->gui) == -1)
        return -1;
    flags = fcntl(s->gui[PIPE_WRITE], F_GETFL);
    if (flags < 0 || fcntl(s->gui[PIPE_WRITE], F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

/* Close every descriptor except the ones needed by the specific child. */
static void close_child_unneeded(Supervisor *s, StageIO io)
{
    int i, j, fd;
    for (i = 0; i < NUM_STAGES - 1; ++i)
        for (j = 0; j < 2; ++j) {
            fd = s->lot[i][j];
            if (fd != io.in_fd && fd != io.out_fd) close_fd(&s->lot[i][j]);
        }
    for (j = 0; j < 2; ++j) {
        fd = s->tel[j];
        if (fd != io.in_fd && fd != io.telemetry_fd) close_fd(&s->tel[j]);
        fd = s->evt[j];
        if (fd != io.event_fd) close_fd(&s->evt[j]);
        fd = s->gui[j];
        if (fd != io.in_fd) close_fd(&s->gui[j]);
    }
}

static StageIO stage_io(Supervisor *s, int id)
{
    StageIO io = {-1, -1, s->tel[PIPE_WRITE], s->evt[PIPE_WRITE], -1};
    if (id > 0) io.in_fd = s->lot[id - 1][PIPE_READ];
    if (id < NUM_STAGES - 1) io.out_fd = s->lot[id][PIPE_WRITE];
    if (id == STAGE_RECEPTION) io.peer_pid = s->pid[STAGE_CLEANING];
    return io;
}

static void call_stage(int id, const Config *cfg, const StageIO *io)
{
    switch (id) {
    case STAGE_RECEPTION: reception_main(cfg, io); break;
    case STAGE_CLEANING: cleaning_main(cfg, io); break;
    case STAGE_WASHING: washing_main(cfg, io); break;
    case STAGE_CRUSHING: crushing_main(cfg, io); break;
    case STAGE_MALAXATION: malaxation_main(cfg, io); break;
    case STAGE_PRESSING: pressing_main(cfg, io); break;
    }
}

static int fork_stage(Supervisor *s, int id)
{
    pid_t child;
    fflush(stdout); /* Do not duplicate parent stdio buffer after fork. */
    child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        StageIO io = stage_io(s, id);
        close_child_unneeded(s, io);
        call_stage(id, s->cfg, &io);
        _exit(0);
    }
    s->pid[id] = child;
    ++s->active_children;
    logger_log("INFO", "START stage=%s pid=%ld", stage_name(id), (long)child);
    return 0;
}

static int fork_auxiliary(Supervisor *s, int is_gui)
{
    pid_t child;
    StageIO io = {-1, -1, -1, -1, -1};
    if (is_gui) {
        io.in_fd = s->gui[PIPE_READ];
    } else {
        io.in_fd = s->tel[PIPE_READ];
        io.event_fd = s->evt[PIPE_WRITE];
    }
    fflush(stdout);
    child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        close_child_unneeded(s, io);
        if (is_gui) gui_main(s->cfg, &io);
        else quality_control_main(s->cfg, &io);
        _exit(0);
    }
    if (is_gui) s->gui_pid = child;
    else s->qc_pid = child;
    ++s->active_children;
    logger_log("INFO", "START %s pid=%ld", is_gui ? "gui" : "quality_control", (long)child);
    return 0;
}

static void close_parent_child_ends(Supervisor *s)
{
    int i;
    for (i = 0; i < NUM_STAGES - 1; ++i) {
        close_fd(&s->lot[i][0]);
        close_fd(&s->lot[i][1]);
    }
    close_fd(&s->tel[0]); close_fd(&s->tel[1]);
    close_fd(&s->evt[1]);
    close_fd(&s->gui[0]);
    if (!s->cfg->enable_gui) close_fd(&s->gui[1]);
}

/* GUI is optional. Dropping one display update never stops production. */
static void gui_event(Supervisor *s, const EventMsg *event)
{
    if (s->gui[1] == -1 || s->gui_pid <= 0) return;
    if (send_event(s->gui[1], event) < 0 && errno == EPIPE)
        close_fd(&s->gui[1]);
}

static void gui_status(Supervisor *s, int id, int status)
{
    EventMsg event = make_event(EV_STAGE_STATUS, id, status, 0, 0, 0, 0, NULL);
    gui_event(s, &event);
}

static void send_shutdown_to_gui(Supervisor *s, const char *result)
{
    EventMsg event;
    if (s->shutdown_sent) return;
    s->shutdown_sent = 1;
    event = make_event(EV_SHUTDOWN, -1, 0, 0, s->oil, s->water,
                       s->pomace, result);
    gui_event(s, &event);
}

static void dump_status(const Supervisor *s)
{
    int i;
    logger_log("INFO", "STATUS lots=%d oil=%.2f water=%.2f pomace=%.2f criticals=%d",
               s->lots_done, s->oil, s->water, s->pomace, s->critical_total);
    for (i = 0; i < NUM_STAGES; ++i)
        logger_log("INFO", "STATUS stage=%s lots=%d warnings=%d criticals=%d status=%d",
                   stage_name(i), s->stage_lots[i], s->warnings[i],
                   s->criticals[i], s->stage_status[i]);
}

static void signal_child(pid_t pid, int sig)
{
    if (pid > 0 && kill(pid, sig) == -1 && errno != ESRCH)
        logger_log("WARN", "kill(%ld,%d): %s", (long)pid, sig, strerror(errno));
}

static void start_shutdown(Supervisor *s, const char *reason, int exit_code)
{
    int i;
    if (s->shutting_down) return;
    s->shutting_down = 1;
    s->exit_code = exit_code;
    s->shutdown_deadline = clock_s() + s->cfg->drain_timeout_ms / 1000.0;
    logger_log("ERROR", "SHUTDOWN reason=%s", reason);
    send_shutdown_to_gui(s, "aborted");
    /* SIGCONT is necessary because SIGTERM cannot act on stopped processes. */
    for (i = 0; i < NUM_STAGES; ++i)
        signal_child(s->pid[i], SIGCONT);
    signal_child(s->pid[STAGE_RECEPTION], SIGTERM);
    for (i = 1; i < NUM_STAGES; ++i)
        signal_child(s->pid[i], SIGTERM);
    signal_child(s->qc_pid, SIGTERM);
    signal_child(s->gui_pid, SIGTERM);
}

static void maybe_pause_stage(Supervisor *s, int id)
{
    if (id < 0 || id >= NUM_STAGES || s->pid[id] <= 0 || s->stage_reaped[id])
        return;
    if (s->resume_at[id] > 0 || s->shutting_down) return;
    signal_child(s->pid[id], SIGSTOP);
    s->resume_at[id] = clock_s() + s->cfg->critical_pause_ms / 1000.0;
    s->stage_status[id] = ST_PAUSED;
    gui_status(s, id, ST_PAUSED);
    logger_log("WARN", "PAUSE stage=%s duration_ms=%d", stage_name(id),
               s->cfg->critical_pause_ms);
}

static void handle_event(Supervisor *s, const EventMsg *e)
{
    int st = e->stage;
    if (st >= 0 && st < NUM_STAGES && e->type == EV_TELEMETRY) {
        s->stage_lots[st]++;
        if (s->last_telemetry_log[st] < 0 ||
            clock_s() - s->last_telemetry_log[st] >= 1.0) {
            logger_log("INFO", "TELEMETRY stage=%s lot=%u temp=%.2f pressure=%.2f time_ms=%.1f",
                       stage_name(st), e->lot_id, e->v1, e->v2, e->v3);
            s->last_telemetry_log[st] = clock_s();
        }
    } else if (e->type == EV_STAGE_STATUS && st >= 0 && st < NUM_STAGES) {
        s->stage_status[st] = e->status;
        logger_log("INFO", "STAGE stage=%s status=%d", stage_name(st), e->status);
    } else if (e->type == EV_STAGE_DONE && st >= 0 && st < NUM_STAGES) {
        s->stage_done[st] = 1;
        s->stage_status[st] = ST_DONE;
        logger_log("INFO", "DONE stage=%s", stage_name(st));
        if (st == STAGE_PRESSING && !s->shutting_down) {
            s->finished = 1;
            send_shutdown_to_gui(s, "complete");
            if (s->cfg->enable_gui && s->cfg->gui_auto_close_ms > 0)
                s->gui_deadline = clock_s() + s->cfg->gui_auto_close_ms / 1000.0;
            logger_log("INFO", "PIPELINE COMPLETE");
        }
    } else if (e->type == EV_LOT_FINISHED) {
        s->lots_done++;
        s->oil += e->v1;
        s->water += e->v2;
        s->pomace += e->v3;
        logger_log("INFO", "LOT_FINISHED id=%u oil_kg=%.2f water_kg=%.2f pomace_kg=%.2f",
                   e->lot_id, e->v1, e->v2, e->v3);
    } else if (e->type == EV_ALERT_WARNING || e->type == EV_ALERT_CRITICAL) {
        if (e->type == EV_ALERT_WARNING) {
            s->warning_total++;
            if (st >= 0 && st < NUM_STAGES) s->warnings[st]++;
        } else {
            s->critical_total++;
            if (st >= 0 && st < NUM_STAGES) s->criticals[st]++;
        }
        logger_log(e->type == EV_ALERT_WARNING ? "WARN" : "ERROR",
                   "ALERT stage=%s level=%s lot=%u parameter=%.47s value=%.2f limits=[%.2f,%.2f]",
                   stage_name(st), e->type == EV_ALERT_WARNING ? "warning" : "critical",
                   e->lot_id, e->text, e->v1, e->v2, e->v3);
        if (e->type == EV_ALERT_CRITICAL && !s->shutting_down) {
            if (s->critical_total > s->cfg->max_critical_events)
                start_shutdown(s, "too many critical alerts", 1);
            else
                maybe_pause_stage(s, st);
        }
    } else if (e->type == EV_CONTAINER) {
        /* A refill is the first reported container amount of a new batch.
           The sender must emit its full batch mass before moving any chunk. */
        if (s->last_container_kg < 0 || e->v1 > s->last_container_kg + 1e-9) {
            ++s->batch_count;
            logger_log("INFO", "BATCH %d mass_kg=%.2f", s->batch_count, e->v1);
        }
        s->last_container_kg = e->v1;
        logger_log("INFO", "CONTAINER lot=%u left_kg=%.2f capacity_kg=%.2f",
                   e->lot_id, e->v1, e->v2);
    } else if (e->type == EV_BELT) {
        logger_log("INFO", "BELT %s", e->v1 > 0.5 ? "RUNNING" : "STOPPED");
    } else if (e->type == EV_INFO) {
        logger_log("INFO", "INFO %.47s", e->text);
    } else {
        logger_log("INFO", "EVENT type=%d stage=%s lot=%u", e->type,
                   stage_name(st), e->lot_id);
    }
    gui_event(s, e);
}

static void toggle_pause(Supervisor *s)
{
    int i;
    s->paused_all = !s->paused_all;
    for (i = 0; i < NUM_STAGES; ++i) {
        if (s->stage_reaped[i] || s->pid[i] <= 0) continue;
        if (s->paused_all) {
            signal_child(s->pid[i], SIGSTOP);
            s->stage_status[i] = ST_PAUSED;
            gui_status(s, i, ST_PAUSED);
        } else if (s->resume_at[i] <= 0) {
            signal_child(s->pid[i], SIGCONT);
            s->stage_status[i] = ST_IDLE;
            gui_status(s, i, ST_IDLE);
        }
    }
    logger_log("INFO", "OPERATOR %s", s->paused_all ? "PAUSE" : "RESUME");
}

static void check_deadlines(Supervisor *s)
{
    int i;
    double now = clock_s();
    if (s->shutting_down) {
        if (now >= s->shutdown_deadline) {
            for (i = 0; i < NUM_STAGES; ++i)
                if (!s->stage_reaped[i]) signal_child(s->pid[i], SIGKILL);
            if (!s->qc_reaped) signal_child(s->qc_pid, SIGKILL);
            if (!s->gui_reaped) signal_child(s->gui_pid, SIGKILL);
            s->shutdown_deadline = now + 1.0; /* retry if necessary */
        }
        return;
    }
    for (i = 0; i < NUM_STAGES; ++i) {
        if (s->resume_at[i] > 0 && now >= s->resume_at[i]) {
            s->resume_at[i] = 0;
            if (!s->paused_all && !s->stage_reaped[i]) {
                signal_child(s->pid[i], SIGCONT);
                s->stage_status[i] = ST_IDLE;
                gui_status(s, i, ST_IDLE);
            }
            logger_log("INFO", "RESUME_DEADLINE stage=%s", stage_name(i));
        }
    }
    if (s->finished && s->gui_deadline > 0 && now >= s->gui_deadline) {
        signal_child(s->gui_pid, SIGTERM);
        s->gui_deadline = 0;
    }
}

static int next_timeout(Supervisor *s)
{
    int i, timeout = POLL_STEP_MS;
    double now = clock_s(), deadline = 0;
    if (s->shutting_down) deadline = s->shutdown_deadline;
    else {
        if (s->gui_deadline > 0) deadline = s->gui_deadline;
        for (i = 0; i < NUM_STAGES; ++i)
            if (s->resume_at[i] > 0 && (deadline == 0 || s->resume_at[i] < deadline))
                deadline = s->resume_at[i];
    }
    if (deadline > 0) {
        int ms = deadline <= now ? 0 : (int)((deadline - now) * 1000);
        if (ms < timeout) timeout = ms;
    }
    return timeout;
}

static void reap_children(Supervisor *s)
{
    int status, i;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        int normal = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        --s->active_children;
        for (i = 0; i < NUM_STAGES; ++i)
            if (s->pid[i] == p) {
                s->stage_reaped[i] = 1;
                s->pid[i] = -1;
                logger_log(normal ? "INFO" : "ERROR", "EXIT stage=%s pid=%ld normal=%d",
                           stage_name(i), (long)p, normal);
                if (!normal && !s->shutting_down)
                    start_shutdown(s, "stage died unexpectedly", 1);
                break;
            }
        if (p == s->qc_pid) {
            s->qc_reaped = 1;
            s->qc_pid = -1;
            logger_log(normal ? "INFO" : "ERROR", "EXIT quality_control normal=%d", normal);
            if (!normal && !s->shutting_down)
                start_shutdown(s, "quality control died", 1);
        } else if (p == s->gui_pid) {
            s->gui_reaped = 1;
            s->gui_pid = -1;
            close_fd(&s->gui[1]);
            logger_log(normal ? "INFO" : "WARN", "EXIT gui normal=%d", normal);
        }
    }
    if (p == -1 && errno != ECHILD && errno != EINTR)
        logger_log("WARN", "waitpid error: %s", strerror(errno));
}

static void read_signals(Supervisor *s)
{
    struct signalfd_siginfo si;
    ssize_t n;
    while ((n = read(s->signal_fd, &si, sizeof si)) == (ssize_t)sizeof si) {
        switch (si.ssi_signo) {
        case SIGINT:
            start_shutdown(s, "Ctrl-C", 130);
            break;
        case SIGTERM:
            start_shutdown(s, "SIGTERM", 1);
            break;
        case SIGUSR1:
            if (!s->shutting_down) toggle_pause(s);
            break;
        case SIGUSR2:
            dump_status(s);
            break;
        case SIGCHLD:
            /* Reap after we drain the event pipe below. */
            break;
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR)
        logger_log("WARN", "signalfd read error: %s", strerror(errno));
}

static void emergency_cleanup(Supervisor *s)
{
    int i, status;
    pid_t p;
    double deadline;
    /* For errors before the event loop, terminate and reap with a deadline. */
    for (i = 0; i < NUM_STAGES; ++i) signal_child(s->pid[i], SIGCONT);
    for (i = 0; i < NUM_STAGES; ++i) signal_child(s->pid[i], SIGTERM);
    signal_child(s->qc_pid, SIGTERM);
    signal_child(s->gui_pid, SIGTERM);
    close_all_pipes(s);
    deadline = clock_s() + s->cfg->drain_timeout_ms / 1000.0;
    while (s->active_children > 0 && clock_s() < deadline) {
        while ((p = waitpid(-1, &status, WNOHANG)) > 0)
            --s->active_children;
        if (p < 0 && errno == ECHILD) { s->active_children = 0; break; }
        if (s->active_children > 0) {
            struct timespec delay = {0, 10000000L};
            nanosleep(&delay, NULL);
        }
    }
    if (s->active_children > 0) {
        for (i = 0; i < NUM_STAGES; ++i) signal_child(s->pid[i], SIGKILL);
        signal_child(s->qc_pid, SIGKILL);
        signal_child(s->gui_pid, SIGKILL);
        while (waitpid(-1, &status, 0) > 0) { /* reap everyone */ }
    }
}

static void print_summary(const Supervisor *s)
{
    /* This must be the LAST logger line; Partner 3's scripts parse it. */
    logger_log("INFO", "SUMMARY lots=%d oil_kg=%.2f water_kg=%.2f pomace_kg=%.2f "
               "warnings=%d criticals=%d result=%s", s->lots_done, s->oil,
               s->water, s->pomace, s->warning_total, s->critical_total,
               (!s->shutting_down && s->finished && s->exit_code == 0)
                 ? "complete" : "aborted");
}

int supervisor_run(const Config *cfg, const char *cfg_path)
{
    Supervisor s;
    sigset_t mask, old_mask;
    struct sigaction old_pipe, ignore_pipe;
    int i, ret = 1, event_eof = 0, poll_rc, saved_errno;
    struct pollfd fds[2];
    init_state(&s, cfg);

    if (logger_open(cfg->log_file) != 0) {
        fprintf(stderr, "Cannot open log file: %s\n", cfg->log_file);
        return 1;
    }
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) == -1) {
        logger_log("ERROR", "sigprocmask failed: %s", strerror(errno));
        goto finish_without_children;
    }
    memset(&ignore_pipe, 0, sizeof ignore_pipe);
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe) == -1) {
        logger_log("ERROR", "sigaction SIGPIPE failed: %s", strerror(errno));
        goto finish_restore_mask;
    }
    logger_log("INFO", "Supervisor started config=%s batches=%d gui=%d",
               cfg_path, cfg->num_batches, cfg->enable_gui);
    if (create_pipes(&s) == -1) {
        logger_log("ERROR", "pipe/fcntl failed: %s", strerror(errno));
        goto finish_emergency;
    }
    /* Downstream first: cleaning PID exists before reception is forked. */
    for (i = STAGE_PRESSING; i >= STAGE_RECEPTION; --i)
        if (fork_stage(&s, i) == -1) {
            logger_log("ERROR", "fork stage failed: %s", strerror(errno));
            goto finish_emergency;
        }
    if (fork_auxiliary(&s, 0) == -1 ||
        (cfg->enable_gui && fork_auxiliary(&s, 1) == -1)) {
        logger_log("ERROR", "fork auxiliary failed: %s", strerror(errno));
        goto finish_emergency;
    }
    close_parent_child_ends(&s);
    s.signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (s.signal_fd == -1) {
        logger_log("ERROR", "signalfd failed: %s", strerror(errno));
        goto finish_emergency;
    }
    logger_log("INFO", "All child processes created (%d)", s.active_children);

    while (s.active_children > 0 || (!event_eof && s.evt[0] >= 0)) {
        EventMsg event;
        fds[0].fd = s.signal_fd;
        fds[0].events = POLLIN;
        fds[1].fd = s.evt[0];
        fds[1].events = POLLIN | POLLHUP;
        poll_rc = poll(fds, 2, next_timeout(&s));
        if (poll_rc < 0 && errno != EINTR) {
            logger_log("ERROR", "poll failed: %s", strerror(errno));
            start_shutdown(&s, "poll failure", 1);
        }
        if (poll_rc > 0 && s.evt[0] >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
            int r = read_msg(s.evt[0], &event, sizeof event);
            if (r == 1) handle_event(&s, &event);
            else if (r == 0) { event_eof = 1; close_fd(&s.evt[0]); }
            else if (r == -1) start_shutdown(&s, "event pipe read failed", 1);
        }
        if (poll_rc > 0 && (fds[0].revents & POLLIN)) read_signals(&s);
        reap_children(&s);
        check_deadlines(&s);
        if (event_eof && !s.finished && !s.shutting_down)
            start_shutdown(&s, "event stream ended before pressing completed", 1);
        if (event_eof && !s.shutting_down)
            for (i = 0; i < NUM_STAGES; ++i)
                if (!s.stage_done[i]) {
                    start_shutdown(&s, "stage did not report DONE", 1);
                    break;
                }
        /* In normal mode the GUI can remain open until auto-close or user close. */
    }
    if (!s.finished && !s.shutting_down)
        start_shutdown(&s, "children exited before completion", 1);
    if (s.shutting_down) ret = s.exit_code;
    else ret = 0;
    close_fd(&s.signal_fd);
    close_all_pipes(&s);
    print_summary(&s);
    goto finish_restore_pipe;

finish_emergency:
    saved_errno = errno;
    emergency_cleanup(&s);
    logger_log("ERROR", "Startup failed (errno=%d)", saved_errno);
    print_summary(&s);
    goto finish_restore_pipe;
finish_restore_pipe:
    sigaction(SIGPIPE, &old_pipe, NULL);
finish_restore_mask:
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
finish_without_children:
    logger_close();
    return ret;
}
