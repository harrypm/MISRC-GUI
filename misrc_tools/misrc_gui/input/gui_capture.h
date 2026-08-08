#ifndef GUI_CAPTURE_H
#define GUI_CAPTURE_H

#include "../core/gui_app.h"

// Global exit flag (defined in misrc_gui.c)
extern volatile atomic_int do_exit;

// Capture callback function
void gui_capture_callback(void *data_info);

// Enable/disable audio capture in the hsdaoh callback.
// Normally audio is enabled during capture for monitoring.
void gui_capture_set_audio_capture(bool enabled);

// Note: Audio buffer now accessed via app->buffers (buffer_manager)
// Use BUF_CAPTURE_AUDIO with bufmgr_read_begin/bufmgr_read_end

// Check if device has timed out (no callbacks for too long)
// Returns true if device appears disconnected
bool gui_capture_device_timeout(gui_app_t *app, uint32_t timeout_ms);

// UI thread: apply cached hsdaoh-rp2350 status/errors at a low rate (e.g. every 2s)
// Major HW issues will be obvious
void gui_capture_poll_hsdaoh_status(gui_app_t *app);

#endif // GUI_CAPTURE_H
