#include "flashcheck/visual.h"

#include "flashcheck/util.h"

#include <unistd.h>

static int g_visual = 0;
static uint8_t g_map[VISUAL_MAP_CELLS];
static uint64_t g_start = 0;
static uint64_t g_end = 0;
static uint64_t g_last_draw = 0;
static int g_stage_idx = 0;
static int g_stage_total = 0;

void visual_set_enabled(int on) { g_visual = on ? 1 : 0; }
int visual_is_enabled(void) { return g_visual; }

void visual_run_begin(int total_stages)
{
    g_stage_total = total_stages;
    g_stage_idx = 0;
}

int visual_stage_index(void) { return g_stage_idx; }
int visual_stage_total(void) { return g_stage_total; }

void visual_stage_begin(const char *label, uint64_t start, uint64_t end)
{
    (void)label;
    g_start = start;
    g_end = end;
    memset(g_map, VISUAL_CELL_EMPTY, sizeof g_map);
    g_last_draw = 0;
    g_stage_idx++;
}

void visual_stage_step(void)
{
    g_stage_idx++;
    g_last_draw = 0;
}

/* "[i/N] " prefix, or "" when the total is unknown/single. */
static void stage_tag(char *buf, size_t n)
{
    if (g_stage_total > 1 && g_stage_idx > 0)
        snprintf(buf, n, "[%d/%d] ", g_stage_idx, g_stage_total);
    else
        buf[0] = '\0';
}

void visual_stage_end(void)
{
    /* Single session line: no newline between stages, the next update
       redraws the same line with the new stage label. */
    if (!g_visual)
        return;
    g_last_draw = 0;
}

void visual_finish(void)
{
    if (!g_visual)
        return;
    if (isatty(STDOUT_FILENO))
        fputc('\n', stdout);
    fflush(stdout);
}

int visual_map_index(uint64_t off, uint64_t start, uint64_t end)
{
    uint64_t span;

    if (end <= start)
        return -1;
    if (off < start || off >= end)
        return -1;
    span = end - start;
    return (int)(((off - start) * (uint64_t)VISUAL_MAP_CELLS) / span);
}

void visual_mark(uint64_t off, visual_status st)
{
    int idx;
    uint8_t v;

    if (!g_visual)
        return;
    idx = visual_map_index(off, g_start, g_end);
    if (idx < 0 || idx >= VISUAL_MAP_CELLS)
        return;
    v = (uint8_t)(st + 1); /* VISUAL_OK -> CELL_OK, etc. */
    /* A failure/io-error marker wins over a previous ok marker. */
    if (g_map[idx] == VISUAL_CELL_EMPTY || v != VISUAL_CELL_OK)
        g_map[idx] = v;
}

void visual_bar(char *buf, size_t n, uint64_t done, uint64_t total, int width)
{
    int fill = 0;
    int i = 0;
    int pos = 0;

    if (width <= 0)
        width = VISUAL_BAR_WIDTH;
    if (total > 0)
        fill = (int)((done * (uint64_t)width) / total);
    if (fill < 0)
        fill = 0;
    if (fill > width)
        fill = width;
    pos += snprintf(buf + pos, n > (size_t)pos ? n - (size_t)pos : 0, "[");
    for (i = 0; i < width && (size_t)pos < n; i++)
        pos += snprintf(buf + pos, n > (size_t)pos ? n - (size_t)pos : 0, "%s",
                        i < fill ? "\xe2\x96\x88" : "\xe2\x96\x91");
    if ((size_t)pos < n)
        pos += snprintf(buf + pos, n - (size_t)pos, "]");
}

static const char *cell_glyph(uint8_t v)
{
    switch (v) {
    case VISUAL_CELL_OK:
        return "\xe2\x96\x88"; /* █ tested ok */
    case VISUAL_CELL_FAIL:
        return "#"; /* fail: plain ASCII, survives any terminal */
    case VISUAL_CELL_IOERR:
        return "?"; /* I/O error: plain ASCII */
    default:
        return "\xe2\x96\x91"; /* ░ untested */
    }
}

void visual_map_render(const uint8_t *cells, char *buf, size_t n)
{
    size_t pos = 0;
    int i;

    pos += (size_t)snprintf(buf + pos, n > pos ? n - pos : 0, "[");
    for (i = 0; i < VISUAL_MAP_CELLS && pos < n; i++)
        pos += (size_t)snprintf(buf + pos, n > pos ? n - pos : 0, "%s",
                                cell_glyph(cells[i]));
    if (pos < n)
        pos += (size_t)snprintf(buf + pos, n - pos, "]");
}

void visual_legend(char *buf, size_t n)
{
    snprintf(buf, n, "(░ untested, █ ok, # fail, ? io-error)");
}

const uint8_t *visual_map_cells(void) { return g_map; }

void visual_update(const char *label, uint64_t done, uint64_t total, double bps)
{
    char bar[256], a[64], b[64], r[64], tag[32];
    uint64_t now = now_ms();
    int pct = 0;
    int tty;

    if (!g_visual)
        return;
    /* Throttle like stage_progress (~500ms effective). */
    if (g_last_draw != 0 && now - g_last_draw < 500)
        return;
    g_last_draw = now;

    if (total > 0)
        pct = (int)((done * 100ULL) / total);
    visual_bar(bar, sizeof bar, done, total, VISUAL_BAR_WIDTH);
    fmt_size(a, sizeof a, done);
    fmt_size(b, sizeof b, total);
    fmt_rate(r, sizeof r, bps);
    stage_tag(tag, sizeof tag);

    tty = isatty(STDOUT_FILENO);
    if (tty)
        fprintf(stdout, "\r  %s%s %s %3d%% %s/%s %s", tag, label, bar, pct, a, b, r);
    else
        fprintf(stdout, "  %s%s %s %3d%% %s/%s %s\n", tag, label, bar, pct, a, b, r);
    fflush(stdout);
}

void visual_update_count(const char *label, uint64_t done, uint64_t total)
{
    char bar[256], tag[32];
    uint64_t now = now_ms();
    int pct = 0;
    int tty;

    if (!g_visual)
        return;
    if (g_last_draw != 0 && now - g_last_draw < 500)
        return;
    g_last_draw = now;

    if (total > 0)
        pct = (int)((done * 100ULL) / total);
    visual_bar(bar, sizeof bar, done, total, VISUAL_BAR_WIDTH);
    stage_tag(tag, sizeof tag);

    tty = isatty(STDOUT_FILENO);
    if (tty)
        fprintf(stdout, "\r  %s%s %s %3d%% %llu/%llu", tag, label, bar, pct,
                (unsigned long long)done, (unsigned long long)total);
    else
        fprintf(stdout, "  %s%s %s %3d%% %llu/%llu\n", tag, label, bar, pct,
                (unsigned long long)done, (unsigned long long)total);
    fflush(stdout);
}
