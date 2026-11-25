/*
 * MISRC GUI - Real-time Spectrogram panel implementation ("Spectro")
 *
 * One panel (PANEL_VIEW_SPECTROGRAPH) with a readout-direction dropdown
 * (harrypm 2026-11-18: "Merge waterfall mode into Spectro add a dropdown
 * horisonstally to switch readout direction"): Horizontal is the former
 * Spectro, Vertical the former Waterfall view (PANEL_VIEW_WATERFALL is kept
 * only as a legacy id that maps here). See gui_spectrogram.h for the
 * high-level description and threading model.
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
 *   Vertical:     freq on X (tex_w = SPEC_FREQ_BINS), time on Y scrolling down,
 *                 newest written at buffer row 0 (drawn at the top).
 *   Horizontal:   time on X scrolling left (tex_w = SPEC_TIME_BINS), freq on Y,
 *                 newest written at buffer col (tex_w-1) (drawn at the right),
 *                 high freq at buffer row 0 (drawn at the top).
 *   Both hold SPEC_FREQ_BINS x SPEC_TIME_BINS pixels, so a direction change
 *   only swaps the dimensions (the GPU texture is re-created).
 */

#include "gui_spectrogram.h"
#include "gui_fft.h"
#include "gui_text.h"
#include "../core/gui_app.h"
#include "../input/gui_playback.h"
#include "../input/gui_cut_markers.h"
#include "../signal/gui_cvbs.h"   // gui_video_panel_sync_source
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
// but only carries the last 65,536-sample display frames). The span is set
// by the span dropdown or the wheel (s_spans: 1 field .. 60 s; pausing opens
// at 1 frame).

#define SPEC_LABEL_H     16     // Vertical: bottom strip reserved for freq labels (px)
#define SPEC_LABEL_W     34     // Horizontal: left strip reserved for freq labels (px)

#define SPEC_AXIS_DIVS   8      // Number of frequency grid divisions

//-----------------------------------------------------------------------------
// Engine State
//-----------------------------------------------------------------------------

