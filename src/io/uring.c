#include "flashcheck/io.h"

#ifdef __linux__

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "flashcheck/util.h"

#ifndef IORING_SETUP_SQPOLL
#define IORING_SETUP_SQPOLL (1 << 5)
#endif

#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS (1 << 0)
#endif

#define IORING_OFF_SQ_RING 0x00000000
#define IORING_OFF_CQ_RING 0x08000000
#define IORING_OFF_SQES 0x10000000

struct io_uring_sq {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    uint32_t *flags;
    uint32_t *dropped;
    uint32_t *array;
    uint32_t *resv1;
    uint32_t *resv2;
};

struct io_uring_cq {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    uint32_t *overflow;
    struct io_uring_cqe *cqes;
};

struct io_uring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

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
    union {
        uint16_t buf_index;
        uint16_t buf_group;
        uint16_t pad;
    };
    int32_t personality;
    uint32_t splice_fd_in;
};

#define IORING_OP_READV 7
#define IORING_OP_WRITEV 9
#define IORING_OP_FSYNC 10

#define IORING_FSYNC_DATASYNC 1

typedef struct {
    io_ops ops;
    int fd;
    int ring_fd;
    void *sq_ring_ptr;
    void *cq_ring_ptr;
    struct io_uring_sqe *sqes;
    struct io_uring_sq sq;
    struct io_uring_cq cq;
    uint32_t ring_entries;
    int writable;
} io_uring;

static int io_uring_setup_syscall(unsigned entries, void *params) {
    return syscall(__NR_io_uring_setup, entries, params);
}

static int io_uring_enter_syscall(int ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags, sigset_t *sig) {
    return syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, sig);
}

static int uring_mmap_rings(io_uring *u, void *params) {
    struct {
        uint32_t sq_off;
        uint32_t cq_off;
        uint32_t sqes_off;
    } *p = params;

    u->sq_ring_ptr = mmap(NULL, p->sq_off + p->cq_off,
                          PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          u->ring_fd, IORING_OFF_SQ_RING);
    if (u->sq_ring_ptr == MAP_FAILED)
        return -1;

    u->cq_ring_ptr = (char *)u->sq_ring_ptr + p->cq_off;

    u->sqes = mmap(NULL, u->ring_entries * sizeof(struct io_uring_sqe),
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   u->ring_fd, IORING_OFF_SQES);
    if (u->sqes == MAP_FAILED)
        return -1;

    return 0;
}

static void uring_init_sq_cq(io_uring *u, void *params) {
    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t flags;
        uint32_t dropped;
        uint32_t array;
        uint32_t resv1;
        uint32_t resv2;
    } *sq = params;

    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t overflow;
        uint32_t cqes;
        uint32_t resv[3];
    } *cq = (char *)params + 64;

    u->sq.head = (uint32_t *)((char *)u->sq_ring_ptr + sq->head);
    u->sq.tail = (uint32_t *)((char *)u->sq_ring_ptr + sq->tail);
    u->sq.ring_mask = (uint32_t *)((char *)u->sq_ring_ptr + sq->ring_mask);
    u->sq.ring_entries = (uint32_t *)((char *)u->sq_ring_ptr + sq->ring_entries);
    u->sq.flags = (uint32_t *)((char *)u->sq_ring_ptr + sq->flags);
    u->sq.dropped = (uint32_t *)((char *)u->sq_ring_ptr + sq->dropped);
    u->sq.array = (uint32_t *)((char *)u->sq_ring_ptr + sq->array);

    u->cq.head = (uint32_t *)((char *)u->cq_ring_ptr + cq->head);
    u->cq.tail = (uint32_t *)((char *)u->cq_ring_ptr + cq->tail);
    u->cq.ring_mask = (uint32_t *)((char *)u->cq_ring_ptr + cq->ring_mask);
    u->cq.ring_entries = (uint32_t *)((char *)u->cq_ring_ptr + cq->ring_entries);
    u->cq.overflow = (uint32_t *)((char *)u->cq_ring_ptr + cq->overflow);
    u->cq.cqes = (struct io_uring_cqe *)((char *)u->cq_ring_ptr + cq->cqes);
}

static int uring_submit(io_uring *u, unsigned count) {
    unsigned head = *u->sq.tail;
    for (unsigned i = 0; i < count; i++) {
        u->sq.array[head & *u->sq.ring_mask] = i;
        head++;
    }
    *u->sq.tail = head;

    return io_uring_enter_syscall(u->ring_fd, count, 0, 0, NULL);
}

