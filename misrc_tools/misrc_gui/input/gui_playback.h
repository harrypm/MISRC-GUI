/*
 * MISRC GUI - Playback Mode
 *
 * Provides playback of previously recorded FLAC files as if they were being
 * captured live from a device. This allows reviewing recordings with the same
 * visualization and analysis tools available during live capture.
 *
 * Features:
 * - Reads FLAC files recorded by MISRC (40kHz sample rate, 8/12/16-bit)
 * - Plays back at real-time rate to simulate live capture
 * - Supports separate files for Channel A and Channel B
 * - Integrates with display, VU meters, and recording pipeline
 * - Playback controls: play, pause, seek, speed adjustment
 */

#ifndef GUI_PLAYBACK_H
#define GUI_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../common/file_utils.h"

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// Playback Configuration
//-----------------------------------------------------------------------------

#define PLAYBACK_SAMPLE_RATE      40000     // 40 kHz - matches MISRC recordings
#define PLAYBACK_BUFFER_SIZE      65536     // Samples per batch (matches simulated)
#define PLAYBACK_UPDATE_INTERVAL_MS  2      // Time between batches (matches simulated)

// Playback speed presets
typedef enum {
    PLAYBACK_SPEED_0_25X = 0,   // 0.25x speed (slow motion)
    PLAYBACK_SPEED_0_5X,        // 0.5x speed
    PLAYBACK_SPEED_1X,          // Real-time (default)
    PLAYBACK_SPEED_2X,          // 2x speed
    PLAYBACK_SPEED_4X,          // 4x speed
    PLAYBACK_SPEED_MAX,         // As fast as possible (no throttling)
    PLAYBACK_SPEED_COUNT
} playback_speed_t;

// Playback state
typedef enum {
    PLAYBACK_STATE_STOPPED = 0,
    PLAYBACK_STATE_PLAYING,
    PLAYBACK_STATE_PAUSED,
    PLAYBACK_STATE_EOF           // End of file reached
} playback_state_t;

//-----------------------------------------------------------------------------
// Playback File Info
//-----------------------------------------------------------------------------

typedef struct {
    char filepath[FILE_PATH_MAX];          // Full path to FLAC file
    uint32_t sample_rate;        // Sample rate from FLAC STREAMINFO header
                                 // (kHz-domain value for RF captures, e.g. 20000 = 20 MSPS)
    uint8_t bits_per_sample;     // Bits per sample (8, 12, 16)
    uint64_t total_samples;      // TRUE total samples in the stream (probe/tag resolved,
                                 // NOT the raw header value on finalized files)
    uint64_t header_total_samples; // Raw STREAMINFO total_samples value (diagnostics:
                                 // kHz-domain RF/1000 on finalized files, may be 0)
    double duration_seconds;     // TRUE total duration (total_samples / real_sample_rate_hz)
    double real_sample_rate_hz;  // Resolved real sample rate in Hz (e.g. 20000000 for a
                                 // 20 MSPS RF capture, or 44100 for a true audio file)
    bool valid;                  // File was successfully opened/validated
    bool length_provisional;     // Bounded tail endpoint; full reader still verifies it
    bool length_verification_failed; // Full reader rejected/retracted that endpoint
} playback_file_info_t;

//-----------------------------------------------------------------------------
// Playback API
//-----------------------------------------------------------------------------

// Start playback with specified FLAC files for each channel
// Either file_a or file_b can be NULL for single-channel playback
// Returns 0 on success, -1 on error
int gui_playback_start(gui_app_t *app, const char *file_a, const char *file_b);

// UI startup: file preparation is private to a joinable background loader.
// Poll every render frame (also publishes background lengths):
// 1 startup committed, -1 failed/cancelled, 0 no startup result.
int gui_playback_start_async(gui_app_t *app, const char *file_a, const char *file_b);
bool gui_playback_is_starting(gui_app_t *app);
int gui_playback_poll_start(gui_app_t *app);
void gui_playback_cancel_start(gui_app_t *app);
// Shutdown only: cancel and join before releasing the app and its buffers.
void gui_playback_finish_start(gui_app_t *app);


// Stop playback and close files
void gui_playback_stop(gui_app_t *app);

// Check if playback is currently active
bool gui_playback_is_running(gui_app_t *app);

// A valid file may have zero/unknown length while the independent reader runs.
// These getters, file-info getters and poll/stop belong to the render thread.
bool gui_playback_is_indexing(gui_app_t *app, int channel); // 0=A, 1=B
typedef struct {
    uint64_t bytes_scanned;       // Audio bytes read, excluding FLAC metadata
    uint64_t total_bytes;
    bool active;                 // Includes a requested channel waiting its turn
    bool queued;
    bool waiting;                // Yielding to capture/record/playback pressure
} playback_index_progress_t;
// Render-thread snapshot; returns false and zeros out when no work is pending.
bool gui_playback_get_index_progress(gui_app_t *app, int channel,
                                     playback_index_progress_t *out);
double gui_playback_get_channel_rate_hz(gui_app_t *app, int channel);

// Get current playback state
playback_state_t gui_playback_get_state(gui_app_t *app);

// Pause/resume playback
void gui_playback_pause(gui_app_t *app);
void gui_playback_resume(gui_app_t *app);

