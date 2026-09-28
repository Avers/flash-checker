#ifndef FLASHCHECK_PIPELINE_H
#define FLASHCHECK_PIPELINE_H

#include "flashcheck/common.h"
#include "flashcheck/io.h"
#include "flashcheck/pattern.h"
#include "flashcheck/pool.h"
#include "flashcheck/stats.h"

#define PIPELINE_BATCH_MAX 16

typedef struct {
    uint8_t *buf;
    uint64_t off;
    size_t len;
} pipeline_pend;

typedef struct {
    uint64_t off;
    uint32_t pass;
    int valid;
    int res;
} pipeline_rd;

typedef struct {
    io_ops *io;
    uint64_t chunk;
    uint64_t capacity;
    pattern_kind kind;
    uint64_t test_id;
    gen_pool *pool;
    uint8_t *got;
    uint64_t io_ns;
    int wbatch;
    int rbatch;
    int npend;
    pipeline_pend pend[PIPELINE_BATCH_MAX];
    int rd_n;
    pipeline_rd rd[PIPELINE_BATCH_MAX];
    uint8_t *rd_arena;
} pipeline;

int pipeline_init(pipeline *p, io_ops *io, uint64_t chunk, pattern_kind kind, uint64_t test_id,
                  uint64_t capacity, int depth, char *err, size_t errn);
void pipeline_free(pipeline *p);
size_t pipeline_len(const pipeline *p, uint64_t off);

int pipeline_write(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st);
int pipeline_verify(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st);
int pipeline_flush(pipeline *p, stage_stats *st);
void pipeline_prefetch(pipeline *p, uint64_t off, uint32_t pass);
int pipeline_window_pass(pipeline *p, uint64_t start, uint64_t end, uint64_t window_chunks,
                         uint32_t pass, stage_stats *st, int stop_on_fail);
int pipeline_verify_offsets(pipeline *p, const uint64_t *offs, size_t n, uint32_t pass,
                            stage_stats *st, int stop_on_fail);
uint64_t pipeline_probes(uint64_t start, uint64_t end, uint64_t chunk, size_t max_probes,
                         uint64_t *out, size_t out_cap);
void pipeline_timing(const pipeline *p, uint64_t *gen_ns, uint64_t *io_ns);

#endif
