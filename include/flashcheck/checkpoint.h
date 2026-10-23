#ifndef FLASHCHECK_CHECKPOINT_H
#define FLASHCHECK_CHECKPOINT_H

#include "flashcheck/common.h"

#define CP_STAGE_MAX 32

typedef struct {
    char stage[CP_STAGE_MAX];
    uint64_t test_id;
    uint64_t offset;
    uint64_t bytes_written;
    uint64_t bytes_verified;
    uint64_t errors;
    uint64_t device_id;
    uint64_t device_fingerprint;
    uint64_t capacity;
    uint64_t tested_start;
    uint64_t tested_limit;
    uint64_t chunk_size;
    uint64_t passes;
    uint64_t pass;
    int phase;
    int pattern;
    int mode;
    double write_sec;
    double read_sec;
    int complete;
} checkpoint;

int checkpoint_load(const char *path, checkpoint *cp);
int checkpoint_save(const char *path, const checkpoint *cp);
int checkpoint_matches(const checkpoint *cp, uint64_t device_id, uint64_t device_fingerprint,
                       uint64_t capacity,
                       uint64_t tested_start, uint64_t tested_limit, uint64_t chunk_size,
                       int pattern, int mode);

#endif
