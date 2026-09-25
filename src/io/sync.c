#include "flashcheck/io.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "flashcheck/util.h"

#define SYNC_MAX_RETRY 4
#define SYNC_SECTOR 512

typedef struct {
    io_ops ops;
    int fd;
    int fd_buf;
    int writable;
} io_sync;

static int loop_xfer(int fd, void *buf, uint64_t off, size_t len, int is_write)
{
    uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n;
        int attempt = 0;

        for (;;) {
            if (is_write)
                n = pwrite(fd, p + done, len - done, (off_t)(off + done));
            else
                n = pread(fd, p + done, len - done, (off_t)(off + done));
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        if (n < 0) {
            int e = errno;
            if ((e == EIO || e == ETIMEDOUT || e == EBUSY) && attempt + 1 < SYNC_MAX_RETRY) {
                sleep_ms(20u * (unsigned)(attempt + 1));
                attempt++;
                continue;
            }
            if (e == EINVAL && is_write)
                return -EINVAL;
            return -e;
        }
        if (n == 0)
            return -EIO;
        p += n;
        done += (size_t)n;
    }
    return 0;
}

static int can_direct(io_sync *s, uint64_t off, size_t len)
{
    if (!s->ops.stats->direct_active)
        return 0;
    if (s->fd_buf < 0)
        return 0;
    if (off % SYNC_SECTOR != 0 || len % SYNC_SECTOR != 0)
        return 0;
    if (s->ops.stats->capacity != 0 && off + len > s->ops.stats->capacity)
        return 0;
    return 1;
}

static int sync_read(io_ops *o, void *buf, uint64_t off, size_t len)
{
    io_sync *s = (io_sync *)o;
    int r = loop_xfer(can_direct(s, off, len) ? s->fd : s->fd_buf, buf, off, len, 0);
    if (r < 0)
        o->stats->io_errors++;
    return r;
}

static int sync_write(io_ops *o, const void *buf, uint64_t off, size_t len)
{
    io_sync *s = (io_sync *)o;
    int r = loop_xfer(can_direct(s, off, len) ? s->fd : s->fd_buf, (void *)(uintptr_t)buf,
                      off, len, 1);
    if (r < 0)
        o->stats->io_errors++;
    return r;
}

static int sync_flush(io_ops *o)
{
    io_sync *s = (io_sync *)o;
    int r = 0;
    if (s->writable) {
        if (fdatasync(s->fd) != 0)
            r = -errno;
        if (s->fd_buf >= 0 && s->fd_buf != s->fd && fdatasync(s->fd_buf) != 0)
            r = -errno;
    }
    return r;
}

static void sync_close(io_ops *o)
{
    io_sync *s = (io_sync *)o;
    if (s->fd >= 0)
        close(s->fd);
    if (s->fd_buf >= 0 && s->fd_buf != s->fd)
        close(s->fd_buf);
    free(s);
}

static int open_raw(const char *path, int writable, int direct, int *err)
{
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOCTTY;
    int fd;

    if (direct) {
#ifdef __linux__
        flags |= O_DIRECT;
#endif
    }
    fd = open(path, flags);
    if (fd < 0 && direct && (errno == EINVAL || errno == ENOTSUP || errno == EPERM)) {
        *err = errno;
        return -1;
    }
    if (fd < 0) {
        *err = errno;
        return -1;
    }
    return fd;
}

io_ops *io_sync_open(const char *path, int writable, int want_direct, uint64_t capacity,
                     int *err)
{
    io_sync *s;
    int fd = -1, fd_buf = -1, e = 0;

    fd = open_raw(path, writable, want_direct, &e);
    if (fd < 0) {
        fd = open_raw(path, writable, 0, &e);
        if (fd < 0) {
            *err = e;
            return NULL;
        }
    }
    if (want_direct) {
        fd_buf = open_raw(path, writable, 0, &e);
        if (fd_buf < 0)
            fd_buf = -1;
    }

    s = xalloc(sizeof *s);
    s->fd = fd;
    s->fd_buf = fd_buf;
    s->writable = writable;
    s->ops.read = sync_read;
    s->ops.write = sync_write;
    s->ops.flush = sync_flush;
    s->ops.close = sync_close;
    s->ops.stats = xalloc(sizeof *s->ops.stats);
    s->ops.stats->direct_requested = want_direct;
    s->ops.stats->direct_active = want_direct && fd_buf >= 0;
    s->ops.stats->capacity = capacity;
    s->ops.name = "sync";
    return &s->ops;
}

void io_sync_probe_direct(const char *path, int *supported, int *err)
{
    int fd = open_raw(path, O_RDONLY, 1, err);
    if (fd < 0) {
        *supported = 0;
        return;
    }
    close(fd);
    *supported = 1;
}
