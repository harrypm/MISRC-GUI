/*
 * MISRC GUI - Waveform Panel Implementation
 *
 * Waveform panel vtable (line and phosphor modes), trigger detection,
 * per-panel resampling, and grid rendering.
 */

#include "gui_oscilloscope.h"
#include "gui_phosphor_rt.h"
#include "gui_fft.h"
#include "../signal/gui_trigger.h"
#include "../ui/gui_popup.h"
#include "gui_text.h"
#include "../ui/gui_ui.h"
#include "gui_panel.h"
#include "../ui/gui_dropdown.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LIBSOXR_ENABLED
#include <soxr.h>
#endif

//-----------------------------------------------------------------------------
// Trigger Mode Labels (for dropdown UI)
//-----------------------------------------------------------------------------

static const char *s_trigger_mode_labels[] = {
    [TRIGGER_MODE_RISING] = "Rising",
    [TRIGGER_MODE_FALLING] = "Falling",
    [TRIGGER_MODE_SYNC] = "Sync",
    [TRIGGER_MODE_CVBS_HSYNC] = "CVBS",
};
static const char *s_trigger_mode_short_labels[] = {
    [TRIGGER_MODE_RISING] = "Rise",
    [TRIGGER_MODE_FALLING] = "Fall",
    [TRIGGER_MODE_SYNC] = "Sync",
    [TRIGGER_MODE_CVBS_HSYNC] = "CVBS",
};
static const char *s_trigger_source_labels[] = {
    [TRIGGER_SOURCE_CH1] = "Ch1",
    [TRIGGER_SOURCE_CH2] = "Ch2",
    [TRIGGER_SOURCE_CH3] = "Ch3",
};

static const char *s_render_mode_labels[] = {
    [WAVEFORM_MODE_LINE] = "Line",
    [WAVEFORM_MODE_PHOSPHOR] = "Phosphor",
};

//-----------------------------------------------------------------------------
// Waveform Panel State (per-panel trigger, resampling, and display)
//-----------------------------------------------------------------------------

typedef struct {
    // Render mode (line vs phosphor)
    waveform_render_mode_t render_mode;

    // Trigger settings (per-panel, independent)
    bool trigger_enabled;
    int16_t trigger_level;
    trigger_mode_t trigger_mode;
    trigger_source_t trigger_source;
    float zoom_scale;
    int trigger_display_pos;

    // Resampler state (per-panel)
    void *resampler;           // soxr_t handle
    float resampler_ratio;     // Current decimation ratio

    // Display buffer (per-panel)
    waveform_sample_t display_samples[DISPLAY_BUFFER_SIZE];
    size_t display_samples_available;
    atomic_int display_width;

    // Phosphor state (allocated on demand for phosphor mode)
    phosphor_rt_t *phosphor;

    // Phosphor color mode
    phosphor_color_mode_t phosphor_color;

    // UI overlay state - two dropdowns: render mode and trigger mode
    Rectangle time_div_rect;
    Rectangle render_mode_btn_rect;
    Rectangle render_mode_opts_rect[WAVEFORM_MODE_COUNT];
    bool render_mode_dropdown_open;

    Rectangle trigger_btn_rect;
    Rectangle trigger_mode_opts_rect[TRIGGER_MODE_COUNT + 1];        // +1 for \"Off\"
    Rectangle trigger_source_opts_rect[TRIGGER_SOURCE_COUNT];
    bool trigger_dropdown_open;

    panel_menu_item_t menu_items[TRIGGER_MODE_COUNT];

    // Drag state for trigger level
    bool dragging;

    // CVBS detected levels (updated when CVBS trigger mode active)
    int16_t cvbs_sync_level;    // Sync tip level (lowest peak)
    int16_t cvbs_blank_level;   // Blanking level (second peak)
    int16_t cvbs_threshold;     // Detection threshold (midpoint)
    bool cvbs_levels_valid;     // True if levels were successfully detected

    // Protects shared state accessed by both display and render threads.
    atomic_flag data_lock;

    // Initialization flag
    bool initialized;
    bool trigger_source_defaulted_by_channel;
} waveform_panel_state_t;

typedef struct {
    bool trigger_enabled;
    int16_t trigger_level;
    trigger_mode_t trigger_mode;
    trigger_source_t trigger_source;
    float zoom_scale;
} waveform_runtime_cfg_t;

typedef struct {
    bool trigger_enabled;
    int16_t trigger_level;
    trigger_mode_t trigger_mode;
    int trigger_display_pos;
    int16_t cvbs_sync_level;
    int16_t cvbs_blank_level;
    int16_t cvbs_threshold;
    bool cvbs_levels_valid;
    float zoom_scale;
    size_t samples_available;
} waveform_render_snapshot_t;

// Forward declarations for static helper functions
static void waveform_cleanup_resampler(waveform_panel_state_t *state);
static size_t waveform_resample_to_buffer(waveform_panel_state_t *state,
                                           waveform_sample_t *dest,
                                           const int16_t *buf, size_t num_samples,
                                           size_t start_idx, float decimation,
                                           size_t target_width);
static bool waveform_process_display(waveform_panel_state_t *state,
                                      const waveform_runtime_cfg_t *cfg,
                                      const int16_t *buf, size_t num_samples,
                                      uint32_t sample_rate_hz,
                                      waveform_sample_t *out_samples,
                                      size_t *out_available,
                                      int *out_trigger_display_pos);

static inline void waveform_state_lock(waveform_panel_state_t *state)
{
    while (atomic_flag_test_and_set(&state->data_lock)) {
        /* spin */
    }
}

static inline void waveform_state_unlock(waveform_panel_state_t *state)
{
    atomic_flag_clear(&state->data_lock);
}

static waveform_runtime_cfg_t waveform_capture_runtime_cfg(waveform_panel_state_t *state)
{
    waveform_runtime_cfg_t cfg;
    waveform_state_lock(state);
    cfg.trigger_enabled = state->trigger_enabled;
    cfg.trigger_level = state->trigger_level;
    cfg.trigger_mode = state->trigger_mode;
    cfg.trigger_source = state->trigger_source;
    cfg.zoom_scale = state->zoom_scale;
    waveform_state_unlock(state);
    return cfg;
}

static void waveform_capture_render_snapshot(waveform_panel_state_t *state,
                                             waveform_render_snapshot_t *snap,
                                             waveform_sample_t *samples_out)
{
    if (!state || !snap || !samples_out) return;

    waveform_state_lock(state);
    snap->trigger_enabled = state->trigger_enabled;
    snap->trigger_level = state->trigger_level;
    snap->trigger_mode = state->trigger_mode;
    snap->trigger_display_pos = state->trigger_display_pos;
    snap->cvbs_sync_level = state->cvbs_sync_level;
    snap->cvbs_blank_level = state->cvbs_blank_level;
    snap->cvbs_threshold = state->cvbs_threshold;
    snap->cvbs_levels_valid = state->cvbs_levels_valid;
    snap->zoom_scale = state->zoom_scale;
    snap->samples_available = state->display_samples_available;
    if (snap->samples_available > DISPLAY_BUFFER_SIZE) {
        snap->samples_available = DISPLAY_BUFFER_SIZE;
    }
    if (snap->samples_available > 0) {
        memcpy(samples_out,
               state->display_samples,
               snap->samples_available * sizeof(waveform_sample_t));
    }
    waveform_state_unlock(state);
}

//-----------------------------------------------------------------------------
// Grid Settings
//-----------------------------------------------------------------------------

