#ifndef FLASHCHECK_SHA256_H
#define FLASHCHECK_SHA256_H

#include "flashcheck/common.h"

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);
void put_le64(uint8_t *p, uint64_t v);
void put_le32(uint8_t *p, uint32_t v);
uint64_t get_le64(const uint8_t *p);
uint32_t get_le32(const uint8_t *p);

#endif
