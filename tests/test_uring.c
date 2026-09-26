#include "flashcheck/io.h"
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

    io->close(io);
    unlink(path);
}
