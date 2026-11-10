#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gui_ld_dts gui_ld_dts_t;

/* Interleaved stereo at 44.1 kHz, timed on the LD 20 MSPS RF grid. */
typedef void (*gui_ld_dts_pcm_t)(void *context, const float *stereo,
                                 size_t frames, uint64_t end_sample);

gui_ld_dts_t *gui_ld_dts_create(void);
void gui_ld_dts_destroy(gui_ld_dts_t *d);
void gui_ld_dts_reset(gui_ld_dts_t *d);
bool gui_ld_dts_available(void);
bool gui_ld_dts_locked(const gui_ld_dts_t *d);

/* True once a DTS core header is found, including builds without a decoder.
 * Keep the DTS classification through erasures so encoded words stay muted.
 * A NULL callback enables detection only. */
bool gui_ld_dts_push(gui_ld_dts_t *d, const int16_t pair[2], bool valid,
                     uint64_t end, gui_ld_dts_pcm_t pcm, void *context);
