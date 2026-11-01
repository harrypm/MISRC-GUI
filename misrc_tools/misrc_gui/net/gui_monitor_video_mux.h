#ifndef GUI_MONITOR_VIDEO_MUX_H
#define GUI_MONITOR_VIDEO_MUX_H
#include <stddef.h>
#include <stdint.h>
#include "gui_monitor_video_wire.h"
/* Per-connection backend selector. No DSP, capture, or UI dependencies.
 * Old clients don't send codec capabilities and remain AVC-only. */
typedef struct mvd_codec_encoder mvd_codec_encoder;
typedef struct mvd_codec_decoder mvd_codec_decoder;
uint32_t mvd_video_decoder_mask(void);
uint32_t mvd_video_encoder_mask(int hw);
mvd_codec_encoder *mvd_video_encoder_best(uint32_t client_mask,
                unsigned w,unsigned h,unsigned fps_x100,unsigned bitrate);
unsigned mvd_video_encoder_codec(const mvd_codec_encoder *);
int mvd_video_encoder_hardware(const mvd_codec_encoder *);
int mvd_video_encoder_frame(mvd_codec_encoder *,const uint8_t *i420,
                     size_t bytes,uint64_t pts_us,uint8_t *out,size_t cap,
                     size_t *len,int *key);
void mvd_video_encoder_free(mvd_codec_encoder *);
mvd_codec_decoder *mvd_video_decoder_new(unsigned wire_codec);
int mvd_video_decoder_frame(mvd_codec_decoder *,const uint8_t *data,size_t len,
                             uint8_t *rgba,size_t cap,unsigned *w,unsigned *h);
void mvd_video_decoder_free(mvd_codec_decoder *);
#endif
