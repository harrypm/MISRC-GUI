/*
 * MISRC GUI - gui_playback harness audio stub
 *
 * The gui_playback harnesses compile gui_playback.c directly into their own
 * TU (no gui_audio.c: it needs raylib's AudioStream + the soxr monitor
 * resampler). Stub the sidecar monitor queue and LD mixer query so the
 * harnesses can test the playback machinery without an audio device.
 */
#include <stddef.h>
#include <stdint.h>

#include "../misrc_gui/core/gui_app.h"
#include "../misrc_gui/input/gui_playback.h"
#include "../misrc_gui/output/gui_audio.h"

void gui_audio_push_playback(const int16_t *stereo_48k, size_t frames) {
    (void)stereo_48k; (void)frames;
}

void gui_audio_reset_playback_queue(void) {
}

bool gui_audio_ld_tail_pending(void) {
    return false;
}

/* Headless playback harnesses have no application settings or capture backend. */
void gui_settings_save(const gui_settings_t *settings) {
    (void)settings;
}

int gui_app_start_capture(gui_app_t *app) {
    const char *file_a = app->settings.playback_file_a;
    const char *file_b = app->settings.playback_file_b;
    return gui_playback_start_async(app, file_a[0] ? file_a : NULL, file_b[0] ? file_b : NULL);
}

void gui_app_stop_capture(gui_app_t *app) {
    gui_playback_stop(app);
}
