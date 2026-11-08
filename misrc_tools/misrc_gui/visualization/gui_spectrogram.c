/*
 * MISRC GUI - Real-time Spectrogram / Waterfall panel implementation
 *
 * Shared engine for the Waterfall (PANEL_VIEW_WATERFALL) and Spectrograph
 * (PANEL_VIEW_SPECTROGRAPH) panels. See gui_spectrogram.h for the high-level
 * description and threading model.
 *
 * Rendering strategy:
 *   A CPU RGBA8 history buffer is kept in "display order" (the row/column that
 *   represents the newest time slice is always at a fixed screen edge). Each
 *   render frame we shift the history by one slice, write the latest FFT
 *   magnitude frame (resampled to SPEC_FREQ_BINS, mapped through a 256-entry
 *   heatmap LUT that replicates the GPU phosphor intensityToHeatmap ramp) into
 *   the freed slot, upload the whole buffer to a Texture2D, and draw it with
 *   DrawTexturePro using a negative source height so buffer row 0 maps to the
 *   top of the screen.
 *
 *   Waterfall:    freq on X (tex_w = SPEC_FREQ_BINS), time on Y scrolling down,
 *                 newest written at buffer row 0 (drawn at the top).
 *   Spectrograph: time on X scrolling left (tex_w = SPEC_TIME_BINS), freq on Y,
 *                 newest written at buffer col (tex_w-1) (drawn at the right),
 *                 high freq at buffer row 0 (drawn at the top).
 */

#include "gui_spectrogram.h"
#include "gui_fft.h"
#include "gui_text.h"
#include "../core/gui_app.h"
#include "../input/gui_playback.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_dropdown.h"  // gui_dropdown_option_color for the palette dropdown
#include "../ui/gui_chrome_hide.h"  // double-click overlay toggle (shared helper)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// Configuration (tunable; lowering these reduces per-frame upload bandwidth)
//-----------------------------------------------------------------------------

#define SPEC_FREQ_BINS   1024   // Fixed frequency resolution of the history texture
#define SPEC_TIME_BINS   512    // History depth (time slices); keep in sync with
                                // PLAYBACK_ANALYSIS_SLICES (gui_playback.h) — the
                                // paused analysis window renders one column per slice

// DAW-style paused analysis window: while playback is PAUSED the panel renders
// a duration-based window of the FILE around the playhead (gui_playback.c
// analysis decoder — async, direct random access) across the full history
// width, instead of the rolling live feed (which is 1:1-correct for playing
// but only carries the last 65,536-sample display frames). Wheel over the
// panel while paused adjusts the span (1 s .. 60 s).
#define SPEC_PAUSED_WINDOW_SECONDS 3.0

#define SPEC_LABEL_H     16     // Waterfall: bottom strip reserved for freq labels (px)
#define SPEC_LABEL_W     34     // Spectrograph: left strip reserved for freq labels (px)

#define SPEC_AXIS_DIVS   8      // Number of frequency grid divisions

//-----------------------------------------------------------------------------
// Engine State
//-----------------------------------------------------------------------------

typedef struct spectro_engine {
    fft_state_t fft;                 // Reusable FFT engine (display thread writes back buffer)

    bool time_on_x;                  // false = Waterfall (freq X, time Y), true = Spectrograph (time X, freq Y)
    int  freq_bins;                  // == SPEC_FREQ_BINS (frequency axis length in the texture)
    int  time_bins;                  // == SPEC_TIME_BINS (time axis length in the texture)

    int  tex_w;                      // Texture width  (depends on orientation)
    int  tex_h;                      // Texture height (depends on orientation)

    unsigned char *pixels;           // CPU RGBA8 history buffer: tex_w * tex_h * 4 bytes
    float         *frame;            // Latest resampled magnitude frame (freq_bins floats, render thread)

    unsigned char  lut_phosphor[256][4];  // Phosphor LUT (matches phosphor intensityToHeatmap)
    unsigned char  lut_heat[256][4];      // Inferno-style red/purple contrasting heat LUT

    Texture2D tex;                   // GPU texture (created lazily in render thread)
    bool tex_valid;
    bool needs_full_upload;          // True after clear/init -> upload whole texture once

    bool frame_valid;                // True once at least one FFT frame has been consumed
    uint32_t last_sample_rate;       // Sample rate snapshot for axis labels
    // Playback non-linear-move detection: a rolling history is only valid
    // while the playhead advances linearly. On a seek (gui_playback.c bumps a
    // per-channel seek sequence) the history is cleared and refills with the
    // real timeline at the new playhead. (A fixed sample-count threshold
    // CANNOT detect jumps: the per-slice playhead advance scales with the
    // playback speed and the file's sample rate — a 3-frame threshold even
    // cleared the history on every tick during 1x playback.)
    uint64_t last_seek_seq;          // Channel seek sequence at the last append
    bool last_seek_seq_valid;

    // DAW-style paused analysis window (playback): rendered directly from
    // the file via the gui_playback analysis decoder.
    bool analysis_active;           // Paused-window mode engaged this session
    bool analysis_requested;        // A window request is in flight
    bool analysis_rerequest;         // Span/palette changed; re-request on next render
    uint64_t analysis_playhead;      // Playhead sample of the last request
    double analysis_span_seconds;    // Requested span (wheel adjustable)
    uint64_t analysis_seq;          // Last consumed analysis result seq

    // Palette dropdown (top-right): Phosphor / Heat. Changing the palette
    // reloads the view (paused: instant re-map of the last result + re-request;
    // rolling: history clears and refills in the new palette).
    Rectangle palette_btn_rect;
    Rectangle palette_opts_rect[2];
    bool palette_dropdown_open;

    // Double-click chrome-hide tracker (see ui/gui_chrome_hide.h): toggles
    // ALL overlay chrome (palette dropdown, axis labels, PAUSED indicator);
    // the spectrogram texture, gridlines and "now" marker keep rendering.
    gui_chrome_hide_t chrome_hide;
} spectro_engine_t;

