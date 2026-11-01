#ifndef GUI_MONITOR_VIDEO_CODEC_H
#define GUI_MONITOR_VIDEO_CODEC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* OpenH264 wrappers: one encoder/decoder instance is owned by ONE worker thread.
 * Unsupported builds report false/NULL; never silently send uncompressed RF. */
typedef struct gui_mvd_encoder gui_mvd_encoder_t;
typedef struct gui_mvd_decoder gui_mvd_decoder_t;
int gui_mvd_codec_available(void);
gui_mvd_encoder_t *gui_mvd_encoder_create(unsigned w, unsigned h,
                                         unsigned fps_x100, unsigned bitrate_bps);
void gui_mvd_encoder_destroy(gui_mvd_encoder_t *ctx);
/* Input: tightly packed I420; output: a complete Annex B access unit.
 * 1 means encoded, 0 means intentionally skipped, -1 means error. */
int gui_mvd_encoder_frame(gui_mvd_encoder_t *ctx, const uint8_t *i420,
                          size_t i420_bytes, uint64_t timestamp_us,
                          uint8_t *out, size_t out_cap, size_t *out_bytes,
                          int *keyframe);
void gui_mvd_encoder_request_keyframe(gui_mvd_encoder_t *ctx);
gui_mvd_decoder_t *gui_mvd_decoder_create(void);
void gui_mvd_decoder_destroy(gui_mvd_decoder_t *ctx);
/* Decode Annex B access unit to tightly packed RGBA; 1 frame / 0 pending / -1 error. */
int gui_mvd_decoder_frame(gui_mvd_decoder_t *ctx, const uint8_t *h264,
                          size_t h264_len, uint8_t *rgba, size_t rgba_cap,
                          unsigned *width, unsigned *height);
#ifdef __cplusplus
}
#endif
#endif
