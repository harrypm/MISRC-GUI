/*
 * MISRC GUI - Waveform Panel Implementation
 *
 * Waveform panel vtable (line and phosphor modes), trigger detection,
 * per-panel resampling, and grid rendering.
 */

#include "gui_oscilloscope.h"
#include "../../common/threading.h"
#include "gui_phosphor_rt.h"
#include "gui_fft.h"
#include "../signal/gui_trigger.h"
#include "../signal/gui_testgen.h"  // Gen row: format names/clamps
#include "../ui/gui_popup.h"
#include "gui_text.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_ui_scale.h"
#include "gui_panel.h"
#include "../ui/gui_dropdown.h"
#include "../ui/gui_chrome_hide.h"  // double-click overlay toggle (shared helper)
#include "../input/gui_capture.h"  // gui_app_level_autostop_vpp for mV scale
#include "../input/gui_playback.h" // DAW-style pause: re-process on config change
#include "../signal/gui_cvbs.h"    // gui_video_follow_generator_system (Gen box)
#include "../../common/misrc_debug.h"  // gated [WFDISP] display-value diagnostics
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

// Per-panel signal type, assigned through the Signal dropdown on the
// generator row. Drives the channel label (e.g. "CH1 CVBS", "CH2 FM RF")
// and the trigger defaults for that panel. Matches the Test Gen channel
// mapping: CH1 = composite CVBS, CH2 = tape RF.
// WAVE_SIGNAL_RAW is only the internal pre-default state of a freshly
// created panel (replaced by the channel default at first render/click);
// it is NOT selectable in the dropdown (user request 2026-11-03).
typedef enum {
    WAVE_SIGNAL_RAW = 0,   // Pre-default internal state (never user-selected)
    WAVE_SIGNAL_CVBS,      // Composite video (CVBS H-sync trigger default)
    WAVE_SIGNAL_FM_RF,     // Tape FM RF (rising-edge trigger default)
    WAVE_SIGNAL_AUDIO,     // Audio-band signal (rising-edge trigger default)
    WAVE_SIGNAL_COUNT
} waveform_signal_type_t;

// Number of SELECTABLE signal options (everything except RAW).
#define WAVE_SIGNAL_OPTION_COUNT (WAVE_SIGNAL_COUNT - 1)
// Map a displayed option index (0..WAVE_SIGNAL_OPTION_COUNT-1) to its
// signal type value: the dropdown lists CVBS / FM RF / Audio.
#define WAVE_SIGNAL_OPTION_VALUE(i) ((waveform_signal_type_t)((i) + 1))

static const char *s_signal_labels[WAVE_SIGNAL_COUNT] = {
    [WAVE_SIGNAL_RAW] = "Raw",
    [WAVE_SIGNAL_CVBS] = "CVBS",
    [WAVE_SIGNAL_FM_RF] = "FM RF",
    [WAVE_SIGNAL_AUDIO] = "Audio",
};

//-----------------------------------------------------------------------------
// Waveform render-resolution presets (playback device only). Res selects
// how many waveform points are drawn across the panel width; Range
// selects which preset group the Res dropdown lists (coarse vs fine
// sampling densities). This is the FIRST implementation's model (user
// confirmed 2026-11-03: "Coarse/Fine mode first implementation" was the
// working one - selecting MORE points than panel pixels oversamples the
// window with soxr and the stretch renderer draws them all).
//-----------------------------------------------------------------------------

typedef enum {
    WAVEFORM_RES_RANGE_COARSE = 0,
    WAVEFORM_RES_RANGE_FINE,
    WAVEFORM_RES_RANGE_COUNT
} waveform_res_range_t;

typedef enum {
    WAVEFORM_RES_AUTO = 0,      // One point per pixel (stock behaviour)
    WAVEFORM_RES_PTS_64,
    WAVEFORM_RES_PTS_128,
    WAVEFORM_RES_PTS_256,
    WAVEFORM_RES_PTS_512,
    WAVEFORM_RES_PTS_1024,
    WAVEFORM_RES_PTS_2048,
    WAVEFORM_RES_PTS_4096,
    WAVEFORM_RES_PTS_8192,
    WAVEFORM_RES_PTS_16384,
    WAVEFORM_RES_PTS_32768,
    WAVEFORM_RES_COUNT
} waveform_res_points_t;

// Coarse range: chunky resolutions. Fine range: the full ladder.
static const waveform_res_points_t s_res_coarse[] = {
    WAVEFORM_RES_PTS_256,
    WAVEFORM_RES_PTS_512,
    WAVEFORM_RES_PTS_1024,
    WAVEFORM_RES_PTS_2048,
    WAVEFORM_RES_PTS_4096,
};
static const waveform_res_points_t s_res_fine[] = {
    WAVEFORM_RES_PTS_64,
    WAVEFORM_RES_PTS_128,
    WAVEFORM_RES_PTS_256,
    WAVEFORM_RES_PTS_512,
    WAVEFORM_RES_PTS_1024,
    WAVEFORM_RES_PTS_2048,
    WAVEFORM_RES_PTS_4096,
    WAVEFORM_RES_PTS_8192,
    WAVEFORM_RES_PTS_16384,
    WAVEFORM_RES_PTS_32768,
};

// Dropdown option array size: the largest preset group.
#define WAVEFORM_RES_OPTION_COUNT (sizeof(s_res_fine) / sizeof(s_res_fine[0]))

static const char *s_res_range_labels[WAVEFORM_RES_RANGE_COUNT] = {
    [WAVEFORM_RES_RANGE_COARSE] = "Coarse",
    [WAVEFORM_RES_RANGE_FINE] = "Fine",
};

// Human label for a Res enum value ("Auto", "64", ..., "32768").
static const char *waveform_res_points_label(int res_points) {
    switch ((waveform_res_points_t)res_points) {
        case WAVEFORM_RES_AUTO:  return "Auto";
        case WAVEFORM_RES_PTS_64:    return "64";
        case WAVEFORM_RES_PTS_128:   return "128";
        case WAVEFORM_RES_PTS_256:   return "256";
        case WAVEFORM_RES_PTS_512:   return "512";
        case WAVEFORM_RES_PTS_1024:  return "1024";
        case WAVEFORM_RES_PTS_2048:  return "2048";
        case WAVEFORM_RES_PTS_4096:  return "4096";
        case WAVEFORM_RES_PTS_8192:  return "8192";
        case WAVEFORM_RES_PTS_16384: return "16384";
        case WAVEFORM_RES_PTS_32768: return "32768";
        default:                     return "Auto";
    }
}

// Enum value -> actual point count (0 = Auto -> caller uses display width).
static int waveform_res_points_value(int res_points) {
    switch ((waveform_res_points_t)res_points) {
        case WAVEFORM_RES_PTS_64:    return 64;
        case WAVEFORM_RES_PTS_128:   return 128;
        case WAVEFORM_RES_PTS_256:   return 256;
        case WAVEFORM_RES_PTS_512:   return 512;
        case WAVEFORM_RES_PTS_1024:  return 1024;
        case WAVEFORM_RES_PTS_2048:  return 2048;
        case WAVEFORM_RES_PTS_4096:  return 4096;
        case WAVEFORM_RES_PTS_8192:  return 8192;
        case WAVEFORM_RES_PTS_16384: return 16384;
        case WAVEFORM_RES_PTS_32768: return 32768;
        case WAVEFORM_RES_AUTO:
        default:                     return 0;
    }
}

// Clamp a Res value into the active range group (a Fine-only pick like
// 8192 falls back to the group's largest preset when the group is Coarse).
static int waveform_res_clamp_to_range(int res_points, int range) {
    const waveform_res_points_t *group = (range == WAVEFORM_RES_RANGE_FINE)
        ? s_res_fine : s_res_coarse;
    size_t n = (range == WAVEFORM_RES_RANGE_FINE)
        ? sizeof(s_res_fine) / sizeof(s_res_fine[0])
        : sizeof(s_res_coarse) / sizeof(s_res_coarse[0]);
    for (size_t i = 0; i < n; i++) {
        if ((int)group[i] == res_points) return res_points;
    }
    return (int)group[n - 1];
}

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

    // Amplitude scale dropdown (left side, beside CH A/CH B). Options:
    // Level (0.X, thinned), Basic (0.X, majors only), Full (0.X, all),
    // mV w/2Vpp, mV w/1Vpp. Mirrors the render-mode dropdown machinery.
    // scale_mode mirrors app->settings.waveform_scale_mode (synced in
    // waveform_render, which has app) so render_overlay (no app) can read it.
    int scale_mode;
    Rectangle scale_btn_rect;
    Rectangle scale_opts_rect[5];
    bool scale_dropdown_open;

    // Mirror of THIS channel's display-container full scale (2048 for the
    // common 12-bit container - 8-bit/12-bit/16-bit-shift4 playback all
    // land there - 4096 for a direct-domain 16-bit playback channel, 512
    // DdD), synced from app in waveform_render (which has app + channel).
    // NOT all sources are 12-bit; consumers must not hardcode 2048 (user
    // report 2026-11-03: "not all samples will be 12-bit").
    uint32_t display_full_scale;

    // Render-resolution selector (playback device only): Res = a DECIMATION
    // scale (0 = None -> full resolution, one point per pixel, the stock
    // behaviour); Range = which preset group the Res dropdown lists.
    int res_range;              // waveform_res_range_t value
    int res_points;             // waveform_res_points_t value (0 = Auto)
    Rectangle range_btn_rect;
    Rectangle range_opts_rect[WAVEFORM_RES_RANGE_COUNT];
    bool range_dropdown_open;
    Rectangle res_btn_rect;
    // +1 slot: row 0 is Auto and rows 1..N the active group's presets
    // (WAVEFORM_RES_OPTION_COUNT is the group size only). Sizing this to the
    // group alone made the render path's res_opts_rect[N] write run 16 bytes
    // out of bounds onto res_dropdown_open/signal state while the FINE group
    // was open — the dropdown closed itself every frame and could not be
    // selected from (user report 2026-11-03).
    Rectangle res_opts_rect[WAVEFORM_RES_OPTION_COUNT + 1];
    bool res_dropdown_open;

    // Signal type dropdown (leftmost of the right-anchored overlay group).
    // Raw keeps the legacy "CH A"/"CH B" label; CVBS / FM RF / Audio
    // relabel to "CH1 CVBS" / "CH2 FM RF" / "CHn Audio" and apply the
    // matching trigger defaults.
    waveform_signal_type_t signal_type;
    Rectangle signal_btn_rect;
    Rectangle signal_opts_rect[WAVE_SIGNAL_COUNT];
    bool signal_dropdown_open;
    // True once the user picked this panel's signal type from the dropdown
    // (test mode only UI). Distinguishes a USER selection from the
    // generator-applied default so leaving test mode can clear the stock
    // trigger config without ever overwriting a user pick.
    bool signal_user_picked;

    // Double-click chrome-hide tracker (see ui/gui_chrome_hide.h): toggles
    // ALL overlay chrome (Scale/Mode/Trig dropdowns + labels); the grid,
    // channel label, time/div and waveform keep rendering.
    gui_chrome_hide_t chrome_hide;

    // Simulated-device system selector button (visible only when the
    // Simulated device is selected; cycles PAL -> NTSC -> SECAM, plus the
    // MESECAM step on FM RF signal panels - MESECAM is RF-only, so CVBS /
    // Audio panels never offer or display it).
    Rectangle sim_sys_btn_rect;

    // Generator tape-format selector button (test mode only, FM RF signal
    // panels only — a tape format parameterizes the RF modulation, so it
    // has no meaning in a CVBS/Audio context and the button is absent
    // there; cycles VHS -> S-VHS -> Betamax -> Video8 -> Hi8). Zeroed
    // (inert) whenever it is not rendered.
    Rectangle sim_fmt_btn_rect;

    panel_menu_item_t menu_items[TRIGGER_MODE_COUNT];

    // Drag state for trigger level
    bool dragging;

    // Middle-button double-click tracker (350 ms / 6 px, same thresholds as
    // the chrome-hide double-click): two middle clicks on the panel reset
    // the waveform to the stock position (default zoom + centered trigger
    // level). User request 2026-11-03.
    uint64_t mm_last_click_ms;
    float mm_last_click_x, mm_last_click_y;

    // Config-changed flag (guarded by data_lock): set when zoom / trigger
    // settings change (scroll, dropdowns, level drag). Cleared when the
    // config is applied to a frame in waveform_vtable_process. While
    // playback is PAUSED no new display frames arrive, so waveform_render
    // re-processes the last frame on this flag (DAW-style: zoom/trigger
    // adjustments keep rendering while paused).
    bool config_dirty;

    // CVBS detected levels (updated when CVBS trigger mode active)
    int16_t cvbs_sync_level;    // Sync tip level (lowest peak)
    int16_t cvbs_blank_level;   // Blanking level (second peak)
    int16_t cvbs_threshold;     // Detection threshold (midpoint)
    bool cvbs_levels_valid;     // True if levels were successfully detected

    // Protects shared state accessed by both display and render threads.
    atomic_flag data_lock;

    // MISRC_DEBUG [WFDISP] tick counter (per panel, so every panel logs).
    uint64_t diag_disp_count;

    // Initialization flag
    bool initialized;
    bool trigger_source_defaulted_by_channel;
    uint32_t remote_last_sequence; /* M-Client only */
} waveform_panel_state_t;

