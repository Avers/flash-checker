#include "flashcheck/io.h"

#ifdef __linux__

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "flashcheck/util.h"

#define IORING_OFF_SQ_RING 0x00000000ULL
#define IORING_OFF_CQ_RING 0x08000000ULL
#define IORING_OFF_SQES 0x10000000ULL

#define IORING_ENTER_GETEVENTS (1u << 0)
#define IORING_FEAT_SINGLE_MMAP (1u << 0)

#define IORING_OP_FSYNC 3
#define IORING_OP_READ 22
#define IORING_OP_WRITE 23

#define IORING_FSYNC_DATASYNC 1u

#define URING_ENTRIES 32
#define URING_MAX_RETRY 4
#define URING_SECTOR 512
#define URING_MAX_SLICE 64

struct io_uring_sqe {
    uint8_t opcode;
    uint8_t flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    union {
        uint32_t rw_flags;
        uint32_t fsync_flags;
    };
    uint64_t user_data;
    uint16_t buf_index;
    uint16_t personality;
    uint32_t splice_fd_in;
    uint64_t __pad2[2];
};

struct io_uring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

struct io_sqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t flags;
    uint32_t dropped;
    uint32_t array;
    uint32_t resv1;
    uint64_t user_addr;
};

struct io_cqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t overflow;
    uint32_t cqes;
    uint32_t flags;
    uint32_t resv1;
    uint64_t user_addr;
};

struct io_uring_params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct io_sqring_offsets sq_off;
    struct io_cqring_offsets cq_off;
};

_Static_assert(sizeof(struct io_uring_sqe) == 64, "io_uring_sqe must match kernel layout");
_Static_assert(sizeof(struct io_uring_cqe) == 16, "io_uring_cqe must match kernel layout");
_Static_assert(sizeof(struct io_sqring_offsets) == 40, "io_sqring_offsets layout");
_Static_assert(sizeof(struct io_cqring_offsets) == 40, "io_cqring_offsets layout");
_Static_assert(sizeof(struct io_uring_params) == 120, "io_uring_params must match kernel layout");
_Static_assert(offsetof(struct io_uring_params, sq_off) == 40, "sq_off offset");
_Static_assert(offsetof(struct io_uring_params, cq_off) == 80, "cq_off offset");

struct io_uring_sq {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    uint32_t *flags;
    uint32_t *dropped;
    uint32_t *array;
};

struct io_uring_cq {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    uint32_t *overflow;
    struct io_uring_cqe *cqes;
};

typedef struct {
    io_ops ops;
    int fd;
    int fd_buf;
    int ring_fd;
    void *sq_ring_ptr;
    size_t sq_ring_sz;
    void *cq_ring_ptr;
    size_t cq_ring_sz;
    struct io_uring_sqe *sqes;
    size_t sqes_sz;
    struct io_uring_sq sq;
    struct io_uring_cq cq;
    uint32_t ring_entries;
    int writable;
    int err;
} io_uring;

static int uring_setup(unsigned entries, struct io_uring_params *params)
{
    return (int)syscall(__NR_io_uring_setup, entries, params);
}

