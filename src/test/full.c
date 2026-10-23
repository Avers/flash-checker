#include "flashcheck/test.h"

#include "flashcheck/checkpoint.h"
#include "flashcheck/util.h"
#include "flashcheck/visual.h"

enum {
    FULL_PHASE_WRITE = 0,
    FULL_PHASE_READ = 1
};

static void checkpoint_metadata(checkpoint *cp, const run_ctx *c)
{
    cp->device_id = c->dev->device_id;
    cp->device_fingerprint = device_fingerprint(c->dev);
    cp->capacity = c->dev->capacity;
    cp->tested_start = c->cfg->offset;
    cp->tested_limit = c->cfg->limit;
    cp->chunk_size = c->cfg->chunk_size;
    cp->passes = c->cfg->passes;
    cp->pattern = c->cfg->pattern;
    cp->mode = c->cfg->mode;
}

static void save_checkpoint(run_ctx *c, const stage_stats *st, int phase, uint64_t pass,
                            uint64_t off, int complete)
{
    checkpoint cp;

    memset(&cp, 0, sizeof cp);
    snprintf(cp.stage, sizeof cp.stage, "full-write-verify");
    cp.test_id = c->test_id;
    cp.offset = off;
    cp.bytes_written = st->bytes_written;
    cp.bytes_verified = st->bytes_verified;
    cp.errors = st->io_errors;
    cp.phase = phase;
    cp.pass = pass;
    cp.complete = complete;
    checkpoint_metadata(&cp, c);
    checkpoint_save(c->checkpoint_path, &cp);
}

static void copy_stats(stage_report *r, const stage_stats *st)
{
    r->regions_written = st->chunks_written;
    r->regions_verified = st->chunks_verified;
    r->chunks_failed = st->chunks_failed;
    r->io_errors = st->io_errors;
    r->bad_bytes = st->bad_bytes;
    r->has_first_fail = st->has_first_fail;
    r->first_fail_off = st->first_fail_off;
    r->alias_src_off = st->alias_src_off;
    r->has_alias = st->has_alias;
    r->alias_confirmed = st->alias_confirmed;
    r->foreign_data = st->foreign_data;
    r->stale_data = st->stale_data;
    r->unwritten_data = st->unwritten_data;
    r->bytes_written = st->bytes_written;
    r->bytes_verified = st->bytes_verified;
}

