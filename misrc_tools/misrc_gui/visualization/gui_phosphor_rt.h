/*
 * MISRC GUI - Digital Phosphor Render Texture Module
 *
 * GPU-accelerated phosphor persistence effect using ping-pong render textures.
 * Used for oscilloscope waveforms, FFT spectrums, or any line-based display.
 *
 * Simulates analog oscilloscope phosphor persistence with heatmap coloring.
 * Pixels accumulate intensity where waveforms pass through, creating a
 * thermal-style visualization (blue = cold/rare, red = hot/frequent).
 *
 * Architecture:
 *   - Two RenderTexture2D (ping-pong for persistence)
 *   - Draw primitives to accumulation buffer with additive blending
 *   - Decay shader applies per-frame fade
 *   - Composite shader renders with heatmap colormap and bloom
 */

#ifndef GUI_PHOSPHOR_RT_H
#define GUI_PHOSPHOR_RT_H

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>

// Include gui_app.h for waveform_sample_t (anonymous struct can't be forward-declared)
#include "../core/gui_app.h"

//-----------------------------------------------------------------------------
// Phosphor Configuration (per-instance settings)
//-----------------------------------------------------------------------------

typedef struct phosphor_rt_config {
    float decay_rate;         // Per-frame decay multiplier (0.0-1.0, higher = slower fade)
    float hit_increment;      // Intensity added per waveform hit (0-1, typically 0.5)
    float bloom_intensity;    // Bloom/glow strength (0.0-2.0, 1.0 = default CRT-like bloom)
    float channel_color[3];   // RGB color for opacity mode (0-1 range)
} phosphor_rt_config_t;

// Default configuration values (used as fallback)
#define PHOSPHOR_DEFAULT_DECAY_RATE    0.75f
#define PHOSPHOR_DEFAULT_HIT_INCREMENT 0.5f
#define PHOSPHOR_DEFAULT_BLOOM         1.0f

//-----------------------------------------------------------------------------
// Phosphor Render Texture State
//-----------------------------------------------------------------------------

typedef struct phosphor_rt {
    RenderTexture2D rt[2];    // Ping-pong render textures
    int rt_index;             // Current render texture index (0 or 1)
    int width;                // Physical render texture width
    int height;               // Physical render texture height
    int logical_width;        // Destination width before global UI scaling
    int logical_height;       // Destination height before global UI scaling
    bool valid;               // True if render textures are initialized
    Matrix outer_modelview;   // UI transform saved across Begin/EndTextureMode
    bool outer_modelview_saved;
    phosphor_rt_config_t config;  // Per-instance configuration
} phosphor_rt_t;

//-----------------------------------------------------------------------------
// Shader Management (shared across all phosphor_rt instances)
//-----------------------------------------------------------------------------

// Initialize phosphor shaders (call once at startup or on first use)
// Returns true on success
bool phosphor_rt_init_shaders(void);

// Cleanup phosphor shaders (call at application exit)
void phosphor_rt_cleanup_shaders(void);

//-----------------------------------------------------------------------------
// Render Texture Lifecycle
//-----------------------------------------------------------------------------

// Initialize or resize a phosphor render texture pair. Dimensions are logical
// UI pixels; render scales keep the off-screen texture at framebuffer density.
// Returns true on success, false on allocation failure
bool phosphor_rt_init(phosphor_rt_t *prt, int logical_width,
                      int logical_height, float render_scale_x,
                      float render_scale_y);

// Clear render textures (reset to black)
void phosphor_rt_clear(phosphor_rt_t *prt);

// Free render texture resources
void phosphor_rt_cleanup(phosphor_rt_t *prt);

// Set configuration for this instance
void phosphor_rt_set_config(phosphor_rt_t *prt, const phosphor_rt_config_t *config);

// Set individual config values
void phosphor_rt_set_decay_rate(phosphor_rt_t *prt, float decay_rate);
void phosphor_rt_set_hit_increment(phosphor_rt_t *prt, float hit_increment);
void phosphor_rt_set_bloom_intensity(phosphor_rt_t *prt, float bloom_intensity);
void phosphor_rt_set_channel_color(phosphor_rt_t *prt, const float *channel_color);

//-----------------------------------------------------------------------------
// Rendering Pipeline
//-----------------------------------------------------------------------------

// Begin a new frame: apply decay to previous frame and prepare for drawing
// Call this before drawing any primitives
// Uses decay_rate from instance config
void phosphor_rt_begin_frame(phosphor_rt_t *prt);

// End frame drawing (call after drawing all primitives)
// This finalizes the accumulation buffer
void phosphor_rt_end_frame(phosphor_rt_t *prt);

// Render the phosphor texture to screen with heatmap colormap and bloom
void phosphor_rt_render(phosphor_rt_t *prt, float x, float y, bool use_alpha_blend);

// Render the phosphor texture with channel color (opacity mode)
// Uses channel_color from instance config
void phosphor_rt_render_opacity(phosphor_rt_t *prt, float x, float y);

// Get the intensity value to use when drawing primitives (for additive blending)
// Uses hit_increment from instance config
Color phosphor_rt_get_draw_color(phosphor_rt_t *prt);

//-----------------------------------------------------------------------------
// Waveform Drawing Helpers
//-----------------------------------------------------------------------------

// Draw waveform to phosphor render texture
// Must be called between phosphor_rt_begin_frame() and phosphor_rt_end_frame()
// Uses hit_increment from instance config
void phosphor_rt_draw_waveform(phosphor_rt_t *prt,
                               const waveform_sample_t *samples, size_t sample_count,
                               float amplitude_scale);

// Same as phosphor_rt_draw_waveform but with an explicit horizontal step
// between consecutive points (in logical pixels): point i is drawn at
// x = i * x_step. Used by the waveform panel's render-resolution selector
// (a point count below the panel width stretches across the panel).
void phosphor_rt_draw_waveform_xstep(phosphor_rt_t *prt,
                                     const waveform_sample_t *samples, size_t sample_count,
                                     float amplitude_scale, float x_step);

//-----------------------------------------------------------------------------
// Channel colors for opacity mode (RGB 0-1 range)
//-----------------------------------------------------------------------------

extern const float PHOSPHOR_CHANNEL_COLOR_A[3];  // Green
extern const float PHOSPHOR_CHANNEL_COLOR_B[3];  // Yellow

#endif // GUI_PHOSPHOR_RT_H
