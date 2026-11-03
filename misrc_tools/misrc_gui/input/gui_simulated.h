/*
 * MISRC GUI - Simulated Device (Test Signal Generator)
 *
 * Provides a simulated capture device for development and testing without
 * hardware. Streams the Test Gen signal through the full capture pipeline
 * (waveform, FFT, Video panel decode, VHS FM preview, recording) like a
 * live capture:
 *
 * - Channel A: hacktv-accurate composite CVBS (PAL/NTSC/SECAM colour bars)
 * - Channel B: VHS-family tape RF (FM luma + color-under QAM, or the
 *   divided-by-four SECAM chroma FM), matching the RF preview decoder
 *   profiles so the VHS FM panel decodes it live
 *
 * Both channels follow app->settings.video_system and hot-swap when it
 * changes (PAL -> NTSC -> SECAM via the Gen box on the waveform panel).
 * Recording support (RAW and FLAC) works like any live capture.
 */

#ifndef GUI_SIMULATED_H
#define GUI_SIMULATED_H

#include <stdbool.h>
#include <stdint.h>

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// Simulated Device Configuration
//-----------------------------------------------------------------------------

#define SIM_SAMPLE_RATE      40000000    // 40 MSPS - matches real hardware
#define SIM_BUFFER_SIZE      65536       // Samples per batch
#define SIM_UPDATE_INTERVAL_MS  2        // Time between batches

//-----------------------------------------------------------------------------
// Simulated Device API
//-----------------------------------------------------------------------------

// Start simulated capture
// Returns 0 on success, -1 on error
int gui_simulated_start(gui_app_t *app);

// Stop simulated capture
void gui_simulated_stop(gui_app_t *app);

// Check if simulated capture is running
bool gui_simulated_is_running(gui_app_t *app);

#endif // GUI_SIMULATED_H
