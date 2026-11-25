/* src/config.c -- Partner 2: parser for the user-defined values / ranges file.
 *
 * Syntax (see config/default.cfg):
 *     key = value          scalar
 *     key = min max        range (a single number means min = max)
 *     key.stage = ...      per-stage key (reception, cleaning, washing, crushing,
 *                          malaxation, pressing)
 *     '#' starts a comment, blank lines are ignored, keys are case-sensitive.
 *
 * config_load() starts from config_set_defaults(), so a file only needs to contain
 * the values it wants to change.  Errors are reported as "file:line: message" on
 * stderr and make the function return -1; an unknown key is only a warning.
 *
 * The parser is table driven: every key is one Entry {name, type, pointer into the
 * Config, allowed interval}.  Range checking therefore happens on the line that
 * contains the bad value, which gives precise error messages.                     */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_BUF    512 /* longest accepted line is LINE_BUF - 1 characters */
#define KEY_LEN     48
#define MAX_ENTRIES 80
#define NO_BOUND    HUGE_VAL
#define PCT_MAX     100.0
#define MIN_WINDOW  200.0

typedef enum { T_INT, T_UINT, T_DBL, T_RANGE, T_STR } ValueType;

typedef struct {
    char name[KEY_LEN];
    ValueType type;
    void *ptr;     /* field inside the Config */
    double lo, hi; /* allowed interval (for ranges: for both ends)       */
    int lo_excl;   /* 1: value must be > lo, 0: value must be >= lo      */
} Entry;

/* ------------------------------------------------------------------ messages */

