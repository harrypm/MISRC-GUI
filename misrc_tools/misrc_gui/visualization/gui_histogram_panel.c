/*
 * MISRC GUI - Histogram Panel Rendering Implementation
 *
 * Renders amplitude histogram with heatmap coloring consistent
 * with the oscilloscope/FFT phosphor displays.
 */

#include "gui_histogram_panel.h"
#include "../core/gui_app.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_dropdown.h"
#include "../ui/gui_chrome_hide.h"  // double-click overlay toggle (shared helper)
#include "gui_text.h"
#include "../signal/gui_cvbs.h"   // gui_video_channel_frame (Parade)
#include "../signal/gui_rf_preview.h"   // RF_DISPLAY_BLACK_255 / WHITE_255
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Rendering Configuration
//-----------------------------------------------------------------------------

// Set to true for logarithmic Y-axis scaling, false for linear
static const bool s_use_log_scale = false;

//-----------------------------------------------------------------------------
// Bin count options for dropdown
//-----------------------------------------------------------------------------

static const int s_bin_options[] = {64, 128, 256, 512, 1024, 2048, 4096};
static const int s_num_bin_options = sizeof(s_bin_options) / sizeof(s_bin_options[0]);

//-----------------------------------------------------------------------------
// Extended Histogram State (includes overlay/menu state)
//-----------------------------------------------------------------------------

typedef struct {
    histogram_state_t hist;         // Base histogram state

    // Overlay UI state (self-contained, no static arrays)
    Rectangle button_rect;
    Rectangle options_rect[7];      // Must match s_num_bin_options
    bool dropdown_open;

    // Double-click chrome-hide tracker (see ui/gui_chrome_hide.h): toggles
    // ALL overlay chrome (the Bins dropdown + label); the histogram bars and
    // their axis labels (drawn by the data render) keep rendering.
    gui_chrome_hide_t chrome_hide;

    // Menu items for vtable interface
    panel_menu_item_t menu_items[7];

    // Mode (harrypm 2026-11-26): Amplitude = the sample histogram above;
    // Parade = WFM-style R / G / B (or one channel / W = luma) raster of the
    // decoded picture of the Video panel on the same channel. Area Active
    // (default) = the Video panel's Active crop, Full = the whole frame.
    int mode;                       // HIST_MODE_*
    int parade_ch;                  // PARADE_CH_*
    bool parade_full;               // Area: false Active, true Full
    bool parade_10bit;              // Range: 8-bit 0..255 / 10-bit 0..1023 scale
    Rectangle mode_rect, ch_rect, area_rect, range_rect;
    // Parade raster: counts [level 0..255][sections * cols], RGBA texture.
    uint32_t *counts;
    uint8_t *pixels;
    int *col_of_x;
    int col_cap;
    int raster_w;                   // sections * cols of the current raster
    int raster_cols;                // columns per section
    int raster_levels;              // 256 (8-bit) or 1024 (10-bit)
    Texture2D tex;
    int tex_w, tex_h;
    bool tex_valid, have_frame;
    double last_update;
    uint64_t last_generation;
} histogram_panel_state_t;

enum { HIST_MODE_AMPLITUDE = 0, HIST_MODE_PARADE = 1 };
enum { PARADE_CH_RGB = 0, PARADE_CH_R, PARADE_CH_G, PARADE_CH_B, PARADE_CH_W, PARADE_CH_COUNT };
static const char *const s_parade_ch_names[PARADE_CH_COUNT] = { "RGB", "R", "G", "B", "W" };
#define PARADE_LEVELS_8 256
#define PARADE_LEVELS_10 1024
// Standard video code values (Rec.601 / BT.656): black / white.
#define PARADE_BLACK_8 16
#define PARADE_WHITE_8 235
#define PARADE_BLACK_10 64
#define PARADE_WHITE_10 940
#define PARADE_MAX_COLS 512
#define PARADE_UPDATE_S 0.04         // at most 25 raster updates per second

// Launch state of new Histogram panels (--histogram; hard rule
// dev/gui_automation_flags_rules.md).
static struct {
    int mode, parade_ch, bins;
    bool parade_full, parade_10bit, from_flag;
} s_hist_launch = { HIST_MODE_AMPLITUDE, PARADE_CH_RGB, 0, false, false, false };

