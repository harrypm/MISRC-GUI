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
#include <stdint.h>

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
    char filepath[512];          // Full path to FLAC file
    uint32_t sample_rate;        // Sample rate from FLAC metadata
    uint8_t bits_per_sample;     // Bits per sample (8, 12, 16)
    uint64_t total_samples;      // Total samples in file
    double duration_seconds;     // Total duration
    bool valid;                  // File was successfully opened/validated
} playback_file_info_t;

//-----------------------------------------------------------------------------
// Playback API
//-----------------------------------------------------------------------------

// Start playback with specified FLAC files for each channel
// Either file_a or file_b can be NULL for single-channel playback
// Returns 0 on success, -1 on error
int gui_playback_start(gui_app_t *app, const char *file_a, const char *file_b);

// Stop playback and close files
void gui_playback_stop(gui_app_t *app);

// Check if playback is currently active
bool gui_playback_is_running(gui_app_t *app);

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

// Get current position
uint64_t gui_playback_get_position_samples(gui_app_t *app);
double gui_playback_get_position_normalized(gui_app_t *app);
double gui_playback_get_position_seconds(gui_app_t *app);

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

// Validate a FLAC file before playback (check format compatibility)
// Returns true if file is compatible with MISRC playback
bool gui_playback_validate_file(const char *filepath, playback_file_info_t *info);

// Loop mode: restart from beginning when EOF reached
void gui_playback_set_loop(gui_app_t *app, bool loop);
bool gui_playback_get_loop(gui_app_t *app);

#endif // GUI_PLAYBACK_H
