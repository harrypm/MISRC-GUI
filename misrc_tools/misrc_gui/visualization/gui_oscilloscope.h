/*
 * MISRC GUI - Waveform Panel
 *
 * Waveform panel vtable registration, grid drawing, and trigger detection.
 * Uses libsoxr for high-quality waveform resampling.
 */

#ifndef GUI_OSCILLOSCOPE_H
#define GUI_OSCILLOSCOPE_H

#include "../core/gui_app.h"
#include "raylib.h"
#include <sys/types.h>
#include "panel_interface.h"

//-----------------------------------------------------------------------------
// Panel Registration
//-----------------------------------------------------------------------------

void gui_waveform_panel_register(void);

//-----------------------------------------------------------------------------
// Phosphor Parameters
//-----------------------------------------------------------------------------

#define SCOPE_DECAY_RATE        0.75f   // Per-frame decay (higher = slower fade)
#define SCOPE_HIT_INCREMENT     0.25f   // Intensity added per waveform hit
#define SCOPE_BLOOM             2.0f    // Bloom factor

//-----------------------------------------------------------------------------
// Cleanup
//-----------------------------------------------------------------------------

void gui_oscilloscope_cleanup(void);

//-----------------------------------------------------------------------------
// Grid Drawing
//-----------------------------------------------------------------------------

// Draw grid with amplitude scale ticks and time labels
void draw_channel_grid(float x, float y, float width, float height,
                       const char *label, Color channel_color, bool show_grid,
                       float zoom_scale, uint32_t sample_rate,
                       bool trigger_enabled, int trigger_display_pos);

//-----------------------------------------------------------------------------
// Trigger Detection
//-----------------------------------------------------------------------------

// Find first trigger point in sample buffer
// Returns sample index of trigger point, or -1 if no trigger found
ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig);

#endif // GUI_OSCILLOSCOPE_H