static bool hist_flag_value(const char *key, const char *val, size_t len) {
    #define HS_IS(lit) (len == sizeof(lit) - 1 && strncmp(val, lit, len) == 0)
    if (strcmp(key, "mode") == 0) {
        if (HS_IS("amplitude")) { s_hist_launch.mode = HIST_MODE_AMPLITUDE; return true; }
        if (HS_IS("parade")) { s_hist_launch.mode = HIST_MODE_PARADE; return true; }
    } else if (strcmp(key, "channel") == 0) {
        static const char *const keys[PARADE_CH_COUNT] = { "rgb", "r", "g", "b", "w" };
        for (int i = 0; i < PARADE_CH_COUNT; i++)
            if (len == strlen(keys[i]) && strncmp(val, keys[i], len) == 0) { s_hist_launch.parade_ch = i; return true; }
    } else if (strcmp(key, "area") == 0) {
        if (HS_IS("active")) { s_hist_launch.parade_full = false; return true; }
        if (HS_IS("full")) { s_hist_launch.parade_full = true; return true; }
    } else if (strcmp(key, "range") == 0) {
        if (HS_IS("8") || HS_IS("8bit")) { s_hist_launch.parade_10bit = false; return true; }
        if (HS_IS("10") || HS_IS("10bit")) { s_hist_launch.parade_10bit = true; return true; }
    } else if (strcmp(key, "bins") == 0) {
        for (int i = 0; i < s_num_bin_options; i++) {
            char b[8];
            snprintf(b, sizeof(b), "%d", s_bin_options[i]);
            if (len == strlen(b) && strncmp(val, b, len) == 0) { s_hist_launch.bins = s_bin_options[i]; return true; }
        }
    }
    #undef HS_IS
    return false;
}

bool gui_histogram_panel_apply_launch_arg(const char *arg) {
    static const char *usage =
        "--histogram: expected comma-separated key=value pairs: mode=amplitude|parade, "
        "channel=rgb|r|g|b|w, area=active|full, range=8|10, bins=64|128|256|512|1024|2048|4096";
    if (!arg || !*arg) {
        fprintf(stderr, "%s (got '')\n", usage);
        return false;
    }
    const char *p = arg;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        char key[16];
        size_t klen = eq ? (size_t)(eq - p) : 0;
        if (!eq || klen == 0 || klen >= sizeof(key)) {
            fprintf(stderr, "%s (got '%s')\n", usage, arg);
            return false;
        }
        memcpy(key, p, klen);
        key[klen] = 0;
        if (!hist_flag_value(key, eq + 1, len - klen - 1)) {
            fprintf(stderr, "%s (got '%s')\n", usage, arg);
            return false;
        }
        p += len + (comma ? 1 : 0);
    }
    s_hist_launch.from_flag = true;
    fprintf(stderr, "[AUTO] --histogram: mode=%s channel=%s area=%s range=%s bins=%d\n",
            s_hist_launch.mode == HIST_MODE_PARADE ? "Parade" : "Amplitude",
            s_parade_ch_names[s_hist_launch.parade_ch], s_hist_launch.parade_full ? "Full" : "Active",
            s_hist_launch.parade_10bit ? "10-bit" : "8-bit",
            s_hist_launch.bins ? s_hist_launch.bins : HISTOGRAM_DEFAULT_NUM_BINS);
    return true;
}

//-----------------------------------------------------------------------------
// Histogram Panel Processing (called from display thread)
//-----------------------------------------------------------------------------

// Helper to process histogram for a single panel config
static void process_panel_histograms(const channel_panel_config_t *config,
                                     const int16_t *samples,
                                     size_t count) {
    if (!config) return;

    // Check left panel
    if (config->left_view == PANEL_VIEW_HISTOGRAM && config->left_state &&
        ((histogram_panel_state_t *)config->left_state)->mode != HIST_MODE_PARADE) {
        histogram_process((histogram_state_t *)config->left_state, samples, count);
    }

    // Check right panel (only if split mode is active)
    if (config->split && config->right_view == PANEL_VIEW_HISTOGRAM && config->right_state &&
        ((histogram_panel_state_t *)config->right_state)->mode != HIST_MODE_PARADE) {
        histogram_process((histogram_state_t *)config->right_state, samples, count);
    }
}

void gui_histogram_panel_process_all(struct gui_app *app,
                                     const int16_t *samples_a,
                                     const int16_t *samples_b,
                                     size_t count) {
    if (!app || count == 0) return;

    // Process channel A histograms
    if (samples_a) {
        process_panel_histograms(&app->panel_config_a, samples_a, count);
    }

    // Process channel B histograms
    if (samples_b) {
        process_panel_histograms(&app->panel_config_b, samples_b, count);
    }
}

//=============================================================================
// Panel Interface (vtable) Implementation
//=============================================================================

//-----------------------------------------------------------------------------
// Lifecycle Functions
//-----------------------------------------------------------------------------

static void *histogram_vtable_create(void) {
    histogram_panel_state_t *state = calloc(1, sizeof(histogram_panel_state_t));
    if (!state) return NULL;

    // Initialize with default bins
    if (!histogram_init(&state->hist, HISTOGRAM_DEFAULT_NUM_BINS)) {
        free(state);
        return NULL;
    }

    state->dropdown_open = false;
    state->mode = s_hist_launch.mode;
    state->parade_ch = s_hist_launch.parade_ch;
    state->parade_full = s_hist_launch.parade_full;
    state->parade_10bit = s_hist_launch.parade_10bit;
    if (s_hist_launch.bins) histogram_set_num_bins(&state->hist, s_hist_launch.bins);
    if (s_hist_launch.from_flag)
        fprintf(stderr, "[AUTO] Histogram panel: mode=%s channel=%s area=%s range=%s bins=%d\n",
                state->mode == HIST_MODE_PARADE ? "Parade" : "Amplitude",
                s_parade_ch_names[state->parade_ch], state->parade_full ? "Full" : "Active",
                state->parade_10bit ? "10-bit" : "8-bit",
                state->hist.num_bins);
    return state;
}

