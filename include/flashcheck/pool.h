#ifndef FLASHCHECK_POOL_H
#define FLASHCHECK_POOL_H

#include "flashcheck/common.h"
#include "flashcheck/pattern.h"

#define GEN_POOL_MAX_DEPTH 16

typedef struct gen_pool gen_pool;

gen_pool *gen_pool_create(pattern_kind kind, uint64_t test_id, uint64_t chunk, uint64_t capacity,
                          int depth);
void gen_pool_destroy(gen_pool *g);
void gen_pool_prefetch(gen_pool *g, uint64_t off, uint32_t pass);
uint8_t *gen_pool_take(gen_pool *g, uint64_t off, uint32_t pass);
void gen_pool_release(gen_pool *g, uint8_t *buf);
uint64_t gen_pool_gen_ns(gen_pool *g);

#endif