//-----------------------------------------------------------------------------
// Availability
//-----------------------------------------------------------------------------

bool gui_spectrogram_available(void) {
    return gui_fft_available();
}

//-----------------------------------------------------------------------------
// Heatmap LUT (replicates gui_phosphor_rt.c intensityToHeatmap exactly so the
// colors match the existing FFT/oscilloscope phosphor display)
//-----------------------------------------------------------------------------

static void build_lut(unsigned char lut[256][4]) {
    for (int i = 0; i < 256; i++) {
        float intensity = (float)i / 255.0f;
        float r, g, b;
        if (intensity < 0.25f) {
            float t = intensity / 0.25f;
            r = 0.0f; g = 0.078f * t; b = 0.392f + 0.608f * t;
        } else if (intensity < 0.5f) {
            float t = (intensity - 0.25f) / 0.25f;
            r = 0.0f; g = 0.078f + 0.922f * t; b = 1.0f - 0.784f * t;
        } else if (intensity < 0.75f) {
            float t = (intensity - 0.5f) / 0.25f;
            r = t; g = 1.0f; b = 0.216f - 0.216f * t;
        } else {
            float t = (intensity - 0.75f) / 0.25f;
            r = 1.0f; g = 1.0f - 0.706f * t; b = 0.0f;
        }
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        if (b < 0.0f) b = 0.0f;
        if (b > 1.0f) b = 1.0f;
        lut[i][0] = (unsigned char)(r * 255.0f + 0.5f);
        lut[i][1] = (unsigned char)(g * 255.0f + 0.5f);
        lut[i][2] = (unsigned char)(b * 255.0f + 0.5f);
        lut[i][3] = 255;
    }
}

//-----------------------------------------------------------------------------
// Heat map palettes (click the panel to toggle; shared by all spectro panels)
//-----------------------------------------------------------------------------

typedef enum {
    SPEC_PALETTE_PHOSPHOR = 0,   // Original blue->cyan->green->yellow->red ramp
    SPEC_PALETTE_HEAT               // Inferno-style red/purple contrasting heat map
} spec_palette_t;

static int s_spec_palette = SPEC_PALETTE_PHOSPHOR;

// Inferno-style red/purple contrasting heat map (matplotlib "inferno"
// control points): near-black -> deep purple -> magenta/red -> orange ->
// pale yellow. The standard spectrogram palette in SDR/analysis tools.
static void build_heat_lut(unsigned char lut[256][4]) {
    static const float stops[][4] = {
        { 0.00f,   0.0f,   0.0f,   4.0f },
        { 0.13f,  31.0f,  12.0f,  72.0f },
        { 0.25f,  85.0f,  15.0f, 109.0f },
        { 0.38f, 136.0f,  34.0f, 106.0f },
        { 0.50f, 186.0f,  54.0f,  85.0f },
        { 0.63f, 227.0f,  89.0f,  51.0f },
        { 0.75f, 249.0f, 140.0f,  10.0f },
        { 0.88f, 249.0f, 201.0f,  50.0f },
        { 1.00f, 252.0f, 255.0f, 164.0f },
    };
    const int n = (int)(sizeof(stops) / sizeof(stops[0]));
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.0f;
        int s = 0;
        while (s + 1 < n - 1 && t > stops[s + 1][0]) s++;
        float f = (t - stops[s][0]) / (stops[s + 1][0] - stops[s][0]);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        for (int c = 0; c < 3; c++) {
            float v = stops[s][1 + c] + f * (stops[s + 1][1 + c] - stops[s][1 + c]);
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            lut[i][c] = (unsigned char)(v + 0.5f);
        }
        lut[i][3] = 255;
    }
}

