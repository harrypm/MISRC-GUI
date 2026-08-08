/*
 * MISRC GUI - FFT Line Spectrum Display with GPU Phosphor Persistence
 *
 * Computes FFT of resampled display waveform and displays as a line-based spectrum
 * using GPU-accelerated phosphor persistence (shared module).
 *
 * Copyright (C) 2024-2025 vrunk11, stefan_o
 * Licensed under GNU GPL v3 or later
 */

#include "gui_fft.h"
#include "../core/gui_app.h"
#include "gui_phosphor_rt.h"
#include "../ui/gui_ui.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>

// Define M_PI if not available (Windows compatibility)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if LIBFFTW_ENABLED
#include <fftw3.h>
#endif

//-----------------------------------------------------------------------------
// Availability Check
//-----------------------------------------------------------------------------

bool gui_fft_available(void) {
#if LIBFFTW_ENABLED
    return true;
#else
    return false;
#endif
}

//-----------------------------------------------------------------------------
// FFT Lifecycle Management
//-----------------------------------------------------------------------------

// Helper: Find smallest power of 2 >= n
static int ceil_power_of_2(int n) {
    if (n <= 0) return 1;
    int power = 1;
    while (power < n) {
        power *= 2;
    }
    return power;
}

// Helper: Resize FFT buffers and recreate plan for new size
static bool fft_resize(fft_state_t *state, int new_size) {
#if LIBFFTW_ENABLED
    if (!state) return false;

    // Round up to power of 2 first
    new_size = ceil_power_of_2(new_size);

    // Clamp to valid range
    if (new_size < FFT_SIZE_MIN) new_size = FFT_SIZE_MIN;
    if (new_size > FFT_SIZE_MAX) new_size = FFT_SIZE_MAX;

    // Already the right size?
    if (state->allocated_size == new_size && state->fftw_plan) {
        return true;
    }

    int new_bins = new_size / 2 + 1;

    // Free old resources
    if (state->fftw_plan) {
        fftwf_destroy_plan((fftwf_plan)state->fftw_plan);
        state->fftw_plan = NULL;
    }
    if (state->fftw_input) {
        fftwf_free(state->fftw_input);
        state->fftw_input = NULL;
    }
    if (state->fftw_output) {
        fftwf_free(state->fftw_output);
        state->fftw_output = NULL;
    }
    if (state->window) {
        free(state->window);
        state->window = NULL;
    }
    // Free double-buffered magnitude arrays
    if (state->magnitude_front) {
        free(state->magnitude_front);
        state->magnitude_front = NULL;
    }
    if (state->magnitude_back) {
        free(state->magnitude_back);
        state->magnitude_back = NULL;
    }
    state->magnitude = NULL;  // Was alias to front
    if (state->peak_hold) {
        free(state->peak_hold);
        state->peak_hold = NULL;
    }

    // Allocate new buffers
    state->fftw_input = (float *)fftwf_malloc(sizeof(float) * new_size);
    if (!state->fftw_input) {
        fprintf(stderr, "[FFT] Failed to allocate FFTW input buffer (%d)\n", new_size);
        return false;
    }

    state->fftw_output = fftwf_malloc(sizeof(fftwf_complex) * new_bins);
    if (!state->fftw_output) {
        fprintf(stderr, "[FFT] Failed to allocate FFTW output buffer (%d bins)\n", new_bins);
        fftwf_free(state->fftw_input);
        state->fftw_input = NULL;
        return false;
    }

    // Create new FFTW plan
    state->fftw_plan = fftwf_plan_dft_r2c_1d(new_size, state->fftw_input,
                                              (fftwf_complex *)state->fftw_output,
                                              FFTW_ESTIMATE);  // Use ESTIMATE for faster plan creation
    if (!state->fftw_plan) {
        fprintf(stderr, "[FFT] Failed to create FFTW plan for size %d\n", new_size);
        fftwf_free(state->fftw_input);
        fftwf_free(state->fftw_output);
        state->fftw_input = NULL;
        state->fftw_output = NULL;
        return false;
    }

    // Allocate Hanning window
    state->window = (float *)malloc(sizeof(float) * new_size);
    if (!state->window) {
        fprintf(stderr, "[FFT] Failed to allocate window buffer (%d)\n", new_size);
        fftwf_destroy_plan((fftwf_plan)state->fftw_plan);
        fftwf_free(state->fftw_input);
        fftwf_free(state->fftw_output);
        state->fftw_plan = NULL;
        state->fftw_input = NULL;
        state->fftw_output = NULL;
        return false;
    }

    // Precompute Hanning window coefficients
    for (int i = 0; i < new_size; i++) {
        state->window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (new_size - 1)));
    }

    // Allocate double-buffered magnitude arrays
    state->magnitude_front = (float *)malloc(sizeof(float) * new_bins);
    state->magnitude_back = (float *)malloc(sizeof(float) * new_bins);
    if (!state->magnitude_front || !state->magnitude_back) {
        fprintf(stderr, "[FFT] Failed to allocate magnitude buffers (%d bins)\n", new_bins);
        fftwf_destroy_plan((fftwf_plan)state->fftw_plan);
        fftwf_free(state->fftw_input);
        fftwf_free(state->fftw_output);
        free(state->window);
        free(state->magnitude_front);
        free(state->magnitude_back);
        state->fftw_plan = NULL;
        state->fftw_input = NULL;
        state->fftw_output = NULL;
        state->window = NULL;
        state->magnitude_front = NULL;
        state->magnitude_back = NULL;
        return false;
    }
    memset(state->magnitude_front, 0, sizeof(float) * new_bins);
    memset(state->magnitude_back, 0, sizeof(float) * new_bins);
    state->magnitude = state->magnitude_front;  // Alias for compatibility

    // Allocate peak-hold buffer for stable peak detection
    state->peak_hold = (float *)malloc(sizeof(float) * new_bins);
    if (!state->peak_hold) {
        fprintf(stderr, "[FFT] Failed to allocate peak_hold buffer (%d bins)\n", new_bins);
        fftwf_destroy_plan((fftwf_plan)state->fftw_plan);
        fftwf_free(state->fftw_input);
        fftwf_free(state->fftw_output);
        free(state->window);
        free(state->magnitude_front);
        free(state->magnitude_back);
        state->fftw_plan = NULL;
        state->fftw_input = NULL;
        state->fftw_output = NULL;
        state->window = NULL;
        state->magnitude_front = NULL;
        state->magnitude_back = NULL;
        state->magnitude = NULL;
        return false;
    }
    memset(state->peak_hold, 0, sizeof(float) * new_bins);

    state->fft_size = new_size;
    state->fft_bins = new_bins;
    state->allocated_size = new_size;

    fprintf(stderr, "[FFT] Resized to %d samples (%d bins)\n", new_size, new_bins);
    return true;
