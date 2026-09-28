#include "flashcheck/report.h"

#include "flashcheck/util.h"
#include "flashcheck/common.h"

static const char *short_desc(const run_ctx *c, verdict v, uint64_t mism)
{
    const stage_report *first = NULL;
    for (size_t i = 0; i < c->nst; i++) {
        if (c->st[i].has_first_fail) {
            first = &c->st[i];
            break;
        }
    }

    if (v == VERDICT_PASS)
        return "device appears genuine";
    if (v == VERDICT_INCONCLUSIVE) {
        if (c->cfg->mode == MODE_IDENTIFY)
            return "identify only prints device info, no test was run";
        if (c->tested_bytes == 0)
            return "no data was written; re-run with --destructive";
        return "cannot certify, re-run with --mode full";
    }
    if (first && first->unwritten_data) {
        static char buf[128];
        char a[64];
        fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk);
        snprintf(buf, sizeof buf, "region at %s was never written", a);
        return buf;
    }
    if (first) {
        static char buf[128];
        char a[64];
        fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk);
        snprintf(buf, sizeof buf,
                 "%llu regions differ, first at %s",
                 (unsigned long long)mism, a);
        return buf;
    }
    return "test failed";
}

static void print_short_result(const run_ctx *c, verdict v, uint64_t mism)
{
    const char *color, *label;
    switch (v) {
    case VERDICT_PASS:         color = COLOR_GREEN; label = "PASS"; break;
    case VERDICT_FAIL:         color = COLOR_RED;   label = "FAIL"; break;
    default:                   color = COLOR_YELLOW; label = "INCONCLUSIVE"; break;
    }
    const char *desc = short_desc(c, v, mism);
    fprintf(stdout, "%s%s%s  %s%s\n", color, COLOR_BOLD, label, desc, COLOR_RESET);
}

const char *report_evidence(const run_ctx *c, char *buf, size_t n)
{
    const stage_report *first = NULL;

    if (c->cfg->mode == MODE_IDENTIFY) {
        snprintf(buf, n, "identify mode writes nothing: device information only, "
                         "no test was performed");
        return buf;
    }

    for (size_t i = 0; i < c->nst; i++) {
        if (c->st[i].has_first_fail) {
            first = &c->st[i];
            break;
        }
    }
    if (first == NULL) {
        for (size_t i = 0; i < c->nst; i++) {
            if (c->st[i].io_errors > 0) {
                snprintf(buf, n, "read/write errors during stage '%s'", c->st[i].name);
                return buf;
            }
        }
        if (c->tested_bytes == 0) {
            snprintf(buf, n, "no data was written to the device: nothing was tested");
            return buf;
        }
        if (c->cfg->mode == MODE_QUICK) {
            char s[64];
            snprintf(buf, n,
                      "no counter-evidence found, but only %s of the device was written; "
                      "a quick test cannot certify capacity",
                      (fmt_size(s, sizeof s, c->tested_bytes), s));
            return buf;
        }
        snprintf(buf, n, "every written region read back exactly as written");
        return buf;
    }

    if (first->alias_confirmed) {
        char a[64], b[64];
        snprintf(buf, n, "region at %s returned data that was written to %s: the controller "
                         "is mapping two logical addresses to the same storage",
                 (fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk), a),
                 (fmt_size(b, sizeof b, first->alias_src_off), b));
    } else if (first->has_alias) {
        char a[64], b[64];
        snprintf(buf, n, "region at %s returned data tagged as written to %s",
                 (fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk), a),
                 (fmt_size(b, sizeof b, first->alias_src_off), b));
    } else if (first->stale_data) {
        char a[64];
        snprintf(buf, n, "region at %s still held data from an earlier pass: writes past the "
                         "real capacity destroyed older data",
                 (fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk), a));
    } else if (first->foreign_data) {
        char a[64];
        snprintf(buf, n,
                 "region at %s returned data that is not part of this test (foreign content)",
                 (fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk), a));
    } else if (first->unwritten_data) {
        char a[64];
        snprintf(buf, n, "region at %s was never written (no test header present)",
                 (fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk), a));
    } else {
        char a[64];
        snprintf(buf, n, "bit errors in region at %s (no aliasing detected: probable media "
                         "decay rather than fake capacity)",
                 (fmt_size(a, sizeof a, first->first_fail_off / c->pl.chunk * c->pl.chunk), a));
    }
    return buf;
}

static void print_stage(const stage_report *r, uint64_t chunk)
{
    char a[64], b[64], c1[64], c2[64];

    log_out("");
    log_out("Stage %s:", r->name);
    if (r->bytes_written > 0) {
        fmt_size(a, sizeof a, r->bytes_written);
        fmt_rate(b, sizeof b, r->write_bps);
        fmt_time(c1, sizeof c1, r->write_sec);
        log_out("  written:        %s in %s (%s)", a, c1, b);
    }
    if (r->bytes_verified > 0) {
        fmt_size(a, sizeof a, r->bytes_verified);
        fmt_rate(b, sizeof b, r->read_bps);
        fmt_time(c1, sizeof c1, r->read_sec);
        log_out("  verified:       %s in %s (%s)", a, c1, b);
    }
    if (r->max_bps > 0 && r->bytes_written > 1024 * 1024) {
        fmt_rate(c2, sizeof c2, r->max_bps);
        log_out("  peak rate:      %s", c2);
    }
    if (r->regions_verified > 0 && r->chunks_failed == 0)
        log_out("  regions:        %llu verified, all intact",
                (unsigned long long)r->regions_verified);
    if (r->chunks_failed > 0) {
        fmt_size(a, sizeof a, r->first_fail_off / chunk * chunk);
        log_out("  FAILED:         %llu of %llu regions differ",
                (unsigned long long)r->chunks_failed,
                (unsigned long long)(r->regions_verified + r->chunks_failed));
        log_out("  first failure:  %s", a);
        if (r->has_alias) {
            fmt_size(b, sizeof b, r->alias_src_off);
            log_out("  aliased from:   %s%s", b, r->alias_confirmed ? " (confirmed)" : "");
        }
    }
    if (r->io_errors > 0)
        log_out("  I/O errors:     %llu", (unsigned long long)r->io_errors);
    if (r->has_capacity) {
        fmt_size(b, sizeof b, r->reliable_capacity);
        log_out("  reliable cap:   >= %s", b);
    }
    if (r->note[0] != '\0')
        log_out("  note:           %s", r->note);
}

