/*
 * MISRC GUI - FFT Line Spectrum Display with GPU Phosphor Persistence
 *
 * Computes FFT of raw ADC samples in the display thread and renders spectrum
 * using GPU-accelerated phosphor persistence (same as oscilloscope phosphor).
 *
 * Thread safety: Display thread writes to back buffer, render thread reads front.
 * Double-buffering ensures lock-free operation between threads.
 *
 * Requires FFTW3 single-precision library (fftw3f).
 */

#ifndef GUI_FFT_H
#define GUI_FFT_H

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

// Include gui_app.h for waveform_sample_t definition
// (it's an anonymous struct so can't be forward-declared)
#include "../core/gui_app.h"
#include "gui_phosphor_rt.h"
#include "panel_interface.h"

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the FFT panel vtable with the panel registry.
// Call this once at startup.
void gui_fft_panel_register(void);

//-----------------------------------------------------------------------------
// FFT Configuration Constants
//-----------------------------------------------------------------------------

// FFT size limits (dynamically sized based on available samples)
#define FFT_SIZE_MIN          128    // Minimum FFT size
#define FFT_SIZE_MAX          4096   // Maximum FFT size (must be <= DISPLAY_BUFFER_SIZE)

// dB range for magnitude normalization
#define FFT_DB_MIN           -120.0f   // Minimum dB (bottom of display)
#define FFT_DB_MAX             0.0f   // Maximum dB (top of display)

#define FFT_DECAY_RATE        0.6f   // Per-frame decay (higher = slower fade)
#define FFT_HIT_INCREMENT     0.1f    // Intensity added per FFT hit
#define FFT_BLOOM             1.0f    // Bloom Factor
#define FFT_EMA_ALPHA         0.75f

//-----------------------------------------------------------------------------
// FFT State Structure
//-----------------------------------------------------------------------------

typedef struct fft_state {
    // FFTW state (opaque pointers to avoid requiring fftw3.h in header)
    void *fftw_plan;           // fftwf_plan
    float *fftw_input;         // Input buffer (fft_size floats)
    void *fftw_output;         // Output buffer (fftwf_complex, fft_bins)

    // Current FFT size (power of 2, dynamically adjusted)
    int fft_size;              // Current FFT window size
    int fft_bins;              // Output bins (fft_size/2 + 1)
    int allocated_size;        // Size of allocated buffers (to know when to reallocate)

    // Hanning window coefficients (precomputed for current size)
    float *window;             // fft_size floats

    // Double-buffered magnitude output for thread safety
    // Display thread writes to back buffer, render thread reads from front
    float *magnitude_front;    // fft_bins floats, read by render thread
    float *magnitude_back;     // fft_bins floats, written by display thread
    atomic_int magnitude_ready; // 1 when back buffer has new data to swap

    // Alias to magnitude_front for render-thread access
    float *magnitude;

    // Peak-hold magnitude for stable peak detection (max-hold with decay)
    float *peak_hold;          // fft_bins floats, decayed peak envelope

    // GPU phosphor render texture (shared module)
    phosphor_rt_t phosphor;    // Reusable phosphor persistence effect

    // Sample rate for frequency axis (set by display thread, read by render)
    atomic_uint sample_rate;   // Raw ADC sample rate in Hz

    // Indicates FFT data is available for rendering
    bool data_ready;

    // Initialization state
    bool initialized;

    // Peak label smoothing (EMA filtered position)
    float peak_label_x;        // Smoothed X position for peak label
    float peak_label_y;        // Smoothed Y position for peak label
    int peak_label_bin;        // Last selected peak bin (for label content)
    bool peak_label_active;    // Whether label is currently being displayed

    // Zoom and pan state (X-axis only, frequency domain)
    float zoom_level;          // Zoom factor (1.0 = full spectrum, >1 = zoomed in)
    float pan_offset;          // Pan offset in normalized frequency (0.0 - 1.0 range)
    bool dragging;             // Currently dragging to pan
    float drag_start_x;        // Mouse X position at drag start
    float drag_start_pan;      // Pan offset at drag start
} fft_state_t;

//-----------------------------------------------------------------------------
// FFT Availability Check
//-----------------------------------------------------------------------------

// Returns true if FFT support is compiled in (FFTW available)
bool gui_fft_available(void);

// FFTW non-execute operations share one process-wide lock. Callers retain
// ownership and must stop users of a plan or buffer before destroying it.
// Without FFTW, plan creation returns NULL and allocation uses malloc/free.
void *gui_fft_plan_create_r2c(int size, float *input, void *output, unsigned flags);
void gui_fft_plan_destroy(void *plan);
void *gui_fft_alloc(size_t bytes);
void gui_fft_free(void *memory);

//-----------------------------------------------------------------------------
// FFT Lifecycle Management
//-----------------------------------------------------------------------------

// Initialize FFT state with FFTW plan and buffers
// Returns true on success, false on failure or if FFTW not available
bool gui_fft_init(fft_state_t *state);

// Clear FFT buffers (reset to empty state)
void gui_fft_clear(fft_state_t *state);

// Free all FFT resources
void gui_fft_cleanup(fft_state_t *state);

//-----------------------------------------------------------------------------
// FFT Processing (called from display thread with raw ADC samples)
//-----------------------------------------------------------------------------

// Compute FFT from raw ADC samples (called from display thread)
// Writes results to back buffer for thread-safe transfer to render thread.
// Parameters:
//   state: FFT state structure
//   samples: Raw ADC samples (int16_t, signed)
//   count: Number of samples available
//   sample_rate: ADC sample rate in Hz (for frequency axis calculation)
void gui_fft_process_raw(fft_state_t *state, const int16_t *samples,
                         size_t count, uint32_t sample_rate);

// Swap buffers if new data available (called from render thread before rendering)
// Copies back buffer to front buffer if magnitude_ready flag is set.
void gui_fft_swap_buffers(fft_state_t *state);

//-----------------------------------------------------------------------------
// FFT Rendering (called from main/render thread)
//-----------------------------------------------------------------------------

// Render FFT spectrum with GPU phosphor persistence
// Uses same shader system as oscilloscope phosphor.
// Call gui_fft_swap_buffers() before this to get latest data.
// Parameters:
//   state: FFT state structure
//   x, y: Screen position
//   width, height: Display size
//   color: Line color for spectrum
//   fonts: Font array (index 0 = Inter, index 1 = Space Mono), NULL for default
void gui_fft_render(fft_state_t *state, float x, float y,
                    float width, float height, Color color, Font *fonts);

/* CPU FFT on M-Server and original phosphor/EMA rendering on M-Client. */
void gui_fft_server_bins(const int16_t *samples,size_t count,uint32_t sample_rate,
                         float *out,size_t bins);
void gui_fft_remote_apply_float(void *state,const float *bins,size_t count,
                                uint32_t sample_rate,uint32_t sequence);
#endif // GUI_FFT_H
