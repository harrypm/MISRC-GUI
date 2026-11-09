#ifndef GUI_LD_RF_ANALOG_H
#define GUI_LD_RF_ANALOG_H

#include "gui_ld_rf_demod.h"

#define GUI_LD_RF_ANALOG_AUDIO_RATE_HZ 48000u

typedef struct gui_ld_rf_analog gui_ld_rf_analog_t;

/* Borrowed 48 kHz interleaved stereo. end_sample is the exclusive end
 * of the processed RF on the caller's 20 MSPS sample axis. */
typedef void (*gui_ld_rf_analog_pcm_callback_t)(void *context, const float *stereo,
                                               size_t frames, uint64_t end_sample);

gui_ld_rf_analog_t *gui_ld_rf_analog_create(void);
void gui_ld_rf_analog_destroy(gui_ld_rf_analog_t *demod);
void gui_ld_rf_analog_reset(gui_ld_rf_analog_t *demod, gui_ld_rf_system_t system);
bool gui_ld_rf_analog_locked(const gui_ld_rf_analog_t *demod);

/* Causal fixed-memory processing of normalized 20 MSPS RF. A gap resets
 * history. Output stays on the 48 kHz clock and missing channels are muted. */
void gui_ld_rf_analog_process(gui_ld_rf_analog_t *demod, const float *samples, size_t count,
                              uint64_t first_sample, gui_ld_rf_analog_pcm_callback_t callback,
                              void *context);

#endif