#define GRID_DIVISIONS_Y 4  // Per channel (amplitude)
#define GRID_MIN_SPACING_PX 120  // Minimum pixels between time grid lines
#define GRID_MAX_DIVISIONS 20   // Maximum number of time divisions

//-----------------------------------------------------------------------------
// Cleanup
//-----------------------------------------------------------------------------

void gui_oscilloscope_cleanup(void) {
    // No static resources - per-panel cleanup handled by vtable destroy()
}

static const int16_t *s_trigger_src_ch1 = NULL;
static const int16_t *s_trigger_src_ch2 = NULL;
static size_t s_trigger_src_count = 0;

void gui_waveform_set_trigger_source_buffers(const int16_t *samples_ch1,
                                             const int16_t *samples_ch2,
                                             size_t count)
{
    s_trigger_src_ch1 = samples_ch1;
    s_trigger_src_ch2 = samples_ch2;
    s_trigger_src_count = count;
}

static inline trigger_source_t waveform_default_trigger_source_for_channel(int channel)
{
    return (channel == 1) ? TRIGGER_SOURCE_CH2 : TRIGGER_SOURCE_CH1;
}

static void waveform_apply_channel_trigger_source_default(waveform_panel_state_t *state, int channel)
{
    if (!state) return;
    waveform_state_lock(state);
    if (!state->trigger_source_defaulted_by_channel) {
        state->trigger_source = waveform_default_trigger_source_for_channel(channel);
        state->trigger_source_defaulted_by_channel = true;
    }
    waveform_state_unlock(state);
}

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

// Snap to 1-2-5 log scale sequence
// Given a rough time division, find the nearest "nice" value in the sequence:
// ...0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100...
static double snap_to_125(double value) {
    if (value <= 0) return 1.0;

    // Find the order of magnitude (power of 10)
    double log_val = log10(value);
    double magnitude = pow(10.0, floor(log_val));
    double normalized = value / magnitude;  // Will be between 1 and 10

    // Snap to 1, 2, or 5 within this magnitude
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

// Format time value with appropriate unit (ns, us, ms, s)
static void format_time_label(char *buf, size_t buf_size, double seconds) {
    if (seconds >= 1.0) {
        snprintf(buf, buf_size, "%.3gs", seconds);
    } else if (seconds >= 0.001) {
        snprintf(buf, buf_size, "%.3gms", seconds * 1000.0);
    } else if (seconds >= 0.000001) {
        snprintf(buf, buf_size, "%.3gus", seconds * 1000000.0);
    } else {
        snprintf(buf, buf_size, "%.3gns", seconds * 1000000000.0);
    }
}

// Draw grid for a single channel with amplitude scale ticks
// zoom_scale: samples per pixel, sample_rate: samples per second
// trigger_enabled: if true, use trigger_display_pos as t=0 reference
// trigger_display_pos: pixel position of trigger point (-1 if not triggered)
void draw_channel_grid(float x, float y, float width, float height,
                       const char *label, Color channel_color, bool show_grid,
                       float zoom_scale, uint32_t sample_rate,
                       bool trigger_enabled, int trigger_display_pos,
                       Rectangle *time_div_rect) {
    if (time_div_rect) *time_div_rect = (Rectangle){0};
    // Background slightly darker than main bg
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){25, 25, 30, 255});

    float center_y = y + height / 2;

    if (show_grid) {
        // Time-based vertical grid lines (if we have sample rate info)
        if (sample_rate > 0 && zoom_scale > 0) {
            // Calculate time per pixel
            double time_per_pixel = (double)zoom_scale / (double)sample_rate;

            // Calculate rough time division to get reasonable spacing
            double rough_division = time_per_pixel * (double)GRID_MIN_SPACING_PX;

            // Snap to 1-2-5 sequence
            double time_division = snap_to_125(rough_division);

            // Determine the reference point (t=0) in pixels from left edge
            // If trigger is enabled and we have a valid position, use that as t=0
            // Otherwise, t=0 is at the left edge
            double t0_pixel = 0.0;
            if (trigger_enabled && trigger_display_pos >= 0 && trigger_display_pos < (int)width) {
                t0_pixel = (double)trigger_display_pos;
            }

            // Calculate the time offset at the left edge (will be negative if trigger is after left edge)
            double time_at_left = -t0_pixel * time_per_pixel;

            // Find the first grid line position (snap to division boundary)
            // We want the first t such that t >= time_at_left and t is a multiple of time_division
            double first_grid_time;
            if (time_at_left >= 0) {
                first_grid_time = ceil(time_at_left / time_division) * time_division;
            } else {
                first_grid_time = ceil(time_at_left / time_division) * time_division;
            }

            // Draw vertical grid lines at time intervals
            char time_buf[8];
            int division_count = 0;
            for (double t = first_grid_time; division_count < GRID_MAX_DIVISIONS; t += time_division) {
                // Convert time to pixel position
                double px = (t - time_at_left) / time_per_pixel;
                if (px >= (double)width) break;
                if (px < 0) continue;

                float gx = x + (float)px;

                // Draw grid line (use major color for t=0)
                bool is_zero = (fabs(t) < time_division * 0.01);
                DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height},
                         is_zero ? COLOR_GRID_MAJOR : COLOR_GRID);

                // Draw time label (skip if too close to edges)
                if (gx > x + 40 && gx < x + width - 40) {
                    if (is_zero) {
                        // Draw "0" for the trigger point
                        int label_w = gui_text_measure_mono("0", FONT_SIZE_OSC_SCALE);
                        gui_text_draw_mono("0", gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
                    } else {
                        format_time_label(time_buf, sizeof(time_buf), fabs(t));
                        int label_w = gui_text_measure_mono(time_buf, FONT_SIZE_OSC_SCALE);
                        gui_text_draw_mono(time_buf, gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
                    }
                }
                division_count++;
            }

            // Show time per division in top-left corner (below channel label)
            format_time_label(time_buf, sizeof(time_buf), time_division);
            char div_label[48];
            snprintf(div_label, sizeof(div_label), "%s/div", time_buf);
            gui_text_draw_mono(div_label, x + 8, y + 26, FONT_SIZE_OSC_DIV, COLOR_TEXT);
            if (time_div_rect) {
                *time_div_rect = (Rectangle){x + 8, y + 26,
                    gui_text_measure_mono(div_label, FONT_SIZE_OSC_DIV) + 1, FONT_SIZE_OSC_DIV};
            }
        } else {
            // Fallback: fixed divisions when no sample rate available
            const int fixed_divisions = 10;
            for (int i = 1; i < fixed_divisions; i++) {
                float gx = x + (width * i / fixed_divisions);
                DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height}, COLOR_GRID);
            }
        }

        // Horizontal grid lines (amplitude divisions)
        for (int i = 1; i < GRID_DIVISIONS_Y; i++) {
            float gy = y + (height * i / GRID_DIVISIONS_Y);
            DrawLineV((Vector2){x, gy}, (Vector2){x + width, gy}, COLOR_GRID);
        }
    }

    // Center line (0V reference) - always show
    DrawLineEx((Vector2){x, center_y}, (Vector2){x + width, center_y}, 1.0f, COLOR_GRID_MAJOR);

    // Border
    DrawRectangleLinesEx((Rectangle){x, y, width, height}, 1, COLOR_GRID_MAJOR);

    // Amplitude scale ticks on left side (use mono font for numbers)
    const char *tick_labels[] = { "+0.5", "0", "-0.5" };
    float tick_positions[] = { 0.25f, 0.5f, 0.75f };
    for (int i = 0; i < 3; i++) {
        float tick_y = y + height * tick_positions[i];
        // Tick mark
        DrawLineEx((Vector2){x, tick_y}, (Vector2){x + 4, tick_y}, 1.0f, COLOR_GRID_MAJOR);
        // Label (offset to not overlap with border)
        gui_text_draw_mono(tick_labels[i], x + 6, tick_y - 7, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
    }

    // Channel label in top-left corner
    gui_text_draw(label, x + 8, y + 4, FONT_SIZE_OSC_LABEL, channel_color);
}

