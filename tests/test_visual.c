#include "test_util.h"

#include "flashcheck/visual.h"

#include <langinfo.h>
#include <locale.h>
#include <strings.h>
#include <unistd.h>

static int test_locale_is_utf8(void)
{
    const char *codeset;

    (void)setlocale(LC_CTYPE, "");
    codeset = nl_langinfo(CODESET);
    return strcasecmp(codeset, "UTF-8") == 0 || strcasecmp(codeset, "UTF8") == 0;
}

static void test_progress_spinner(void)
{
    FILE *capture = tmpfile();
    char first[256], second[256];
    const char *first_frame = test_locale_is_utf8() ? "⠋" : "|";
    const char *second_frame = test_locale_is_utf8() ? "⠙" : "/";
    int saved_stdout;

    T_BEGIN("progress shows a rotating spinner before the stage counter");
    CHECK(capture != NULL);
    if (capture == NULL)
        return;

    saved_stdout = dup(STDOUT_FILENO);
    CHECK(saved_stdout >= 0);
    if (saved_stdout < 0) {
        fclose(capture);
        return;
    }
    fflush(stdout);
    if (dup2(fileno(capture), STDOUT_FILENO) != STDOUT_FILENO) {
        CHECK(0);
        close(saved_stdout);
        fclose(capture);
        return;
    }

    visual_set_enabled(1);
    visual_run_begin(2);
    visual_stage_begin("write", 0, 100);
    visual_update("write", 0, 100, 0.0);
    visual_stage_step();
    visual_update("read", 0, 100, 0.0);
    fflush(stdout);

    CHECK(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved_stdout);
    visual_set_enabled(0);

    rewind(capture);
    if (fgets(first, sizeof first, capture) == NULL) {
        CHECK(0);
    } else {
        char expected[64];
        snprintf(expected, sizeof expected, "  %s [1/2] write", first_frame);
        CHECK(strncmp(first, expected, strlen(expected)) == 0);
    }
    if (fgets(second, sizeof second, capture) == NULL) {
        CHECK(0);
    } else {
        char expected[64];
        snprintf(expected, sizeof expected, "  %s [2/2] read", second_frame);
        CHECK(strncmp(second, expected, strlen(expected)) == 0);
    }
    fclose(capture);
}

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

    test_progress_spinner();

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
