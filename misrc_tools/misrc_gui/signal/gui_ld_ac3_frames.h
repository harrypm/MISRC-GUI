/* MISRC GUI LaserDisc AC-3 RF frame recovery. GPL-3.0-or-later. */
#ifndef GUI_LD_AC3_FRAMES_H
#define GUI_LD_AC3_FRAMES_H

#include <stddef.h>
#include <stdint.h>

typedef struct gui_ld_ac3_frames gui_ld_ac3_frames_t;
typedef void (*gui_ld_ac3_frame_callback_t)(void *context, const uint8_t *ac3,
                                           size_t bytes, uint64_t end_sample);

gui_ld_ac3_frames_t *gui_ld_ac3_frames_create(void);
void gui_ld_ac3_frames_destroy(gui_ld_ac3_frames_t *decoder);
void gui_ld_ac3_frames_reset(gui_ld_ac3_frames_t *decoder);

/* Supply one recovered DQPSK symbol (0..3) at a time. The callback receives
 * a 48 kHz, 384 kb/s AC-3 frame to validate and decode. Its buffer is valid
 * until the callback returns. All timestamps use the 20 MSPS RF timeline. */
void gui_ld_ac3_frames_push(gui_ld_ac3_frames_t *decoder, uint8_t symbol,
                            uint64_t end_sample, gui_ld_ac3_frame_callback_t emit,
                            void *context);
/* At natural EOF, attempt erasure repair of the last incomplete RF block. */
void gui_ld_ac3_frames_finish(gui_ld_ac3_frames_t *decoder, gui_ld_ac3_frame_callback_t emit,
                              void *context);

#endif
