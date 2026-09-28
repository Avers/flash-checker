#include "flashcheck/pipeline.h"

#include "flashcheck/util.h"

_Static_assert(PIPELINE_BATCH_MAX >= GEN_POOL_MAX_DEPTH,
               "write and read batches must cover the deepest generator pool");

static int drain_writes(pipeline *p, stage_stats *st);

int pipeline_init(pipeline *p, io_ops *io, uint64_t chunk, pattern_kind kind, uint64_t test_id,
                  uint64_t capacity, int depth, char *err, size_t errn)
{
    memset(p, 0, sizeof *p);
    if (chunk < FC_ALIGN || chunk % FC_ALIGN != 0) {
        snprintf(err, errn, "chunk size must be a multiple of %d bytes", FC_ALIGN);
        return -1;
    }
    if (depth < 0 || depth > GEN_POOL_MAX_DEPTH) {
        snprintf(err, errn, "depth must be between 0 and %d", GEN_POOL_MAX_DEPTH);
        return -1;
    }
    p->io = io;
    p->chunk = chunk;
    p->capacity = capacity;
    p->kind = kind;
    p->test_id = test_id;
    p->wbatch = depth >= 2 && io->writev != NULL ? depth : 0;
    p->rbatch = depth >= 2 && io->readv != NULL ? depth : 0;
    if (p->rbatch > 0)
        p->rd_arena = xalloc_aligned(FC_ALIGN, (size_t)p->rbatch * (size_t)chunk);
    else
        p->got = xalloc_aligned(FC_ALIGN, (size_t)chunk);
    p->pool = gen_pool_create(kind, test_id, chunk, capacity, depth);
    if (p->pool == NULL) {
        free(p->got);
        free(p->rd_arena);
        snprintf(err, errn, "cannot create generator pool");
        return -1;
    }
    return 0;
}

void pipeline_free(pipeline *p)
{
    if (p->npend > 0) {
        stage_stats st;

        stage_stats_reset(&st);
        drain_writes(p, &st);
    }
    gen_pool_destroy(p->pool);
    free(p->got);
    free(p->rd_arena);
    memset(p, 0, sizeof *p);
}

size_t pipeline_len(const pipeline *p, uint64_t off)
{
    uint64_t rem = p->capacity - off;
    return (size_t)FC_MIN(p->chunk, rem);
}

static pattern_id id_for(const pipeline *p, uint64_t off, uint32_t pass)
{
    pattern_id id;
    id.test_id = p->test_id;
    id.pass = pass;
    id.chunk_index = off / p->chunk;
    return id;
}

void pipeline_prefetch(pipeline *p, uint64_t off, uint32_t pass)
{
    if (off >= p->capacity)
        return;
    gen_pool_prefetch(p->pool, off, pass);
}

void pipeline_timing(const pipeline *p, uint64_t *gen_ns, uint64_t *io_ns)
{
    *gen_ns = gen_pool_gen_ns(p->pool);
    *io_ns = p->io_ns;
}

static int drain_writes(pipeline *p, stage_stats *st)
{
    io_seg segs[PIPELINE_BATCH_MAX];
    uint64_t t0;
    int i, failed = 0;

    if (p->npend == 0)
        return 0;

    for (i = 0; i < p->npend; i++) {
        segs[i].buf = p->pend[i].buf;
        segs[i].off = p->pend[i].off;
        segs[i].len = p->pend[i].len;
        segs[i].res = 0;
    }
    t0 = now_ns();
    p->io->writev(p->io, segs, (size_t)p->npend);
    p->io_ns += now_ns() - t0;

    for (i = 0; i < p->npend; i++) {
        gen_pool_release(p->pool, p->pend[i].buf);
        if (segs[i].res < 0) {
            st->chunks_written--;
            st->bytes_written -= p->pend[i].len;
            st->io_errors++;
            failed = 1;
        }
    }
    p->npend = 0;
    p->rd_n = 0;
    return failed ? -1 : 0;
}