void report_console(const run_ctx *c, verdict v)
{
    char a[64], b[64], r2[64], t[64];
    char ev[512];
    uint64_t mism = 0, io_err = 0;

    for (size_t i = 0; i < c->nst; i++)
        print_stage(&c->st[i], c->pl.chunk);

    log_out("");
    log_out("Summary:");
    fmt_size(a, sizeof a, c->dev->capacity);
    log_out("  reported capacity:   %s", a);
    fmt_size(b, sizeof b, c->tested_bytes);
    log_out("  data written:        %s", b);
    if (c->has_capacity) {
        fmt_size(r2, sizeof r2, c->reliable_capacity);
        log_out("  reliable capacity:   >= %s", r2);
    }
    for (size_t i = 0; i < c->nst; i++) {
        io_err += c->st[i].io_errors;
        mism += c->st[i].chunks_failed;
    }
    log_out("  mismatched regions:  %llu", (unsigned long long)mism);
    log_out("  I/O errors:          %llu", (unsigned long long)io_err);
    {
        uint64_t gen_ns = 0, io_ns = 0;
        char g1[64], i1[64];

        pipeline_timing(&c->pl, &gen_ns, &io_ns);
        if (gen_ns > 0 || io_ns > 0) {
            fmt_time(g1, sizeof g1, (double)gen_ns / 1e9);
            fmt_time(i1, sizeof i1, (double)io_ns / 1e9);
            log_out("  data generation:     %s", g1);
            log_out("  device I/O:          %s", i1);
        }
    }
    fmt_time(t, sizeof t, 0);
    log_out("");
    if (c->cfg->mode == MODE_IDENTIFY) {
        log_out("NOTE: identify mode performed no test (%s written).", b);
        log_out("      re-run with --destructive to test capacity (default: adaptive).");
        return;
    }
    log_out("RESULT: %s", verdict_str(v));
    log_out("  %s", report_evidence(c, ev, sizeof ev));
    print_short_result(c, v, mism);
    if (v == VERDICT_PASS) {
        log_out("  The device returned every byte written to it, over the tested range.");
    } else if (v == VERDICT_INCONCLUSIVE) {
        if (c->dev != NULL && (ctx_start(c) != 0 || ctx_end(c) < c->dev->capacity))
            log_out("  Only part of the claim was tested; re-run without --limit/--offset to "
                    "certify.");
        else
            log_out("  This is not proof of authenticity; re-run with --mode full to certify.");
    }
    if (c->dev != NULL && (ctx_start(c) != 0 || ctx_end(c) < c->dev->capacity)) {
        char rs[64], re[64], rc[64];

        fmt_size(rs, sizeof rs, ctx_start(c));
        fmt_size(re, sizeof re, ctx_end(c));
        fmt_size(rc, sizeof rc, c->dev->capacity);
        log_out("  Tested range %s..%s of %s reported; the claim was not fully probed.",
                rs, re, rc);
    }
}

void report_bench(const run_ctx *c)
{
    char a[64], b[64];
    uint64_t mism = 0, io_err = 0;

    for (size_t i = 0; i < c->nst; i++)
        print_stage(&c->st[i], c->pl.chunk);

    log_out("");
    log_out("Summary:");
    fmt_size(a, sizeof a, c->dev->capacity);
    log_out("  reported capacity:   %s", a);
    fmt_size(b, sizeof b, c->tested_bytes);
    log_out("  data written:        %s", b);
    for (size_t i = 0; i < c->nst; i++) {
        io_err += c->st[i].io_errors;
        mism += c->st[i].chunks_failed;
    }
    log_out("  mismatched regions:  %llu", (unsigned long long)mism);
    log_out("  I/O errors:          %llu", (unsigned long long)io_err);
    {
        uint64_t gen_ns = 0, io_ns = 0;
        char g1[64], i1[64];

        pipeline_timing(&c->pl, &gen_ns, &io_ns);
        if (gen_ns > 0 || io_ns > 0) {
            fmt_time(g1, sizeof g1, (double)gen_ns / 1e9);
            fmt_time(i1, sizeof i1, (double)io_ns / 1e9);
            log_out("  data generation:     %s", g1);
            log_out("  device I/O:          %s", i1);
        }
    }
    log_out("");
    log_out("NOTE: benchmark only, no authenticity verdict was produced.");
    if (io_err > 0)
        log_out("      I/O errors occurred: the measured rates may be unreliable.");
    else if (mism > 0)
        log_out("      read-back did not match: the device returned corrupted data.");
    else
        log_out("      every byte written in the benchmark range read back as written.");
}
