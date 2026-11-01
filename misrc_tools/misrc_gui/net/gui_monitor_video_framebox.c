#include "gui_monitor_video_framebox.h"
#include "gui_monitor_video_wire.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct gui_monitor_video_framebox {
    atomic_flag busy;
    atomic_uint_fast64_t published, dropped;
    uint8_t *pixels;
    size_t capacity;
    unsigned max_width, max_height;
    gui_monitor_video_frame_info_t latest;
};

static bool image_bytes(unsigned width, unsigned height, unsigned channels,
                        size_t *bytes) {
    if (width == 0 || height == 0 || width > MVD_MAX_WIDTH ||
        height > MVD_MAX_HEIGHT || (channels != 3 && channels != 4)) return false;
    if ((size_t)width > SIZE_MAX / channels) return false;
    size_t row = (size_t)width * channels;
    if ((size_t)height > SIZE_MAX / row) return false;
    *bytes = row * height;
    return true;
}
gui_monitor_video_framebox_t *gui_monitor_video_framebox_create(unsigned width,
                                                                 unsigned height) {
    size_t needed = 0;
    if (!image_bytes(width, height, 4, &needed)) return NULL;
    gui_monitor_video_framebox_t *box = calloc(1, sizeof(*box));
    if (!box) return NULL;
    box->pixels = malloc(needed);
    if (!box->pixels) { free(box); return NULL; }
    box->capacity = needed;
    box->max_width = width;
    box->max_height = height;
    atomic_flag_clear(&box->busy);
    atomic_init(&box->published, 0);
    atomic_init(&box->dropped, 0);
    return box;
}
void gui_monitor_video_framebox_destroy(gui_monitor_video_framebox_t *box) {
    if (!box) return;
    /* Producer and consumer MUST have been stopped/joined by owner. */
    free(box->pixels);
    free(box);
}
bool gui_monitor_video_framebox_publish(gui_monitor_video_framebox_t *box,
    const uint8_t *pixels, size_t capacity, size_t stride,
    unsigned width, unsigned height, unsigned channels, uint64_t timestamp_us) {
    size_t bytes = 0;
    if (!box || !pixels || !image_bytes(width, height, channels, &bytes) ||
        width > box->max_width || height > box->max_height) return false;
    size_t row = (size_t)width * channels;
    if (stride < row || (size_t)(height - 1) > (SIZE_MAX - row) / stride ||
        (size_t)(height - 1) * stride + row > capacity || bytes > box->capacity)
        return false;
    /* Never wait on encoder/consumer; dropping a preview is safe. */
    if (atomic_flag_test_and_set_explicit(&box->busy, memory_order_acquire)) {
        atomic_fetch_add_explicit(&box->dropped, 1, memory_order_relaxed);
        return false;
    }
    for (unsigned y = 0; y < height; y++)
        memcpy(box->pixels + (size_t)y * row, pixels + (size_t)y * stride, row);
    box->latest.width = width;
    box->latest.height = height;
    box->latest.channels = channels;
    box->latest.timestamp_us = timestamp_us;
    ++box->latest.generation;
    atomic_fetch_add_explicit(&box->published, 1, memory_order_relaxed);
    atomic_flag_clear_explicit(&box->busy, memory_order_release);
    return true;
}
bool gui_monitor_video_framebox_take(gui_monitor_video_framebox_t *box,
    uint8_t *out, size_t capacity, uint64_t *generation,
    gui_monitor_video_frame_info_t *info) {
    if (!box || !out || !generation) return false;
    if (atomic_flag_test_and_set_explicit(&box->busy, memory_order_acquire))
        return false;
    gui_monitor_video_frame_info_t latest = box->latest;
    size_t bytes = (size_t)latest.width * latest.height * latest.channels;
    bool ok = latest.generation != 0 && latest.generation != *generation &&
              bytes <= capacity;
    if (ok) {
        memcpy(out, box->pixels, bytes);
        *generation = latest.generation;
        if (info) *info = latest;
    }
    atomic_flag_clear_explicit(&box->busy, memory_order_release);
    return ok;
}
static void framebox_sink(void *context, const uint8_t *pixels, unsigned width,
                          unsigned height, unsigned channels, uint64_t timestamp_us) {
    size_t bytes = 0;
    if (!image_bytes(width, height, channels, &bytes)) return;
    (void)gui_monitor_video_framebox_publish(
        (gui_monitor_video_framebox_t *)context, pixels, bytes,
        (size_t)width * channels, width, height, channels, timestamp_us);
}
void gui_monitor_video_framebox_cvbs_sink(void *context, const uint8_t *rgb,
    unsigned width, unsigned height, uint64_t timestamp_us) {
    framebox_sink(context, rgb, width, height, 3, timestamp_us);
}
void gui_monitor_video_framebox_rf_sink(void *context, const uint8_t *rgba,
    unsigned width, unsigned height, uint64_t timestamp_us) {
    framebox_sink(context, rgba, width, height, 4, timestamp_us);
}
uint64_t gui_monitor_video_framebox_published(const gui_monitor_video_framebox_t *box) {
    return box ? atomic_load_explicit(&box->published, memory_order_relaxed) : 0;
}
uint64_t gui_monitor_video_framebox_dropped(const gui_monitor_video_framebox_t *box) {
    return box ? atomic_load_explicit(&box->dropped, memory_order_relaxed) : 0;
}
