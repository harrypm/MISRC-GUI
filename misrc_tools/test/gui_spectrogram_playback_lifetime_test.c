/* CPU-only regression for the production spectrogram palette reload.
 * Playback publications are borrowed for one render pass; a stop releases
 * them, and a reconnect may not have published its replacement yet. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../misrc_gui/visualization/gui_spectrogram.c"

static playback_state_t test_state;
static float *test_magnitudes;
static bool test_ready;
static uint64_t test_seq;
static unsigned test_polls;
static int test_channel;

playback_state_t gui_playback_get_state(gui_app_t *app) {
    (void)app;
    return test_state;
}

/* Stubs: the per-channel held gate (2026-11-03) checks the session identity
 * before polling. Model the harness sessions: running unless stopped, and a
 * master pause holds every track. */
bool gui_playback_is_running(gui_app_t *app) {
    (void)app;
    return test_state != PLAYBACK_STATE_STOPPED;
}

bool gui_playback_is_channel_paused(gui_app_t *app, int channel) {
    (void)app;
    assert(channel == test_channel);
    return test_state == PLAYBACK_STATE_PAUSED;
}

uint64_t gui_playback_get_position_samples_channel(gui_app_t *app, int channel) {
    (void)app;
    assert(channel == test_channel);
    return 42;
}

void gui_playback_request_analysis_window(gui_app_t *app, int channel,
                                          uint64_t center, double span) {
    (void)app; (void)center; (void)span;
    assert(channel == test_channel);
}

bool gui_playback_poll_analysis_window(gui_app_t *app, int channel,
                                       uint64_t *seq, const float **magnitudes,
                                       size_t *slices, size_t *bins) {
    (void)app;
    assert(channel == test_channel);
    test_polls++;
    if (!test_ready || test_state != PLAYBACK_STATE_PAUSED ||
        (seq && *seq >= test_seq)) return false;
    assert(test_magnitudes);
    if (seq) *seq = test_seq;
    if (magnitudes) *magnitudes = test_magnitudes;
    if (slices) *slices = 2;
    if (bins) *bins = 3;
    return true;
}

static bool pixels_zero(const unsigned char *pixels, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        if (pixels[i] != 0) return false;
    }
    return true;
}

static void test_palette_session(int channel) {
    spectro_engine_t engine = {0};
    float frame[3] = {0};
    unsigned char pixels[24] = {0};
    unsigned char previous[24];
    engine.time_on_x = channel == 1;
    engine.freq_bins = 3;
    engine.time_bins = 2;
    engine.tex_w = engine.time_on_x ? 2 : 3;
    engine.tex_h = engine.time_on_x ? 3 : 2;
    engine.frame = frame;
    engine.pixels = pixels;
    engine.analysis_active = true;
    engine.analysis_span_seconds = 3.0;
    build_lut(engine.lut_phosphor);
    build_heat_lut(engine.lut_heat);
    test_channel = channel;
    test_polls = 0;
    test_state = PLAYBACK_STATE_PAUSED;
    test_ready = true;
    test_seq = 7;
    test_magnitudes = malloc(6 * sizeof(float));
    assert(test_magnitudes);
    for (size_t i = 0; i < 6; i++) test_magnitudes[i] = (float)i / 6.0f;
    s_spec_palette = SPEC_PALETTE_PHOSPHOR;
    spectro_update_paused_window(&engine, NULL, channel);
    assert(test_polls == 1 && engine.frame_valid);
    assert(!pixels_zero(pixels, sizeof(pixels)));

    memcpy(previous, pixels, sizeof(pixels));
    s_spec_palette = SPEC_PALETTE_HEAT;
    spectro_palette_reload(&engine, NULL, channel);
    assert(test_polls == 2 && engine.frame_valid && engine.analysis_rerequest);
    assert(memcmp(previous, pixels, sizeof(pixels)) != 0);

    /* Stop frees playback-owned magnitudes before the next panel render:
     * analysis_active may still be true when the user clicks the palette. */
    free(test_magnitudes);
    test_magnitudes = NULL;
    test_ready = false;
    test_state = PLAYBACK_STATE_STOPPED;
    engine.analysis_rerequest = false;
    memset(pixels, 0xcc, sizeof(pixels));
    spectro_palette_reload(&engine, NULL, channel);
    assert(test_polls == 2 && !engine.frame_valid);
    assert(pixels_zero(pixels, sizeof(pixels)));

    /* Reconnect and pause before its first publication. No old-session
     * alias may be used, even though the panel instance was preserved. */
    test_state = PLAYBACK_STATE_PAUSED;
    engine.analysis_rerequest = false;
    memset(pixels, 0xcc, sizeof(pixels));
    spectro_palette_reload(&engine, NULL, channel);
    assert(test_polls == 3 && !engine.frame_valid && engine.analysis_rerequest);
    assert(pixels_zero(pixels, sizeof(pixels)));

    test_magnitudes = malloc(6 * sizeof(float));
    assert(test_magnitudes);
    for (size_t i = 0; i < 6; i++) test_magnitudes[i] = 1.0f - (float)i / 6.0f;
    test_ready = true;
    test_seq = 1; /* New sessions restart the publication sequence. */
    spectro_palette_reload(&engine, NULL, channel);
    assert(test_polls == 4 && engine.frame_valid);
    assert(!pixels_zero(pixels, sizeof(pixels)));
    free(test_magnitudes);
    test_magnitudes = NULL;
}

int main(void) {
    test_palette_session(0);
    test_palette_session(1);
    puts("gui_spectrogram_playback_lifetime: PASS");
    return 0;
}