//-----------------------------------------------------------------------------
// Trigger Marker Drawing (for panel state)
//-----------------------------------------------------------------------------

// Helper to draw a dashed horizontal line
static void draw_dashed_hline(float x, float y, float w, Color color, float dash_len, float gap_len) {
    for (float dx = 0; dx < w; dx += dash_len + gap_len) {
        float dash_end = dx + dash_len;
        if (dash_end > w) dash_end = w;
        DrawLineEx((Vector2){x + dx, y}, (Vector2){x + dash_end, y}, 1.0f, color);
    }
}

static void draw_panel_trigger_markers(float x, float y, float w, float h,
                                        const waveform_render_snapshot_t *snap,
                                        float amplitude_scale, Color color) {
    if (!snap || !snap->trigger_enabled) return;

    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * amplitude_scale;

    // CVBS mode: draw detected sync, blanking, and threshold levels
    if (snap->trigger_mode == TRIGGER_MODE_CVBS_HSYNC) {
        if (!snap->cvbs_levels_valid) return;

        // Colors for CVBS levels
        Color sync_color = { 255, 100, 100, 140 };    // Red-ish for sync tip
        Color blank_color = { 100, 255, 100, 140 };   // Green-ish for blanking
        Color thresh_color = { color.r, color.g, color.b, 180 };  // Channel color for threshold

        // Convert levels to Y positions (levels are -2048 to +2047)
        float sync_y = center_y - (snap->cvbs_sync_level / 2048.0f) * scale;
        float blank_y = center_y - (snap->cvbs_blank_level / 2048.0f) * scale;
        float thresh_y = center_y - (snap->cvbs_threshold / 2048.0f) * scale;

        // Clamp to bounds
        if (sync_y < y) sync_y = y;
        if (sync_y > y + h) sync_y = y + h;
        if (blank_y < y) blank_y = y;
        if (blank_y > y + h) blank_y = y + h;
        if (thresh_y < y) thresh_y = y;
        if (thresh_y > y + h) thresh_y = y + h;

        // Draw sync tip level (dotted)
        draw_dashed_hline(x, sync_y, w, sync_color, 4.0f, 4.0f);

        // Draw blanking level (dotted)
        draw_dashed_hline(x, blank_y, w, blank_color, 4.0f, 4.0f);

        // Draw threshold level (dashed, more prominent)
        draw_dashed_hline(x, thresh_y, w, thresh_color, 8.0f, 4.0f);

        // Draw labels on right edge
        gui_text_draw_mono("S", x + w - 12, sync_y - 6, FONT_SIZE_OSC_SCALE, sync_color);
        gui_text_draw_mono("B", x + w - 12, blank_y - 6, FONT_SIZE_OSC_SCALE, blank_color);
        gui_text_draw_mono("T", x + w - 12, thresh_y - 6, FONT_SIZE_OSC_SCALE, thresh_color);

        // Draw vertical trigger position marker at actual trigger position (if triggered)
        if (snap->trigger_display_pos >= 0 && snap->trigger_display_pos < (int)w) {
            float trigger_x = x + (float)snap->trigger_display_pos;
            Color marker_color = { color.r, color.g, color.b, 80 };
            DrawLineEx((Vector2){trigger_x, y}, (Vector2){trigger_x, y + h}, 1.0f, marker_color);
        }
        return;
    }

    // Sync mode: phase-lock marker only (no manual trigger-level line).
    if (snap->trigger_mode == TRIGGER_MODE_SYNC) {
        if (snap->trigger_display_pos >= 0 && snap->trigger_display_pos < (int)w) {
            float trigger_x = x + (float)snap->trigger_display_pos;
            Color marker_color = { color.r, color.g, color.b, 120 };
            DrawLineEx((Vector2){trigger_x, y}, (Vector2){trigger_x, y + h}, 1.0f, marker_color);
        }
        return;
    }

    // Normal trigger mode: draw single trigger level line
    float level_norm = snap->trigger_level / 2048.0f;
    float level_y = center_y - level_norm * scale;

    // Clamp to panel bounds
    if (level_y < y) level_y = y;
    if (level_y > y + h) level_y = y + h;

    // Draw dashed trigger level line (semi-transparent channel color)
    Color trig_color = { color.r, color.g, color.b, 128 };
    draw_dashed_hline(x, level_y, w, trig_color, 8.0f, 4.0f);

    // Draw small trigger arrow on the left edge
    float arrow_size = 6.0f;
    Vector2 arrow_tip = { x + 2, level_y };
    Vector2 arrow_top = { x + 2 + arrow_size, level_y - arrow_size/2 };
    Vector2 arrow_bot = { x + 2 + arrow_size, level_y + arrow_size/2 };
    DrawTriangle(arrow_tip, arrow_bot, arrow_top, trig_color);

    // Draw vertical trigger position marker at actual trigger position (if triggered)
    if (snap->trigger_display_pos >= 0 && snap->trigger_display_pos < (int)w) {
        float trigger_x = x + (float)snap->trigger_display_pos;

        Color marker_color = { color.r, color.g, color.b, 80 };
        DrawLineEx((Vector2){trigger_x, y}, (Vector2){trigger_x, y + h}, 1.0f, marker_color);

        // Draw small "T" marker at the trigger intersection
        Color t_marker_color = { color.r, color.g, color.b, 200 };
        DrawLineEx((Vector2){trigger_x - 4, level_y - 8}, (Vector2){trigger_x + 4, level_y - 8}, 2.0f, t_marker_color);
        DrawLineEx((Vector2){trigger_x, level_y - 8}, (Vector2){trigger_x, level_y - 2}, 2.0f, t_marker_color);
    }
}


//-----------------------------------------------------------------------------
// Waveform Panel Rendering (using panel state)
//-----------------------------------------------------------------------------

// Internal render for line mode using panel state
static void render_waveform_line_internal(waveform_panel_state_t *state,
                                           gui_app_t *app, int channel,
                                           Rectangle bounds, Color color) {
    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
    const char *label = (channel == 0) ? "CH A" : "CH B";
    waveform_render_snapshot_t snap = {0};
    waveform_sample_t render_samples[DISPLAY_BUFFER_SIZE];
    waveform_capture_render_snapshot(state, &snap, render_samples);

    // Update display width for processing thread
    int new_display_width = (int)w;
    if (new_display_width < 100) new_display_width = 100;
    if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
    atomic_store(&state->display_width, new_display_width);

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      snap.zoom_scale, sample_rate,
                      snap.trigger_enabled, snap.trigger_display_pos, &state->time_div_rect);

    // Draw trigger level and position markers
    draw_panel_trigger_markers(x, y, w, h, &snap, app->settings.amplitude_scale, color);
    if (snap.samples_available == 0) return;

    int display_width = (int)w;
    if (display_width > DISPLAY_BUFFER_SIZE) display_width = DISPLAY_BUFFER_SIZE;
    int samples_to_draw = (snap.samples_available < (size_t)display_width) ?
                          (int)snap.samples_available : display_width;

    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;

    // Draw waveform as connected line
    float prev_py = center_y;
    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + px;
        float py = center_y - render_samples[px].value * scale;

        // Clamp to bounds
        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, color);
        }
        prev_py = py;
    }
}

