#include "flashcheck/test.h"

#include "flashcheck/util.h"

typedef struct {
    run_ctx *c;
    stage_stats st;
    speed_track w;
    speed_track rd;
    int verdict_ok;
} bound_ctx;

static int probe_ok(bound_ctx *b, uint64_t x)
{
    stage_stats st;
    pattern_id id;
    uint64_t off;
    int ok = 1;

    stage_stats_reset(&st);
    id.test_id = b->c->test_id;
    id.pass = 9000;
    id.chunk_index = x / b->c->pl.chunk;
    off = (x / b->c->pl.chunk) * b->c->pl.chunk;

    if (pipeline_write(&b->c->pl, off, (uint32_t)id.pass, &st) != 0) {
        b->st.io_errors++;
        return 0;
    }
    for (uint64_t o = 0; o < x; o += b->c->pl.chunk) {
        if (o + b->c->pl.chunk < x)
            pipeline_prefetch(&b->c->pl, o + b->c->pl.chunk, 9001);
        if (pipeline_write(&b->c->pl, o, 9001, &st) != 0) {
            b->st.io_errors++;
            return 0;
        }
    }
    if (pipeline_flush(&b->c->pl, &st) != 0) {
        b->st.io_errors++;
        return 0;
    }
    if (pipeline_verify(&b->c->pl, off, (uint32_t)id.pass, &st) != 0)
        ok = 0;
    b->st.chunks_written += st.chunks_written;
    b->st.chunks_verified += st.chunks_verified;
    b->st.bytes_written += st.bytes_written;
    b->st.bytes_verified += st.bytes_verified;
    if (!ok) {
        b->st.chunks_failed++;
        if (!b->st.has_first_fail) {
            b->st.has_first_fail = 1;
            b->st.first_fail_off = st.first_fail_off;
            b->st.alias_src_off = st.alias_src_off;
            b->st.has_alias = st.has_alias;
            b->st.alias_confirmed = st.alias_confirmed;
            b->st.foreign_data = st.foreign_data;
            b->st.stale_data = st.stale_data;
            b->st.unwritten_data = st.unwritten_data;
        }
    }
    speed_mark(&b->w, b->st.bytes_written);
    speed_mark(&b->rd, b->st.bytes_verified);
    return ok;
}

int stage_boundary(run_ctx *c, stage_report *r)
{
    bound_ctx b;
    uint64_t chunk = c->pl.chunk;
    uint64_t end = ctx_end(c);
    uint64_t budget = c->cfg->boundary_budget;
    uint64_t spent = 0;
    uint64_t x = chunk * 8;
    uint64_t lo = 0, hi = 0;
    int have_fail = 0;
    char sz[64], sz2[64];

    memset(&b, 0, sizeof b);
    b.c = c;
    stage_stats_reset(&b.st);
    if (end < chunk * 16) {
        snprintf(r->note, sizeof r->note, "skipped: range too small for boundary search");
        return 0;
    }
    speed_init(&b.w);
    speed_init(&b.rd);

    log_out("");
    log_out("Stage: reliable capacity search (budget %s, resolution %s)",
            (fmt_size(sz, sizeof sz, budget), sz),
            (fmt_size(sz2, sizeof sz2, c->cfg->boundary_resolution), sz2));

    for (;;) {
        if (x > end) {
            if (!have_fail)
                hi = end;
            break;
        }
        log_out("  probing %s ...", (fmt_size(sz, sizeof sz, x), sz));
        if (spent + 2 * x > budget) {
            log_warn("boundary search stopped: budget exhausted");
            break;
        }
        if (probe_ok(&b, x)) {
            lo = x;
            spent += 2 * x;
            if (x > end / 2)
                break;
            x *= 2;
        } else {
            hi = x;
            have_fail = 1;
            break;
        }
    }

    if (have_fail) {
        uint64_t res = FC_MAX(chunk, c->cfg->boundary_resolution);
        while (hi - lo > res) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (spent + 2 * mid > budget) {
                log_warn("boundary bisection stopped: budget exhausted");
                break;
            }
            log_out("  bisect %s ...", (fmt_size(sz, sizeof sz, mid), sz));
            if (probe_ok(&b, mid))
                lo = mid;
            else
                hi = mid;
            spent += 2 * mid;
        }
    }

    r->bytes_written = b.st.bytes_written;
    r->bytes_verified = b.st.bytes_verified;
    stage_fill_speed(r, &b.w, &b.rd);
    r->regions_written = b.st.chunks_written;
    r->regions_verified = b.st.chunks_verified;
    r->chunks_failed = b.st.chunks_failed;
    r->io_errors = b.st.io_errors;
    r->has_first_fail = b.st.has_first_fail;
    r->first_fail_off = b.st.first_fail_off;
    r->alias_src_off = b.st.alias_src_off;
    r->has_alias = b.st.has_alias;
    r->alias_confirmed = b.st.alias_confirmed;
    r->foreign_data = b.st.foreign_data;
    r->stale_data = b.st.stale_data;
    r->unwritten_data = b.st.unwritten_data;
    c->tested_bytes += b.st.bytes_written;
    speed_free(&b.w);
    speed_free(&b.rd);
    r->reliable_capacity = lo;
    r->has_capacity = 1;
    snprintf(r->note, sizeof r->note, "reliable capacity >= %s (%s spent of budget)",
             (fmt_size(sz, sizeof sz, lo), sz), (fmt_size(sz2, sizeof sz2, spent), sz2));
    return 0;
}
