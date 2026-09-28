#include "flashcheck/io.h"
#include "flashcheck/util.h"
#include "test_util.h"

#include <fcntl.h>
#include <unistd.h>

#define STEST_SIZE (1u << 20)

static int make_temp_file(char *path, size_t n)
{
    int fd;

    snprintf(path, n, "/tmp/flashcheck-sync-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, STEST_SIZE) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

void test_sync(void)
{
    char path[64];
    io_ops *io;
    uint8_t *wbuf = xalloc_aligned(4096, 4096);
    uint8_t *rbuf = xalloc_aligned(4096, 4096);
    int e = 0;

    T_BEGIN("sync buffered round trip with want_direct=0");
    if (make_temp_file(path, sizeof path) != 0) {
        T_FAIL("cannot create temp file");
        return;
    }
    io = io_sync_open(path, 1, 0, STEST_SIZE, &e);
    if (io == NULL) {
        T_FAIL("io_sync_open(want_direct=0) failed: %s", errno_str(e));
        unlink(path);
        return;
    }
    CHECK_EQ_U64(io->stats->direct_requested, 0);
    CHECK_EQ_U64(io->stats->direct_active, 0);

    memset(wbuf, 0x5a, 4096);
    CHECK_EQ_U64(io->write(io, wbuf, 0, 4096), 0);
    memset(rbuf, 0, 4096);
    CHECK_EQ_U64(io->read(io, rbuf, 0, 4096), 0);
    CHECK(memcmp(wbuf, rbuf, 4096) == 0);
    CHECK_EQ_U64(io->stats->io_errors, 0);

    T_BEGIN("sync buffered handles an unaligned offset");
    memset(wbuf, 0x3c, 100);
    CHECK_EQ_U64(io->write(io, wbuf, 4196, 100), 0);
    memset(rbuf, 0, 4096);
    CHECK_EQ_U64(io->read(io, rbuf, 4196, 100), 0);
    CHECK(memcmp(wbuf, rbuf, 100) == 0);
    CHECK_EQ_U64(io->stats->io_errors, 0);

    T_BEGIN("sync buffered flush");
    CHECK_EQ_U64(io->flush(io), 0);
    io->close(io);

    T_BEGIN("sync buffered read past end of file fails");
    e = 0;
    io = io_sync_open(path, 0, 0, STEST_SIZE, &e);
    if (io == NULL) {
        T_FAIL("re-open for reading failed: %s", errno_str(e));
    } else {
        CHECK(io->read(io, rbuf, STEST_SIZE - 16, 4096) != 0);
        CHECK(io->stats->io_errors > 0);
        io->close(io);
    }

#ifdef __linux__
    T_BEGIN("sync want_direct=1 on a device that rejects O_DIRECT");
    e = 0;
    io = io_sync_open("/dev/null", 1, 1, 4096, &e);
    if (io == NULL) {
        T_FAIL("io_sync_open(want_direct=1) on /dev/null failed: %s", errno_str(e));
    } else {
        CHECK_EQ_U64(io->stats->direct_requested, 1);
        CHECK_EQ_U64(io->stats->direct_active, 0);
        CHECK_EQ_U64(io->write(io, wbuf, 0, 4096), 0);
        CHECK_EQ_U64(io->stats->io_errors, 0);
        io->close(io);
    }
#endif

    unlink(path);
    free(wbuf);
    free(rbuf);
}
