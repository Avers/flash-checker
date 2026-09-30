#ifndef FLASHCHECK_VISUAL_H
#define FLASHCHECK_VISUAL_H

#include "flashcheck/common.h"

#define VISUAL_MAP_CELLS 64
#define VISUAL_BAR_WIDTH 24
/* Rendered map string: 3 bytes per block glyph plus "[" + "]" + NUL. */
#define VISUAL_MAP_STR (VISUAL_MAP_CELLS * 3 + 3)

/* Chunk status for the map. */
typedef enum {
    VISUAL_OK = 0,
    VISUAL_FAIL = 1,
    VISUAL_IOERR = 2
} visual_status;

/* Global on/off switch, driven by -v/--verbose. */
void visual_set_enabled(int on);
int visual_is_enabled(void);

/* Start of a run: total planned progress steps (quick=3, standard=5,
   adaptive/full=6, bench=2). Resets the step counter. */
void visual_run_begin(int total_stages);
/* Advance to the next step without resetting the chunk map (e.g. the
   benchmark's write pass and read pass are two steps of one stage). */
void visual_stage_step(void);
int visual_stage_index(void);
int visual_stage_total(void);

/* Range the current stage covers; map cells are spread evenly over it. */
void visual_stage_begin(const char *label, uint64_t start, uint64_t end);
void visual_stage_end(void);

/* Mark one chunk offset with a status. No-op when disabled or out of range. */
void visual_mark(uint64_t off, visual_status st);

/* Progress line: bar + percent + done/total + rate. Respects TTY vs pipe. */
void visual_update(const char *label, uint64_t done, uint64_t total, double bps);

/* Count variant for non-byte progress (e.g. sparse probes): "1/4". */
void visual_update_count(const char *label, uint64_t done, uint64_t total);

/* End the single live line before printing the final summary. */
void visual_finish(void);

/* Pure helpers, unit-testable (no I/O, no globals). */
void visual_bar(char *buf, size_t n, uint64_t done, uint64_t total, int width);
int visual_map_index(uint64_t off, uint64_t start, uint64_t end);
/* Chunk-map cell states (single byte each; rendered as block glyphs). */
typedef enum {
    VISUAL_CELL_EMPTY = 0,
    VISUAL_CELL_OK = 1,
    VISUAL_CELL_FAIL = 2,
    VISUAL_CELL_IOERR = 3
} visual_cell;

void visual_map_render(const uint8_t *cells, char *buf, size_t n);
/* Legend matching the render above. */
void visual_legend(char *buf, size_t n);
const uint8_t *visual_map_cells(void);

#endif
