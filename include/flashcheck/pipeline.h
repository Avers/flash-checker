#ifndef FLASHCHECK_PIPELINE_H
#define FLASHCHECK_PIPELINE_H

#include "flashcheck/common.h"
#include "flashcheck/io.h"
#include "flashcheck/pattern.h"
#include "flashcheck/stats.h"

typedef struct {
    io_ops *io;
    uint64_t chunk;
    uint64_t capacity;
    pattern_kind kind;
    uint64_t test_id;
    uint8_t *exp;
    uint8_t *got;
} pipeline;

int pipeline_init(pipeline *p, io_ops *io, uint64_t chunk, pattern_kind kind, uint64_t test_id,
                  uint64_t capacity, char *err, size_t errn);
void pipeline_free(pipeline *p);
size_t pipeline_len(const pipeline *p, uint64_t off);

int pipeline_write(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st);
int pipeline_verify(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st);
int pipeline_window_pass(pipeline *p, uint64_t start, uint64_t end, uint64_t window_chunks,
                         uint32_t pass, stage_stats *st, int stop_on_fail);
int pipeline_verify_offsets(pipeline *p, const uint64_t *offs, size_t n, uint32_t pass,
                            stage_stats *st, int stop_on_fail);
uint64_t pipeline_probes(uint64_t start, uint64_t end, uint64_t chunk, size_t max_probes,
                         uint64_t *out, size_t out_cap);

#endif
