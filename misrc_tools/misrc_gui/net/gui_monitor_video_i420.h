#ifndef GUI_MONITOR_VIDEO_I420_H
#define GUI_MONITOR_VIDEO_I420_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 3 = packed RGB, 4 = packed RGBA; alpha is ignored. */
typedef enum { MVD_RGB24 = 3, MVD_RGBA32 = 4 } mvd_rgb_format;

/* Returns 0 for invalid dimensions or overflow; I420: Y, U, V planes. */
size_t mvd_i420_buffer_size(unsigned width, unsigned height);

/*
 * Optional nearest-neighbour downscaling and limited-range BT.601 conversion.
 * Destination dimensions must be even; no allocations or external libraries.
 * Inputs must be immutable until the function returns. No overlapping buffers.
 * Caller controls conversion frequency: never call on capture/recording thread.
 */
int mvd_rgb_to_i420(const uint8_t *source, size_t source_capacity,
                    unsigned source_width, unsigned source_height,
                    size_t source_stride, mvd_rgb_format source_format,
                    uint8_t *destination, size_t destination_capacity,
                    unsigned target_width, unsigned target_height);

#ifdef __cplusplus
}
#endif
#endif