// Toggle pause state
void gui_playback_toggle_pause(gui_app_t *app);

// Seek to position (0.0 to 1.0 normalized, or absolute sample position)
void gui_playback_seek_normalized(gui_app_t *app, double position);
void gui_playback_seek_sample(gui_app_t *app, uint64_t sample);
void gui_playback_seek_sample_channel(gui_app_t *app, int channel, uint64_t sample); // channel: 0=A, 1=B

// Get current position
uint64_t gui_playback_get_position_samples(gui_app_t *app);
uint64_t gui_playback_get_position_samples_channel(gui_app_t *app, int channel); // channel: 0=A, 1=B
double gui_playback_get_position_normalized(gui_app_t *app);
double gui_playback_get_position_seconds(gui_app_t *app);

// Per-channel seek sequence: increments on every successful seek of that
// channel (scrub / wheel / paused backfill). Display panels use it to detect
// NON-LINEAR playhead moves exactly (a fixed sample-count threshold cannot
// work: the per-slice position advance scales with the playback speed and
// the file's sample rate) and clear rolling history on a jump.
uint64_t gui_playback_get_seek_seq_channel(gui_app_t *app, int channel); // channel: 0=A, 1=B

// Get total duration
uint64_t gui_playback_get_total_samples(gui_app_t *app);
double gui_playback_get_duration_seconds(gui_app_t *app);

// Playback speed control
void gui_playback_set_speed(gui_app_t *app, playback_speed_t speed);
playback_speed_t gui_playback_get_speed(gui_app_t *app);
const char* gui_playback_speed_name(playback_speed_t speed);

// Get file info for loaded files
bool gui_playback_get_file_info_a(gui_app_t *app, playback_file_info_t *info);
bool gui_playback_get_file_info_b(gui_app_t *app, playback_file_info_t *info);

// Get file info for the playback file selected in settings (channel: 0=A, 1=B).
// Returns the live playback file info while playback is running; otherwise
// validates the settings-selected file (cached by path, so it is cheap to
// call per frame) so the UI can show bit depth / sample rate / duration
// readouts before playback starts. Returns true when the info is valid.
bool gui_playback_get_selected_file_info(gui_app_t *app, int channel, playback_file_info_t *info);

// Validate a FLAC file before playback (check format compatibility)
// Returns true if file is compatible with MISRC playback
bool gui_playback_validate_file(const char *filepath, playback_file_info_t *info);

// Loop mode: restart from beginning when EOF reached
void gui_playback_set_loop(gui_app_t *app, bool loop);
bool gui_playback_get_loop(gui_app_t *app);

//-----------------------------------------------------------------------------
// DAW-Style Paused Analysis Window (direct file random access)
//-----------------------------------------------------------------------------
// While PAUSED the display panels render analysis windows straight from the
// FLAC file (the rolling live pipeline only carries one 65,536-sample display
// frame — 3.3 ms at 20 MSPS — which is 1:1-correct for playing, but far too
// small for paused analysis). A dedicated analysis decoder per channel
// (own FILE handle + IO layer + STREAMINFO patch, isolated from the playback
// thread's decoders) decodes the requested window asynchronously:
//   gui_playback_request_analysis_window: center/span -> worker (non-blocking)
//   gui_playback_poll_analysis_window:    newest completed result (or false)
// The span is DURATION-based (sample rates vary across files: 5/10/20/40
// MSPS) and centered on the playhead. Results are SLICE_COUNT FFT-sized
// sample slices strided across the window; the spectrogram panel FFTs and
// renders them across its full history width.

#define PLAYBACK_ANALYSIS_SLICES 512        // == SPEC_TIME_BINS (gui_spectrogram.c); keep in sync
#define PLAYBACK_ANALYSIS_SLICE_SAMPLES 4096
#define PLAYBACK_ANALYSIS_BINS (PLAYBACK_ANALYSIS_SLICE_SAMPLES / 2 + 1)  // 2049

// FULL-RESOLUTION contiguous model: each column of the result is ONE true
// FFT over that column's entire contiguous sample span (hop = span/SLICES,
// zero-padded to the next power of two for FFTW). No sub-window sampling,
// no averaging, no max-hold — the span simply trades time vs frequency
// resolution exactly like an FFT-size setting in a DAW. Longer spans are
// capped by the per-column FFT budget (rate-dependent, duration-based).
#define PLAYBACK_ANALYSIS_HOP_MAX 262144   // Max samples per column FFT (2^18)

void gui_playback_request_analysis_window(gui_app_t *app, int channel,
                                           uint64_t center_sample,
                                           double span_seconds);
// Returns true when a result newer than *io_seq is available; updates *io_seq
// and points the outputs at the published per-slice magnitude frames: a
// slice_count x BINS float array (normalized 0-1 intensities, max-hold across
// the slice's sub-windows — see the HARD DEV NOTE in gui_playback.c). The
// render thread only resamples + LUT-maps these (no FFTs on the render path).
// The array is pinned until the next successful poll on this channel or stop;
// do not retain it across disconnect/reconnect.
bool gui_playback_poll_analysis_window(gui_app_t *app, int channel,
                                        uint64_t *io_seq,
                                        const float **out_magnitudes,
                                        size_t *out_slice_count,
                                        size_t *out_bins);

#endif // GUI_PLAYBACK_H
