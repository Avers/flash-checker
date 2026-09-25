#ifndef FLASHCHECK_PATTERN_H
#define FLASHCHECK_PATTERN_H

#include "flashcheck/common.h"

#define PAT_MAGIC "FLCHKPAT"
#define PAT_MAGIC_LEN 8
#define PAT_VERSION 1u
#define PAT_HEADER_SIZE 64u

typedef struct {
    uint64_t test_id;
    uint64_t pass;
    uint64_t chunk_index;
} pattern_id;

typedef enum { PAT_CHACHA20 = 0, PAT_PRNG = 1, PAT_ZERO = 2 } pattern_kind;

const char *pattern_kind_str(pattern_kind k);
int pattern_kind_parse(const char *s, pattern_kind *out);

void pattern_seed(const pattern_id *id, uint8_t key[32]);
void pattern_tag(const pattern_id *id, uint64_t *tag);
void pattern_header(const pattern_id *id, uint8_t hdr[PAT_HEADER_SIZE]);
void pattern_fill(pattern_kind kind, void *buf, size_t len, const pattern_id *id,
                  uint64_t off_in_chunk);
int pattern_parse_header(const uint8_t *buf, pattern_id *out);
size_t pattern_body_off(uint64_t off_in_chunk);

#endif
