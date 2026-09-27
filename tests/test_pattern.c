#include "flashcheck/config.h"
#include "flashcheck/pattern.h"
#include "flashcheck/pipeline.h"
#include "flashcheck/sha256.h"
#include "flashcheck/util.h"
#include "test_util.h"

static pattern_id mk(uint64_t idx, uint64_t pass, uint64_t tid)
{
    pattern_id id;
    id.test_id = tid;
    id.pass = pass;
    id.chunk_index = idx;
    return id;
}

static void test_pattern_determinism(void)
{
    uint8_t *a = xalloc(65536), *b = xalloc(65536);
    pattern_id id = mk(7, 1, 0xabcdef);

    T_BEGIN("pattern fill is deterministic");
    pattern_fill(PAT_CHACHA20, a, 65536, &id, 0);
    pattern_fill(PAT_CHACHA20, b, 65536, &id, 0);
    CHECK(memcmp(a, b, 65536) == 0);
    free(a);
    free(b);
}

static void test_pattern_uniqueness(void)
{
    uint8_t *a = xalloc(8192), *b = xalloc(8192);
    pattern_id ia = mk(1, 1, 42), ib = mk(2, 1, 42), ip = mk(1, 2, 42), it = mk(1, 1, 43);
    pattern_kind kinds[3] = { PAT_CHACHA20, PAT_PRNG, PAT_ZERO };

    T_BEGIN("different chunks differ");
    for (int k = 0; k < 3; k++) {
        pattern_fill(kinds[k], a, 8192, &ia, 0);
        pattern_fill(kinds[k], b, 8192, &ib, 0);
        CHECK(memcmp(a, b, 8192) != 0);
    }
    T_BEGIN("pass number changes pattern");
    pattern_fill(PAT_CHACHA20, a, 8192, &ia, 0);
    pattern_fill(PAT_CHACHA20, b, 8192, &ip, 0);
    CHECK(memcmp(a, b, 8192) != 0);
    T_BEGIN("test id changes pattern");
    pattern_fill(PAT_CHACHA20, a, 8192, &ia, 0);
    pattern_fill(PAT_CHACHA20, b, 8192, &it, 0);
    CHECK(memcmp(a, b, 8192) != 0);
    free(a);
    free(b);
}

static void test_pattern_seek(void)
{
    uint8_t *full = xalloc(131072);
    uint8_t *part = xalloc(4096);
    pattern_id id = mk(11, 3, 0x1234);

    T_BEGIN("pattern seek equals full fill");
    pattern_fill(PAT_CHACHA20, full, 131072, &id, 0);
    pattern_fill(PAT_CHACHA20, part, 4096, &id, 100000);
    CHECK(memcmp(part, full + 100000, 4096) == 0);
    pattern_fill(PAT_CHACHA20, part, 3000, &id, 50000);
    CHECK(memcmp(part, full + 50000, 3000) == 0);
    free(full);
    free(part);
}

static void test_pattern_prng_seek(void)
{
    uint8_t *full = xalloc(4u << 20);
    uint8_t *part = xalloc(8192);
    pattern_id id = mk(5, 1, 77);

    T_BEGIN("prng seek equals full fill");
    pattern_fill(PAT_PRNG, full, 4u << 20, &id, 0);
    pattern_fill(PAT_PRNG, part, 8192, &id, (3u << 20) + 12345);
    CHECK(memcmp(part, full + (3u << 20) + 12345, 8192) == 0);
    free(full);
    free(part);
}

static void test_pattern_header(void)
{
    uint8_t buf[4096];
    pattern_id id = mk(99, 2, 0xfeed), parsed;
    uint64_t tag = 0, tag2 = 0;

    T_BEGIN("header round trip and tag");
    pattern_fill(PAT_CHACHA20, buf, sizeof buf, &id, 0);
    CHECK(pattern_parse_header(buf, &parsed) == 1);
    CHECK_EQ_U64(parsed.test_id, id.test_id);
    CHECK_EQ_U64(parsed.pass, id.pass);
    CHECK_EQ_U64(parsed.chunk_index, id.chunk_index);
    pattern_tag(&id, &tag);
    CHECK_EQ_U64(get_le64(buf + 40), tag);
    pattern_tag(&id, &tag2);
    CHECK_EQ_U64(tag, tag2);
    T_BEGIN("foreign data has no header");
    memset(buf, 0x5a, sizeof buf);
    CHECK(pattern_parse_header(buf, &parsed) == 0);
}