// Internal render for phosphor mode using panel state
static void render_waveform_phosphor_internal(waveform_panel_state_t *state,
                                               gui_app_t *app, int channel,
                                               Rectangle bounds, Color color) {
    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
    const char *label = (channel == 0) ? "CH A" : "CH B";
    waveform_render_snapshot_t snap = {0};
    waveform_sample_t render_samples[DISPLAY_BUFFER_SIZE];
    waveform_capture_render_snapshot(state, &snap, render_samples);

    // Update display width for processing thread
    int new_display_width = (int)w;
    if (new_display_width < 100) new_display_width = 100;
    if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
    atomic_store(&state->display_width, new_display_width);

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      snap.zoom_scale, sample_rate,
                      snap.trigger_enabled, snap.trigger_display_pos, &state->time_div_rect);

    // Draw trigger level and position markers
    draw_panel_trigger_markers(x, y, w, h, &snap, app->settings.amplitude_scale, color);
    if (snap.samples_available == 0) return;

    int buf_width = (int)w;
    int buf_height = (int)h;
    if (buf_width <= 0 || buf_height <= 0) return;

    int samples_to_draw = (snap.samples_available < (size_t)buf_width) ?
                          (int)snap.samples_available : buf_width;

    // Get phosphor state from panel
    phosphor_rt_t *prt = state->phosphor;
    if (prt) {
        // Initialize/resize phosphor if needed
        Vector2 render_scale = gui_ui_get_render_scale();
        phosphor_rt_init(prt, buf_width, buf_height,
                         render_scale.x, render_scale.y);

        // Update phosphor
        phosphor_rt_begin_frame(prt);
        phosphor_rt_draw_waveform(prt, render_samples, samples_to_draw, app->settings.amplitude_scale);
        phosphor_rt_end_frame(prt);

        // Render phosphor to screen
        if (state->phosphor_color == PHOSPHOR_COLOR_OPACITY) {
            phosphor_rt_render_opacity(prt, x, y);
        } else {
            phosphor_rt_render(prt, x, y, false);
        }
    }

    // Draw line overlay on phosphor (semi-transparent)
    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;
    float prev_py = center_y;
    Color waveform_color = {color.r, color.g, color.b, 200};

    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + px;
        float py = center_y - render_samples[px].value * scale;

        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, waveform_color);
        }
        prev_py = py;
    }
}

//-----------------------------------------------------------------------------
// Trigger Detection
//-----------------------------------------------------------------------------

ssize_t find_trigger_point_from(const int16_t *buf, size_t count,
                                 const channel_trigger_t *trig, size_t min_index) {
    return trigger_find_from_config(buf, count, trig, min_index);
}

ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig) {
    return trigger_find_from_config(buf, count, trig, 1);
}

//=============================================================================
// Panel Interface (vtable) Implementation
//=============================================================================

//-----------------------------------------------------------------------------
// Waveform Panel Resampler (per-panel libsoxr)
//-----------------------------------------------------------------------------

#if LIBSOXR_ENABLED
// Ensure resampler is initialized with correct decimation ratio
static soxr_t waveform_ensure_resampler(waveform_panel_state_t *state, float decimation) {
    // Check if we need to create or recreate the resampler
    float ratio_diff = fabsf(state->resampler_ratio - decimation);
    bool need_recreate = (state->resampler == NULL) ||
                         (ratio_diff > decimation * 0.001f);

    if (!need_recreate) {
        return (soxr_t)state->resampler;
    }

    // Destroy old resampler if exists
    if (state->resampler) {
        soxr_delete((soxr_t)state->resampler);
        state->resampler = NULL;
    }

    // Create new resampler for this decimation ratio
    const double in_rate = (double)decimation;
    const double out_rate = 1.0;

    soxr_error_t soxr_err = NULL;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t qual_spec = soxr_quality_spec(SOXR_QQ, 0);

    soxr_t resampler = soxr_create(in_rate, out_rate, 1, &soxr_err, &io_spec, &qual_spec, NULL);
    if (!resampler || soxr_err) {
        return NULL;
    }

    state->resampler = resampler;
    state->resampler_ratio = decimation;

    return resampler;
}
#endif

static void waveform_cleanup_resampler(waveform_panel_state_t *state) {
#if LIBSOXR_ENABLED
    if (state && state->resampler) {
        soxr_delete((soxr_t)state->resampler);
        state->resampler = NULL;
        state->resampler_ratio = 0.0f;
    }
#else
    (void)state;
#endif
}

//-----------------------------------------------------------------------------
// Waveform Panel Resampling (per-panel display buffer)
//-----------------------------------------------------------------------------

static size_t waveform_resample_to_buffer(waveform_panel_state_t *state,
                                           waveform_sample_t *dest,
                                           const int16_t *buf, size_t num_samples,
                                           size_t start_idx, float decimation,
                                           size_t target_width) {
    const float scale = 1.0f / 2048.0f;

    // Clamp target width to buffer size
    if (target_width > DISPLAY_BUFFER_SIZE) target_width = DISPLAY_BUFFER_SIZE;
    if (target_width == 0) target_width = DISPLAY_BUFFER_SIZE;

    // Calculate how many source samples we have available
    size_t available = (start_idx < num_samples) ? (num_samples - start_idx) : 0;
    if (available == 0) return 0;

    // Calculate display count based on available samples and decimation
    size_t display_count = (size_t)((float)available / decimation);
    if (display_count > target_width) display_count = target_width;
    if (display_count == 0) return 0;

    // Calculate how many source samples we actually need
    size_t source_samples_needed = (size_t)ceilf((float)display_count * decimation);
    if (source_samples_needed > available) source_samples_needed = available;


#if LIBSOXR_ENABLED
    // Bypass soxr for 1:1 ratio
    if (decimation >= 0.999f && decimation <= 1.001f) {
        size_t count = (display_count < source_samples_needed) ? display_count : source_samples_needed;
        for (size_t i = 0; i < count; i++) {
            dest[i].value = (float)buf[start_idx + i] * scale;
        }
        return count;
    }

    // Temporary buffers for float conversion
    static float temp_input[DISPLAY_BUFFER_SIZE * 256];
    static float temp_output[DISPLAY_BUFFER_SIZE];

    size_t max_input = sizeof(temp_input) / sizeof(temp_input[0]);
    if (source_samples_needed > max_input) {
        source_samples_needed = max_input;
        display_count = (size_t)((float)source_samples_needed / decimation);
        if (display_count == 0) display_count = 1;
    }

    // Convert input to float with scaling
    for (size_t i = 0; i < source_samples_needed; i++) {
        temp_input[i] = (float)buf[start_idx + i] * scale;
    }

    // Get or create resampler for this decimation ratio
    soxr_t resampler = waveform_ensure_resampler(state, decimation);
    if (!resampler) {
        return 0;
    }

    // Clear resampler state for fresh data each frame
    soxr_clear(resampler);

    // Process through resampler
    size_t in_done = 0, out_done = 0;
    soxr_error_t soxr_err = soxr_process(resampler,
                                          temp_input, source_samples_needed, &in_done,
                                          temp_output, display_count, &out_done);

    if (soxr_err || out_done == 0) {
        return 0;
    }

    // Copy to destination
    for (size_t i = 0; i < out_done; i++) {
        dest[i].value = temp_output[i];
    }

    return out_done;
#else
    // No libsoxr: simple point sampling
    for (size_t i = 0; i < display_count; i++) {
        size_t src_idx = start_idx + (size_t)((float)i * decimation);
        if (src_idx >= num_samples) src_idx = num_samples - 1;
        dest[i].value = (float)buf[src_idx] * scale;
    }
    return display_count;
#endif
}

