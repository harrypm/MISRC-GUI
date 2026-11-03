#ifndef GUI_MONITOR_VIDEO_FRAMEBOX_H
#define GUI_MONITOR_VIDEO_FRAMEBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gui_monitor_video_framebox gui_monitor_video_framebox_t;
typedef struct {
    unsigned width, height;
    unsigned channels; /* tightly packed RGB24 (3) or RGBA32 (4) */
    uint64_t generation;
    uint64_t timestamp_us; /* supplied by producer; same time base as stream */
} gui_monitor_video_frame_info_t;

/* Allocate once before publishing. Stop/join producers before destroy. */
gui_monitor_video_framebox_t *gui_monitor_video_framebox_create(unsigned max_width,
                                                                 unsigned max_height);
void gui_monitor_video_framebox_destroy(gui_monitor_video_framebox_t *box);

/* One call per decoded output frame, on the decoder/preview worker ONLY.
 * No allocation, waiting, encoding or network I/O. A contended frame is
 * discarded immediately. Caller owns input and keeps it valid during call.
 * Source stride and capacity are checked to prevent overflow/read overruns.
 */
bool gui_monitor_video_framebox_publish(gui_monitor_video_framebox_t *box,
    const uint8_t *pixels, size_t capacity, size_t stride,
    unsigned width, unsigned height, unsigned channels, uint64_t timestamp_us);

/* Non-blocking snapshot; consumer owns out. Returns false if unchanged,
 * lock contested, or destination too small. Caller tracks generation (0
 * initially); it changes only on a successful copy.
 */
bool gui_monitor_video_framebox_take(gui_monitor_video_framebox_t *box,
    uint8_t *out, size_t capacity, uint64_t *generation,
    gui_monitor_video_frame_info_t *info);

/* Plug directly into gui_cvbs_set_monitor_frame_sink() (RGB24)
 * or gui_rf_preview_worker_set_monitor_frame_sink() (RGBA32).
 * These callbacks synchronously copy to a bounded latest-frame slot;
 * they do NOT encode or send data. */
void gui_monitor_video_framebox_cvbs_sink(void *context, const uint8_t *rgb,
    unsigned width, unsigned height, uint64_t timestamp_us);
void gui_monitor_video_framebox_rf_sink(void *context, const uint8_t *rgba,
    unsigned width, unsigned height, uint64_t timestamp_us);

uint64_t gui_monitor_video_framebox_published(const gui_monitor_video_framebox_t *box);
uint64_t gui_monitor_video_framebox_dropped(const gui_monitor_video_framebox_t *box);

#ifdef __cplusplus
}
#endif
#endif
