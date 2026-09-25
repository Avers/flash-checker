#include "flashcheck/pipeline.h"

#include "flashcheck/util.h"

int pipeline_init(pipeline *p, io_ops *io, uint64_t chunk, pattern_kind kind, uint64_t test_id,
                  uint64_t capacity, char *err, size_t errn)
{
    memset(p, 0, sizeof *p);
    if (chunk < FC_ALIGN || chunk % FC_ALIGN != 0) {
        snprintf(err, errn, "chunk size must be a multiple of %d bytes", FC_ALIGN);
        return -1;
    }
    p->io = io;
    p->chunk = chunk;
    p->capacity = capacity;
    p->kind = kind;
    p->test_id = test_id;
    p->exp = xalloc_aligned(FC_ALIGN, (size_t)chunk);
    p->got = xalloc_aligned(FC_ALIGN, (size_t)chunk);
    return 0;
}

void pipeline_free(pipeline *p)
{
    free(p->exp);
    free(p->got);
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

int pipeline_write(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st)
{
    pattern_id id = id_for(p, off, pass);
    size_t len = pipeline_len(p, off);
    int r;

    pattern_fill(p->kind, p->exp, len, &id, 0);
    r = p->io->write(p->io, p->exp, off, len);
    if (r < 0) {
        st->io_errors++;
        return r;
    }
    st->chunks_written++;
    st->bytes_written += len;
    return 0;
}

int pipeline_verify(pipeline *p, uint64_t off, uint32_t pass, stage_stats *st)
{
    pattern_id id = id_for(p, off, pass);
    size_t len = pipeline_len(p, off);
    size_t bad;
    int r;

    r = p->io->read(p->io, p->got, off, len);
    if (r < 0) {
        st->io_errors++;
        return r;
    }
    st->chunks_verified++;
    st->bytes_verified += len;
    pattern_fill(p->kind, p->exp, len, &id, 0);
    bad = first_diff(p->exp, p->got, len);
    if (bad == len)
        return 0;

    st->chunks_failed++;
    st->bad_bytes += len - bad;
    if (!st->has_first_fail) {
        pattern_id src;
        st->has_first_fail = 1;
        st->first_fail_off = off + bad;
        st->first_fail_lba = off / p->chunk;
        if (pattern_parse_header(p->got, &src) && src.test_id == p->test_id) {
            if (src.chunk_index != id.chunk_index) {
                pattern_id cand = id;
                uint8_t *tmp = xalloc_aligned(FC_ALIGN, len);
                cand.chunk_index = src.chunk_index;
                st->has_alias = 1;
                st->alias_src_off = src.chunk_index * p->chunk;
                pattern_fill(p->kind, tmp, len, &cand, 0);
                st->alias_confirmed = first_diff(tmp, p->got, len) == len;
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
    return 1;
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
            if (pipeline_write(p, w, pass, st) != 0)
                return -1;
        }
        if (p->io->flush(p->io) != 0) {
            st->io_errors++;
            return -1;
        }
        for (w = off; w < wend; w += p->chunk) {
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
