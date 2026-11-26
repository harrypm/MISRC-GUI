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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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
} histogram_panel_state_t;

//-----------------------------------------------------------------------------
// Histogram Panel Processing (called from display thread)
//-----------------------------------------------------------------------------

// Helper to process histogram for a single panel config
static void process_panel_histograms(const channel_panel_config_t *config,
                                     const int16_t *samples,
                                     size_t count) {
    if (!config) return;

    // Check left panel
    if (config->left_view == PANEL_VIEW_HISTOGRAM && config->left_state) {
        histogram_process((histogram_state_t *)config->left_state, samples, count);
    }

    // Check right panel (only if split mode is active)
    if (config->split && config->right_view == PANEL_VIEW_HISTOGRAM && config->right_state) {
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
    return state;
}

static void histogram_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_cleanup(&state->hist);
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

static void histogram_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                                    Rectangle bounds, Color channel_color) {
    (void)app;
    (void)channel;

    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_render_internal(&state->hist, bounds, channel_color);
}

static void histogram_vtable_render_overlay(void *state_ptr, Rectangle bounds) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_state_t *hist = &state->hist;

    if (!hist->initialized) return;

    // Double-click on the panel (histogram_vtable_handle_click) toggles ALL
    // the overlay chrome — the Bins dropdown + its label — for a clean view;
    // the histogram bars + axis labels (drawn by the data render path) keep
    // rendering.
    if (gui_chrome_hide_hidden(&state->chrome_hide)) return;

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