static void test_chunk_math(void)
{
    io_ops *io;
    pipeline p;
    char err[128];
    int ie = 0;
    uint64_t chunk = 1u << 20;
    uint64_t cap = chunk * 4 + 1234;

    T_BEGIN("pipeline tail chunk length");
    io = io_fake_open(cap, cap, FAKE_HONEST, &ie);
    CHECK(io != NULL);
    CHECK_EQ_U64(pipeline_init(&p, io, chunk, PAT_CHACHA20, 1, cap, 0, err, sizeof err), 0);
    CHECK_EQ_U64(pipeline_len(&p, 0), chunk);
    CHECK_EQ_U64(pipeline_len(&p, chunk * 4), 1234);
    pipeline_free(&p);
    io->close(io);
}

static void test_probes(void)
{
    uint64_t out[64];
    uint64_t n, chunk = 1u << 20;
    uint64_t end = 64ULL << 30;

    T_BEGIN("probe offsets are increasing and inside range");
    n = pipeline_probes(0, end, chunk, 48, out, 64);
    CHECK(n >= 8);
    for (uint64_t i = 0; i < n; i++) {
        CHECK(out[i] % chunk == 0);
        if (i > 0)
            CHECK(out[i] > out[i - 1]);
        CHECK(out[i] < end);
    }
    T_BEGIN("probe count is capped");
    n = pipeline_probes(0, end, chunk, 4, out, 64);
    CHECK_EQ_U64(n, 4);
    T_BEGIN("tiny range yields one probe");
    n = pipeline_probes(0, chunk, chunk, 8, out, 64);
    CHECK_EQ_U64(n, 1);
}

static void test_util_parsing(void)
{
    uint64_t v = 0;

    T_BEGIN("size parsing");
    CHECK(parse_size("1024", &v) == 0 && v == 1024);
    CHECK(parse_size("1MiB", &v) == 0 && v == 1048576);
    CHECK(parse_size("8M", &v) == 0 && v == 8388608);
    CHECK(parse_size("1.5GiB", &v) == 0 && v == 1610612736);
    CHECK(parse_size("2G", &v) == 0 && v == 2147483648ULL);
    CHECK(parse_size("bogus", &v) != 0);
    CHECK(parse_size("12x", &v) != 0);
    CHECK(parse_size("", &v) != 0);
    T_BEGIN("size formatting");
    {
        char sbuf[64];
        fmt_size(sbuf, sizeof sbuf, 0);
        CHECK_STR(sbuf, "0.00 B");
    }
}

static void test_first_diff(void)
{
    uint8_t a[64], b[64];

    T_BEGIN("first_diff locates first differing byte");
    memset(a, 1, sizeof a);
    memcpy(b, a, sizeof b);
    CHECK_EQ_U64(first_diff(a, b, sizeof a), sizeof a);
    b[37] = 2;
    CHECK_EQ_U64(first_diff(a, b, sizeof a), 37);
}

static void test_config_validate(void)
{
    config c;
    char err[256];

    T_BEGIN("config validation");
    config_defaults(&c);
    CHECK_EQ_U64(config_validate(&c, err, sizeof err), 0);
    c.chunk_size = 1000;
    CHECK(config_validate(&c, err, sizeof err) != 0);
    config_defaults(&c);
    c.window_size = 1;
    CHECK(config_validate(&c, err, sizeof err) != 0);
    config_defaults(&c);
    c.offset = 4096;
    c.chunk_size = 8u << 20;
    CHECK(config_validate(&c, err, sizeof err) != 0);
    config_defaults(&c);
    c.pattern = PAT_PRNG;
    CHECK_EQ_U64(config_validate(&c, err, sizeof err), 0);
}

void test_pattern_and_math(void)
{
    printf("pattern and math\n");
    test_pattern_determinism();
    test_pattern_uniqueness();
    test_pattern_seek();
    test_pattern_prng_seek();
    test_pattern_header();
    test_chunk_math();
    test_probes();
    test_util_parsing();
    test_first_diff();
    test_config_validate();
}
