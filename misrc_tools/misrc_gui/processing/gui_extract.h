/*
 * MISRC GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#ifndef GUI_EXTRACT_H
#define GUI_EXTRACT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"

// Forward declarations
typedef struct gui_app gui_app_t;

// Initialize extraction subsystem (allocates buffers, gets extraction function)
void gui_extract_init(void);

// Cleanup extraction subsystem
void gui_extract_cleanup(void);

// Start the extraction thread (call after capture starts)
// Uses app->buffers (buffer_manager) for reading BUF_CAPTURE_RF
// Returns 0 on success, -1 on error
int gui_extract_start(gui_app_t *app);
// Start a new stopped session: clear old RF/display tails and reset sideband.
// Call before any capture producer starts. Recording buffers are not touched.
void gui_extract_prepare_stream(gui_app_t *app);
/* Poll the consumer's discard acknowledgement while the Client RF pump is paused. */
bool gui_extract_discard_client_rf(gui_app_t *app);
// Playback publishes one stamp for each committed 65536-sample pair block,
// before advancing the RF FIFO. Positions refer to source files, excluding padding.
#include "gui_rf_preview_worker.h"
int gui_extract_start_tagged(gui_app_t *app);
bool gui_extract_stamp_playback(const gui_rf_block_info_t info[2], const size_t counts[2]);
// Independent mixed-rate playback: each channel writes its OWN RF FIFO
// (BUF_CAPTURE_RF_A/B) and publishes a stamp into that channel's sideband
// ring before the FIFO write is committed. Positions/rates are that
// channel's own file clock (no shared axis).
bool gui_extract_stamp_playback_channel(int channel,
                                        const gui_rf_block_info_t *info,
                                        size_t count);
// Use -1 for the paired FIFO and 0/1 for independent playback. The forwarded
// cursor counts only stamps whose RF has reached the preview worker queues.
uint64_t gui_extract_playback_stamps_written(int channel);
uint64_t gui_extract_playback_stamps_forwarded(int channel);

// Stop the extraction thread (call before capture stops)
void gui_extract_stop(void);

// Check if extraction thread is running
bool gui_extract_is_running(void);

// Note: Record buffers now accessed via app->buffers (buffer_manager)
// Use BUF_RECORD_A and BUF_RECORD_B with bufmgr_read_begin/bufmgr_read_end

// Enable/disable recording mode
// When enabled, extraction thread writes to record ringbuffers
// rf_bits_* semantics:
// - If use_flac=true: bits per sample for FLAC stream (8/12/16)
// - If use_flac=false: bits per sample for RAW output (8/16)
// direct_a/direct_b: the channel is recorded via the direct native tap (its
// record ringbuffer producer is the capture-side tap, not this thread), so
// extraction skips that channel's record-buffer writes (single producer per
// ring).
/* Close record publication immediately. An in-flight block can still finish. */
void gui_extract_disable_recording(void);
/* Stop admitting samples after the producer position visible at this call. */
void gui_extract_request_record_stop(void);
/* Wait for pending RF publication and report whether its tail finished. */
bool gui_extract_wait_recording_idle(void);
void gui_extract_finish_recording(void);
void gui_extract_set_recording(bool enabled, bool use_flac, uint8_t rf_bits_a, uint8_t rf_bits_b,
                               bool direct_a, bool direct_b);

// Reset record buffers via buffer manager (call before starting writer threads)
void gui_extract_reset_record_rbs(gui_app_t *app);

// Initialize record buffers via buffer manager (for simulated capture that doesn't use extraction thread)
void gui_extract_init_record_rbs(gui_app_t *app);

// Check if recording is enabled and get FLAC mode
bool gui_extract_is_recording(bool *use_flac);

// Get the extraction function pointer (for direct use)
typedef void (*extract_fn_t)(uint32_t *buf, size_t num_samples, size_t *clip,
                             uint8_t *aux_buf, int16_t *buf_a, int16_t *buf_b,
                             uint16_t *peak);
extract_fn_t gui_extract_get_function(void);

// Get extraction output buffers (for direct use)
int16_t *gui_extract_get_buf_a(void);
int16_t *gui_extract_get_buf_b(void);
uint8_t *gui_extract_get_buf_aux(void);

// Update clip counts and peak values from extracted samples
// Processes sample buffers and updates app's atomic counters
void gui_extract_update_stats(gui_app_t *app, const int16_t *buf_a,
                              const int16_t *buf_b, size_t num_samples);

// Note: gui_extract_update_display() has been removed - display processing is now
// handled by the display thread via panel_process_all()

// Get the "data available" event (callback signals this after writing to ringbuffer)
// Returns NULL if extraction is not initialized
rb_event_t *gui_extract_get_data_event(void);

// Get the "space available" event (extraction signals this after consuming from ringbuffer)
// Returns NULL if extraction is not initialized
rb_event_t *gui_extract_get_space_event(void);

// Note: Record space events now managed by buffer manager
// Use bufmgr_get_space_event(&app->buffers, BUF_RECORD_A/B) if needed

#endif // GUI_EXTRACT_H
