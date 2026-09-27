#ifndef FLASHCHECK_CONFIG_H
#define FLASHCHECK_CONFIG_H

#include "flashcheck/common.h"
#include "flashcheck/pattern.h"

typedef enum {
    MODE_IDENTIFY = 0,
    MODE_QUICK = 1,
    MODE_STANDARD = 2,
    MODE_FULL = 3,
    MODE_ADAPTIVE = 4
} run_mode;

typedef enum {
    IO_BACKEND_SYNC = 0,
    IO_BACKEND_URING = 1,
} io_backend;

typedef struct {
    const char *device;
    run_mode mode;
    int destructive;
    int assume_yes;
    int dry_run;
    int verbose;
    int want_direct;
    io_backend io_backend;
    int depth;
    uint64_t chunk_size;
    uint64_t window_size;
    uint64_t limit;
    uint64_t offset;
    uint64_t passes;
    uint64_t bench_bytes;
    uint64_t boundary_budget;
    uint64_t boundary_resolution;
    pattern_kind pattern;
    int self_test;
    uint64_t st_reported;
    uint64_t st_real;
    int st_real_set;
    const char *json_path;
    const char *checkpoint_path;
    int resume;
} config;

void config_defaults(config *c);
int config_validate(config *c, char *err, size_t errn);
const char *mode_str(run_mode m);

#endif
