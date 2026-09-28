#include "flashcheck/io.h"
#include "flashcheck/pipeline.h"
#include "flashcheck/pool.h"
#include "flashcheck/util.h"
#include "test_util.h"

#include <stdio.h>

static void test_pool_prefetch_matches_reference(void)
{
    gen_pool *g;
    uint8_t *buf;
    uint8_t *ref;
    pattern_id id;
    uint64_t chunk = 1u << 20;
    uint64_t cap = 16u << 20;

    T_BEGIN("pool prefetch yields the same bytes as pattern_fill");
    g = gen_pool_create(PAT_CHACHA20, 42, chunk, cap, 4);
    CHECK(g != NULL);
    if (g == NULL)
        return;

    ref = xalloc_aligned(FC_ALIGN, (size_t)chunk);
    id.test_id = 42;
    id.pass = 1;
    id.chunk_index = 1;

    gen_pool_prefetch(g, chunk, 1);
    buf = gen_pool_take(g, chunk, 1);
    CHECK(buf != NULL);
    pattern_fill(PAT_CHACHA20, ref, (size_t)chunk, &id, 0);
    CHECK(memcmp(buf, ref, (size_t)chunk) == 0);
    gen_pool_release(g, buf);

    T_BEGIN("pool take without prefetch fills inline");
    buf = gen_pool_take(g, chunk * 3, 7);
    CHECK(buf != NULL);
    id.pass = 7;
    id.chunk_index = 3;
    pattern_fill(PAT_CHACHA20, ref, (size_t)chunk, &id, 0);
    CHECK(memcmp(buf, ref, (size_t)chunk) == 0);
    gen_pool_release(g, buf);

    T_BEGIN("pool take is repeatable after release");
    buf = gen_pool_take(g, chunk * 3, 7);
    CHECK(buf != NULL);
    CHECK(memcmp(buf, ref, (size_t)chunk) == 0);
    gen_pool_release(g, buf);

    T_BEGIN("pool generation time is accounted");
    CHECK(gen_pool_gen_ns(g) > 0);

    free(ref);
    gen_pool_destroy(g);
}

static void test_pool_depth_zero(void)
{
    gen_pool *g;
    uint8_t *buf;
    uint64_t chunk = 1u << 20;

    T_BEGIN("depth 0 pool works without a helper thread");
    g = gen_pool_create(PAT_ZERO, 1, chunk, chunk * 4, 0);
    CHECK(g != NULL);
    if (g == NULL)
        return;
    gen_pool_prefetch(g, chunk, 0);
    buf = gen_pool_take(g, chunk, 0);
    CHECK(buf != NULL);
    CHECK(memcmp(buf, PAT_MAGIC, PAT_MAGIC_LEN) == 0);
    CHECK(buf[PAT_HEADER_SIZE] == 0);
    CHECK(buf[(size_t)chunk - 1] == 0);
    gen_pool_release(g, buf);
    gen_pool_destroy(g);
}

static int run_window(int depth, int policy, stage_stats *st)
{
    io_ops *io;
    pipeline p;
    char err[128];
    int e = 0;
    int rc;

    io = io_fake_open(16u << 20, policy == FAKE_HONEST ? 16u << 20 : 4u << 20, policy, &e);
    if (io == NULL)
        return -2;
    if (pipeline_init(&p, io, 1u << 20, PAT_CHACHA20, 7, 16u << 20, depth, err, sizeof err) != 0) {
        io->close(io);
        return -2;
    }
    stage_stats_reset(st);
    rc = pipeline_window_pass(&p, 0, 8u << 20, 8, 1, st, 0);
    pipeline_free(&p);
    io->close(io);
    return rc;
}

