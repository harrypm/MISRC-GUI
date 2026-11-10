/* MISRC GUI LaserDisc digital audio preview. GPL-3.0-or-later. */
#ifndef GUI_LD_RF_DIGITAL_H
#define GUI_LD_RF_DIGITAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gui_ld_rf_digital gui_ld_rf_digital_t;
typedef void (*gui_ld_rf_digital_pcm_t)(void *context, const float *stereo,
                                        size_t frames, uint64_t end_sample);

gui_ld_rf_digital_t *gui_ld_rf_digital_create(void);
void gui_ld_rf_digital_destroy(gui_ld_rf_digital_t *decoder);
void gui_ld_rf_digital_reset(gui_ld_rf_digital_t *decoder, bool decode_dts);
bool gui_ld_rf_digital_locked(const gui_ld_rf_digital_t *decoder);

/* Normalized 20 MSPS RF to interleaved 48 kHz stereo (PCM or decoded DTS).
 * end_sample estimates the exclusive media position on the RF grid, before
 * CIRC and PCM buffering.
 * The callback's samples are valid only for the duration of the call. */
void gui_ld_rf_digital_process(gui_ld_rf_digital_t *decoder, const float *samples,
                               size_t count, uint64_t first_sample,
                               gui_ld_rf_digital_pcm_t pcm, void *context);
/* Drain pending PCM and the final partial output batch at natural EOF. */
void gui_ld_rf_digital_finish(gui_ld_rf_digital_t *decoder,
                              gui_ld_rf_digital_pcm_t pcm, void *context);
#endif