static int write_one(pipeline *p, uint64_t off, uint32_t pass, size_t len, stage_stats *st)
{
    uint8_t *exp = gen_pool_take(p->pool, off, pass);
    uint64_t t0 = now_ns();
    int r = p->io->write(p->io, exp, off, len);

    p->io_ns += now_ns() - t0;
    gen_pool_release(p->pool, exp);
    if (r < 0) {
        st->io_errors++;
        return r;
    }
    st->chunks_written++;
    st->bytes_written += len;
    return 0;
}

int pipeline_write(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st)
{
    size_t len = pipeline_len(p, off);

    p->rd_n = 0;
    if (p->wbatch == 0)
        return write_one(p, off, pass, len, st);

    if (p->npend >= p->wbatch && drain_writes(p, st) != 0)
        return -1;
    p->pend[p->npend].buf = gen_pool_take(p->pool, off, pass);
    p->pend[p->npend].off = off;
    p->pend[p->npend].len = len;
    p->npend++;
    st->chunks_written++;
    st->bytes_written += len;
    return 0;
}

static int rdq_want_batch(const pipeline *p, uint64_t off)
{
    if (p->rd_n == 0)
        return 1;
    if (off == p->rd[0].off + (uint64_t)p->rd_n * p->chunk)
        return 1;
    return 0;
}

static int rdq_fill(pipeline *p, uint64_t off, uint32_t pass, int batched)
{
    io_seg segs[PIPELINE_BATCH_MAX];
    uint64_t t0;
    int i, n = 0;
    int want = batched ? p->rbatch : 1;

    p->rd_n = 0;
    for (i = 0; i < want; i++) {
        uint64_t o = off + (uint64_t)i * p->chunk;

        if (o >= p->capacity)
            break;
        segs[n].buf = p->rd_arena + (size_t)n * p->chunk;
        segs[n].off = o;
        segs[n].len = pipeline_len(p, o);
        segs[n].res = 0;
        p->rd[n].off = o;
        p->rd[n].pass = pass;
        p->rd[n].valid = 0;
        p->rd[n].res = 0;
        n++;
    }
    p->rd_n = n;
    if (n == 0)
        return -EINVAL;

    t0 = now_ns();
    p->io->readv(p->io, segs, (size_t)n);
    p->io_ns += now_ns() - t0;

    for (i = 0; i < n; i++) {
        p->rd[i].res = segs[i].res;
        p->rd[i].valid = segs[i].res == 0;
    }
    return 0;
}

static int verify_read(pipeline *p, uint64_t off, uint32_t pass, size_t len, const uint8_t **out)
{
    int i;

    if (p->rbatch == 0) {
        uint64_t t0 = now_ns();
        int r = p->io->read(p->io, p->got, off, len);

        p->io_ns += now_ns() - t0;
        if (r < 0)
            return r;
        *out = p->got;
        return 0;
    }

    for (i = 0; i < p->rd_n; i++) {
        if (p->rd[i].valid && p->rd[i].off == off && p->rd[i].pass == pass) {
            *out = p->rd_arena + (size_t)i * p->chunk;
            return 0;
        }
    }

    if (rdq_fill(p, off, pass, rdq_want_batch(p, off)) != 0)
        return -EINVAL;
    if (p->rd[0].off != off)
        return -EINVAL;
    if (!p->rd[0].valid)
        return p->rd[0].res < 0 ? p->rd[0].res : -EIO;
    *out = p->rd_arena;
    return 0;
}

