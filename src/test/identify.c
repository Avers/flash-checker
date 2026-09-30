#include "flashcheck/test.h"

#include "flashcheck/util.h"
#include "flashcheck/visual.h"

int stage_identify(run_ctx *c, stage_report *r)
{
    if (visual_is_enabled() && c->cfg->mode != MODE_IDENTIFY) {
        char cap[64];

        fmt_size(cap, sizeof cap, c->dev->capacity);
        log_out("Checking %s (%s)...", c->dev->path, cap);
    } else {
        device_print_info(c->dev);
    }
    snprintf(r->note, sizeof r->note, "O_DIRECT %s, backend %s",
             c->io->stats->direct_active ? "active" : "inactive", c->io->name);
    return 0;
}
