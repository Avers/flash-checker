#include "flashcheck/test.h"

#include "flashcheck/util.h"

int stage_sparse(run_ctx *c, stage_report *r)
{
    stage_stats st;
    uint64_t start = ctx_start(c);
    uint64_t end = ctx_end(c);
    uint64_t probes[64];
    uint64_t n, i;
    int failed;
    char a[64], b[64];

    stage_stats_reset(&st);
    n = pipeline_probes(start, end, c->pl.chunk, 48, probes, 64);
    if (n == 0) {
        snprintf(r->note, sizeof r->note, "skipped: empty range");
        return 0;
    }
    fmt_size(a, sizeof a, start);
    fmt_size(b, sizeof b, end);
    log_out("");
    log_out("Stage: sparse probe (%llu regions, %s .. %s)", (unsigned long long)n, a, b);

    for (i = 0; i < n; i++) {
        if (pipeline_write(&c->pl, probes[i], 0, &st) != 0) {
            r->io_errors = st.io_errors;
            snprintf(r->note, sizeof r->note, "I/O error writing probe");
            return -1;
        }
    }
    if (c->io->flush(c->io) != 0) {
        r->io_errors = ++st.io_errors;
        snprintf(r->note, sizeof r->note, "flush failed");
        return -1;
    }
    failed = pipeline_verify_offsets(&c->pl, probes, (size_t)n, 0, &st, 0);

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
    if (failed)
        snprintf(r->note, sizeof r->note, "regions do not hold unique data");
    return 0;
}