#else
    (void)state;
    (void)new_size;
    return false;
#endif
}

bool gui_fft_init(fft_state_t *state) {
    if (!state) return false;

    // Clear state first
    memset(state, 0, sizeof(fft_state_t));

#if LIBFFTW_ENABLED
    // Initialize atomic variables
    atomic_init(&state->magnitude_ready, 0);
    atomic_init(&state->sample_rate, 0);

    // Initialize with minimum size - will be resized on first process call
    if (!fft_resize(state, FFT_SIZE_MIN)) {
        return false;
    }

    // Phosphor render textures will be created on first render (need OpenGL context)
    memset(&state->phosphor, 0, sizeof(phosphor_rt_t));

    // Set FFT-specific phosphor config
    state->phosphor.config.decay_rate = FFT_DECAY_RATE;
    state->phosphor.config.hit_increment = FFT_HIT_INCREMENT;
    state->phosphor.config.bloom_intensity = FFT_BLOOM;

    state->data_ready = false;
    state->initialized = true;

    // Initialize zoom/pan state
    state->zoom_level = 1.0f;
    state->pan_offset = 0.0f;
    state->dragging = false;
    state->drag_start_x = 0.0f;
    state->drag_start_pan = 0.0f;

    fprintf(stderr, "[FFT] Initialized with dynamic sizing (%d-%d samples)\n",
            FFT_SIZE_MIN, FFT_SIZE_MAX);

    return true;
#else
    fprintf(stderr, "[FFT] FFTW not available, FFT support disabled\n");
    return false;
#endif
}

void gui_fft_clear(fft_state_t *state) {
    if (!state || !state->initialized) return;

#if LIBFFTW_ENABLED
    // Clear both magnitude buffers
    if (state->magnitude_front && state->fft_bins > 0) {
        memset(state->magnitude_front, 0, sizeof(float) * state->fft_bins);
    }
    if (state->magnitude_back && state->fft_bins > 0) {
        memset(state->magnitude_back, 0, sizeof(float) * state->fft_bins);
    }
    atomic_store(&state->magnitude_ready, 0);

    // Clear peak-hold buffer
    if (state->peak_hold && state->fft_bins > 0) {
        memset(state->peak_hold, 0, sizeof(float) * state->fft_bins);
    }

    // Clear phosphor render textures
    phosphor_rt_clear(&state->phosphor);

    // Reset peak label smoothing state
    state->peak_label_active = false;
    state->peak_label_x = 0.0f;
    state->peak_label_y = 0.0f;
    state->peak_label_bin = 0;

    // Reset zoom/pan state
    state->zoom_level = 1.0f;
    state->pan_offset = 0.0f;
    state->dragging = false;

    state->data_ready = false;
#endif
}

void gui_fft_cleanup(fft_state_t *state) {
    if (!state) return;

#if LIBFFTW_ENABLED
    if (state->fftw_plan) {
        fftwf_destroy_plan((fftwf_plan)state->fftw_plan);
        state->fftw_plan = NULL;
    }

    if (state->fftw_input) {
        fftwf_free(state->fftw_input);
        state->fftw_input = NULL;
    }

    if (state->fftw_output) {
        fftwf_free(state->fftw_output);
        state->fftw_output = NULL;
    }

    if (state->window) {
        free(state->window);
        state->window = NULL;
    }

    // Free double-buffered magnitude arrays
    if (state->magnitude_front) {
        free(state->magnitude_front);
        state->magnitude_front = NULL;
    }
    if (state->magnitude_back) {
        free(state->magnitude_back);
        state->magnitude_back = NULL;
    }
    state->magnitude = NULL;

    if (state->peak_hold) {
        free(state->peak_hold);
        state->peak_hold = NULL;
    }

    phosphor_rt_cleanup(&state->phosphor);
#endif

    state->initialized = false;
}

//-----------------------------------------------------------------------------
// FFT Processing - Compute FFT from raw ADC samples (display thread)
//-----------------------------------------------------------------------------

