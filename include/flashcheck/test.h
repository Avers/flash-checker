#ifndef FLASHCHECK_TEST_H
#define FLASHCHECK_TEST_H

#include "flashcheck/common.h"
#include "flashcheck/config.h"
#include "flashcheck/device.h"
#include "flashcheck/io.h"
#include "flashcheck/pipeline.h"
#include "flashcheck/stats.h"

#define MAX_STAGES 8

typedef struct {
    const char *name;
    int executed;
    uint64_t bytes_written;
    uint64_t bytes_verified;
    uint64_t regions_written;
    uint64_t regions_verified;
    uint64_t chunks_failed;
    uint64_t io_errors;
    uint64_t bad_bytes;
    uint64_t first_fail_off;
    int has_first_fail;
    uint64_t alias_src_off;
    int has_alias;
    int alias_confirmed;
    int foreign_data;
    int stale_data;
    int unwritten_data;
    double write_sec;
    double read_sec;
    double write_bps;
    double read_bps;
    double max_bps;
    double min_bps;
    uint64_t reliable_capacity;
    int has_capacity;
    char note[192];
} stage_report;

typedef struct {
    const config *cfg;
    device_info *dev;
    io_ops *io;
    pipeline pl;
    int pipeline_ready;
    volatile int interrupted;
    stage_report st[MAX_STAGES];
    size_t nst;
    uint64_t reliable_capacity;
    int has_capacity;
    uint64_t tested_bytes;
    uint64_t test_id;
    uint64_t resume_off;
    const char *checkpoint_path;
} run_ctx;

stage_report *stage_new(run_ctx *c, const char *name);
void stage_progress(const char *label, uint64_t done, uint64_t total, double bps);
uint64_t ctx_start(const run_ctx *c);
uint64_t ctx_end(const run_ctx *c);
void stage_fill_speed(stage_report *r, const speed_track *w, const speed_track *rd);

int stage_identify(run_ctx *c, stage_report *r);
int stage_benchmark(run_ctx *c, stage_report *r);
int stage_sparse(run_ctx *c, stage_report *r);
int stage_retention(run_ctx *c, stage_report *r);
int stage_full(run_ctx *c, stage_report *r);
int stage_boundary(run_ctx *c, stage_report *r);

int run_execute(run_ctx *c);
int bench_run(run_ctx *c);
verdict run_verdict(const run_ctx *c);

#endif