typedef struct {
    bool trigger_enabled;
    int16_t trigger_level;
    trigger_mode_t trigger_mode;
    trigger_source_t trigger_source;
    float zoom_scale;
    uint32_t full_scale;        // Display-container full scale (NOT always 2048)
    int res_points;             // Render-resolution pick (0 = Auto)
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
    uint32_t full_scale;        // Display-container full scale for markers
    size_t samples_available;
} waveform_render_snapshot_t;

// Forward declarations for static helper functions
static void waveform_cleanup_resampler(waveform_panel_state_t *state);
static size_t waveform_resample_to_buffer(waveform_panel_state_t *state,
                                           waveform_sample_t *dest,
                                           const int16_t *buf, size_t num_samples,
                                           size_t start_idx, float decimation,
                                           size_t target_width,
                                           float norm_scale);
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
    cfg.full_scale = state->display_full_scale ? state->display_full_scale : 2048;
    cfg.res_points = state->res_points;
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
    snap->full_scale = state->display_full_scale ? state->display_full_scale : 2048;
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
// scale_mode: amplitude tick label format + density (0=Level thinned,
// 1=Basic majors-only, 2=Full all, 3=mV w/2Vpp, 4=mV w/1Vpp).
// channel: 0=A, 1=B (kept for ABI; unused now that the scale control is a
// per-panel overlay dropdown rather than a per-channel button).
void draw_channel_grid(float x, float y, float width, float height,
                       const char *label, Color channel_color, bool show_grid,
                       float zoom_scale, uint32_t sample_rate,
                       bool trigger_enabled, int trigger_display_pos,
                       Rectangle *time_div_rect,
                       int scale_mode, int channel) {
    if (time_div_rect) *time_div_rect = (Rectangle){0};
    // Background slightly darker than main bg
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){25, 25, 30, 255});

    float center_y = y + height / 2;

    // Text measurement contract: gui_text_measure returns the width at the
    // given fontSize in the SAME space the text is drawn (logical layout px
    // under the Clay rlScalef) - both scale together through the render
    // transform. The old 1/scale division assumed physical measurements and
    // shrank every label/button while the text stayed full-size, so at UI
    // scales >100% the text overflowed its buttons (iOS 200% report
    // 2026-11-08; desktop at >100% had the same latent bug).
    float to_logical_w = 1.0f;

    // Channel label width (used to place the time/div label beside it).
    int ch_label_w = (int)(gui_text_measure(label, FONT_SIZE_OSC_LABEL) * to_logical_w);
    int ch_label_x = (int)x + 8;
    int ch_label_y = (int)y + 4;
    (void)channel;

    // Resolve the tick set + label mode from scale_mode.
    // scale_mode index: 0=Basic,1=Expanded,2=Full,3=mV/1Vpp,4=mV/2Vpp
    // Basic (0): majors only (+-0.5/0).
    // Expanded (1): thinned (+-0.1/0.3/0.5/0.8).
    // Full (2): every 0.1 step from -0.8..+0.8.
    // mV modes (3,4): thinned ticks + mV labels.
    static const int basic_steps[]   = { -5, 0, 5 };
    static const int thinned_steps[] = { -8, -5, -3, -1, 0, 1, 3, 5, 8 };
    static const int full_steps[]   = { -8,-7,-6,-5,-4,-3,-2,-1, 0, 1,2,3,4,5,6,7,8 };
    const int *steps; int n_steps;
    if (scale_mode == 0) { steps = basic_steps;   n_steps = (int)(sizeof(basic_steps)/sizeof(basic_steps[0])); }
    else if (scale_mode == 2) { steps = full_steps;  n_steps = (int)(sizeof(full_steps)/sizeof(full_steps[0])); }
    else                     { steps = thinned_steps; n_steps = (int)(sizeof(thinned_steps)/sizeof(thinned_steps[0])); }
    bool label_mv = (scale_mode == 3 || scale_mode == 4);
    float vpp = (scale_mode == 3) ? 1.0f : 2.0f;  // mV/1Vpp vs mV/2Vpp (default 2 for level modes)

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
                        int label_w = (int)(gui_text_measure_mono("0", FONT_SIZE_OSC_SCALE) * to_logical_w);
                        gui_text_draw_mono("0", gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
                    } else {
                        format_time_label(time_buf, sizeof(time_buf), fabs(t));
                        int label_w = (int)(gui_text_measure_mono(time_buf, FONT_SIZE_OSC_SCALE) * to_logical_w);
                        gui_text_draw_mono(time_buf, gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
                    }
                }
                division_count++;
            }

            // Show time per division. Tiered collapse: the time/div label
            // stays beside the CH A/CH B readout on wide panels, then moves
            // below the channel label as the first collapse step when the
            // panel narrows (before the Scale label hides or the Mode/Trig
            // dropdowns wrap). This follows the existing overlay collapse order.
            format_time_label(time_buf, sizeof(time_buf), time_division);
            char div_label[48];
            snprintf(div_label, sizeof(div_label), "%s/div", time_buf);
            int div_x = ch_label_x + ch_label_w + 8;
            int div_label_w = (int)(gui_text_measure_mono(div_label, FONT_SIZE_OSC_DIV) * to_logical_w);
            int div_y = ch_label_y;
            // Measure the right-side overlay group (Scale + Mode + Trig
            // labels + buttons + gaps + 8px margin) so the time/div collapse
            // threshold is based on the actual space the right side needs.
            int mode_prefix_w_meas = (int)(gui_text_measure("Mode:", FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
            int trig_prefix_w_meas = (int)(gui_text_measure("Trig:", FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
            int scale_prefix_w_meas = (int)(gui_text_measure("Scale:", FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
            int btn_w_logical = (int)(98 * to_logical_w);
            int right_group_w = trig_prefix_w_meas + 4 + btn_w_logical + 4 + 8 +
                               mode_prefix_w_meas + 4 + btn_w_logical + 4 + 8 +
                               scale_prefix_w_meas + 4 + btn_w_logical + 4 + 8;
            // time/div moves below when channel + time/div + right group
            // can't all fit on one row. This is the first collapse step.
            bool div_fits_beside =
                (div_x + div_label_w + 8 + right_group_w) < (int)width;
            if (!div_fits_beside) {
                div_x = ch_label_x;
                div_y = ch_label_y + FONT_SIZE_OSC_LABEL + 2;
            }
            gui_text_draw_mono(div_label, div_x, div_y, FONT_SIZE_OSC_DIV, COLOR_TEXT);
            if (time_div_rect) {
                *time_div_rect = (Rectangle){(float)div_x, (float)div_y,
                    (float)div_label_w + 1, (float)FONT_SIZE_OSC_DIV};
            }
        } else {
            // Fallback: fixed divisions when no sample rate available
            const int fixed_divisions = 10;
            for (int i = 1; i < fixed_divisions; i++) {
                float gx = x + (width * i / fixed_divisions);
                DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height}, COLOR_GRID);
            }
        }

        // Horizontal amplitude grid lines for the active scale mode's tick set.
        // Major (0, +-0.5) uses COLOR_GRID_MAJOR; the rest use COLOR_GRID.
        for (int si = 0; si < n_steps; si++) {
            int step = steps[si];
            float v = (float)step * 0.1f;
            float frac = 0.5f - 0.5f * v;
            if (frac < 0.0f || frac > 1.0f) continue;
            float gy = y + height * frac;
            bool is_major = (step == 0 || step == 5 || step == -5);
            DrawLineV((Vector2){x, gy}, (Vector2){x + width, gy},
                      is_major ? COLOR_GRID_MAJOR : COLOR_GRID);
        }
    }

    // Center line (0V reference) - always show
    DrawLineEx((Vector2){x, center_y}, (Vector2){x + width, center_y}, 1.0f, COLOR_GRID_MAJOR);

    // Border
    DrawRectangleLinesEx((Rectangle){x, y, width, height}, 1, COLOR_GRID_MAJOR);

    // Amplitude scale ticks for the active scale mode. Major ticks (+-0.5/0)
    // use the major color; the rest use the minor grid color. Labels: 0.X level
    // for modes 0-2, mV peak (level * Vpp/2 * 1000) for modes 3-4.
    // On narrow panels the minor tick labels hide first (leaving 0, +-0.5),
    // then all labels hide — same tiered collapse as the overlay dropdowns.
    bool show_minor_labels = width >= 200;
    bool show_major_labels = width >= 120;
    for (int si = 0; si < n_steps; si++) {
        int step = steps[si];
        float v = (float)step * 0.1f;
        float frac = 0.5f - 0.5f * v;
        if (frac < 0.0f || frac > 1.0f) continue;
        float tick_y = y + height * frac;
        bool is_major = (step == 0 || step == 5 || step == -5);
        Color tick_color = is_major ? COLOR_GRID_MAJOR : COLOR_GRID;
        Color label_color = is_major ? COLOR_TEXT : COLOR_TEXT_DIM;
        // Tick mark (always draw the mark; only the label hides)
        DrawLineEx((Vector2){x, tick_y}, (Vector2){x + (is_major ? 4 : 3), tick_y},
                   1.0f, tick_color);
        // Label. Skip minor labels on narrow panels, and all labels on
        // very narrow panels. 0 is "0" in both modes; mV modes show mV, else 0.X.
        if (!show_major_labels) continue;
        if (!is_major && !show_minor_labels) continue;
        char label_buf[16];
        if (step == 0) {
            snprintf(label_buf, sizeof(label_buf), "0");
        } else if (label_mv) {
            int mv = (int)roundf(v * (vpp * 0.5f) * 1000.0f);
            if (step > 0) snprintf(label_buf, sizeof(label_buf), "+%dmV", mv);
            else snprintf(label_buf, sizeof(label_buf), "-%dmV", mv);
        } else {
            if (step > 0) snprintf(label_buf, sizeof(label_buf), "+0.%d", step);
            else snprintf(label_buf, sizeof(label_buf), "-0.%d", -step);
        }
        gui_text_draw_mono(label_buf, x + 6, tick_y - 7, FONT_SIZE_OSC_SCALE, label_color);
    }

    // Channel label in top-left corner (drawn last so it sits on top of the grid).
    // The amplitude scale is now controlled by a dropdown drawn on the waveform
    // panel overlay (see waveform_render_overlay), not a button here.
    gui_text_draw(label, ch_label_x, ch_label_y, FONT_SIZE_OSC_LABEL, channel_color);
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

        // Convert levels to Y positions (raw counts of THIS channel's
        // container; full-scale aware - not all sources are 12-bit)
        float fs = (float)(snap->full_scale ? snap->full_scale : 2048);
        float sync_y = center_y - (snap->cvbs_sync_level / fs) * scale;
        float blank_y = center_y - (snap->cvbs_blank_level / fs) * scale;
        float thresh_y = center_y - (snap->cvbs_threshold / fs) * scale;

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

    // Normal trigger mode: draw single trigger level line (full-scale aware)
    float fs = (float)(snap->full_scale ? snap->full_scale : 2048);
    float level_norm = snap->trigger_level / fs;
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
// Signal Type Helpers
//-----------------------------------------------------------------------------

// Test mode = the Simulated Test Gen device is the ACTIVE capture source
// (selected, not recording, no playback session). The CVBS/FM RF/Audio
// signal-type labels and the generator option row only apply in test mode;
// every other source (real capture devices, FLAC playback) falls back to
// the plain CH A/CH B names.
static bool waveform_test_mode_active(gui_app_t *app) {
    if (!app || app->selected_device < 0 ||
        app->selected_device >= app->device_count) {
        return false;
    }
    return app->devices[app->selected_device].type == DEVICE_TYPE_SIMULATED &&
           !app->is_recording &&
           !atomic_load(&app->playback_running);
}

// Player-only render-resolution selector visibility: the selected device is
// the playback device and the panel is not in test-generator mode (the Gen
// row owns the second overlay row there). User request 2026-11-03.
static bool waveform_playback_res_visible(gui_app_t *app) {
    if (!app || app->selected_device < 0 ||
        app->selected_device >= app->device_count) {
        return false;
    }
    if (waveform_test_mode_active(app)) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_PLAYBACK;
}

// Channel label shown in the panel grid, derived from the assigned signal
// type. Raw (or any source outside test mode) keeps the legacy
// "CH A"/"CH B" names; a typed channel uses the "CH1"/"CH2" numbering
// with the signal appended (e.g. "CH1 CVBS").
static void waveform_channel_label(char *buf, size_t len, int channel,
                                    waveform_signal_type_t type,
                                    bool type_active) {
    if (!buf || len == 0) return;
    if (!type_active) {
        snprintf(buf, len, "%s", (channel == 0) ? "CH A" : "CH B");
        return;
    }
    switch (type) {
        case WAVE_SIGNAL_CVBS:
            snprintf(buf, len, "CH%d CVBS", channel + 1);
            break;
        case WAVE_SIGNAL_FM_RF:
            snprintf(buf, len, "CH%d FM RF", channel + 1);
            break;
        case WAVE_SIGNAL_AUDIO:
            snprintf(buf, len, "CH%d Audio", channel + 1);
            break;
        case WAVE_SIGNAL_RAW:
        default:
            snprintf(buf, len, "%s", (channel == 0) ? "CH A" : "CH B");
            break;
    }
}

static void waveform_apply_signal_defaults(waveform_panel_state_t *state,
                                           waveform_signal_type_t type);

// Channel default signal type: CH1 = composite CVBS, CH2 = tape FM RF
// (the Test Gen channel mapping). TEST SIGNAL MODE ONLY: applied once, at
// the panel's first render/click while the Simulated generator is the
// active source, so the waveform monitors open with the generator mapping
// already assigned (user request 2026-11-03: "set cvbs on ch1 and fm rf
// on ch2 on default opening"). Record/playback panels NEVER get these
// stock trigger configs (user request 2026-11-03: "remove the stock
// triggers on config from record/playback that was applied over from test
// signal mode"). Raw is never user-selectable.
static void waveform_apply_channel_signal_default(waveform_panel_state_t *state,
                                                  gui_app_t *app, int channel) {
    if (!state || state->signal_type != WAVE_SIGNAL_RAW) return;
    if (!waveform_test_mode_active(app)) return;
    waveform_apply_signal_defaults(state,
                                   (channel == 1) ? WAVE_SIGNAL_FM_RF
                                                  : WAVE_SIGNAL_CVBS);
}

// Leaving Test Signal mode: drop the generator's stock signal/trigger
// config that was applied over from test mode. Record/playback panels run
// with the STOCK trigger (off, rising edge, level 0) and the plain CH A /
// CH B mapping. A signal type the USER picked from the dropdown is never
// touched (the pick survives mode switches as before).
static void waveform_clear_generator_signal(waveform_panel_state_t *state) {
    if (!state || state->signal_type == WAVE_SIGNAL_RAW) return;
    if (state->signal_user_picked) return;
    waveform_state_lock(state);
    state->signal_type = WAVE_SIGNAL_RAW;
    state->trigger_enabled = false;
    state->trigger_mode = TRIGGER_MODE_RISING;
    state->trigger_level = 0;
    state->config_dirty = true;
    waveform_state_unlock(state);
}

static void waveform_apply_signal_defaults(waveform_panel_state_t *state,
                                           waveform_signal_type_t type) {
    if (!state) return;
    waveform_state_lock(state);
    switch (type) {
        case WAVE_SIGNAL_CVBS:
            // Composite video: auto-detected CVBS H-sync trigger
            state->trigger_enabled = true;
            state->trigger_mode = TRIGGER_MODE_CVBS_HSYNC;
            break;
        case WAVE_SIGNAL_FM_RF:
        case WAVE_SIGNAL_AUDIO:
            // RF / audio carriers: plain rising-edge trigger at mid-scale
            state->trigger_enabled = true;
            state->trigger_mode = TRIGGER_MODE_RISING;
            state->trigger_level = 0;
            break;
        case WAVE_SIGNAL_RAW:
        default:
            break;
    }
    state->signal_type = type;
    state->config_dirty = true;
    waveform_state_unlock(state);
}

//-----------------------------------------------------------------------------
// Waveform Panel Rendering (using panel state)
//-----------------------------------------------------------------------------

// Internal render for line mode using panel state
static void render_waveform_line_internal(waveform_panel_state_t *state,
                                           gui_app_t *app, int channel,
                                           Rectangle bounds, Color color) {
    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
    char label_buf[32];
    waveform_channel_label(label_buf, sizeof(label_buf), channel,
                           state->signal_type, waveform_test_mode_active(app));
    waveform_render_snapshot_t snap = {0};
    waveform_sample_t render_samples[DISPLAY_BUFFER_SIZE];
    waveform_capture_render_snapshot(state, &snap, render_samples);

    // Update display width for processing thread
    int new_display_width = (int)w;
    if (new_display_width < 100) new_display_width = 100;
    if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
    atomic_store(&state->display_width, new_display_width);

    // Trigger x in POINT domain: the frame holds display_width/factor
    // points across the panel (Res 1:1 = one point per pixel). Map the
    // trigger point index to panel pixels for the grid t=0 reference.
    int points_in_frame = (int)snap.samples_available;
    int trig_px = snap.trigger_display_pos;
    if (snap.trigger_enabled && trig_px >= 0 && points_in_frame > 1) {
        trig_px = (int)((float)trig_px * w / (float)(points_in_frame - 1));
        if (trig_px >= (int)w) trig_px = (int)w - 1;
    }

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label_buf, color, app->settings.show_grid,
                      snap.zoom_scale, sample_rate,
                      snap.trigger_enabled, trig_px, &state->time_div_rect,
                      app->settings.waveform_scale_mode, channel);

    // Draw trigger level and position markers
    draw_panel_trigger_markers(x, y, w, h, &snap, app->settings.amplitude_scale, color);
    if (snap.samples_available == 0) return;

    // ALL points in the frame are drawn, stretched across the panel width
    // (Res selector: the point count is the output resolution, not pixels).
    int samples_to_draw = (int)snap.samples_available;
    if (samples_to_draw > DISPLAY_BUFFER_SIZE) samples_to_draw = DISPLAY_BUFFER_SIZE;
    float step_x = (samples_to_draw > 1) ? w / (float)(samples_to_draw - 1) : 0.0f;

    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;

    // Draw waveform as connected line
    float prev_py = center_y;
    float prev_px_x = x;
    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + (float)px * step_x;
        float py = center_y - render_samples[px].value * scale;

        // Clamp to bounds
        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){prev_px_x, prev_py}, (Vector2){px_x, py}, 1.0f, color);
        }
        prev_px_x = px_x;
        prev_py = py;
    }
}

