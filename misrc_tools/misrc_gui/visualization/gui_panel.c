/*
 * MISRC GUI - Panel Abstraction Implementation
 *
 * Provides render dispatch and state management for panel views.
 * This file orchestrates panel rendering by delegating to specialized
 * panel render functions in their respective modules.
 */

#include "gui_panel.h"
#include "../net/gui_net.h"
#include "../net/gui_remote_monitor.h"
#include "../net/gui_remote_display.h"
#include "panel_interface.h"
#include "../core/gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_fft.h"
#include "gui_spectrogram.h"
#include "../signal/gui_demod.h"
#include "../ui/gui_ui.h"
#include <stdio.h>

//-----------------------------------------------------------------------------
// View Type Names (for UI dropdowns)
//-----------------------------------------------------------------------------

static const char* s_view_names[] = {
    [PANEL_VIEW_WAVEFORM] = "Waveform",
    [PANEL_VIEW_FFT] = "FFT",
    [PANEL_VIEW_CVBS] = "Video",
    [PANEL_VIEW_HISTOGRAM] = "Histogram",
    [PANEL_VIEW_WATERFALL] = "Waterfall",
    [PANEL_VIEW_SPECTROGRAPH] = "Spectro",
    [PANEL_VIEW_DEMOD] = "Demod",
};

const char* panel_view_type_name(panel_view_type_t type) {
    if (type < PANEL_VIEW_COUNT) return s_view_names[type];
    return "Unknown";
}

bool panel_view_type_available(panel_view_type_t type) {
    switch (type) {
        case PANEL_VIEW_WAVEFORM:
        case PANEL_VIEW_CVBS:
        case PANEL_VIEW_HISTOGRAM:
            return true;
        case PANEL_VIEW_FFT:
            return gui_fft_available();
        case PANEL_VIEW_WATERFALL:
        case PANEL_VIEW_SPECTROGRAPH:
            return gui_spectrogram_available();
        case PANEL_VIEW_DEMOD:
            return gui_demod_available();
        default:
            return false;
    }
}

//-----------------------------------------------------------------------------
// View State Management
//-----------------------------------------------------------------------------

void* panel_create_view_state(panel_view_type_t type) {
    // Use vtable factory if registered (all panel types should use this path)
    const panel_vtable_t *vtable = panel_get_vtable(type);
    if (vtable && vtable->create) {
        return vtable->create();
    }

    // Fallback for panels without vtable (should not be reached)
    return NULL;
}

void panel_destroy_view_state(panel_view_type_t type, void *state) {
    if (!state) return;

    // Use vtable destroy if registered (handles proper cleanup)
    const panel_vtable_t *vtable = panel_get_vtable(type);
    if (vtable && vtable->destroy) {
        vtable->destroy(state);
    }
}

void panel_clear_view_state(panel_view_type_t type, void *state) {
    if (!state) return;

    // Use vtable clear if registered
    const panel_vtable_t *vtable = panel_get_vtable(type);
    if (vtable && vtable->clear) {
        vtable->clear(state);
    }
}

//-----------------------------------------------------------------------------
// Channel Panel Rendering (Main Entry Point)
//-----------------------------------------------------------------------------

/* Unsupported remote views are intentionally placeholders until preview
 * data exists. Waveform and FFT use the original local Harry panel vtables. */
static void render_remote_unsupported(Rectangle bounds) {
    DrawRectangleRec(bounds, COLOR_PANEL_BG);
    DrawRectangleLinesEx(bounds, 1.0f, COLOR_GRID_MAJOR);
    DrawText("Remote preview: panel in a later stage", (int)bounds.x+12,
             (int)(bounds.y+bounds.height/2-8), 16, COLOR_TEXT_DIM);
}