static int uring_wait_cqe(io_uring *u, struct io_uring_cqe **cqe) {
    while (*u->cq.head == *u->cq.tail) {
        int ret = io_uring_enter_syscall(u->ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
        if (ret < 0)
            return ret;
    }

    *cqe = &u->cq.cqes[*u->cq.head & *u->cq.ring_mask];
    return 0;
}

static void uring_seen_cqe(io_uring *u) {
    (*u->cq.head)++;
}

static int uring_prep_rw(struct io_uring_sqe *sqe, int op, int fd, void *buf,
                         size_t len, uint64_t off, uint64_t user_data) {
    sqe->opcode = op;
    sqe->flags = 0;
    sqe->ioprio = 0;
    sqe->fd = fd;
    sqe->off = off;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = len;
    sqe->rw_flags = 0;
    sqe->user_data = user_data;
    sqe->buf_index = 0;
    sqe->personality = 0;
    sqe->splice_fd_in = 0;
    return 0;
}

static int uring_prep_fsync(struct io_uring_sqe *sqe, int fd, uint64_t user_data) {
    sqe->opcode = IORING_OP_FSYNC;
    sqe->flags = 0;
    sqe->ioprio = 0;
    sqe->fd = fd;
    sqe->off = 0;
    sqe->addr = 0;
    sqe->len = 0;
    sqe->fsync_flags = IORING_FSYNC_DATASYNC;
    sqe->user_data = user_data;
    return 0;
}

static int uring_read(io_ops *o, void *buf, uint64_t off, size_t len) {
    io_uring *u = (io_uring *)o;
    struct io_uring_sqe *sqe = &u->sqes[0];
    struct io_uring_cqe *cqe;
    int ret;

    uring_prep_rw(sqe, IORING_OP_READV, u->fd, buf, len, off, 1);
    ret = uring_submit(u, 1);
    if (ret < 0)
        return ret;

    ret = uring_wait_cqe(u, &cqe);
    if (ret < 0)
        return ret;

    ret = cqe->res;
    uring_seen_cqe(u);

    if (ret < 0)
        return ret;
    if ((size_t)ret != len)
        return -EIO;
    return 0;
}

static int uring_write(io_ops *o, const void *buf, uint64_t off, size_t len) {
    io_uring *u = (io_uring *)o;
    struct io_uring_sqe *sqe = &u->sqes[0];
    struct io_uring_cqe *cqe;
    int ret;

    uring_prep_rw(sqe, IORING_OP_WRITEV, u->fd, (void *)(uintptr_t)buf, len, off, 1);
    ret = uring_submit(u, 1);
    if (ret < 0)
        return ret;

    ret = uring_wait_cqe(u, &cqe);
    if (ret < 0)
        return ret;

    ret = cqe->res;
    uring_seen_cqe(u);

    if (ret < 0)
        return ret;
    if ((size_t)ret != len)
        return -EIO;
    return 0;
}

static int uring_flush(io_ops *o) {
    io_uring *u = (io_uring *)o;
    struct io_uring_sqe *sqe = &u->sqes[0];
    struct io_uring_cqe *cqe;
    int ret;

    uring_prep_fsync(sqe, u->fd, 2);
    ret = uring_submit(u, 1);
    if (ret < 0)
        return ret;

    ret = uring_wait_cqe(u, &cqe);
    if (ret < 0)
        return ret;

    ret = cqe->res;
    uring_seen_cqe(u);
    return ret < 0 ? ret : 0;
}

static void uring_close(io_ops *o) {
    io_uring *u = (io_uring *)o;
    if (u->sqes)
        munmap(u->sqes, u->ring_entries * sizeof(struct io_uring_sqe));
    if (u->sq_ring_ptr)
        munmap(u->sq_ring_ptr, 0);
    if (u->ring_fd >= 0)
        close(u->ring_fd);
    if (u->fd >= 0)
        close(u->fd);
    free(u->ops.stats);
    free(u);
}

static int uring_probe_supported(void) {
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    struct {
        uint32_t sq_entries;
        uint32_t cq_entries;
        uint32_t flags;
        uint32_t sq_thread_cpu;
        uint32_t sq_thread_idle;
        uint32_t features;
        uint32_t resv[4];
    } params = {0};

    params.sq_entries = 8;
    params.cq_entries = 8;
    params.flags = 0;

    int ring_fd = io_uring_setup_syscall(8, &params);
    if (ring_fd >= 0) {
        close(ring_fd);
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

io_ops *io_uring_open(const char *path, int writable, uint64_t capacity, int *err) {
    if (!uring_probe_supported()) {
        *err = ENOTSUP;
        return NULL;
    }

    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOCTTY;
    int fd = open(path, flags);
    if (fd < 0) {
        *err = errno;
        return NULL;
    }

    struct {
        uint32_t sq_entries;
        uint32_t cq_entries;
        uint32_t flags;
        uint32_t sq_thread_cpu;
        uint32_t sq_thread_idle;
        uint32_t features;
        uint32_t resv[4];
    } params = {0};

    params.sq_entries = 32;
    params.cq_entries = 32;
    params.flags = 0;

    int ring_fd = io_uring_setup_syscall(32, &params);
    if (ring_fd < 0) {
        *err = errno;
        close(fd);
        return NULL;
    }

    if (uring_mmap_rings(NULL, &params) != 0) {
        close(ring_fd);
        close(fd);
        *err = errno;
        return NULL;
    }

    io_uring *u = xalloc(sizeof *u);
    u->fd = fd;
    u->ring_fd = ring_fd;
    u->writable = writable;
    u->ring_entries = params.sq_entries;

    if (uring_mmap_rings(u, &params) != 0) {
        uring_close(&u->ops);
        *err = errno;
        return NULL;
    }
    uring_init_sq_cq(u, &params);

    u->ops.read = uring_read;
    u->ops.write = uring_write;
    u->ops.flush = uring_flush;
    u->ops.close = uring_close;
    u->ops.stats = xalloc(sizeof *u->ops.stats);
    u->ops.stats->direct_requested = 1;
    u->ops.stats->direct_active = 1;
    u->ops.stats->capacity = capacity;
    u->ops.name = "io_uring";

    return &u->ops;
}

void io_uring_probe(const char *path, int *supported, int *err) {
    (void)path;
    *supported = uring_probe_supported();
    *err = 0;
}

#else

io_ops *io_uring_open(const char *path, int writable, uint64_t capacity, int *err) {
    (void)path;
    (void)writable;
    (void)capacity;
    *err = ENOTSUP;
    return NULL;
}

void io_uring_probe(const char *path, int *supported, int *err) {
    (void)path;
    *supported = 0;
    *err = 0;
}

#endif