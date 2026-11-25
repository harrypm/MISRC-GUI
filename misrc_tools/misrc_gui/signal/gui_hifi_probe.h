/*
 * Hi-Fi carrier probe: find which RF channel carries a Hi-Fi audio FM pair,
 * and which pair (VHS NTSC 1.3 / 1.7 MHz, VHS PAL 1.4 / 1.8 MHz, Beta NTSC
 * 1.38 / 1.53 / 1.68 / 1.83 MHz), from the RF itself - no TV system setting.
 *
 * A block's power is measured in narrow windows around each carrier and
 * compared with the whole 0.9 - 2.2 MHz band. A Hi-Fi RF track puts most of
 * that band's power in its carriers; video RF (FM luma sidebands, colour-
 * under) and noise spread it evenly, scoring about the windows' share of the
 * band (~0.14 for a pair).
 *
 * Cost: one 4096-sample Hann-windowed Goertzel bank over the band, about
 * 1 M multiply-adds per block. The caller rate-limits (gui_hifi34.c probes
 * each channel a few times a second, only while Hi-Fi decode is in use).
 */
#ifndef GUI_HIFI_PROBE_H
#define GUI_HIFI_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GUI_HIFI_FMT_VHS_NTSC = 0,
    GUI_HIFI_FMT_VHS_PAL,
    GUI_HIFI_FMT_BETA_NTSC,
    GUI_HIFI_FMT_COUNT
} gui_hifi_fmt_t;

#define GUI_HIFI_PROBE_SAMPLES 4096u
/* A score at or above this is a Hi-Fi carrier set (measured: Hi-Fi RF
 * 0.8+, video RF and noise 0.1 - 0.25; dev/prompt_video_field_view_readme.md). */
#define GUI_HIFI_PROBE_PRESENT 0.45f

typedef struct {
    float score[GUI_HIFI_FMT_COUNT];   /* 0..1: carrier windows' share of the band */
    float band_power;                  /* Mean power in the band (0 = silent block) */
} gui_hifi_probe_result_t;

/* Uses the first GUI_HIFI_PROBE_SAMPLES samples (needs at least that many).
 * rate_hz is the real RF sample rate (8 - 40 MSPS). False if too short or
 * the rate cannot hold the band. */
bool gui_hifi_probe_block(const int16_t *samples, size_t count, double rate_hz,
                          gui_hifi_probe_result_t *out);

const char *gui_hifi_fmt_name(gui_hifi_fmt_t fmt);

#endif
