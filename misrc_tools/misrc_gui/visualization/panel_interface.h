/*
 * MISRC GUI - Unified Panel Interface
 *
 * Provides a vtable-based interface for self-contained panels.
 * Each panel handles its own rendering, click callbacks, data processing,
 * and state management through a unified interface.
 */

#ifndef PANEL_INTERFACE_H
#define PANEL_INTERFACE_H

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../core/gui_app.h"

//-----------------------------------------------------------------------------
// Forward Declarations
//-----------------------------------------------------------------------------

typedef struct panel_vtable panel_vtable_t;
typedef struct panel_instance panel_instance_t;

//-----------------------------------------------------------------------------
// Panel Menu System (for overlay dropdowns)
//-----------------------------------------------------------------------------

// Single menu item (e.g., "PAL", "NTSC", "16 bins", etc.)
typedef struct panel_menu_item {
    const char *label;      // Display text
    int value;              // Value to pass to on_select
    bool selected;          // Currently selected item
} panel_menu_item_t;

// Menu definition returned by get_menu()
typedef struct panel_menu {
    const char *title;                              // Menu title (e.g., "System", "Bins")
    panel_menu_item_t *items;                       // Array of menu items
    size_t count;                                   // Number of items
    void (*on_select)(void *state, int value);      // Callback when item selected
} panel_menu_t;

//-----------------------------------------------------------------------------
// Panel Virtual Table (vtable)
//-----------------------------------------------------------------------------

typedef struct panel_vtable {
    const char *name;       // Human-readable name ("Histogram", "FFT", etc.)

    //-------------------------------------------------------------------------
    // Lifecycle
    //-------------------------------------------------------------------------

    // Create panel state. Returns opaque state pointer or NULL on failure.
    void *(*create)(void);

    // Destroy panel state and free resources.
    void (*destroy)(void *state);

    // Clear/reset panel state without destroying (e.g., clear histogram bins).
    void (*clear)(void *state);

    //-------------------------------------------------------------------------
    // Processing (called from display thread with raw ADC samples)
    //-------------------------------------------------------------------------

    // Process raw samples from ADC. Called from display thread.
    // May be NULL if panel doesn't need processing (e.g., waveform panels).
    // Parameters:
    //   state: Panel-specific state
    //   samples: Raw ADC samples (int16_t)
    //   count: Number of samples
    //   sample_rate: ADC sample rate in Hz
    void (*process)(void *state, const int16_t *samples, size_t count, uint32_t sample_rate);

    //-------------------------------------------------------------------------
    // Rendering (called from render thread)
    //-------------------------------------------------------------------------

    // Render the main panel content.
    // Parameters:
    //   state: Panel-specific state (created by vtable->create)
    //   app: Application state (for accessing display samples, trigger, etc.)
    //   channel: Channel index (0 or 1)
    //   bounds: Panel rectangle
    //   color: Channel color
    void (*render)(void *state, struct gui_app *app, int channel,
                   Rectangle bounds, Color color);

    // Render overlay UI (dropdowns, labels, etc.) on top of panel.
    // May be NULL if panel has no overlay.
    void (*render_overlay)(void *state, Rectangle bounds);

    //-------------------------------------------------------------------------
    // Interaction (called from render thread after rendering)
    //-------------------------------------------------------------------------

    // Handle mouse click. Returns true if click was consumed.
    // May be NULL if panel doesn't handle clicks.
    // Parameters:
    //   state: Panel-specific state
    //   app: Application state (for accessing channel settings, etc.)
    //   channel: Channel index (0 or 1)
    //   click: Mouse click position
    //   bounds: Panel rectangle
    bool (*handle_click)(void *state, struct gui_app *app, int channel,
                         Vector2 click, Rectangle bounds);

    // Handle mouse scroll. Returns true if scroll was consumed.
    // May be NULL if panel doesn't handle scrolling.
    bool (*handle_scroll)(void *state, float delta, Rectangle bounds);

    //-------------------------------------------------------------------------
    // Menu System (panel-owned dropdowns)
    //-------------------------------------------------------------------------

    // Get number of menus this panel provides (0 if none).
    // May be NULL (treated as 0 menus).
    size_t (*get_menu_count)(void *state);

    // Get menu definition at index. Caller must not free returned menu.
    // May be NULL if get_menu_count is NULL or returns 0.
    panel_menu_t (*get_menu)(void *state, size_t index);

    // True while the panel's double-click chrome-hide is on (its OSD /
    // controls hidden): the grid then hides the pop-out / full-screen
    // button too (harrypm 2026-11-26). May be NULL (never hidden).
    bool (*chrome_hidden)(void *state);

} panel_vtable_t;

