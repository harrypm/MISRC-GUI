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

// Append a timestamped capture/record event to the active session log (if any)
void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t error_count);

// Spillover support for record-path backpressure.
// Channel: 0 = A, 1 = B.
bool gui_record_spill_is_forced(int channel);
bool gui_record_spill_enqueue(gui_app_t *app, int channel, const int16_t *samples, size_t bytes,
                              uint32_t frame_index, char *error_msg, size_t error_msg_size);

// Proactive low-disk guard for active recording paths.
// Returns true when free space drops below threshold and capture should be stopped safely.
bool gui_record_check_disk_space_guard(gui_app_t *app, uint32_t frame_index,
                                       char *status_msg, size_t status_msg_size);
// Query currently available free bytes for app->settings.output_path.
// Returns false when output_path is unset or the filesystem query fails.
bool gui_record_get_output_free_space_bytes(const gui_app_t *app, uint64_t *free_bytes_out);

#endif // GUI_RECORD_H
