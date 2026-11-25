/* include/common.h -- FROZEN shared header (ENCS4330 Project #1, Olive Press)
 * Every partner uses this file byte-for-byte. Do NOT edit it locally.
 * Change requests go to Partner 1, who re-publishes it to everybody. */
#ifndef OLIVE_COMMON_H
#define OLIVE_COMMON_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define NUM_STAGES 6
#define PATH_LEN   256

typedef enum {
    STAGE_RECEPTION  = 0,
    STAGE_CLEANING   = 1,
    STAGE_WASHING    = 2,
    STAGE_CRUSHING   = 3,
    STAGE_MALAXATION = 4,
    STAGE_PRESSING   = 5
} StageId;

static inline const char *stage_name(int id)
{
    static const char *const names[NUM_STAGES] = {
        "reception", "cleaning", "washing", "crushing", "malaxation", "pressing"};
    return (id >= 0 && id < NUM_STAGES) ? names[id] : "unknown";
}

typedef enum {
    ST_IDLE = 0,    /* waiting for work                              */
    ST_RUNNING = 1, /* processing a lot                              */
    ST_STOPPED = 2, /* belt stopped (cleaning) / container empty     */
    ST_PAUSED = 3,  /* paused by supervisor (SIGSTOP)                */
    ST_ERROR = 4,
    ST_DONE = 5
} StageStatus;

typedef enum {
    EV_STAGE_STATUS = 1, /* status = StageStatus                                   */
    EV_TELEMETRY,        /* v1=temp C, v2=pressure bar, v3=simulated processing ms */
    EV_ALERT_WARNING,    /* v1=value, v2=limit min, v3=limit max, text=parameter   */
    EV_ALERT_CRITICAL,   /* same payload as EV_ALERT_WARNING                       */
    EV_LOT_FINISHED,     /* v1=oil kg, v2=veg. water kg, v3=pomace kg              */
    EV_BELT,             /* v1 = 1 belt running, 0 belt stopped                    */
    EV_CONTAINER,        /* v1 = kg left in container, v2 = capacity kg            */
    EV_STAGE_DONE,       /* stage finished (EOF on its input or SIGTERM)           */
    EV_INFO,             /* free text in .text                                     */
    EV_SHUTDOWN          /* supervisor -> GUI: pipeline ended, v1=total oil kg,
                            v2=total water kg, v3=total pomace kg                  */
} EventType;

typedef struct { double min, max; } Range;

/* One lot (= one conveyor chunk of olives / paste) travelling through the
 * pipeline. sizeof(Lot) < PIPE_BUF, so one write() is atomic. */
typedef struct {
    uint32_t lot_id;
    uint32_t batch_id;
    double mass_kg;           /* current mass of olives / paste                 */
    double debris_pct;        /* leaves, twigs, dust (% of mass)                */
    double impurity_pct;      /* soil and residues (% of mass)                  */
    double paste_particle_mm; /* set by crushing                                */
    double oil_droplet_um;    /* set by malaxation (grows while mixing)         */
    double oil_kg;            /* set by pressing                                */
    double water_kg;          /* set by pressing (vegetable water)              */
    double pomace_kg;         /* set by pressing (solids)                       */
    double t_created;         /* now_s() when reception created the lot         */
} Lot;

/* One message type used on the telemetry pipe, the event pipe and the GUI pipe. */
typedef struct {
    int32_t type;    /* EventType  */
    int32_t stage;   /* StageId    */
    int32_t status;  /* StageStatus (EV_STAGE_STATUS only) */
    uint32_t lot_id;
    double v1, v2, v3;
    double timestamp; /* now_s() */
    char text[48];
} EventMsg;

/* File descriptors and peer info handed to every process entry point.
 * Unused descriptors are -1. */
typedef struct {
    int in_fd;        /* reads Lot (stages) / EventMsg (QC, GUI)       */
    int out_fd;       /* writes Lot to next stage; -1 for pressing/QC/GUI */
    int telemetry_fd; /* write end of telemetry pipe (stages)           */
    int event_fd;     /* write end of event pipe (stages and QC)        */
    pid_t peer_pid;   /* reception: pid of the cleaning process         */
} StageIO;