//-----------------------------------------------------------------------------
// Panel Instance
//-----------------------------------------------------------------------------

typedef struct panel_instance {
    panel_view_type_t type;         // Panel type enum
    const panel_vtable_t *vtable;   // Virtual function table (NULL if no vtable registered)
    void *state;                    // Panel-specific state (created by vtable->create)
    Rectangle last_bounds;          // Cached bounds from last render (for click handling)
} panel_instance_t;

//-----------------------------------------------------------------------------
// Panel Registry API
//-----------------------------------------------------------------------------

// Initialize the panel registry (call once at startup)
void panel_registry_init(void);

// Register a vtable for a panel type
void panel_register(panel_view_type_t type, const panel_vtable_t *vtable);

// Get vtable for a panel type (returns NULL if not registered)
const panel_vtable_t *panel_get_vtable(panel_view_type_t type);

//-----------------------------------------------------------------------------
// Panel Instance Lifecycle
//-----------------------------------------------------------------------------

// Create a panel instance of the given type.
// Returns instance with vtable and state populated, or empty instance on failure.
panel_instance_t panel_instance_create(panel_view_type_t type);

// Destroy a panel instance and free its state.
void panel_instance_destroy(panel_instance_t *instance);

// Clear panel instance state without destroying.
void panel_instance_clear(panel_instance_t *instance);

//-----------------------------------------------------------------------------
// Panel Instance Operations
//-----------------------------------------------------------------------------

// Process raw samples (display thread). No-op if vtable->process is NULL.
void panel_instance_process(panel_instance_t *instance, const int16_t *samples, size_t count, uint32_t sample_rate);

// Render panel (render thread). Updates last_bounds.
void panel_instance_render(panel_instance_t *instance, struct gui_app *app, int channel,
                           Rectangle bounds, Color channel_color);

// Render overlay (render thread). No-op if vtable->render_overlay is NULL.
void panel_instance_render_overlay(panel_instance_t *instance, Rectangle bounds);

// Handle click (render thread). Returns true if consumed.
bool panel_instance_handle_click(panel_instance_t *instance, struct gui_app *app, int channel,
                                 Vector2 click, Rectangle bounds);

// Handle scroll (render thread). Returns true if consumed.
bool panel_instance_handle_scroll(panel_instance_t *instance, float delta, Rectangle bounds);

// Get menu count. Returns 0 if vtable->get_menu_count is NULL.
size_t panel_instance_get_menu_count(panel_instance_t *instance);

// Get menu at index. Returns empty menu if out of bounds or no vtable.
panel_menu_t panel_instance_get_menu(panel_instance_t *instance, size_t index);

//-----------------------------------------------------------------------------
// Batch Processing (called from display thread)
//-----------------------------------------------------------------------------

// Process all active panels for both channels with raw ADC samples.
// Iterates through panel configs and calls process() on panels that have it.
// Parameters:
//   app: Application state (contains panel configs)
//   samples_a: Channel A raw samples
//   samples_b: Channel B raw samples
//   count: Number of samples per channel
//   sample_rate: ADC sample rate in Hz
void panel_process_all(struct gui_app *app,
                       const int16_t *samples_a,
                       const int16_t *samples_b,
                       size_t count,
                       uint32_t sample_rate);

#endif // PANEL_INTERFACE_H
