#ifndef FLASHCHECK_STATS_H
#define FLASHCHECK_STATS_H

#include "flashcheck/common.h"
#include "flashcheck/pattern.h"

#define SPEED_MAX_SAMPLES 8192
#define SPEED_WINDOW_MS 50

typedef struct {
    uint64_t t_ms;
    uint64_t total_bytes;
} speed_sample;

typedef struct {
    speed_sample *s;
    size_t n;
    size_t cap;
    uint64_t start_ms;
    uint64_t last_ms;
    uint64_t total_bytes;
} speed_track;

void speed_init(speed_track *t);
void speed_free(speed_track *t);
void speed_mark(speed_track *t, uint64_t total_bytes);
void speed_summary(const speed_track *t, uint64_t *bytes, double *sec, double *avg_bps,
                   double *max_bps, double *min_bps);
void speed_json(const speed_track *t, void *f, int indent);

typedef struct {
    uint64_t chunks_written;
    uint64_t chunks_verified;
    uint64_t chunks_failed;
    uint64_t bytes_written;
    uint64_t bytes_verified;
    uint64_t bad_bytes;
    uint64_t io_errors;
    uint64_t retries;
    uint64_t first_fail_off;
    int has_first_fail;
    uint64_t first_fail_lba;
    uint64_t alias_src_off;
    int has_alias;
    int alias_confirmed;
    int foreign_data;
    int stale_data;
    int unwritten_data;
} stage_stats;

void stage_stats_reset(stage_stats *s);
void stage_stats_merge(stage_stats *dst, const stage_stats *src);

#endif
