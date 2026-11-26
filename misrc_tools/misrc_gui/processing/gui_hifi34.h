/*
 * Hi-Fi audio decode on CH3/4 (harrypm 2026-11-25: "Add HiFi-Decoding to
 * Ch3/4 on player mode and on capture modes (if there is no input/stream
 * from a MISRC V2.5 or HSDAOH with 4ch input)").
 *
 * One audio-only RF preview worker (VHS Hi-Fi NTSC / PAL profile, or the
 * Beta Hi-Fi profile) decodes the Hi-Fi FM carriers of one RF channel to
 * 48 kHz stereo. It runs only when picked by hand (HiFi panel Decoder
 * button, settings hifi34_enabled, --hifi decode=) while a source runs
 * (harrypm 2026-11-26). Then, when the device has no real CH3/4 audio:
 * - CH3/4 meters show the decoded L / R (app->hifi34_peak),
 * - the CH3/4 monitor plays it (gui_video_update_rf_routes override),
 * - the HiFi panel draws its waveform (gui_hifi34_copy_recent).
 *
 * Channel and format: Auto by the carrier probe (gui_hifi_probe.c) on both
 * RF channels - the channel whose 0.9-2.2 MHz band is mostly a VHS NTSC
 * (1.3 / 1.7 MHz) or PAL (1.4 / 1.8 MHz) carrier pair - or forced by the
 * settings (hifi34_source / hifi34_format; --hifi src=,fmt=).
 */
#ifndef GUI_HIFI34_H
#define GUI_HIFI34_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../signal/gui_hifi_probe.h"

struct gui_app;

typedef struct {
    bool wanted;            // In use (CH3/4 or a HiFi panel) and a source runs
    bool running;           // Decoder worker exists
    bool owns_ch34;         // CH3/4 meters + monitor carry the decoded Hi-Fi
    bool real_ch34;         // The device streams real CH3/4 audio (not taken over)
    bool monitored;         // The CH3/4 monitor is playing it
    bool locked;            // Decoder reports carrier lock
    bool rate_unsupported;  // The channel's RF rate cannot feed the decoder
    int channel;            // Decoded RF channel (0 = A, 1 = B), -1 none
    gui_hifi_fmt_t fmt;     // Decoded format (valid while running)
    float score[2][GUI_HIFI_FMT_COUNT];  // Smoothed probe scores per channel
    double cpu_percent;     // Worker CPU (100 = one core)
    char reason[96];        // Decoder status text
} gui_hifi34_status_t;

/* Registers the RF tap / monitor hooks (gui_video_set_rf_tap). Once, at startup. */
void gui_hifi34_init(struct gui_app *app);
/* Main thread, once per frame BEFORE gui_video_update_rf_routes. */
void gui_hifi34_update(struct gui_app *app);
/* Stops the decoder (main thread; app exit). */
void gui_hifi34_shutdown(void);

void gui_hifi34_get_status(gui_hifi34_status_t *status);
/* Newest decoded stereo frames (interleaved L R, 48 kHz), oldest first.
 * Returns the frames copied (fewer while the history fills). Render thread. */
size_t gui_hifi34_copy_recent(float *stereo, size_t frames);
/* Total frames decoded so far (changes while audio flows). */
uint64_t gui_hifi34_frames_total(void);

/* Settings value names (Auto / A / B; Auto / VHS PAL / VHS NTSC / Beta). */
const char *gui_hifi34_source_name(int source);
const char *gui_hifi34_format_name(int format);

#endif