// Helper: Render a single panel using vtable dispatch
static void render_single_panel(gui_app_t *app, int channel,
                                 panel_view_type_t type, void *state,
                                 Rectangle bounds, Color color) {
    if (gui_net_is_client(app) && type != PANEL_VIEW_WAVEFORM && type != PANEL_VIEW_FFT) {
        render_remote_unsupported(bounds);
        return;
    }
    const panel_vtable_t *vtable = panel_get_vtable(type);
    if (gui_net_is_client(app) && type == PANEL_VIEW_FFT) {
        static gui_rd_frame_t frame;
        bool valid = app->is_capturing && gui_rd_client_snapshot(&frame);
        if (!valid || channel >= (int)frame.channels || !state) {
            DrawRectangleRec(bounds, COLOR_PANEL_BG);
            DrawRectangleLinesEx(bounds, 1.0f, COLOR_GRID_MAJOR);
            DrawText("Waiting for remote FFT", (int)bounds.x+12,
                     (int)(bounds.y+bounds.height/2-8), 16, COLOR_TEXT_DIM);
            return;
        }
        gui_fft_remote_apply_float(state,frame.ch[channel].fft,
                                   GUI_RD_BINS,frame.sample_rate,frame.sequence);
    }
    if (gui_net_is_client(app) && type == PANEL_VIEW_WAVEFORM) {
        /* Reuse Harry's unmodified grid/line/phosphor/trigger/overlay renderer
         * instead of the v2 custom 256-column remote drawing code. */
        /* Rendering is main-thread only; reuse scratch rather than allocating
         * a 64 KiB scope packet on every redraw. */
        static gui_rd_frame_t frame;
        bool valid = app->is_capturing && gui_rd_client_snapshot(&frame);
        if (valid && state && channel < (int)frame.channels)
            gui_waveform_remote_apply_points(state,&frame.ch[channel],frame.sequence);
        if(state)gui_waveform_remote_publish_config(state,channel);
        if (!valid || channel >= (int)frame.channels) {
            DrawRectangleRec(bounds, COLOR_PANEL_BG);
            DrawRectangleLinesEx(bounds, 1.0f, COLOR_GRID_MAJOR);
            DrawText("Waiting for remote waveform", (int)bounds.x + 12,
                     (int)(bounds.y + bounds.height/2 - 8), 16, COLOR_TEXT_DIM);
            return;
        }
    }

    if (vtable && vtable->render) {
        vtable->render(state, app, channel, bounds, color);

        // Render overlay if present
        if (vtable->render_overlay) {
            vtable->render_overlay(state, bounds);
        }
    } else {
        // No vtable - draw placeholder
        DrawRectangleRec(bounds, (Color){20, 20, 20, 255});
        DrawRectangleLinesEx(bounds, 1, (Color){60, 60, 60, 255});
        const char *name = panel_view_type_name(type);
        int w = MeasureText(name, 14);
        DrawText(name, (int)(bounds.x + bounds.width/2 - w/2),
                 (int)(bounds.y + bounds.height/2 - 7), 14, COLOR_TEXT_DIM);
    }
}

void render_channel_panels(gui_app_t *app, int channel,
                           float x, float y, float width, float height,
                           Color channel_color) {

    // Get the config for this channel
    channel_panel_config_t *config = (channel == 0)
        ? &app->panel_config_a
        : &app->panel_config_b;

    if (!config->split) {
        // Single panel - render at full width
        Rectangle bounds = {x, y, width, height};
        config->left_bounds = bounds;
        config->right_bounds = (Rectangle){0, 0, 0, 0};

        render_single_panel(app, channel, config->left_view,
                           config->left_state, bounds, channel_color);
    } else {
        // Split panels - divide width between left and right
        float half_width = width / 2.0f;
        float divider_x = x + half_width;

        // Cache bounds for click handling
        Rectangle left_bounds = {x, y, half_width - 1, height};
        Rectangle right_bounds = {divider_x + 2, y, half_width - 3, height};
        config->left_bounds = left_bounds;
        config->right_bounds = right_bounds;

        // Left panel
        render_single_panel(app, channel, config->left_view,
                           config->left_state, left_bounds, channel_color);

        // Divider line
        DrawLineEx((Vector2){divider_x, y}, (Vector2){divider_x, y + height},
                   2.0f, COLOR_GRID_MAJOR);

        // Right panel
        render_single_panel(app, channel, config->right_view,
                           config->right_state, right_bounds, channel_color);
    }
}

//-----------------------------------------------------------------------------
// Panel Configuration Helpers
//-----------------------------------------------------------------------------

void panel_config_init_default(channel_panel_config_t *config) {
    config->split = false;
    config->left_view = PANEL_VIEW_WAVEFORM;
    config->right_view = PANEL_VIEW_FFT;
    config->left_cached_view = PANEL_VIEW_COUNT;
    config->left_cached_state = NULL;
    config->right_cached_view = PANEL_VIEW_COUNT;
    config->right_cached_state = NULL;
    // Create initial state for the default left view
    config->left_state = panel_create_view_state(config->left_view);
    // Right state only created when split mode is enabled
    config->right_state = NULL;
}

void panel_config_cleanup(channel_panel_config_t *config) {
    if (config->left_state) {
        panel_destroy_view_state(config->left_view, config->left_state);
        config->left_state = NULL;
    }
    if (config->left_cached_state) {
        panel_destroy_view_state(config->left_cached_view,
                                 config->left_cached_state);
        config->left_cached_state = NULL;
    }
    if (config->right_state) {
        panel_destroy_view_state(config->right_view, config->right_state);
        config->right_state = NULL;
    }
    if (config->right_cached_state) {
        panel_destroy_view_state(config->right_cached_view,
                                 config->right_cached_state);
        config->right_cached_state = NULL;
    }
}