void gui_fft_process_raw(fft_state_t *state, const int16_t *samples,
                         size_t count, uint32_t sample_rate) {
#if LIBFFTW_ENABLED
    if (!state || !state->initialized || !samples) return;

    // Store sample rate for render thread (atomic)
    atomic_store(&state->sample_rate, sample_rate);

    // Need minimum samples
    if (count < FFT_SIZE_MIN) return;

    // Determine FFT size: round UP sample count to nearest power of 2 for zero-padding
    int target_size = ceil_power_of_2((int)count);
    if (target_size > FFT_SIZE_MAX) target_size = FFT_SIZE_MAX;
    if (target_size < FFT_SIZE_MIN) target_size = FFT_SIZE_MIN;

    // Resize if needed (note: not thread-safe, but resize only happens rarely)
    if (target_size != state->fft_size) {
        if (!fft_resize(state, target_size)) {
            return;  // Resize failed
        }
    }

    int fft_size = state->fft_size;
    int fft_bins = state->fft_bins;

    // Use all available samples (up to fft_size)
    size_t samples_to_use = (count > (size_t)fft_size) ? (size_t)fft_size : count;

    // Calculate DC offset (mean of actual samples) to remove from signal
    // Use int32 accumulator to avoid overflow
    int64_t dc_sum = 0;
    for (size_t i = 0; i < samples_to_use; i++) {
        dc_sum += samples[i];
    }
    float dc_offset = (float)dc_sum / (float)samples_to_use;

    // Zero-pad symmetrically: place windowed data in center of FFT buffer
    int pad_total = fft_size - (int)samples_to_use;
    int pad_left = pad_total / 2;
    int pad_right = pad_total - pad_left;

    // Zero-pad left side
    for (int i = 0; i < pad_left; i++) {
        state->fftw_input[i] = 0.0f;
    }

    // Apply Hanning window to actual sample data and place in center
    // Normalize int16 samples to -1.0 to +1.0 range
    // const float scale = 1.0f / 32768.0f; --- Results in low FFT scale ~30db
    const float scale = 1.0f / 2048.0f;
    for (size_t i = 0; i < samples_to_use; i++) {
        float window_val = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(samples_to_use - 1)));
        float sample_val = ((float)samples[i] - dc_offset) * scale;
        state->fftw_input[pad_left + (int)i] = sample_val * window_val;
    }

    // Zero-pad right side
    for (int i = 0; i < pad_right; i++) {
        state->fftw_input[pad_left + (int)samples_to_use + i] = 0.0f;
    }

    // Execute FFT
    fftwf_execute((fftwf_plan)state->fftw_plan);

    // Compute magnitude in dB, then normalize to 0-1
    // Write to back buffer (display thread owns this)
    fftwf_complex *output = (fftwf_complex *)state->fftw_output;
    float *back = state->magnitude_back;

    for (int j = 0; j < fft_bins; j++) {
        float real = output[j][0];
        float imag = output[j][1];
        float mag = sqrtf(real * real + imag * imag);

        // Normalize by actual sample count (not padded size) for correct amplitude
        mag /= (float)samples_to_use;

        // Hanning window coherent gain correction (window gain = 0.5, correction = x2) MA - correct FFT window Scaling
        mag *= 2.0f;
        
        // Convert to dB (with small epsilon to avoid log(0))
        float db = 20.0f * log10f(mag + 1e-10f);

        // Clamp to dB range
        if (db < FFT_DB_MIN) db = FFT_DB_MIN;
        if (db > FFT_DB_MAX) db = FFT_DB_MAX;

        // Convert dB to normalized intensity (0-1)
        back[j] = (db - FFT_DB_MIN) / (FFT_DB_MAX - FFT_DB_MIN);
    }

    // Signal that new data is ready for the render thread
    atomic_store(&state->magnitude_ready, 1);
#else
    (void)state;
    (void)samples;
    (void)count;
    (void)sample_rate;
#endif
}

//-----------------------------------------------------------------------------
// FFT Buffer Swap - Copy back buffer to front (render thread)
//-----------------------------------------------------------------------------

void gui_fft_swap_buffers(fft_state_t *state) {
#if LIBFFTW_ENABLED
    if (!state || !state->initialized) return;

    // Check if new data is available
    if (atomic_exchange(&state->magnitude_ready, 0)) {
        // Copy back buffer to front buffer
        // Apply EMA smoothing during copy for temporal smoothing
        int fft_bins = state->fft_bins;
        float *front = state->magnitude_front;
        float *back = state->magnitude_back;

        for (int j = 0; j < fft_bins; j++) {
            float previous = front[j];
            float current = back[j];
            front[j] = FFT_EMA_ALPHA * previous + (1.0f - FFT_EMA_ALPHA) * current;
        }

        state->data_ready = true;
    }
#else
    (void)state;
#endif
}

//-----------------------------------------------------------------------------
// FFT Rendering - GPU phosphor persistence using shared module
//-----------------------------------------------------------------------------

// Grid settings for FFT (matching oscilloscope style)
#define FFT_GRID_MIN_SPACING_PX 80   // Minimum pixels between frequency grid lines
#define FFT_GRID_MAX_DIVISIONS 12    // Maximum number of frequency divisions

// Peak detection settings
#define FFT_PEAK_SEARCH_RADIUS_PX 30   // Pixels around mouse to search for peaks
#define FFT_PEAK_MIN_PROMINENCE 0.02f  // Minimum prominence (0-1) to qualify as a peak
#define FFT_PEAK_MAX_CANDIDATES 32     // Maximum peaks to consider when finding closest
#define FFT_PEAK_MAX_DISPLAY 1         // Maximum peaks to display near mouse
#define FFT_PEAK_HOLD_DECAY 0.99f     // Per-frame decay for peak hold (closer to 1 = slower decay)
#define FFT_PEAK_LABEL_EMA_ALPHA 0.1f // EMA smoothing for label position (lower = smoother)

// Peak info structure for rendering
typedef struct {
    int bin;           // FFT bin index
    float magnitude;   // Normalized magnitude (0-1)
    float freq_hz;     // Frequency in Hz
    float db;          // dB value
    float screen_x;    // Screen X position
    float screen_y;    // Screen Y position
} fft_peak_t;

