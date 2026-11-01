#ifndef GUI_MONITOR_VIDEO_FFMPEG_H
#define GUI_MONITOR_VIDEO_FFMPEG_H
#include <stddef.h>
#include <stdint.h>
#include "gui_monitor_video_wire.h"
/* Optional FFmpeg backend. Zero feature masks and NULL objects when absent.
 * Does not depend on Harry's CVBS/VHS code. The hardware mask only lists
 * codecs whose HW implementation is compiled; create() probes the device. */
typedef struct mvd_ff_encoder mvd_ff_encoder;
typedef struct mvd_ff_decoder mvd_ff_decoder;
uint32_t mvd_ff_encode_mask(int hardware);
uint32_t mvd_ff_decode_mask(void);
mvd_ff_encoder *mvd_ff_encoder_create(unsigned codec,unsigned w,unsigned h,
                                     unsigned fps_x100,unsigned bitrate,int hardware);
void mvd_ff_encoder_free(mvd_ff_encoder *);
int mvd_ff_encoder_frame(mvd_ff_encoder *,const uint8_t *i420,size_t bytes,
                         uint64_t pts_us,uint8_t *out,size_t cap,size_t *len,int *key);
mvd_ff_decoder *mvd_ff_decoder_create(unsigned codec);
void mvd_ff_decoder_free(mvd_ff_decoder *);
int mvd_ff_decoder_frame(mvd_ff_decoder *,const uint8_t *packet,size_t len,
                         uint8_t *rgba,size_t capacity,unsigned *w,unsigned *h);
#endif