// Internal render for phosphor mode using panel state
static void render_waveform_phosphor_internal(waveform_panel_state_t *state,
                                               gui_app_t *app, int channel,
                                               Rectangle bounds, Color color) {
    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
    char label_buf[32];
    waveform_channel_label(label_buf, sizeof(label_buf), channel,
                           state->signal_type, waveform_test_mode_active(app));
    waveform_render_snapshot_t snap = {0};
    waveform_sample_t render_samples[DISPLAY_BUFFER_SIZE];
    waveform_capture_render_snapshot(state, &snap, render_samples);

    // Update display width for processing thread
    int new_display_width = (int)w;
    if (new_display_width < 100) new_display_width = 100;
    if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
    atomic_store(&state->display_width, new_display_width);

    // Trigger x in POINT domain (see render_waveform_line_internal)
    int points_in_frame = (int)snap.samples_available;
    int trig_px = snap.trigger_display_pos;
    if (snap.trigger_enabled && trig_px >= 0 && points_in_frame > 1) {
        trig_px = (int)((float)trig_px * w / (float)(points_in_frame - 1));
        if (trig_px >= (int)w) trig_px = (int)w - 1;
    }

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label_buf, color, app->settings.show_grid,
                      snap.zoom_scale, sample_rate,
                      snap.trigger_enabled, trig_px, &state->time_div_rect,
                      app->settings.waveform_scale_mode, channel);

    // Draw trigger level and position markers
    draw_panel_trigger_markers(x, y, w, h, &snap, app->settings.amplitude_scale, color);
    if (snap.samples_available == 0) return;

    int buf_width = (int)w;
    int buf_height = (int)h;
    if (buf_width <= 0 || buf_height <= 0) return;

    // ALL points in the frame are drawn, stretched across the panel width
    // (Res selector: the point count is the output resolution, not pixels).
    int samples_to_draw = (int)snap.samples_available;
    if (samples_to_draw > DISPLAY_BUFFER_SIZE) samples_to_draw = DISPLAY_BUFFER_SIZE;
    float step_x = (samples_to_draw > 1) ? (float)buf_width / (float)(samples_to_draw - 1) : 0.0f;

    // Get phosphor state from panel
    phosphor_rt_t *prt = state->phosphor;
    if (prt) {
        // Initialize/resize phosphor if needed
        Vector2 render_scale = gui_ui_get_render_scale();
        phosphor_rt_init(prt, buf_width, buf_height,
                         render_scale.x, render_scale.y);

        // Update phosphor
        phosphor_rt_begin_frame(prt);
        phosphor_rt_draw_waveform_xstep(prt, render_samples, samples_to_draw,
                                        app->settings.amplitude_scale, step_x);
        phosphor_rt_end_frame(prt);

        // Render phosphor to screen
        if (state->phosphor_color == PHOSPHOR_COLOR_OPACITY) {
            phosphor_rt_render_opacity(prt, x, y);
        } else {
            phosphor_rt_render(prt, x, y, false);
        }
    }

    // Draw line overlay on phosphor (semi-transparent), stretched across
    // the panel with the same point-domain step as the phosphor pass
    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;
    float prev_py = center_y;
    float prev_px_x = x;
    Color waveform_color = {color.r, color.g, color.b, 200};

    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + (float)px * step_x;
        float py = center_y - render_samples[px].value * scale;

        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){prev_px_x, prev_py}, (Vector2){px_x, py}, 1.0f, waveform_color);
        }
        prev_px_x = px_x;
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
                                           size_t target_width,
                                           float norm_scale) {
    // Normalize by THIS source's display-container full scale (caller
    // derives it from the per-channel full-scale state; NOT always 2048 -
    // 8-bit/12-bit/16-bit-shift4 playback is 2048, direct-domain 16-bit is
    // 4096, DdD is 512).
    const float scale = (norm_scale > 0.0f) ? norm_scale : (1.0f / 2048.0f);

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

    // Normalization for THIS source's container (not all sources are
    // 12-bit: 8-bit/12-bit/16-bit-shift4 playback is 2048, direct-domain
    // 16-bit is 4096, DdD is 512).
    float norm_scale = 1.0f / (float)(cfg->full_scale ? cfg->full_scale : 2048);

    // Resolved render resolution (FIRST-implementation model, restored
    // 2026-11-03 per user confirmation): Auto -> one point per pixel; a
    // fixed Res PINS THE POINT COUNT (clamped to the display buffer)
    // while the panel coverage stays display_width pixels - selecting
    // MORE points than pixels OVERSAMPLES the window (more visual
    // information; the stretch renderer draws every point).
    size_t point_count = display_width;
    if (cfg->res_points != WAVEFORM_RES_AUTO) {
        int fixed = waveform_res_points_value(cfg->res_points);
        if (fixed > 0) {
            point_count = (size_t)fixed;
            if (point_count > DISPLAY_BUFFER_SIZE) point_count = DISPLAY_BUFFER_SIZE;
        }
    }
    if (point_count == 0) point_count = 1;

    // Get decimation factor from zoom_scale. zoom_scale is samples per
    // PIXEL; the resampler runs in the point domain, so scale by the
    // points-per-pixel stretch factor (no-op for Auto; a higher preset
    // count LOWERS the decimation -> more raw samples per frame).
    float decimation = cfg->zoom_scale;
    if (decimation < ZOOM_SCALE_MIN) decimation = ZOOM_SCALE_MIN;
    if (decimation > ZOOM_SCALE_MAX) decimation = ZOOM_SCALE_MAX;
    decimation *= (float)display_width / (float)point_count;

    // How many raw samples we need for the full display at this zoom:
    // points across the panel x raw samples per point.
    float display_window = (float)point_count * decimation;

    // If trigger is disabled, just show the start of the buffer
    if (!cfg->trigger_enabled) {
        *out_trigger_display_pos = -1;
        *out_available = waveform_resample_to_buffer(
            state, out_samples, buf, num_samples, 0, decimation, point_count,
            norm_scale);
        return true;
    }

    // When zoomed out so far that display_window >= 90% of buffer,
    // there's no room for trigger positioning - just show from start
    if (display_window >= (float)num_samples * 0.9f) {
        *out_trigger_display_pos = -1;
        *out_available = waveform_resample_to_buffer(
            state, out_samples, buf, num_samples, 0, decimation, point_count,
            norm_scale);
        return true;
    }

    // Trigger point should appear at 10% across the display (point
    // domain: the renderer stretches the points across the panel width)
    size_t trigger_display_pos = point_count / 10;
    float pre_trigger_raw_samples = (float)trigger_display_pos * decimation;
    float post_trigger_raw_samples = display_window - pre_trigger_raw_samples;

    // Calculate the valid search range for triggers
    size_t min_trig_pos = (size_t)pre_trigger_raw_samples;
    size_t max_trig_pos = num_samples - (size_t)post_trigger_raw_samples;

    // Check if there's a valid search range
    if (min_trig_pos >= max_trig_pos) {
        *out_trigger_display_pos = -1;
        *out_available = waveform_resample_to_buffer(
            state, out_samples, buf, num_samples, 0, decimation, point_count,
            norm_scale);
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
        state, out_samples, buf, num_samples, start_pos, decimation, point_count,
        norm_scale);
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

        // Cheap gated diagnostics (MISRC_DEBUG, ~2 s per panel): the EXACT
        // normalized display values driving the trace - what the resampler
        // produced against this panel's full scale. Automated verification
        // of playback levels without eyeballs (dev note 2026-11-03; the
        // common path is a counter compare, the scan only runs on the log
        // tick - the old WFTRACE pattern).
        if (misrc_debug_enabled() && new_available > 0) {
            state->diag_disp_count++;
            if ((state->diag_disp_count % 560) == 0) {
                size_t n = new_available < 4096 ? new_available : 4096;
                float mn = 1e9f, mx = -1e9f;
                double sum = 0.0;
                for (size_t i = 0; i < n; i++) {
                    float v = new_samples[i].value;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                    sum += v;
                }
                fprintf(stderr,
                        "[WFDISP] panel=%p n=%zu vrange=[%.3f,%.3f] vmean=%.3f\n",
                        (void *)state, new_available, mn, mx,
                        (float)(sum / (double)n));
            }
        }
    }
    // The current config has been applied to this frame: a pending
    // zoom/trigger change no longer needs a paused re-process.
    state->config_dirty = false;
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
    state->scale_dropdown_open = false;
    state->scale_mode = 0;  // Basic (matches the settings default)
    // Display-container full scale mirror (synced from app each render;
    // 2048 = stock 12-bit container until a source says otherwise)
    state->display_full_scale = 2048;
    // Render-resolution selector defaults (playback-only control): Fine
    // range group, maximum preset (user request 2026-11-03 - the player
    // opens with the most visual information). The process path clamps the
    // preset to DISPLAY_BUFFER_SIZE, so 32768 renders at the engine's
    // point-count cap.
    state->res_range = WAVEFORM_RES_RANGE_FINE;
    state->res_points = WAVEFORM_RES_PTS_32768;
    state->range_dropdown_open = false;
    state->res_dropdown_open = false;
    state->signal_type = WAVE_SIGNAL_RAW;
    state->signal_dropdown_open = false;
    state->signal_user_picked = false;
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

    // Double-click on the panel (waveform_panel_handle_click) toggles ALL
    // the overlay chrome — Scale/Mode/Trig dropdowns + their labels — for a
    // clean waveform view; the grid, channel label and waveform (drawn by
    // the data render path) keep rendering.
    if (gui_chrome_hide_hidden(&state->chrome_hide)) return;

    float btn_h = 18;
    float render_btn_y = bounds.y + 8;
    float trig_btn_y = render_btn_y;
    Vector2 mouse = gui_ui_get_mouse_position();

    // Text measurement contract: gui_text_measure is in the draw space
    // (logical px; see the grid-render note above) - no conversion.
    float to_logical_w = 1.0f;
    #define TEXT_W(t, fs) ((int)(gui_text_measure(t, fs) * to_logical_w))

    // Reserve the widest label plus arrow/padding, not just the current value.
    // Both dropdowns keep the same width when the selection changes.
    float button_width = 98.0f * to_logical_w;
    for (int i = 0; i < WAVEFORM_MODE_COUNT; i++) {
        button_width = fmaxf(button_width,
            (float)TEXT_W(s_render_mode_labels[i], FONT_SIZE_DROPDOWN_OPT) + 24);
    }
    for (int i = 0; i < TRIGGER_MODE_COUNT; i++) {
        button_width = fmaxf(button_width,
            (float)TEXT_W(s_trigger_mode_labels[i], FONT_SIZE_DROPDOWN_OPT) + 12);
        for (int source = 0; source < TRIGGER_SOURCE_COUNT; source++) {
            char label[24];
            snprintf(label, sizeof(label), "%s/%s",
                     s_trigger_mode_short_labels[i], s_trigger_source_labels[source]);
            button_width = fmaxf(button_width,
                (float)TEXT_W(label, FONT_SIZE_DROPDOWN_OPT) + 24);
        }
    }
    float trig_btn_w = button_width;
    float render_btn_w = button_width;

    // Scale dropdown uses the same button width as Mode/Trig for a uniform look.
    float scale_btn_w = button_width;

    const char *trig_prefix = "Trig:";
    const char *mode_prefix = "Mode:";
    const char *scale_prefix = "Scale:";
    int trig_prefix_w = TEXT_W(trig_prefix, FONT_SIZE_DROPDOWN_OPT);
    int mode_prefix_w = TEXT_W(mode_prefix, FONT_SIZE_DROPDOWN_OPT);
    int scale_prefix_w = TEXT_W(scale_prefix, FONT_SIZE_DROPDOWN_OPT);

    // All three dropdowns are right-anchored as a unified group:
    //   [Scale:] [Mode:] [Trig:]   (right to left)
    float trig_btn_x = bounds.x + bounds.width - trig_btn_w - 8;
    float render_btn_x = trig_btn_x - trig_prefix_w - 4 - render_btn_w - 4;
    float scale_btn_x = render_btn_x - mode_prefix_w - 4 - scale_btn_w - 4;
    float scale_label_x = scale_btn_x - scale_prefix_w - 4;
    float mode_label_x = render_btn_x - mode_prefix_w - 4;
    float trig_label_x = trig_btn_x - trig_prefix_w - 4;
    float text_offset_y = (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    float scale_label_y = render_btn_y + text_offset_y;
    float mode_label_y = render_btn_y + text_offset_y;
    float trig_label_y = trig_btn_y + text_offset_y;
    float channel_label_w = fmaxf((float)TEXT_W("CH A", FONT_SIZE_OSC_LABEL),
                                 (float)TEXT_W("CH B", FONT_SIZE_OSC_LABEL));

    // Fixed logical thresholds for label hiding. The dropdown BUTTONS
    // always stay; only their text LABELS (Scale:/Mode:/Trig:) hide
    // based on the panel's logical width. This triggers correctly at
    // all UI zoom levels (e.g. 200% zoom halves the logical width,
    // so labels hide on normal-size windows when zoomed in).
    bool scale_label_hidden = (bounds.width < 600);
    bool mode_label_hidden = (bounds.width < 500);
    bool trig_label_hidden = (bounds.width < 400);

    // Re-derive button x positions from the right edge, skipping
    // hidden label gaps (buttons slide right to fill freed space).
    // Group, right to left: [Trig] [Mode] [Scale].
    float group_right = bounds.x + bounds.width - 8;
    trig_btn_x = group_right - trig_btn_w;
    render_btn_x = trig_btn_x - (trig_label_hidden ? 0 : (trig_prefix_w + 4)) - render_btn_w - 4;
    scale_btn_x = render_btn_x - (mode_label_hidden ? 0 : (mode_prefix_w + 4)) - scale_btn_w - 4;
    scale_label_x = scale_btn_x - (scale_label_hidden ? 0 : (scale_prefix_w + 4));
    mode_label_x = render_btn_x - (mode_label_hidden ? 0 : (mode_prefix_w + 4));
    trig_label_x = trig_btn_x - (trig_label_hidden ? 0 : (trig_prefix_w + 4));
    if (scale_label_hidden) scale_label_x = -1;
    if (mode_label_hidden) mode_label_x = -1;
    if (trig_label_hidden) trig_label_x = -1;

    // Clamp the Scale button so it stays inside the panel.
    if (scale_btn_x < bounds.x + 8) {
        scale_btn_x = bounds.x + 8;
    }

    // Wrap Mode/Trig to rows when the Mode button itself doesn't fit
    // beside the CH label.
    if (render_btn_x < bounds.x + 8 + channel_label_w + 8) {
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
        scale_label_y = render_btn_y + text_offset_y;
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

    // Draw "Trig:" label (hidden on narrow panels)
    if (!trig_label_hidden) {
        gui_text_draw(trig_prefix, trig_label_x, trig_label_y,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);
    }

    char trig_label_buf[24];
    const char *trig_label = "Off";
    if (state->trigger_enabled) {
        snprintf(trig_label_buf, sizeof(trig_label_buf), "%s/%s",
                 s_trigger_mode_short_labels[state->trigger_mode],
                 s_trigger_source_labels[state->trigger_source]);
        trig_label = trig_label_buf;
    }
    draw_dropdown_button(state->trigger_btn_rect, trig_label, state->trigger_dropdown_open);

    // Draw all controls before any popup so a wrapped button cannot cover
    // an open menu. Mode is above trigger when the controls use separate rows.
    state->render_mode_btn_rect = (Rectangle){render_btn_x, render_btn_y, render_btn_w, btn_h};
    if (!mode_label_hidden) {
        gui_text_draw(mode_prefix, mode_label_x, mode_label_y,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);
    }
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

    //-------------------------------------------------------------------------
    // Amplitude Scale Dropdown (part of the right-anchored group, left of Mode)
    // Options: Basic / Expanded / Full / 1Vpp / 2Vpp. Same width + alignment as
    // Mode/Trig. Hides entirely on narrow panels (handled by the wrap logic
    // above which sets scale_btn_x < 0).
    //-------------------------------------------------------------------------
    static const char *s_scale_labels[5] = { "Basic", "Expanded", "Full", "1Vpp", "2Vpp" };
    // Scale dropdown: button ALWAYS visible (never hidden).
    // Only its "Scale:" text label hides on narrow panels.
    bool scale_visible = true;
    float scale_btn_y = render_btn_y;  // same row as Mode
    if (scale_visible) {
        if (!scale_label_hidden && scale_label_x >= 0) {
            gui_text_draw(scale_prefix, scale_label_x, scale_label_y,
                          FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);
        }
        state->scale_btn_rect = (Rectangle){scale_btn_x, scale_btn_y, scale_btn_w, btn_h};
        const char *scale_cur = (state->scale_mode >= 0 && state->scale_mode < 5)
            ? s_scale_labels[state->scale_mode] : s_scale_labels[0];
        draw_dropdown_button(state->scale_btn_rect, scale_cur, state->scale_dropdown_open);

        // Draw scale dropdown options if open
        if (state->scale_dropdown_open) {
            float opt_h = 20;
            float opt_y = fmaxf(bounds.y, fminf(scale_btn_y + btn_h,
                                bounds.y + bounds.height - opt_h * 5 - 4));
            DrawRectangleRounded((Rectangle){scale_btn_x, opt_y, scale_btn_w, opt_h * 5},
                                 0.1f, 4, COLOR_PANEL_BG);
            for (int i = 0; i < 5; i++) {
                Rectangle opt_rect = {scale_btn_x, opt_y + i * opt_h, scale_btn_w, opt_h};
                state->scale_opts_rect[i] = opt_rect;
                bool is_selected = (state->scale_mode == i);
                bool hover = CheckCollisionPointRec(mouse, opt_rect);
                DrawRectangleRec(opt_rect, gui_dropdown_option_color(is_selected, hover));
                const char *opt_label = s_scale_labels[i];
                int opt_w = gui_text_measure(opt_label, FONT_SIZE_DROPDOWN_OPT);
                gui_text_draw(opt_label, scale_btn_x + scale_btn_w/2 - opt_w/2,
                              opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2,
                              FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
            }
        }
    }

    // The Signal Type dropdown is NOT part of this row-1 group: it lives on
    // the test-mode-only generator row below (see waveform_render), next to
    // the Gen system box. Outside test mode it is not rendered at all.
#undef TEXT_W
}

//-----------------------------------------------------------------------------
// Waveform Panel Click Handler (with dropdown support)
//-----------------------------------------------------------------------------

// Overlay control clicks (Scale / Mode / Trig dropdowns). Returns true when a
// control consumed the click (button hit or an open dropdown's option/close
// path); false when no control was hit, so the caller can offer the click to
// the double-click chrome-hide tracker.
static bool waveform_handle_control_click(waveform_panel_state_t *state, struct gui_app *app,
                                           int channel, Vector2 click) {

    // A popup can overlap either button after wrapping or shifting upward.
    // Its visible rows (including the Channel header) take click priority.
    Rectangle render_menu = state->render_mode_opts_rect[0];
    render_menu.height = WAVEFORM_MODE_COUNT * 20;
    Rectangle trigger_menu = state->trigger_mode_opts_rect[0];
    trigger_menu.height = (TRIGGER_MODE_COUNT + 2 + TRIGGER_SOURCE_COUNT) * 20;
    Rectangle scale_menu = state->scale_opts_rect[0];
    scale_menu.height = 5 * 20;
    Rectangle signal_menu = state->signal_opts_rect[0];
    signal_menu.height = WAVE_SIGNAL_OPTION_COUNT * 20;
    Rectangle res_menu = state->res_opts_rect[0];
    {
        // Match the menu the render path actually drew: Auto + the ACTIVE
        // group's presets. Using the fine-group size while Coarse is active
        // counted an 80px dead zone below the menu as "on menu".
        size_t res_group_n = (state->res_range == WAVEFORM_RES_RANGE_FINE)
            ? sizeof(s_res_fine) / sizeof(s_res_fine[0])
            : sizeof(s_res_coarse) / sizeof(s_res_coarse[0]);
        res_menu.height = (float)(res_group_n + 1) * 20.0f;
    }
    Rectangle range_menu = state->range_opts_rect[0];
    range_menu.height = WAVEFORM_RES_RANGE_COUNT * 20;
    bool click_on_menu =
        (state->render_mode_dropdown_open && CheckCollisionPointRec(click, render_menu)) ||
        (state->trigger_dropdown_open && CheckCollisionPointRec(click, trigger_menu)) ||
        (state->scale_dropdown_open && CheckCollisionPointRec(click, scale_menu)) ||
        (state->signal_dropdown_open && CheckCollisionPointRec(click, signal_menu)) ||
        (state->res_dropdown_open && CheckCollisionPointRec(click, res_menu)) ||
        (state->range_dropdown_open && CheckCollisionPointRec(click, range_menu));

    //-------------------------------------------------------------------------
    // Signal Type Dropdown (leftmost of the group)
    //-------------------------------------------------------------------------
    if (!click_on_menu && CheckCollisionPointRec(click, state->signal_btn_rect)) {
        state->signal_dropdown_open = !state->signal_dropdown_open;
        state->scale_dropdown_open = false;  // Close other dropdowns
        state->render_mode_dropdown_open = false;
        state->trigger_dropdown_open = false;
        state->res_dropdown_open = false;
        return true;
    }

    if (state->signal_dropdown_open) {
        // Options list CVBS / FM RF / Audio (Raw is not selectable)
        for (int i = 0; i < WAVE_SIGNAL_OPTION_COUNT; i++) {
            Rectangle opt_rect = state->signal_opts_rect[i];
            if (CheckCollisionPointRec(click, opt_rect)) {
                waveform_apply_signal_defaults(state,
                                               WAVE_SIGNAL_OPTION_VALUE(i));
                // A user pick survives mode switches; only the
                // generator-applied default is cleared outside test mode.
                waveform_state_lock(state);
                state->signal_user_picked = true;
                waveform_state_unlock(state);
                state->signal_dropdown_open = false;
                return true;
            }
        }
        // Click outside closes dropdown
        state->signal_dropdown_open = false;
        return true;
    }

    //-------------------------------------------------------------------------
    // Render-Resolution Dropdown (playback device only; the render path
    // zeroes the button rect for every other source, so this is inert
    // outside the player)
    //-------------------------------------------------------------------------
    if (!click_on_menu && state->res_btn_rect.width > 0 &&
        CheckCollisionPointRec(click, state->res_btn_rect)) {
        state->res_dropdown_open = !state->res_dropdown_open;
        state->scale_dropdown_open = false;
        state->render_mode_dropdown_open = false;
        state->trigger_dropdown_open = false;
        state->signal_dropdown_open = false;
        return true;
    }

    if (state->res_dropdown_open) {
        const waveform_res_points_t *group =
            (state->res_range == WAVEFORM_RES_RANGE_FINE) ? s_res_fine : s_res_coarse;
        size_t group_n = (state->res_range == WAVEFORM_RES_RANGE_FINE)
            ? sizeof(s_res_fine) / sizeof(s_res_fine[0])
            : sizeof(s_res_coarse) / sizeof(s_res_coarse[0]);
        // Row 0: Auto (stock one-point-per-pixel)
        if (CheckCollisionPointRec(click, state->res_opts_rect[0])) {
            waveform_state_lock(state);
            state->res_points = WAVEFORM_RES_AUTO;
            state->config_dirty = true;
            waveform_state_unlock(state);
            state->res_dropdown_open = false;
            return true;
        }
        // Rows 1..N: the active group's presets (rects drawn by the
        // render path; row i+1 holds group[i])
        for (size_t i = 0; i < group_n; i++) {
            if (CheckCollisionPointRec(click, state->res_opts_rect[i + 1])) {
                waveform_state_lock(state);
                state->res_points = (int)group[i];
                state->config_dirty = true;
                waveform_state_unlock(state);
                state->res_dropdown_open = false;
                return true;
            }
        }
        // Click outside closes dropdown
        state->res_dropdown_open = false;
        return true;
    }

    if (!click_on_menu && state->range_btn_rect.width > 0 &&
        CheckCollisionPointRec(click, state->range_btn_rect)) {
        state->range_dropdown_open = !state->range_dropdown_open;
        state->res_dropdown_open = false;
        state->scale_dropdown_open = false;
        state->render_mode_dropdown_open = false;
        state->trigger_dropdown_open = false;
        state->signal_dropdown_open = false;
        return true;
    }

    if (state->range_dropdown_open) {
        for (int i = 0; i < WAVEFORM_RES_RANGE_COUNT; i++) {
            if (CheckCollisionPointRec(click, state->range_opts_rect[i])) {
                waveform_state_lock(state);
                state->res_range = i;
                // A Fine-only pick falls back to the group's largest
                // preset so the Res selection stays valid.
                state->res_points = waveform_res_clamp_to_range(state->res_points, i);
                state->config_dirty = true;
                waveform_state_unlock(state);
                state->range_dropdown_open = false;
                return true;
            }
        }
        // Click outside closes dropdown
        state->range_dropdown_open = false;
        return true;
    }

    //-------------------------------------------------------------------------
    // Amplitude Scale Dropdown (left side)
    //-------------------------------------------------------------------------
    if (!click_on_menu && CheckCollisionPointRec(click, state->scale_btn_rect)) {
        state->scale_dropdown_open = !state->scale_dropdown_open;
        state->render_mode_dropdown_open = false;  // Close other dropdowns
        state->trigger_dropdown_open = false;
        state->signal_dropdown_open = false;
        state->res_dropdown_open = false;
        return true;
    }

    if (state->scale_dropdown_open) {
        for (int i = 0; i < 5; i++) {
            Rectangle opt_rect = state->scale_opts_rect[i];
            if (CheckCollisionPointRec(click, opt_rect)) {
                app->settings.waveform_scale_mode = i;
                state->scale_mode = i;
                gui_settings_save(&app->settings);
                state->scale_dropdown_open = false;
                return true;
            }
        }
        // Click outside closes dropdown
        state->scale_dropdown_open = false;
        return true;
    }

    //-------------------------------------------------------------------------
    // Render Mode Dropdown
    //-------------------------------------------------------------------------
    if (!click_on_menu && CheckCollisionPointRec(click, state->render_mode_btn_rect)) {
        state->render_mode_dropdown_open = !state->render_mode_dropdown_open;
        state->trigger_dropdown_open = false;  // Close other dropdown
        state->scale_dropdown_open = false;
        state->signal_dropdown_open = false;
        state->res_dropdown_open = false;
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
        state->scale_dropdown_open = false;
        state->signal_dropdown_open = false;
        state->res_dropdown_open = false;
        return true;
    }

    if (state->trigger_dropdown_open) {
        // Check \"Off\" option
        Rectangle off_rect = state->trigger_mode_opts_rect[0];
        if (CheckCollisionPointRec(click, off_rect)) {
            waveform_state_lock(state);
            state->trigger_enabled = false;
            state->config_dirty = true;
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
                state->config_dirty = true;
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
                state->config_dirty = true;
                waveform_state_unlock(state);
                state->trigger_dropdown_open = false;
                return true;
            }
        }

        // Click outside closes dropdown
        state->trigger_dropdown_open = false;
        return true;
    }

    // No overlay control was hit by this click.
    return false;
}

static bool waveform_panel_handle_click(void *state_ptr, struct gui_app *app, int channel,
                                         Vector2 click, Rectangle bounds) {

    if (!state_ptr || !app) return false;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    waveform_apply_channel_trigger_source_default(state, channel);
    waveform_apply_channel_signal_default(state, app, channel);

    // Click outside the panel resets double-click tracking.
    if (!CheckCollisionPointRec(click, bounds)) {
        gui_chrome_hide_reset(&state->chrome_hide);
        return false;
    }

    // Simulated-device system selector button (rendered by waveform_render,
    // test mode only): cycles PAL -> NTSC -> SECAM, plus MESECAM on FM RF
    // signal panels. MESECAM is an RF-only system (VHS SECAM tapes are
    // MESECAM color-under; the composite side of a MESECAM generator is
    // plain SECAM), so CVBS/Audio panels cycle the 3 composite systems and
    // a MESECAM generator state displays as its composite equivalent,
    // SECAM, cycling on from there (user request 2026-11-03: "remove
    // MESECAM from CVBS mode"). The simulated capture thread detects the
    // settings.video_system change on its next batch and regenerates both
    // channels. Every Video panel's decoder previews switch to the new
    // system (manual picks keep working between generator changes; the
    // next Gen change flips them again).
    if (state->sim_sys_btn_rect.width > 0 &&
        CheckCollisionPointRec(click, state->sim_sys_btn_rect)) {
        int cur = gui_testgen_clamp_system(app->settings.video_system);
        int span = (state->signal_type == WAVE_SIGNAL_FM_RF) ? 4 : 3;
        int base = (span == 3 && cur == 3) ? 2 : cur;
        int next = (base + 1) % span;
        app->settings.video_system = next;
        gui_settings_save(&app->settings);
        gui_video_follow_generator_system(app, next);
        gui_app_set_status(app, next == 0 ? "Test Gen: PAL" :
                                 next == 1 ? "Test Gen: NTSC" :
                                 next == 2 ? "Test Gen: SECAM" :
                                            "Test Gen: MESECAM");
        return true;
    }

    // Generator tape-format selector button (test mode only, FM RF signal
    // panels — the render path zeroes the rect in every other context, so
    // the width gate doubles as the visibility gate): cycles
    // VHS -> S-VHS -> Betamax -> Video8 -> Hi8; the simulated capture
    // thread detects the settings.gen_tape_format change on its next batch
    // and regenerates ONLY the RF modulation parameters (CH1's composite
    // stream keeps flowing untouched).
    if (state->sim_fmt_btn_rect.width > 0 &&
        CheckCollisionPointRec(click, state->sim_fmt_btn_rect)) {
        int next_fmt = (gui_testgen_clamp_format(app->settings.gen_tape_format) + 1)
                       % GUI_TESTGEN_FORMAT_COUNT;
        app->settings.gen_tape_format = next_fmt;
        gui_settings_save(&app->settings);
        gui_app_set_status(app, next_fmt == 0 ? "Test Gen Format: VHS" :
                                 next_fmt == 1 ? "Test Gen Format: S-VHS" :
                                 next_fmt == 2 ? "Test Gen Format: Betamax" :
                                 next_fmt == 3 ? "Test Gen Format: Video8" :
                                                "Test Gen Format: Hi8");
        return true;
    }

    // Control clicks first (only while the overlay is shown — its rectangles
    // are stale while hidden) so a double-click ON a control never toggles
    // the chrome; a consumed control click also resets the tracker.
    if (!gui_chrome_hide_hidden(&state->chrome_hide) &&
        waveform_handle_control_click(state, app, channel, click)) {
        gui_chrome_hide_reset(&state->chrome_hide);
        return true;
    }

    // Double-click anywhere else on the panel toggles ALL overlay
    // chrome (Signal/Scale/Mode/Trig dropdowns + labels) for a clean
    // waveform view.
    if (gui_chrome_hide_feed_click(&state->chrome_hide, click)) {
        state->render_mode_dropdown_open = false;
        state->trigger_dropdown_open = false;
        state->scale_dropdown_open = false;
        state->signal_dropdown_open = false;
        state->res_dropdown_open = false;
        return true;
    }

    //-------------------------------------------------------------------------
    // Trigger Level Drag
    //-------------------------------------------------------------------------

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
        // Level in THIS channel's raw container domain (full-scale aware)
        state->trigger_level = (int16_t)(level_norm *
            (float)(state->display_full_scale ? state->display_full_scale - 1 : 2047));
        state->config_dirty = true;
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

        // Level in THIS channel's raw container domain (full-scale aware)
        state->trigger_level = (int16_t)(level_norm *
            (float)(state->display_full_scale ? state->display_full_scale - 1 : 2047));
        state->config_dirty = true;
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
        state->config_dirty = true;
        waveform_state_unlock(state);
    } else {
        // Scroll down = zoom out (more samples per pixel)
        waveform_state_lock(state);
        state->zoom_scale *= zoom_factor;
        if (state->zoom_scale > ZOOM_SCALE_MAX) {
            state->zoom_scale = ZOOM_SCALE_MAX;
        }
        state->config_dirty = true;
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
    // Generator mapping applies only while Test Signal mode is active;
    // outside it, any generator-applied stock config is cleared back to the
    // stock trigger (record/playback never inherit test-mode triggers).
    if (waveform_test_mode_active(app)) {
        waveform_apply_channel_signal_default(state, app, channel);
    } else {
        waveform_clear_generator_signal(state);
    }

    // Sync the global amplitude scale mode into the per-panel mirror so the
    // overlay dropdown (which has no app pointer) can read + display it.
    state->scale_mode = app->settings.waveform_scale_mode;

    // Sync THIS channel's display-container full scale. Playback decode
    // normalizes EVERY input convention (8-bit <<4, 12-bit as-is, 16-bit
    // recorder >>4, direct-domain <=2047 raw, 13-bit >>1, full-range >>4)
    // into the 12-bit container, so playback is always 2048 - the SAME
    // reference as live capture (user rule 2026-11-03: "8/12/16-bit in/out
    // should match"). Capture backends keep their own container scale via
    // the autostop helper (e.g. DdD 512).
    {
        uint32_t fs = gui_app_level_autostop_full_scale(app);
        if (fs < 1) fs = 2048;
        waveform_state_lock(state);
        state->display_full_scale = fs;
        waveform_state_unlock(state);
    }

    // Middle-button double-click: reset the waveform view to the stock
    // position (default zoom, trigger level centered, phosphor cleared).
    // The left-click dispatch never sees the middle button, so this is
    // polled here per frame with a bounds hit test.
    if (IsMouseButtonPressed(MOUSE_MIDDLE_BUTTON) && !gui_popup_is_open()) {
        Vector2 mouse = gui_ui_get_mouse_position();
        if (CheckCollisionPointRec(mouse, bounds)) {
            uint64_t now_ms = get_time_ms();
            bool double_click =
                state->mm_last_click_ms != 0 &&
                now_ms >= state->mm_last_click_ms &&
                now_ms - state->mm_last_click_ms <= 350 &&
                fabsf(mouse.x - state->mm_last_click_x) <= 6.0f &&
                fabsf(mouse.y - state->mm_last_click_y) <= 6.0f;
            state->mm_last_click_ms = double_click ? 0 : now_ms;
            state->mm_last_click_x = mouse.x;
            state->mm_last_click_y = mouse.y;
            if (double_click) {
                waveform_state_lock(state);
                state->zoom_scale = ZOOM_SCALE_DEFAULT;
                state->trigger_level = 0;
                state->trigger_display_pos = -1;
                state->config_dirty = true;
                waveform_state_unlock(state);
                if (state->phosphor) {
                    phosphor_rt_clear(state->phosphor);
                }
            }
        }
    }

    // Update drag state for trigger level (continuous while mouse is held)
    waveform_panel_update_drag(state, app, bounds);

    // DAW-style pause hold (playback only): while THIS channel is held no
    // new display frames arrive, so zoom / trigger-level changes would not
    // render until resume (config is applied per frame in
    // waveform_vtable_process). Re-process the LAST display frame — the
    // trigger source buffers still reference it — whenever the config
    // changed while holding, so zoom, trigger level and trigger mode keep
    // rendering live. Per-channel hold gate (user request 2026-11-03): the
    // per-channel transport pauses a track WITHOUT flipping the master
    // PLAYBACK_STATE, so the old master-state check left zoom dead on a
    // per-channel-paused track. Covers master pause and the no-loop
    // end-of-file hold too; the explicit state check keeps
    // STARTING/STOPPED sessions on the normal path.
    playback_state_t pb_state = gui_playback_get_state(app);
    bool playback_zoom_hold =
        app->selected_device >= 0 &&
        app->selected_device < app->device_count &&
        app->devices[app->selected_device].type == DEVICE_TYPE_PLAYBACK &&
        gui_playback_is_running(app) &&
        (pb_state == PLAYBACK_STATE_PAUSED ||
         pb_state == PLAYBACK_STATE_EOF ||
         (pb_state == PLAYBACK_STATE_PLAYING &&
          atomic_load(&app->playback_channel_paused[channel])));
    if (playback_zoom_hold) {
        bool dirty = false;
        waveform_state_lock(state);
        dirty = state->config_dirty;
        waveform_state_unlock(state);
        if (dirty) {
            const int16_t *src = (channel == 1) ? s_trigger_src_ch2 : s_trigger_src_ch1;
            if (src && s_trigger_src_count > 0) {
                waveform_vtable_process(state, src, s_trigger_src_count,
                                        atomic_load(&app->sample_rate));
            }
        }
    }

    // Dispatch based on render mode
    if (state->render_mode == WAVEFORM_MODE_PHOSPHOR) {
        render_waveform_phosphor_internal(state, app, channel, bounds, channel_color);
    } else {
        render_waveform_line_internal(state, app, channel, bounds, channel_color);
    }

    // Generator option row: rendered AFTER the waveform so the phosphor
    // texture and grid drawing cannot cover it, one row BELOW the
    // Scale/Mode/Trig group, right-aligned at the same margin:
    //     [Signal: Raw/CVBS/FM RF/Audio] [Fmt: VHS] [Gen: PAL/NTSC/SECAM/MESECAM]
    // TEST MODE ONLY (Simulated device active, not recording, no playback
    // session — see waveform_test_mode_active): the generator options never
    // appear on real capture devices or FLAC playback. The Gen button
    // cycles the system (the simulated capture thread regenerates both
    // channels; every Video panel's decoders follow) — the MESECAM step is
    // FM RF signal panels only, matching the composite reality that a
    // MESECAM generator's CVBS output is plain SECAM. The Fmt button
    // cycles the tape format driving the RF parameters — it renders on FM
    // RF signal panels ONLY: a tape format does not apply to a composite
    // or audio signal, and the button follows the panel's Signal pick
    // rather than the CH1/CH2 channel mapping, so the default CVBS panel
    // shows just [Signal][Gen] (user request 2026-11-03). The Signal
    // button assigns this channel's signal type (label + trigger defaults).
    {
        if (waveform_test_mode_active(app) &&
            !gui_chrome_hide_hidden(&state->chrome_hide)) {
            // Text measurement contract: gui_text_measure is in the draw
            // space (logical px; see the grid-render note above) - the
            // buttons scale with the UI zoom through the render transform
            // without any conversion (the 1/scale division shrank buttons
            // under full-size text -> overlapping controls at >100%).
            const float to_logical_w = 1.0f;
            const float btn_h = 18.0f;
            const float row_y = bounds.y + 8.0f + btn_h + 6.0f;
            const float x_right = bounds.x + bounds.width - 8.0f;

            // Gen system button (rightmost on the row). MESECAM is RF-only:
            // display the composite-equivalent system (SECAM) on non-FM-RF
            // panels so MESECAM never appears in a CVBS/Audio context.
            const char *sys_labels[] = {"PAL", "NTSC", "SECAM", "MESECAM"};
            int sys = gui_testgen_clamp_system(app->settings.video_system);
            if (sys == 3 && state->signal_type != WAVE_SIGNAL_FM_RF) sys = 2;
            char gen_label[24];
            snprintf(gen_label, sizeof(gen_label), "Gen: %s", sys_labels[sys]);
            float gen_w = (float)gui_text_measure(gen_label, FONT_SIZE_DROPDOWN_OPT) * to_logical_w + 16.0f;
            if (gen_w < 90.0f * to_logical_w) gen_w = 90.0f * to_logical_w;
            float gen_x = x_right - gen_w;
            state->sim_sys_btn_rect = (Rectangle){gen_x, row_y, gen_w, btn_h};
            DrawRectangleRounded(state->sim_sys_btn_rect, 0.15f, 4, COLOR_BUTTON_ACTIVE);
            gui_text_draw(gen_label, gen_x + 8.0f,
                          row_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f,
                          FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

            // Tape-format selector (between Signal and Gen) — FM RF signal
            // panels only: a tape format parameterizes the RF modulation,
            // so CVBS/Audio panels skip the button entirely (rect zeroed,
            // keeping its click handler inert) and Signal anchors directly
            // next to Gen.
            bool show_fmt = (state->signal_type == WAVE_SIGNAL_FM_RF);
            float fmt_x = gen_x;
            if (show_fmt) {
                int fmt = gui_testgen_clamp_format(app->settings.gen_tape_format);
                char fmt_label[32];
                snprintf(fmt_label, sizeof(fmt_label), "Fmt: %s",
                         gui_testgen_format_name((gui_testgen_format_t)fmt));
                float fmt_w = (float)gui_text_measure(fmt_label, FONT_SIZE_DROPDOWN_OPT) * to_logical_w + 16.0f;
                if (fmt_w < 96.0f * to_logical_w) fmt_w = 96.0f * to_logical_w;
                fmt_x = gen_x - 8.0f - fmt_w;
                if (fmt_x < bounds.x + 8.0f) fmt_x = bounds.x + 8.0f;
                state->sim_fmt_btn_rect = (Rectangle){fmt_x, row_y, fmt_w, btn_h};
                DrawRectangleRounded(state->sim_fmt_btn_rect, 0.15f, 4, COLOR_BUTTON_ACTIVE);
                gui_text_draw(fmt_label, fmt_x + 8.0f,
                              row_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f,
                              FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
            } else {
                state->sim_fmt_btn_rect = (Rectangle){0};
            }

            // Signal type dropdown (leftmost on the row)
            char signal_label[40];
            snprintf(signal_label, sizeof(signal_label), "Signal: %s",
                     s_signal_labels[state->signal_type]);
            float sig_w = (float)gui_text_measure(signal_label, FONT_SIZE_DROPDOWN_OPT) * to_logical_w + 28.0f;
            if (sig_w < 110.0f * to_logical_w) sig_w = 110.0f * to_logical_w;
            float sig_x = fmt_x - 8.0f - sig_w;
            if (sig_x < bounds.x + 8.0f) sig_x = bounds.x + 8.0f;
            state->signal_btn_rect = (Rectangle){sig_x, row_y, sig_w, btn_h};
            draw_dropdown_button(state->signal_btn_rect, signal_label,
                                 state->signal_dropdown_open);

            // Signal dropdown options (below the button, clamped in-bounds).
            // Lists CVBS / FM RF / Audio — Raw is not selectable.
            if (state->signal_dropdown_open) {
                const float opt_h = 20.0f;
                float opt_y = fmaxf(bounds.y,
                    fminf(row_y + btn_h,
                          bounds.y + bounds.height -
                              opt_h * WAVE_SIGNAL_OPTION_COUNT - 4.0f));
                DrawRectangleRounded(
                    (Rectangle){sig_x, opt_y, sig_w,
                                opt_h * WAVE_SIGNAL_OPTION_COUNT},
                    0.1f, 4, COLOR_PANEL_BG);
                Vector2 mouse = gui_ui_get_mouse_position();
                for (int i = 0; i < WAVE_SIGNAL_OPTION_COUNT; i++) {
                    waveform_signal_type_t value = WAVE_SIGNAL_OPTION_VALUE(i);
                    Rectangle opt_rect = {sig_x, opt_y + i * opt_h, sig_w, opt_h};
                    state->signal_opts_rect[i] = opt_rect;
                    bool is_selected = (state->signal_type == value);
                    bool hover = CheckCollisionPointRec(mouse, opt_rect);
                    DrawRectangleRec(opt_rect, gui_dropdown_option_color(is_selected, hover));
                    int opt_w = (int)(gui_text_measure(s_signal_labels[value],
                                                 FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
                    gui_text_draw(s_signal_labels[value],
                                  sig_x + sig_w / 2.0f - opt_w / 2.0f,
                                  opt_y + i * opt_h +
                                      (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                                  FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
                }
            }
        } else {
            state->sim_sys_btn_rect = (Rectangle){0};
            state->sim_fmt_btn_rect = (Rectangle){0};
            state->signal_btn_rect = (Rectangle){0};
            // No stale open popup once the generator row is hidden.
            state->signal_dropdown_open = false;
        }
    }

    // Playback render-resolution row: [Res: 1:1|1:2|1:4]. Same second-row
    // slot as the test-mode Gen row (mutually exclusive). PLAYER ONLY: the
    // output decimation of the visualised waveform (1:1 = full resolution);
    // every other source zeroes the rect (click-inert).
    {
        if (waveform_playback_res_visible(app) &&
            !gui_chrome_hide_hidden(&state->chrome_hide)) {
            // Text measurement contract: logical px, no conversion (see the
            // generator-row note above).
            const float to_logical_w = 1.0f;
            const float btn_h = 18.0f;
            const float row_y = bounds.y + 8.0f + btn_h + 6.0f;
            const float x_right = bounds.x + bounds.width - 8.0f;
            Vector2 mouse = gui_ui_get_mouse_position();

            // Res button (rightmost on the row)
            char res_label[24];
            snprintf(res_label, sizeof(res_label), "Res: %s",
                     waveform_res_points_label(state->res_points));
            float res_w = (float)gui_text_measure(res_label, FONT_SIZE_DROPDOWN_OPT) * to_logical_w + 24.0f;
            if (res_w < 90.0f * to_logical_w) res_w = 90.0f * to_logical_w;
            float res_x = x_right - res_w;
            state->res_btn_rect = (Rectangle){res_x, row_y, res_w, btn_h};
            draw_dropdown_button(state->res_btn_rect, res_label,
                                 state->res_dropdown_open);

            // Res options: Auto + the active range group's ladder.
            if (state->res_dropdown_open) {
                const waveform_res_points_t *group =
                    (state->res_range == WAVEFORM_RES_RANGE_FINE) ? s_res_fine : s_res_coarse;
                size_t group_n = (state->res_range == WAVEFORM_RES_RANGE_FINE)
                    ? sizeof(s_res_fine) / sizeof(s_res_fine[0])
                    : sizeof(s_res_coarse) / sizeof(s_res_coarse[0]);
                size_t rows = group_n + 1;  // +1 for Auto
                const float opt_h = 20.0f;
                float opt_y = fmaxf(bounds.y,
                    fminf(row_y + btn_h,
                          bounds.y + bounds.height - opt_h * (float)rows - 4.0f));
                DrawRectangleRounded(
                    (Rectangle){res_x, opt_y, res_w, opt_h * (float)rows},
                    0.1f, 4, COLOR_PANEL_BG);
                // Row 0: Auto (stock one-point-per-pixel)
                Rectangle auto_rect = {res_x, opt_y, res_w, opt_h};
                state->res_opts_rect[0] = auto_rect;
                bool auto_sel = (state->res_points == WAVEFORM_RES_AUTO);
                bool auto_hover = CheckCollisionPointRec(mouse, auto_rect);
                DrawRectangleRec(auto_rect, gui_dropdown_option_color(auto_sel, auto_hover));
                int a_w = (int)(gui_text_measure("Auto", FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
                gui_text_draw("Auto", res_x + res_w / 2.0f - a_w / 2.0f,
                              opt_y + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                              FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
                // Rows 1..N: the group's preset point counts
                for (size_t i = 0; i < group_n; i++) {
                    Rectangle opt_rect = {res_x, opt_y + (float)(i + 1) * opt_h, res_w, opt_h};
                    state->res_opts_rect[i + 1] = opt_rect;
                    bool is_selected = (state->res_points == (int)group[i]);
                    bool hover = CheckCollisionPointRec(mouse, opt_rect);
                    DrawRectangleRec(opt_rect, gui_dropdown_option_color(is_selected, hover));
                    const char *lab = waveform_res_points_label((int)group[i]);
                    int o_w = (int)(gui_text_measure(lab, FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
                    gui_text_draw(lab, res_x + res_w / 2.0f - o_w / 2.0f,
                                  opt_y + (float)(i + 1) * opt_h +
                                      (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                                  FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
                }
            }

            // Range button (left of Res): selects the Res preset group
            const char *range_label_buf = s_res_range_labels[
                (state->res_range >= 0 && state->res_range < WAVEFORM_RES_RANGE_COUNT)
                    ? state->res_range : 0];
            char range_label[32];
            snprintf(range_label, sizeof(range_label), "Range: %s", range_label_buf);
            float range_w = (float)gui_text_measure(range_label, FONT_SIZE_DROPDOWN_OPT) * to_logical_w + 24.0f;
            if (range_w < 110.0f * to_logical_w) range_w = 110.0f * to_logical_w;
            float range_x = res_x - 8.0f - range_w;
            if (range_x < bounds.x + 8.0f) range_x = bounds.x + 8.0f;
            state->range_btn_rect = (Rectangle){range_x, row_y, range_w, btn_h};
            draw_dropdown_button(state->range_btn_rect, range_label,
                                 state->range_dropdown_open);

            // Range options: Coarse / Fine
            if (state->range_dropdown_open) {
                const float opt_h = 20.0f;
                float opt_y = fmaxf(bounds.y,
                    fminf(row_y + btn_h,
                          bounds.y + bounds.height - opt_h * WAVEFORM_RES_RANGE_COUNT - 4.0f));
                DrawRectangleRounded(
                    (Rectangle){range_x, opt_y, range_w,
                                opt_h * WAVEFORM_RES_RANGE_COUNT},
                    0.1f, 4, COLOR_PANEL_BG);
                for (int i = 0; i < WAVEFORM_RES_RANGE_COUNT; i++) {
                    Rectangle opt_rect = {range_x, opt_y + i * opt_h, range_w, opt_h};
                    state->range_opts_rect[i] = opt_rect;
                    bool is_selected = (state->res_range == i);
                    bool hover = CheckCollisionPointRec(mouse, opt_rect);
                    DrawRectangleRec(opt_rect, gui_dropdown_option_color(is_selected, hover));
                    int o_w = (int)(gui_text_measure(s_res_range_labels[i],
                                               FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
                    gui_text_draw(s_res_range_labels[i],
                                  range_x + range_w / 2.0f - o_w / 2.0f,
                                  opt_y + i * opt_h +
                                      (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                                  FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
                }
            }
        } else {
            // Not playback (or chrome hidden): zero the rects so the
            // click handler ignores them, and collapse any stale popup.
            state->res_btn_rect = (Rectangle){0};
            state->range_btn_rect = (Rectangle){0};
            state->res_dropdown_open = false;
            state->range_dropdown_open = false;
        }
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

//-----------------------------------------------------------------------------
// Generator Default Channel Mapping
//-----------------------------------------------------------------------------

// Default the waveform monitors to the Test Gen channel mapping (CH1 = CVBS
// on channel A, CH2 = FM RF on channel B) when the Simulated device starts.
// Only waveform panels still in Raw state are assigned, so a user selection
// is never overwritten. Called from gui_app_start_capture after
// gui_simulated_start() succeeds (UI thread or the capture-start worker);
// takes the panel config lock to serialize against the display thread and
// view switching (same lock panel_registry.c uses for panel processing).
void gui_waveform_apply_generator_defaults(gui_app_t *app) {
    if (!app) return;

    while (atomic_flag_test_and_set(&app->panel_config_lock)) {
        /* spin */
    }

    const waveform_signal_type_t want[2] = { WAVE_SIGNAL_CVBS, WAVE_SIGNAL_FM_RF };
    channel_panel_config_t *configs[2] = { &app->panel_config_a, &app->panel_config_b };

    for (int ch = 0; ch < 2; ch++) {
        channel_panel_config_t *cfg = configs[ch];

        if (cfg->left_view == PANEL_VIEW_WAVEFORM && cfg->left_state) {
            waveform_panel_state_t *st = (waveform_panel_state_t *)cfg->left_state;
            if (st->initialized && st->signal_type == WAVE_SIGNAL_RAW) {
                waveform_apply_signal_defaults(st, want[ch]);
            }
        }

        if (cfg->split && cfg->right_view == PANEL_VIEW_WAVEFORM && cfg->right_state) {
            waveform_panel_state_t *st = (waveform_panel_state_t *)cfg->right_state;
            if (st->initialized && st->signal_type == WAVE_SIGNAL_RAW) {
                waveform_apply_signal_defaults(st, want[ch]);
            }
        }
    }

    atomic_flag_clear(&app->panel_config_lock);
}

/* v5: server uses EXACT Local waveform processing (including SOXR + trigger),
 * but produces normalized display samples without creating a graphics window.
 * HTTP request threads serialize this CPU-only renderer because the Local
 * resampler uses static temporary scratch buffers. */
static atomic_flag s_rd_wave_lock=ATOMIC_FLAG_INIT;
size_t gui_waveform_server_display_points(const int16_t *src,const int16_t *a,
        const int16_t *b,size_t count,uint32_t rate,const gui_rd_config_t *cfg,
        float out[GUI_RD_WIDTH],int32_t *trigger_pos){
    if(!src||!out||!cfg||!trigger_pos||!count)return 0;
    while(atomic_flag_test_and_set_explicit(&s_rd_wave_lock,memory_order_acquire))thrd_sleep_ms(1);
    /* Keep resamplers cached across frames; no GPU resources used. */
    static waveform_panel_state_t states[2];
    static bool ready[2]={false,false};
    int ch=(src==b)?1:0;
    waveform_panel_state_t *st=&states[ch];
    if(!ready[ch]){
        atomic_flag_clear(&st->data_lock);
        atomic_init(&st->display_width,GUI_RD_WIDTH);
        ready[ch]=true;
    }
    atomic_store(&st->display_width,(int)cfg->width);
    waveform_runtime_cfg_t local={
        .zoom_scale=cfg->zoom,
        .trigger_enabled=cfg->trigger_enabled!=0,
        .trigger_level=(int16_t)cfg->trigger_level,
        .trigger_mode=(trigger_mode_t)cfg->trigger_mode,
        .trigger_source=(trigger_source_t)cfg->trigger_source,
        .full_scale=2048,
        .res_points=WAVEFORM_RES_AUTO,
    };
    waveform_sample_t temp[GUI_RD_WIDTH];size_t n=0;int tr=-1;
    gui_waveform_set_trigger_source_buffers(a,b,count);
    bool ok=waveform_process_display(st,&local,src,count,rate,temp,&n,&tr);
    gui_waveform_set_trigger_source_buffers(NULL,NULL,0);
    if(ok){for(size_t i=0;i<n;i++)out[i]=temp[i].value;}
    *trigger_pos=ok?tr:-1;
    atomic_flag_clear_explicit(&s_rd_wave_lock,memory_order_release);
    return ok?n:0;
}
void gui_waveform_remote_apply_points(void *state_ptr,const gui_rd_channel_t *ch,uint32_t sequence){
    if(!state_ptr||!ch)return;
    waveform_panel_state_t *state=state_ptr;
    if(state->remote_last_sequence==sequence || ch->count==0)return;
    waveform_state_lock(state);
    for(size_t i=0;i<ch->count;i++)state->display_samples[i].value=ch->waveform[i];
    state->display_samples_available=ch->count;
    state->trigger_display_pos=ch->trigger_pos;
    state->remote_last_sequence=sequence;
    state->config_dirty=false;
    waveform_state_unlock(state);
}
void gui_waveform_remote_publish_config(void *state_ptr,int ch){
    if(!state_ptr)return;
    waveform_panel_state_t *state=state_ptr;
    gui_rd_config_t cfg;
    waveform_state_lock(state);
    cfg.zoom=state->zoom_scale;
    cfg.width=(uint32_t)atomic_load(&state->display_width);
    cfg.trigger_enabled=state->trigger_enabled;
    cfg.trigger_level=state->trigger_level;
    cfg.trigger_mode=state->trigger_mode;
    cfg.trigger_source=state->trigger_source;
    waveform_state_unlock(state);
    gui_rd_publish_request(ch,cfg);
}