// Find peaks near a given bin index within a search radius
// Returns number of peaks found (up to max_peaks)
static int fft_find_peaks_near(const float *magnitude, int num_bins,
                               int center_bin, int search_radius,
                               float min_prominence,
                               fft_peak_t *peaks, int max_peaks) {
    if (!magnitude || num_bins < 3 || !peaks || max_peaks <= 0) return 0;

    int start_bin = center_bin - search_radius;
    int end_bin = center_bin + search_radius;
    if (start_bin < 1) start_bin = 1;
    if (end_bin >= num_bins - 1) end_bin = num_bins - 2;

    int peak_count = 0;

    // Find local maxima in the search range
    for (int i = start_bin; i <= end_bin && peak_count < max_peaks; i++) {
        float val = magnitude[i];
        float prev = magnitude[i - 1];
        float next = magnitude[i + 1];

        // Check if this is a local maximum
        if (val > prev && val > next) {
            // Calculate prominence: how much higher than surrounding valleys
            // Look left for valley
            float left_valley = val;
            for (int j = i - 1; j >= 0; j--) {
                if (magnitude[j] < left_valley) {
                    left_valley = magnitude[j];
                }
                if (magnitude[j] > val) break;  // Hit a higher peak
            }

            // Look right for valley
            float right_valley = val;
            for (int j = i + 1; j < num_bins; j++) {
                if (magnitude[j] < right_valley) {
                    right_valley = magnitude[j];
                }
                if (magnitude[j] > val) break;  // Hit a higher peak
            }

            float prominence = val - fmaxf(left_valley, right_valley);

            if (prominence >= min_prominence) {
                peaks[peak_count].bin = i;
                peaks[peak_count].magnitude = val;
                peak_count++;
            }
        }
    }

    // Sort peaks by magnitude (descending) - simple bubble sort for small array
    for (int i = 0; i < peak_count - 1; i++) {
        for (int j = i + 1; j < peak_count; j++) {
            if (peaks[j].magnitude > peaks[i].magnitude) {
                fft_peak_t tmp = peaks[i];
                peaks[i] = peaks[j];
                peaks[j] = tmp;
            }
        }
    }

    return peak_count;
}

// Snap to 1-2-5 log scale sequence (same as oscilloscope)
static double fft_snap_to_125(double value) {
    if (value <= 0) return 1.0;

    double log_val = log10(value);
    double magnitude = pow(10.0, floor(log_val));
    double normalized = value / magnitude;

    double snapped;
    if (normalized < 1.5) {
        snapped = 1.0;
    } else if (normalized < 3.5) {
        snapped = 2.0;
    } else if (normalized < 7.5) {
        snapped = 5.0;
    } else {
        snapped = 10.0;
    }

    return snapped * magnitude;
}

// Format frequency value with appropriate unit (Hz, kHz, MHz)
static void format_freq_label(char *buf, size_t buf_size, double hz) {
    if (hz >= 1000000.0) {
        snprintf(buf, buf_size, "%.3gMHz", hz / 1000000.0);
    } else if (hz >= 1000.0) {
        snprintf(buf, buf_size, "%.3gkHz", hz / 1000.0);
    } else {
        snprintf(buf, buf_size, "%.3gHz", hz);
    }
}

// Helper to draw text with font
static void fft_draw_text(Font *fonts, const char *text, float px, float py, int fontSize, Color color) {
    if (fonts) {
        DrawTextEx(fonts[0], text, (Vector2){px, py}, (float)fontSize, 1.0f, color);
    } else {
        DrawText(text, (int)px, (int)py, fontSize, color);
    }
}

// Helper to draw text with monospace font (for numbers)
static void fft_draw_text_mono(Font *fonts, const char *text, float px, float py, int fontSize, Color color) {
    if (fonts) {
        DrawTextEx(fonts[1], text, (Vector2){px, py}, (float)fontSize, 1.0f, color);
    } else {
        DrawText(text, (int)px, (int)py, fontSize, color);
    }
}

// Helper to measure text with font
static int fft_measure_text(Font *fonts, const char *text, int fontSize) {
    if (fonts) {
        Vector2 size = MeasureTextEx(fonts[0], text, (float)fontSize, 1.0f);
        return (int)size.x;
    }
    return MeasureText(text, fontSize);
}

