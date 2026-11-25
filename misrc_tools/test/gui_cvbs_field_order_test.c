/* Field order contract, CVBS decoder (harrypm 2026-11-19: "first [field, as
 * on the spectrogram] should be top and second bottom in split view modes -
 * ensure this is enforced").
 *
 * The Spectro's f1 / f2 (signal/gui_field_locate.c) is ITU field 1 / 2: on
 * the Test Gen every frame opens with field 1's pre-equalising train, and
 * gui_field_locate_test pins the locator to that. Here the same Test Gen
 * frames run through the production CVBS decoder; every accepted field's
 * start is logged with its parity. Field 1 is the field whose start sits at
 * a Test Gen frame start (within a few lines). The test then asserts:
 *   - every field 1 has the parity gui_cvbs_field1_parity() reports, and
 *     every field 2 the other one (PAL and NTSC);
 *   - the Split view's top block is that field's rows
 *     (gui_cvbs_split_top_parity), i.e. field 1 on top, field 2 below. */
#define CVBS_LIVE_DIAG 1
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/signal/gui_testgen.h"
#include <assert.h>

void UnloadTexture(Texture2D texture) { (void)texture; }
void TraceLog(int logLevel, const char *text, ...) { (void)logLevel; (void)text; }

static int failures;

static void run(const char *name, gui_testgen_system_t sys, int format_select, uint32_t rate) {
    const size_t fn = gui_testgen_frame_samples(sys, rate);
    const int frames = 8;
    int16_t *sig = malloc(fn * frames * sizeof(int16_t));
    assert(sig);
    for (int f = 0; f < frames; f++)
        assert(gui_testgen_frame(sys, sig + (size_t)f * fn, fn, rate, (uint32_t)f) == fn);

    static cvbs_decoder_t decoder;
    memset(&decoder, 0, sizeof(decoder));
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, format_select);
    const int lines = sys == GUI_TESTGEN_NTSC ? 525 : 625;
    const double line = (double)fn / (double)lines;

    size_t last_start = (size_t)-1;
    int f1 = 0, f2 = 0, f1_bad = 0, f2_bad = 0;
    const int want1 = gui_cvbs_field1_parity(&decoder);
    for (size_t pos = 0; pos < fn * frames;) {
        size_t chunk = fn * frames - pos < 2048 ? fn * frames - pos : 2048;
        gui_cvbs_process_buffer(&decoder, sig + pos, chunk, rate);
        pos += chunk;
        const cvbs_vsync_state_t *vs = &decoder.vsync;
        if (!vs->have_field || vs->field_start == last_start) continue;
        last_start = vs->field_start;
        if (vs->field_start < fn) continue;   // let the decoder lock first
        // Field start relative to the Test Gen frame grid, in lines.
        double off = fmod((double)vs->field_start, (double)fn) / line;
        if (off > lines / 2.0 + lines / 4.0) off -= lines;
        const bool is_f1 = fabs(off) < 4.0;
        const bool is_f2 = fabs(off - lines / 2.0) < 4.0;
        if (!is_f1 && !is_f2) {
            printf("  %s: field start %.2f lines off the grid\n", name, off);
            failures++;
            continue;
        }
        if (is_f1) { f1++; if (vs->field_parity != want1) f1_bad++; }
        else       { f2++; if (vs->field_parity == want1) f2_bad++; }
    }
    // The weave rule the Split mapping relies on: re-weave the decoder's two
    // field buffers and confirm each frame row came from the field
    // gui_cvbs_row_parity names.
    decoder.field_ready[0] = decoder.field_ready[1] = true;
    decoder.field_last_update_seq[0] = 1;
    decoder.field_last_update_seq[1] = 2;
    deinterlace_fields(&decoder);
    const size_t row_bytes = (size_t)decoder.frame_width * 3u;
    const int rows = decoder.frame_height;
    int weave_bad = 0;
    for (int r = 20; r < rows - 20; r++) {
        const int p = gui_cvbs_row_parity(r);
        const uint8_t *frow = decoder.frame_buffer + (size_t)r * row_bytes;
        const uint8_t *src = decoder.field_buffer[p] + (size_t)(r >> 1) * row_bytes;
        if (memcmp(frow, src, row_bytes) != 0) weave_bad++;
    }
    // Split order: field 1's rows form the top block, field 2's the block
    // below, each in order, every texture row used once.
    const int top = gui_cvbs_split_top_parity(&decoder);
    int top_rows = 0, split_bad = 0;
    (void)gui_cvbs_split_row(0, rows, top, &top_rows);
    static unsigned char used[2048];
    memset(used, 0, sizeof(used));
    for (int r = 0; r < rows; r++) {
        const int d = gui_cvbs_split_row(r, rows, top, NULL);
        const bool field1 = gui_cvbs_row_parity(r) == want1;
        if (d < 0 || d >= rows || used[d]++ || field1 != (d < top_rows)) split_bad++;
    }
    const bool ok = f1 >= 3 && f2 >= 3 && !f1_bad && !f2_bad && top == want1 && !weave_bad && !split_bad;
    printf("%-10s field 1 parity %d (%d seen, %d wrong), field 2 (%d seen, %d wrong), "
           "weave rows wrong %d, Split: field 1 on top (%d rows), %d bad: %s\n", name, want1, f1, f1_bad,
           f2, f2_bad, weave_bad, top_rows, split_bad, ok ? "ok" : "FAIL");
    if (!ok) failures++;
    gui_cvbs_cleanup(&decoder);
    free(sig);
}

int main(void) {
    run("PAL", GUI_TESTGEN_PAL, 0, 40000000u);
    run("NTSC", GUI_TESTGEN_NTSC, 1, 40000000u);
    run("PAL 20M", GUI_TESTGEN_PAL, 0, 20000000u);
    if (failures) { printf("%d CVBS field order failure(s)\n", failures); return 1; }
    puts("CVBS field order: field 1 / 2 parity fixed per system; Split shows field 1 on top");
    return 0;
}
