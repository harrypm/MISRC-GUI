#ifndef GUI_MONITOR_VIDEO_SERVER_H
#define GUI_MONITOR_VIDEO_SERVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../processing/gui_rf_preview_worker.h"
/* One bounded background decode producer per channel, separate from recording. */
int gui_monitor_video_server_start(void);
void gui_monitor_video_server_stop(void);
/* ONLY called from the extraction worker: never waits for a consumer. */
void gui_monitor_video_server_capture(const int16_t *a, const int16_t *b,
    size_t a_count, size_t b_count, const gui_rf_block_info_t info[2],
    bool input_supported);
/* Called by an existing HTTP connection worker. Owns no socket, returns after
 * disconnect or server stop. Channel 0=A, 1=B; mode 0=CVBS, 1=Tape, 2=VHS RF.
 * Caller sends an HTTP error if this returns false. */
bool gui_monitor_video_server_stream(uintptr_t fd, unsigned channel,
    unsigned mode, unsigned bitrate_bps, unsigned fps_x100);
#endif
