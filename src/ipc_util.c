/* src/ipc_util.c -- FROZEN shared utilities. Do NOT edit locally. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_terminate = 0;
double g_excluded_ms = 0.0;

double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int write_full(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int read_msg(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) {
                if (got == 0)
                    return -2;
                continue;
            }
            return -1;
        }
        if (r == 0)
            return got == 0 ? 0 : -1;
        got += (size_t)r;
    }
    return 1;
}

void sleep_ms_scaled(double sim_ms, double time_scale, volatile sig_atomic_t *stop)
{
    double real_ms;
    struct timespec ts, rem;
    if (time_scale <= 0.0)
        time_scale = 1.0;
    real_ms = sim_ms / time_scale;
    if (real_ms <= 0.0)
        return;
    ts.tv_sec = (time_t)(real_ms / 1000.0);
    ts.tv_nsec = (long)((real_ms - (double)ts.tv_sec * 1000.0) * 1e6);
    while (nanosleep(&ts, &rem) == -1 && errno == EINTR) {
        if (stop && *stop)
            return;
        ts = rem;
    }
}

double rand_unit(unsigned *seed)
{
    return (double)rand_r(seed) / (double)RAND_MAX;
}

double rand_range(unsigned *seed, Range r)
{
    return r.min + (r.max - r.min) * rand_unit(seed);
}

/* splitmix64: pure function, safe inside OpenMP loops */
double hash_unit(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return (double)(x >> 11) * (1.0 / 9007199254740992.0);
}

int install_handler(int sig, void (*h)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags; /* 0 => no SA_RESTART: blocking read() returns EINTR */
    return sigaction(sig, &sa, NULL);
}

EventMsg make_event(int type, int stage, int status, uint32_t lot_id,
                    double v1, double v2, double v3, const char *text)
{
    EventMsg e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.stage = stage;
    e.status = status;
    e.lot_id = lot_id;
    e.v1 = v1;
    e.v2 = v2;
    e.v3 = v3;
    e.timestamp = now_s();
    if (text)
        strncpy(e.text, text, sizeof e.text - 1);
    return e;
}

int send_event(int fd, const EventMsg *ev)
{
    if (fd < 0)
        return -1;
    return write_full(fd, ev, sizeof *ev);
}
