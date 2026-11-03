#ifndef GUI_MONITOR_VIDEO_CLIENT_H
#define GUI_MONITOR_VIDEO_CLIENT_H
#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"
/* The video connection is independent from the M/2 status and MRD5 poller. */
int gui_monitor_video_client_start(const char *host,uint16_t port);
void gui_monitor_video_client_stop(void);
/* UI/render thread only: uses the existing Video panel rectangle. */
void gui_monitor_video_client_draw(unsigned channel,Rectangle bounds);
#endif
