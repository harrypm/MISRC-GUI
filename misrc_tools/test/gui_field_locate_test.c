/* Field locator contract: on Test Gen frames (exact half-line slot grid;
 * each frame opens with its pre-equalising train) the field starts land on
 * each standard's line 1 / its second-field start within half a line, with
 * field 1 / 2 parity, for CVBS and VHS FM RF, PAL and NTSC, from an
 * arbitrary run start. */
#include "../misrc_gui/signal/gui_field_locate.h"
#include "../misrc_gui/signal/gui_testgen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static void run(const char *name, gui_testgen_system_t sys, bool rf, uint32_t rate) {
    const int lines = sys == GUI_TESTGEN_NTSC ? 525 : 625;
    const size_t fn = gui_testgen_frame_samples(sys, rate);
    const int frames = 3;
    int16_t *buf = malloc(fn * frames * sizeof(int16_t));
    int32_t *x = malloc(fn * frames * sizeof(int32_t));
    if (!buf || !x) { puts("alloc"); exit(1); }
    for (int f = 0; f < frames; f++) {
        size_t got = rf ? gui_testgen_rf_frame(sys, GUI_TESTGEN_FORMAT_VHS, buf + (size_t)f * fn, fn, rate, (uint32_t)f)
                        : gui_testgen_frame(sys, buf + (size_t)f * fn, fn, rate, (uint32_t)f);
        if (got != fn) { printf("%s: generate failed\n", name); failures++; return; }
    }
    for (size_t i = 0; i < fn * frames; i++) x[i] = buf[i];
    // Start the run 0.3 field in (an arbitrary window start).
    const size_t off = fn * 3 / 20;
    const uint64_t abs0 = 1000000;   // absolute position of x[off]
    gui_field_locate_t fl = {0};
    gui_field_locate_begin(&fl, rf ? GUI_FIELD_LOCATE_FM : GUI_FIELD_LOCATE_CVBS, (double)rate, lines,
                           abs0, fn * frames - off);
    for (size_t at = off; at < fn * frames; at += 4096) {
        size_t n = fn * frames - at < 4096 ? fn * frames - at : 4096;
        gui_field_locate_push(&fl, x + at, n);
    }
    gui_field_mark_t m[16];
    size_t nm = gui_field_locate_finish(&fl, m, 16);
    gui_field_locate_free(&fl);
    const double half = (double)fn / 2.0, line = (double)fn / (double)lines;
    // The Test Gen's frame opens with the pre-equalising train (gui_testgen.c:
    // PAL 5 EQ slots = 2.5 lines, NTSC 6 = 3 lines), then the broad pulses.
    // Line 1 by each standard: 625 lines - the first broad pulse (2.5 lines
    // in); 525 lines - the first pre-equalising pulse (sample 0).
    const double line1 = sys == GUI_TESTGEN_NTSC ? 0.0 : 2.5 * line;
    double worst = 0.0;
    bool parity_ok = true;
    for (size_t i = 0; i < nm; i++) {
        const double s = (double)(m[i].pos - abs0) + (double)off - line1;   // from frame 0 line 1
        const double k = floor(s / half + 0.5);                        // field index (0 = frame 0 field 1)
        const double err = fabs(s - k * half) / line;
        if (err > worst) worst = err;
        const int want = ((long)k % 2 == 0) ? 1 : 2;
        if (m[i].field != want) parity_ok = false;
    }
    // A 2.7-frame run holds 5 whole field starts (fields 2..6 of frames 0..2).
    const bool ok = nm >= 4 && worst < 0.5 && parity_ok;
    printf("%-14s %zu field starts, worst %.2f lines off, parity %s %s\n", name, nm, worst,
           parity_ok ? "ok" : "WRONG", ok ? "ok" : "FAIL");
    for (size_t i = 0; i < nm && !ok; i++) {
        const double s = (double)(m[i].pos - abs0) + (double)off - line1;
        printf("    mark %zu at %.1f lines from line 1 (field %d)\n", i, s / line, m[i].field);
    }
    if (!ok) failures++;
    free(buf);
    free(x);
}

int main(void) {
    run("PAL CVBS", GUI_TESTGEN_PAL, false, 20000000u);
    run("NTSC CVBS", GUI_TESTGEN_NTSC, false, 20000000u);
    run("PAL VHS RF", GUI_TESTGEN_PAL, true, 20000000u);
    run("NTSC VHS RF", GUI_TESTGEN_NTSC, true, 20000000u);
    run("PAL CVBS 40M", GUI_TESTGEN_PAL, false, 40000000u);
    run("PAL VHS RF 40M", GUI_TESTGEN_PAL, true, 40000000u);
    if (failures) { printf("%d field locate failure(s)\n", failures); return 1; }
    puts("Field locator: field starts on line 1 / 313 (263) within half a line, parity correct");
    return 0;
}