int stage_full(run_ctx *c, stage_report *r)
{
    stage_stats st;
    speed_track w, rd;
    uint64_t start = ctx_start(c);
    uint64_t end = ctx_end(c);
    uint64_t window_chunks = c->cfg->window_size / c->pl.chunk;
    uint64_t first_pass = c->resume_valid ? c->resume_pass : 0;
    int first_phase = c->resume_valid ? c->resume_phase : FULL_PHASE_WRITE;
    uint64_t resume_off = c->resume_valid ? c->resume_off : start;
    uint64_t pass;
    int failed = 0;
    int interrupted = 0;
    char sz[64];

    stage_stats_reset(&st);
    if (c->resume_valid) {
        st.bytes_written = c->resume_bytes_written;
        st.bytes_verified = c->resume_bytes_verified;
    }
    speed_init(&w);
    speed_init(&rd);
    if (window_chunks == 0)
        window_chunks = 1;

    if (!visual_is_enabled()) {
        log_out("");
        if (c->resume_valid)
            log_out("Stage: resuming full-range write/verify pass %llu from %s",
                    (unsigned long long)(first_pass + 1),
                    (fmt_size(sz, sizeof sz, resume_off), sz));
        else
            log_out("Stage: full-range write then verify (%s pattern, %llu pass%s)",
                    pattern_kind_str(c->cfg->pattern),
                    (unsigned long long)c->cfg->passes,
                    c->cfg->passes == 1 ? "" : "es");
    }
    visual_stage_begin(first_phase == FULL_PHASE_WRITE ? "full-write" : "full-read", start, end);

    for (pass = first_pass; pass < c->cfg->passes; pass++) {
        uint32_t tag = (uint32_t)(pass + 1);
        int phase = pass == first_pass ? first_phase : FULL_PHASE_WRITE;
        uint64_t phase_off = (pass == first_pass && c->resume_valid) ? resume_off : start;
        uint64_t off;

        if (fc_interrupted) {
            save_checkpoint(c, &st, phase, pass, phase_off, 0);
            interrupted = 1;
            break;
        }

        if (phase == FULL_PHASE_WRITE) {
            for (off = phase_off; off < end;) {
                uint64_t wend = FC_MIN(off + window_chunks * c->pl.chunk, end);
                uint64_t o;

                if (fc_interrupted) {
                    save_checkpoint(c, &st, FULL_PHASE_WRITE, pass, off, 0);
                    interrupted = 1;
                    break;
                }
                for (o = off; o < wend; o += c->pl.chunk) {
                    if (fc_interrupted)
                        break;
                    if (o + c->pl.chunk < wend)
                        pipeline_prefetch(&c->pl, o + c->pl.chunk, tag);
                    if (pipeline_write(&c->pl, o, tag, &st) != 0) {
                        save_checkpoint(c, &st, FULL_PHASE_WRITE, pass, off, 0);
                        goto io_error;
                    }
                    speed_mark(&w, st.bytes_written);
                }
                if (fc_interrupted) {
                    save_checkpoint(c, &st, FULL_PHASE_WRITE, pass, off, 0);
                    interrupted = 1;
                    break;
                }
                if (pipeline_flush(&c->pl, &st) != 0) {
                    if (fc_interrupted) {
                        save_checkpoint(c, &st, FULL_PHASE_WRITE, pass, off, 0);
                        interrupted = 1;
                        break;
                    }
                    save_checkpoint(c, &st, FULL_PHASE_WRITE, pass, off, 0);
                    goto io_error;
                }
                off = wend;
                save_checkpoint(c, &st, FULL_PHASE_WRITE, pass, off, 0);
                stage_progress("write", st.bytes_written,
                               (end - start) * c->cfg->passes,
                               w.total_bytes / FC_MAX(w.last_ms - w.start_ms, 1) * 1000.0);
            }
            if (interrupted)
                break;
            phase = FULL_PHASE_READ;
            phase_off = start;
            save_checkpoint(c, &st, phase, pass, phase_off, 0);
            visual_stage_step();
        }

        for (off = phase_off; off < end;) {
            uint64_t wend = FC_MIN(off + window_chunks * c->pl.chunk, end);
            uint64_t o;

            if (fc_interrupted) {
                save_checkpoint(c, &st, FULL_PHASE_READ, pass, off, 0);
                interrupted = 1;
                break;
            }
            for (o = off; o < wend; o += c->pl.chunk) {
                if (fc_interrupted)
                    break;
                if (o + c->pl.chunk < wend)
                    pipeline_prefetch(&c->pl, o + c->pl.chunk, tag);
                if (pipeline_verify(&c->pl, o, tag, &st) != 0) {
                    if (st.io_errors > 0) {
                        save_checkpoint(c, &st, FULL_PHASE_READ, pass, off, 0);
                        goto io_error;
                    }
                    visual_mark(o, VISUAL_FAIL);
                    failed = 1;
                    save_checkpoint(c, &st, FULL_PHASE_READ, pass, o, 0);
                    break;
                }
                speed_mark(&rd, st.bytes_verified);
                visual_mark(o, VISUAL_OK);
            }
            if (failed)
                break;
            if (fc_interrupted) {
                save_checkpoint(c, &st, FULL_PHASE_READ, pass, off, 0);
                interrupted = 1;
                break;
            }
            off = wend;
            save_checkpoint(c, &st, FULL_PHASE_READ, pass, off, 0);
            stage_progress("verify", st.bytes_verified,
                           (end - start) * c->cfg->passes,
                           rd.total_bytes / FC_MAX(rd.last_ms - rd.start_ms, 1) * 1000.0);
        }
        if (failed || interrupted)
            break;
        if (pass + 1 < c->cfg->passes) {
            phase = FULL_PHASE_WRITE;
            save_checkpoint(c, &st, phase, pass + 1, start, 0);
            visual_stage_step();
        }
    }

    visual_stage_end();
    stage_fill_speed(r, &w, &rd);
    copy_stats(r, &st);
    c->tested_bytes += st.bytes_written;
    if (failed)
        snprintf(r->note, sizeof r->note, "verification failed at the first mismatching region");
    if (!failed && !interrupted && !fc_interrupted && start == 0 && end == c->dev->capacity) {
        c->reliable_capacity = end;
        c->has_capacity = 1;
    }
    if (!failed && !interrupted && !fc_interrupted)
        save_checkpoint(c, &st, FULL_PHASE_READ, c->cfg->passes - 1, end, 1);
    speed_free(&w);
    speed_free(&rd);
    return 0;

io_error:
    visual_stage_end();
    stage_fill_speed(r, &w, &rd);
    copy_stats(r, &st);
    c->tested_bytes += st.bytes_written;
    snprintf(r->note, sizeof r->note, "I/O error");
    speed_free(&w);
    speed_free(&rd);
    return -1;
}
