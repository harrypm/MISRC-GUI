/* Field order contract, RF Preview decoder (harrypm 2026-11-19: first field
 * on top, second below in the Split view, matching the Spectro's f1 / f2).
 *
 * Test Gen RF frames each open with field 1's pre-equalising train (the
 * definition gui_field_locate_test pins the Spectro's f1 to). The decoder
 * publishes a frame when a field completes, with that field's parity in
 * status->last_field_parity and its end in `end`. A field that ends at a
 * Test Gen frame start is field 2; one ending mid-frame is field 1. `end`'s
 * unit is the decoder's own, so the scale comes from the field spacing. The
 * test asserts gui_rf_field1_parity() per profile and that gui_rf_split_row
 * puts field 1's rows in the top block (one to one). */
#include "../misrc_gui/signal/gui_rf_preview.c"
#include "../misrc_gui/signal/gui_testgen.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RATE 20000000u
#define TEST_FRAMES 10
#define MAX_EVENTS 64

typedef struct {
    int n;
    uint64_t end[MAX_EVENTS];
    int parity[MAX_EVENTS];
    double start[MAX_EVENTS];   // the published field's own start (decoder domain)
    int dparity[MAX_EVENTS];    // ... and the decoder's own parity for it
    int height;
    const gui_rf_decoder_t *decoder;
} events_t;

static int failures;

static void receive(void *context, const uint8_t *rgba, int width, int height,
                    uint64_t end, const gui_rf_decode_status_t *status) {
    (void)rgba; (void)width;
    events_t *e = context;
    e->height = height;
    if (e->n < MAX_EVENTS) {
        e->end[e->n] = end;
        e->parity[e->n] = status->last_field_parity;
        e->start[e->n] = e->decoder->field_start;
        e->dparity[e->n] = e->decoder->field_parity;
        e->n++;
    }
}

static void run(const char *name, gui_testgen_system_t system, gui_rf_profile_t profile) {
    const size_t fn = gui_testgen_frame_samples(system, TEST_RATE);
    int16_t *block = malloc(fn * TEST_FRAMES * sizeof(int16_t));
    float *rf = malloc(fn * TEST_FRAMES * sizeof(float));
    assert(block && rf);
    for (uint32_t i = 0; i < TEST_FRAMES; i++)
        assert(gui_testgen_rf_frame(system, GUI_TESTGEN_FORMAT_VHS, block + (size_t)i * fn, fn,
                                    TEST_RATE, i) == fn);
    for (size_t n = 0; n < fn * TEST_FRAMES; n++) rf[n] = (float)block[n] / 32768.0f;

    gui_rf_decoder_t *decoder = gui_rf_decoder_create_profile(profile);
    assert(decoder);
    events_t ev = {0};
    ev.decoder = decoder;
    for (size_t pos = 0; pos < fn * TEST_FRAMES;) {
        size_t count = (pos & 1) ? 32771u : 4093u;
        if (count > fn * TEST_FRAMES - pos) count = fn * TEST_FRAMES - pos;
        gui_rf_decoder_process(decoder, rf + pos, count, pos, receive, &ev);
        pos += count;
    }
    gui_rf_decoder_destroy(decoder);

    // Units per input sample from the field spacing (half a frame).
    int f1 = 0, f2 = 0, f1_bad = 0, f2_bad = 0, off_grid = 0;
    const int want1 = gui_rf_field1_parity(profile);
    if (ev.n >= 4) {
        // The published field's own start. The decoder works at 5 samples per
        // us (rf line = line_us * 5; its frame `end` is start * 4 at the
        // 20 MS/s input), so input samples = start * TEST_RATE / 5 MHz. The
        // decoder publishes once per FRAME, so the spacing is no scale.
        const double units = 5000000.0 / (double)TEST_RATE;
        for (int i = 1; i < ev.n; i++) {
            if (ev.parity[i] != ev.dparity[i]) {
                printf("  %s: status parity %d != decoder parity %d\n", name, ev.parity[i], ev.dparity[i]);
                f1_bad++;
            }
            double at = ev.start[i] / units;                    // input samples
            double phase = fmod(at, (double)fn) / (double)fn;   // 0 = frame start
            if (phase > 0.75) phase -= 1.0;
            if (getenv("RF_FIELD_ORDER_TRACE"))
                printf("  %s: start phase %.4f parity %d\n", name, phase, ev.dparity[i]);
            if (fabs(phase) < 0.02) {                 // starts at a frame start: field 1
                f1++; if (ev.parity[i] != want1) f1_bad++;
            } else if (fabs(phase - 0.5) < 0.02) {    // starts mid-frame: field 2
                f2++; if (ev.parity[i] == want1) f2_bad++;
            } else {
                off_grid++;
                if (off_grid <= 3) printf("  %s: field start at frame phase %.4f\n", name, phase);
            }
        }
    }
    // Split order over the published frame height.
    int top_rows = 0, split_bad = 0;
    const int rows = ev.height;
    (void)gui_rf_split_row(0, rows, want1, &top_rows);
    static unsigned char used[2048];
    memset(used, 0, sizeof(used));
    for (int r = 0; r < rows; r++) {
        const int d = gui_rf_split_row(r, rows, want1, NULL);
        const bool field1 = (r & 1) == want1;
        if (d < 0 || d >= rows || used[d]++ || field1 != (d < top_rows)) split_bad++;
    }
    // The decoder publishes once per frame, as field 1 completes (measured),
    // so every publish must be a field 1 start on the Test Gen frame grid.
    const bool ok = f1 >= 3 && f2 == 0 && !f1_bad && !f2_bad && !off_grid && rows > 0 && !split_bad;
    printf("%-6s field 1 parity %d (%d seen, %d wrong), field 2 (%d seen, %d wrong), %d off grid, "
           "Split: field 1 on top (%d of %d rows), %d bad: %s\n", name, want1, f1, f1_bad, f2, f2_bad,
           off_grid, top_rows, rows, split_bad, ok ? "ok" : "FAIL");
    if (!ok) failures++;
    free(block);
    free(rf);
}

int main(void) {
    run("PAL", GUI_TESTGEN_PAL, GUI_RF_PROFILE_PAL_VHS);
    run("NTSC", GUI_TESTGEN_NTSC, GUI_RF_PROFILE_NTSC_VHS);
    run("SECAM", GUI_TESTGEN_SECAM, GUI_RF_PROFILE_SECAM_VHS);
    if (failures) { printf("%d RF field order failure(s)\n", failures); return 1; }
    puts("RF field order: field 1 / 2 parity fixed per system; Split shows field 1 on top");
    return 0;
}
