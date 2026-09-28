#include "flashcheck/stats.h"

#include "flashcheck/util.h"

void speed_init(speed_track *t)
{
    memset(t, 0, sizeof *t);
    t->cap = 256;
    t->s = xalloc(t->cap * sizeof *t->s);
    t->start_ms = now_ms();
    t->last_ms = t->start_ms;
}

void speed_free(speed_track *t)
{
    free(t->s);
    memset(t, 0, sizeof *t);
}

void speed_mark(speed_track *t, uint64_t total_bytes)
{
    uint64_t now = now_ms();
    t->total_bytes = total_bytes;
    if (now == t->last_ms)
        return;
    t->last_ms = now;
    if (t->n == t->cap) {
        t->cap *= 2;
        if (t->cap > SPEED_MAX_SAMPLES) {
            size_t keep = SPEED_MAX_SAMPLES / 2;
            memmove(t->s, t->s + (t->n - keep), keep * sizeof *t->s);
            t->n = keep;
        } else {
            speed_sample *ns = realloc(t->s, t->cap * sizeof *t->s);
            if (ns == NULL)
                return;
            t->s = ns;
        }
    }
    t->s[t->n].t_ms = now - t->start_ms;
    t->s[t->n].total_bytes = total_bytes;
    t->n++;
}

void speed_summary(const speed_track *t, uint64_t *bytes, double *sec, double *avg_bps,
                   double *max_bps, double *min_bps)
{
    double elapsed = (double)(t->last_ms - t->start_ms) / 1000.0;
    double mx = 0, mn = 0;
    int have = 0;
    size_t j = 0;

    if (bytes != NULL)
        *bytes = t->total_bytes;
    if (sec != NULL)
        *sec = elapsed;
    if (avg_bps != NULL)
        *avg_bps = elapsed > 0 ? (double)t->total_bytes / elapsed : 0;
    for (size_t i = 1; i < t->n; i++) {
        double dt, db, bps;

        while (j + 1 < i && (t->s[i].t_ms - t->s[j + 1].t_ms) >= SPEED_WINDOW_MS)
            j++;
        if (t->s[i].t_ms - t->s[j].t_ms < SPEED_WINDOW_MS)
            continue;
        dt = (double)(t->s[i].t_ms - t->s[j].t_ms) / 1000.0;
        db = (double)(t->s[i].total_bytes - t->s[j].total_bytes);
        bps = db / dt;
        if (!have || bps > mx)
            mx = bps;
        if (!have || bps < mn)
            mn = bps;
        have = 1;
    }
    if (!have && elapsed > 0) {
        mx = (double)t->total_bytes / elapsed;
        mn = mx;
    }
    if (max_bps != NULL)
        *max_bps = mx;
    if (min_bps != NULL)
        *min_bps = mn;
}

void speed_json(const speed_track *t, void *fv, int indent)
{
    FILE *f = fv;
    size_t start = t->n > 128 ? t->n - 128 : 0;

    fprintf(f, "%*s[\n", indent, "");
    for (size_t i = start; i < t->n; i++) {
        char b[64];
        fmt_size(b, sizeof b, t->s[i].total_bytes);
        fprintf(f, "%*s  { \"t_ms\": %llu, \"written\": \"%s\" }%s\n", indent, "",
                (unsigned long long)t->s[i].t_ms, b, i + 1 == t->n ? "" : ",");
    }
    fprintf(f, "%*s]", indent, "");
}

void stage_stats_reset(stage_stats *s) { memset(s, 0, sizeof *s); }

void stage_stats_merge(stage_stats *d, const stage_stats *s)
{
    if (d->chunks_failed == 0 && s->chunks_failed > 0 && !d->has_first_fail)
        d->has_first_fail = s->has_first_fail;
    if (!d->has_first_fail && s->has_first_fail) {
        d->has_first_fail = 1;
        d->first_fail_off = s->first_fail_off;
        d->first_fail_lba = s->first_fail_lba;
        d->alias_src_off = s->alias_src_off;
        d->has_alias = s->has_alias;
        d->alias_confirmed = s->alias_confirmed;
        d->foreign_data = s->foreign_data;
        d->stale_data = s->stale_data;
        d->unwritten_data = s->unwritten_data;
    }
    d->chunks_written += s->chunks_written;
    d->chunks_verified += s->chunks_verified;
    d->chunks_failed += s->chunks_failed;
    d->bytes_written += s->bytes_written;
    d->bytes_verified += s->bytes_verified;
    d->bad_bytes += s->bad_bytes;
    d->io_errors += s->io_errors;
    d->retries += s->retries;
}
