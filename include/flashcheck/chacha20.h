#ifndef FLASHCHECK_CHACHA20_H
#define FLASHCHECK_CHACHA20_H

#include "flashcheck/common.h"

void chacha20_block(const uint8_t key[32], uint64_t counter, const uint8_t nonce[12],
                    uint8_t out[64]);
void chacha20_stream(const uint8_t key[32], uint64_t counter, const uint8_t nonce[12],
                     uint8_t *out, size_t len);
void chacha20_xor(const uint8_t key[32], uint64_t counter, const uint8_t nonce[12],
                  uint8_t *data, size_t len);
void chacha20_stream_at(const uint8_t key[32], const uint8_t nonce[12], uint64_t byte_off,
                        uint8_t *out, size_t len);

#endif
