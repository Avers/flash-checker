#include "flashcheck/io.h"

#include "flashcheck/util.h"

typedef struct {
    io_ops ops;
    uint8_t *mem;
    uint64_t real;
    uint64_t reported;
    int policy;
} io_fake;

int fake_policy_parse(const char *s, int *out)
{
    if (strcmp(s, "honest") == 0)
        *out = FAKE_HONEST;
    else if (strcmp(s, "alias") == 0)
        *out = FAKE_ALIAS;
    else if (strcmp(s, "stale") == 0)
        *out = FAKE_STALE;
    else if (strcmp(s, "unwritten") == 0)
        *out = FAKE_UNWRITTEN;
    else if (strcmp(s, "error") == 0)
        *out = FAKE_ERROR;
    else if (strcmp(s, "slow") == 0)
        *out = FAKE_SLOW;
    else
        return -1;
    return 0;
}

const char *fake_policy_name(int pol)
{
    switch (pol) {
    case FAKE_ALIAS:
        return "alias";
    case FAKE_STALE:
        return "stale";
    case FAKE_UNWRITTEN:
        return "unwritten";
    case FAKE_ERROR:
        return "error";
    case FAKE_SLOW:
        return "slow";
    default:
        return "honest";
    }
}

static uint64_t fake_map(io_fake *f, uint64_t off)
{
    if (off >= f->real) {
        if (f->policy == FAKE_ALIAS)
            return off % f->real;
        if (f->policy == FAKE_ERROR)
            return f->real;
        return off;
    }
    return off;
}

static int fake_read(io_ops *o, void *buf, uint64_t off, size_t len)
{
    io_fake *f = (io_fake *)o;

    if (off + len > f->reported || off + len < off) {
        o->stats->io_errors++;
        return -EINVAL;
    }
    if (f->policy == FAKE_ERROR && off + len > f->real) {
        o->stats->io_errors++;
        return -EIO;
    }
    if (f->policy == FAKE_SLOW)
        sleep_ms(1);
    if (f->policy == FAKE_UNWRITTEN && off < f->real) {
        memset(buf, 0xa5, len);
        return 0;
    }
    off = fake_map(f, off);
    if (off + len > f->real) {
        memset(buf, 0, len);
        return 0;
    }
    memcpy(buf, f->mem + off, len);
    return 0;
}

static int fake_write(io_ops *o, const void *buf, uint64_t off, size_t len)
{
    io_fake *f = (io_fake *)o;

    if (off + len > f->reported || off + len < off) {
        o->stats->io_errors++;
        return -EINVAL;
    }
    if (f->policy == FAKE_ERROR && off + len > f->real) {
        o->stats->io_errors++;
        return -EIO;
    }
    if (f->policy == FAKE_SLOW)
        sleep_ms(1);
    if (f->policy == FAKE_STALE && off + len > f->real)
        return 0;
    off = fake_map(f, off);
    if (off + len > f->real)
        return 0;
    memcpy(f->mem + off, buf, len);
    return 0;
}

static int fake_flush(io_ops *o)
{
    (void)o;
    return 0;
}

static void fake_close(io_ops *o)
{
    io_fake *f = (io_fake *)o;
    free(f->mem);
    free(f->ops.stats);
    free(f);
}

io_ops *io_fake_open(uint64_t reported, uint64_t real, int policy, int *err)
{
    io_fake *f;

    if (real == 0 || real > reported) {
        *err = EINVAL;
        return NULL;
    }
    f = xalloc(sizeof *f);
    f->mem = xalloc((size_t)real);
    if (policy == FAKE_UNWRITTEN)
        memset(f->mem, 0xa5, (size_t)real);
    f->real = real;
    f->reported = reported;
    f->policy = policy;
    f->ops.read = fake_read;
    f->ops.write = fake_write;
    f->ops.flush = fake_flush;
    f->ops.close = fake_close;
    f->ops.stats = xalloc(sizeof *f->ops.stats);
    f->ops.stats->capacity = reported;
    f->ops.name = "fake";
    return &f->ops;
}
