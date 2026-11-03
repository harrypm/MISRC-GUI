#include "gui_monitor_video_i420.h"
#include "gui_monitor_video_wire.h"

#include <stdint.h>

static uint8_t clamp8(int x) {
    if (x < 0) return 0;
    if (x > 255) return 255;
    return (uint8_t)x;
}
static uint8_t luma(int r, int g, int b) {
    return clamp8(16 + (66*r + 129*g + 25*b + 128) / 256);
}
static uint8_t chroma_u(int r, int g, int b) {
    return clamp8(128 + (-38*r - 74*g + 112*b) / 256);
}
static uint8_t chroma_v(int r, int g, int b) {
    return clamp8(128 + (112*r - 94*g - 18*b) / 256);
}
size_t mvd_i420_buffer_size(unsigned w, unsigned h) {
    if (w < 2 || h < 2 || w > MVD_MAX_WIDTH || h > MVD_MAX_HEIGHT ||
        (w & 1u) || (h & 1u)) return 0;
    return (size_t)w * h * 3u / 2u;
}
int mvd_rgb_to_i420(const uint8_t *src, size_t src_cap,
                    unsigned sw, unsigned sh, size_t stride,
                    mvd_rgb_format format, uint8_t *out, size_t out_cap,
                    unsigned w, unsigned h) {
    size_t needed = mvd_i420_buffer_size(w,h);
    if (!src || !out || !needed || out_cap < needed ||
        sw == 0 || sh == 0 || sw > MVD_MAX_WIDTH || sh > MVD_MAX_HEIGHT ||
        (format != MVD_RGB24 && format != MVD_RGBA32) ||
        w > sw || h > sh || stride < (size_t)sw * (unsigned)format ||
        stride > SIZE_MAX / sh ||
        (size_t)(sh - 1u) * stride + (size_t)sw * (unsigned)format > src_cap) return -1;

    uint8_t *y_plane = out;
    uint8_t *u_plane = out + (size_t)w*h;
    uint8_t *v_plane = u_plane + (size_t)w*h/4u;
    for (unsigned y = 0; y < h; y += 2) {
        for (unsigned x = 0; x < w; x += 2) {
            int r_sum=0, g_sum=0, b_sum=0;
            for (unsigned dy = 0; dy < 2; ++dy) {
                unsigned sy = (unsigned)(((uint64_t)(y+dy) * sh) / h);
                for (unsigned dx = 0; dx < 2; ++dx) {
                    unsigned sx = (unsigned)(((uint64_t)(x+dx) * sw) / w);
                    const uint8_t *p = src + (size_t)sy * stride + (size_t)sx * (unsigned)format;
                    int r=p[0], g=p[1], b=p[2];
                    y_plane[(size_t)(y+dy)*w + x + dx] = luma(r,g,b);
                    r_sum += r; g_sum += g; b_sum += b;
                }
            }
            size_t uv = (size_t)(y/2u)*(w/2u) + (x/2u);
            u_plane[uv] = chroma_u((r_sum+2)/4, (g_sum+2)/4, (b_sum+2)/4);
            v_plane[uv] = chroma_v((r_sum+2)/4, (g_sum+2)/4, (b_sum+2)/4);
        }
    }
    return 0;
}
