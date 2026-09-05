#ifndef GUI_UI_SCALE_H
#define GUI_UI_SCALE_H

#include <stdbool.h>

#define GUI_UI_SCALE_MIN_PERCENT 75
#define GUI_UI_SCALE_MAX_PERCENT 200
#define GUI_UI_SCALE_DEFAULT_PERCENT 100
#define GUI_UI_SCALE_STEP_PERCENT 10
#define GUI_UI_SCALE_HUD_DURATION_S 1.5
#define GUI_UI_SCALE_HUD_FADE_S 0.3
#define GUI_UI_MODAL_MARGIN 12
#define GUI_UI_VU_BAR_WIDTH 35.0f
// Compact (short-label) readouts kick in below this width. Keep this below
// the default startup window so full labels remain visible at stock size and
// compact labels only appear once width pressure is real.
#define GUI_UI_STATUS_COMPACT_BREAKPOINT 1360
#define GUI_UI_STATUS_RECORDING_FULL_BREAKPOINT 1425
#define GUI_UI_STATUS_RECORDING_MINIMAL_BREAKPOINT 920
#define GUI_UI_STATUS_NARROW_BREAKPOINT 960
#define GUI_UI_STATUS_RECORDING_NARROW_BREAKPOINT 1100
#define GUI_UI_STATUS_TINY_BREAKPOINT 760
#define GUI_UI_STATUS_QUARTER_MAX_WIDTH 1000
#define GUI_UI_STATUS_QUARTER_MAX_HEIGHT 700

typedef enum gui_ui_status_layout_mode {
    GUI_UI_STATUS_LAYOUT_FULL_SINGLE,
    GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
    GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE,
} gui_ui_status_layout_mode_t;

typedef struct gui_ui_zoom_state {
    float wheel_remainder;
} gui_ui_zoom_state_t;

typedef struct gui_ui_zoom_result {
    int percent;
    float passthrough_x;
    float passthrough_y;
    bool consumed;
    bool step_attempted;
    bool changed;
} gui_ui_zoom_result_t;

// Invalid persisted values fall back to 100% so a damaged settings file cannot
// leave the UI permanently too small or too large to operate.
int gui_ui_scale_sanitize_percent(int percent);

// Parses the integer JSON value used by settings persistence. Malformed,
// trailing, or out-of-range input returns the safe 100% default.
int gui_ui_scale_parse_percent(const char *text);

// Applies one keyboard/wheel zoom step while preserving the special 75%-80%
// transition and the configured scale bounds. A zero direction is a no-op.
int gui_ui_scale_step_percent(int current_percent, int direction);

// Only the channel stats panel width grows by 60% of zoom above 100%.
// Return its width relative to the globally scaled UI; text and controls
// keep the global scale. Zoom-out and invalid values stay unchanged.
float gui_ui_stats_width_scale(int percent);

typedef struct gui_ui_channel_spacing {
    float vu_column_width;
    int horizontal_gap;
} gui_ui_channel_spacing_t;

// Follow the status bar's viewport-only compact-label boundary, so live
// values and capture state cannot resize the channel area. Compact channels
// cap empty margins in physical pixels without shrinking the logical VU bar;
// render_scale_x includes OS backing scale. Full labels keep the old spacing.
gui_ui_channel_spacing_t gui_ui_get_channel_spacing(bool compact,
                                                     float render_scale_x);

// Returns the transient zoom HUD opacity from its remaining display time.
// The HUD stays opaque until the final fade interval, then reaches zero at
// the deadline.
float gui_ui_scale_hud_opacity(double remaining_seconds);

// Caps a modal extent to the scale-adjusted logical viewport while retaining
// a small margin on both sides. The result is always at least one pixel.
int gui_ui_modal_max_extent(int layout_extent, int configured_max);

// Returns true when the measured single-row toolbar would exceed the
// scale-adjusted logical viewport.
bool gui_ui_toolbar_uses_two_rows(int layout_width,
                                  int single_row_required_width);

// Chooses a deterministic status-bar layout from scale-adjusted logical
// dimensions. Recording reserves extra width for its timer/runway, while
// labels compact and lower-priority counters disappear before overflow. Very
// small/short layouts keep one minimal row to preserve plot height.
gui_ui_status_layout_mode_t gui_ui_get_status_layout_mode(int layout_width,
                                                          int layout_height,
                                                          bool is_recording);

// Error text is the only state important enough to add a second row. Minimal
// layouts remain single-row to protect the remaining plot height.
bool gui_ui_status_uses_two_rows(gui_ui_status_layout_mode_t layout_mode,
                                 bool status_is_error);

// Extended frame/missed/error counters are hidden below this width so a
// normal compact status bar can remain on one line.
bool gui_ui_status_shows_extended_counters(int layout_width,
                                           bool is_recording);

// Nonzero stream faults still need a compact indicator when the individual
// counters are hidden by the measured budget, not a width breakpoint.
// Critical stop messages retain their existing priority.
bool gui_ui_status_uses_fault_summary(bool has_missed, bool has_errors,
                                      bool show_missed, bool show_errors);

// Routes one frame of wheel input. Vertical Ctrl/Cmd+wheel is accumulated into
// discrete scale steps and consumed so it cannot also scroll Clay or a panel.
gui_ui_zoom_result_t gui_ui_zoom_process(gui_ui_zoom_state_t *state,
                                         int current_percent,
                                         bool primary_modifier_down,
                                         float wheel_x,
                                         float wheel_y);

#endif // GUI_UI_SCALE_H
