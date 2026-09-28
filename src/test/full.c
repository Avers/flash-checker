#include "flashcheck/test.h"

#include "flashcheck/checkpoint.h"
#include "flashcheck/util.h"

int stage_full(run_ctx *c, stage_report *r)
{
    stage_stats st;
    speed_track w, rd;
    uint64_t start = ctx_start(c);
    uint64_t end = ctx_end(c);
    uint64_t window_chunks = c->cfg->window_size / c->pl.chunk;
    uint64_t off;
    uint64_t resume = c->resume_off;
    uint64_t last_cp = now_ms();
    int failed = 0;
    char sz[64];
    checkpoint cp;

    stage_stats_reset(&st);
    speed_init(&w);
    speed_init(&rd);
    if (window_chunks == 0)
        window_chunks = 1;

    log_out("");
    if (resume > start && resume < end) {
        log_out("Stage: resuming full verify at %s", (fmt_size(sz, sizeof sz, resume), sz));
        start = resume;
    }
    log_out("Stage: full destructive verify (%s window, %s pattern)",
            (fmt_size(sz, sizeof sz, window_chunks * c->pl.chunk), sz),
            pattern_kind_str(c->cfg->pattern));

    for (off = start; off < end; off += window_chunks * c->pl.chunk) {
        uint64_t wend = FC_MIN(off + window_chunks * c->pl.chunk, end);
        uint64_t o;

        for (o = off; o < wend; o += c->pl.chunk) {
            if (o + c->pl.chunk < wend)
                pipeline_prefetch(&c->pl, o + c->pl.chunk, 1);
            if (pipeline_write(&c->pl, o, 1, &st) != 0)
                goto io_error;
            speed_mark(&w, st.bytes_written);
        }
        if (pipeline_flush(&c->pl, &st) != 0)
            goto io_error;
        for (o = off; o < wend; o += c->pl.chunk) {
            if (o + c->pl.chunk < wend)
                pipeline_prefetch(&c->pl, o + c->pl.chunk, 1);
            if (pipeline_verify(&c->pl, o, 1, &st) != 0) {
                failed = 1;
                break;
            }
            speed_mark(&rd, st.bytes_verified);
        }
        stage_progress("verify", st.bytes_written, end - start, 0);
        if (now_ms() - last_cp > 5000) {
            memset(&cp, 0, sizeof cp);
            snprintf(cp.stage, sizeof cp.stage, "full-verify");
            cp.test_id = c->test_id;
            cp.offset = wend;
            cp.bytes_written = st.bytes_written;
            cp.bytes_verified = st.bytes_verified;
            cp.errors = st.io_errors;
            if (checkpoint_save(c->checkpoint_path, &cp) == 0)
                last_cp = now_ms();
        }
        if (failed)
            break;
    }

    stage_fill_speed(r, &w, &rd);
    r->regions_written = st.chunks_written;
    r->regions_verified = st.chunks_verified;
    r->chunks_failed = st.chunks_failed;
    r->io_errors = st.io_errors;
    r->bad_bytes = st.bad_bytes;
    r->has_first_fail = st.has_first_fail;
    r->first_fail_off = st.first_fail_off;
    r->alias_src_off = st.alias_src_off;
    r->has_alias = st.has_alias;
    r->alias_confirmed = st.alias_confirmed;
    r->foreign_data = st.foreign_data;
    r->stale_data = st.stale_data;
    r->unwritten_data = st.unwritten_data;
    c->tested_bytes += st.bytes_written;
    if (failed)
        snprintf(r->note, sizeof r->note, "verification failed; stopped at first bad region");
    memset(&cp, 0, sizeof cp);
    snprintf(cp.stage, sizeof cp.stage, "full-verify");
    cp.test_id = c->test_id;
    cp.offset = end;
    cp.bytes_written = st.bytes_written;
    cp.bytes_verified = st.bytes_verified;
    cp.errors = st.io_errors;
    cp.complete = !failed;
    checkpoint_save(c->checkpoint_path, &cp);
    speed_free(&w);
    speed_free(&rd);
    return 0;

io_error:
    r->io_errors = st.io_errors;
    r->has_first_fail = st.has_first_fail;
    r->first_fail_off = st.first_fail_off;
    snprintf(r->note, sizeof r->note, "I/O error");
    speed_free(&w);
    speed_free(&rd);
    return -1;
}