int pipeline_verify(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st)
{
    pattern_id id = id_for(p, off, pass);
    size_t len = pipeline_len(p, off);
    uint8_t *exp;
    const uint8_t *got;
    size_t bad;
    int r;

    if (p->npend > 0 && drain_writes(p, st) != 0)
        return -1;

    if (len == 0) {
        st->chunks_verified++;
        return 0;
    }

    exp = gen_pool_take(p->pool, off, pass);
    r = verify_read(p, off, pass, len, &got);
    if (r < 0) {
        gen_pool_release(p->pool, exp);
        st->io_errors++;
        return -1;
    }
    st->chunks_verified++;
    st->bytes_verified += len;
    bad = first_diff(exp, got, len);
    if (bad == len) {
        gen_pool_release(p->pool, exp);
        return 0;
    }

    st->chunks_failed++;
    st->bad_bytes += len - bad;
    if (!st->has_first_fail) {
        pattern_id src;
        st->has_first_fail = 1;
        st->first_fail_off = off + bad;
        st->first_fail_lba = off / p->chunk;
        if (pattern_parse_header(got, &src) && src.test_id == p->test_id) {
            if (src.chunk_index != id.chunk_index) {
                pattern_id cand = id;
                uint8_t *tmp = xalloc_aligned(FC_ALIGN, len);
                cand.chunk_index = src.chunk_index;
                st->has_alias = 1;
                st->alias_src_off = src.chunk_index * p->chunk;
                pattern_fill(p->kind, tmp, len, &cand, 0);
                st->alias_confirmed = first_diff(tmp, got, len) == len;
                free(tmp);
            } else {
                st->stale_data = 1;
            }
        } else if (bad >= PAT_HEADER_SIZE) {
            st->foreign_data = 1;
        } else {
            st->unwritten_data = 1;
        }
    }
    gen_pool_release(p->pool, exp);
    return 1;
}

int pipeline_flush(pipeline *p, stage_stats *st)
{
    if (p->npend > 0 && drain_writes(p, st) != 0)
        return -1;
    return p->io->flush(p->io);
}

int pipeline_window_pass(pipeline *p, uint64_t start, uint64_t end, uint64_t window_chunks,
                         uint32_t pass, stage_stats *st, int stop_on_fail)
{
    uint64_t off;
    int failed = 0;

    if (window_chunks == 0)
        window_chunks = 1;
    for (off = start; off < end; off += window_chunks * p->chunk) {
        uint64_t wend = FC_MIN(off + window_chunks * p->chunk, end);
        uint64_t w;

        for (w = off; w < wend; w += p->chunk) {
            if (w + p->chunk < wend)
                pipeline_prefetch(p, w + p->chunk, pass);
            if (pipeline_write(p, w, pass, st) != 0)
                return -1;
        }
        if (pipeline_flush(p, st) != 0) {
            st->io_errors++;
            return -1;
        }
        for (w = off; w < wend; w += p->chunk) {
            if (w + p->chunk < wend)
                pipeline_prefetch(p, w + p->chunk, pass);
            if (pipeline_verify(p, w, pass, st) != 0) {
                failed = 1;
                if (stop_on_fail)
                    return 1;
            }
        }
        if (stop_on_fail && failed)
            return 1;
    }
    return failed;
}

int pipeline_verify_offsets(pipeline *p, const uint64_t *offs, size_t n, uint32_t pass,
                            stage_stats *st, int stop_on_fail)
{
    int failed = 0;

    for (size_t i = 0; i < n; i++) {
        if (i + 1 < n)
            pipeline_prefetch(p, offs[i + 1], pass);
        if (pipeline_verify(p, offs[i], pass, st) != 0) {
            failed = 1;
            if (stop_on_fail)
                break;
        }
    }
    return failed;
}

uint64_t pipeline_probes(uint64_t start, uint64_t end, uint64_t chunk, size_t max_probes,
                         uint64_t *out, size_t out_cap)
{
    uint64_t offs[128];
    size_t n = 0;
    size_t cap = FC_MIN(max_probes, out_cap);
    uint64_t span = end > start ? end - start : 0;
    uint64_t d;

    if (cap == 0)
        return 0;
    start = (start / chunk) * chunk;
    offs[n++] = start;
    for (d = chunk; n < cap && d <= span / 2; d *= 2) {
        uint64_t o = start + d;
        if (o < end && o > offs[n - 1])
            offs[n++] = o;
    }
    for (size_t i = 1; i < n; i++) {
        uint64_t v = offs[i];
        size_t j = i;
        while (j > 0 && offs[j - 1] > v) {
            offs[j] = offs[j - 1];
            j--;
        }
        offs[j] = v;
    }
    for (size_t i = 0; i < n; i++)
        out[i] = offs[i];
    return n;
}