static int uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags)
{
    int r = (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
    return r < 0 ? -errno : r;
}

static uint32_t ring_load(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void ring_store(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static int uring_mmap_rings(io_uring *u, const struct io_uring_params *params)
{
    size_t sq_sz = (size_t)params->sq_off.array +
                   (size_t)params->sq_entries * sizeof(uint32_t);
    size_t cq_sz = (size_t)params->cq_off.cqes +
                   (size_t)params->cq_entries * sizeof(struct io_uring_cqe);
    int single = (params->features & IORING_FEAT_SINGLE_MMAP) != 0;
    size_t map_sz = single ? FC_MAX(sq_sz, cq_sz) : sq_sz;

    u->sq_ring_ptr = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          u->ring_fd, IORING_OFF_SQ_RING);
    if (u->sq_ring_ptr == MAP_FAILED) {
        u->sq_ring_ptr = NULL;
        return -1;
    }
    u->sq_ring_sz = map_sz;

    if (single) {
        u->cq_ring_ptr = u->sq_ring_ptr;
        u->cq_ring_sz = 0;
    } else {
        u->cq_ring_ptr = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                              u->ring_fd, IORING_OFF_CQ_RING);
        if (u->cq_ring_ptr == MAP_FAILED) {
            u->cq_ring_ptr = NULL;
            return -1;
        }
        u->cq_ring_sz = cq_sz;
    }

    u->sqes_sz = (size_t)params->sq_entries * sizeof(struct io_uring_sqe);
    u->sqes = mmap(NULL, u->sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   u->ring_fd, IORING_OFF_SQES);
    if (u->sqes == MAP_FAILED) {
        u->sqes = NULL;
        return -1;
    }
    return 0;
}

static void uring_init_sq_cq(io_uring *u, const struct io_uring_params *params)
{
    char *sq = u->sq_ring_ptr;
    char *cq = u->cq_ring_ptr;
    const struct io_sqring_offsets *so = &params->sq_off;
    const struct io_cqring_offsets *co = &params->cq_off;

    u->sq.head = (uint32_t *)(sq + so->head);
    u->sq.tail = (uint32_t *)(sq + so->tail);
    u->sq.ring_mask = (uint32_t *)(sq + so->ring_mask);
    u->sq.ring_entries = (uint32_t *)(sq + so->ring_entries);
    u->sq.flags = (uint32_t *)(sq + so->flags);
    u->sq.dropped = (uint32_t *)(sq + so->dropped);
    u->sq.array = (uint32_t *)(sq + so->array);

    u->cq.head = (uint32_t *)(cq + co->head);
    u->cq.tail = (uint32_t *)(cq + co->tail);
    u->cq.ring_mask = (uint32_t *)(cq + co->ring_mask);
    u->cq.ring_entries = (uint32_t *)(cq + co->ring_entries);
    u->cq.overflow = (uint32_t *)(cq + co->overflow);
    u->cq.cqes = (struct io_uring_cqe *)(cq + co->cqes);
}

static int uring_wait_cqe(io_uring *u, struct io_uring_cqe **cqe)
{
    for (;;) {
        uint32_t head = ring_load(u->cq.head);
        uint32_t tail = ring_load(u->cq.tail);

        if (head != tail) {
            *cqe = &u->cq.cqes[head & ring_load(u->cq.ring_mask)];
            return 0;
        }

        int r = uring_enter(u->ring_fd, 0, 1, IORING_ENTER_GETEVENTS);
        if (r < 0 && r != -EINTR)
            return r;
    }
}

static void uring_seen_cqe(io_uring *u)
{
    ring_store(u->cq.head, ring_load(u->cq.head) + 1);
}

static int uring_submit_wait(io_uring *u, unsigned min_cq)
{
    for (;;) {
        uint32_t sqh = ring_load(u->sq.head);
        uint32_t sqt = ring_load(u->sq.tail);
        uint32_t cqh = ring_load(u->cq.head);
        uint32_t cqt = ring_load(u->cq.tail);
        unsigned to_submit = sqt - sqh;
        int r;

        if (to_submit == 0 && cqt - cqh >= min_cq)
            return 0;
        r = uring_enter(u->ring_fd, to_submit, min_cq, IORING_ENTER_GETEVENTS);
        if (r == -EINTR)
            continue;
        if (r < 0)
            return r;
        if (to_submit != 0 && ring_load(u->sq.head) == sqh)
            return -EIO;
    }
}

static int uring_transact(io_uring *u, int *res)
{
    struct io_uring_cqe *cqe;
    uint32_t mask = ring_load(u->sq.ring_mask);
    uint32_t tail = __atomic_load_n(u->sq.tail, __ATOMIC_RELAXED);
    int r;

    if (u->err != 0)
        return u->err;

    u->sq.array[tail & mask] = 0;
    __atomic_store_n(u->sq.tail, tail + 1, __ATOMIC_RELEASE);

    r = uring_submit_wait(u, 1);
    if (r < 0) {
        u->err = r;
        return r;
    }

    r = uring_wait_cqe(u, &cqe);
    if (r < 0) {
        u->err = r;
        return r;
    }

    *res = cqe->res;
    uring_seen_cqe(u);
    return 0;
}

static int uring_retryable(int e)
{
    return e == -EINTR || e == -EIO || e == -ETIMEDOUT || e == -EBUSY || e == -EAGAIN;
}

static int uring_can_direct(const io_uring *u, uint64_t off, size_t len)
{
    if (!u->ops.stats->direct_active || u->fd_buf < 0)
        return 0;
    if (off % URING_SECTOR != 0 || len % URING_SECTOR != 0)
        return 0;
    if (u->ops.stats->capacity != 0 && off + len > u->ops.stats->capacity)
        return 0;
    return 1;
}

static int uring_fd_for(const io_uring *u, uint64_t off, size_t len)
{
    if (u->fd_buf >= 0 && !uring_can_direct(u, off, len))
        return u->fd_buf;
    return u->fd;
}

static void uring_prep_rw(io_uring *u, uint32_t slot, const io_seg *s, int is_write, size_t idx)
{
    struct io_uring_sqe *sqe = &u->sqes[slot];

    memset(sqe, 0, sizeof *sqe);
    sqe->opcode = is_write ? IORING_OP_WRITE : IORING_OP_READ;
    sqe->fd = uring_fd_for(u, s->off, s->len);
    sqe->off = s->off;
    sqe->addr = (uint64_t)(uintptr_t)s->buf;
    sqe->len = (uint32_t)s->len;
    sqe->user_data = (uint64_t)idx + 1;
}

static int uring_rw_slice(io_uring *u, int is_write, io_seg *segs, size_t n)
{
    io_seg work[URING_MAX_SLICE];
    size_t pend[URING_MAX_SLICE];
    uint8_t att[URING_MAX_SLICE];
    uint8_t done[URING_MAX_SLICE];
    size_t i, j;
    int failed = 0;

    for (i = 0; i < n; i++) {
        work[i] = segs[i];
        att[i] = 0;
        done[i] = u->err != 0;
        segs[i].res = u->err;
        if (done[i])
            continue;
        if (work[i].len == 0)
            done[i] = 1;
        else if (work[i].len > UINT32_MAX) {
            segs[i].res = -EINVAL;
            done[i] = 1;
        }
    }

    for (;;) {
        uint32_t mask, tail;
        size_t wave = 0;
        int r;

        for (i = 0; i < n; i++) {
            if (done[i])
                continue;
            if (wave == (size_t)u->ring_entries)
                break;
            pend[wave++] = i;
        }
        if (wave == 0)
            break;

        mask = ring_load(u->sq.ring_mask);
        tail = ring_load(u->sq.tail);
        for (i = 0; i < wave; i++) {
            uint32_t slot = (tail + (uint32_t)i) & mask;

            u->sq.array[slot] = slot;
            uring_prep_rw(u, slot, &work[pend[i]], is_write, pend[i]);
        }
        ring_store(u->sq.tail, tail + (uint32_t)wave);

        r = uring_submit_wait(u, (unsigned)wave);
        if (r < 0) {
            u->err = r;
            for (j = 0; j < n; j++) {
                if (!done[j]) {
                    segs[j].res = r;
                    done[j] = 1;
                }
            }
            break;
        }

        for (i = 0; i < wave; i++) {
            struct io_uring_cqe *cqe;
            size_t idx;
            int res;

            r = uring_wait_cqe(u, &cqe);
            if (r < 0) {
                u->err = r;
                for (j = 0; j < n; j++) {
                    if (!done[j]) {
                        segs[j].res = r;
                        done[j] = 1;
                    }
                }
                break;
            }
            idx = (size_t)cqe->user_data - 1;
            res = cqe->res;
            uring_seen_cqe(u);

            if (idx >= n || done[idx]) {
                u->err = -EIO;
                for (j = 0; j < n; j++) {
                    if (!done[j]) {
                        segs[j].res = -EIO;
                        done[j] = 1;
                    }
                }
                break;
            }

            if (res < 0 && uring_retryable(res) && att[idx] + 1 < URING_MAX_RETRY) {
                att[idx]++;
                sleep_ms(20u * (unsigned)att[idx]);
                continue;
            }
            if (res >= 0 && (size_t)res < work[idx].len) {
                if (res > 0 && att[idx] + 1 < URING_MAX_RETRY * 2) {
                    att[idx]++;
                    work[idx].buf = (uint8_t *)work[idx].buf + res;
                    work[idx].off += (uint64_t)res;
                    work[idx].len -= (size_t)res;
                    continue;
                }
                res = -EIO;
            }
            segs[idx].res = res < 0 ? res : 0;
            done[idx] = 1;
        }
    }

    for (i = 0; i < n; i++) {
        if (segs[i].res < 0) {
            u->ops.stats->io_errors++;
            failed = 1;
        }
    }
    return failed ? -1 : 0;
}

static int uring_rw(io_uring *u, int is_write, io_seg *segs, size_t n)
{
    size_t done = 0;
    size_t i;
    int failed = 0;

    while (done < n) {
        size_t k = n - done;

        if (k > URING_MAX_SLICE)
            k = URING_MAX_SLICE;
        uring_rw_slice(u, is_write, segs + done, k);
        done += k;
    }

    for (i = 0; i < n; i++) {
        if (segs[i].res < 0)
            failed = 1;
    }
    return failed ? -1 : 0;
}

static int uring_xfer(io_uring *u, int is_write, void *buf, uint64_t off, size_t len)
{
    io_seg seg;

    if (len == 0)
        return 0;
    if (len > UINT32_MAX)
        return -EINVAL;

    seg.buf = buf;
    seg.off = off;
    seg.len = len;
    seg.res = 0;
    uring_rw(u, is_write, &seg, 1);
    return seg.res;
}

static int uring_read(io_ops *o, void *buf, uint64_t off, size_t len)
{
    return uring_xfer((io_uring *)o, 0, buf, off, len);
}

static int uring_write(io_ops *o, const void *buf, uint64_t off, size_t len)
{
    return uring_xfer((io_uring *)o, 1, (void *)(uintptr_t)buf, off, len);
}

static int uring_readv(io_ops *o, io_seg *segs, size_t n)
{
    return uring_rw((io_uring *)o, 0, segs, n);
}

static int uring_writev(io_ops *o, io_seg *segs, size_t n)
{
    return uring_rw((io_uring *)o, 1, segs, n);
}

static int uring_fsync_fd(io_uring *u, int fd)
{
    struct io_uring_sqe *sqe = u->sqes;
    int res = 0;
    int r;

    memset(sqe, 0, sizeof *sqe);
    sqe->opcode = IORING_OP_FSYNC;
    sqe->fd = fd;
    sqe->fsync_flags = IORING_FSYNC_DATASYNC;
    sqe->user_data = 2;

    r = uring_transact(u, &res);
    if (r < 0)
        return r;
    return res < 0 ? res : 0;
}

static int uring_flush(io_ops *o)
{
    io_uring *u = (io_uring *)o;
    int r;

    if (!u->writable)
        return 0;

    r = uring_fsync_fd(u, u->fd);
    if (r != 0)
        return r;
    if (u->fd_buf >= 0 && u->fd_buf != u->fd)
        return uring_fsync_fd(u, u->fd_buf);
    return 0;
}

static void uring_close(io_ops *o)
{
    io_uring *u = (io_uring *)o;

    if (u->sqes && u->sqes_sz)
        munmap(u->sqes, u->sqes_sz);
    if (u->cq_ring_ptr && u->cq_ring_ptr != u->sq_ring_ptr && u->cq_ring_sz)
        munmap(u->cq_ring_ptr, u->cq_ring_sz);
    if (u->sq_ring_ptr && u->sq_ring_sz)
        munmap(u->sq_ring_ptr, u->sq_ring_sz);
    if (u->ring_fd >= 0)
        close(u->ring_fd);
    if (u->fd >= 0)
        close(u->fd);
    if (u->fd_buf >= 0 && u->fd_buf != u->fd)
        close(u->fd_buf);
    free(u->ops.stats);
    free(u);
}

static int uring_probe_supported(void)
{
    struct io_uring_params params;
    int fd;

    memset(&params, 0, sizeof params);
    fd = uring_setup(2, &params);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int uring_open_fd(const char *path, int writable, int direct)
{
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOCTTY;

    if (direct)
        flags |= O_DIRECT;
    return open(path, flags);
}

io_ops *io_uring_open(const char *path, int writable, int want_direct, uint64_t capacity, int *err)
{
    struct io_uring_params params;
    io_uring *u;
    int fd = -1, fd_buf = -1, ring_fd, saved;
    int direct_active = 0;

    if (!uring_probe_supported()) {
        *err = ENOTSUP;
        return NULL;
    }

    if (want_direct) {
        fd = uring_open_fd(path, writable, 1);
        if (fd >= 0) {
            fd_buf = uring_open_fd(path, writable, 0);
            if (fd_buf >= 0) {
                direct_active = 1;
            } else {
                close(fd);
                fd = -1;
            }
        }
    }
    if (fd < 0)
        fd = uring_open_fd(path, writable, 0);
    if (fd < 0) {
        *err = errno;
        return NULL;
    }

    memset(&params, 0, sizeof params);
    ring_fd = uring_setup(URING_ENTRIES, &params);
    if (ring_fd < 0) {
        *err = errno;
        close(fd);
        if (fd_buf >= 0)
            close(fd_buf);
        return NULL;
    }

    u = xalloc(sizeof *u);
    u->fd = fd;
    u->fd_buf = fd_buf;
    u->ring_fd = ring_fd;
    u->writable = writable;
    u->ring_entries = params.sq_entries;
    u->ops.stats = xalloc(sizeof *u->ops.stats);
    u->ops.stats->capacity = capacity;
    u->ops.stats->direct_requested = want_direct;
    u->ops.stats->direct_active = direct_active;

    if (uring_mmap_rings(u, &params) != 0) {
        saved = errno;
        uring_close(&u->ops);
        *err = saved;
        return NULL;
    }
    uring_init_sq_cq(u, &params);

    u->ops.read = uring_read;
    u->ops.write = uring_write;
    u->ops.readv = uring_readv;
    u->ops.writev = uring_writev;
    u->ops.flush = uring_flush;
    u->ops.close = uring_close;
    u->ops.name = "io_uring";

    return &u->ops;
}

void io_uring_probe(const char *path, int *supported, int *err)
{
    (void)path;
    *supported = uring_probe_supported();
    *err = 0;
}

#else

io_ops *io_uring_open(const char *path, int writable, int want_direct, uint64_t capacity, int *err)
{
    (void)path;
    (void)writable;
    (void)want_direct;
    (void)capacity;
    *err = ENOTSUP;
    return NULL;
}

void io_uring_probe(const char *path, int *supported, int *err)
{
    (void)path;
    *supported = 0;
    *err = 0;
}

#endif
