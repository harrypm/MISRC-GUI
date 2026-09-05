/*
 * MISRC GUI - VU Meter Rendering
 *
 * Bipolar VU meter for AC-coupled signals with smooth gradient bars.
 */

#include "gui_vu_meter.h"
#include "gui_text.h"
#include "../ui/gui_ui.h"
#include <math.h>

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static Color lerp_color(Color a, Color b, float t) {
    t = clamp01(t);
    Color out;
    out.r = (unsigned char)(a.r + (b.r - a.r) * t);
    out.g = (unsigned char)(a.g + (b.g - a.g) * t);
    out.b = (unsigned char)(a.b + (b.b - a.b) * t);
    out.a = (unsigned char)(a.a + (b.a - a.a) * t);
    return out;
}

// Smooth green->yellow->red gradient by distance from center (0 = center, 1 = edge)
static Color meter_gradient_color(float t) {
    t = clamp01(t);
    const float mid = 0.55f; // green→yellow transition
    if (t < mid) {
        float lt = t / mid;
        return lerp_color(COLOR_METER_GREEN, COLOR_METER_YELLOW, lt);
    }
    float lt = (t - mid) / (1.0f - mid);
    return lerp_color(COLOR_METER_YELLOW, COLOR_METER_RED, lt);
}

// Draw one direction of the meter with a continuous vertical gradient
static void draw_meter_bar(float meter_x, float meter_width, float center_y,
                           float half_height, float level, bool going_up) {
    level = clamp01(level);
    float bar_extent = level * half_height;
    if (bar_extent < 1.0f) return;

    float x = meter_x + 1.0f;
    float w = meter_width - 2.0f;
    Color inner = meter_gradient_color(0.0f);    // at center
    Color outer = meter_gradient_color(level);    // at bar edge

    if (going_up) {
        float y = center_y - bar_extent;
        DrawRectangleGradientV((int)x, (int)y, (int)w, (int)bar_extent, outer, inner);
    } else {
        float y = center_y;
        DrawRectangleGradientV((int)x, (int)y, (int)w, (int)bar_extent, inner, outer);
    }
}

// Bipolar VU meter for AC-coupled signals
void render_vu_meter(float x, float y, float width, float height,
                     vu_meter_state_t *meter, const char *label,
                     bool is_clipping_pos, bool is_clipping_neg, Color channel_color) {
    (void)label;         // Not used - label comes from layout
    (void)channel_color; // Not used
    (void)is_clipping_pos;
    (void)is_clipping_neg;

    float padding = 0.0f;

    // Only the surrounding column spacing contracts in compact layouts.
    // Keep the visible bar at its original logical width and centered.
    float available_w = width - 2 * padding;
    float meter_width = fminf(available_w, GUI_UI_VU_BAR_WIDTH);
    float meter_x = x + padding + (available_w - meter_width) * 0.5f;
    float meter_y = y + padding;
    float meter_height = height - 2 * padding;

    float center_y = meter_y + meter_height / 2.0f;
    float half_height = meter_height / 2.0f;

    // Background
    DrawRectangle((int)meter_x, (int)meter_y, (int)meter_width, (int)meter_height, COLOR_METER_BG);

    // Center line (0 reference)
    DrawLineEx((Vector2){meter_x, center_y}, (Vector2){meter_x + meter_width, center_y},
               2.0f, (Color){120, 130, 150, 180});

    // Draw positive bar (upward from center) - independent level
    draw_meter_bar(meter_x, meter_width, center_y, half_height, meter->level_pos, true);

    // Draw negative bar (downward from center) - independent level
    draw_meter_bar(meter_x, meter_width, center_y, half_height, meter->level_neg, false);

    // Peak hold indicator for positive (white line going up)
    if (meter->peak_pos > 0.02f) {
        float peak_y = center_y - meter->peak_pos * half_height;
        DrawRectangle((int)meter_x, (int)peak_y - 1, (int)meter_width, 3, WHITE);
    }

    // Peak hold indicator for negative (white line going down)
    if (meter->peak_neg > 0.02f) {
        float peak_y = center_y + meter->peak_neg * half_height;
        DrawRectangle((int)meter_x, (int)peak_y - 1, (int)meter_width, 3, WHITE);
    }

    // Border
    DrawRectangleLinesEx((Rectangle){meter_x, meter_y, meter_width, meter_height}, 1, COLOR_GRID_MAJOR);

    // Scale ticks inside the meter
    float tick_positions[] = { 0.25f, 0.5f, 0.75f };
    for (int i = 0; i < 3; i++) {
        float tick_y = meter_y + meter_height * tick_positions[i];
        // Tick marks on both sides
        DrawLineEx((Vector2){meter_x, tick_y}, (Vector2){meter_x + 3, tick_y}, 1.5f, COLOR_GRID_MAJOR);
        DrawLineEx((Vector2){meter_x + meter_width - 3, tick_y}, (Vector2){meter_x + meter_width, tick_y}, 1.5f, COLOR_GRID_MAJOR);
    }

}