void gui_fft_render(fft_state_t *state, float x, float y,
                    float width, float height, Color color, Font *fonts) {
#if LIBFFTW_ENABLED
    if (!state || !state->initialized) return;

    (void)color; // Not used currently

    // Get sample rate from atomic (set by display thread)
    uint32_t sample_rate = atomic_load(&state->sample_rate);

    int rt_width = (int)width;
    int rt_height = (int)height;

    // Ensure we have phosphor render textures of the right size
    if (!phosphor_rt_init(&state->phosphor, rt_width, rt_height)) {
        // Fallback: just draw background
        DrawRectangle((int)x, (int)y, rt_width, rt_height, COLOR_METER_BG);
        fft_draw_text(fonts, "FFT: GPU init failed", x + 10, y + 10, FONT_SIZE_OSC_SCALE, (Color){255, 80, 80, 255});
        return;
    }

    // Begin phosphor frame (applies decay and prepares for drawing)
    phosphor_rt_begin_frame(&state->phosphor);

    // Draw FFT bins as connected line segments (with zoom/pan)
    if (state->magnitude && state->data_ready && state->fft_bins > 1) {
        Color lineColor = phosphor_rt_get_draw_color(&state->phosphor);

        int fft_bins = state->fft_bins;
        float zoom = state->zoom_level;
        float pan = state->pan_offset;

        // Calculate visible bin range based on zoom/pan
        // pan_offset is normalized frequency (0-1), zoom_level scales the view
        // visible range: [pan, pan + 1/zoom] in normalized frequency
        float visible_start = pan;
        float visible_end = pan + 1.0f / zoom;

        // Convert to bin indices
        int start_bin = (int)(visible_start * (fft_bins - 1));
        int end_bin = (int)ceilf(visible_end * (fft_bins - 1));
        if (start_bin < 0) start_bin = 0;
        if (end_bin >= fft_bins) end_bin = fft_bins - 1;

        for (int bin = start_bin; bin < end_bin; bin++) {
            float intensity1 = state->magnitude[bin];
            float intensity2 = state->magnitude[bin + 1];

            // Convert bin to normalized frequency (0-1)
            float freq_norm1 = (float)bin / (float)(fft_bins - 1);
            float freq_norm2 = (float)(bin + 1) / (float)(fft_bins - 1);

            // Map to screen X using zoom/pan
            // screen_x = (freq_norm - pan) * zoom * width
            float x1 = (freq_norm1 - pan) * zoom * rt_width;
            float x2 = (freq_norm2 - pan) * zoom * rt_width;

            // Y positions based on intensity (0 = bottom, 1 = top)
            float y1 = rt_height - (intensity1 * rt_height);
            float y2 = rt_height - (intensity2 * rt_height);

            // Draw line segment
            DrawLineEx((Vector2){x1, y1}, (Vector2){x2, y2}, 1.5f, lineColor);
        }
    }

    // End phosphor frame (finalize accumulation buffer)
    phosphor_rt_end_frame(&state->phosphor);

    // Draw background (same as oscilloscope)
    DrawRectangle((int)x, (int)y, rt_width, rt_height, COLOR_METER_BG);

    // Grid setup
    float db_range = FFT_DB_MAX - FFT_DB_MIN;
    float rough_db_division = db_range * (float)FFT_GRID_MIN_SPACING_PX / height;
    float db_division = (float)fft_snap_to_125((double)rough_db_division);

    // First gridline
    float first_db = ceilf(FFT_DB_MIN / db_division) * db_division;
    int div_count = 0;

    for (float db = first_db;
        db <= FFT_DB_MAX && div_count < FFT_GRID_MAX_DIVISIONS;
        db += db_division)
    {
        // Skip min/max gridlines (draw only interior lines)
        if (db <= FFT_DB_MIN || db >= FFT_DB_MAX)
            continue;

        float normalized = (db - FFT_DB_MIN) / db_range;
        float line_y = y + height - (normalized * height);

        bool is_zero = (db == 0.0f);

        DrawLineV((Vector2){x, line_y}, (Vector2){x + width, line_y},
                is_zero ? COLOR_GRID_MAJOR : COLOR_GRID);

        char label[16];
        snprintf(label, sizeof(label), "%+.0fdB", db);
        fft_draw_text_mono(fonts, label, x + 5, line_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);

        div_count++;
    }

    // Draw vertical frequency grid lines with 1-2-5 snapping (with zoom/pan support)
    if (sample_rate > 0) {
        float nyquist = (float)sample_rate / 2.0f;
        float zoom = state->zoom_level;
        float pan = state->pan_offset;

        // Visible frequency range based on zoom/pan
        float visible_freq_start = pan * nyquist;
        float visible_freq_end = (pan + 1.0f / zoom) * nyquist;
        float visible_freq_range = visible_freq_end - visible_freq_start;

        // Calculate frequency per pixel for visible range
        float freq_per_pixel = visible_freq_range / width;

        // Calculate rough frequency division to get reasonable spacing
        float rough_division = freq_per_pixel * (float)FFT_GRID_MIN_SPACING_PX;

        // Snap to 1-2-5 sequence
        float freq_division = (float)fft_snap_to_125((double)rough_division);

        // Find first grid line in visible range
        float first_grid_freq = ceilf(visible_freq_start / freq_division) * freq_division;

        // Draw vertical grid lines at frequency intervals
        char freq_buf[32];
        div_count = 0;
        for (float freq = first_grid_freq; freq < visible_freq_end && div_count < FFT_GRID_MAX_DIVISIONS; freq += freq_division) {
            // Convert frequency to screen position using zoom/pan
            float freq_norm = freq / nyquist;  // Normalized frequency (0-1)
            float screen_norm = (freq_norm - pan) * zoom;  // Normalized screen position (0-1)
            float line_x = x + (screen_norm * width);

            // Only draw if within visible bounds
            if (line_x >= x && line_x <= x + width) {
                DrawLineV((Vector2){line_x, y}, (Vector2){line_x, y + height}, COLOR_GRID);

                // Draw frequency label (skip if too close to edges)
                if (line_x > x + 30 && line_x < x + width - 30) {
                    format_freq_label(freq_buf, sizeof(freq_buf), freq);
                    int label_w = fft_measure_text(fonts, freq_buf, FONT_SIZE_OSC_SCALE);
                    fft_draw_text_mono(fonts, freq_buf, line_x - label_w / 2, y + height - 14, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
                }
            }
            div_count++;
        }

        // Show frequency per division in top-right corner (below FFT label)
        format_freq_label(freq_buf, sizeof(freq_buf), freq_division);
        char div_label[48];
        snprintf(div_label, sizeof(div_label), "%s/div", freq_buf);
        int div_label_w = fft_measure_text(fonts, div_label, FONT_SIZE_OSC_DIV);
        fft_draw_text_mono(fonts, div_label, x + width - div_label_w - 8, y + 26, FONT_SIZE_OSC_DIV, COLOR_TEXT);

        // Show zoom level if zoomed in
        if (zoom > 1.01f) {
            char zoom_label[32];
            snprintf(zoom_label, sizeof(zoom_label), "%.1fx", zoom);
            int zoom_label_w = fft_measure_text(fonts, zoom_label, FONT_SIZE_OSC_DIV);
            fft_draw_text_mono(fonts, zoom_label, x + width - zoom_label_w - 8, y + 42, FONT_SIZE_OSC_DIV, COLOR_TEXT_DIM);
        }
    }

    // Draw center line (0V equivalent position not applicable for FFT, skip)

    // Border (same as oscilloscope)
    DrawRectangleLinesEx((Rectangle){x, y, width, height}, 1, COLOR_GRID_MAJOR);

    // Render phosphor texture with heatmap colormap and bloom
    phosphor_rt_render(&state->phosphor, x, y, true);

    // Update peak-hold buffer: max-hold with decay for stable peak detection
    if (state->magnitude && state->peak_hold && state->fft_bins > 1) {
        int fft_bins = state->fft_bins;
        for (int i = 0; i < fft_bins; i++) {
            // Decay existing peak-hold value
            state->peak_hold[i] *= FFT_PEAK_HOLD_DECAY;
            // Take max of decayed value and current magnitude
            if (state->magnitude[i] > state->peak_hold[i]) {
                state->peak_hold[i] = state->magnitude[i];
            }
        }
    }

    // Peak detection on mouse hover - show closest peak to mouse (with zoom/pan support)
    if (state->peak_hold && state->data_ready && state->fft_bins > 1 && sample_rate > 0) {
        Vector2 mouse = GetMousePosition();
        Rectangle fft_rect = {x, y, width, height};

        if (CheckCollisionPointRec(mouse, fft_rect)) {
            float nyquist = (float)sample_rate / 2.0f;
            int fft_bins = state->fft_bins;
            float zoom = state->zoom_level;
            float pan = state->pan_offset;

            // Convert mouse X to normalized frequency using zoom/pan
            // screen_norm = (freq_norm - pan) * zoom
            // => freq_norm = screen_norm / zoom + pan
            float screen_norm = (mouse.x - x) / width;  // 0.0 to 1.0
            float freq_norm = screen_norm / zoom + pan;

            // Convert to bin index
            int mouse_bin = (int)(freq_norm * (fft_bins - 1) + 0.5f);
            if (mouse_bin < 0) mouse_bin = 0;
            if (mouse_bin >= fft_bins) mouse_bin = fft_bins - 1;

            // Calculate search radius in bins (adjusted for zoom)
            // At higher zoom, bins are wider on screen, so search covers fewer bins
            float bin_width_screen = width * zoom / (float)(fft_bins - 1);
            int search_radius_bins = (int)(FFT_PEAK_SEARCH_RADIUS_PX / bin_width_screen + 0.5f);
            if (search_radius_bins < 2) search_radius_bins = 2;

            // Find peaks near mouse using peak-hold data (stable envelope)
            // Use larger buffer to find all candidates, then select closest
            fft_peak_t peaks[FFT_PEAK_MAX_CANDIDATES];
            int peak_count = fft_find_peaks_near(state->peak_hold, fft_bins,
                                                  mouse_bin, search_radius_bins,
                                                  FFT_PEAK_MIN_PROMINENCE,
                                                  peaks, FFT_PEAK_MAX_CANDIDATES);

            // Find the best peak - balance between closest to mouse and largest magnitude
            if (peak_count > 0) {
                // Find the maximum magnitude among candidates for normalization
                float max_mag = peaks[0].magnitude;
                for (int i = 1; i < peak_count; i++) {
                    if (peaks[i].magnitude > max_mag) {
                        max_mag = peaks[i].magnitude;
                    }
                }

                // Score each peak: use circular (Euclidean) distance from mouse
                // Calculate screen position for each peak and measure actual distance
                // Weight magnitude more heavily so prominent peaks are preferred
                const float distance_weight = 0.3f;
                const float magnitude_weight = 0.7f;
                float search_radius_px = (float)FFT_PEAK_SEARCH_RADIUS_PX;

                int best_idx = 0;
                float best_score = -1.0f;

                for (int i = 0; i < peak_count; i++) {
                    // Calculate screen position for this peak using zoom/pan
                    int bin = peaks[i].bin;
                    float peak_mag = state->peak_hold[bin];
                    float bin_freq_norm = (float)bin / (float)(fft_bins - 1);
                    float px = x + (bin_freq_norm - pan) * zoom * width;
                    float py = y + height - (peak_mag * height);

                    // Calculate circular (Euclidean) distance from mouse
                    float dx = px - mouse.x;
                    float dy = py - mouse.y;
                    float dist = sqrtf(dx * dx + dy * dy);

                    // Only consider peaks within circular radius
                    if (dist > search_radius_px) continue;

                    float distance_score = 1.0f - dist / search_radius_px;
                    float magnitude_score = (max_mag > 0) ? peaks[i].magnitude / max_mag : 0.0f;
                    float score = distance_weight * distance_score + magnitude_weight * magnitude_score;

                    if (score > best_score) {
                        best_score = score;
                        best_idx = i;
                    }
                }

                // Skip if no peak within circular radius
                if (best_score < 0) {
                    state->peak_label_active = false;
                } else {

                // Get the peak-hold magnitude for rendering
                int peak_bin = peaks[best_idx].bin;
                float peak_mag = state->peak_hold[peak_bin];

                // Calculate screen position for closest peak using zoom/pan
                float peak_freq_norm = (float)peak_bin / (float)(fft_bins - 1);
                float peak_x = x + (peak_freq_norm - pan) * zoom * width;
                float peak_y = y + height - (peak_mag * height);

                // Calculate frequency and dB
                float freq = (float)peak_bin / (float)(fft_bins - 1) * nyquist;
                float db = FFT_DB_MIN + peak_mag * db_range;

                // Draw peak marker (dot) - at actual peak position (no smoothing)
                float dot_radius = 4.0f;
                DrawCircleV((Vector2){peak_x, peak_y}, dot_radius, COLOR_TEXT);

                // Apply EMA smoothing to label position only
                float label_anchor_x, label_anchor_y;
                if (!state->peak_label_active) {
                    // First time - initialize directly to raw position
                    state->peak_label_x = peak_x;
                    state->peak_label_y = peak_y;
                    state->peak_label_bin = peak_bin;
                    state->peak_label_active = true;
                    label_anchor_x = peak_x;
                    label_anchor_y = peak_y;
                } else {
                    // Apply EMA: smoothed = alpha * raw + (1 - alpha) * previous
                    state->peak_label_x = FFT_PEAK_LABEL_EMA_ALPHA * peak_x +
                                          (1.0f - FFT_PEAK_LABEL_EMA_ALPHA) * state->peak_label_x;
                    state->peak_label_y = FFT_PEAK_LABEL_EMA_ALPHA * peak_y +
                                          (1.0f - FFT_PEAK_LABEL_EMA_ALPHA) * state->peak_label_y;
                    state->peak_label_bin = peak_bin;
                    label_anchor_x = state->peak_label_x;
                    label_anchor_y = state->peak_label_y;
                }

                // Format frequency label
                char freq_buf[32];
                format_freq_label(freq_buf, sizeof(freq_buf), freq);

                // Format dB label
                char db_buf[16];
                snprintf(db_buf, sizeof(db_buf), "%.1fdB", db);

                // Combined label
                char peak_label[64];
                snprintf(peak_label, sizeof(peak_label), "%s %s", db_buf, freq_buf);

                // Draw label with background for readability
                // Use smoothed anchor position for label placement
                int label_w = fft_measure_text(fonts, peak_label, FONT_SIZE_OSC_SCALE);
                float label_x = label_anchor_x - label_w / 2;
                float label_y = label_anchor_y - dot_radius - 10 - FONT_SIZE_OSC_SCALE;

                // Keep label on screen
                if (label_x < x + 2) label_x = x + 2;
                if (label_x + label_w > x + width - 2) label_x = x + width - label_w - 2;
                if (label_y < y + 2) label_y = label_anchor_y + dot_radius + 4;

                // Draw background rectangle
                // DrawRectangle((int)(label_x - 2), (int)(label_y - 1),
                //              label_w + 4, FONT_SIZE_OSC_SCALE + 2,
                //              (Color){0, 0, 0, 180});

                // Draw label text
                fft_draw_text_mono(fonts, peak_label, label_x, label_y,
                                   FONT_SIZE_NORMAL, COLOR_TEXT);
                }  // end if (best_score >= 0)
            } else {
                // No peak found - deactivate label smoothing
                state->peak_label_active = false;
            }
        } else {
            // Mouse left FFT area - deactivate label smoothing
            state->peak_label_active = false;
        }
    }

    // Draw "FFT" label in top-right corner (matching oscilloscope channel label style)
    const char *fft_label = "FFT";
    int label_width = fft_measure_text(fonts, fft_label, FONT_SIZE_OSC_LABEL);
    fft_draw_text(fonts, fft_label, x + width - label_width - 8, y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);

#else
    (void)state;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
    (void)fonts;
#endif
}

//=============================================================================
// Zoom and Pan Interaction
//=============================================================================

// Zoom limits
#define FFT_ZOOM_MIN 1.0f      // No zoom (full spectrum)
#define FFT_ZOOM_MAX 64.0f     // Maximum zoom (64x)
#define FFT_ZOOM_FACTOR 1.15f  // Zoom step per scroll tick

// Handle mouse scroll for zoom (X-axis only, centered on mouse position)
static bool fft_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
#if LIBFFTW_ENABLED
    if (!state_ptr || delta == 0.0f) return false;

    fft_state_t *state = (fft_state_t *)state_ptr;
    if (!state->initialized) return false;

    Vector2 mouse = GetMousePosition();

    // Check if mouse is inside bounds
    if (!CheckCollisionPointRec(mouse, bounds)) return false;

    // Calculate the normalized frequency position under mouse before zoom
    float rel_x = (mouse.x - bounds.x) / bounds.width;  // 0.0 to 1.0
    float freq_under_mouse = state->pan_offset + rel_x / state->zoom_level;

    // Apply zoom
    if (delta > 0.0f) {
        // Scroll up = zoom in
        state->zoom_level *= FFT_ZOOM_FACTOR;
        if (state->zoom_level > FFT_ZOOM_MAX) state->zoom_level = FFT_ZOOM_MAX;
    } else {
        // Scroll down = zoom out
        state->zoom_level /= FFT_ZOOM_FACTOR;
        if (state->zoom_level < FFT_ZOOM_MIN) state->zoom_level = FFT_ZOOM_MIN;
    }

    // Adjust pan to keep the frequency under mouse at the same screen position
    // freq_under_mouse = pan_offset + rel_x / zoom_level
    // => pan_offset = freq_under_mouse - rel_x / zoom_level
    state->pan_offset = freq_under_mouse - rel_x / state->zoom_level;

    // Clamp pan to valid range [0, 1 - 1/zoom]
    float max_pan = 1.0f - 1.0f / state->zoom_level;
    if (max_pan < 0.0f) max_pan = 0.0f;
    if (state->pan_offset < 0.0f) state->pan_offset = 0.0f;
    if (state->pan_offset > max_pan) state->pan_offset = max_pan;

    return true;
#else
    (void)state_ptr;
    (void)delta;
    (void)bounds;
    return false;
#endif
}

