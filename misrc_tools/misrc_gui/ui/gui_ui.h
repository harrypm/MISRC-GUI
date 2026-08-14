#ifndef GUI_UI_H
#define GUI_UI_H

#include "../core/gui_app.h"
#include "clay.h"

// UI colors
#define COLOR_BG              (Color){ 30, 30, 35, 255 }
#define COLOR_PANEL_BG        (Color){ 40, 40, 48, 255 }
#define COLOR_TOOLBAR_BG      (Color){ 50, 50, 58, 255 }
#define COLOR_BUTTON          (Color){ 65, 65, 75, 255 }
#define COLOR_BUTTON_HOVER    (Color){ 80, 80, 92, 255 }
#define COLOR_BUTTON_ACTIVE   (Color){ 95, 95, 110, 255 }
#define COLOR_TEXT            (Color){ 220, 220, 230, 255 }
#define COLOR_TEXT_DIM        (Color){ 140, 140, 155, 255 }
#define COLOR_CHANNEL_A       (Color){ 80, 220, 100, 255 }
#define COLOR_CHANNEL_B       (Color){ 220, 200, 80, 255 }
#define COLOR_SYNC_GREEN      (Color){ 50, 200, 80, 255 }
#define COLOR_SYNC_RED        (Color){ 200, 60, 60, 255 }
#define COLOR_CLIP_RED        (Color){ 255, 50, 50, 255 }
#define COLOR_GRID            (Color){ 90, 90, 110, 130 }
#define COLOR_GRID_MAJOR      (Color){ 130, 130, 160, 180 }
#define COLOR_METER_BG        (Color){ 25, 25, 30, 255 }
#define COLOR_METER_GREEN     (Color){ 0, 255, 0, 255 }
#define COLOR_METER_YELLOW    (Color){ 255, 230, 0, 255 }
#define COLOR_METER_RED       (Color){ 255, 0, 0, 255 }

// Font sizes - adjust these to change all UI text sizes
#define FONT_SIZE_TITLE        26    // Main title "MISRC Capture"
#define FONT_SIZE_HEADING      20    // Section headings like "Statistics"
#define FONT_SIZE_NORMAL       18    // Normal UI text, buttons, labels
#define FONT_SIZE_DROPDOWN     18    // Dropdown menu headers
#define FONT_SIZE_DROPDOWN_OPT 16   // Dropdown menu option items
#define FONT_SIZE_STATUS       18    // Status bar text
#define FONT_SIZE_OSC_LABEL    20    // Oscilloscope channel labels "CH A", "CH B"
#define FONT_SIZE_OSC_SCALE    16    // Oscilloscope scale ticks "+1", "0", "-1"
#define FONT_SIZE_OSC_DIV      18    // Oscilloscope division labels "1ms/div", "1kHz/div"
#define FONT_SIZE_OSC_MSG      26    // Oscilloscope messages like "No Signal"
#define FONT_SIZE_VU_SCALE     16    // VU meter scale ticks
#define FONT_SIZE_VU_CLIP      14    // VU meter clip indicators "+CLIP", "-CLIP"
#define FONT_SIZE_STATS_LABEL  18    // Per-channel stats panel channel label
#define FONT_SIZE_STATS        16    // Per-channel stats panel text

// UI Layout functions
void gui_render_layout(gui_app_t *app);
void gui_handle_interactions(gui_app_t *app);
void gui_ui_sync_android_keyboard_state(void);

// Check if UI consumed the current frame's click (prevents click-through to oscilloscope)
bool gui_ui_click_consumed(void);

// Text measurement function (from clay_renderer_raylib.c)
Clay_Dimensions Raylib_MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData);

// Raylib render function (from clay_renderer_raylib.c)
void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font* fonts);

#endif // GUI_UI_H
