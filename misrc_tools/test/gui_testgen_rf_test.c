/* Test Gen tape-RF contract: the Simulated device's Channel B frames
 * (gui_testgen_rf_frame) must decode through the GUI's own RF preview
 * decoder for PAL / NTSC / native SECAM VHS, with correct bar hue order.
 * The RF generator is a physical modulator on the shared Test Gen slot
 * grid; this test is the panel-path A/B for it (same philosophy as the
 * SECAM composite verification in TECHNICAL.md). */
#include "../misrc_gui/signal/gui_rf_preview.h"
#include "../misrc_gui/signal/gui_testgen.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RATE 20000000u
#define TEST_FRAMES 12

/* 75% bars, same content as the Test Gen composite channel. */
static const double bars[8][3] = {
    {.75,.75,.75},{.75,.75,0},{0,.75,.75},{0,.75,0},
    {.75,0,.75},{.75,0,0},{0,0,.75},{0,0,0}
};

typedef struct {
    uint8_t *rgba;
    int width, height, frames;
    uint64_t last_end;
    bool ever_colour_locked;
    gui_rf_decode_status_t status;
} frames_t;

static void receive(void *context, const uint8_t *rgba, int width, int height,
                    uint64_t end, const gui_rf_decode_status_t *status) {
    frames_t *f = context;
    assert(width > 0 && width <= GUI_RF_FRAME_MAX_WIDTH);
    assert(height > 0 && height <= GUI_RF_FRAME_MAX_HEIGHT);
    assert(!f->frames || end >= f->last_end);
    memcpy(f->rgba, rgba, (size_t)width * height * 4u);
    f->width = width;
    f->height = height;
    f->frames++;
    f->last_end = end;
    f->status = *status;
    f->ever_colour_locked |= status->color_locked;
}

static double bar_mean(const frames_t *f, int bar, int parity, int channel,
                       int row0, int row1) {
    double sum = 0.0;
    int count = 0;
    int centre = (int)((bar + .5) * f->width / 8.0);
    for (int row = row0 + parity; row < row1; row += 2)
        for (int x = centre - 3; x <= centre + 3; ++x) {
            if (x < 0 || x >= f->width) continue;
            sum += f->rgba[((size_t)row * f->width + x) * 4u + channel];
            count++;
        }
    assert(count > 0);
    return sum / (count * 255.0);
}

/* Hue order per bar: every channel that is ON in the source bar must read
 * clearly brighter than every channel that is OFF, in both woven fields. */
static void check_bar_hues(const frames_t *f, int row0, int row1) {
    for (int bar = 1; bar < 8; ++bar)
        for (int parity = 0; parity < 2; ++parity) {
            double minimum_on = 1.0, maximum_off = 0.0;
            for (int c = 0; c < 3; ++c) {
                double value = bar_mean(f, bar, parity, c, row0, row1);
                if (bars[bar][c] > 0.5) minimum_on = fmin(minimum_on, value);
                else maximum_off = fmax(maximum_off, value);
            }
            if (minimum_on - maximum_off <= 0.20) {
                fprintf(stderr, "bar %d field %d wrong hue: %.3f vs %.3f\n",
                        bar, parity, minimum_on, maximum_off);
            }
            assert(minimum_on - maximum_off > 0.20);
        }
}

static void run_system(gui_testgen_system_t system, gui_rf_profile_t profile,
                       int row0, int row1) {
    const size_t frame_samples =
        gui_testgen_frame_samples(system, TEST_RATE);
    assert(frame_samples > 0);

    int16_t *block = malloc(frame_samples * TEST_FRAMES * sizeof(int16_t));
    float *rf = malloc(frame_samples * TEST_FRAMES * sizeof(float));
    frames_t frames = {0};
    frames.rgba = malloc((size_t)GUI_RF_FRAME_MAX_WIDTH
                         * GUI_RF_FRAME_MAX_HEIGHT * 4u);
    assert(block && rf && frames.rgba);

    for (uint32_t idx = 0; idx < TEST_FRAMES; ++idx) {
        size_t got = gui_testgen_rf_frame(system,
                                          block + (size_t)idx * frame_samples,
                                          frame_samples, TEST_RATE, idx);
        assert(got == frame_samples);
    }
    for (size_t n = 0; n < frame_samples * TEST_FRAMES; ++n) {
        rf[n] = (float)block[n] / 32768.0f;
    }

    gui_rf_decoder_t *decoder = gui_rf_decoder_create_profile(profile);
    assert(decoder);
    /* Feed in uneven chunks like the real worker path does. */
    size_t total = frame_samples * TEST_FRAMES;
    size_t position = 0;
    while (position < total) {
        size_t count = (position & 1) ? 32771u : 4093u;
        if (count > total - position) count = total - position;
        gui_rf_decoder_process(decoder, rf + position, count, position,
                               receive, &frames);
        position += count;
    }

    printf("%s: %d woven frames (%dx%d), H%d V%d C%d, %s\n",
           gui_testgen_system_name(system), frames.frames,
           frames.width, frames.height, frames.status.h_locked,
           frames.status.v_locked, frames.status.color_locked,
           frames.status.reason);
    fflush(stdout);

    assert(frames.frames >= 2);
    assert(frames.status.h_locked && frames.status.v_locked);
    assert(frames.ever_colour_locked);
    assert(frames.status.fields_rejected == frames.status.reject_cadence +
        frames.status.reject_parity + frames.status.reject_line_map);
    check_bar_hues(&frames, row0, row1);

    free(block);
    free(rf);
    free(frames.rgba);
    gui_rf_decoder_destroy(decoder);
}

int main(void) {
    assert(gui_testgen_rf_frame(GUI_TESTGEN_SYSTEM_COUNT, NULL, 0,
                                TEST_RATE, 0) == 0);
    run_system(GUI_TESTGEN_PAL, GUI_RF_PROFILE_PAL_VHS, 200, 300);
    run_system(GUI_TESTGEN_NTSC, GUI_RF_PROFILE_NTSC_VHS, 150, 230);
    run_system(GUI_TESTGEN_SECAM, GUI_RF_PROFILE_SECAM_VHS, 200, 300);
    printf("Test Gen RF: PAL/NTSC/SECAM decode with correct bar hues\n");
    return 0;
}
