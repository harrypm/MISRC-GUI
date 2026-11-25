/*
 * MISRC GUI - Panel Abstraction System
 *
 * Provides a flexible panel system for displaying different views
 * (waveform, FFT, etc.) in configurable layouts per channel.
 */

#ifndef GUI_PANEL_H
#define GUI_PANEL_H

#include "raylib.h"
#include <stdbool.h>
#include "../core/gui_app.h"  // For panel_view_type_t and channel_panel_config_t

//-----------------------------------------------------------------------------
// Panel System API
//-----------------------------------------------------------------------------

// Get human-readable name for a view type (for dropdowns)
const char* panel_view_type_name(panel_view_type_t type);
// View picker display order: index 0..PANEL_VIEW_COUNT-1 -> view id (HiFi under Video).
panel_view_type_t panel_view_menu_order(int index);

// Check if a view type is available (e.g., FFT requires FFTW)
bool panel_view_type_available(panel_view_type_t type);

// Create view-specific state for a panel (e.g., fft_state_t for FFT)
// Returns NULL if no state is needed or on failure
void* panel_create_view_state(panel_view_type_t type);

// Destroy view-specific state
void panel_destroy_view_state(panel_view_type_t type, void *state);

// Clear view-specific state (reset without destroying)
void panel_clear_view_state(panel_view_type_t type, void *state);

//-----------------------------------------------------------------------------
// Channel Panel Rendering
//-----------------------------------------------------------------------------

// Render all panels for a channel using the panel configuration
// This is the main entry point for panel rendering
void render_channel_panels(gui_app_t *app, int channel,
                           float x, float y, float width, float height,
                           Color channel_color);

//-----------------------------------------------------------------------------
// Panel Configuration Helpers
//-----------------------------------------------------------------------------

// Initialize panel config to default (single panel, phosphor waveform)
void panel_config_init_default(channel_panel_config_t *config);

// Cleanup panel config (destroy any allocated state)
void panel_config_cleanup(channel_panel_config_t *config);

// Set view for left panel (handles state allocation/deallocation)
void panel_config_set_left_view(channel_panel_config_t *config, panel_view_type_t type);

// Set view for right panel (handles state allocation/deallocation)
void panel_config_set_right_view(channel_panel_config_t *config, panel_view_type_t type);

// Toggle split mode (allocates/deallocates right panel state as needed)
void panel_config_set_split(channel_panel_config_t *config, bool split);

//-----------------------------------------------------------------------------
// Unified Panel Input Handling
//-----------------------------------------------------------------------------

// Handle click events for all panel overlays using vtable dispatch.
// Iterates through both channels' panel configs and checks for clicks.
// Returns true if any panel consumed the click.
bool panel_handle_all_clicks(gui_app_t *app, Vector2 mouse_pos);

// Handle scroll events for all panels using vtable dispatch.
// Checks which panel the mouse is over and calls its scroll handler.
// Returns true if any panel consumed the scroll.
bool panel_handle_all_scrolls(gui_app_t *app, float delta);

#endif // GUI_PANEL_H
