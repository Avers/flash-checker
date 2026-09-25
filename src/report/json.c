#include "flashcheck/report.h"

#include "flashcheck/util.h"

static void json_stage(FILE *f, const stage_report *r)
{
    char a[64];

    fprintf(f, "    {\n");
    fprintf(f, "      \"name\": \"%s\",\n", r->name);
    fprintf(f, "      \"bytes_written\": %llu,\n", (unsigned long long)r->bytes_written);
    fprintf(f, "      \"bytes_verified\": %llu,\n", (unsigned long long)r->bytes_verified);
    fprintf(f, "      \"regions_written\": %llu,\n",
            (unsigned long long)r->regions_written);
    fprintf(f, "      \"regions_verified\": %llu,\n",
            (unsigned long long)r->regions_verified);
    fprintf(f, "      \"write_seconds\": %.3f,\n", r->write_sec);
    fprintf(f, "      \"read_seconds\": %.3f,\n", r->read_sec);
    fprintf(f, "      \"write_bytes_per_sec\": %.1f,\n", r->write_bps);
    fprintf(f, "      \"read_bytes_per_sec\": %.1f,\n", r->read_bps);
    fprintf(f, "      \"peak_bytes_per_sec\": %.1f,\n", r->max_bps);
    fprintf(f, "      \"mismatched_regions\": %llu,\n", (unsigned long long)r->chunks_failed);
    fprintf(f, "      \"mismatched_bytes\": %llu,\n", (unsigned long long)r->bad_bytes);
    fprintf(f, "      \"io_errors\": %llu,\n", (unsigned long long)r->io_errors);
    if (r->has_first_fail) {
        fmt_size(a, sizeof a, r->first_fail_off);
        fprintf(f, "      \"first_failure_offset\": %llu,\n",
                (unsigned long long)r->first_fail_off);
        fprintf(f, "      \"first_failure_offset_human\": \"%s\",\n", a);
    } else {
        fprintf(f, "      \"first_failure_offset\": null,\n");
    }
    if (r->has_alias) {
        fmt_size(a, sizeof a, r->alias_src_off);
        fprintf(f, "      \"aliased_from_offset\": %llu,\n",
                (unsigned long long)r->alias_src_off);
        fprintf(f, "      \"aliased_from_offset_human\": \"%s\",\n", a);
        fprintf(f, "      \"alias_confirmed\": %s,\n", r->alias_confirmed ? "true" : "false");
    }
    if (r->has_capacity) {
        fmt_size(a, sizeof a, r->reliable_capacity);
        fprintf(f, "      \"reliable_capacity_bytes\": %llu,\n",
                (unsigned long long)r->reliable_capacity);
        fprintf(f, "      \"reliable_capacity_human\": \"%s\",\n", a);
    }
    fprintf(f, "      \"note\": \"%s\"\n", r->note);
    fprintf(f, "    }");
}

int report_json(const run_ctx *c, verdict v, const char *path)
{
    FILE *f = fopen(path, "w");
    char a[64], ev[512];

    if (f == NULL) {
        log_warn("cannot write JSON report to %s: %s", path, strerror(errno));
        return -1;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"tool\": \"%s\",\n", FC_PROG);
    fprintf(f, "  \"version\": \"%s\",\n", FC_VERSION);
    device_json(&c->dev[0], f, 2);
    fprintf(f, ",\n");
    fprintf(f, "  \"run\": {\n");
    fprintf(f, "    \"mode\": \"%s\",\n", mode_str(c->cfg->mode));
    fprintf(f, "    \"destructive\": %s,\n", c->cfg->destructive ? "true" : "false");
    fprintf(f, "    \"pattern\": \"%s\",\n", pattern_kind_str(c->cfg->pattern));
    fprintf(f, "    \"chunk_size\": %llu,\n", (unsigned long long)c->cfg->chunk_size);
    fprintf(f, "    \"window_size\": %llu,\n", (unsigned long long)c->cfg->window_size);
    fprintf(f, "    \"passes\": %llu,\n", (unsigned long long)c->cfg->passes);
    fprintf(f, "    \"backend\": \"%s\",\n", c->io->name);
    fprintf(f, "    \"o_direct_active\": %s,\n",
            c->io->stats->direct_active ? "true" : "false");
    fprintf(f, "    \"test_id\": %llu\n", (unsigned long long)c->test_id);
    fprintf(f, "  },\n");
    fmt_size(a, sizeof a, c->tested_bytes);
    fprintf(f, "  \"bytes_written\": %llu,\n", (unsigned long long)c->tested_bytes);
    fprintf(f, "  \"bytes_written_human\": \"%s\",\n", a);
    fprintf(f, "  \"tested_range\": { \"offset\": %llu, \"end\": %llu },\n",
            (unsigned long long)ctx_start(c), (unsigned long long)ctx_end(c));
    if (c->has_capacity) {
        fmt_size(a, sizeof a, c->reliable_capacity);
        fprintf(f, "  \"reliable_capacity_bytes\": %llu,\n",
                (unsigned long long)c->reliable_capacity);
        fprintf(f, "  \"reliable_capacity_human\": \"%s\",\n", a);
    } else {
        fprintf(f, "  \"reliable_capacity_bytes\": null,\n");
    }
    fprintf(f, "  \"stages\": [\n");
    for (size_t i = 0; i < c->nst; i++) {
        json_stage(f, &c->st[i]);
        fprintf(f, "%s\n", i + 1 == c->nst ? "" : ",");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"verdict\": \"%s\",\n", verdict_str(v));
    fprintf(f, "  \"evidence\": \"%s\"\n", report_evidence(c, ev, sizeof ev));
    fprintf(f, "}\n");
    if (fclose(f) != 0) {
        log_warn("error closing JSON report %s", path);
        return -1;
    }
    return 0;
}