static void histogram_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_cleanup(&state->hist);
    if (state->tex_valid) UnloadTexture(state->tex);
    free(state->counts);
    free(state->pixels);
    free(state->col_of_x);
    free(state);
}

static void histogram_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_clear(&state->hist);
}

//-----------------------------------------------------------------------------
// Processing Function (called from display thread)
//-----------------------------------------------------------------------------

static void histogram_vtable_process(void *state_ptr, const int16_t *samples, size_t count, uint32_t sample_rate) {
    (void)sample_rate;  // Histogram doesn't need sample rate
    if (!state_ptr || !samples || count == 0) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    if (state->mode == HIST_MODE_PARADE) return;
    histogram_process(&state->hist, samples, count);
}

//-----------------------------------------------------------------------------
// Rendering Functions
//-----------------------------------------------------------------------------

// Internal render function
static void histogram_render_internal(histogram_state_t *hist, Rectangle bounds, Color color) {
    if (!hist || !hist->initialized) {
        // Draw "No Histogram" message if state is invalid
        const char *text = "No Histogram";
        int text_width = gui_text_measure(text, FONT_SIZE_OSC_MSG);
        gui_text_draw(text, bounds.x + bounds.width/2 - text_width/2,
                      bounds.y + bounds.height/2 - 12,
                      FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;

    // Draw background
    DrawRectangle((int)x, (int)y, (int)w, (int)h, COLOR_METER_BG);

    // Calculate bin rendering parameters - labels drawn inside panel at bottom
    int panel_width = (int)w;
    int num_bins = hist->num_bins;
    float label_height = 18.0f;  // Height reserved for bottom labels
    float top_margin = 1.0f;     // Small margin at top for border
    float bottom_margin = 1.0f;  // Small margin at bottom above labels
    float drawable_height = h - label_height - top_margin - bottom_margin;
    float draw_y_start = y + top_margin;  // Y position where bars start (top)

    // Find actual maximum
    float actual_max = 0.0f;
    for (int i = 0; i < num_bins; i++) {
        if (hist->bins[i] > actual_max) actual_max = hist->bins[i];
    }
    if (actual_max < 0.001f) actual_max = 1.0f;

    // Calculate scaling - limit to 95% of drawable height for headroom
    float max_height_ratio = 0.95f;
    float height_scale, log_max = 0.0f;
    if (s_use_log_scale) {
        log_max = log10f(1.0f + actual_max * 1000.0f);
        if (log_max < 0.001f) log_max = 1.0f;
        height_scale = (drawable_height * max_height_ratio) / log_max;
    } else {
        height_scale = (drawable_height * max_height_ratio) / actual_max;
    }

    // Draw vertical grid lines (matching FFT/oscilloscope style)
    // Center line (0V position) - major grid
    float center_x = x + w / 2.0f;
    float draw_y_end = draw_y_start + drawable_height;
    DrawLineV((Vector2){center_x, draw_y_start}, (Vector2){center_x, draw_y_end}, COLOR_GRID_MAJOR);

    // Quarter grid lines (at -0.5 and +0.5)
    float quarter_x = x + w / 4.0f;
    float three_quarter_x = x + 3.0f * w / 4.0f;
    DrawLineV((Vector2){quarter_x, draw_y_start}, (Vector2){quarter_x, draw_y_end}, COLOR_GRID);
    DrawLineV((Vector2){three_quarter_x, draw_y_start}, (Vector2){three_quarter_x, draw_y_end}, COLOR_GRID);

    // Draw bars - bars grow upward from draw_y_end
    int max_bar_height = (int)drawable_height;  // Clamp to drawable area

    if (num_bins <= panel_width) {
        float bar_width = (float)panel_width / (float)num_bins;
        for (int bin = 0; bin < num_bins; bin++) {
            float intensity = hist->bins[bin];
            int bar_height;
            if (s_use_log_scale) {
                bar_height = (int)(log10f(1.0f + intensity * 1000.0f) * height_scale);
            } else {
                bar_height = (int)(intensity * height_scale);
            }
            if (bar_height < 1 && intensity > 0.0001f) bar_height = 1;
            if (bar_height > max_bar_height) bar_height = max_bar_height;  // Clamp

            if (bar_height > 0) {
                int bar_x = (int)(x + bin * bar_width);
                int bar_x_next = (int)(x + (bin + 1) * bar_width);
                int actual_width = bar_x_next - bar_x;
                if (actual_width < 1) actual_width = 1;
                int bar_y = (int)(draw_y_end - bar_height);
                DrawRectangle(bar_x, bar_y, actual_width, bar_height, color);
            }
        }
    } else {
        for (int px = 0; px < panel_width; px++) {
            int bin_start = (px * num_bins) / panel_width;
            int bin_end = ((px + 1) * num_bins) / panel_width;
            if (bin_end > num_bins) bin_end = num_bins;
            if (bin_end <= bin_start) bin_end = bin_start + 1;

            float max_val = 0.0f;
            for (int b = bin_start; b < bin_end && b < num_bins; b++) {
                if (hist->bins[b] > max_val) max_val = hist->bins[b];
            }

            int bar_height;
            if (s_use_log_scale) {
                bar_height = (int)(log10f(1.0f + max_val * 1000.0f) * height_scale);
            } else {
                bar_height = (int)(max_val * height_scale);
            }
            if (bar_height < 1 && max_val > 0.0001f) bar_height = 1;
            if (bar_height > max_bar_height) bar_height = max_bar_height;  // Clamp

            if (bar_height > 0) {
                int bar_x = (int)x + px;
                int bar_y = (int)(draw_y_end - bar_height);
                DrawRectangle(bar_x, bar_y, 1, bar_height, color);
            }
        }
    }

    // Draw horizontal line at bottom of histogram area (above labels)
    DrawLine((int)x, (int)draw_y_end, (int)(x + w), (int)draw_y_end, COLOR_GRID);

    // Border (matching FFT/oscilloscope style)
    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);

    // Draw axis labels with monospace font (matching FFT style)
    // Labels are drawn at the bottom inside the panel, below the separator line
    float label_y = draw_y_end + 2;

    // "-1" label at left
    gui_text_draw_mono("-1", x + 4, label_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);

    // "-0.5" label at quarter
    int half_neg_w = gui_text_measure_mono("-0.5", FONT_SIZE_OSC_SCALE);
    gui_text_draw_mono("-0.5", quarter_x - half_neg_w / 2, label_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);

    // "0" label at center
    int zero_w = gui_text_measure_mono("0", FONT_SIZE_OSC_SCALE);
    gui_text_draw_mono("0", center_x - zero_w / 2, label_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);

    // "+0.5" label at three-quarter
    int half_pos_w = gui_text_measure_mono("+0.5", FONT_SIZE_OSC_SCALE);
    gui_text_draw_mono("+0.5", three_quarter_x - half_pos_w / 2, label_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);

    // "+1" label at right
    int pos_w = gui_text_measure_mono("+1", FONT_SIZE_OSC_SCALE);
    gui_text_draw_mono("+1", x + w - pos_w - 4, label_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);

    // Draw "Histogram" label in top-left corner
    gui_text_draw("Histogram", x + 8, y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);
}

//-----------------------------------------------------------------------------
// Parade (WFM-style RGB raster of the Video panel's decoded picture)
//-----------------------------------------------------------------------------

static const Color s_parade_colors[4] = {
    { 255, 72, 72, 255 }, { 72, 255, 96, 255 }, { 96, 150, 255, 255 }, { 235, 235, 235, 255 },
};

static bool parade_alloc(histogram_panel_state_t *s, int raster_w, int levels, int src_w) {
    if (raster_w != s->raster_w || levels != s->raster_levels) {
        uint32_t *c = realloc(s->counts, (size_t)raster_w * levels * sizeof(uint32_t));
        if (!c) return false;
        s->counts = c;
        uint8_t *px = realloc(s->pixels, (size_t)raster_w * levels * 4u);
        if (!px) return false;
        s->pixels = px;
        s->raster_w = raster_w;
        s->raster_levels = levels;
    }
    if (src_w > s->col_cap) {
        int *m = realloc(s->col_of_x, (size_t)src_w * sizeof(int));
        if (!m) return false;
        s->col_of_x = m;
        s->col_cap = src_w;
    }
    return true;
}

// Count every pixel of the area into [level][section * cols + col] and turn
// the counts into the RGBA raster (top row = level 255).
// Raster rows: the code range (256 / 1024) folded onto at most the plot's
// pixel height, so the GPU never minifies the raster vertically (sparse
// 10-bit rows would alias away).
static int parade_rows(const histogram_panel_state_t *s, int plot_h) {
    const int levels = s->parade_10bit ? PARADE_LEVELS_10 : PARADE_LEVELS_8;
    return plot_h < 16 ? 16 : plot_h < levels ? plot_h : levels;
}

static void parade_build(histogram_panel_state_t *s, const gui_video_frame_view_t *v, int cols, int plot_h) {
    const int sections = s->parade_ch == PARADE_CH_RGB ? 3 : 1;
    const int rx = s->parade_full ? 0 : v->active_x;
    const int ry = s->parade_full ? 0 : v->active_y;
    const int rw = s->parade_full ? v->width : v->active_w;
    const int rh = s->parade_full ? v->height : v->active_h;
    if (cols > rw) cols = rw;
    if (cols < 1 || rh < 1) return;
    const int raster_w = sections * cols;
    const int code_levels = s->parade_10bit ? PARADE_LEVELS_10 : PARADE_LEVELS_8;
    const int levels = parade_rows(s, plot_h);   // raster rows
    if (!parade_alloc(s, raster_w, levels, rw)) return;
    s->raster_cols = cols;
    memset(s->counts, 0, (size_t)raster_w * levels * sizeof(uint32_t));
    // The decoders' display bytes are not code values: their active-area
    // mapping puts 0 IRE black at RF_DISPLAY_BLACK_255 (10) and 100 IRE
    // white at RF_DISPLAY_WHITE_255 (245). Undo it onto the standard scale:
    // 8-bit black 16 / white 235, 10-bit 64 / 940 (harrypm 2026-11-26).
    const bool ten = code_levels == PARADE_LEVELS_10;
    const double c_black = ten ? PARADE_BLACK_10 : PARADE_BLACK_8;
    const double c_span = ten ? PARADE_WHITE_10 - PARADE_BLACK_10 : PARADE_WHITE_8 - PARADE_BLACK_8;
    const double d_span = RF_DISPLAY_WHITE_255 - RF_DISPLAY_BLACK_255;
    uint16_t lv[256];
    for (int c8 = 0; c8 < 256; c8++) {
        int code = (int)lround(c_black + ((double)c8 - RF_DISPLAY_BLACK_255) * c_span / d_span);
        if (code < 0) code = 0;
        if (code > code_levels - 1) code = code_levels - 1;
        lv[c8] = (uint16_t)(code * levels / code_levels);
    }
    // Luma (W) from the weighted sum (display byte * 256), same transfer.
    const double w_scale = c_span / (d_span * 256.0);
    const double w_off = c_black - RF_DISPLAY_BLACK_255 * 256.0 * w_scale + 0.5;
    for (int x = 0; x < rw; x++) s->col_of_x[x] = (int)((int64_t)x * cols / rw);
    uint32_t *counts = s->counts;
    const int ch = s->parade_ch;
    for (int y = 0; y < rh; y++) {
        const uint8_t *row = v->pixels + (size_t)(ry + y) * (size_t)v->stride + (size_t)rx * (size_t)v->bpp;
        if (ch == PARADE_CH_RGB) {
            for (int x = 0; x < rw; x++, row += v->bpp) {
                const int c = s->col_of_x[x];
                counts[(size_t)lv[row[0]] * raster_w + c]++;
                counts[(size_t)lv[row[1]] * raster_w + cols + c]++;
                counts[(size_t)lv[row[2]] * raster_w + 2 * cols + c]++;
            }
        } else if (ch == PARADE_CH_W) {
            for (int x = 0; x < rw; x++, row += v->bpp) {
                // Rec.601 luma (SD picture); the weighted sum keeps its
                // fraction on the 10-bit scale.
                const unsigned sum = 77u * row[0] + 150u * row[1] + 29u * row[2];   // 0..65280
                int code = (int)(w_off + (double)sum * w_scale);
                if (code < 0) code = 0;
                if (code > code_levels - 1) code = code_levels - 1;
                const unsigned l = (unsigned)code * (unsigned)levels / (unsigned)code_levels;
                counts[(size_t)l * raster_w + s->col_of_x[x]]++;
            }
        } else {
            const int off = ch - PARADE_CH_R;
            for (int x = 0; x < rw; x++, row += v->bpp)
                counts[(size_t)lv[row[off]] * raster_w + s->col_of_x[x]]++;
        }
    }
    // Brightness: a column whose pixels all sit on one level saturates; the
    // per-hit floor keeps single outliers visible.
    const float per_col = (float)rh * (float)rw / (float)cols;
    const float k = 48.0f / per_col;
    for (int level = 0; level < levels; level++) {
        const uint32_t *src = counts + (size_t)level * raster_w;
        uint8_t *dst = s->pixels + (size_t)(levels - 1 - level) * raster_w * 4u;
        for (int i = 0; i < raster_w; i++, dst += 4) {
            const int sec = i / cols;
            const Color col = s_parade_colors[ch == PARADE_CH_RGB ? sec : ch == PARADE_CH_W ? 3 : ch - PARADE_CH_R];
            float a = 0.0f;
            if (src[i]) {
                a = 1.0f - expf(-(float)src[i] * k);
                if (a < 0.18f) a = 0.18f;
            }
            dst[0] = col.r; dst[1] = col.g; dst[2] = col.b;
            dst[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
    if (s->tex_valid && (s->tex_w != raster_w || s->tex_h != levels)) {
        UnloadTexture(s->tex);
        s->tex_valid = false;
    }
    if (!s->tex_valid) {
        Image img = { .data = s->pixels, .width = raster_w, .height = levels,
                      .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        s->tex = LoadTextureFromImage(img);
        SetTextureFilter(s->tex, TEXTURE_FILTER_BILINEAR);
        s->tex_w = raster_w;
        s->tex_h = levels;
        s->tex_valid = s->tex.id != 0;
    } else {
        UpdateTexture(s->tex, s->pixels);
    }
}

static void parade_render(histogram_panel_state_t *s, gui_app_t *app, int channel, Rectangle b) {
    DrawRectangleRec(b, COLOR_METER_BG);
    const int sections = s->parade_ch == PARADE_CH_RGB ? 3 : 1;
    const float scale_w = (float)gui_text_measure_mono(s->parade_10bit ? "1023" : "255", FONT_SIZE_OSC_SCALE) + 8.0f;
    Rectangle plot = { b.x + scale_w, b.y + 30.0f, b.width - scale_w - 6.0f, b.height - 36.0f };
    if (plot.width < 24.0f || plot.height < 24.0f) {
        DrawRectangleLinesEx(b, 1, COLOR_GRID_MAJOR);
        return;
    }
    const float gap = sections > 1 ? 6.0f : 0.0f;
    const float sec_w = (plot.width - gap * (float)(sections - 1)) / (float)sections;

    gui_video_frame_view_t view;
    const bool have = gui_video_channel_frame(app, channel, &view);
    const double now = GetTime();
    if (have) {
        int cols = (int)sec_w;
        if (cols > PARADE_MAX_COLS) cols = PARADE_MAX_COLS;
        if (cols < 16) cols = 16;
        const bool layout_changed = !s->tex_valid
            || s->raster_levels != parade_rows(s, (int)plot.height)
            || s->raster_w != sections * (cols > (s->parade_full ? view.width : view.active_w)
                                        ? (s->parade_full ? view.width : view.active_w) : cols);
        const bool fresh = view.generation == 0 || view.generation != s->last_generation;
        if (layout_changed || (fresh && now - s->last_update >= PARADE_UPDATE_S)) {
            parade_build(s, &view, cols, (int)plot.height);
            s->last_update = now;
            s->last_generation = view.generation;
        }
    }
    s->have_frame = have;

    // Graticule in standard code values (harrypm 2026-11-26): limits 0 /
    // max, black 16 / 64 and white 235 / 940 (major, labelled), 25 / 50 /
    // 75 % between black and white (minor; 50 % labelled).
    const int code_max = s->parade_10bit ? 1023 : 255;
    const int g_black = s->parade_10bit ? PARADE_BLACK_10 : PARADE_BLACK_8;
    const int g_white = s->parade_10bit ? PARADE_WHITE_10 : PARADE_WHITE_8;
    const struct { int code; bool major, label; } marks[] = {
        { 0, false, true }, { g_black, true, true },
        { g_black + (g_white - g_black) / 4, false, false },
        { (g_black + g_white + 1) / 2, false, true },
        { g_black + 3 * (g_white - g_black) / 4, false, false },
        { g_white, true, true }, { code_max, false, true },
    };
    const float min_gap = (float)FONT_SIZE_OSC_SCALE + 1.0f;
    const float px_per_code = plot.height / (float)(code_max + 1);
    for (size_t mi = 0; mi < sizeof(marks) / sizeof(marks[0]); mi++) {
        const float gy = plot.y + plot.height - ((float)marks[mi].code + 0.5f) * px_per_code;
        const Color gc = marks[mi].major ? COLOR_GRID_MAJOR : COLOR_GRID;
        for (int sec = 0; sec < sections; sec++) {
            const float sx = plot.x + (float)sec * (sec_w + gap);
            DrawLineV((Vector2){ sx, gy }, (Vector2){ sx + sec_w, gy }, gc);
        }
        if (!marks[mi].label) continue;
        // The 0 / max limit labels give way to black / white when crowded.
        if (!marks[mi].major && (marks[mi].code == 0 || marks[mi].code == code_max)) {
            const int near = marks[mi].code == 0 ? g_black : code_max - g_white;
            if ((float)near * px_per_code < min_gap) continue;
        }
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d", marks[mi].code);
        const int lw = gui_text_measure_mono(lbl, FONT_SIZE_OSC_SCALE);
        gui_text_draw_mono(lbl, plot.x - (float)lw - 4.0f, gy - FONT_SIZE_OSC_SCALE / 2.0f,
                           FONT_SIZE_OSC_SCALE, marks[mi].major ? COLOR_TEXT : COLOR_TEXT_DIM);
    }
    for (int sec = 0; sec < sections; sec++) {
        const Rectangle dst = { plot.x + (float)sec * (sec_w + gap), plot.y, sec_w, plot.height };
        DrawRectangleLinesEx(dst, 1, COLOR_GRID);
        if (have && s->tex_valid && s->raster_cols > 0) {
            const Rectangle src = { (float)(sec * s->raster_cols), 0.0f, (float)s->raster_cols, (float)s->raster_levels };
            DrawTexturePro(s->tex, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        }
        const int ci = s->parade_ch == PARADE_CH_RGB ? sec : s->parade_ch == PARADE_CH_W ? 3 : s->parade_ch - PARADE_CH_R;
        const char *name = s->parade_ch == PARADE_CH_RGB ? s_parade_ch_names[1 + sec] : s_parade_ch_names[s->parade_ch];
        gui_text_draw(name, dst.x + 4.0f, dst.y + 2.0f, FONT_SIZE_OSC_SCALE, s_parade_colors[ci]);
    }
    if (!have) {
        const char *msg = "Parade: no Video panel on this channel";
        const int mw = gui_text_measure(msg, FONT_SIZE_OSC_MSG);
        gui_text_draw(msg, b.x + b.width / 2 - mw / 2, b.y + b.height / 2 - 12, FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
    }
    DrawRectangleLinesEx(b, 1, COLOR_GRID_MAJOR);
    gui_text_draw("Histogram", b.x + 8, b.y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);
}

static void histogram_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                                    Rectangle bounds, Color channel_color) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    if (state->mode == HIST_MODE_PARADE) {
        parade_render(state, app, channel, bounds);
        return;
    }
    histogram_render_internal(&state->hist, bounds, channel_color);
}

// Overlay text button ending at *right_x (moved left past it + a gap).
static Rectangle hist_draw_button(const char *text, float *right_x, float y) {
    const float h = 18.0f;
    const float w = (float)gui_text_measure(text, FONT_SIZE_DROPDOWN_OPT) + 14.0f;
    Rectangle r = { *right_x - w, y, w, h };
    const bool hover = CheckCollisionPointRec(gui_ui_get_mouse_position(), r);
    DrawRectangleRounded(r, 0.15f, 4, hover ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
    gui_text_draw(text, r.x + 7.0f, y + (h - FONT_SIZE_DROPDOWN_OPT) / 2, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    *right_x = r.x - 6.0f;
    return r;
}

static void histogram_vtable_render_overlay(void *state_ptr, Rectangle bounds) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_state_t *hist = &state->hist;

    if (!hist->initialized) return;
    state->mode_rect = state->ch_rect = state->area_rect = state->range_rect = (Rectangle){ 0 };

    // Double-click on the panel (histogram_vtable_handle_click) toggles ALL
    // the overlay chrome — the Bins dropdown + its label — for a clean view;
    // the histogram bars + axis labels (drawn by the data render path) keep
    // rendering.
    if (gui_chrome_hide_hidden(&state->chrome_hide)) return;

    if (state->mode == HIST_MODE_PARADE) {
        state->button_rect = (Rectangle){ 0 };
        state->dropdown_open = false;
        char area[24], ch[16];
        snprintf(area, sizeof(area), "Area: %s", state->parade_full ? "Full" : "Active");
        snprintf(ch, sizeof(ch), "Ch: %s", s_parade_ch_names[state->parade_ch]);
        float rx = bounds.x + bounds.width - 8.0f;
        state->range_rect = hist_draw_button(state->parade_10bit ? "Range: 10-bit" : "Range: 8-bit", &rx, bounds.y + 8.0f);
        state->area_rect = hist_draw_button(area, &rx, bounds.y + 8.0f);
        state->ch_rect = hist_draw_button(ch, &rx, bounds.y + 8.0f);
        state->mode_rect = hist_draw_button("Mode: Parade", &rx, bounds.y + 8.0f);
        return;
    }

    // Format current bin count
    char bins_str[16];
    snprintf(bins_str, sizeof(bins_str), "%d", hist->num_bins);

    // Button dimensions
    float btn_w = 55, btn_h = 18;
    float btn_x = bounds.x + bounds.width - btn_w - 8;
    float btn_y = bounds.y + 8;

    // Draw "Bins:" label before the dropdown button
    const char *bins_label = "Bins:";
    int bins_label_w = gui_text_measure(bins_label, FONT_SIZE_DROPDOWN_OPT);
    float label_x = btn_x - bins_label_w - 6;
    float label_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(bins_label, label_x, label_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);
    float mode_rx = label_x - 8.0f;
    state->mode_rect = hist_draw_button("Mode: Amplitude", &mode_rx, btn_y);

    state->button_rect = (Rectangle){btn_x, btn_y, btn_w, btn_h};

    // Draw button
    Color btn_bg = state->dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(state->button_rect, 0.15f, 4, btn_bg);

    // Draw text centered with arrow
    int text_w = gui_text_measure(bins_str, FONT_SIZE_DROPDOWN_OPT);
    float arrow_w = 8;
    float total_w = text_w + arrow_w + 4;
    float text_x = btn_x + (btn_w - total_w) / 2;
    float text_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(bins_str, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Draw arrow
    float arrow_size = 5.0f;
    float arrow_x = text_x + text_w + 6;
    float arrow_cy = btn_y + btn_h / 2;
    if (state->dropdown_open) {
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

    // Draw dropdown options if open
    if (state->dropdown_open) {
        float opt_y = btn_y + btn_h;
        float opt_h = 20;

        DrawRectangleRounded((Rectangle){btn_x, opt_y, btn_w, opt_h * s_num_bin_options},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < s_num_bin_options; i++) {
            Rectangle opt_rect = {btn_x, opt_y + i * opt_h, btn_w, opt_h};
            state->options_rect[i] = opt_rect;

            bool is_selected = (hist->num_bins == s_bin_options[i]);
            Vector2 mouse = gui_ui_get_mouse_position();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            char opt_str[16];
            snprintf(opt_str, sizeof(opt_str), "%d", s_bin_options[i]);

            int opt_text_w = gui_text_measure(opt_str, FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = btn_x + btn_w/2 - opt_text_w/2;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
            gui_text_draw(opt_str, opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

//-----------------------------------------------------------------------------
// Interaction Functions
//-----------------------------------------------------------------------------

// Bins dropdown clicks. Returns true when a control consumed the click
// (button hit or an open dropdown's option); false otherwise (an open
// dropdown's close-outside click just closes it and reports not consumed).
static bool histogram_handle_bins_click(histogram_panel_state_t *state, Vector2 click) {

    // Check button click
    if (CheckCollisionPointRec(click, state->button_rect)) {
        state->dropdown_open = !state->dropdown_open;
        return true;
    }

    // Check option clicks if open
    if (state->dropdown_open) {
        for (int i = 0; i < s_num_bin_options; i++) {
            if (CheckCollisionPointRec(click, state->options_rect[i])) {
                histogram_set_num_bins(&state->hist, s_bin_options[i]);
                state->dropdown_open = false;
                return true;
            }
        }
        // Click outside dropdown closes it
        state->dropdown_open = false;
    }

    return false;
}

static bool histogram_vtable_handle_click(void *state_ptr, gui_app_t *app, int channel,
                                          Vector2 click, Rectangle bounds) {
    (void)app;
    (void)channel;

    if (!state_ptr) return false;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;

    // Click outside the panel resets double-click tracking.
    if (!CheckCollisionPointRec(click, bounds)) {
        gui_chrome_hide_reset(&state->chrome_hide);
        return false;
    }

    // Bins dropdown clicks only while the overlay is shown (its rectangles
    // are stale while hidden); a consumed control click resets the tracker
    // so a double-click ON the dropdown never toggles the chrome.
    if (!gui_chrome_hide_hidden(&state->chrome_hide)) {
        bool hit = true;
        if (CheckCollisionPointRec(click, state->mode_rect)) {
            state->mode = state->mode == HIST_MODE_PARADE ? HIST_MODE_AMPLITUDE : HIST_MODE_PARADE;
            state->dropdown_open = false;
            if (state->mode == HIST_MODE_AMPLITUDE) histogram_clear(&state->hist);
        } else if (CheckCollisionPointRec(click, state->ch_rect)) {
            state->parade_ch = (state->parade_ch + 1) % PARADE_CH_COUNT;
            state->last_update = 0.0;
        } else if (CheckCollisionPointRec(click, state->range_rect)) {
            state->parade_10bit = !state->parade_10bit;
            state->last_update = 0.0;
        } else if (CheckCollisionPointRec(click, state->area_rect)) {
            state->parade_full = !state->parade_full;
            state->last_update = 0.0;
        } else {
            hit = false;
        }
        if (hit) {
            state->last_generation = 0;
            gui_chrome_hide_reset(&state->chrome_hide);
            return true;
        }
    }
    if (!gui_chrome_hide_hidden(&state->chrome_hide) &&
        histogram_handle_bins_click(state, click)) {
        gui_chrome_hide_reset(&state->chrome_hide);
        return true;
    }

    // Double-click anywhere else on the panel toggles ALL overlay chrome
    // (the Bins dropdown + label) for a clean histogram view.
    if (gui_chrome_hide_feed_click(&state->chrome_hide, click)) {
        state->dropdown_open = false;
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Menu Functions
//-----------------------------------------------------------------------------

static void histogram_menu_on_select(void *state_ptr, int value) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_set_num_bins(&state->hist, value);
    state->dropdown_open = false;
}

static size_t histogram_vtable_get_menu_count(void *state_ptr) {
    (void)state_ptr;
    return 1;  // One menu: bin count selector
}

static panel_menu_t histogram_vtable_get_menu(void *state_ptr, size_t index) {
    panel_menu_t empty = {0};
    if (index != 0 || !state_ptr) return empty;

    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_state_t *hist = &state->hist;

    // Populate menu items
    for (int i = 0; i < s_num_bin_options; i++) {
        char label[16];
        snprintf(label, sizeof(label), "%d", s_bin_options[i]);
        // Note: label string is static, but we need persistent storage
        // For now, we'll use the format in the caller
        state->menu_items[i].label = NULL;  // Caller formats
        state->menu_items[i].value = s_bin_options[i];
        state->menu_items[i].selected = (hist->num_bins == s_bin_options[i]);
    }

    panel_menu_t menu = {
        .title = "Bins",
        .items = state->menu_items,
        .count = s_num_bin_options,
        .on_select = histogram_menu_on_select
    };

    return menu;
}

//-----------------------------------------------------------------------------
// Vtable Definition
//-----------------------------------------------------------------------------

// Double-click chrome-hide state, for the grid's pop-out button.
static bool histogram_vtable_chrome_hidden(void *state) {
    const histogram_panel_state_t *p = (const histogram_panel_state_t *)state;
    return p && gui_chrome_hide_hidden(&p->chrome_hide);
}

static const panel_vtable_t s_histogram_vtable = {
    .chrome_hidden = histogram_vtable_chrome_hidden,
    .name = "Histogram",

    // Lifecycle
    .create = histogram_vtable_create,
    .destroy = histogram_vtable_destroy,
    .clear = histogram_vtable_clear,

    // Processing
    .process = histogram_vtable_process,

    // Rendering
    .render = histogram_vtable_render,
    .render_overlay = histogram_vtable_render_overlay,

    // Interaction
    .handle_click = histogram_vtable_handle_click,
    .handle_scroll = NULL,

    // Menus
    .get_menu_count = histogram_vtable_get_menu_count,
    .get_menu = histogram_vtable_get_menu,
};

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------

void gui_histogram_panel_register(void) {
    panel_register(PANEL_VIEW_HISTOGRAM, &s_histogram_vtable);
}
