#include "flashcheck/pattern.h"

#include "flashcheck/chacha20.h"
#include "flashcheck/sha256.h"

#define PRNG_SUBBLOCK (1u << 20)

const char *pattern_kind_str(pattern_kind k)
{
    switch (k) {
    case PAT_PRNG:
        return "prng";
    case PAT_ZERO:
        return "zero";
    default:
        return "chacha20";
    }
}

int pattern_kind_parse(const char *s, pattern_kind *out)
{
    if (strcmp(s, "chacha20") == 0 || strcmp(s, "chacha") == 0)
        *out = PAT_CHACHA20;
    else if (strcmp(s, "prng") == 0 || strcmp(s, "xoshiro") == 0)
        *out = PAT_PRNG;
    else if (strcmp(s, "zero") == 0)
        *out = PAT_ZERO;
    else
        return -1;
    return 0;
}

void pattern_seed(const pattern_id *id, uint8_t key[32])
{
    uint8_t m[64];

    memcpy(m, "flashcheck/pat/v1", 17);
    put_le64(m + 17, id->test_id);
    put_le64(m + 25, id->pass);
    put_le64(m + 33, id->chunk_index);
    memset(m + 41, 0, 23);
    sha256(m, sizeof m, key);
}

void pattern_tag(const pattern_id *id, uint64_t *tag)
{
    uint8_t m[64], d[32];

    memcpy(m, "flashcheck/tag/v1", 17);
    put_le64(m + 17, id->test_id);
    put_le64(m + 25, id->pass);
    put_le64(m + 33, id->chunk_index);
    memset(m + 41, 0, 23);
    sha256(m, sizeof m, d);
    *tag = get_le64(d);
}

void pattern_header(const pattern_id *id, uint8_t hdr[PAT_HEADER_SIZE])
{
    uint64_t tag;

    memset(hdr, 0, PAT_HEADER_SIZE);
    memcpy(hdr, PAT_MAGIC, PAT_MAGIC_LEN);
    put_le32(hdr + 8, PAT_VERSION);
    put_le32(hdr + 12, PAT_HEADER_SIZE);
    put_le64(hdr + 16, id->test_id);
    put_le64(hdr + 24, id->pass);
    put_le64(hdr + 32, id->chunk_index);
    pattern_tag(id, &tag);
    put_le64(hdr + 40, tag);
}

int pattern_parse_header(const uint8_t *buf, pattern_id *out)
{
    if (memcmp(buf, PAT_MAGIC, PAT_MAGIC_LEN) != 0)
        return 0;
    if (get_le32(buf + 8) != PAT_VERSION)
        return 0;
    if (get_le32(buf + 12) != PAT_HEADER_SIZE)
        return 0;
    out->test_id = get_le64(buf + 16);
    out->pass = get_le64(buf + 24);
    out->chunk_index = get_le64(buf + 32);
    return 1;
}

size_t pattern_body_off(uint64_t off_in_chunk) { return off_in_chunk < PAT_HEADER_SIZE ? 0 : 1; }

static void fill_chacha(uint8_t *buf, size_t len, const pattern_id *id, uint64_t off)
{
    uint8_t key[32], nonce[12];
    uint64_t pos;

    if (off < PAT_HEADER_SIZE) {
        size_t n = (size_t)FC_MIN(len, (size_t)(PAT_HEADER_SIZE - off));
        uint8_t hdr[PAT_HEADER_SIZE];
        pattern_header(id, hdr);
        memcpy(buf, hdr + off, n);
        buf += n;
        len -= n;
        off += n;
        if (len == 0)
            return;
    }
    pattern_seed(id, key);
    memset(nonce, 0, sizeof nonce);
    pos = off - PAT_HEADER_SIZE;
    chacha20_stream_at(key, nonce, pos, buf, len);
}

static uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static void prng_seed(const pattern_id *id, uint64_t subblock, uint64_t s[4])
{
    uint8_t key[32], m[40];

    pattern_seed(id, key);
    memcpy(m, key, 32);
    put_le64(m + 32, subblock);
    sha256(m, sizeof m, key);
    for (int i = 0; i < 4; i++)
        s[i] = get_le64(key + i * 8);
}

static uint64_t prng_next_word(uint64_t s[4])
{
    uint64_t r = s[1] * 5;
    uint64_t v;

    r = rotl64(r, 7) * 9;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= s[1];
    s[3] = rotl64(s[3], 11);
    v = s[0] + s[3];
    return v;
}

static void fill_prng(uint8_t *buf, size_t len, const pattern_id *id, uint64_t off)
{
    size_t done = 0;

    if (off < PAT_HEADER_SIZE) {
        size_t n = (size_t)FC_MIN(len, (size_t)(PAT_HEADER_SIZE - off));
        uint8_t hdr[PAT_HEADER_SIZE];
        pattern_header(id, hdr);
        memcpy(buf, hdr + off, n);
        buf += n;
        len -= n;
        off += n;
        if (len == 0)
            return;
    }
    while (done < len) {
        uint64_t pos = off + done - PAT_HEADER_SIZE;
        uint64_t sb = pos / PRNG_SUBBLOCK;
        uint64_t sub_off = pos % PRNG_SUBBLOCK;
        uint64_t s[4];
        uint8_t word[8];
        size_t wpos = (size_t)(sub_off % 8);
        size_t n = (size_t)FC_MIN(len - done, (size_t)(PRNG_SUBBLOCK - sub_off));

        prng_seed(id, sb, s);
        for (uint64_t i = 0; i < sub_off / 8; i++)
            (void)prng_next_word(s);
        put_le64(word, prng_next_word(s));
        for (size_t i = 0; i < n; i++) {
            buf[done + i] = word[wpos];
            wpos++;
            if (wpos == 8) {
                wpos = 0;
                put_le64(word, prng_next_word(s));
            }
        }
        done += n;
    }
}

void pattern_fill(pattern_kind kind, void *buf, size_t len, const pattern_id *id,
                  uint64_t off_in_chunk)
{
    uint8_t *p = buf;

    switch (kind) {
    case PAT_ZERO: {
        memset(p, 0, len);
        if (off_in_chunk < PAT_HEADER_SIZE) {
            size_t n = (size_t)FC_MIN(len, (size_t)(PAT_HEADER_SIZE - off_in_chunk));
            uint8_t hdr[PAT_HEADER_SIZE];
            pattern_header(id, hdr);
            memcpy(p, hdr + off_in_chunk, n);
        }
        break;
    }
    case PAT_PRNG:
        fill_prng(p, len, id, off_in_chunk);
        break;
    default:
        fill_chacha(p, len, id, off_in_chunk);
        break;
    }
}