// Handle mouse click for drag-to-pan
static bool fft_handle_click(void *state_ptr, struct gui_app *app, int channel,
                              Vector2 click, Rectangle bounds) {
#if LIBFFTW_ENABLED
    (void)app;
    (void)channel;

    if (!state_ptr) return false;

    fft_state_t *state = (fft_state_t *)state_ptr;
    if (!state->initialized) return false;

    // Only allow panning when zoomed in
    if (state->zoom_level <= 1.0f) return false;

    // Check if click is inside bounds
    if (!CheckCollisionPointRec(click, bounds)) {
        state->dragging = false;
        return false;
    }

    // Start dragging
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        state->dragging = true;
        state->drag_start_x = click.x;
        state->drag_start_pan = state->pan_offset;
        return true;
    }

    return false;
#else
    (void)state_ptr;
    (void)app;
    (void)channel;
    (void)click;
    (void)bounds;
    return false;
#endif
}

// Update drag state (call from render to handle continuous drag)
static void fft_update_drag(fft_state_t *state, Rectangle bounds) {
#if LIBFFTW_ENABLED
    if (!state || !state->initialized || !state->dragging) return;

    Vector2 mouse = GetMousePosition();

    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        // Calculate pan delta based on mouse movement
        float dx = mouse.x - state->drag_start_x;
        // Convert pixel delta to normalized frequency delta
        // At zoom_level, the visible frequency range is 1/zoom_level
        // So pixel movement maps to frequency: dx / width * (1/zoom_level)
        float freq_delta = -dx / bounds.width / state->zoom_level;
        state->pan_offset = state->drag_start_pan + freq_delta;

        // Clamp pan to valid range
        float max_pan = 1.0f - 1.0f / state->zoom_level;
        if (max_pan < 0.0f) max_pan = 0.0f;
        if (state->pan_offset < 0.0f) state->pan_offset = 0.0f;
        if (state->pan_offset > max_pan) state->pan_offset = max_pan;
    } else {
        // Mouse released - stop dragging
        state->dragging = false;
    }
