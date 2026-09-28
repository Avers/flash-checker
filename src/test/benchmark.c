#include "flashcheck/test.h"

#include "flashcheck/util.h"

int stage_benchmark(run_ctx *c, stage_report *r)
{
    speed_track w, rd;
    stage_stats st;
    uint64_t start = ctx_start(c);
    uint64_t end = ctx_end(c);
    uint64_t bend = FC_MIN(end, start + c->cfg->bench_bytes);
    uint64_t off;
    char sz[64];

    stage_stats_reset(&st);
    speed_init(&w);
    speed_init(&rd);

    fmt_size(sz, sizeof sz, bend - start);
    log_out("");
    log_out("Stage: speed benchmark (%s, pattern %s)", sz, pattern_kind_str(c->cfg->pattern));

    bend = (bend / c->pl.chunk) * c->pl.chunk;
    if (bend <= start) {
        speed_free(&w);
        speed_free(&rd);
        snprintf(r->note, sizeof r->note, "skipped: range smaller than one chunk");
        return 0;
    }

    for (off = start; off < bend; off += c->pl.chunk) {
        if (off + c->pl.chunk < bend)
            pipeline_prefetch(&c->pl, off + c->pl.chunk, 0);
        if (pipeline_write(&c->pl, off, 0, &st) != 0)
            goto io_error;
        speed_mark(&w, st.bytes_written);
        stage_progress("write", st.bytes_written, bend - start,
                       w.total_bytes / FC_MAX(w.last_ms - w.start_ms, 1) * 1000.0);
    }
    if (pipeline_flush(&c->pl, &st) != 0)
        goto io_error;

    for (off = start; off < bend; off += c->pl.chunk) {
        if (off + c->pl.chunk < bend)
            pipeline_prefetch(&c->pl, off + c->pl.chunk, 0);
        if (pipeline_verify(&c->pl, off, 0, &st) != 0 && st.io_errors > 0)
            goto io_error;
        speed_mark(&rd, st.bytes_verified);
        stage_progress("read", st.bytes_verified, bend - start,
                       rd.total_bytes / FC_MAX(rd.last_ms - rd.start_ms, 1) * 1000.0);
    }

    stage_fill_speed(r, &w, &rd);
    r->chunks_failed = st.chunks_failed;
    r->io_errors = st.io_errors;
    r->bad_bytes = st.bad_bytes;
    r->has_first_fail = st.has_first_fail;
    r->first_fail_off = st.first_fail_off;
    c->tested_bytes += st.bytes_written;
    speed_free(&w);
    speed_free(&rd);
    return 0;

io_error:
    r->io_errors = st.io_errors;
    r->chunks_failed = st.chunks_failed;
    r->has_first_fail = st.has_first_fail;
    r->first_fail_off = st.first_fail_off;
    snprintf(r->note, sizeof r->note, "I/O error during benchmark");
    speed_free(&w);
    speed_free(&rd);
    return -1;
}
