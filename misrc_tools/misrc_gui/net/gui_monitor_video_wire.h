#ifndef GUI_MONITOR_VIDEO_WIRE_H
#define GUI_MONITOR_VIDEO_WIRE_H

/*
 * MISRC-M monitor video extension, revision 1.
 * Transport framing only: H.264 Annex B access units, not RF samples.
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
#define MVD_FLAG_KEYFRAME 0x01u
#define MVD_FLAG_CODEC_CONFIG 0x02u

/* Every frame is self-described. SPS/PPS must be supplied with each IDR. */
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
