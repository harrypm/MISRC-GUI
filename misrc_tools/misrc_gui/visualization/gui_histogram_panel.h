/*
 * MISRC GUI - Histogram Panel Rendering
 *
 * Renders amplitude histogram as a panel view with heatmap coloring.
 * Integrates with the unified panel interface system.
 */

#ifndef GUI_HISTOGRAM_PANEL_H
#define GUI_HISTOGRAM_PANEL_H

#include "raylib.h"
#include "../signal/gui_histogram.h"
#include "panel_interface.h"

// Forward declaration
struct gui_app;

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the histogram panel vtable with the panel registry.
// Call this once at startup.
void gui_histogram_panel_register(void);

// --histogram <key=value,...>: launch state of new Histogram panels
// (mode=amplitude|parade, channel=rgb|r|g|b|w, area=active|full, range=8|10,
// bins=64..4096). Prints the accepted list and returns false on error.
bool gui_histogram_panel_apply_launch_arg(const char *arg);

//-----------------------------------------------------------------------------
// Histogram Panel Processing (called from display thread)
//-----------------------------------------------------------------------------

// Process histograms for all active histogram panels.
// Iterates through panel configs and calls histogram_process for each.
// Call this from the display thread with the raw sample data.
//
// Parameters:
//   app: Application state (contains panel configurations)
//   samples_a: Channel A samples
//   samples_b: Channel B samples
//   count: Number of samples per channel
void gui_histogram_panel_process_all(struct gui_app *app,
                                     const int16_t *samples_a,
                                     const int16_t *samples_b,
                                     size_t count);

#endif // GUI_HISTOGRAM_PANEL_H