static inline const unsigned char *lut_lookup(spectro_engine_t *e, float m) {
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    int idx = (int)(m * 255.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    return (s_spec_palette == SPEC_PALETTE_HEAT) ? e->lut_heat[idx]
                                                 : e->lut_phosphor[idx];
}

//-----------------------------------------------------------------------------
// Resample (linear) the dynamic fft_bins magnitude frame into freq_bins slots
//-----------------------------------------------------------------------------

static void resample_magnitude(const float *src, int n_src, float *dst, int n_dst) {
    if (!dst || n_dst <= 0) return;
    if (!src || n_src <= 0) {
        for (int i = 0; i < n_dst; i++) dst[i] = 0.0f;
        return;
    }
    if (n_src == 1) {
        for (int i = 0; i < n_dst; i++) dst[i] = src[0];
        return;
    }
    if (n_dst == 1) {
        dst[0] = src[n_src / 2];
        return;
    }
    for (int i = 0; i < n_dst; i++) {
        float pos = (float)i * (float)(n_src - 1) / (float)(n_dst - 1);
        if (pos < 0.0f) pos = 0.0f;
        int i0 = (int)pos;
        if (i0 >= n_src - 1) { dst[i] = src[n_src - 1]; continue; }
        float frac = pos - (float)i0;
        dst[i] = src[i0] * (1.0f - frac) + src[i0 + 1] * frac;
    }
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

static void *spectro_create(bool time_on_x) {
    if (!gui_spectrogram_available()) return NULL;

    spectro_engine_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->time_on_x = time_on_x;
    e->freq_bins = SPEC_FREQ_BINS;
    e->time_bins = SPEC_TIME_BINS;
    e->tex_w = time_on_x ? e->time_bins : e->freq_bins;
    e->tex_h = time_on_x ? e->freq_bins : e->time_bins;

    e->pixels = calloc((size_t)e->tex_w * e->tex_h, 4);
    e->frame  = calloc((size_t)e->freq_bins, sizeof(float));
    if (!e->pixels || !e->frame) {
        free(e->pixels);
        free(e->frame);
        free(e);
        return NULL;
    }

    build_lut(e->lut_phosphor);
    build_heat_lut(e->lut_heat);

    if (!gui_fft_init(&e->fft)) {
        free(e->pixels);
        free(e->frame);
        free(e);
        return NULL;
    }

    e->tex_valid = false;
    e->needs_full_upload = true;
    e->frame_valid = false;
    e->last_sample_rate = 0;
    return e;
}

static void *waterfall_vtable_create(void)    { return spectro_create(false); }
static void *spectrograph_vtable_create(void) { return spectro_create(true); }

static void spectro_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    if (e->tex_valid) {
        UnloadTexture(e->tex);
        e->tex_valid = false;
    }
    gui_fft_cleanup(&e->fft);
    free(e->pixels);
    free(e->frame);
    free(e);
}

static void spectro_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    if (e->pixels) memset(e->pixels, 0, (size_t)e->tex_w * e->tex_h * 4);
    e->needs_full_upload = true;
    e->frame_valid = false;
    // gui_fft_clear() only touches CPU buffers + the unused phosphor state
    // (phosphor render textures are never created for this panel, so the
    // phosphor clear is a no-op and no GL calls occur here).
    gui_fft_clear(&e->fft);
}

//-----------------------------------------------------------------------------
// Processing (display thread, no GL)
//-----------------------------------------------------------------------------

static void spectro_vtable_process(void *state_ptr, const int16_t *samples,
                                    size_t count, uint32_t sample_rate) {
    if (!state_ptr || !samples || count == 0) return;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    // Compute FFT into the embedded back buffer and set magnitude_ready.
    gui_fft_process_raw(&e->fft, samples, count, sample_rate);
}

//-----------------------------------------------------------------------------
// History append (render thread)
//-----------------------------------------------------------------------------

static inline void put_pixel(spectro_engine_t *e, int col, int row, const unsigned char *c) {
    unsigned char *p = &e->pixels[((size_t)row * e->tex_w + col) * 4];
    p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
}

static void spectro_append_frame(spectro_engine_t *e) {
    if (!e->frame_valid) return;

    if (!e->time_on_x) {
        // Waterfall: time on Y (rows), freq on X (cols). Newest at row 0 (top).
        // Shift rows down by one (discard oldest at bottom).
        memmove(e->pixels + (size_t)e->tex_w * 4,
                e->pixels,
                (size_t)(e->tex_h - 1) * e->tex_w * 4);
        // Write new frame as row 0.
        unsigned char *row0 = e->pixels;
        for (int f = 0; f < e->freq_bins; f++) {
            const unsigned char *c = lut_lookup(e, e->frame[f]);
            unsigned char *p = &row0[(size_t)f * 4];
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
        }
    } else {
        // Spectrograph: time on X (cols), freq on Y (rows). Newest at col (tex_w-1) (right).
        // High freq at row 0 (top), low freq at row (tex_h-1) (bottom).
        // Shift columns left by one (discard oldest at left), per row.
        for (int r = 0; r < e->tex_h; r++) {
            unsigned char *row = &e->pixels[(size_t)r * e->tex_w * 4];
            memmove(row, row + 4, (size_t)(e->tex_w - 1) * 4);
        }
        // Write new frame as the rightmost column.
        int col = e->tex_w - 1;
        for (int f = 0; f < e->freq_bins; f++) {
            int row = e->freq_bins - 1 - f;   // f = freq index (0 = DC/low). high freq -> row 0 (top)
            const unsigned char *c = lut_lookup(e, e->frame[f]);
            put_pixel(e, col, row, c);
        }
    }
}

//-----------------------------------------------------------------------------
// Paused analysis window: write one slice at a fixed history index
//-----------------------------------------------------------------------------

