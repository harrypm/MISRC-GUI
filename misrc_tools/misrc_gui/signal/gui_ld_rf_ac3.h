/* MISRC GUI LaserDisc AC-3 RF audio preview. GPL-3.0-or-later. */
#ifndef GUI_LD_RF_AC3_H
#define GUI_LD_RF_AC3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gui_ld_rf_ac3 gui_ld_rf_ac3_t;
typedef void (*gui_ld_rf_ac3_pcm_t)(void *context, const float *stereo,
                                   size_t frames, uint64_t end_sample);

gui_ld_rf_ac3_t *gui_ld_rf_ac3_create(void);
void gui_ld_rf_ac3_destroy(gui_ld_rf_ac3_t *decoder);
void gui_ld_rf_ac3_reset(gui_ld_rf_ac3_t *decoder);
bool gui_ld_rf_ac3_locked(const gui_ld_rf_ac3_t *decoder);

/* Feed normalized 20 MSPS RF. PCM is 48 kHz stereo and valid during the
 * callback; end_sample is an estimate on the same 20 MSPS media axis. */
void gui_ld_rf_ac3_process(gui_ld_rf_ac3_t *decoder, const float *samples,
                           size_t count, uint64_t first_sample,
                           gui_ld_rf_ac3_pcm_t pcm, void *context);
/* Flush a recoverable final RF block through the last process callback. */
void gui_ld_rf_ac3_finish(gui_ld_rf_ac3_t *decoder);
#endif
