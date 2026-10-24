/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#ifndef GUI_RECORD_H
#define GUI_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct gui_app gui_app_t;

typedef enum {
    GUI_ERROR_CLASS_NONE = 0,
    GUI_ERROR_CLASS_SYSTEM = 1,
    GUI_ERROR_CLASS_PARSER = 2
} gui_error_class_t;

/* Sticky count for any confirmed record-path loss; reset on record start. */
void gui_record_note_lost_block(void);

/* Hardware/stream watchdog interruption makes an active recording incomplete,
 * even if no ring-buffer block was dropped. Call BEFORE stopping capture. */
void gui_record_mark_capture_interrupted(gui_app_t *app, const char *reason);

/* Headless-side, once per 5s: raw input/encode throughput and record backlog. */
void gui_record_poll_performance(gui_app_t *app, double now);

// Initialize recording subsystem
void gui_record_init(void);

// Cleanup recording subsystem
void gui_record_cleanup(void);

// Recording start return codes
#define RECORD_OK       0   // Recording started successfully
#define RECORD_ERROR   -1   // Error occurred
#define RECORD_PENDING  1   // Waiting for user confirmation (popup shown)

// Start recording to files
// Returns RECORD_OK on success, RECORD_ERROR on error, RECORD_PENDING if waiting for confirmation
int gui_record_start(gui_app_t *app);

// Check popup result and continue recording if confirmed
// Call this each frame when gui_record_is_pending() returns true
void gui_record_check_popup(gui_app_t *app);

// Check if waiting for popup confirmation
bool gui_record_is_pending(void);

// Stop recording
void gui_record_stop(gui_app_t *app);

// Returns true while record-stop finalization is running in background.
bool gui_record_is_finalizing(void);

// Check if recording is active
bool gui_record_is_active(void);

// Check if any recording channel has a persistent output-file write error
// (e.g. the file is locked by another app). Used by the UI to flash the
// finalizing icon when capture ends while the write issue persists.
bool gui_record_has_write_error(void);

// Append a timestamped capture/record event to the active session log (if any)
void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t error_count);

// Spillover support for record-path backpressure.
// Channel: 0 = A, 1 = B.
bool gui_record_spill_is_forced(int channel);
// Clear the sticky spill-forced flag for a channel. Call this when the
// in-memory record ringbuffer write succeeds again, so the capture path
// returns to direct ringbuffer -> FLAC encoding and the spill temp file
// can be recycled via the existing drain path. Prevents a single
// backpressure blip from permanently routing recording through disk.
void gui_record_spill_clear_forced(int channel);
// Spillover support for record-path backpressure. Byte-generic: `data` may
// be the extraction thread's int16 sample blocks or the direct RAW path's
// native device bytes.
bool gui_record_spill_enqueue(gui_app_t *app, int channel, const void *data, size_t bytes,
                              uint32_t frame_index, char *error_msg, size_t error_msg_size);

// Read `bytes` from a channel's spill backlog into dst (byte-generic).
// Returns false when less than `bytes` remains (or no spill file is open).
bool gui_record_spill_read_block(int channel, void *dst, size_t bytes);

// Current spill backlog in bytes for a channel (0 when none).
uint64_t gui_record_spill_backlog_bytes(int channel);

// Direct native RAW passthrough (CXADC, FLAC off, per-channel resampling
// off): push the exact bytes just read from CXADC card `card` (0 = channel
// A, 1 = channel B) into that channel's record path. No-op unless the
// channel was set up as direct native at record start (decided by
// gui_record.c). Drop handling (capture log + stop_on_dropout) mirrors the
// extraction thread's record-drop path.
void gui_record_direct_tap_push(int card, const uint8_t *bytes, size_t len, uint32_t frame_index);

// Iteration-scoped tap gate for multi-channel lockstep capture threads
// (CXADC): latch the record decision once per iteration so a record
// start/stop handshake landing between the per-channel pushes cannot split
// the channels by one chunk. tap_channel_active() reports whether the
// channel is a direct native RAW channel this record session; enter/leave
// mark the channel's tap in-flight (covering the gated pushes for the
// stopper's disable + wait-idle handshake); tap_push_latched honors the
// iteration-latched decision.
bool gui_record_direct_tap_channel_active(int channel);
bool gui_record_tap_iteration_enter(int channel);
void gui_record_tap_iteration_leave(int channel);
void gui_record_direct_tap_push_latched(int channel, bool latched_on,
                                         const uint8_t *bytes, size_t len,
                                         uint32_t frame_index);

// Proactive low-disk guard for active recording paths.
// Returns true when free space drops below threshold and capture should be stopped safely.
bool gui_record_check_disk_space_guard(gui_app_t *app, uint32_t frame_index,
                                       char *status_msg, size_t status_msg_size);
// Query currently available free bytes for app->settings.output_path.
// Returns false when output_path is unset or the filesystem query fails.
bool gui_record_get_output_free_space_bytes(const gui_app_t *app, uint64_t *free_bytes_out);

// Live on-disk size of each channel's output file via stat(). For the UI
// readout so it shows the exact file size (1:1 with `ls`/file-manager), not
// the writer-thread atomic which can lead the flushed file by the FILE*
// buffer. Returns false when not recording; per-channel 0 if that file
// isn't open or stat fails.
bool gui_record_get_live_output_bytes(uint64_t *out_a, uint64_t *out_b);

#endif // GUI_RECORD_H