typedef struct {
    /* general */
    unsigned seed;
    double time_scale;               /* >1 runs the simulation faster than real time */
    /* reception / conveyor */
    int num_batches;
    Range batch_mass_kg;
    Range batch_interval_ms;         /* pause between batches (simulated ms)  */
    double container_capacity_kg;
    double conveyor_chunk_kg;
    int conveyor_tick_ms;            /* simulated ms between two chunks        */
    Range initial_debris_pct;
    Range initial_impurity_pct;
    /* process physics */
    Range cleaning_removal_pct;
    Range washing_removal_pct;
    Range paste_particle_mm;
    Range droplet_final_um;
    Range oil_yield_pct;
    Range water_pct;
    int press_method;                /* 0 = hydraulic press, 1 = decanter centrifuge */
    /* per stage (indexed by StageId) */
    Range proc_time_ms[NUM_STAGES];
    Range temp_nominal_c[NUM_STAGES];
    Range temp_limit_c[NUM_STAGES];
    Range pressure_nominal_bar[NUM_STAGES];
    Range pressure_limit_bar[NUM_STAGES];
    double max_proc_time_ms[NUM_STAGES];
    /* sensors / quality control */
    double fault_prob_pct;           /* chance per lot of an abnormal sensor reading */
    double warn_margin_pct;          /* warning band = this % of the limit span      */
    int qc_report_interval_ms;
    /* OpenMP */
    int use_openmp;                  /* 0/1                                          */
    int omp_threads;
    int sim_elements_per_lot;        /* loop size of every simulation kernel         */
    /* supervisor */
    int critical_pause_ms;
    int max_critical_events;
    int drain_timeout_ms;
    char log_file[PATH_LEN];
    /* GUI */
    int enable_gui;
    int gui_auto_close_ms;           /* 0 = wait for the user to close the window    */
    int window_w, window_h;
} Config;

static inline Range make_range(double lo, double hi)
{
    Range r;
    r.min = lo;
    r.max = hi;
    return r;
}

/* Built-in defaults; they MUST equal config/default.cfg. */
static inline void config_set_defaults(Config *c)
{
    static const double pt_lo[NUM_STAGES] = {0, 400, 500, 400, 800, 600};
    static const double pt_hi[NUM_STAGES] = {0, 800, 900, 700, 1200, 1000};
    static const double tn_lo[NUM_STAGES] = {18, 18, 18, 20, 24, 22};
    static const double tn_hi[NUM_STAGES] = {28, 30, 30, 30, 28, 30};
    static const double tl_lo[NUM_STAGES] = {5, 5, 5, 10, 18, 15};
    static const double tl_hi[NUM_STAGES] = {35, 38, 38, 35, 30, 35};
    static const double pn_lo[NUM_STAGES] = {1.0, 1.0, 1.0, 1.0, 1.0, 40};
    static const double pn_hi[NUM_STAGES] = {1.1, 1.2, 1.5, 1.3, 1.2, 60};
    static const double pl_lo[NUM_STAGES] = {0.8, 0.8, 0.8, 0.8, 0.8, 30};
    static const double pl_hi[NUM_STAGES] = {1.5, 1.6, 2.5, 2.0, 1.6, 70};
    static const double mp[NUM_STAGES] = {2000, 2000, 2000, 2000, 3000, 2000};
    int i;

    memset(c, 0, sizeof *c);
    c->seed = 42;
    c->time_scale = 1.0;
    c->num_batches = 3;
    c->batch_mass_kg = make_range(200, 500);
    c->batch_interval_ms = make_range(1500, 3000);
    c->container_capacity_kg = 500;
    c->conveyor_chunk_kg = 50;
    c->conveyor_tick_ms = 200;
    c->initial_debris_pct = make_range(2, 8);
    c->initial_impurity_pct = make_range(3, 10);
    c->cleaning_removal_pct = make_range(85, 98);
    c->washing_removal_pct = make_range(90, 99);
    c->paste_particle_mm = make_range(1.0, 5.0);
    c->droplet_final_um = make_range(30, 60);
    c->oil_yield_pct = make_range(12, 18);
    c->water_pct = make_range(35, 45);
    c->press_method = 0;
    for (i = 0; i < NUM_STAGES; i++) {
        c->proc_time_ms[i] = make_range(pt_lo[i], pt_hi[i]);
        c->temp_nominal_c[i] = make_range(tn_lo[i], tn_hi[i]);
        c->temp_limit_c[i] = make_range(tl_lo[i], tl_hi[i]);
        c->pressure_nominal_bar[i] = make_range(pn_lo[i], pn_hi[i]);
        c->pressure_limit_bar[i] = make_range(pl_lo[i], pl_hi[i]);
        c->max_proc_time_ms[i] = mp[i];
    }
    c->fault_prob_pct = 1.0;
    c->warn_margin_pct = 10.0;
    c->qc_report_interval_ms = 2000;
    c->use_openmp = 1;
    c->omp_threads = 4;
    c->sim_elements_per_lot = 500000;
    c->critical_pause_ms = 2000;
    c->max_critical_events = 10;
    c->drain_timeout_ms = 3000;
    strncpy(c->log_file, "olive_press.log", PATH_LEN - 1);
    c->enable_gui = 1;
    c->gui_auto_close_ms = 5000;
    c->window_w = 1100;
    c->window_h = 700;
}