//-----------------------------------------------------------------------------
// Waveform Panel Processing (per-panel trigger detection and resampling)
//-----------------------------------------------------------------------------

// Find trigger point using panel's trigger configuration
static ssize_t waveform_find_trigger_from(const int16_t *buf, size_t count,
                                           const waveform_runtime_cfg_t *cfg,
                                           size_t min_index,
                                           uint32_t sample_rate_hz) {
    if (!cfg || !cfg->trigger_enabled) return -1;
    if (count < 2 || min_index >= count) return -1;

    // Use the gui_trigger module for source-aware trigger detection.
    channel_trigger_t temp_trig = {
        .enabled = cfg->trigger_enabled,
        .level = cfg->trigger_level,
        .trigger_mode = cfg->trigger_mode,
        .trigger_source = cfg->trigger_source,
    };
    const int16_t *src_ch1 = s_trigger_src_ch1 ? s_trigger_src_ch1 : buf;
    const int16_t *src_ch2 = s_trigger_src_ch2 ? s_trigger_src_ch2 : buf;
    size_t src_count = count;
    if (s_trigger_src_count > 0 && s_trigger_src_count < src_count) {
        src_count = s_trigger_src_count;
    }
    if (min_index >= src_count) return -1;
    return trigger_find_from_config_multi(src_ch1, src_ch2, src_count,
                                          &temp_trig, min_index, sample_rate_hz);
}

