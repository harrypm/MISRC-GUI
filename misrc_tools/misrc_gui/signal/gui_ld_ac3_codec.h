/* MISRC GUI LaserDisc AC-3 audio decoder. GPL-3.0-or-later. */
#ifndef GUI_LD_AC3_CODEC_H
#define GUI_LD_AC3_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GUI_LD_AC3_PCM_FRAMES = 1536 };

typedef struct gui_ld_ac3_codec gui_ld_ac3_codec_t;

gui_ld_ac3_codec_t *gui_ld_ac3_codec_create(void);
void gui_ld_ac3_codec_destroy(gui_ld_ac3_codec_t *codec);
void gui_ld_ac3_codec_reset(gui_ld_ac3_codec_t *codec);

/* Decode one complete 48 kHz AC-3 frame. On success, stereo contains
 * GUI_LD_AC3_PCM_FRAMES interleaved pairs. Discard it on failure. */
bool gui_ld_ac3_codec_decode(gui_ld_ac3_codec_t *codec, const uint8_t *frame,
                             size_t bytes, float *stereo);

#endif
