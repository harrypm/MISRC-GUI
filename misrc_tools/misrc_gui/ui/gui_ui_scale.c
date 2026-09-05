#include "gui_ui_scale.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

int gui_ui_scale_sanitize_percent(int percent)
{
    if (percent == GUI_UI_SCALE_MIN_PERCENT) return percent;
    if (percent >= 80 && percent <= GUI_UI_SCALE_MAX_PERCENT &&
        (percent % GUI_UI_SCALE_STEP_PERCENT) == 0) return percent;
    return GUI_UI_SCALE_DEFAULT_PERCENT;
}

int gui_ui_scale_parse_percent(const char *text)
{
    if (!text || text[0] == '\0') return GUI_UI_SCALE_DEFAULT_PERCENT;

    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return GUI_UI_SCALE_DEFAULT_PERCENT;
    }

    return gui_ui_scale_sanitize_percent((int)parsed);
}

int gui_ui_scale_step_percent(int current_percent, int direction)
{
    int percent = gui_ui_scale_sanitize_percent(current_percent);
    if (direction == 0) return percent;

    int next_percent;
    if (direction > 0 && percent == GUI_UI_SCALE_MIN_PERCENT) {
        next_percent = 80;
    } else if (direction < 0 && percent == 80) {
        next_percent = GUI_UI_SCALE_MIN_PERCENT;
    } else {
        next_percent = percent +
            ((direction > 0) ? GUI_UI_SCALE_STEP_PERCENT
                             : -GUI_UI_SCALE_STEP_PERCENT);
    }

    if (next_percent < GUI_UI_SCALE_MIN_PERCENT) {
        next_percent = GUI_UI_SCALE_MIN_PERCENT;
    }
    if (next_percent > GUI_UI_SCALE_MAX_PERCENT) {
        next_percent = GUI_UI_SCALE_MAX_PERCENT;
    }
    return next_percent;
}

float gui_ui_stats_width_scale(int percent)
{
    float scale = (float)gui_ui_scale_sanitize_percent(percent) / 100.0f;
    if (scale <= 1.0f) return 1.0f;
    return (1.0f + (scale - 1.0f) * 0.6f) / scale;
}

gui_ui_channel_spacing_t gui_ui_get_channel_spacing(bool compact,
                                                     float render_scale_x)
{
    gui_ui_channel_spacing_t spacing = { GUI_UI_VU_BAR_WIDTH * 2.0f, 4 };
    if (!compact || !isfinite(render_scale_x) || render_scale_x <= 0.0f) {
        return spacing;
    }

    float margin = fminf(GUI_UI_VU_BAR_WIDTH * 0.5f, 8.0f / render_scale_x);
    spacing.vu_column_width = GUI_UI_VU_BAR_WIDTH + margin * 2.0f;
    spacing.horizontal_gap = (int)fminf(4.0f, floorf(4.0f / render_scale_x));
    return spacing;
}

float gui_ui_scale_hud_opacity(double remaining_seconds)
{
    if (remaining_seconds <= 0.0) return 0.0f;
    if (remaining_seconds >= GUI_UI_SCALE_HUD_FADE_S) return 1.0f;
    return (float)(remaining_seconds / GUI_UI_SCALE_HUD_FADE_S);
}

int gui_ui_modal_max_extent(int layout_extent, int configured_max)
{
    int available = layout_extent - (GUI_UI_MODAL_MARGIN * 2);
    if (available < 1) available = 1;
    if (configured_max < 1) configured_max = 1;
    return (available < configured_max) ? available : configured_max;
}

bool gui_ui_toolbar_uses_two_rows(int layout_width,
                                  int single_row_required_width)
{
    return layout_width < single_row_required_width;
}

gui_ui_status_layout_mode_t gui_ui_get_status_layout_mode(int layout_width,
                                                          int layout_height,
                                                          bool is_recording)
{
    bool quarter_scale =
        layout_width <= GUI_UI_STATUS_QUARTER_MAX_WIDTH &&
        layout_height <= GUI_UI_STATUS_QUARTER_MAX_HEIGHT;

    if (layout_width < GUI_UI_STATUS_TINY_BREAKPOINT || quarter_scale ||
        (is_recording &&
         layout_width < GUI_UI_STATUS_RECORDING_MINIMAL_BREAKPOINT)) {
        return GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE;
    }
    if (layout_width < GUI_UI_STATUS_COMPACT_BREAKPOINT ||
        (is_recording &&
         layout_width < GUI_UI_STATUS_RECORDING_FULL_BREAKPOINT)) {
        return GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE;
    }
    return GUI_UI_STATUS_LAYOUT_FULL_SINGLE;
}

bool gui_ui_status_uses_two_rows(gui_ui_status_layout_mode_t layout_mode,
                                 bool status_is_error)
{
    return status_is_error &&
        layout_mode != GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE;
}

bool gui_ui_status_shows_extended_counters(int layout_width,
                                           bool is_recording)
{
    int breakpoint = is_recording
        ? GUI_UI_STATUS_RECORDING_NARROW_BREAKPOINT
        : GUI_UI_STATUS_NARROW_BREAKPOINT;
    return layout_width >= breakpoint;
}

bool gui_ui_status_uses_fault_summary(bool has_missed, bool has_errors,
                                      bool show_missed, bool show_errors)
{
    return (has_missed && !show_missed) || (has_errors && !show_errors);
}

gui_ui_zoom_result_t gui_ui_zoom_process(gui_ui_zoom_state_t *state,
                                         int current_percent,
                                         bool primary_modifier_down,
                                         float wheel_x,
                                         float wheel_y)
{
    gui_ui_zoom_result_t result = {
        .percent = gui_ui_scale_sanitize_percent(current_percent),
        .passthrough_x = wheel_x,
        .passthrough_y = wheel_y,
        .consumed = false,
        .step_attempted = false,
        .changed = false,
    };

    if (!state) return result;

    if (!primary_modifier_down) {
        state->wheel_remainder = 0.0f;
        return result;
    }

    // Idle frames keep a partial trackpad gesture alive until more vertical
    // motion arrives. A horizontal-dominant gesture remains normal scrolling
    // and cancels any earlier vertical remainder.
    if (wheel_y == 0.0f) {
        if (wheel_x != 0.0f) state->wheel_remainder = 0.0f;
        return result;
    }
    if (fabsf(wheel_x) > fabsf(wheel_y)) {
        state->wheel_remainder = 0.0f;
        return result;
    }

    result.consumed = true;
    result.passthrough_x = 0.0f;
    result.passthrough_y = 0.0f;

    // Do not make a small trackpad movement in the opposite direction fight a
    // previously accumulated gesture.
    if ((state->wheel_remainder > 0.0f && wheel_y < 0.0f) ||
        (state->wheel_remainder < 0.0f && wheel_y > 0.0f)) {
        state->wheel_remainder = 0.0f;
    }
    state->wheel_remainder += wheel_y;

    while (state->wheel_remainder >= 1.0f ||
           state->wheel_remainder <= -1.0f) {
        result.step_attempted = true;
        int direction = (state->wheel_remainder > 0.0f) ? 1 : -1;
        int next_percent = gui_ui_scale_step_percent(result.percent, direction);

        if (next_percent == result.percent) {
            state->wheel_remainder = 0.0f;
            break;
        }

        result.percent = next_percent;
        result.changed = true;
        state->wheel_remainder -= (float)direction;

        if (result.percent == GUI_UI_SCALE_MIN_PERCENT ||
            result.percent == GUI_UI_SCALE_MAX_PERCENT) {
            state->wheel_remainder = 0.0f;
            break;
        }
    }

    return result;
}
