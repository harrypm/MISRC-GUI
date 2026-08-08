/*
 * MISRC GUI - Custom Clay Element Types
 *
 * Shared definitions for custom Clay rendering elements.
 * Used by both gui_ui.c (element creation) and clay_renderer_raylib.c (rendering).
 */

#ifndef GUI_CUSTOM_ELEMENTS_H
#define GUI_CUSTOM_ELEMENTS_H

#include <stdbool.h>
#include "raylib.h"

// Forward declarations
typedef struct gui_app gui_app_t;
typedef struct vu_meter_state vu_meter_state_t;

//-----------------------------------------------------------------------------
// Custom Element Types
//-----------------------------------------------------------------------------

typedef enum {
    CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL,  // Channel panel area (waveform, FFT, histogram, etc.)
    CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER,
    CUSTOM_LAYOUT_ELEMENT_TYPE_SETTINGS_ICON,
    CUSTOM_LAYOUT_ELEMENT_TYPE_CLOCK_ICON
} CustomLayoutElementType;

//-----------------------------------------------------------------------------
// Custom Element Data Structures
//-----------------------------------------------------------------------------

typedef struct {
    gui_app_t *app;
    int channel;  // 0 = A, 1 = B
} CustomLayoutElement_ChannelPanel;

typedef struct {
    vu_meter_state_t *meter;
    const char *label;
    bool is_clipping_pos;
    bool is_clipping_neg;
    Color channel_color;
} CustomLayoutElement_VUMeter;

typedef struct {
    CustomLayoutElementType type;
    union {
        CustomLayoutElement_ChannelPanel channel_panel;
        CustomLayoutElement_VUMeter vu_meter;
    } customData;
} CustomLayoutElement;

#endif // GUI_CUSTOM_ELEMENTS_H
