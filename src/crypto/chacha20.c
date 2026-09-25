#include "flashcheck/chacha20.h"

#include "flashcheck/sha256.h"

#define ROTL32(v, c) (uint32_t)(((v) << (c)) | ((v) >> (32 - (c))))
#define QR(a, b, c, d)                                                                            \
    a += b;                                                                                        \
    d ^= a;                                                                                        \
    d = ROTL32(d, 16);                                                                             \
    c += d;                                                                                        \
    b ^= c;                                                                                        \
    b = ROTL32(b, 12);                                                                             \
    a += b;                                                                                        \
    d ^= a;                                                                                        \
    d = ROTL32(d, 8);                                                                              \
    c += d;                                                                                        \
    b ^= c;                                                                                        \
    b = ROTL32(b, 7)

static void load32(const uint8_t *p, uint32_t *v)
{
    *v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void chacha20_block(const uint8_t key[32], uint64_t counter, const uint8_t nonce[12],
                    uint8_t out[64])
{
    uint32_t st[16], x[16];
    int i;

    st[0] = 0x61707865u;
    st[1] = 0x3320646eu;
    st[2] = 0x79622d32u;
    st[3] = 0x6b206574u;
    for (i = 0; i < 8; i++)
        load32(key + i * 4, &st[4 + i]);
    st[12] = (uint32_t)counter;
    for (i = 0; i < 3; i++)
        load32(nonce + i * 4, &st[13 + i]);

    memcpy(x, st, sizeof st);
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++)
        put_le32(out + i * 4, x[i] + st[i]);
}

void chacha20_stream(const uint8_t key[32], uint64_t counter, const uint8_t nonce[12],
                     uint8_t *out, size_t len)
{
    uint8_t ks[64];
    size_t off = 0;

    while (off < len) {
        size_t n = FC_MIN(sizeof ks, len - off);
        chacha20_block(key, counter, nonce, ks);
        memcpy(out + off, ks, n);
        off += n;
        counter++;
    }
}

void chacha20_xor(const uint8_t key[32], uint64_t counter, const uint8_t nonce[12],
                  uint8_t *data, size_t len)
{
    uint8_t ks[64];
    size_t off = 0;

    while (off < len) {
        size_t n = FC_MIN(sizeof ks, len - off);
        chacha20_block(key, counter, nonce, ks);
        for (size_t i = 0; i < n; i++)
            data[off + i] ^= ks[i];
        off += n;
        counter++;
    }
}

void chacha20_stream_at(const uint8_t key[32], const uint8_t nonce[12], uint64_t byte_off,
                        uint8_t *out, size_t len)
{
    uint8_t ks[64];
    size_t skip = (size_t)(byte_off % 64);

    if (skip != 0) {
        size_t n = FC_MIN(len, sizeof ks - skip);
        chacha20_block(key, byte_off / 64, nonce, ks);
        memcpy(out, ks + skip, n);
        out += n;
        len -= n;
        byte_off += n;
    }
    chacha20_stream(key, byte_off / 64, nonce, out, len);
}
