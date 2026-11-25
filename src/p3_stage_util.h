/* Small private checks for P3. This does not change the shared interface. */
#ifndef P3_STAGE_UTIL_H
#define P3_STAGE_UTIL_H
#include "common.h"
#include <math.h>
#include <stdio.h>

static inline int p3_range_valid(Range r)
{
    return isfinite(r.min) && isfinite(r.max) && r.min >= 0 && r.max >= r.min;
}

static inline int p3_input_valid(const Config *cfg, const Lot *lot,
                                 const unsigned *seed, StageId id)
{
    if (!cfg || !lot || !seed || cfg->sim_elements_per_lot <= 0 ||
        cfg->omp_threads <= 0 || !isfinite(cfg->time_scale) || cfg->time_scale <= 0 ||
        !isfinite(lot->mass_kg) || lot->mass_kg < 0 ||
        !p3_range_valid(cfg->proc_time_ms[id])) {
        fprintf(stderr, "%s: invalid configuration or lot\n", stage_name(id));
        return 0;
    }
    return 1;
}
#endif