// Switch a panel slot to a new view through the ONE-SLOT state cache: the
// current live state is PARKED (not destroyed) so switching back to its
// view restores it instantly - the video panel keeps its decoder sync
// lock and system selection, the waveform keeps its zoom (user report
// 2026-10-20: switching panel modes lost sync and reset the system to
// PAL). A switch to any different view evicts the parked state. A parked
// panel is neither rendered nor processed; its decoder resumes on restore
// (one stream-jump relock, far cheaper than a full re-acquisition - the
// levels, rate calibration and SECAM calibration all persist).
static void panel_slot_switch_view(panel_view_type_t *view, void **state,
                                   panel_view_type_t *cached_view,
                                   void **cached_state,
                                   panel_view_type_t new_view,
                                   bool create_state) {
    if (*view == new_view) return;
    if (*cached_state && *cached_view == new_view) {
        // Switching back: restore the parked state, park the current one.
        void *restored = *cached_state;
        *cached_state = *state;
        *cached_view = *view;
        *state = restored;
        *view = new_view;
        return;
    }
    // A different view: evict whatever is parked, park the current state,
    // create the new view's state (or leave NULL for the un-split right
    // slot, which only materialises on set_split).
    if (*cached_state) {
        panel_destroy_view_state(*cached_view, *cached_state);
        *cached_state = NULL;
    }
    *cached_view = *view;
    *cached_state = *state;
    *view = new_view;
    *state = create_state ? panel_create_view_state(new_view) : NULL;
}

void panel_config_set_left_view(channel_panel_config_t *config, panel_view_type_t type) {
    panel_slot_switch_view(&config->left_view, &config->left_state,
                           &config->left_cached_view,
                           &config->left_cached_state,
                           type, true);
}

void panel_config_set_right_view(channel_panel_config_t *config, panel_view_type_t type) {
    panel_slot_switch_view(&config->right_view, &config->right_state,
                           &config->right_cached_view,
                           &config->right_cached_state,
                           type, config->split);
}

void panel_config_set_split(channel_panel_config_t *config, bool split) {
    if (config->split == split) return;

    config->split = split;

    if (split) {
        // Entering split mode: restore the parked right state when it is
        // the right view's own (else create fresh).
        if (config->right_cached_state
            && config->right_cached_view == config->right_view) {
            config->right_state = config->right_cached_state;
            config->right_cached_state = NULL;
        } else {
            config->right_state = panel_create_view_state(config->right_view);
        }
    } else {
        // Leaving split mode: PARK the right state instead of destroying
        // it, so re-splitting restores the live decoders.
        if (config->right_state) {
            if (config->right_cached_state) {
                panel_destroy_view_state(config->right_cached_view,
                                         config->right_cached_state);
            }
            config->right_cached_view = config->right_view;
            config->right_cached_state = config->right_state;
        }
        config->right_state = NULL;
    }
}

//-----------------------------------------------------------------------------
// Unified Panel Click Handling
//-----------------------------------------------------------------------------

// Helper: Try vtable click handler for a panel
static bool try_panel_click(gui_app_t *app, int channel, panel_view_type_t type,
                            void *state, Rectangle bounds, Vector2 mouse_pos) {
    // First check if click is within bounds
    if (!CheckCollisionPointRec(mouse_pos, bounds)) {
        return false;
    }

    // Try vtable handler
    // Note: state may be NULL for panels like CVBS that store state on gui_app_t
    // Individual handlers must check for NULL state if they need it
    const panel_vtable_t *vtable = panel_get_vtable(type);
    if (vtable && vtable->handle_click) {
        return vtable->handle_click(state, app, channel, mouse_pos, bounds);
    }

    return false;
}

bool panel_handle_all_clicks(gui_app_t *app, Vector2 mouse_pos) {
    if (!app) return false;

    // Process both channels
    for (int ch = 0; ch < 2; ch++) {
        channel_panel_config_t *config = (ch == 0)
            ? &app->panel_config_a
            : &app->panel_config_b;

        // Try left panel
        if (try_panel_click(app, ch, config->left_view, config->left_state,
                            config->left_bounds, mouse_pos)) {
            return true;
        }

        // Try right panel if in split mode
        if (config->split) {
            if (try_panel_click(app, ch, config->right_view, config->right_state,
                                config->right_bounds, mouse_pos)) {
                return true;
            }
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// Unified Panel Scroll Handling
//-----------------------------------------------------------------------------

// Helper: Try vtable scroll handler for a panel
static bool try_panel_scroll(panel_view_type_t type, void *state,
                              Rectangle bounds, float delta) {
    // First check if mouse is within bounds
    Vector2 mouse = gui_ui_get_mouse_position();
    if (!CheckCollisionPointRec(mouse, bounds)) {
        return false;
    }

    // Try vtable handler
    const panel_vtable_t *vtable = panel_get_vtable(type);
    if (vtable && vtable->handle_scroll) {
        return vtable->handle_scroll(state, delta, bounds);
    }

    return false;
}

bool panel_handle_all_scrolls(gui_app_t *app, float delta) {
    if (!app || delta == 0.0f) return false;

    // Process both channels
    for (int ch = 0; ch < 2; ch++) {
        channel_panel_config_t *config = (ch == 0)
            ? &app->panel_config_a
            : &app->panel_config_b;

        // Try left panel
        if (try_panel_scroll(config->left_view, config->left_state,
                              config->left_bounds, delta)) {
            return true;
        }

        // Try right panel if in split mode
        if (config->split) {
            if (try_panel_scroll(config->right_view, config->right_state,
                                  config->right_bounds, delta)) {
                return true;
            }
        }
    }

    return false;
}
