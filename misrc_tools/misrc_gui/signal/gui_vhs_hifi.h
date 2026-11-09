#ifndef GUI_VHS_HIFI_H
#define GUI_VHS_HIFI_H

#include "gui_beta_hifi.h"
#include "gui_rf_preview.h"

typedef struct gui_vhs_hifi_decoder gui_vhs_hifi_decoder_t;

static inline bool gui_vhs_hifi_is_native_low_rate(uint32_t rate) {
    return rate == 8000000u || rate == 8192000u || rate == 10000000u;
}

/* The TV system and input rate are fixed for the lifetime of the decoder.
 * Supports 20 MSPS and the native low rates above. */
gui_vhs_hifi_decoder_t *gui_vhs_hifi_create(gui_rf_format_t format, uint32_t rf_rate_hz);
bool gui_vhs_hifi_iq_supported(gui_rf_format_t format, uint32_t iq_rate_hz,
                              uint64_t center_hz);
gui_vhs_hifi_decoder_t *gui_vhs_hifi_create_iq(gui_rf_format_t format, uint32_t iq_rate_hz,
                                             uint64_t center_hz);
void gui_vhs_hifi_destroy(gui_vhs_hifi_decoder_t *decoder);
void gui_vhs_hifi_reset(gui_vhs_hifi_decoder_t *decoder);

/* PAL/NTSC VHS Hi-Fi RF to borrowed 48 kHz interleaved stereo.
 * Sample positions are in the configured input rate's domain.
 * A discontinuity resets lock and all signal history. Unusable RF is muted. */
void gui_vhs_hifi_process(gui_vhs_hifi_decoder_t *decoder, const float *samples,
                          size_t count, uint64_t first_sample,
                          gui_beta_hifi_pcm_callback_t callback, void *context);

/* Complex baseband input. Count and source positions refer to I/Q pairs. */
void gui_vhs_hifi_process_iq(gui_vhs_hifi_decoder_t *decoder, const float *i, const float *q,
                            size_t count, uint64_t first_sample,
                            gui_beta_hifi_pcm_callback_t callback, void *context);

#endif