/* ------------------------- src/ipc_util.c ------------------------------- */
extern volatile sig_atomic_t g_terminate; /* set by the SIGTERM handler            */
extern double g_excluded_ms;              /* real ms a stage waited (belt stopped); 
                                             stage_run() subtracts it from the lot time */
double now_s(void);                                           /* wall clock, seconds */
int write_full(int fd, const void *buf, size_t n);            /* 0 ok, -1 error      */
/* returns 1 = full message, 0 = EOF, -1 = error, -2 = interrupted by a signal
 * before any byte was read (caller should re-check g_terminate / flags). */
int read_msg(int fd, void *buf, size_t n);
void sleep_ms_scaled(double sim_ms, double time_scale, volatile sig_atomic_t *stop);
double rand_unit(unsigned *seed);                             /* [0,1], rand_r based */
double rand_range(unsigned *seed, Range r);
double hash_unit(uint64_t x);                                 /* thread-safe, [0,1)  */
int install_handler(int sig, void (*h)(int), int flags);      /* sigaction wrapper   */
EventMsg make_event(int type, int stage, int status, uint32_t lot_id,
                    double v1, double v2, double v3, const char *text);
int send_event(int fd, const EventMsg *ev);                   /* 0 ok, -1 error      */

/* ------------------------- src/stage_support.c -------------------------- */
typedef void (*ProcessFn)(const Config *cfg, Lot *lot, unsigned *seed);
void stage_signals_init(void);    /* SIGINT/SIGPIPE ignored, SIGTERM -> g_terminate                 */
void stage_signals_unblock(void); /* SIGUSR1/2 ignored unless a handler was installed, then empty the
                                     signal mask inherited from the supervisor                      */
void report_status(const StageIO *io, StageId id, StageStatus st);
void report_event(const StageIO *io, int type, StageId id, uint32_t lot_id,
                  double v1, double v2, double v3, const char *text);
void report_telemetry(const Config *cfg, const StageIO *io, StageId id,
                      uint32_t lot_id, double sim_ms, unsigned *seed);
/* Sleep whatever is left of this lot's random processing time (proc_time_ms[id])
 * after the computation that started at t0_s. */
void pad_processing_time(const Config *cfg, StageId id, double t0_s, unsigned *seed);
/* Generic loop for stages 1..5: read Lot -> fn() -> telemetry -> forward Lot.
 * Returns 0 when its input reaches EOF or g_terminate is set. */
int stage_run(const Config *cfg, const StageIO *io, StageId id, ProcessFn fn);

/* ------------------------- src/config.c  (Partner 2) -------------------- */
int config_load(const char *path, Config *cfg); /* starts from config_set_defaults; 0 ok, -1 error */

/* ------------------------- process entry points ------------------------- */
/* Each runs in its own forked process, MUST return normally (never exit()) and
 * begins with stage_signals_init() ... stage_signals_unblock().                */
void reception_main(const Config *cfg, const StageIO *io);   /* Partner 2 */
void cleaning_main(const Config *cfg, const StageIO *io);    /* Partner 2 */
void washing_main(const Config *cfg, const StageIO *io);     /* Partner 2 */
void crushing_main(const Config *cfg, const StageIO *io);    /* Partner 3 */
void malaxation_main(const Config *cfg, const StageIO *io);  /* Partner 3 */
void pressing_main(const Config *cfg, const StageIO *io);    /* Partner 3 */
void quality_control_main(const Config *cfg, const StageIO *io); /* Partner 4: in_fd=telemetry, event_fd=events */
void gui_main(const Config *cfg, const StageIO *io);         /* Partner 4: in_fd=GUI pipe */

/* Per-lot simulation kernels (called by stage_run). */
void cleaning_process(const Config *cfg, Lot *lot, unsigned *seed);   /* P2 */
void washing_process(const Config *cfg, Lot *lot, unsigned *seed);    /* P2 */
void crushing_process(const Config *cfg, Lot *lot, unsigned *seed);   /* P3 */
void malaxation_process(const Config *cfg, Lot *lot, unsigned *seed); /* P3 */
void pressing_process(const Config *cfg, Lot *lot, unsigned *seed);   /* P3 */

#endif /* OLIVE_COMMON_H */