typedef struct spectro_engine {
    fft_state_t fft;                 // Reusable FFT engine (display thread writes back buffer)

    bool time_on_x;                  // Dir: true = Horizontal (time X, freq Y), false = Vertical (freq X, time Y)
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
    bool tex_recreate;               // Direction changed: dimensions swapped, render re-creates the texture
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
    uint64_t analysis_req_center;    // Centre sample of the last request (snapped for field spans)
    double analysis_t0;              // Window start (s) of the last request
    double line_seconds;             // One line of the Video System setting (64 / 63.56 us)
    // Field lock (harrypm 2026-11-19: "frame/field locked with the decoder
    // mode that is active"): the field lattice is anchored at lattice_t0, the
    // file time of a field-1 start found by the channel's Video decoder mode
    // (0 = nominal: counted from the file start, until a detection lands).
    int sync_mode;                   // GUI_FIELD_LOCATE_* requested (AUTO: format-agnostic)
    int found_mode, found_lines;     // What the field lock found (0 = not yet)
    double hs_t[32];                 // FM head switches in the window (seconds)
    int hs_head[32];
    size_t hs_count;
    int sync_lines;                  // 625 / 525
    double lattice_t0;
    bool lattice_locked;
    double lattice_pending_t0;       // Gross miss seen once, awaiting a second window
    bool lattice_pending;
    double mark_t[64];               // Detected field starts of the shown window (s)
    int mark_field[64];              // 1 / 2 (0 = parity unresolved)
    size_t mark_count;
    double analysis_span_seconds;    // Requested span (wheel adjustable)
    int analysis_span_fields;        // > 0: the span is this many video fields (short spans)
    int span_idx;                    // Paused span: index into s_spans
    double field_seconds;            // One field of the Video System setting (625: 20 ms, 525: 16.68 ms)
    uint64_t analysis_seq;          // Last consumed analysis result seq

    // Palette dropdown (top-right): Phosphor / Heat. Changing the palette
    // reloads the view (paused: instant re-map of the last result + re-request;
    // rolling: history clears and refills in the new palette).
    Rectangle palette_btn_rect;
    Rectangle palette_opts_rect[2];
    bool palette_dropdown_open;

    // Readout-direction dropdown (no label; left of the palette dropdown on
    // the same row): Horizontal / Vertical.
    Rectangle dir_btn_rect;
    Rectangle dir_opts_rect[2];
    bool dir_dropdown_open;

    // Paused-window span dropdown (left of Dir, shown only while paused).
    Rectangle span_btn_rect;
    Rectangle span_opts_rect[12];
    bool span_dropdown_open;
    bool marks_hidden;               // Marks toggle: field / frame wireframe off
    Rectangle marks_btn_rect;

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

static int s_spec_palette = SPEC_PALETTE_HEAT;   // Default Heat (harrypm 2026-11-19)

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
// Readout direction + launch state (--spectro)
//-----------------------------------------------------------------------------

enum { SPEC_DIR_HORIZONTAL = 0, SPEC_DIR_VERTICAL = 1, SPEC_DIR_COUNT };

// Paused-window spans (the span dropdown, the wheel and --spectro span=):
// video fields, two a frame (harrypm 2026-11-19: "Frames / Fields as each
// frame is 2 fields combined"), then seconds. Pausing opens at 1 frame.
static const struct {
    int fields;            // > 0: a span of fields
    double secs;           // else seconds
    const char *name;      // dropdown / label
    const char *key;       // --spectro span=
} s_spans[] = {
    { 1, 0, "1 field", "1field" },     { 2, 0, "1 frame", "1frame" },
    { 4, 0, "2 frames", "2frames" },   { 6, 0, "3 frames", "3frames" },
    { 10, 0, "5 frames", "5frames" },  { 20, 0, "10 frames", "10frames" },
    { 50, 0, "25 frames", "25frames" },
    { 0, 1.0, "1 s", "1s" },   { 0, 3.0, "3 s", "3s" },   { 0, 10.0, "10 s", "10s" },
    { 0, 30.0, "30 s", "30s" }, { 0, 60.0, "60 s", "60s" },
};
#define SPEC_SPAN_COUNT ((int)(sizeof(s_spans) / sizeof(s_spans[0])))
#define SPEC_SPAN_DEFAULT 1   // 1 frame
static const char *const s_dir_names[SPEC_DIR_COUNT] = { "Horizontal", "Vertical" };
static const char *const s_palette_names[2] = { "Phosphor", "Heat" };

// Marks toggle (harrypm 2026-11-25: "a toggle for the wire frames for the
// field frame info"): a plain on/off button, highlighted while the marks show.
static void spec_draw_toggle_button(Rectangle r, const char *label, bool on, float to_logical_w) {
    DrawRectangleRounded(r, 0.15f, 4, on ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
    const float tw = (float)gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT) * to_logical_w;
    gui_text_draw(label, r.x + (r.width - tw) * 0.5f, r.y + (r.height - FONT_SIZE_DROPDOWN_OPT) * 0.5f,
                  FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
}

// Launch state for new panels (hard rule dev/gui_automation_flags_rules.md).
static struct {
    int dir;
    int span;
    bool from_flag;
    bool marks_off;
} s_spec_launch = { SPEC_DIR_HORIZONTAL, SPEC_SPAN_DEFAULT, false, false };

bool gui_spectrogram_apply_launch_arg(const char *arg) {
    static const char *usage =
        "--spectro: expected comma-separated key=value pairs: dir=horizontal|vertical, "
        "palette=phosphor|heat, span=1field|1frame|2frames|3frames|5frames|10frames|25frames|"
        "1s|3s|10s|30s|60s (the paused window), marks=on|off (field / frame marks)";
    if (!arg || !*arg) {
        fprintf(stderr, "%s (got '')\n", usage);
        return false;
    }
    int dir = s_spec_launch.dir, palette = s_spec_palette, span = s_spec_launch.span;
    bool marks_off = s_spec_launch.marks_off;
    const char *p = arg;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        size_t klen = eq ? (size_t)(eq - p) : 0;
        const char *val = eq ? eq + 1 : NULL;
        size_t vlen = eq ? len - klen - 1 : 0;
        #define SPEC_IS(str, n, lit) ((n) == sizeof(lit) - 1 && strncmp((str), lit, (n)) == 0)
        bool ok = false;
        if (eq && SPEC_IS(p, klen, "dir")) {
            if (SPEC_IS(val, vlen, "horizontal")) { dir = SPEC_DIR_HORIZONTAL; ok = true; }
            else if (SPEC_IS(val, vlen, "vertical")) { dir = SPEC_DIR_VERTICAL; ok = true; }
        } else if (eq && SPEC_IS(p, klen, "span")) {
            for (int i = 0; i < SPEC_SPAN_COUNT && !ok; i++) {
                if (vlen == strlen(s_spans[i].key) && strncmp(val, s_spans[i].key, vlen) == 0) {
                    span = i;
                    ok = true;
                }
            }
        } else if (eq && SPEC_IS(p, klen, "palette")) {
            if (SPEC_IS(val, vlen, "phosphor")) { palette = SPEC_PALETTE_PHOSPHOR; ok = true; }
            else if (SPEC_IS(val, vlen, "heat")) { palette = SPEC_PALETTE_HEAT; ok = true; }
        } else if (eq && SPEC_IS(p, klen, "marks")) {
            if (SPEC_IS(val, vlen, "on")) { marks_off = false; ok = true; }
            else if (SPEC_IS(val, vlen, "off")) { marks_off = true; ok = true; }
        }
        #undef SPEC_IS
        if (!ok) {
            fprintf(stderr, "%s (got '%s')\n", usage, arg);
            return false;
        }
        p += len + (comma ? 1 : 0);
    }
    s_spec_launch.dir = dir;
    s_spec_launch.span = span;
    s_spec_launch.marks_off = marks_off;
    s_spec_launch.from_flag = true;
    s_spec_palette = palette;   // the palette is shared by every Spectro panel
    fprintf(stderr, "[AUTO] --spectro: dir=%s palette=%s span=%s marks=%s\n", s_dir_names[dir],
            s_palette_names[s_spec_palette], s_spans[span].name, marks_off ? "off" : "on");
    return true;
}

// Set the readout direction (no GL: the render re-creates the texture). The
// history buffer holds the same pixel count either way; it is cleared (the
// rolling history is laid out per direction).
static void spectro_set_time_on_x(spectro_engine_t *e, bool time_on_x) {
    if (!e || e->time_on_x == time_on_x) return;
    e->time_on_x = time_on_x;
    e->tex_w = time_on_x ? e->time_bins : e->freq_bins;
    e->tex_h = time_on_x ? e->freq_bins : e->time_bins;
    if (e->pixels) memset(e->pixels, 0, (size_t)e->tex_w * e->tex_h * 4);
    e->tex_recreate = true;
    e->needs_full_upload = true;
    if (e->analysis_active) e->analysis_rerequest = true;
}

// Select a paused-window span (s_spans index); field spans follow the
// Video System setting's field length.
static void spectro_set_span(spectro_engine_t *e, int idx) {
    if (idx < 0) idx = 0;
    if (idx >= SPEC_SPAN_COUNT) idx = SPEC_SPAN_COUNT - 1;
    // A wheel tick past either end clamps to the same span: no re-read
    // (traced: every tick below 1 field re-read the same window).
    if (idx == e->span_idx && e->analysis_span_seconds > 0.0) return;
    e->span_idx = idx;
    e->analysis_span_fields = s_spans[idx].fields;
    const double field_s = e->field_seconds > 0.0 ? e->field_seconds : 0.02;
    e->analysis_span_seconds = s_spans[idx].fields > 0 ? (double)s_spans[idx].fields * field_s
                                                       : s_spans[idx].secs;
    e->analysis_rerequest = true;
}

void gui_spectrogram_set_vertical(void *state, bool vertical) {
    spectro_set_time_on_x((spectro_engine_t *)state, !vertical);
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

static void *spectrograph_vtable_create(void) {
    void *state = spectro_create(s_spec_launch.dir == SPEC_DIR_HORIZONTAL);
    if (state) ((spectro_engine_t *)state)->marks_hidden = s_spec_launch.marks_off;
    if (state && s_spec_launch.from_flag) {
        fprintf(stderr, "[AUTO] Spectro panel: dir=%s palette=%s span=%s\n",
                s_dir_names[s_spec_launch.dir], s_palette_names[s_spec_palette],
                s_spans[s_spec_launch.span].name);
    }
    return state;
}

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
        // Vertical: time on Y (rows), freq on X (cols). Newest at row 0 (top).
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
        // Horizontal: time on X (cols), freq on Y (rows). Newest at col (tex_w-1) (right).
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
// Horizontal: slice 0 at the LEFT edge, newest (window end) at the right.
// Vertical: slice 0 at the BOTTOM, newest at the top (matches live flow).
static void spectro_write_slice_index(spectro_engine_t *e, int index) {
    if (!e || !e->frame_valid || index < 0) return;

    if (!e->time_on_x) {
        // Vertical: freq on X (cols), time on Y (rows). Oldest at bottom.
        int row = e->time_bins - 1 - index;
        if (row < 0 || row >= e->tex_h) return;
        for (int f = 0; f < e->freq_bins && f < e->tex_w; f++) {
            const unsigned char *c = lut_lookup(e, e->frame[f]);
            unsigned char *p = &e->pixels[((size_t)row * e->tex_w + (size_t)f) * 4];
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
        }
    } else {
        // Horizontal: time on X (cols), freq on Y (rows). Oldest at left.
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
    uint64_t playhead = gui_playback_get_cue_samples_channel(app, channel);
    // The file's true rate (the time axis's), for the window start in seconds
    // and the field snap.
    playback_file_info_t info = {0};
    (void)gui_playback_get_selected_file_info(app, channel, &info);
    const double rate = info.real_sample_rate_hz;
    // Frame / field spans (harrypm 2026-11-19: "scale locking at frame /
    // field accurate to lines ... so it clicks on these points"): the window
    // centred on the playhead, its START snapped to the nearest field
    // boundary, so the field / frame markers stay put and the window moves a
    // field at a time. Boundaries count from the file start (nominal until
    // the field lock keys them to real syncs). A field is a whole number of
    // lines, so the scale is exact to lines.
    uint64_t center = playhead;
    double t0 = 0.0;
    if (rate > 0.0) {
        const double span_samples = e->analysis_span_seconds * rate;
        double start_t = (double)playhead / rate - e->analysis_span_seconds / 2.0;
        if (e->analysis_span_fields > 0 && e->field_seconds > 0.0) {
            // On the field lattice: whole frames for frame spans, fields for
            // the 1-field span.
            const double step = (e->analysis_span_fields % 2 == 0) ? 2.0 * e->field_seconds
                                                                    : e->field_seconds;
            start_t = e->lattice_t0 + floor((start_t - e->lattice_t0) / step + 0.5) * step;
        }
        double start = start_t * rate;
        if (start < 0.0) start = 0.0;
        center = (uint64_t)(start + span_samples / 2.0 + 0.5);
        t0 = start / rate;
    }
    // A centre move under 2 lines alone (a re-anchor's sub-line wobble) does
    // not re-read: at 2-3 frames that wobble alternated the window one line
    // each read (traced: centre 46331379 <-> 46332651 for ever = the jump).
    const double center_tol = rate > 0.0 && e->line_seconds > 0.0 ? 2.0 * e->line_seconds * rate : 0.0;
    const bool center_moved = fabs((double)center - (double)e->analysis_req_center) > center_tol;
    if (!e->analysis_requested || e->analysis_rerequest || center_moved) {
        if (getenv("MISRC_PAUSED_TRACE"))
            fprintf(stderr, "[TRACE] spectro ch%d request center=%llu (prev %llu) rereq=%d lattice_t0=%.6f lines=%d span=%.4f\n",
                    channel, (unsigned long long)center, (unsigned long long)e->analysis_req_center,
                    (int)e->analysis_rerequest, e->lattice_t0, e->sync_lines, e->analysis_span_seconds);
        if (e->analysis_span_fields > 0) {
            gui_playback_request_analysis_window_fields(app, channel, center, e->analysis_span_seconds,
                                                        GUI_FIELD_LOCATE_AUTO,
                                                        gui_playback_measured_lines(app, channel)
                                                            ? gui_playback_measured_lines(app, channel)
                                                            : e->found_lines ? e->found_lines : e->sync_lines);
        } else {
            gui_playback_request_analysis_window(app, channel, center, e->analysis_span_seconds);
        }
        e->analysis_requested = true;
        e->analysis_rerequest = false;
        e->analysis_req_center = center;
        e->analysis_t0 = t0;
        e->mark_count = 0;          // until this window's marks publish
    }
    e->analysis_playhead = playhead;

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
        // This window's field starts (frame / field spans): the markers, and
        // the lattice re-anchored on a detected field 1. A window that sat
        // more than half a line off the real lattice is requested again.
        gui_field_mark_t marks[64];
        size_t nm = e->analysis_span_fields > 0
            ? gui_playback_analysis_field_marks(app, channel, marks, 64) : 0;
        // Head switches (FM): drawn as HS marks (format-independent reference).
        {
            uint64_t hp[32];
            int hh[32];
            size_t nh = e->analysis_span_fields > 0
                ? gui_playback_analysis_head_switches(app, channel, hp, hh, 32) : 0;
            e->hs_count = 0;
            for (size_t i = 0; i < nh && rate > 0.0; i++) {
                e->hs_t[e->hs_count] = (double)hp[i] / rate;
                e->hs_head[e->hs_count] = hh[i];
                e->hs_count++;
            }
        }
        // Format-agnostic lock (harrypm 2026-11-19): the locator measured the
        // mode and system from the file; adopt its line count.
        {
            int fm = 0, fln = 0;
            if (nm && gui_playback_analysis_field_info(app, channel, &fm, &fln) && fln) {
                e->found_mode = fm;
                if (fln != e->found_lines || fln != e->sync_lines) {
                    e->found_lines = fln;
                    if (fln != e->sync_lines) {
                        if (getenv("MISRC_PAUSED_TRACE"))
                            fprintf(stderr, "[TRACE] spectro ch%d system %d -> %d (found)\n", channel, e->sync_lines, fln);
                        e->sync_lines = fln;
                        e->lattice_locked = false;
                        e->lattice_pending = false;
                        e->analysis_rerequest = true;
                    }
                }
            }
        }
        e->mark_count = 0;
        for (size_t i = 0; i < nm && rate > 0.0; i++) {
            e->mark_t[e->mark_count] = (double)marks[i].pos / rate;
            e->mark_field[e->mark_count] = marks[i].field;
            e->mark_count++;
        }
        // Coherence (harrypm 2026-11-25 "on the hifi-audio fm file its a bit
        // glitchy"): a Hi-Fi audio carrier has no video fields, so the
        // window's "field 1" marks land anywhere (traced: each window put
        // field 1 10 / -160 / +149 lines off the last, every one a gross
        // re-anchor + re-read, for ever). Anchor only on a field 1 that most
        // of the window's field 1 marks agree with (same frame lattice within
        // 3 lines); a window with no such majority leaves the lattice alone.
        const double frame_s = 2.0 * e->field_seconds;
        size_t anchor = e->mark_count;
        {
            size_t n_f1 = 0, best = 0;
            for (size_t i = 0; i < e->mark_count; i++) {
                if (e->mark_field[i] != 1) continue;
                n_f1++;
                size_t agree = 0;
                for (size_t j = 0; j < e->mark_count; j++) {
                    if (e->mark_field[j] != 1) continue;
                    const double d = e->mark_t[j] - e->mark_t[i];
                    if (fabs(d - floor(d / frame_s + 0.5) * frame_s) < 3.0 * e->line_seconds) agree++;
                }
                if (agree > best) { best = agree; anchor = i; }
            }
            if (n_f1 >= 3 && best * 2 <= n_f1) {
                if (getenv("MISRC_PAUSED_TRACE"))
                    fprintf(stderr, "[TRACE] spectro ch%d marks incoherent (%zu of %zu field 1 agree): lattice kept\n",
                            channel, best, n_f1);
                anchor = e->mark_count;
            }
        }
        for (size_t i = anchor; i < e->mark_count; i++) {
            if (e->mark_field[i] != 1) continue;
            const double off = e->mark_t[i] - e->lattice_t0;
            const double drift = off - floor(off / frame_s + 0.5) * frame_s;
            // Locked, a "field 1" one FIELD off the lattice is a parity misread
            // of a field 2 start (tape RF H-sync phase wobble; traced: the
            // lattice flipped +-261 lines between windows): ignore it.
            if (e->lattice_locked &&
                fabs(fabs(drift) - e->field_seconds) < 15.0 * e->line_seconds) {
                if (getenv("MISRC_PAUSED_TRACE"))
                    fprintf(stderr, "[TRACE] spectro ch%d ignore parity misread (drift %.2f lines)\n",
                            channel, drift / e->line_seconds);
                break;
            }
            // Dead band 2.5 lines: the field-1 estimate wobbles by a line with
            // the window position (traced +-1.00 line at 2-3 frames); only a
            // real drift beyond that moves the lattice.
            if (!e->lattice_locked || fabs(drift) > 2.5 * e->line_seconds) {
                // Tape timing drifts the extrapolated lattice a little per
                // frame. A small drift re-anchors QUIETLY (the next step or
                // zoom places its window from this fresh anchor); only no
                // lock yet or a gross miss re-requests at once - each of
                // those re-requests was a second visible jump per step
                // (harrypm 2026-11-19 "spectro is still jumping").
                const bool gross = !e->lattice_locked || fabs(drift) > 10.0 * e->line_seconds;
                // Locked, a gross miss must be seen in two windows in a row
                // before it moves the lattice: the first sighting only arms
                // it (no re-read), so a one-off misread can't start a loop.
                if (gross && e->lattice_locked) {
                    bool confirmed = false;
                    if (e->lattice_pending) {
                        const double pd = e->mark_t[i] - e->lattice_pending_t0;
                        confirmed = fabs(pd - floor(pd / frame_s + 0.5) * frame_s) < 3.0 * e->line_seconds;
                    }
                    if (!confirmed) {
                        if (getenv("MISRC_PAUSED_TRACE"))
                            fprintf(stderr, "[TRACE] spectro ch%d gross drift %.2f lines held (awaiting a second window)\n",
                                    channel, drift / e->line_seconds);
                        e->lattice_pending = true;
                        e->lattice_pending_t0 = e->mark_t[i];
                        break;
                    }
                }
                e->lattice_pending = false;
                if (getenv("MISRC_PAUSED_TRACE"))
                    fprintf(stderr, "[TRACE] spectro ch%d re-anchor lattice %.6f -> %.6f (drift %.2f lines, locked %d)%s\n",
                            channel, e->lattice_t0, e->mark_t[i], drift / e->line_seconds, (int)e->lattice_locked,
                            gross ? " re-request" : " quiet");
                e->lattice_t0 = e->mark_t[i];
                e->lattice_locked = true;
                if (gross) e->analysis_rerequest = true;
            } else {
                e->lattice_pending = false;
            }
            break;
        }
    }
}

// Reload the view after a palette or direction change (same spectra, new
// LUT / layout):
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

// DAW-style absolute timecode: h:mm:ss.s / m:ss.s, or milliseconds
// (m:ss.sss) for sub-second windows (frame / field spans).
static void format_abs_timecode(char *buf, size_t bufsz, double secs, bool ms) {
    if (!(secs >= 0.0) || !isfinite(secs)) secs = 0.0;
    int h = (int)(secs / 3600.0);
    int m = (int)(secs / 60.0) % 60;
    double s = secs - (double)(h * 3600 + m * 60);
    if (ms)         snprintf(buf, bufsz, "%d:%06.3f", h * 60 + m, s);
    else if (h > 0) snprintf(buf, bufsz, "%d:%02d:%04.1f", h, m, s);
    else            snprintf(buf, bufsz, "%d:%04.1f", m, s);
}

// Frame.field position of a file time (harrypm 2026-11-19: frames / fields
// position context): field index from the file start, two fields a frame.
// Nominal until the field lock aligns the window to real syncs.
// Frame / field / line position of a file time on the field lattice
// (lattice_t0 = a field-1 start; frames numbered from the file start).
static double spec_floor_mod(double x, double m) { return x - floor(x / m) * m; }

// Frame line (1 .. 625 / 525, field 1 first) of a file time.
static int frame_line_at(const spectro_engine_t *e, double secs) {
    const double fs = e->field_seconds, ls = e->line_seconds;
    if (fs <= 0.0 || ls <= 0.0) return 0;
    const double in_frame = spec_floor_mod(secs - e->lattice_t0, 2.0 * fs);
    return (int)floor(in_frame / ls + 1e-9) + 1;
}

static void format_frame_pos(char *buf, size_t bufsz, const spectro_engine_t *e, double secs) {
    const double fs = e->field_seconds;
    if (fs <= 0.0 || !isfinite(secs)) { snprintf(buf, bufsz, "-"); return; }
    const double frame_s = 2.0 * fs;
    const long n0 = (long)floor(e->lattice_t0 / frame_s + 0.5);       // the anchor frame's number
    const long fi = (long)floor((secs - e->lattice_t0) / fs + 1e-9);  // fields from the anchor
    const long frame = n0 + (long)floor((double)fi / 2.0);
    const int field = (int)(fi - 2 * (long)floor((double)fi / 2.0)) + 1;
    snprintf(buf, bufsz, "%ld.%d", frame, field);
}

// A time label on the paused window: frame.field for frame / field spans,
// else the absolute timecode (ms under 1 s).
static void format_window_time(char *buf, size_t bufsz, const spectro_engine_t *e, double secs,
                               double window_s) {
    if (e->analysis_span_fields > 0) format_frame_pos(buf, bufsz, e, secs);
    else format_abs_timecode(buf, bufsz, secs, window_s < 1.0);
}

static void format_span(char *buf, size_t bufsz, const spectro_engine_t *e) {
    snprintf(buf, bufsz, "%s", s_spans[e->span_idx].name);
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
            time_center_s = e->analysis_t0 + time_window_s / 2.0;
        }
    }

    if (!e->time_on_x) {
        // Vertical: frequency on X (0 at left, Nyquist at right).
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
                    format_window_time(tb, sizeof(tb), e, time_center_s - time_window_s / 2.0 + (double)(t_divs - d) / (double)t_divs * time_window_s, time_window_s);
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
        // Horizontal: frequency on Y (0 at bottom, Nyquist at top).
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
                    format_window_time(tb, sizeof(tb), e, t, time_window_s);
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

// Field / frame boundary markers on a paused frame / field span (harrypm
// 2026-11-19: "I need field and frame boundry markers"): a thin line at each
// field start, a brighter, thicker one at each frame start (field 1), and an
// f1 / f2 tag in each field where it fits. Boundaries are counted from the
// file start (nominal until the field lock aligns them to real syncs). The
// lines are data like the grid; the tags hide with the chrome.
static void spectro_draw_field_marks(spectro_engine_t *e, double real_rate_hz, Rectangle bounds) {
    if (!e->analysis_active || e->analysis_span_fields <= 0 || real_rate_hz <= 0.0 ||
        e->field_seconds <= 0.0) return;
    const double win = e->analysis_span_seconds;
    const double t0 = e->analysis_t0;
    const double fs = e->field_seconds;
    const bool tags = !gui_chrome_hide_hidden(&e->chrome_hide);
    const Color frame_col = Fade(WHITE, 0.85f), field_col = Fade(WHITE, 0.45f);
    const float len = e->time_on_x ? bounds.width : bounds.height;
    const float px_per_field = (float)(fs / win) * len;

    // Line ticks along the time edge (bottom / left), every 1 / 5 / 10 / 25 /
    // 50 / 100 lines - the finest step at least 6 px apart - with every 5th
    // tick longer. Counted from each frame start (frame lines 1..625 / 525).
    const double ls = e->line_seconds;
    if (ls > 0.0) {
        static const int steps[] = { 1, 5, 10, 25, 50, 100 };
        const double px_per_line = (double)len * ls / win;
        int step = 0;
        for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++) {
            if (px_per_line * steps[i] >= 6.0) { step = steps[i]; break; }
        }
        if (step > 0) {
            const Color tc = Fade(WHITE, 0.55f);
            const double frame_s = 2.0 * fs;
            const double base = e->lattice_t0;
            const long f0 = (long)floor((t0 - base) / frame_s);
            const int lines_per_frame = (int)floor(frame_s / ls + 0.5);
            for (long f = f0; base + (double)f * frame_s <= t0 + win; f++) {
                for (int ln = 0; ln < lines_per_frame; ln += step) {
                    const double t = base + (double)f * frame_s + (double)ln * ls;
                    if (t < t0 || t > t0 + win) continue;
                    const float frac = (float)((t - t0) / win);
                    const float tl = ((ln / step) % 5 == 0) ? 8.0f : 4.0f;
                    if (e->time_on_x) {
                        const float x = bounds.x + frac * bounds.width;
                        DrawLineV((Vector2){ x, bounds.y + bounds.height - tl },
                                  (Vector2){ x, bounds.y + bounds.height }, tc);
                    } else {
                        const float y = bounds.y + (1.0f - frac) * bounds.height;
                        DrawLineV((Vector2){ bounds.x, y }, (Vector2){ bounds.x + tl, y }, tc);
                    }
                }
            }
        }
    }

    // FM head switches: dashed cyan lines tagged HS (harrypm 2026-11-19:
    // "headswitch peaks are an absolute irrespective of format").
    for (size_t i = 0; i < e->hs_count; i++) {
        if (e->hs_t[i] < t0 || e->hs_t[i] > t0 + win) continue;
        const float frac = (float)((e->hs_t[i] - t0) / win);
        const Color hc = (Color){ 90, 220, 255, 220 };
        if (e->time_on_x) {
            const float x = bounds.x + frac * bounds.width;
            for (float y = bounds.y; y < bounds.y + bounds.height; y += 8.0f)
                DrawLineV((Vector2){ x, y }, (Vector2){ x, fminf(y + 4.0f, bounds.y + bounds.height) }, hc);
            if (tags) gui_text_draw_mono(e->hs_head[i] ? "HS2" : "HS1", x + 3.0f, bounds.y + 40.0f,
                                         FONT_SIZE_OSC_SCALE, hc);
        } else {
            const float y = bounds.y + (1.0f - frac) * bounds.height;
            for (float x = bounds.x; x < bounds.x + bounds.width; x += 8.0f)
                DrawLineV((Vector2){ x, y }, (Vector2){ fminf(x + 4.0f, bounds.x + bounds.width), y }, hc);
            if (tags) gui_text_draw_mono(e->hs_head[i] ? "HS2" : "HS1", bounds.x + bounds.width - 44.0f,
                                         y - FONT_SIZE_OSC_SCALE - 2.0f, FONT_SIZE_OSC_SCALE, hc);
        }
    }

    // The boundaries: this window's detected field starts, or the lattice's.
    double bt[96];
    int bf[96];
    size_t nb = 0;
    if (e->mark_count > 0) {
        for (size_t i = 0; i < e->mark_count && nb < 96; i++) {
            bt[nb] = e->mark_t[i];
            bf[nb] = e->mark_field[i];
            nb++;
        }
    } else {
        long k = (long)ceil((t0 - e->lattice_t0) / fs - 1e-9);
        for (; e->lattice_t0 + (double)k * fs <= t0 + win + 1e-9 && nb < 96; k++) {
            bt[nb] = e->lattice_t0 + (double)k * fs;
            bf[nb] = (k - 2 * (long)floor((double)k / 2.0)) == 0 ? 1 : 2;
            nb++;
        }
    }
    for (size_t b = 0; b < nb; b++) {
        if (bt[b] < t0 - 1e-9 || bt[b] > t0 + win + 1e-9) continue;
        const float frac = (float)((bt[b] - t0) / win);   // 0 = window start (oldest)
        const bool frame_start = bf[b] == 1;
        const Color c = frame_start ? frame_col : field_col;
        const float thick = frame_start ? 2.0f : 1.0f;
        if (e->time_on_x) {
            const float x = bounds.x + frac * bounds.width;
            DrawLineEx((Vector2){ x, bounds.y }, (Vector2){ x, bounds.y + bounds.height }, thick, c);
        } else {
            const float y = bounds.y + (1.0f - frac) * bounds.height;   // newest at the top
            DrawLineEx((Vector2){ bounds.x, y }, (Vector2){ bounds.x + bounds.width, y }, thick, c);
        }
        // Tag the field that starts here, if it has room on screen.
        if (tags && px_per_field >= 22.0f && frac < 1.0f) {
            const char *tag = bf[b] == 1 ? "f1" : bf[b] == 2 ? "f2" : "f?";
            if (e->time_on_x) {
                const float x = bounds.x + frac * bounds.width + 3.0f;
                if (x + 14.0f < bounds.x + bounds.width) {
                    gui_text_draw_mono(tag, x, bounds.y + 26.0f, FONT_SIZE_OSC_SCALE, c);
                }
            } else {
                const float y = bounds.y + (1.0f - frac) * bounds.height - FONT_SIZE_OSC_SCALE - 2.0f;
                if (y > bounds.y + 26.0f) {
                    gui_text_draw_mono(tag, bounds.x + bounds.width - 22.0f, y, FONT_SIZE_OSC_SCALE, c);
                }
            }
        }
    }
}

// Cut markers (FLAC-Chop IN / OUT, set from the playback row's marker
// button or I / O) across the paused window: a coloured line with a dark
// edge and an IN / OUT tag, where the mark falls inside the window.
static void spectro_draw_cut_marks(spectro_engine_t *e, gui_app_t *app, int channel,
                                   double real_rate_hz, Rectangle bounds) {
    if (!e->analysis_active || real_rate_hz <= 0.0 || e->analysis_span_seconds <= 0.0) return;
    const char *path = channel == 1 ? app->settings.playback_file_b : app->settings.playback_file_a;
    const gui_cut_marks_t m = gui_cut_marks_get(channel, path);
    const double t0 = e->analysis_t0, win = e->analysis_span_seconds;
    const bool tags = !gui_chrome_hide_hidden(&e->chrome_hide);
    for (int k = 0; k < 2; k++) {
        if (k == 0 ? !m.has_in : !m.has_out) continue;
        const double t = (double)(k == 0 ? m.in : m.out) / real_rate_hz;
        if (t < t0 || t > t0 + win) continue;
        const float frac = (float)((t - t0) / win);
        const Color c = k == 0 ? COLOR_CUT_IN : COLOR_CUT_OUT;
        const Color edge = Fade(BLACK, 0.7f);
        const char *tag = k == 0 ? "IN" : "OUT";
        if (e->time_on_x) {
            const float x = bounds.x + frac * bounds.width;
            DrawRectangleRec((Rectangle){ x - 2.0f, bounds.y, 4.0f, bounds.height }, edge);
            DrawRectangleRec((Rectangle){ x - 1.0f, bounds.y, 2.0f, bounds.height }, c);
            if (tags) {
                const float tx = k == 0 ? x + 4.0f : x - 4.0f - 22.0f;
                gui_text_draw_mono(tag, tx, bounds.y + bounds.height - 40.0f, FONT_SIZE_OSC_SCALE, c);
            }
        } else {
            const float y = bounds.y + (1.0f - frac) * bounds.height;   // newest at the top
            DrawRectangleRec((Rectangle){ bounds.x, y - 2.0f, bounds.width, 4.0f }, edge);
            DrawRectangleRec((Rectangle){ bounds.x, y - 1.0f, bounds.width, 2.0f }, c);
            if (tags) {
                const float ty = k == 0 ? y - FONT_SIZE_OSC_SCALE - 3.0f : y + 3.0f;
                gui_text_draw_mono(tag, bounds.x + 30.0f, ty, FONT_SIZE_OSC_SCALE, c);
            }
        }
    }
}

// Hover readout (harrypm 2026-11-19): the frequency and time under the
// mouse, on the axes' own scales (rolling: seconds back from now; paused:
// the file's absolute timecode). Statistics like the axis labels, but hidden
// with the chrome and while a dropdown is open.
static void spectro_draw_hover(spectro_engine_t *e, double real_rate_hz, Rectangle bounds) {
    if (gui_chrome_hide_hidden(&e->chrome_hide) || e->palette_dropdown_open || e->dir_dropdown_open ||
        e->span_dropdown_open) return;
    Vector2 m = gui_ui_get_mouse_position();
    if (!CheckCollisionPointRec(m, bounds) || e->last_sample_rate == 0) return;
    if (CheckCollisionPointRec(m, e->palette_btn_rect) || CheckCollisionPointRec(m, e->dir_btn_rect) ||
        (e->analysis_active && (CheckCollisionPointRec(m, e->span_btn_rect) ||
                                CheckCollisionPointRec(m, e->marks_btn_rect)))) return;
    const double fx = (m.x - bounds.x) / bounds.width, fy = (m.y - bounds.y) / bounds.height;
    const double nyq = (double)e->last_sample_rate * 0.5;
    // Frequency: Horizontal = Y (Nyquist at the top), Vertical = X.
    const double hz = nyq * (e->time_on_x ? 1.0 - fy : fx);
    // Time: the fraction of the window from its oldest edge.
    const double tf = e->time_on_x ? fx : 1.0 - fy;
    char fbuf[24], tbuf[24], line[64];
    if (hz >= 1.0e6)      snprintf(fbuf, sizeof(fbuf), "%.3f MHz", hz / 1.0e6);
    else if (hz >= 1.0e3) snprintf(fbuf, sizeof(fbuf), "%.1f kHz", hz / 1.0e3);
    else                  snprintf(fbuf, sizeof(fbuf), "%.0f Hz", hz);
    tbuf[0] = 0;
    if (real_rate_hz > 0.0) {
        if (e->analysis_active) {
            const double win = e->analysis_span_seconds;
            const double t = e->analysis_t0 + tf * win;
            if (e->analysis_span_fields > 0) {
                char fp[24];
                format_frame_pos(fp, sizeof(fp), e, t);
                const char *dot = strchr(fp, '.');
                snprintf(tbuf, sizeof(tbuf), "frame %.*s f%s L%d", dot ? (int)(dot - fp) : (int)strlen(fp),
                         fp, dot ? dot + 1 : "1", frame_line_at(e, t));
            } else {
                format_abs_timecode(tbuf, sizeof(tbuf), t, win < 1.0);
            }
        } else {
            const double win = (double)e->time_bins * 65536.0 / real_rate_hz;
            snprintf(tbuf, sizeof(tbuf), "%.3fs", -win * (1.0 - tf));
        }
    }
    snprintf(line, sizeof(line), "%s  %s", fbuf, tbuf);
    const int tw = gui_text_measure_mono(line, FONT_SIZE_OSC_SCALE);
    const float pad = 3.0f, h = (float)FONT_SIZE_OSC_SCALE + 2 * pad;
    float x = m.x + 14.0f, y = m.y + 14.0f;
    if (x + tw + 2 * pad > bounds.x + bounds.width) x = m.x - 14.0f - tw - 2 * pad;
    if (y + h > bounds.y + bounds.height) y = m.y - 14.0f - h;
    DrawRectangleRec((Rectangle){ x, y, (float)tw + 2 * pad, h }, Fade(BLACK, 0.7f));
    gui_text_draw_mono(line, x + pad, y + pad, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
}

//-----------------------------------------------------------------------------
// Dropdown drawing (the palette dropdown's style, shared by Dir)
//-----------------------------------------------------------------------------

static float spec_dropdown_width(const char *const *names, int n, float to_logical_w) {
    float w = 0;
    for (int i = 0; i < n; i++) {
        float lw = (float)((int)(gui_text_measure(names[i], FONT_SIZE_DROPDOWN_OPT) * to_logical_w)) + 26;
        if (lw > w) w = lw;
    }
    return w;
}

static void spec_draw_dropdown_button(Rectangle btn, const char *lbl, bool open, float to_logical_w) {
    DrawRectangleRounded(btn, 0.15f, 4, open ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
    int text_w = (int)((float)gui_text_measure(lbl, FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
    float total_w = (float)text_w + 12;
    float text_x = btn.x + (btn.width - total_w) / 2;
    float text_y = btn.y + (btn.height - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(lbl, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    float as = 5.0f;
    float ax = text_x + (float)text_w + 6;
    float acy = btn.y + btn.height / 2;
    if (open) {
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

static void spec_draw_dropdown_options(Rectangle btn, const char *const *names, int n, int selected,
                                       Rectangle *opts, float to_logical_w) {
    const float opt_h = 20;
    for (int i = 0; i < n; i++) {
        Rectangle opt = { btn.x, btn.y + btn.height + (float)i * opt_h, btn.width, opt_h };
        opts[i] = opt;
        bool hover = CheckCollisionPointRec(gui_ui_get_mouse_position(), opt);
        DrawRectangleRec(opt, gui_dropdown_option_color(selected == i, hover));
        int lw = (int)((float)gui_text_measure(names[i], FONT_SIZE_DROPDOWN_OPT) * to_logical_w);
        gui_text_draw(names[i], opt.x + (opt.width - (float)lw) / 2,
                      opt.y + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    }
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

    // A direction change swapped the history's dimensions: re-create the
    // texture at the new size.
    if (e->tex_recreate) {
        if (e->tex_valid) UnloadTexture(e->tex);
        e->tex_valid = false;
        e->tex_recreate = false;
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

    // One field of the Video System setting (the Video panel's System
    // dropdown writes it): a field-unit span follows a system change.
    // The channel's Video panel names the decoder mode (CVBS / RF Preview)
    // and system the field lock follows; without one: CVBS on the Video
    // System setting.
    {
        const channel_panel_config_t *cfg = channel == 1 ? &app->panel_config_b : &app->panel_config_a;
        const void *video = NULL;
        if (cfg->left_view == PANEL_VIEW_CVBS) video = cfg->left_state;
        else if (cfg->split && cfg->right_view == PANEL_VIEW_CVBS) video = cfg->right_state;
        // The field lock is format-agnostic (AUTO: the locator measures mode
        // and system from the file); the Video panel / Video System setting
        // only seed the nominal lattice until a lock lands.
        int mode = GUI_FIELD_LOCATE_CVBS, lines = app->settings.video_system == 1 ? 525 : 625;
        if (video) (void)gui_video_panel_sync_source(video, &mode, &lines);
        e->sync_mode = GUI_FIELD_LOCATE_AUTO;
        // Measured on this channel (by any panel's paused read): adopt it.
        const int measured = playback_selected ? gui_playback_measured_lines(app, channel) : 0;
        if (measured && measured != e->sync_lines) {
            e->sync_lines = measured;
            e->found_lines = measured;
            e->lattice_locked = false;
            e->lattice_pending = false;
            e->analysis_rerequest = true;
        } else if (!measured && !e->found_lines && lines != e->sync_lines) {
            e->sync_lines = lines;
            e->lattice_locked = false;
            e->lattice_pending = false;
            e->analysis_rerequest = true;
        }
    }
    e->field_seconds = (e->sync_lines == 525) ? 1001.0 / 60000.0 : 1.0 / 50.0;
    e->line_seconds = 2.0 * e->field_seconds / (double)e->sync_lines;
    if (e->analysis_span_fields > 0) {
        const double want = (double)e->analysis_span_fields * e->field_seconds;
        if (e->analysis_span_seconds != want) {
            e->analysis_span_seconds = want;
            e->analysis_rerequest = true;
        }
    }

    if (playback_paused) {
        // Diagnostics only (no GUI control): MISRC_DEBUG_SPANSTEP=<n> steps the
        // paused span down one notch every 1.5 s, n times, as the wheel does.
        {
            static int span_steps_left = -1;
            static double span_step_at = 0.0;
            if (span_steps_left < 0) {
                const char *ev = getenv("MISRC_DEBUG_SPANSTEP");
                span_steps_left = ev ? atoi(ev) : 0;
                span_step_at = GetTime() + 3.0;
            }
            if (span_steps_left > 0 && GetTime() >= span_step_at) {
                spectro_set_span(e, e->span_idx - 1);
                fprintf(stderr, "[TRACE] spanstep: span idx %d (%d fields)\n", e->span_idx,
                        e->analysis_span_fields);
                span_steps_left--;
                span_step_at = GetTime() + 1.5;
            }
        }
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
            spectro_set_span(e, s_spec_launch.span);
            e->lattice_t0 = 0.0;
            e->lattice_locked = false;
            e->lattice_pending = false;
            e->mark_count = 0;
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
    if (!e->marks_hidden) spectro_draw_field_marks(e, real_rate_hz, bounds);
    if (playback_selected) spectro_draw_cut_marks(e, app, channel, real_rate_hz, bounds);
    spectro_draw_hover(e, real_rate_hz, bounds);

    // Paused analysis indicator: window span + pending state. Every column
    // of the paused window is ONE true FFT over that column's full contiguous
    // samples (full resolution; see gui_playback.c). Overlay chrome: hidden
    // with the panel's chrome toggle.
    if (e->analysis_active && !gui_chrome_hide_hidden(&e->chrome_hide)) {
        char span_buf[32], win_buf[64];
        format_span(span_buf, sizeof(span_buf), e);
        const char *lock = e->analysis_span_fields <= 0 ? ""
            : !e->lattice_locked ? "  nominal"
            : e->found_mode == GUI_FIELD_LOCATE_FM
                ? (e->sync_lines == 525 ? "  locked FM 525" : "  locked FM 625")
                : (e->sync_lines == 525 ? "  locked CVBS 525" : "  locked CVBS 625");
        snprintf(win_buf, sizeof(win_buf), "PAUSED  %s%s%s", span_buf, lock,
                 e->analysis_requested ? "" : "  (rendering...)");
        gui_text_draw(win_buf, bounds.x + 38.0f, bounds.y + 3.0f,
                     FONT_SIZE_OSC_SCALE, COLOR_TEXT);
    }

    // Top-right dropdown row, left to right: [Horizontal|Vertical]
    // (readout direction) then the palette [Phosphor|Heat]. Selecting a
    // palette or direction RELOADS the view (see spectro_palette_reload).
    // Skipped entirely while the chrome is hidden (the button rects then
    // stay stale; spectro_vtable_handle_click guards clicks on the flag).
    if (!gui_chrome_hide_hidden(&e->chrome_hide)) {
        const float btn_h = 18;
        const float pad = 8;
        const float gap = 6;
        float to_logical_w = 1.0f / gui_ui_get_scale_factor();
        float pal_w = spec_dropdown_width(s_palette_names, 2, to_logical_w);
        float dir_w = spec_dropdown_width(s_dir_names, SPEC_DIR_COUNT, to_logical_w);
        e->palette_btn_rect = (Rectangle){
            bounds.x + bounds.width - pal_w - pad, bounds.y + 6, pal_w, btn_h
        };
        e->dir_btn_rect = (Rectangle){
            e->palette_btn_rect.x - gap - dir_w, bounds.y + 6, dir_w, btn_h
        };
        // No text label (harrypm 2026-11-18): the button reads Horizontal /
        // Vertical on its own, like the palette button.
        spec_draw_dropdown_button(e->dir_btn_rect, s_dir_names[e->time_on_x ? SPEC_DIR_HORIZONTAL
                                                                            : SPEC_DIR_VERTICAL],
                                  e->dir_dropdown_open, to_logical_w);
        spec_draw_dropdown_button(e->palette_btn_rect, s_palette_names[s_spec_palette],
                                  e->palette_dropdown_open, to_logical_w);
        if (e->analysis_active) {
            const char *span_names[SPEC_SPAN_COUNT];
            for (int i = 0; i < SPEC_SPAN_COUNT; i++) span_names[i] = s_spans[i].name;
            float span_w = spec_dropdown_width(span_names, SPEC_SPAN_COUNT, to_logical_w);
            e->span_btn_rect = (Rectangle){ e->dir_btn_rect.x - gap - span_w, bounds.y + 6, span_w, btn_h };
            spec_draw_dropdown_button(e->span_btn_rect, s_spans[e->span_idx].name,
                                      e->span_dropdown_open, to_logical_w);
            {
                const char *ml = e->marks_hidden ? "Marks: Off" : "Marks: On";
                const float mw = (float)gui_text_measure("Marks: Off", FONT_SIZE_DROPDOWN_OPT) * to_logical_w + 16.0f;
                e->marks_btn_rect = (Rectangle){ e->span_btn_rect.x - gap - mw, bounds.y + 6, mw, btn_h };
                spec_draw_toggle_button(e->marks_btn_rect, ml, !e->marks_hidden, to_logical_w);
            }
            if (e->span_dropdown_open) {
                spec_draw_dropdown_options(e->span_btn_rect, span_names, SPEC_SPAN_COUNT, e->span_idx,
                                           e->span_opts_rect, to_logical_w);
            }
        } else {
            e->span_dropdown_open = false;
            e->marks_btn_rect = (Rectangle){0};
        }
        if (e->dir_dropdown_open) {
            spec_draw_dropdown_options(e->dir_btn_rect, s_dir_names, SPEC_DIR_COUNT,
                                       e->time_on_x ? SPEC_DIR_HORIZONTAL : SPEC_DIR_VERTICAL,
                                       e->dir_opts_rect, to_logical_w);
        }
        if (e->palette_dropdown_open) {
            spec_draw_dropdown_options(e->palette_btn_rect, s_palette_names, 2, s_spec_palette,
                                       e->palette_opts_rect, to_logical_w);
        }
    }
}

//-----------------------------------------------------------------------------
// Vtables + Registration
//-----------------------------------------------------------------------------

// Dropdown clicks (top-right row: Dir, palette): open / select / close. A
// selection RELOADS the view (paused: the same spectra re-mapped instantly +
// a fresh request; rolling: history cleared and refilled). One dropdown is
// open at a time. Returns true when a control consumed the click; false
// when no control was hit.
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
    if (e->span_dropdown_open) {
        for (int i = 0; i < SPEC_SPAN_COUNT; i++) {
            if (CheckCollisionPointRec(click, e->span_opts_rect[i])) {
                if (e->span_idx != i) spectro_set_span(e, i);
                e->span_dropdown_open = false;
                return true;
            }
        }
        e->span_dropdown_open = false;
        return true;
    }
    if (e->dir_dropdown_open) {
        for (int i = 0; i < SPEC_DIR_COUNT; i++) {
            if (CheckCollisionPointRec(click, e->dir_opts_rect[i])) {
                const bool time_on_x = i == SPEC_DIR_HORIZONTAL;
                if (e->time_on_x != time_on_x) {
                    spectro_set_time_on_x(e, time_on_x);
                    spectro_palette_reload(e, app, channel);
                }
                e->dir_dropdown_open = false;
                return true;
            }
        }
        e->dir_dropdown_open = false;
        return true;
    }
    if (e->marks_btn_rect.width > 0 && CheckCollisionPointRec(click, e->marks_btn_rect)) {
        e->marks_hidden = !e->marks_hidden;
        return true;
    }
    if (CheckCollisionPointRec(click, e->palette_btn_rect)) {
        e->palette_dropdown_open = true;
        return true;
    }
    if (CheckCollisionPointRec(click, e->dir_btn_rect)) {
        e->dir_dropdown_open = true;
        return true;
    }
    if (e->analysis_active && CheckCollisionPointRec(click, e->span_btn_rect)) {
        e->span_dropdown_open = true;
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
    // (Dir + palette dropdowns, axis labels, PAUSED indicator) for a clean
    // spectrogram view.
    if (gui_chrome_hide_feed_click(&e->chrome_hide, click)) {
        e->palette_dropdown_open = false;
        e->dir_dropdown_open = false;
        e->span_dropdown_open = false;
        return true;
    }
    return false;
}

// Wheel over the panel while the paused analysis window is active steps the
// span through the span dropdown's list (1 field .. 60 s); longer spans are
// also capped by the full-resolution hop budget inside gui_playback.c — the
// span trades time vs frequency resolution like an FFT-size setting.
static bool spectro_vtable_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
    if (!state_ptr || delta == 0.0f) return false;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    if (!e->analysis_active) return false;
    Vector2 mouse = gui_ui_get_mouse_position();
    if (!CheckCollisionPointRec(mouse, bounds)) return false;
    spectro_set_span(e, e->span_idx + (delta > 0.0f ? -1 : 1));
    return true;
}

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

void gui_spectrograph_panel_register(void) {
    panel_register(PANEL_VIEW_SPECTROGRAPH, &s_spectrograph_vtable);
}