#else
    (void)state;
    (void)bounds;
#endif
}

//=============================================================================
// Panel Interface (vtable) Implementation
//=============================================================================

// FFT panel state is just the base fft_state_t
// Processing happens in display thread, rendering in render thread

//-----------------------------------------------------------------------------
// Lifecycle Functions
//-----------------------------------------------------------------------------

static void *fft_vtable_create(void) {
#if LIBFFTW_ENABLED
    fft_state_t *state = calloc(1, sizeof(fft_state_t));
    if (!state) return NULL;

    if (!gui_fft_init(state)) {
        free(state);
        return NULL;
    }

    return state;
#else
    return NULL;
#endif
}

static void fft_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    fft_state_t *state = (fft_state_t *)state_ptr;
    gui_fft_cleanup(state);
    free(state);
}

static void fft_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    fft_state_t *state = (fft_state_t *)state_ptr;
    gui_fft_clear(state);
}

//-----------------------------------------------------------------------------
// Processing Function (called from display thread with raw ADC samples)
//-----------------------------------------------------------------------------

static void fft_vtable_process(void *state_ptr, const int16_t *samples, size_t count, uint32_t sample_rate) {
    if (!state_ptr || !samples || count == 0) return;
    fft_state_t *state = (fft_state_t *)state_ptr;

    // Process raw ADC samples and compute FFT
    gui_fft_process_raw(state, samples, count, sample_rate);
}

