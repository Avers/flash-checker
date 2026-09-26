#include "flashcheck/io.h"
#include "flashcheck/util.h"
#include "test_util.h"

#include <fcntl.h>
#include <unistd.h>

#define UTEST_SIZE (1u << 20)

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

void test_uring(void)
{
    char path[64];
    uint8_t wbuf[4096];
    uint8_t rbuf[4096];
    io_ops *io;
    int e = 0;
    int i;

    if (make_temp_file(path, sizeof path) != 0) {
        T_FAIL("cannot create temp file for io_uring test");
        return;
    }

    io = io_uring_open(path, 1, UTEST_SIZE, &e);
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

    T_BEGIN("io_uring backend identity");
    CHECK_STR(io->name, "io_uring");
    CHECK(io->stats != NULL);
    CHECK_EQ_U64(io->stats->capacity, UTEST_SIZE);

    T_BEGIN("io_uring write/read round trip");
    memset(wbuf, 0xa5, sizeof wbuf);
    CHECK_EQ_U64(io->write(io, wbuf, 8192, sizeof wbuf), 0);
    memset(rbuf, 0, sizeof rbuf);
    CHECK_EQ_U64(io->read(io, rbuf, 8192, sizeof rbuf), 0);
    CHECK_EQ_U64(memcmp(wbuf, rbuf, sizeof wbuf), 0);

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

    T_BEGIN("io_uring read past end of file fails");
    CHECK(io->read(io, rbuf, UTEST_SIZE - 16, sizeof rbuf) != 0);
    CHECK(io->stats->io_errors > 0);

    T_BEGIN("io_uring repeated ops reuse the ring");
    for (i = 0; i < 100; i++) {
        memset(wbuf, (uint8_t)i, sizeof wbuf);
        if (io->write(io, wbuf, 0, sizeof wbuf) != 0) {
            T_FAIL("write %d failed", i);
            break;
        }
        memset(rbuf, 0, sizeof rbuf);
        if (io->read(io, rbuf, 0, sizeof rbuf) != 0) {
            T_FAIL("read %d failed", i);
            break;
        }
        if (memcmp(wbuf, rbuf, sizeof wbuf) != 0) {
            T_FAIL("mismatch at iteration %d", i);
            break;
        }
    }

    T_BEGIN("io_uring flush");
    CHECK_EQ_U64(io->flush(io), 0);

    io->close(io);
    unlink(path);
}