static void msg_at(const char *path, int line, const char *fmt, ...)
{
    va_list ap;
    if (line > 0)
        fprintf(stderr, "%s:%d: ", path, line);
    else
        fprintf(stderr, "%s: ", path);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ the table */

static void add(Entry *t, int *n, const char *name, ValueType type, void *ptr,
                double lo, int lo_excl, double hi)
{
    Entry *e;
    if (*n >= MAX_ENTRIES)
        return; /* cannot happen: MAX_ENTRIES is larger than the number of keys */
    e = &t[(*n)++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->type = type;
    e->ptr = ptr;
    e->lo = lo;
    e->hi = hi;
    e->lo_excl = lo_excl;
}

static void add_per_stage(Entry *t, int *n, const char *base, ValueType type,
                          void *first, size_t elem_size, double lo, int lo_excl, double hi)
{
    char name[KEY_LEN];
    int i;
    for (i = 0; i < NUM_STAGES; i++) {
        snprintf(name, sizeof name, "%s.%s", base, stage_name(i));
        add(t, n, name, type, (char *)first + (size_t)i * elem_size, lo, lo_excl, hi);
    }
}

static int build_table(Config *c, Entry *t)
{
    int n = 0;
    /* general */
    add(t, &n, "seed", T_UINT, &c->seed, 0, 0, 4294967295.0);
    add(t, &n, "time_scale", T_DBL, &c->time_scale, 0, 1, NO_BOUND);
    /* reception / conveyor */
    add(t, &n, "num_batches", T_INT, &c->num_batches, 1, 0, INT_MAX);
    add(t, &n, "batch_mass_kg", T_RANGE, &c->batch_mass_kg, 0, 1, NO_BOUND);
    add(t, &n, "batch_interval_ms", T_RANGE, &c->batch_interval_ms, 0, 0, NO_BOUND);
    add(t, &n, "container_capacity_kg", T_DBL, &c->container_capacity_kg, 0, 1, NO_BOUND);
    add(t, &n, "conveyor_chunk_kg", T_DBL, &c->conveyor_chunk_kg, 0, 1, NO_BOUND);
    add(t, &n, "conveyor_tick_ms", T_INT, &c->conveyor_tick_ms, 1, 0, INT_MAX);
    add(t, &n, "initial_debris_pct", T_RANGE, &c->initial_debris_pct, 0, 0, PCT_MAX);
    add(t, &n, "initial_impurity_pct", T_RANGE, &c->initial_impurity_pct, 0, 0, PCT_MAX);
    /* process physics */
    add(t, &n, "cleaning_removal_pct", T_RANGE, &c->cleaning_removal_pct, 0, 0, PCT_MAX);
    add(t, &n, "washing_removal_pct", T_RANGE, &c->washing_removal_pct, 0, 0, PCT_MAX);
    add(t, &n, "paste_particle_mm", T_RANGE, &c->paste_particle_mm, 0, 0, NO_BOUND);
    add(t, &n, "droplet_final_um", T_RANGE, &c->droplet_final_um, 0, 0, NO_BOUND);
    add(t, &n, "oil_yield_pct", T_RANGE, &c->oil_yield_pct, 0, 0, PCT_MAX);
    add(t, &n, "water_pct", T_RANGE, &c->water_pct, 0, 0, PCT_MAX);
    add(t, &n, "press_method", T_INT, &c->press_method, 0, 0, 1);
    /* per stage */
    add_per_stage(t, &n, "proc_time_ms", T_RANGE, c->proc_time_ms, sizeof(Range), 0, 0, NO_BOUND);
    add_per_stage(t, &n, "temp_nominal_c", T_RANGE, c->temp_nominal_c, sizeof(Range), -NO_BOUND, 0, NO_BOUND);
    add_per_stage(t, &n, "temp_limit_c", T_RANGE, c->temp_limit_c, sizeof(Range), -NO_BOUND, 0, NO_BOUND);
    add_per_stage(t, &n, "pressure_nominal_bar", T_RANGE, c->pressure_nominal_bar, sizeof(Range), -NO_BOUND, 0, NO_BOUND);
    add_per_stage(t, &n, "pressure_limit_bar", T_RANGE, c->pressure_limit_bar, sizeof(Range), -NO_BOUND, 0, NO_BOUND);
    add_per_stage(t, &n, "max_proc_time_ms", T_DBL, c->max_proc_time_ms, sizeof(double), 0, 0, NO_BOUND);
    /* sensors / quality control */
    add(t, &n, "fault_prob_pct", T_DBL, &c->fault_prob_pct, 0, 0, PCT_MAX);
    add(t, &n, "warn_margin_pct", T_DBL, &c->warn_margin_pct, 0, 0, PCT_MAX);
    add(t, &n, "qc_report_interval_ms", T_INT, &c->qc_report_interval_ms, 0, 0, INT_MAX);
    /* OpenMP */
    add(t, &n, "use_openmp", T_INT, &c->use_openmp, 0, 0, 1);
    add(t, &n, "omp_threads", T_INT, &c->omp_threads, 1, 0, INT_MAX);
    add(t, &n, "sim_elements_per_lot", T_INT, &c->sim_elements_per_lot, 1, 0, INT_MAX);
    /* supervisor */
    add(t, &n, "critical_pause_ms", T_INT, &c->critical_pause_ms, 0, 0, INT_MAX);
    add(t, &n, "max_critical_events", T_INT, &c->max_critical_events, 0, 0, INT_MAX);
    add(t, &n, "drain_timeout_ms", T_INT, &c->drain_timeout_ms, 0, 0, INT_MAX);
    add(t, &n, "log_file", T_STR, c->log_file, 0, 0, 0);
    /* GUI */
    add(t, &n, "enable_gui", T_INT, &c->enable_gui, 0, 0, 1);
    add(t, &n, "gui_auto_close_ms", T_INT, &c->gui_auto_close_ms, 0, 0, INT_MAX);
    add(t, &n, "window_w", T_INT, &c->window_w, MIN_WINDOW, 0, INT_MAX);
    add(t, &n, "window_h", T_INT, &c->window_h, MIN_WINDOW, 0, INT_MAX);
    return n;
}

/* ------------------------------------------------------------------ value parsing */

static char *trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static char *skip_ws(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Parses one finite number at the start of s.  Returns the position after it, or NULL. */
static char *parse_number(char *s, double *out)
{
    char *end;
    errno = 0;
    *out = strtod(s, &end);
    if (end == s || errno == ERANGE || !isfinite(*out))
        return NULL;
    return end;
}

static int in_bounds(const Entry *e, double v)
{
    if (e->lo_excl ? !(v > e->lo) : (v < e->lo))
        return 0;
    return v <= e->hi;
}

static void describe_bounds(const Entry *e, char *buf, size_t n)
{
    char lo[32], hi[32];
    if (e->lo == -NO_BOUND)
        snprintf(lo, sizeof lo, "-inf");
    else
        snprintf(lo, sizeof lo, "%.10g", e->lo);
    if (e->hi == NO_BOUND)
        snprintf(hi, sizeof hi, "inf");
    else
        snprintf(hi, sizeof hi, "%.10g", e->hi);
    snprintf(buf, n, "%c%s, %s]", e->lo_excl ? '(' : '[', lo, hi);
}

static int check_bounds(const char *path, int line, const Entry *e, double v)
{
    char allowed[80];
    if (in_bounds(e, v))
        return 0;
    describe_bounds(e, allowed, sizeof allowed);
    msg_at(path, line, "value %g for key '%s' is out of the allowed interval %s", v, e->name, allowed);
    return -1;
}

/* Stores the value text of one line into the field of entry e. 0 ok, -1 error. */
static int apply_value(const char *path, int line, const Entry *e, char *val)
{
    char *end;

    switch (e->type) {
    case T_STR:
        if (strlen(val) >= PATH_LEN) {
            msg_at(path, line, "value of '%s' is too long (maximum %d characters)", e->name, PATH_LEN - 1);
            return -1;
        }
        strcpy((char *)e->ptr, val);
        return 0;

    case T_INT: {
        long v;
        errno = 0;
        v = strtol(val, &end, 10);
        if (end == val || *skip_ws(end) != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
            msg_at(path, line, "bad number '%s' for key '%s' (an integer is expected)", val, e->name);
            return -1;
        }
        if (check_bounds(path, line, e, (double)v) < 0)
            return -1;
        *(int *)e->ptr = (int)v;
        return 0;
    }

    case T_UINT: {
        unsigned long v;
        errno = 0;
        v = strtoul(val, &end, 10);
        if (end == val || *skip_ws(end) != '\0' || errno == ERANGE || strchr(val, '-') != NULL ||
            v > 4294967295UL) {
            msg_at(path, line, "bad number '%s' for key '%s' (an unsigned integer is expected)", val, e->name);
            return -1;
        }
        *(unsigned *)e->ptr = (unsigned)v;
        return 0;
    }

    case T_DBL: {
        double v;
        end = parse_number(val, &v);
        if (!end || *skip_ws(end) != '\0') {
            msg_at(path, line, "bad number '%s' for key '%s' (one number is expected)", val, e->name);
            return -1;
        }
        if (check_bounds(path, line, e, v) < 0)
            return -1;
        *(double *)e->ptr = v;
        return 0;
    }

    case T_RANGE: {
        double a, b;
        char *rest;
        end = parse_number(val, &a);
        if (!end) {
            msg_at(path, line, "bad number '%s' for key '%s' (expected 'min max' or one number)", val, e->name);
            return -1;
        }
        rest = skip_ws(end);
        if (*rest == '\0') {
            b = a; /* single number => min = max */
        } else {
            end = parse_number(rest, &b);
            if (!end) {
                msg_at(path, line, "bad number '%s' for key '%s' (expected 'min max' or one number)", rest, e->name);
                return -1;
            }
            if (*skip_ws(end) != '\0') {
                msg_at(path, line, "too many values for key '%s' (expected 'min max' or one number)", e->name);
                return -1;
            }
        }
        if (a > b) {
            msg_at(path, line, "range for key '%s' has min (%g) greater than max (%g)", e->name, a, b);
            return -1;
        }
        if (check_bounds(path, line, e, a) < 0 || check_bounds(path, line, e, b) < 0)
            return -1;
        ((Range *)e->ptr)->min = a;
        ((Range *)e->ptr)->max = b;
        return 0;
    }
    }
    return -1;
}

/* ------------------------------------------------------------------ cross-field checks (warnings only) */

static void warn_nominal_vs_limit(const char *path, const char *what, int stage, Range nom, Range lim)
{
    if (nom.min < lim.min || nom.max > lim.max)
        msg_at(path, 0, "warning: %s.%s nominal range [%g, %g] is not inside its limit range [%g, %g]",
               what, stage_name(stage), nom.min, nom.max, lim.min, lim.max);
}

static void cross_check(const char *path, const Config *c)
{
    int i;
    for (i = 0; i < NUM_STAGES; i++) {
        warn_nominal_vs_limit(path, "temp", i, c->temp_nominal_c[i], c->temp_limit_c[i]);
        warn_nominal_vs_limit(path, "pressure", i, c->pressure_nominal_bar[i], c->pressure_limit_bar[i]);
    }
    if (c->oil_yield_pct.max + c->water_pct.max > PCT_MAX)
        msg_at(path, 0, "warning: oil_yield_pct.max + water_pct.max exceeds 100, pomace could become negative");
}

/* ------------------------------------------------------------------ public function */

int config_load(const char *path, Config *cfg)
{
    Entry tab[MAX_ENTRIES];
    char line[LINE_BUF];
    FILE *fp;
    int n, lineno = 0, rc = 0;

    if (!path || !cfg)
        return -1;
    config_set_defaults(cfg);
    fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return -1;
    }
    n = build_table(cfg, tab);

    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        char *hash, *s, *eq, *key, *val;
        int i, found = -1;

        lineno++;
        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            msg_at(path, lineno, "line too long (maximum %d characters)", LINE_BUF - 1);
            rc = -1;
            break;
        }
        hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        s = trim(line);
        if (*s == '\0')
            continue;

        eq = strchr(s, '=');
        if (!eq) {
            msg_at(path, lineno, "malformed line '%s' (expected 'key = value')", s);
            rc = -1;
            break;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        if (*key == '\0') {
            msg_at(path, lineno, "missing key before '='");
            rc = -1;
            break;
        }

        for (i = 0; i < n; i++)
            if (strcmp(tab[i].name, key) == 0) {
                found = i;
                break;
            }
        if (found < 0) {
            msg_at(path, lineno, "unknown key '%s' ignored", key);
            continue;
        }
        if (*val == '\0') {
            msg_at(path, lineno, "missing value for key '%s'", key);
            rc = -1;
            break;
        }
        if (apply_value(path, lineno, &tab[found], val) < 0) {
            rc = -1;
            break;
        }
    }
    if (rc == 0 && ferror(fp)) {
        perror(path);
        rc = -1;
    }
    fclose(fp);
    if (rc == 0)
        cross_check(path, cfg);
    return rc;
}
