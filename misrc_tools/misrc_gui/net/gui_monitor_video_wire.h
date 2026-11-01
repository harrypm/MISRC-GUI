#ifndef GUI_MONITOR_VIDEO_WIRE_H
#define GUI_MONITOR_VIDEO_WIRE_H

/*
 * MISRC-M monitor video extension, revision 1.
 * Transport framing of H.264/HEVC Annex B, VP9 or AV1 access units; never RF samples.
 * All multi-byte fields are big-endian, independent of host ABI.
 * This module does not perform network I/O and never owns the payload buffer.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVD_HEADER_BYTES 40u
#define MVD_MAX_PAYLOAD (2u * 1024u * 1024u)
#define MVD_MAX_WIDTH 1920u
#define MVD_MAX_HEIGHT 1080u
#define MVD_CODEC_H264_ANNEXB 1u
#define MVD_CODEC_HEVC_ANNEXB 2u
#define MVD_CODEC_VP9 3u
#define MVD_CODEC_AV1 4u
/* Bitmap on the optional ?codecs= request. Legacy requests imply AVC-only. */
#define MVD_CODEC_BIT(codec) (1u << (codec))
#define MVD_CODEC_ALL (MVD_CODEC_BIT(MVD_CODEC_H264_ANNEXB) | \
    MVD_CODEC_BIT(MVD_CODEC_HEVC_ANNEXB) | MVD_CODEC_BIT(MVD_CODEC_VP9) | \
    MVD_CODEC_BIT(MVD_CODEC_AV1))
#define MVD_CODEC_AVC_BIT MVD_CODEC_BIT(MVD_CODEC_H264_ANNEXB)
static inline int mvd_codec_valid(unsigned codec) {
    return codec>=MVD_CODEC_H264_ANNEXB && codec<=MVD_CODEC_AV1;
}
/* Hardware AV1/VP9/HEVC preferred over AVC, then a software fallback.
 * The caller supplies actually usable encoder and client decoder masks. */
static inline unsigned mvd_codec_select(uint32_t client, uint32_t hw, uint32_t sw) {
    static const unsigned preference[] = {
        MVD_CODEC_AV1, MVD_CODEC_VP9, MVD_CODEC_HEVC_ANNEXB,
        MVD_CODEC_H264_ANNEXB
    };
    const uint32_t allowed = client & MVD_CODEC_ALL;
    for(unsigned i=0;i<sizeof(preference)/sizeof(preference[0]);i++) {
        const unsigned id=preference[i];
        if((allowed & hw & MVD_CODEC_BIT(id))!=0)return id;
    }
    for(unsigned i=0;i<sizeof(preference)/sizeof(preference[0]);i++) {
        const unsigned id=preference[i];
        if((allowed & sw & MVD_CODEC_BIT(id))!=0)return id;
    }
    return 0;
}
static inline const char *mvd_codec_name(unsigned codec) {
    switch(codec) {
        case MVD_CODEC_H264_ANNEXB: return "H.264";
        case MVD_CODEC_HEVC_ANNEXB: return "HEVC";
        case MVD_CODEC_VP9: return "VP9";
        case MVD_CODEC_AV1: return "AV1";
        default: return "unknown";
    }
}
#define MVD_FLAG_KEYFRAME 0x01u
#define MVD_FLAG_CODEC_CONFIG 0x02u
#define MVD_FLAG_HEARTBEAT 0x04u

/* Every frame is self-described. New codecs start on a keyframe.
 * AVC and HEVC must include parameter sets at their keyframes. */
typedef struct {
    uint8_t codec;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t sequence;
    uint64_t pts_us;
    uint16_t width;
    uint16_t height;
    uint16_t fps_x100;
    uint32_t payload_bytes;
} mvd_frame_header;

/* Returns 0 on success; -1 on invalid arguments/unsupported frame. */
int mvd_header_encode(uint8_t out[MVD_HEADER_BYTES], const mvd_frame_header *frame);
int mvd_header_decode(mvd_frame_header *frame, const uint8_t data[MVD_HEADER_BYTES]);

/*
 * Incremental reader for TCP streams (partial/multiple reads are expected).
 * Caller allocates payload[MVD_MAX_PAYLOAD] or its desired smaller bound.
 * feed() consumes <= input_bytes and sets *consumed. Results:
 *   0 = more bytes needed; 1 = complete frame; -1 = invalid header;
 *  -2 = frame exceeds supplied payload buffer.
 * Upon result 1, read frame metadata from reader.frame and payload bytes
 * from reader.payload. Call reset() before consuming the next frame.
 * The reader is not shared between connections or threads.
 */
typedef struct {
    uint8_t header_buf[MVD_HEADER_BYTES];
    size_t header_used;
    uint8_t *payload;
    size_t payload_capacity;
    size_t payload_used;
    mvd_frame_header frame;
    int state; /* 0 header, 1 payload, 2 complete, -1 failed */
} mvd_frame_reader;

void mvd_reader_init(mvd_frame_reader *reader, uint8_t *payload, size_t capacity);
void mvd_reader_reset(mvd_frame_reader *reader);
int mvd_reader_feed(mvd_frame_reader *reader, const uint8_t *input, size_t input_bytes,
                    size_t *consumed);

#ifdef __cplusplus
}
#endif
#endif
