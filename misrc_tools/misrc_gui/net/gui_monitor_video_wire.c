#include "gui_monitor_video_wire.h"

#include <string.h>

static void be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void be64(uint8_t *p, uint64_t v) {
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t get64(const uint8_t *p) {
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}
static int valid(const mvd_frame_header *f) {
    return f && mvd_codec_valid(f->codec) &&
           (f->flags & (uint8_t)~(MVD_FLAG_KEYFRAME | MVD_FLAG_CODEC_CONFIG | MVD_FLAG_HEARTBEAT)) == 0 &&
           f->width >= 2 && f->height >= 2 &&
           f->width <= MVD_MAX_WIDTH && f->height <= MVD_MAX_HEIGHT &&
           (f->width & 1u) == 0 && (f->height & 1u) == 0 &&
           f->fps_x100 >= 100 && f->fps_x100 <= 6000 &&
           f->payload_bytes >= 4 && f->payload_bytes <= MVD_MAX_PAYLOAD;
}

int mvd_header_encode(uint8_t out[MVD_HEADER_BYTES], const mvd_frame_header *f) {
    if (!out || !valid(f)) return -1;
    memset(out, 0, MVD_HEADER_BYTES);
    memcpy(out, "MVD1", 4);
    out[4] = 1; /* framing version */
    out[5] = MVD_HEADER_BYTES;
    out[6] = f->codec;
    out[7] = f->flags;
    be32(out + 8, f->stream_id);
    be32(out + 12, f->sequence);
    be64(out + 16, f->pts_us);
    be16(out + 24, f->width);
    be16(out + 26, f->height);
    be16(out + 28, f->fps_x100);
    be32(out + 32, f->payload_bytes);
    return 0;
}

int mvd_header_decode(mvd_frame_header *f, const uint8_t buf[MVD_HEADER_BYTES]) {
    if (!f || !buf || memcmp(buf, "MVD1", 4) != 0 || buf[4] != 1 ||
        buf[5] != MVD_HEADER_BYTES || buf[30] != 0 || buf[31] != 0 ||
        buf[36] != 0 || buf[37] != 0 || buf[38] != 0 || buf[39] != 0) return -1;
    mvd_frame_header candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.codec = buf[6];
    candidate.flags = buf[7];
    candidate.stream_id = get32(buf + 8);
    candidate.sequence = get32(buf + 12);
    candidate.pts_us = get64(buf + 16);
    candidate.width = get16(buf + 24);
    candidate.height = get16(buf + 26);
    candidate.fps_x100 = get16(buf + 28);
    candidate.payload_bytes = get32(buf + 32);
    if (!valid(&candidate)) return -1;
    *f = candidate;
    return 0;
}

void mvd_reader_reset(mvd_frame_reader *r) {
    if (!r) return;
    r->header_used = 0;
    r->payload_used = 0;
    r->state = 0;
    memset(&r->frame, 0, sizeof(r->frame));
}

void mvd_reader_init(mvd_frame_reader *r, uint8_t *payload, size_t capacity) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->payload = payload;
    r->payload_capacity = capacity;
}

int mvd_reader_feed(mvd_frame_reader *r, const uint8_t *input, size_t bytes, size_t *consumed) {
    size_t n;
    if (!consumed) return -1;
    *consumed = 0;
    if (!r || (!input && bytes > 0) || (bytes > 0 && !r->payload)) return -1;
    if (r->state < 0 || r->state == 2) return -1;
    if (r->state == 0) {
        n = MVD_HEADER_BYTES - r->header_used;
        if (n > bytes) n = bytes;
        if (n) memcpy(r->header_buf + r->header_used, input, n);
        r->header_used += n;
        *consumed = n;
        if (r->header_used != MVD_HEADER_BYTES) return 0;
        if (mvd_header_decode(&r->frame, r->header_buf) != 0) {
            r->state = -1;
            return -1;
        }
        if (r->frame.payload_bytes > r->payload_capacity) {
            r->state = -1;
            return -2;
        }
        r->state = 1;
    }
    n = (size_t)r->frame.payload_bytes - r->payload_used;
    if (n > bytes - *consumed) n = bytes - *consumed;
    if (n) memcpy(r->payload + r->payload_used, input + *consumed, n);
    r->payload_used += n;
    *consumed += n;
    if (r->payload_used == r->frame.payload_bytes) {
        r->state = 2;
        return 1;
    }
    return 0;
}
