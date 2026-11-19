/* Cut markers contract: marks belong to the file they were set on, IN stays
 * before OUT (a crossing mark drops the other), a --markers preset binds to
 * the first file shown, and the FLAC-Chop line carries exact sample counts
 * with the path quoted for the host shell. */
#include "../misrc_gui/input/gui_cut_markers.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, what)                                   \
    do {                                                    \
        if (!(cond)) { printf("FAIL: %s\n", what); failures++; } \
    } while (0)

int main(void) {
    const char *a = "C:/caps/tape1.flac", *b = "C:/caps/tape2.flac";
    gui_cut_marks_t m = gui_cut_marks_get(0, a);
    CHECK(!m.has_in && !m.has_out, "starts empty");

    CHECK(gui_cut_marks_set(0, a, false, 1000) == GUI_CUT_SET_OK, "set IN");
    CHECK(gui_cut_marks_set(0, a, true, 5000) == GUI_CUT_SET_OK, "set OUT");
    m = gui_cut_marks_get(0, a);
    CHECK(m.has_in && m.in == 1000 && m.has_out && m.out == 5000, "IN/OUT read back");
    m = gui_cut_marks_get(0, b);
    CHECK(!m.has_in && !m.has_out, "another file hides the marks");
    m = gui_cut_marks_get(1, a);
    CHECK(!m.has_in && !m.has_out, "channels are independent");

    CHECK(gui_cut_marks_set(0, a, false, 6000) == GUI_CUT_SET_DROPPED_OTHER, "IN past OUT reported");
    m = gui_cut_marks_get(0, a);
    CHECK(m.has_in && m.in == 6000 && !m.has_out, "IN past OUT drops OUT");
    CHECK(gui_cut_marks_set(0, a, true, 6000) == GUI_CUT_SET_DROPPED_OTHER, "OUT on IN reported");
    m = gui_cut_marks_get(0, a);
    CHECK(!m.has_in && m.has_out && m.out == 6000, "OUT at IN drops IN");

    gui_cut_marks_set(0, b, false, 42);
    m = gui_cut_marks_get(0, b);
    CHECK(m.has_in && m.in == 42 && !m.has_out, "setting on a new file starts fresh");
    m = gui_cut_marks_get(0, a);
    CHECK(!m.has_in && !m.has_out, "old file's marks are gone");

    gui_cut_marks_clear(0);
    m = gui_cut_marks_get(0, b);
    CHECK(!m.has_in && !m.has_out, "clear");

    gui_cut_marks_preset(1, 123456789012ULL, UINT64_MAX);
    m = gui_cut_marks_get(1, b);
    CHECK(m.has_in && m.in == 123456789012ULL && !m.has_out, "preset binds to the first file");
    m = gui_cut_marks_get(1, a);
    CHECK(!m.has_in, "preset bound to that file only");
    gui_cut_marks_preset(1, 500, 100);
    m = gui_cut_marks_get(1, a);
    CHECK(m.has_in && m.in == 500 && !m.has_out, "preset OUT before IN dropped");

    char cmd[512];
    gui_cut_marks_t both = { true, true, 20000000ULL, 1340000000ULL };
    CHECK(gui_cut_marks_flac_chop_command(&both, "C:/caps/my tape.flac", cmd, sizeof(cmd)), "command");
#ifdef _WIN32
    const char *want = "flac-chop --gui \"C:/caps/my tape.flac\" --in 20000000 --out 1340000000 --units samples";
#else
    const char *want = "flac-chop --gui 'C:/caps/my tape.flac' --in 20000000 --out 1340000000 --units samples";
#endif
    CHECK(strcmp(cmd, want) == 0, "command text");
    printf("  %s\n", cmd);
#ifndef _WIN32
    CHECK(gui_cut_marks_flac_chop_command(&both, "/caps/it's.flac", cmd, sizeof(cmd)) &&
          strstr(cmd, "'/caps/it'\\''s.flac'") != NULL, "POSIX quote escape");
#endif
    gui_cut_marks_t in_only = { true, false, 7, 0 };
    CHECK(gui_cut_marks_flac_chop_command(&in_only, "x.flac", cmd, sizeof(cmd)) &&
          strstr(cmd, "--in 7 --units samples") && !strstr(cmd, "--out"), "IN only");
    CHECK(!gui_cut_marks_flac_chop_command(&both, "x.flac", cmd, 20), "too small buffer fails");
    CHECK(!gui_cut_marks_flac_chop_command(&both, "", cmd, sizeof(cmd)), "no file fails");

    if (failures) { printf("%d cut marker failure(s)\n", failures); return 1; }
    puts("Cut markers: per-file, IN before OUT, preset binding, FLAC-Chop command ok");
    return 0;
}