// Write the current e->frame as time slice `index` (0 = window start).
// Spectrograph: slice 0 at the LEFT edge, newest (window end) at the right.
// Waterfall: slice 0 at the BOTTOM, newest at the top (matches live flow).
static void spectro_write_slice_index(spectro_engine_t *e, int index) {
    if (!e || !e->frame_valid || index < 0) return;

    if (!e->time_on_x) {
        // Waterfall: freq on X (cols), time on Y (rows). Oldest at bottom.
        int row = e->time_bins - 1 - index;
        if (row < 0 || row >= e->tex_h) return;
        for (int f = 0; f < e->freq_bins && f < e->tex_w; f++) {
            const unsigned char *c = lut_lookup(e, e->frame[f]);
            unsigned char *p = &e->pixels[((size_t)row * e->tex_w + (size_t)f) * 4];
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
        }
    } else {
        // Spectrograph: time on X (cols), freq on Y (rows). Oldest at left.
        int col = index;
        if (col >= e->tex_w) return;
        for (int f = 0; f < e->freq_bins; f++) {
            int row = e->freq_bins - 1 - f;   // high freq -> row 0 (top)
            if (row < 0 || row >= e->tex_h) continue;
            const unsigned char *c = lut_lookup(e, e->frame[f]);
            put_pixel(e, col, row, c);
        }
    }
}

// Request/poll/render the paused analysis window into the history texture.
// Fills the FULL history width with the window's slices (async: keeps the
// last completed window until the worker publishes the new one).
static void spectro_update_paused_window(spectro_engine_t *e, gui_app_t *app, int channel) {
    uint64_t playhead = gui_playback_get_position_samples_channel(app, channel);
    if (!e->analysis_requested || e->analysis_rerequest ||
        playhead != e->analysis_playhead) {
        gui_playback_request_analysis_window(app, channel, playhead,
                                              e->analysis_span_seconds);
        e->analysis_requested = true;
        e->analysis_rerequest = false;
        e->analysis_playhead = playhead;
    }

    const float *mags = NULL;
    size_t slice_count = 0;
    size_t bins = 0;
    if (gui_playback_poll_analysis_window(app, channel, &e->analysis_seq,
                                           &mags, &slice_count, &bins) &&
        mags && bins > 0) {
        // The worker already FFT'd the full contiguous columns (gui_playback.c):
        // the render only resamples each magnitude frame to the panel's
        // frequency resolution and writes the column — no render-thread
        // FFTs (512 columns render in ~ms, no frame stalls).
        size_t limit = (slice_count < (size_t)e->time_bins)
            ? slice_count : (size_t)e->time_bins;
        for (size_t i = 0; i < limit; i++) {
            resample_magnitude(mags + i * bins, (int)bins, e->frame, e->freq_bins);
            e->frame_valid = true;
            spectro_write_slice_index(e, (int)i);
        }
    }
}

// Reload the view after a palette change (same spectra, new LUT):
// - Paused analysis: instantly re-map the current publication through the
//   new LUT AND re-request the window (guarantees a fully-correct render
//   even if a new publish raced the snapshot).
// - Rolling: the history pixels are already LUT-mapped, so re-rendering is
//   not possible — clear it and let it refill in the new palette.
static void spectro_palette_reload(spectro_engine_t *e, gui_app_t *app, int channel) {
    if (!e) return;
    const float *mags = NULL;
    size_t slice_count = 0;
    size_t bins = 0;
    uint64_t seq = 0;
    // Fetch only the current session's publication. The playback-owned
    // buffers are released on stop, so never retain their aliases across
    // frames for a later palette click.
    bool remap = e->analysis_active &&
        gui_playback_is_running(app) &&
        gui_playback_is_channel_paused(app, channel) &&
        gui_playback_poll_analysis_window(app, channel, &seq,
                                          &mags, &slice_count, &bins) &&
        mags && bins > 0;
    if (remap) {
        size_t limit = (slice_count < (size_t)e->time_bins)
            ? slice_count : (size_t)e->time_bins;
        for (size_t i = 0; i < limit; i++) {
            resample_magnitude(mags + i * bins, (int)bins, e->frame, e->freq_bins);
            e->frame_valid = true;
            spectro_write_slice_index(e, (int)i);
        }
    } else if (e->pixels) {
        memset(e->pixels, 0, (size_t)e->tex_w * e->tex_h * 4);
        e->frame_valid = false;
    }
    if (e->analysis_active) e->analysis_rerequest = true;
}

//-----------------------------------------------------------------------------
// Overlay: frequency axis grid + labels + "now" edge marker
//-----------------------------------------------------------------------------

static void format_freq(char *buf, size_t bufsz, double hz) {
    if (hz >= 1.0e6)      snprintf(buf, bufsz, "%.1fM", hz / 1.0e6);
    else if (hz >= 1.0e3) snprintf(buf, bufsz, "%.0fk", hz / 1.0e3);
    else                  snprintf(buf, bufsz, "%.0f", hz);
}

// DAW-style absolute timecode: h:mm:ss.s / m:ss.s
static void format_abs_timecode(char *buf, size_t bufsz, double secs) {
    if (!(secs >= 0.0) || !isfinite(secs)) secs = 0.0;
    int h = (int)(secs / 3600.0);
    int m = (int)(secs / 60.0) % 60;
    double s = secs - (double)(h * 3600 + m * 60);
    if (h > 0) snprintf(buf, bufsz, "%d:%02d:%04.1f", h, m, s);
    else       snprintf(buf, bufsz, "%d:%04.1f", m, s);
}

