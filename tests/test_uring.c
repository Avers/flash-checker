#include "flashcheck/io.h"
#include "flashcheck/pipeline.h"
#include "flashcheck/util.h"
#include "test_util.h"

#include <fcntl.h>
#include <unistd.h>

#define UTEST_SIZE (1u << 20)
#define UTEST_RING_ITERS 100

static int make_temp_file(char *path, size_t n)
{
    int fd;

    snprintf(path, n, "/tmp/flashcheck-uring-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, UTEST_SIZE) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

static void test_uring_roundtrip(io_ops *io)
{
    uint8_t *wbuf = xalloc_aligned(4096, 4096);
    uint8_t *rbuf = xalloc_aligned(4096, 4096);
    int i;

    T_BEGIN("io_uring write/read round trip");
    memset(wbuf, 0xa5, 4096);
    CHECK_EQ_U64(io->write(io, wbuf, 8192, 4096), 0);
    memset(rbuf, 0, 4096);
    CHECK_EQ_U64(io->read(io, rbuf, 8192, 4096), 0);
    CHECK_EQ_U64(memcmp(wbuf, rbuf, 4096), 0);

    T_BEGIN("io_uring unaligned offset falls back to the buffered fd");
    memset(wbuf, 0x3c, 100);
    CHECK_EQ_U64(io->write(io, wbuf, 4196, 100), 0);
    memset(rbuf, 0, 4096);
    CHECK_EQ_U64(io->read(io, rbuf, 4196, 100), 0);
    CHECK_EQ_U64(memcmp(wbuf, rbuf, 100), 0);

    T_BEGIN("io_uring read past end of file fails");
    CHECK(io->read(io, rbuf, UTEST_SIZE - 16, 4096) != 0);
    CHECK(io->stats->io_errors > 0);

    T_BEGIN("io_uring repeated ops reuse the ring");
    for (i = 0; i < UTEST_RING_ITERS; i++) {
        memset(wbuf, (uint8_t)i, 4096);
        if (io->write(io, wbuf, 0, 4096) != 0) {
            T_FAIL("write %d failed", i);
            break;
        }
        memset(rbuf, 0, 4096);
        if (io->read(io, rbuf, 0, 4096) != 0) {
            T_FAIL("read %d failed", i);
            break;
        }
        if (memcmp(wbuf, rbuf, 4096) != 0) {
            T_FAIL("mismatch at iteration %d", i);
            break;
        }
    }

    T_BEGIN("io_uring flush");
    CHECK_EQ_U64(io->flush(io), 0);

    free(wbuf);
    free(rbuf);
}

#define UTEST_BATCH_SEGS 40

static void test_uring_batch(io_ops *io)
{
    uint8_t *wbufs = xalloc_aligned(4096, UTEST_BATCH_SEGS * 4096);
    uint8_t *rbufs = xalloc_aligned(4096, UTEST_BATCH_SEGS * 4096);
    io_seg wsegs[UTEST_BATCH_SEGS];
    io_seg rsegs[UTEST_BATCH_SEGS];
    io_stats before;
    int i;

    T_BEGIN("io_uring advertises vector transfers");
    CHECK(io->readv != NULL);
    CHECK(io->writev != NULL);

    T_BEGIN("io_uring batched writes land in one submission");
    for (i = 0; i < UTEST_BATCH_SEGS; i++) {
        memset(wbufs + (size_t)i * 4096, 0x10 + i, 4096);
        wsegs[i].buf = wbufs + (size_t)i * 4096;
        wsegs[i].off = (uint64_t)i * 4096;
        wsegs[i].len = 4096;
        wsegs[i].res = -1;
    }
    CHECK_EQ_U64(io->writev(io, wsegs, UTEST_BATCH_SEGS), 0);
    for (i = 0; i < UTEST_BATCH_SEGS; i++)
        CHECK_EQ_U64(wsegs[i].res, 0);

    T_BEGIN("io_uring batch larger than the ring submits in waves");
    for (i = 0; i < UTEST_BATCH_SEGS; i++) {
        rsegs[i].buf = rbufs + (size_t)i * 4096;
        rsegs[i].off = (uint64_t)i * 4096;
        rsegs[i].len = 4096;
        rsegs[i].res = -1;
    }
    CHECK_EQ_U64(io->readv(io, rsegs, UTEST_BATCH_SEGS), 0);
    for (i = 0; i < UTEST_BATCH_SEGS; i++) {
        CHECK_EQ_U64(rsegs[i].res, 0);
        if (memcmp(wbufs + (size_t)i * 4096, rbufs + (size_t)i * 4096, 4096) != 0)
            T_FAIL("batch segment %d mismatch", i);
    }

    T_BEGIN("io_uring batch mixes aligned and unaligned segments");
    memset(wbufs, 0x77, 64);
    wsegs[0].buf = wbufs;
    wsegs[0].off = 100;
    wsegs[0].len = 64;
    wsegs[0].res = -1;
    wsegs[1].buf = wbufs + 4096;
    wsegs[1].off = 8192;
    wsegs[1].len = 4096;
    wsegs[1].res = -1;
    wsegs[2].buf = wbufs + 8192;
    wsegs[2].off = 9000;
    wsegs[2].len = 0;
    wsegs[2].res = -1;
    CHECK_EQ_U64(io->writev(io, wsegs, 3), 0);
    CHECK_EQ_U64(wsegs[0].res, 0);
    CHECK_EQ_U64(wsegs[1].res, 0);
    CHECK_EQ_U64(wsegs[2].res, 0);
    memset(rbufs, 0, 4096);
    rsegs[0].buf = rbufs;
    rsegs[0].off = 100;
    rsegs[0].len = 64;
    rsegs[0].res = -1;
    rsegs[1].buf = rbufs + 4096;
    rsegs[1].off = 8192;
    rsegs[1].len = 4096;
    rsegs[1].res = -1;
    CHECK_EQ_U64(io->readv(io, rsegs, 2), 0);
    CHECK_EQ_U64(rsegs[0].res, 0);
    CHECK_EQ_U64(rsegs[1].res, 0);
    CHECK_EQ_U64(memcmp(wbufs, rbufs, 64), 0);
    CHECK_EQ_U64(memcmp(wbufs + 4096, rbufs + 4096, 4096), 0);

    T_BEGIN("io_uring batch reports per-segment failures");
    before = *io->stats;
    rsegs[0].buf = rbufs;
    rsegs[0].off = UTEST_SIZE - 16;
    rsegs[0].len = 4096;
    rsegs[0].res = -1;
    rsegs[1].buf = rbufs + 4096;
    rsegs[1].off = 0;
    rsegs[1].len = 4096;
    rsegs[1].res = -1;
    CHECK_EQ_U64(io->readv(io, rsegs, 2), (uint64_t)-1);
    CHECK(rsegs[0].res < 0);
    CHECK_EQ_U64(rsegs[1].res, 0);
    CHECK(io->stats->io_errors > before.io_errors);

    free(wbufs);
    free(rbufs);
}

static void test_uring_pipeline(const char *path)
{
    io_ops *io;
    pipeline p;
    stage_stats st;
    char err[128];
    uint64_t chunk = 256u << 10;
    int e = 0;
    int i;

    e = 0;
    io = io_uring_open(path, 1, 0, UTEST_SIZE, &e);
    if (io == NULL) {
        if (e != ENOTSUP)
            T_FAIL("io_uring_open failed: %s", errno_str(e));
        return;
    }

    T_BEGIN("pipeline write-behind queue runs over the uring backend");
    CHECK_EQ_U64(pipeline_init(&p, io, chunk, PAT_CHACHA20, 77, UTEST_SIZE, 4, err, sizeof err),
                 0);
    stage_stats_reset(&st);
    for (i = 0; i < 4; i++) {
        if (pipeline_write(&p, (uint64_t)i * chunk, 1, &st) != 0) {
            T_FAIL("pipeline_write %d failed", i);
            break;
        }
    }
    CHECK_EQ_U64(st.chunks_written, 4);
    CHECK_EQ_U64(pipeline_flush(&p, &st), 0);
    CHECK_EQ_U64(st.chunks_written, 4);

    T_BEGIN("uring read-back detects a byte corrupted after the write");
    {
        int fd = open(path, O_RDWR);
        off_t off = (off_t)(3 * chunk + 100);
        uint8_t b = 0;

        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(pread(fd, &b, 1, off) == 1);
            b ^= 0xff;
            CHECK(pwrite(fd, &b, 1, off) == 1);
            close(fd);
        }
    }
    stage_stats_reset(&st);
    CHECK_EQ_U64(pipeline_verify(&p, 0, 1, &st), 0);
    CHECK_EQ_U64(pipeline_verify(&p, chunk, 1, &st), 0);
    CHECK_EQ_U64(pipeline_verify(&p, 2 * chunk, 1, &st), 0);
    CHECK_EQ_U64(pipeline_verify(&p, 3 * chunk, 1, &st), 1);
    CHECK_EQ_U64(st.chunks_verified, 4);
    CHECK_EQ_U64(st.chunks_failed, 1);
    CHECK_EQ_U64(st.first_fail_off, 3 * chunk + 100);
    pipeline_free(&p);
    io->close(io);
}

void test_uring(void)
{
    char path[64];
    uint8_t rbuf[4096];
    io_ops *io;
    int e = 0;

    if (make_temp_file(path, sizeof path) != 0) {
        T_FAIL("cannot create temp file for io_uring test");
        return;
    }

    io = io_uring_open(path, 1, 0, UTEST_SIZE, &e);
    if (io == NULL) {
        if (e == ENOTSUP) {
            printf("    [skip] io_uring not supported on this kernel\n");
            unlink(path);
            return;
        }
        T_FAIL("io_uring_open failed: %s", errno_str(e));
        unlink(path);
        return;
    }

    T_BEGIN("io_uring backend identity, buffered mode");
    CHECK_STR(io->name, "io_uring");
    CHECK(io->stats != NULL);
    CHECK_EQ_U64(io->stats->capacity, UTEST_SIZE);
    CHECK_EQ_U64(io->stats->direct_requested, 0);
    CHECK_EQ_U64(io->stats->direct_active, 0);

    test_uring_roundtrip(io);
    test_uring_batch(io);

    T_BEGIN("io_uring sees data written through the page cache path");
    {
        int fd = open(path, O_RDWR);
        uint8_t direct[512];

        CHECK(fd >= 0);
        memset(direct, 0x5a, sizeof direct);
        CHECK(pwrite(fd, direct, sizeof direct, 4096) == (ssize_t)sizeof direct);
        close(fd);
        memset(rbuf, 0, sizeof rbuf);
        CHECK_EQ_U64(io->read(io, rbuf, 4096, sizeof direct), 0);
        CHECK_EQ_U64(rbuf[0], 0x5a);
        CHECK_EQ_U64(rbuf[sizeof direct - 1], 0x5a);
    }

    io->close(io);

    T_BEGIN("io_uring O_DIRECT request is honoured or degraded, never fatal");
    e = 0;
    io = io_uring_open(path, 1, 1, UTEST_SIZE, &e);
    if (io == NULL) {
        T_FAIL("io_uring_open(want_direct=1) failed: %s", errno_str(e));
        unlink(path);
        return;
    }
    CHECK_STR(io->name, "io_uring");
    CHECK_EQ_U64(io->stats->direct_requested, 1);
    CHECK(io->stats->direct_active == 0 || io->stats->direct_active == 1);
    printf("    [info] O_DIRECT active: %s\n", io->stats->direct_active ? "yes" : "no");

    test_uring_roundtrip(io);
    test_uring_batch(io);

    io->close(io);

    test_uring_pipeline(path);
    unlink(path);
}
