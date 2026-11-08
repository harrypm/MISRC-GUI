#ifndef GUI_AUDIO_H
#define GUI_AUDIO_H

#include "../core/gui_app.h"
#include "../../common/buffer_manager.h"
#include "../../common/file_utils.h"

// Start audio writer/monitor thread.
// If no audio outputs are enabled in settings, this is a no-op and returns 0.
int gui_audio_start(gui_app_t *app, buffer_manager_t *bufmgr);

// Stop audio writer/monitor thread if running.
void gui_audio_stop(gui_app_t *app);
/* Record paths are resolved by the caller before publishing a new session. */
#define GUI_AUDIO_RECORD_OUTPUT_COUNT 7
#define GUI_AUDIO_RECORD_PATH_MAX (FILE_PATH_MAX + MAX_FILENAME_LEN)
void gui_audio_begin_recording(const gui_app_t *app,
    const char paths[GUI_AUDIO_RECORD_OUTPUT_COUNT][GUI_AUDIO_RECORD_PATH_MAX],
    unsigned overwrite_mask);
/* Record-only stop keeps the monitor running. Wait from the finalizer. */
void gui_audio_request_record_stop(const gui_app_t *app);
bool gui_audio_wait_recording_idle(void);

// True if audio thread is running.
bool gui_audio_is_running(void);

// Enable/disable playback monitoring to system audio device.
// This affects live monitoring only (not file writing).
void gui_audio_set_playback_enabled(gui_app_t *app, bool enabled);

// Maintain the monitor session/device and pump ordinary playback on the main
// thread once per frame. HiFi PCM is pulled by the audio device callback.
void gui_audio_update_playback(gui_app_t *app);

// Push demodulated audio (stereo int16 @ 48k) into the live monitor queue so
// the "Audio Mon" button plays it. Safe to call from the render thread.
// The user must enable Audio Mon to hear it (mirrors device-audio monitoring).
void gui_audio_push_demod(const int16_t *stereo_48k, size_t frames);

// Push drag-and-drop playback audio (stereo int16 @ 48k) into the ordinary
// monitor queue. Runs on the playback thread, requires Audio Mon and an active
// session, drops the oldest PCM on overflow, and is ignored while HiFi is selected.
void gui_audio_push_playback(const int16_t *stereo_48k, size_t frames);

// Invalidate ordinary-monitor PCM after a playback seek/loop. Nonblocking;
// the main pump flushes the device stream before playing the new generation.
// A sidecar reset cannot invalidate the selected HiFi input.
void gui_audio_reset_playback_queue(void);

// Select one RF HiFi worker as the live monitor input; NULL restores ordinary
// device/demod monitoring. Call on the main thread. Source tokens must remain
// unique until the previous producer has stopped. Source selection flushes
// queued PCM and the existing output stream on the next playback update.
void gui_audio_set_hifi_source(const void *source);
const void *gui_audio_get_hifi_source(void);

// Interleaved stereo float PCM at exactly 48 kHz, nominal range [-1, +1].
// Nonblocking and allocation-free; stale/unselected sources are ignored.
// Audio Mon and an active capture monitoring session are required. The main
// playback pump opens each session after discarding old PCM; pushes made while
// capture is stopped or before its first pump are ignored.
// Overflow keeps the newest 85 ms; NaN/Inf mute.
void gui_audio_push_hifi(const void *source, const float *stereo_48k, size_t frames);

// Notify a selected producer discontinuity. Safe on the worker thread; device
// flushing happens later on the main thread. Other producers cannot reset it.
void gui_audio_reset_hifi(const void *source);

#endif // GUI_AUDIO_H