static void spectro_draw_overlay(spectro_engine_t *e, double real_rate_hz,
                                 Rectangle bounds, Rectangle plot) {
    (void)plot;  // Labels are drawn inside bounds, so plot == bounds.
    uint32_t sr = e->last_sample_rate;
    double nyq = (double)sr * 0.5;
    if (nyq <= 0.0) nyq = 1.0;   // before first data, avoid divide-by-zero / nonsense labels
    // Axis labels are overlay chrome: hidden while the panel's chrome is
    // toggled off (gridlines, "now" marker and border keep rendering).
    bool labels_hidden = gui_chrome_hide_hidden(&e->chrome_hide);

    const int divs = SPEC_AXIS_DIVS;
    const float pad = 3.0f;      // Inset from the border so labels sit inside the window
    const float label_h = (float)FONT_SIZE_OSC_SCALE;

    // DAW-style time axis. The rolling history covers time_bins display
    // frames (65536 samples each); the paused analysis window covers
    // analysis_span_seconds centered on the playhead (absolute timecodes).
    double time_window_s = 0.0;
    bool time_absolute = e->analysis_active;
    double time_center_s = 0.0;
    if (real_rate_hz > 0.0) {
        time_window_s = (double)e->time_bins * 65536.0 / real_rate_hz;
        if (time_absolute) {
            time_window_s = e->analysis_span_seconds;
            time_center_s = (double)e->analysis_playhead / real_rate_hz;
        }
    }

    if (!e->time_on_x) {
        // Waterfall: frequency on X (0 at left, Nyquist at right).
        // Labels drawn just inside the bottom edge.
        float label_y = bounds.y + bounds.height - label_h - pad;
        for (int d = 0; d <= divs; d++) {
            float frac = (float)d / (float)divs;
            float px = bounds.x + bounds.width * frac;
            DrawLineEx((Vector2){px, bounds.y}, (Vector2){px, bounds.y + bounds.height}, 1.0f, COLOR_GRID);
            if (!labels_hidden) {
                char buf[16];
                format_freq(buf, sizeof(buf), nyq * (double)frac);
                int tw = gui_text_measure_mono(buf, FONT_SIZE_OSC_SCALE);
                float lx = px - tw / 2.0f;
                // Keep label horizontally inside bounds.
                if (lx < bounds.x + pad) lx = bounds.x + pad;
                if (lx + tw > bounds.x + bounds.width - pad) lx = bounds.x + bounds.width - tw - pad;
                gui_text_draw_mono(buf, lx, label_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
            }
        }
        // Time axis (left inside edge, top = now): seconds back from now.
        if (!labels_hidden && real_rate_hz > 0.0) {
            const int t_divs = 2;
            float tx = bounds.x + pad;
            for (int d = 0; d <= t_divs; d++) {
                float frac = (float)d / (float)t_divs;          // 0 = top (newest)
                float py = bounds.y + bounds.height * frac;
                double t = -time_window_s * (double)frac;
                char tb[24];
                if (time_absolute) {
                    format_abs_timecode(tb, sizeof(tb), time_center_s - time_window_s / 2.0 + (double)(t_divs - d) / (double)t_divs * time_window_s);
                } else {
                    snprintf(tb, sizeof(tb), "%.2fs", t);
                }
                float ly = py - label_h / 2.0f;
                if (ly < bounds.y + pad) ly = bounds.y + pad;
                if (ly + label_h > bounds.y + bounds.height - pad) {
                    ly = bounds.y + bounds.height - label_h - pad;
                }
                gui_text_draw_mono(tb, tx, ly, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
            }
        }
        // "Now" marker at the top edge (newest slice).
        DrawLineEx((Vector2){bounds.x, bounds.y}, (Vector2){bounds.x + bounds.width, bounds.y},
                   2.0f, COLOR_GRID_MAJOR);
    } else {
        // Spectrograph: frequency on Y (0 at bottom, Nyquist at top).
        // Labels drawn just inside the left edge, vertically centered on each gridline.
        float label_x = bounds.x + pad;
        for (int d = 0; d <= divs; d++) {
            float frac = (float)d / (float)divs;          // 0 = 0 Hz, 1 = Nyquist
            float py = bounds.y + bounds.height * (1.0f - frac);  // frac=1 -> top
            DrawLineEx((Vector2){bounds.x, py}, (Vector2){bounds.x + bounds.width, py}, 1.0f, COLOR_GRID);
            if (!labels_hidden) {
                char buf[16];
                format_freq(buf, sizeof(buf), nyq * (double)frac);
                float ly = py - label_h / 2.0f;
                // Keep label vertically inside bounds.
                if (ly < bounds.y + pad) ly = bounds.y + pad;
                if (ly + label_h > bounds.y + bounds.height - pad) ly = bounds.y + bounds.height - label_h - pad;
                gui_text_draw_mono(buf, label_x, ly, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
            }
        }
        // Time axis (bottom inside edge): DAW-style. Paused analysis shows
        // ABSOLUTE timecodes across the window; rolling shows seconds back
        // from "now" (right edge = 0).
        if (real_rate_hz > 0.0) {
            const int t_divs = 4;
            float ty = bounds.y + bounds.height - label_h - pad;
            for (int d = 0; d <= t_divs; d++) {
                float frac = (float)d / (float)t_divs;          // 0 = left (oldest)
                float px = bounds.x + bounds.width * frac;
                DrawLineEx((Vector2){px, bounds.y}, (Vector2){px, bounds.y + bounds.height},
                           1.0f, COLOR_GRID);
                char tb[24];
                double t;
                if (time_absolute) {
                    t = time_center_s - time_window_s / 2.0 + (double)frac * time_window_s;
                    format_abs_timecode(tb, sizeof(tb), t);
                } else {
                    t = -time_window_s * (1.0 - (double)frac);
                    snprintf(tb, sizeof(tb), "%.2fs", t);
                }
                int tw = gui_text_measure_mono(tb, FONT_SIZE_OSC_SCALE);
                float lx = px - tw / 2.0f;
                if (lx < bounds.x + pad) lx = bounds.x + pad;
                if (lx + tw > bounds.x + bounds.width - pad) {
                    lx = bounds.x + bounds.width - tw - pad;
                }
                if (!labels_hidden) {
                    gui_text_draw_mono(tb, lx, ty, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
                }
            }
        }
        // "Now" marker at the right edge (newest slice).
        DrawLineEx((Vector2){bounds.x + bounds.width, bounds.y},
                   (Vector2){bounds.x + bounds.width, bounds.y + bounds.height},
                   2.0f, COLOR_GRID_MAJOR);
    }

    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
}

//-----------------------------------------------------------------------------
// Rendering (render thread, GL available)
//-----------------------------------------------------------------------------

static void spectro_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                                   Rectangle bounds, Color channel_color) {
    (void)channel_color;
    if (!state_ptr || !app) return;

    spectro_engine_t *e = (spectro_engine_t *)state_ptr;

    if (!e->fft.initialized) {
        const char *text = gui_spectrogram_available()
            ? "Spectrogram Initializing..."
            : "Spectrogram Not Available (FFTW)";
        int tw = gui_text_measure(text, FONT_SIZE_OSC_MSG);
        gui_text_draw(text, bounds.x + bounds.width / 2 - tw / 2,
                 bounds.y + bounds.height / 2 - 12,
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
        return;
    }

    // Create the GPU texture lazily (needs an OpenGL context, i.e. render thread).
    if (!e->tex_valid) {
        Image img = GenImageColor(e->tex_w, e->tex_h, BLACK);
        e->tex = LoadTextureFromImage(img);
        UnloadImage(img);
        if (e->tex.id == 0) {
            const char *text = "Spectrogram: texture alloc failed";
            int tw = gui_text_measure(text, FONT_SIZE_OSC_MSG);
            gui_text_draw(text, bounds.x + bounds.width / 2 - tw / 2,
                     bounds.y + bounds.height / 2 - 12,
                     FONT_SIZE_OSC_MSG, COLOR_CLIP_RED);
            DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
            return;
        }
        SetTextureFilter(e->tex, TEXTURE_FILTER_BILINEAR);
        e->tex_valid = true;
        e->needs_full_upload = true;
    }

    // DAW-style mode select (the selected-device check mirrors gui_ui.c's
    // gui_ui_selected_device_is_playback, which is file-static; the device
    // fields come from gui_app.h).
    bool playback_selected =
        app && app->selected_device >= 0 &&
        app->selected_device < app->device_count &&
        app->devices[app->selected_device].type == DEVICE_TYPE_PLAYBACK;
    // HELD = paused for THIS channel (user request 2026-11-03). The
    // per-channel transport pauses a track WITHOUT flipping the master
    // PLAYBACK_STATE, so the old master-state gate left the rolling feed
    // scrolling (repeating the last frame 60x/s) and the DAW paused
    // analysis window (direct file load around the playhead) never
    // engaged. Covers master pause and the no-loop end-of-file hold too;
    // the explicit state check keeps STARTING/STOPPED sessions on the
    // rolling path exactly like before.
    playback_state_t pb_state = gui_playback_get_state(app);
    bool playback_paused =
        playback_selected &&
        gui_playback_is_running(app) &&
        (pb_state == PLAYBACK_STATE_PAUSED ||
         pb_state == PLAYBACK_STATE_EOF ||
         (pb_state == PLAYBACK_STATE_PLAYING &&
          atomic_load(&app->playback_channel_paused[channel])));

    if (playback_paused) {
        // --- DAW-style paused analysis window (direct file render) ---
        // Playing stays the 1:1 live rolling feed; paused renders a real
        // window of the FILE around the playhead across the full history
        // width (duration-based span, async worker — keeps the previous
        // window until the new one is published).
        if (!e->analysis_active) {
            e->analysis_active = true;
            e->analysis_requested = false;
            e->analysis_rerequest = false;
            e->analysis_playhead = 0;
            e->analysis_seq = 0;
            e->analysis_span_seconds = SPEC_PAUSED_WINDOW_SECONDS;
        }
        spectro_update_paused_window(e, app, channel);
        UpdateTexture(e->tex, e->pixels);
        e->needs_full_upload = false;
    } else {
        if (e->analysis_active) {
            // Paused -> playing/live transition: restart the rolling feed.
            e->analysis_active = false;
            e->analysis_requested = false;
            e->last_seek_seq_valid = false;
            if (e->pixels) {
                memset(e->pixels, 0, (size_t)e->tex_w * e->tex_h * 4);
            }
            e->frame_valid = false;
            e->needs_full_upload = true;
        }

        // Consume the latest FFT frame from the display thread (no EMA: each
        // time slice is a distinct, crisp spectrum).
        bool got_new_frame = atomic_exchange(&e->fft.magnitude_ready, 0) != 0;
        if (got_new_frame) {
            int bins = e->fft.fft_bins;
            if (bins > 0 && e->fft.magnitude_back) {
                resample_magnitude(e->fft.magnitude_back, bins, e->frame, e->freq_bins);
            }
            e->last_sample_rate = atomic_load(&e->fft.sample_rate);
            e->frame_valid = true;
        }

        // Non-linear move detection (playback only): the playback thread bumps
        // the channel's seek sequence on every successful seek — clear the
        // rolling history on a change so it refills with the real timeline at
        // the new playhead (a paused scrub backfills the window ending at the
        // playhead; see gui_playback.c PLAYBACK_PREVIEW_WINDOW_SECONDS).
        if (playback_selected) {
            uint64_t seq = gui_playback_get_seek_seq_channel(app, channel);
            if (e->last_seek_seq_valid && seq != e->last_seek_seq && e->pixels) {
                memset(e->pixels, 0, (size_t)e->tex_w * e->tex_h * 4);
            }
            e->last_seek_seq = seq;
            e->last_seek_seq_valid = true;
        }
        // Scroll one slice and write the latest frame (repeats the last frame
        // if no new data arrived this render tick -> smooth 60 fps scroll).
        spectro_append_frame(e);

        // Upload the whole history buffer (no-flip; the draw uses negative
        // source height so buffer row 0 lands at the top of the screen).
        UpdateTexture(e->tex, e->pixels);
        e->needs_full_upload = false;
    }

    // Plot fills the whole bounds now; labels are drawn inside it on top of
    // the spectrogram (no reserved strip), so they read as part of the window.
    Rectangle plot = bounds;

    // Background + spectrogram.
    DrawRectangleRec(bounds, COLOR_METER_BG);
    DrawTexturePro(e->tex,
                   (Rectangle){0, 0, (float)e->tex_w, -(float)e->tex_h},
                   (Rectangle){plot.x, plot.y, plot.width, plot.height},
                   (Vector2){0, 0}, 0.0f, WHITE);

    // Real rate for the time axis: playback resolves the file's TRUE rate;
    // live capture's display rate is kHz-domain, so x1000.
    double real_rate_hz = 0.0;
    if (playback_selected) {
        playback_file_info_t info = {0};
        (void)gui_playback_get_selected_file_info(app, channel, &info);
        if (info.real_sample_rate_hz > 0.0) real_rate_hz = info.real_sample_rate_hz;
    }
    if (real_rate_hz <= 0.0 && e->last_sample_rate > 0) {
        real_rate_hz = (double)e->last_sample_rate * 1000.0;
    }

    spectro_draw_overlay(e, real_rate_hz, bounds, plot);

    // Paused analysis indicator: window span + pending state. Every column
    // of the paused window is ONE true FFT over that column's full contiguous
    // samples (full resolution; see gui_playback.c). Overlay chrome: hidden
    // with the panel's chrome toggle.
    if (e->analysis_active && !gui_chrome_hide_hidden(&e->chrome_hide)) {
        char win_buf[64];
        snprintf(win_buf, sizeof(win_buf), "PAUSED  %.2fs window%s",
                 e->analysis_span_seconds,
                 e->analysis_requested ? "" : "  (rendering...)");
        gui_text_draw(win_buf, bounds.x + 38.0f, bounds.y + 3.0f,
                     FONT_SIZE_OSC_SCALE, COLOR_TEXT);
    }

    // Palette dropdown (top-right): Phosphor / Heat. Selecting a palette
    // RELOADS the view through the new LUT (see spectro_palette_reload).
    // Skipped entirely while the chrome is hidden (the button rect then
    // stays stale; spectro_vtable_handle_click guards clicks on the flag).
    if (!gui_chrome_hide_hidden(&e->chrome_hide)) {
        static const char *pal_names[2] = { "Phosphor", "Heat" };
        float btn_h = 18;
        float pad = 8;
        // Text measurement contract: gui_text_measure is in the draw space
        // (logical px) - no 1/scale conversion (same fix as gui_oscilloscope).
        float to_logical_w = 1.0f;
        float btn_w = 0;
        for (int i = 0; i < 2; i++) {
            float w = (float)((int)(gui_text_measure(pal_names[i], FONT_SIZE_DROPDOWN_OPT) * to_logical_w)) + 26;
            if (w > btn_w) btn_w = w;
        }
        e->palette_btn_rect = (Rectangle){
            bounds.x + bounds.width - btn_w - pad, bounds.y + 6, btn_w, btn_h
        };
        Color btn_bg = e->palette_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
        DrawRectangleRounded(e->palette_btn_rect, 0.15f, 4, btn_bg);
        {
            const char *lbl = pal_names[s_spec_palette];
            int text_w = (int)((float)gui_text_measure(lbl, FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
            float total_w = (float)text_w + 12;
            float text_x = e->palette_btn_rect.x + (e->palette_btn_rect.width - total_w) / 2;
            float text_y = e->palette_btn_rect.y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
            gui_text_draw(lbl, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
            float as = 5.0f;
            float ax = text_x + (float)text_w + 6;
            float acy = e->palette_btn_rect.y + btn_h / 2;
            if (e->palette_dropdown_open) {
                Vector2 top = { ax + as / 2, acy - as / 2 };
                Vector2 left = { ax, acy + as / 2 };
                Vector2 right = { ax + as, acy + as / 2 };
                DrawTriangle(top, left, right, COLOR_TEXT);
            } else {
                Vector2 bottom = { ax + as / 2, acy + as / 2 };
                Vector2 left = { ax, acy - as / 2 };
                Vector2 right = { ax + as, acy - as / 2 };
                DrawTriangle(bottom, right, left, COLOR_TEXT);
            }
        }
        if (e->palette_dropdown_open) {
            float opt_h = 20;
            for (int i = 0; i < 2; i++) {
                Rectangle opt = {
                    e->palette_btn_rect.x,
                    e->palette_btn_rect.y + btn_h + (float)i * opt_h,
                    e->palette_btn_rect.width,
                    opt_h
                };
                e->palette_opts_rect[i] = opt;
                bool selected = ((int)s_spec_palette == i);
                bool hover = CheckCollisionPointRec(gui_ui_get_mouse_position(), opt);
                DrawRectangleRec(opt, gui_dropdown_option_color(selected, hover));
                const char *lbl = pal_names[i];
                int lw = (int)((float)gui_text_measure(lbl, FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
                float lx = opt.x + (opt.width - (float)lw) / 2;
                float ly = opt.y + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
                gui_text_draw(lbl, lx, ly, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Vtables + Registration
//-----------------------------------------------------------------------------

// Palette dropdown clicks (top-right): open/select/close. Selecting a
// palette RELOADS the view through the new LUT (paused: the same spectra
// re-mapped instantly + a fresh request; rolling: history cleared and
// refilled in the new palette). Returns true when a control consumed the
// click; false when no control was hit.
static bool spectro_handle_palette_click(spectro_engine_t *e, gui_app_t *app, int channel,
                                         Vector2 click) {

    if (e->palette_dropdown_open) {
        for (int i = 0; i < 2; i++) {
            if (CheckCollisionPointRec(click, e->palette_opts_rect[i])) {
                if ((int)s_spec_palette != i) {
                    s_spec_palette = (i == (int)SPEC_PALETTE_HEAT)
                        ? SPEC_PALETTE_HEAT : SPEC_PALETTE_PHOSPHOR;
                    spectro_palette_reload(e, app, channel);
                }
                e->palette_dropdown_open = false;
                return true;
            }
        }
        // Button or outside click closes the dropdown.
        e->palette_dropdown_open = false;
        return true;
    }
    if (CheckCollisionPointRec(click, e->palette_btn_rect)) {
        e->palette_dropdown_open = true;
        return true;
    }
    return false;
}

static bool spectro_vtable_handle_click(void *state_ptr, gui_app_t *app, int channel,
                                        Vector2 click, Rectangle bounds) {
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    if (!e) return false;

    // Click outside the panel resets double-click tracking.
    if (!CheckCollisionPointRec(click, bounds)) {
        gui_chrome_hide_reset(&e->chrome_hide);
        return false;
    }

    // Palette clicks only while the chrome is shown (the button rect is
    // stale while hidden); a consumed control click resets the tracker so
    // a double-click ON the dropdown never toggles the chrome.
    if (!gui_chrome_hide_hidden(&e->chrome_hide) &&
        spectro_handle_palette_click(e, app, channel, click)) {
        gui_chrome_hide_reset(&e->chrome_hide);
        return true;
    }

    // Double-click anywhere else on the panel toggles ALL overlay chrome
    // (palette dropdown, axis labels, PAUSED indicator) for a clean
    // spectrogram view.
    if (gui_chrome_hide_feed_click(&e->chrome_hide, click)) {
        e->palette_dropdown_open = false;
        return true;
    }
    return false;
}

// Wheel over the panel while the paused analysis window is active adjusts the
// window span (x1.25 / :1.25 steps, 0.1 s .. 60 s; longer spans are also capped
// by the full-resolution hop budget inside gui_playback.c — the span trades
// time vs frequency resolution exactly like an FFT-size setting, never detail).
static bool spectro_vtable_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
    if (!state_ptr || delta == 0.0f) return false;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    if (!e->analysis_active) return false;
    Vector2 mouse = gui_ui_get_mouse_position();
    if (!CheckCollisionPointRec(mouse, bounds)) return false;
    double span = e->analysis_span_seconds;
    if (delta > 0.0f) span /= 1.25;
    else span *= 1.25;
    if (span < 0.1) span = 0.1;
    if (span > 60.0) span = 60.0;
    e->analysis_span_seconds = span;
    e->analysis_rerequest = true;
    return true;
}

static const panel_vtable_t s_waterfall_vtable = {
    .name = "Waterfall",
    .create = waterfall_vtable_create,
    .destroy = spectro_vtable_destroy,
    .clear = spectro_vtable_clear,
    .process = spectro_vtable_process,
    .render = spectro_vtable_render,
    .render_overlay = NULL,
    .handle_click = spectro_vtable_handle_click,
    .handle_scroll = spectro_vtable_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

static const panel_vtable_t s_spectrograph_vtable = {
    .name = "Spectro",
    .create = spectrograph_vtable_create,
    .destroy = spectro_vtable_destroy,
    .clear = spectro_vtable_clear,
    .process = spectro_vtable_process,
    .render = spectro_vtable_render,
    .render_overlay = NULL,
    .handle_click = spectro_vtable_handle_click,
    .handle_scroll = spectro_vtable_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_waterfall_panel_register(void) {
    panel_register(PANEL_VIEW_WATERFALL, &s_waterfall_vtable);
}

void gui_spectrograph_panel_register(void) {
    panel_register(PANEL_VIEW_SPECTROGRAPH, &s_spectrograph_vtable);
}
