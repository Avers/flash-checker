#ifndef FLASHCHECK_IO_H
#define FLASHCHECK_IO_H

#include "flashcheck/common.h"

typedef struct io_ops io_ops;

typedef struct {
    int direct_requested;
    int direct_active;
    uint64_t capacity;
    uint64_t retries;
    uint64_t io_errors;
} io_stats;

struct io_ops {
    int (*read)(io_ops *o, void *buf, uint64_t off, size_t len);
    int (*write)(io_ops *o, const void *buf, uint64_t off, size_t len);
    int (*flush)(io_ops *o);
    void (*close)(io_ops *o);
    io_stats *stats;
    const char *name;
};

io_ops *io_sync_open(const char *path, int writable, int want_direct, uint64_t capacity,
                     int *err);
void io_sync_probe_direct(const char *path, int *supported, int *err);

io_ops *io_uring_open(const char *path, int writable, uint64_t capacity, int *err);
void io_uring_probe(const char *path, int *supported, int *err);

#define FAKE_HONEST 0
#define FAKE_ALIAS 1
#define FAKE_STALE 2
#define FAKE_UNWRITTEN 3
#define FAKE_ERROR 4
#define FAKE_SLOW 5

int fake_policy_parse(const char *s, int *out);
const char *fake_policy_name(int pol);
io_ops *io_fake_open(uint64_t reported, uint64_t real, int policy, int *err);

#endif