static void test_pipeline_parallel(void)
{
    stage_stats a, b;
    uint64_t gen_ns = 0, io_ns = 0;
    io_ops *io;
    pipeline p;
    char err[128];
    int e = 0;

    T_BEGIN("parallel window pass matches sequential results");
    CHECK_EQ_U64((uint64_t)(run_window(0, FAKE_HONEST, &a) == 0), 1);
    CHECK_EQ_U64((uint64_t)(run_window(4, FAKE_HONEST, &b) == 0), 1);
    CHECK_EQ_U64(b.chunks_written, a.chunks_written);
    CHECK_EQ_U64(b.chunks_verified, a.chunks_verified);
    CHECK_EQ_U64(b.bytes_verified, a.bytes_verified);
    CHECK_EQ_U64(b.chunks_failed, a.chunks_failed);

    T_BEGIN("parallel window pass detects alias device");
    CHECK(run_window(4, FAKE_ALIAS, &b) > 0);
    CHECK(b.chunks_failed > 0);
    CHECK(b.has_first_fail);

    T_BEGIN("parallel pipeline accounts generation and io time");
    io = io_fake_open(16u << 20, 16u << 20, FAKE_HONEST, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    CHECK_EQ_U64(pipeline_init(&p, io, 1u << 20, PAT_CHACHA20, 7, 16u << 20, 4, err, sizeof err),
                 0);
    stage_stats_reset(&b);
    CHECK_EQ_U64(pipeline_window_pass(&p, 0, 4u << 20, 2, 1, &b, 0), 0);
    pipeline_timing(&p, &gen_ns, &io_ns);
    CHECK(gen_ns > 0);
    CHECK(io_ns > 0);
    pipeline_free(&p);
    io->close(io);
}

static void test_pipeline_prefetched_offsets(void)
{
    io_ops *io;
    pipeline p;
    stage_stats st;
    char err[128];
    int e = 0;
    uint64_t offs[4] = { 3u << 20, 0, 2u << 20, 1u << 20 };

    T_BEGIN("verify_offsets with prefetch works out of order");
    io = io_fake_open(16u << 20, 16u << 20, FAKE_HONEST, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    CHECK_EQ_U64(pipeline_init(&p, io, 1u << 20, PAT_CHACHA20, 11, 16u << 20, 4, err, sizeof err),
                 0);
    stage_stats_reset(&st);
    for (size_t i = 0; i < 4; i++) {
        if (pipeline_write(&p, offs[i], 1, &st) != 0)
            break;
    }
    CHECK_EQ_U64(pipeline_flush(&p, &st), 0);
    CHECK_EQ_U64(st.chunks_written, 4);
    stage_stats_reset(&st);
    CHECK_EQ_U64(pipeline_verify_offsets(&p, offs, 4, 1, &st, 0), 0);
    CHECK_EQ_U64(st.chunks_verified, 4);
    CHECK_EQ_U64(st.chunks_failed, 0);
    pipeline_free(&p);
    io->close(io);
}

static void test_pipeline_write_queue(void)
{
    io_ops *io;
    pipeline p;
    stage_stats st;
    char err[128];
    int e = 0;
    int rc = 0;
    int i;

    T_BEGIN("batched writes report progress before the flush");
    io = io_fake_open(16u << 20, 16u << 20, FAKE_HONEST, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    CHECK_EQ_U64(pipeline_init(&p, io, 1u << 20, PAT_CHACHA20, 13, 16u << 20, 4, err, sizeof err),
                 0);
    stage_stats_reset(&st);
    for (i = 0; i < 6; i++) {
        if (pipeline_write(&p, (uint64_t)i << 20, 1, &st) != 0) {
            T_FAIL("write %d failed", i);
            rc = 1;
            break;
        }
    }
    CHECK_EQ_U64(rc, 0);
    CHECK_EQ_U64(st.chunks_written, 6);
    CHECK_EQ_U64(pipeline_flush(&p, &st), 0);
    CHECK_EQ_U64(st.chunks_written, 6);

    T_BEGIN("read-ahead window serves sequential verifies");
    stage_stats_reset(&st);
    for (i = 0; i < 6; i++) {
        if (pipeline_verify(&p, (uint64_t)i << 20, 1, &st) != 0) {
            T_FAIL("verify %d failed", i);
            break;
        }
    }
    CHECK_EQ_U64(st.chunks_verified, 6);
    CHECK_EQ_U64(st.chunks_failed, 0);

    T_BEGIN("a new write invalidates the read-ahead window");
    stage_stats_reset(&st);
    CHECK_EQ_U64(pipeline_write(&p, 0, 2, &st), 0);
    CHECK_EQ_U64(pipeline_flush(&p, &st), 0);
    stage_stats_reset(&st);
    CHECK_EQ_U64(pipeline_verify(&p, 0, 2, &st), 0);
    CHECK_EQ_U64(st.chunks_verified, 1);
    CHECK_EQ_U64(st.chunks_failed, 0);

    pipeline_free(&p);
    io->close(io);
}

static void test_pipeline_failed_batch(void)
{
    io_ops *io;
    pipeline p;
    stage_stats st;
    char err[128];
    int e = 0;
    int i;

    T_BEGIN("failed batched writes are un-counted and reported as io errors");
    io = io_fake_open(16u << 20, 4u << 20, FAKE_ERROR, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    CHECK_EQ_U64(pipeline_init(&p, io, 1u << 20, PAT_CHACHA20, 17, 16u << 20, 4, err, sizeof err),
                 0);
    stage_stats_reset(&st);
    for (i = 0; i < 8; i++)
        pipeline_write(&p, (uint64_t)i << 20, 1, &st);
    CHECK_EQ_U64(st.chunks_written, 8);
    CHECK(pipeline_flush(&p, &st) != 0);
    CHECK_EQ_U64(st.chunks_written, 4);
    CHECK_EQ_U64(st.bytes_written, 4u << 20);
    CHECK_EQ_U64(st.io_errors, 4);
    pipeline_free(&p);
    io->close(io);
}

void test_pool(void)
{
    printf("generator pool and parallel pipeline\n");
    test_pool_prefetch_matches_reference();
    test_pool_depth_zero();
    test_pipeline_parallel();
    test_pipeline_prefetched_offsets();
    test_pipeline_write_queue();
    test_pipeline_failed_batch();
}
