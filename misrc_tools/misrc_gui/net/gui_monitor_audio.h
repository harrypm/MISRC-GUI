/* Low-bandwidth Opus Audio Mon for Monitor & Remote only.
 * The legacy /baseband Client, local capture and recorder are unchanged. */
#ifndef GUI_MONITOR_AUDIO_H
#define GUI_MONITOR_AUDIO_H
#include "../core/gui_app.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called on the normal server's existing audio capture tap. Never blocks. */
void gui_monitor_audio_server_tap(gui_app_t *app, const void *data, size_t bytes);
void gui_monitor_audio_server_reset(void);

/* Per-HTTP-client stream. Called on the server's existing request worker.
 * The caller retains and closes the socket after this function returns. */
typedef int (*gui_monitor_audio_send_fn)(uintptr_t sock, const char *bytes, size_t size);
bool gui_monitor_audio_server_stream(uintptr_t sock, gui_app_t *app, unsigned pair,
                                     gui_monitor_audio_send_fn send_packet);

/* Separate Monitor-only client socket/Opus decoder; start/stop on mode changes.
 * Audio device creation/destruction remains on the GUI thread. */
int gui_monitor_audio_client_start(gui_app_t *app, const char *host, uint16_t port);
void gui_monitor_audio_client_stop(void);
void gui_monitor_audio_client_update(gui_app_t *app);

#endif