// Process raw samples and update panel's display buffer
static bool waveform_process_display(waveform_panel_state_t *state,
                                      const waveform_runtime_cfg_t *cfg,
                                      const int16_t *buf, size_t num_samples,
                                      uint32_t sample_rate_hz,
                                      waveform_sample_t *out_samples,
                                      size_t *out_available,
                                      int *out_trigger_display_pos) {
    if (!state || !cfg || !buf || num_samples == 0 ||
        !out_samples || !out_available || !out_trigger_display_pos) return false;

    // Get display width (set by renderer, defaults to DISPLAY_BUFFER_SIZE)
    size_t display_width = (size_t)atomic_load(&state->display_width);
    if (display_width == 0 || display_width > DISPLAY_BUFFER_SIZE) {
        display_width = DISPLAY_BUFFER_SIZE;
    }

    // Get decimation factor from zoom_scale
    float decimation = cfg->zoom_scale;
    if (decimation < ZOOM_SCALE_MIN) decimation = ZOOM_SCALE_MIN;
    if (decimation > ZOOM_SCALE_MAX) decimation = ZOOM_SCALE_MAX;

    // How many raw samples we need for the full display at this zoom
    float display_window = (float)display_width * decimation;

    // If trigger is disabled, just show the start of the buffer
    if (!cfg->trigger_enabled) {
        *out_trigger_display_pos = -1;
        *out_available = waveform_resample_to_buffer(
            state, out_samples, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // When zoomed out so far that display_window >= 90% of buffer,
    // there's no room for trigger positioning - just show from start
    if (display_window >= (float)num_samples * 0.9f) {
        *out_trigger_display_pos = -1;
        *out_available = waveform_resample_to_buffer(
            state, out_samples, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Trigger point should appear at 10% across the display
    size_t trigger_display_pos = display_width / 10;
    float pre_trigger_raw_samples = (float)trigger_display_pos * decimation;
    float post_trigger_raw_samples = display_window - pre_trigger_raw_samples;

    // Calculate the valid search range for triggers
    size_t min_trig_pos = (size_t)pre_trigger_raw_samples;
    size_t max_trig_pos = num_samples - (size_t)post_trigger_raw_samples;

    // Check if there's a valid search range
    if (min_trig_pos >= max_trig_pos) {
        *out_trigger_display_pos = -1;
        *out_available = waveform_resample_to_buffer(
            state, out_samples, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Find trigger point starting from minimum valid position
    ssize_t trig_pos = waveform_find_trigger_from(buf, max_trig_pos, cfg,
                                                  min_trig_pos, sample_rate_hz);

    if (trig_pos < 0) {
        // No trigger found - hold previous display
        return false;
    }

    // Trigger found - place it at desired position
    size_t start_pos = (size_t)((float)trig_pos - pre_trigger_raw_samples);
    *out_trigger_display_pos = (int)trigger_display_pos;
    *out_available = waveform_resample_to_buffer(
        state, out_samples, buf, num_samples, start_pos, decimation, display_width);
    return true;
}

// Vtable process function (called from display thread via panel_process_all)
static void waveform_vtable_process(void *state_ptr, const int16_t *samples,
                                     size_t count, uint32_t sample_rate) {
    if (!state_ptr || !samples || count == 0) return;

    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    waveform_runtime_cfg_t cfg = waveform_capture_runtime_cfg(state);
    waveform_sample_t new_samples[DISPLAY_BUFFER_SIZE];
    size_t new_available = 0;
    int new_trigger_display_pos = -1;
    int16_t cvbs_sync = 0;
    int16_t cvbs_blank = 0;
    int16_t cvbs_threshold = 0;
    bool cvbs_valid = false;

    // Update CVBS levels when in CVBS trigger mode
    if (cfg.trigger_enabled && cfg.trigger_mode == TRIGGER_MODE_CVBS_HSYNC) {
        cvbs_levels_t levels;
        trigger_analyze_cvbs_levels(samples, count, &levels);
        if (levels.range >= 100) {
            cvbs_sync = levels.sig_min;
            cvbs_blank = levels.black_level;
            cvbs_threshold = levels.sync_threshold;
            cvbs_valid = true;
        }
    }
    bool updated = waveform_process_display(state,
                                            &cfg,
                                            samples,
                                            count,
                                            sample_rate,
                                            new_samples,
                                            &new_available,
                                            &new_trigger_display_pos);
    waveform_state_lock(state);
    state->cvbs_levels_valid = cvbs_valid;
    if (cvbs_valid) {
        state->cvbs_sync_level = cvbs_sync;
        state->cvbs_blank_level = cvbs_blank;
        state->cvbs_threshold = cvbs_threshold;
    }
    if (updated) {
        if (new_available > DISPLAY_BUFFER_SIZE) {
            new_available = DISPLAY_BUFFER_SIZE;
        }
        if (new_available > 0) {
            memcpy(state->display_samples,
                   new_samples,
                   new_available * sizeof(waveform_sample_t));
        }
        state->display_samples_available = new_available;
        state->trigger_display_pos = new_trigger_display_pos;
    }
    waveform_state_unlock(state);
}

//-----------------------------------------------------------------------------
// Waveform Line Panel Vtable
//-----------------------------------------------------------------------------

static void *waveform_create(void) {
    waveform_panel_state_t *state = calloc(1, sizeof(waveform_panel_state_t));
    if (!state) return NULL;

    // Default to phosphor mode
    state->render_mode = WAVEFORM_MODE_PHOSPHOR;

    // Initialize trigger settings with defaults
    state->trigger_enabled = false;
    state->trigger_level = 0;
    state->trigger_mode = TRIGGER_MODE_RISING;
    state->trigger_source = TRIGGER_SOURCE_CH1;
    state->zoom_scale = ZOOM_SCALE_DEFAULT;
    state->trigger_display_pos = -1;

    // Initialize display state
    atomic_init(&state->display_width, DISPLAY_BUFFER_SIZE);
    state->display_samples_available = 0;

    // Resampler initialized on demand
    state->resampler = NULL;
    state->resampler_ratio = 0.0f;

    // Allocate phosphor state (used when in phosphor mode)
    state->phosphor = calloc(1, sizeof(phosphor_rt_t));
    if (!state->phosphor) {
        free(state);
        return NULL;
    }
    state->phosphor_color = PHOSPHOR_COLOR_HEATMAP;

    // UI state
    state->render_mode_dropdown_open = false;
    state->trigger_dropdown_open = false;
    state->dragging = false;
    atomic_flag_clear(&state->data_lock);

    state->initialized = true;
    state->trigger_source_defaulted_by_channel = false;
    return state;
}

static void waveform_destroy(void *state_ptr) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    waveform_cleanup_resampler(state);

    // Clean up phosphor
    if (state->phosphor) {
        phosphor_rt_cleanup(state->phosphor);
        free(state->phosphor);
        state->phosphor = NULL;
    }

    free(state);
}

static void waveform_clear(void *state_ptr) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    waveform_state_lock(state);

    state->display_samples_available = 0;
    state->trigger_display_pos = -1;
    waveform_state_unlock(state);

    // Clear phosphor persistence if in phosphor mode
    if (state->phosphor) {
        phosphor_rt_clear(state->phosphor);
    }
}

//-----------------------------------------------------------------------------
// Waveform Panel Overlay (Render Mode + Trigger Mode Dropdowns)
//-----------------------------------------------------------------------------

// Helper to draw a dropdown button with label
static void draw_dropdown_button(Rectangle btn, const char *label, bool is_open) {
    Color btn_bg = is_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(btn, 0.15f, 4, btn_bg);

    int text_w = gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT);
    float arrow_w = 8;
    float total_w = text_w + arrow_w + 4;
    float text_x = btn.x + (btn.width - total_w) / 2;
    float text_y = btn.y + (btn.height - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(label, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Draw arrow
    float arrow_size = 5.0f;
    float arrow_x = text_x + text_w + 6;
    float arrow_cy = btn.y + btn.height / 2;
    if (is_open) {
        Vector2 top = { arrow_x + arrow_size/2, arrow_cy - arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy + arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy + arrow_size/2 };
        DrawTriangle(top, left, right, COLOR_TEXT);
    } else {
        Vector2 bottom = { arrow_x + arrow_size/2, arrow_cy + arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy - arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy - arrow_size/2 };
        DrawTriangle(bottom, right, left, COLOR_TEXT);
    }
}

static void waveform_render_overlay(void *state_ptr, Rectangle bounds) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    if (!state->initialized) return;

    float btn_h = 18;
    float render_btn_y = bounds.y + 8;
    float trig_btn_y = render_btn_y;
    Vector2 mouse = gui_ui_get_mouse_position();

    // Reserve the widest label plus arrow/padding, not just the current value.
    // Both dropdowns keep the same width when the selection changes.
    float button_width = 98;
    for (int i = 0; i < WAVEFORM_MODE_COUNT; i++) {
        button_width = fmaxf(button_width,
            gui_text_measure(s_render_mode_labels[i], FONT_SIZE_DROPDOWN_OPT) + 24);
    }
    for (int i = 0; i < TRIGGER_MODE_COUNT; i++) {
        button_width = fmaxf(button_width,
            gui_text_measure(s_trigger_mode_labels[i], FONT_SIZE_DROPDOWN_OPT) + 12);
        for (int source = 0; source < TRIGGER_SOURCE_COUNT; source++) {
            char label[24];
            snprintf(label, sizeof(label), "%s/%s",
                     s_trigger_mode_short_labels[i], s_trigger_source_labels[source]);
            button_width = fmaxf(button_width,
                gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT) + 24);
        }
    }
    float trig_btn_w = button_width;
    float render_btn_w = button_width;

    const char *trig_prefix = "Trig:";
    const char *mode_prefix = "Mode:";
    int trig_prefix_w = gui_text_measure(trig_prefix, FONT_SIZE_DROPDOWN_OPT);
    int mode_prefix_w = gui_text_measure(mode_prefix, FONT_SIZE_DROPDOWN_OPT);
    float trig_btn_x = bounds.x + bounds.width - trig_btn_w - 8;
    float render_btn_x = trig_btn_x - trig_prefix_w - 8 - render_btn_w - 8;
    float mode_label_x = render_btn_x - mode_prefix_w - 4;
    float trig_label_x = trig_btn_x - trig_prefix_w - 4;
    float text_offset_y = (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    float mode_label_y = render_btn_y + text_offset_y;
    float trig_label_y = trig_btn_y + text_offset_y;
    float channel_label_w = fmaxf(gui_text_measure("CH A", FONT_SIZE_OSC_LABEL),
                                 gui_text_measure("CH B", FONT_SIZE_OSC_LABEL));

    // Wrap into aligned label/button columns. Only avoid the actual time/div
    // label, not an empty strip across the full width of the waveform.
    if (mode_label_x < bounds.x + 8 + channel_label_w + 8) {
        float row_gap = 2;
        float label_width = fmaxf(mode_prefix_w, trig_prefix_w);
        render_btn_x = bounds.x + bounds.width - render_btn_w - 8;
        mode_label_x = render_btn_x - label_width - 4;
        trig_label_x = mode_label_x;
        if (mode_label_x < bounds.x + 8 + channel_label_w + 8) {
            render_btn_y = bounds.y + 4 + FONT_SIZE_OSC_LABEL + row_gap;
        }
        bool labels_above = mode_label_x < bounds.x + 8;
        float row_x = labels_above ? bounds.x + 8 : mode_label_x;
        float row_h = labels_above ? FONT_SIZE_DROPDOWN_OPT + row_gap + btn_h : btn_h;
        Rectangle row = {row_x, render_btn_y, bounds.x + bounds.width - 8 - row_x, row_h};
        if (state->time_div_rect.width > 0 && CheckCollisionRecs(row, state->time_div_rect)) {
            render_btn_y = state->time_div_rect.y + state->time_div_rect.height + row_gap;
        }
        mode_label_y = render_btn_y + text_offset_y;
        if (labels_above) {
            // Very narrow panels put the label above its dropdown as well.
            mode_label_x = bounds.x + 8;
            mode_label_y = render_btn_y;
            render_btn_y += FONT_SIZE_DROPDOWN_OPT + row_gap;
        }
        trig_btn_y = render_btn_y + btn_h + row_gap;
        row.y = trig_btn_y;
        if (state->time_div_rect.width > 0 && CheckCollisionRecs(row, state->time_div_rect)) {
            trig_btn_y = state->time_div_rect.y + state->time_div_rect.height + row_gap;
        }
        trig_label_y = trig_btn_y + text_offset_y;
        if (labels_above) {
            trig_label_x = bounds.x + 8;
            trig_label_y = trig_btn_y;
            trig_btn_y += FONT_SIZE_DROPDOWN_OPT + row_gap;
        }
    }

    //-------------------------------------------------------------------------
    // Trigger Mode Dropdown (top-right, rightmost)
    //-------------------------------------------------------------------------
    state->trigger_btn_rect = (Rectangle){trig_btn_x, trig_btn_y, trig_btn_w, btn_h};

    // Draw "Trig:" label
    gui_text_draw(trig_prefix, trig_label_x, trig_label_y,
                  FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);

    char trig_label_buf[24];
    const char *trig_label = "Off";
    if (state->trigger_enabled) {
        snprintf(trig_label_buf, sizeof(trig_label_buf), "%s/%s",
                 s_trigger_mode_short_labels[state->trigger_mode],
                 s_trigger_source_labels[state->trigger_source]);
        trig_label = trig_label_buf;
    }
    draw_dropdown_button(state->trigger_btn_rect, trig_label, state->trigger_dropdown_open);

    // Draw both controls before either popup so a wrapped button cannot cover
    // an open menu. Mode is above trigger when the controls use separate rows.
    state->render_mode_btn_rect = (Rectangle){render_btn_x, render_btn_y, render_btn_w, btn_h};
    gui_text_draw(mode_prefix, mode_label_x, mode_label_y,
                  FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);
    const char *render_label = s_render_mode_labels[state->render_mode];
    draw_dropdown_button(state->render_mode_btn_rect, render_label, state->render_mode_dropdown_open);

    // Draw trigger dropdown options if open
    if (state->trigger_dropdown_open) {
        float opt_h = 20;
        int mode_rows = TRIGGER_MODE_COUNT + 1;  // +1 for \"Off\"
        int header_rows = 1;                     // \"Channel\" section header
        int source_rows = TRIGGER_SOURCE_COUNT;
        int total_rows = mode_rows + header_rows + source_rows;
        // Shift the popup up when wrapping leaves too little space below.
        // Panel dispatch only accepts clicks inside these same bounds.
        float opt_y = fmaxf(bounds.y, fminf(trig_btn_y + btn_h,
                            bounds.y + bounds.height - opt_h * total_rows - 4));

        DrawRectangleRounded((Rectangle){trig_btn_x, opt_y, trig_btn_w, opt_h * total_rows},
                             0.1f, 4, COLOR_PANEL_BG);

        // Option 0: Off
        Rectangle off_rect = {trig_btn_x, opt_y, trig_btn_w, opt_h};
        state->trigger_mode_opts_rect[0] = off_rect;

        bool is_off = !state->trigger_enabled;
        bool hover_off = CheckCollisionPointRec(mouse, off_rect);
        DrawRectangleRec(off_rect, gui_dropdown_option_color(is_off, hover_off));

        const char *off_label = "Off";
        int off_w = gui_text_measure(off_label, FONT_SIZE_DROPDOWN_OPT);
        gui_text_draw(off_label, trig_btn_x + trig_btn_w/2 - off_w/2,
                      opt_y + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

        // Options 1..N: Trigger modes
        for (int i = 0; i < TRIGGER_MODE_COUNT; i++) {
            Rectangle opt_rect = {trig_btn_x, opt_y + (i + 1) * opt_h, trig_btn_w, opt_h};
            state->trigger_mode_opts_rect[i + 1] = opt_rect;

            bool is_selected = state->trigger_enabled && (state->trigger_mode == (trigger_mode_t)i);
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            DrawRectangleRec(opt_rect, gui_dropdown_option_color(is_selected, hover));

            const char *opt_label = s_trigger_mode_labels[i];
            int opt_w = gui_text_measure(opt_label, FONT_SIZE_DROPDOWN_OPT);
            gui_text_draw(opt_label, trig_btn_x + trig_btn_w/2 - opt_w/2,
                          opt_y + (i + 1) * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2,
                          FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }

        // Section header: Channel
        int channel_header_row = mode_rows;
        float header_y = opt_y + (channel_header_row * opt_h);
        DrawRectangleRec((Rectangle){trig_btn_x, header_y, trig_btn_w, opt_h}, COLOR_PANEL_BG);
        const char *header = "Channel";
        int header_w = gui_text_measure(header, FONT_SIZE_DROPDOWN_OPT);
        gui_text_draw(header, trig_btn_x + trig_btn_w/2 - header_w/2,
                      header_y + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);

        // Source options: CH1 / CH2 / CH3
        for (int i = 0; i < TRIGGER_SOURCE_COUNT; i++) {
            int row = mode_rows + 1 + i;
            Rectangle src_rect = {trig_btn_x, opt_y + row * opt_h, trig_btn_w, opt_h};
            state->trigger_source_opts_rect[i] = src_rect;

            bool is_selected = (state->trigger_source == (trigger_source_t)i);
            bool hover = CheckCollisionPointRec(mouse, src_rect);
            DrawRectangleRec(src_rect, gui_dropdown_option_color(is_selected, hover));

            const char *src_label = s_trigger_source_labels[i];
            int src_w = gui_text_measure(src_label, FONT_SIZE_DROPDOWN_OPT);
            gui_text_draw(src_label, trig_btn_x + trig_btn_w/2 - src_w/2,
                          src_rect.y + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2,
                          FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }

    // Draw render mode dropdown options if open
    if (state->render_mode_dropdown_open) {
        float opt_h = 20;
        float opt_y = fmaxf(bounds.y, fminf(render_btn_y + btn_h,
                            bounds.y + bounds.height - opt_h * WAVEFORM_MODE_COUNT - 4));

        DrawRectangleRounded((Rectangle){render_btn_x, opt_y, render_btn_w, opt_h * WAVEFORM_MODE_COUNT},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < WAVEFORM_MODE_COUNT; i++) {
            Rectangle opt_rect = {render_btn_x, opt_y + i * opt_h, render_btn_w, opt_h};
            state->render_mode_opts_rect[i] = opt_rect;

            bool is_selected = (state->render_mode == (waveform_render_mode_t)i);
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            const char *opt_label = s_render_mode_labels[i];
            int opt_text_w = gui_text_measure(opt_label, FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = render_btn_x + render_btn_w/2 - opt_text_w/2;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
            gui_text_draw(opt_label, opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

//-----------------------------------------------------------------------------
// Waveform Panel Click Handler (with dropdown support)
//-----------------------------------------------------------------------------

static bool waveform_panel_handle_click(void *state_ptr, struct gui_app *app, int channel,
                                         Vector2 click, Rectangle bounds) {

    if (!state_ptr || !app) return false;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    waveform_apply_channel_trigger_source_default(state, channel);

    // A popup can overlap either button after wrapping or shifting upward.
    // Its visible rows (including the Channel header) take click priority.
    Rectangle render_menu = state->render_mode_opts_rect[0];
    render_menu.height = WAVEFORM_MODE_COUNT * 20;
    Rectangle trigger_menu = state->trigger_mode_opts_rect[0];
    trigger_menu.height = (TRIGGER_MODE_COUNT + 2 + TRIGGER_SOURCE_COUNT) * 20;
    bool click_on_menu =
        (state->render_mode_dropdown_open && CheckCollisionPointRec(click, render_menu)) ||
        (state->trigger_dropdown_open && CheckCollisionPointRec(click, trigger_menu));

    //-------------------------------------------------------------------------
    // Render Mode Dropdown
    //-------------------------------------------------------------------------
    if (!click_on_menu && CheckCollisionPointRec(click, state->render_mode_btn_rect)) {
        state->render_mode_dropdown_open = !state->render_mode_dropdown_open;
        state->trigger_dropdown_open = false;  // Close other dropdown
        return true;
    }

    if (state->render_mode_dropdown_open) {
        for (int i = 0; i < WAVEFORM_MODE_COUNT; i++) {
            Rectangle opt_rect = state->render_mode_opts_rect[i];
            if (CheckCollisionPointRec(click, opt_rect)) {
                state->render_mode = (waveform_render_mode_t)i;
                state->render_mode_dropdown_open = false;
                return true;
            }
        }
        // Click outside closes dropdown
        state->render_mode_dropdown_open = false;
        return true;
    }

    //-------------------------------------------------------------------------
    // Trigger Mode Dropdown
    //-------------------------------------------------------------------------
    if (!click_on_menu && CheckCollisionPointRec(click, state->trigger_btn_rect)) {
        state->trigger_dropdown_open = !state->trigger_dropdown_open;
        state->render_mode_dropdown_open = false;  // Close other dropdown
        return true;
    }

    if (state->trigger_dropdown_open) {
        // Check \"Off\" option
        Rectangle off_rect = state->trigger_mode_opts_rect[0];
        if (CheckCollisionPointRec(click, off_rect)) {
            waveform_state_lock(state);
            state->trigger_enabled = false;
            waveform_state_unlock(state);
            state->trigger_dropdown_open = false;
            return true;
        }

        // Check trigger mode options
        for (int i = 0; i < TRIGGER_MODE_COUNT; i++) {
            Rectangle opt_rect = state->trigger_mode_opts_rect[i + 1];
            if (CheckCollisionPointRec(click, opt_rect)) {
                waveform_state_lock(state);
                bool was_enabled = state->trigger_enabled;
                state->trigger_enabled = true;
                if (!was_enabled) {
                    state->trigger_source = waveform_default_trigger_source_for_channel(channel);
                }
                state->trigger_mode = (trigger_mode_t)i;
                waveform_state_unlock(state);
                state->trigger_dropdown_open = false;
                return true;
            }
        }

        // Check trigger source options
        for (int i = 0; i < TRIGGER_SOURCE_COUNT; i++) {
            Rectangle src_rect = state->trigger_source_opts_rect[i];
            if (CheckCollisionPointRec(click, src_rect)) {
                waveform_state_lock(state);
                state->trigger_source = (trigger_source_t)i;
                waveform_state_unlock(state);
                state->trigger_dropdown_open = false;
                return true;
            }
        }

        // Click outside closes dropdown
        state->trigger_dropdown_open = false;
        return true;
    }

    //-------------------------------------------------------------------------
    // Trigger Level Drag
    //-------------------------------------------------------------------------
    if (!CheckCollisionPointRec(click, bounds)) {
        return false;
    }

    // Trigger level drag adjusts an already-enabled trigger only.
    if (!state->trigger_enabled) {
        return false;
    }

    // Don't allow trigger level drag in CVBS/Sync modes (auto trigger)
    // or CH3 headswitch mode (phase predictor ignores manual level).
    if (state->trigger_mode == TRIGGER_MODE_CVBS_HSYNC ||
        state->trigger_mode == TRIGGER_MODE_SYNC ||
        state->trigger_source == TRIGGER_SOURCE_CH3) {
        return false;
    }

    // Start dragging on mouse press
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        waveform_state_lock(state);
        state->dragging = true;
        waveform_state_unlock(state);

        // Calculate trigger level from click position
        float center_y = bounds.y + bounds.height / 2.0f;
        float half_height = (bounds.height / 2.0f) * app->settings.amplitude_scale;

        float level_norm = (center_y - click.y) / half_height;
        if (level_norm > 1.0f) level_norm = 1.0f;
        if (level_norm < -1.0f) level_norm = -1.0f;

        waveform_state_lock(state);
        state->trigger_level = (int16_t)(level_norm * 2047.0f);
        waveform_state_unlock(state);

        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// Waveform Panel Drag Update (called during render for continuous drag)
//-----------------------------------------------------------------------------

static void waveform_panel_update_drag(waveform_panel_state_t *state, gui_app_t *app, Rectangle bounds) {
    if (!state || !app) return;
    waveform_state_lock(state);
    bool dragging = state->dragging;
    waveform_state_unlock(state);
    if (!dragging) return;

    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        Vector2 mouse = gui_ui_get_mouse_position();

        // Calculate trigger level from mouse position
        float center_y = bounds.y + bounds.height / 2.0f;
        float half_height = (bounds.height / 2.0f) * app->settings.amplitude_scale;

        float level_norm = (center_y - mouse.y) / half_height;
        if (level_norm > 1.0f) level_norm = 1.0f;
        if (level_norm < -1.0f) level_norm = -1.0f;
        waveform_state_lock(state);

        state->trigger_level = (int16_t)(level_norm * 2047.0f);
        waveform_state_unlock(state);
    } else {
        // Mouse released - stop dragging
        waveform_state_lock(state);
        state->dragging = false;
        waveform_state_unlock(state);
    }
}

//-----------------------------------------------------------------------------
// Waveform Panel Scroll Handler (per-panel zoom)
//-----------------------------------------------------------------------------

static bool waveform_panel_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
    if (!state_ptr || delta == 0.0f) return false;

    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    Vector2 mouse = gui_ui_get_mouse_position();
    if (!CheckCollisionPointRec(mouse, bounds)) return false;

    // Smooth zoom: multiply/divide by a factor for each scroll step
    const float zoom_factor = 1.10f;

    if (delta > 0.0f) {
        // Scroll up = zoom in (fewer samples per pixel)
        waveform_state_lock(state);
        state->zoom_scale /= zoom_factor;
        if (state->zoom_scale < ZOOM_SCALE_MIN) {
            state->zoom_scale = ZOOM_SCALE_MIN;
        }
        waveform_state_unlock(state);
    } else {
        // Scroll down = zoom out (more samples per pixel)
        waveform_state_lock(state);
        state->zoom_scale *= zoom_factor;
        if (state->zoom_scale > ZOOM_SCALE_MAX) {
            state->zoom_scale = ZOOM_SCALE_MAX;
        }
        waveform_state_unlock(state);
    }

    return true;
}

static void waveform_render(void *state_ptr, gui_app_t *app, int channel,
                             Rectangle bounds, Color channel_color) {
    if (!app || !state_ptr) return;

    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    if (!state->initialized) return;
    waveform_apply_channel_trigger_source_default(state, channel);

    // Update drag state for trigger level (continuous while mouse is held)
    waveform_panel_update_drag(state, app, bounds);


    // Dispatch based on render mode
    if (state->render_mode == WAVEFORM_MODE_PHOSPHOR) {
        render_waveform_phosphor_internal(state, app, channel, bounds, channel_color);
    } else {
        render_waveform_line_internal(state, app, channel, bounds, channel_color);
    }
}

//-----------------------------------------------------------------------------
// Unified Waveform Panel Vtable
//-----------------------------------------------------------------------------

static const panel_vtable_t s_waveform_vtable = {
    .name = "Waveform",
    .create = waveform_create,
    .destroy = waveform_destroy,
    .clear = waveform_clear,
    .process = waveform_vtable_process,
    .render = waveform_render,
    .render_overlay = waveform_render_overlay,
    .handle_click = waveform_panel_handle_click,
    .handle_scroll = waveform_panel_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_waveform_panel_register(void) {
    panel_register(PANEL_VIEW_WAVEFORM, &s_waveform_vtable);
}