//-----------------------------------------------------------------------------
// Rendering Functions
//-----------------------------------------------------------------------------

static void fft_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                              Rectangle bounds, Color channel_color) {
    (void)channel;  // FFT uses sample_rate from state, not channel-specific

    if (!state_ptr || !app) return;

    fft_state_t *fft = (fft_state_t *)state_ptr;

    if (!fft->initialized) {
        const char *text = gui_fft_available() ? "FFT Initializing..." : "FFT Not Available";
        int text_width = MeasureText(text, FONT_SIZE_OSC_MSG);
        DrawText(text, (int)(bounds.x + bounds.width/2 - text_width/2),
                 (int)(bounds.y + bounds.height/2 - 12),
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    // Update drag state for pan (continuous while mouse is held)
    fft_update_drag(fft, bounds);

    // Swap buffers to get latest FFT data from display thread
    gui_fft_swap_buffers(fft);

    // Render FFT (sample_rate is read atomically inside gui_fft_render)
    gui_fft_render(fft, bounds.x, bounds.y, bounds.width, bounds.height,
                   channel_color, app->fonts);
}

//-----------------------------------------------------------------------------
// Vtable Definition
//-----------------------------------------------------------------------------

static const panel_vtable_t s_fft_vtable = {
    .name = "FFT",

    // Lifecycle
    .create = fft_vtable_create,
    .destroy = fft_vtable_destroy,
    .clear = fft_vtable_clear,

    // Processing (no-op for FFT - uses display samples in render)
    .process = fft_vtable_process,

    // Rendering
    .render = fft_vtable_render,
    .render_overlay = NULL,  // No overlay for FFT

    // Interaction
    .handle_click = fft_handle_click,    // Drag to pan
    .handle_scroll = fft_handle_scroll,  // Scroll to zoom (centered on mouse)

    // Menus
    .get_menu_count = NULL,
    .get_menu = NULL,
};

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------

void gui_fft_panel_register(void) {
    panel_register(PANEL_VIEW_FFT, &s_fft_vtable);
}
