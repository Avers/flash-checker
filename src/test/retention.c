#include "flashcheck/test.h"

#include "flashcheck/util.h"
#include "flashcheck/visual.h"

int stage_retention(run_ctx *c, stage_report *r)
{
    stage_stats st;
    speed_track w, rd;
    uint64_t start = ctx_start(c);
    uint64_t end = ctx_end(c);
    uint64_t anchors[16];
    uint64_t n_anchors, fill_end, off;
    uint64_t pass;
    int failed = 0;
    char sz[64];

    stage_stats_reset(&st);
    speed_init(&w);
    speed_init(&rd);
    n_anchors = pipeline_probes(start, end, c->pl.chunk, 8, anchors, 16);
    if (n_anchors == 0 || end <= start) {
        speed_free(&w);
        speed_free(&rd);
        snprintf(r->note, sizeof r->note, "skipped: empty range");
        return 0;
    }
    fill_end = (end / c->pl.chunk) * c->pl.chunk;

    if (!visual_is_enabled()) {
        log_out("");
        log_out("Stage: retention (old data must survive writes past it)");
        log_out("  %llu anchor regions, write-only fill over %s", (unsigned long long)n_anchors,
                (fmt_size(sz, sizeof sz, fill_end - start), sz));
    }
    visual_stage_begin("retention", start, fill_end);

    for (pass = 0; pass < c->cfg->passes && !failed; pass++) {
        uint32_t p = (uint32_t)(pass + 1);

        for (off = 0; off < n_anchors; off++) {
            if (pipeline_write(&c->pl, anchors[off], p, &st) != 0) {
                visual_mark(anchors[off], VISUAL_IOERR);
                visual_stage_end();
                r->io_errors = st.io_errors;
                snprintf(r->note, sizeof r->note, "I/O error writing anchor");
                speed_free(&w);
                speed_free(&rd);
                return -1;
            }
            speed_mark(&w, st.bytes_written);
            visual_mark(anchors[off], VISUAL_OK);
        }
        if (pipeline_flush(&c->pl, &st) != 0) {
            r->io_errors = ++st.io_errors;
            snprintf(r->note, sizeof r->note, "flush failed");
            speed_free(&w);
            speed_free(&rd);
            return -1;
        }
        for (off = start; off < fill_end; off += c->pl.chunk) {
            int is_anchor = 0;

            if (off + c->pl.chunk < fill_end)
                pipeline_prefetch(&c->pl, off + c->pl.chunk, (uint32_t)(pass + 1000));
            for (uint64_t i = 0; i < n_anchors; i++) {
                if (anchors[i] == off) {
                    is_anchor = 1;
                    break;
                }
            }
            if (is_anchor)
                continue;
            if (pipeline_write(&c->pl, off, (uint32_t)(pass + 1000), &st) != 0) {
                visual_mark(off, VISUAL_IOERR);
                visual_stage_end();
                r->io_errors = st.io_errors;
                snprintf(r->note, sizeof r->note, "I/O error during fill");
                speed_free(&w);
                speed_free(&rd);
                return -1;
            }
            speed_mark(&w, st.bytes_written);
            visual_mark(off, VISUAL_OK);
            stage_progress("fill", st.bytes_written, (fill_end - start) * c->cfg->passes,
                           w.total_bytes / FC_MAX(w.last_ms - w.start_ms, 1) * 1000.0);
        }
        if (pipeline_flush(&c->pl, &st) != 0) {
            r->io_errors = ++st.io_errors;
            speed_free(&w);
            speed_free(&rd);
            return -1;
        }
        if (pipeline_verify_offsets(&c->pl, anchors, (size_t)n_anchors, p, &st, 1) > 0)
            failed = 1;
        speed_mark(&rd, st.bytes_verified);
    }
    visual_stage_end();
    if (st.has_first_fail)
        visual_mark(st.first_fail_off, VISUAL_FAIL);
    stage_fill_speed(r, &w, &rd);
    r->bytes_written = st.bytes_written;
    r->bytes_verified = st.bytes_verified;
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
    speed_free(&w);
    speed_free(&rd);
    if (failed)
        snprintf(r->note, sizeof r->note, "previously written data did not survive");
    return 0;
}
