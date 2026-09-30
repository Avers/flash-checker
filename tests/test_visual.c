#include "test_util.h"

#include "flashcheck/visual.h"

void test_visual(void)
{
    char buf[256];
    char map[VISUAL_MAP_STR];

    T_BEGIN("visual bar empty");
    visual_bar(buf, sizeof buf, 0, 100, 4);
    CHECK(strstr(buf, "[") == buf);
    CHECK(strlen(buf) > 6);

    T_BEGIN("visual bar full");
    {
        char full[256], empty[256];
        visual_bar(full, sizeof full, 100, 100, 4);
        visual_bar(empty, sizeof empty, 0, 100, 4);
        CHECK(strcmp(full, empty) != 0);
    }

    T_BEGIN("visual bar zero total");
    visual_bar(buf, sizeof buf, 0, 0, 4);
    CHECK(strstr(buf, "[") == buf);

    T_BEGIN("visual map index");
    CHECK(visual_map_index(0, 0, 640) == 0);
    CHECK(visual_map_index(639, 0, 640) == VISUAL_MAP_CELLS - 1);
    CHECK(visual_map_index(320, 0, 640) == VISUAL_MAP_CELLS / 2);
    CHECK(visual_map_index(640, 0, 640) == -1);
    CHECK(visual_map_index(0, 100, 100) == -1);
    CHECK(visual_map_index(50, 100, 200) == -1);

    T_BEGIN("visual map render");
    {
        uint8_t cells[VISUAL_MAP_CELLS];
        memset(cells, VISUAL_CELL_EMPTY, sizeof cells);
        cells[0] = VISUAL_CELL_OK;
        cells[1] = VISUAL_CELL_FAIL;
        cells[2] = VISUAL_CELL_IOERR;
        visual_map_render(cells, map, sizeof map);
        CHECK(map[0] == '[');
        CHECK(strncmp(map + 1, "\xe2\x96\x88", 3) == 0); /* █ ok */
        CHECK(map[4] == '#'); /* fail piped */
        CHECK(map[5] == '?'); /* io-error piped */
        CHECK(strncmp(map + 6, "\xe2\x96\x91", 3) == 0); /* ░ untested */
        CHECK(map[strlen(map) - 1] == ']');
    }

    T_BEGIN("visual mark + cells");
    visual_set_enabled(1);
    visual_stage_begin("test", 0, 640);
    visual_mark(0, VISUAL_OK);
    visual_mark(639, VISUAL_FAIL);
    CHECK(visual_map_cells()[0] == VISUAL_CELL_OK);
    CHECK(visual_map_cells()[VISUAL_MAP_CELLS - 1] == VISUAL_CELL_FAIL);
    /* Fail wins over ok. */
    visual_mark(0, VISUAL_FAIL);
    CHECK(visual_map_cells()[0] == VISUAL_CELL_FAIL);
    visual_mark(0, VISUAL_OK);
    CHECK(visual_map_cells()[0] == VISUAL_CELL_FAIL);
    visual_set_enabled(0);

    T_BEGIN("visual stage counter");
    visual_run_begin(5);
    CHECK(visual_stage_total() == 5);
    CHECK(visual_stage_index() == 0);
    visual_stage_begin("a", 0, 100);
    CHECK(visual_stage_index() == 1);
    visual_stage_begin("b", 0, 100);
    CHECK(visual_stage_index() == 2);
    visual_run_begin(0);
    CHECK(visual_stage_total() == 0);
    CHECK(visual_stage_index() == 0);

    T_BEGIN("visual stage step");
    visual_run_begin(3);
    visual_stage_begin("a", 0, 100);
    CHECK(visual_stage_index() == 1);
    visual_stage_step();
    CHECK(visual_stage_index() == 2);

    T_BEGIN("visual map legend");
    {
        char legend[64];
        visual_legend(legend, sizeof legend);
        CHECK(strstr(legend, "# fail") != NULL);
        CHECK(strstr(legend, "? io-error") != NULL);
        CHECK(strstr(legend, "\x1b[") == NULL); /* never any color escapes */
    }
}
