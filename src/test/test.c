#include "flashcheck/test.h"

#include "flashcheck/util.h"

static uint64_t g_last_progress;

stage_report *stage_new(run_ctx *c, const char *name)
{
    stage_report *r;

    if (c->nst >= MAX_STAGES) {
        log_warn("stage table full, dropping stage %s", name);
        r = &c->st[MAX_STAGES - 1];
    } else {
        r = &c->st[c->nst++];
    }
    memset(r, 0, sizeof *r);
    r->name = name;
    r->executed = 1;
    return r;
}

void stage_progress(const char *label, uint64_t done, uint64_t total, double bps)
{
    uint64_t now = now_ms();
    char a[64], b[64], r[64];

    if (now - g_last_progress < 2000)
        return;
    g_last_progress = now;
    fmt_size(a, sizeof a, done);
    fmt_size(b, sizeof b, total);
    fmt_rate(r, sizeof r, bps);
    log_out("  [%s] %s / %s  %s", label, a, b, r);
    g_last_progress = now - 1500;
}

uint64_t ctx_start(const run_ctx *c) { return c->cfg->offset; }

uint64_t ctx_end(const run_ctx *c)
{
    uint64_t e = c->cfg->limit != 0 ? FC_MIN(c->cfg->limit, c->dev->capacity) : c->dev->capacity;
    uint64_t chunk = c->cfg->chunk_size;
    return (e / chunk) * chunk;
}

void stage_fill_speed(stage_report *r, const speed_track *w, const speed_track *rd)
{
    uint64_t wb = 0, rb = 0;
    double ws = 0, rs = 0, wmax = 0, wmin = 0, rmax = 0, rmin = 0;

    speed_summary(w, &wb, &ws, &r->write_bps, &wmax, &wmin);
    speed_summary(rd, &rb, &rs, &r->read_bps, &rmax, &rmin);
    r->write_sec = ws;
    r->read_sec = rs;
    r->max_bps = wmax > rmax ? wmax : rmax;
    r->min_bps = wmin;
    r->bytes_written = wb;
    r->bytes_verified = rb;
}

verdict run_verdict(const run_ctx *c)
{
    uint64_t io_err = 0, mism = 0;

    for (size_t i = 0; i < c->nst; i++) {
        io_err += c->st[i].io_errors;
        mism += c->st[i].chunks_failed;
    }
    if (mism > 0)
        return VERDICT_FAIL;
    if (io_err > 0)
        return VERDICT_INCONCLUSIVE;
    if (c->tested_bytes == 0)
        return VERDICT_INCONCLUSIVE;
    if (c->cfg->mode == MODE_QUICK)
        return VERDICT_INCONCLUSIVE;
    if (c->cfg->mode == MODE_STANDARD && !c->has_capacity)
        return VERDICT_INCONCLUSIVE;
    return VERDICT_PASS;
}

int run_execute(run_ctx *c)
{
    stage_report *r;

    r = stage_new(c, "identify");
    stage_identify(c, r);

    if (c->cfg->mode == MODE_IDENTIFY)
        return 0;

    r = stage_new(c, "benchmark");
    if (stage_benchmark(c, r) < 0)
        return -1;
    if (r->io_errors > 0)
        return 0;

    r = stage_new(c, "sparse-probe");
    if (stage_sparse(c, r) < 0)
        return -1;
    if (r->chunks_failed > 0)
        return 0;

    r = stage_new(c, "retention");
    if (stage_retention(c, r) < 0)
        return -1;
    if (r->chunks_failed > 0)
        return 0;

    r = stage_new(c, "capacity-boundary");
    if (stage_boundary(c, r) < 0)
        return -1;
    if (r->chunks_failed > 0) {
        if (r->has_capacity) {
            c->reliable_capacity = r->reliable_capacity;
            c->has_capacity = 1;
        }
        return 0;
    }
    if (r->has_capacity) {
        c->reliable_capacity = r->reliable_capacity;
        c->has_capacity = 1;
    }

    if (c->cfg->mode == MODE_FULL) {
        r = stage_new(c, "full-verify");
        if (stage_full(c, r) < 0)
            return -1;
    }
    return 0;
}
