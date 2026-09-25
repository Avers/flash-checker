#include "flashcheck/config.h"

#include "flashcheck/io.h"
#include "flashcheck/util.h"

void config_defaults(config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = MODE_STANDARD;
    c->chunk_size = 8u << 20;
    c->window_size = 1ULL << 30;
    c->passes = 1;
    c->bench_bytes = 256ULL << 20;
    c->boundary_budget = 8ULL << 30;
    c->boundary_resolution = 64ULL << 20;
    c->pattern = PAT_CHACHA20;
    c->want_direct = -1;
    c->self_test = -1;
    c->st_reported = 256ULL << 20;
    c->st_real = 256ULL << 20;
}

const char *mode_str(run_mode m)
{
    switch (m) {
    case MODE_IDENTIFY:
        return "identify";
    case MODE_QUICK:
        return "quick";
    case MODE_FULL:
        return "full";
    default:
        return "standard";
    }
}

int config_validate(config *c, char *err, size_t errn)
{
    if (c->self_test >= 0 && !c->st_real_set) {
        if (c->self_test == FAKE_HONEST || c->self_test == FAKE_SLOW)
            c->st_real = c->st_reported;
        else
            c->st_real = c->st_reported / 2;
    }
    if (c->chunk_size < FC_ALIGN || c->chunk_size % FC_ALIGN != 0) {
        snprintf(err, errn, "--block-size must be a multiple of %d", FC_ALIGN);
        return -1;
    }
    if (c->window_size < c->chunk_size) {
        snprintf(err, errn, "--window must be at least one block (%llu bytes)",
                 (unsigned long long)c->chunk_size);
        return -1;
    }
    if (c->passes == 0) {
        snprintf(err, errn, "--passes must be >= 1");
        return -1;
    }
    if (c->offset % c->chunk_size != 0) {
        snprintf(err, errn, "--offset must be a multiple of --block-size (%llu)",
                 (unsigned long long)c->chunk_size);
        return -1;
    }
    if (c->mode != MODE_IDENTIFY && c->mode != MODE_FULL && c->bench_bytes == 0) {
        snprintf(err, errn, "--bench-bytes must be > 0");
        return -1;
    }
    return 0;
}
