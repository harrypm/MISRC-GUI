/*
 * MISRC GUI - Pop-out panels (harrypm 2026-11-26: "add a pop-out feature to
 * fully pop out panels to put them on dedicated screens if wanted").
 *
 * Any of the four panel slots (channel A/B x left/right) can be popped out
 * into its own native window. It opens exactly over the panel in the main
 * window (same screen, position and size) and is movable to any screen, F11 (or its corner
 * button) for fullscreen on the screen it is on. The panel keeps its state
 * and keeps working: clicks, the mouse wheel and hover go to the panel in
 * the pop-out window. The main grid shows "Popped out - click to return"
 * in that slot; closing the pop-out window (or that click) returns it.
 *
 * How: the pop-out windows are GLFW windows sharing raylib's GL context
 * (textures are shared). Each frame the popped panel renders, in the main
 * context, into a render texture at the pop-out's framebuffer size and the
 * app's UI scale; each pop-out then draws that texture full-window with a
 * minimal shader in its own context (no vsync wait) and swaps.
 *
 * Desktop only (GLFW through raylib); without it (Android, or a raylib
 * that does not export GLFW) the feature compiles out:
 * gui_popout_available() is false and no pop-out button is drawn.
 *
 * Flag: --popout <a|b>:<left|right>[,...] opens slots at launch. Open
 * slots and each slot's window position / size / fullscreen persist
 * (settings popout_windows).
 *
 * v1 limits: drag gestures that read raylib's main-window button state
 * (Waveform / FFT drag) do not work in a pop-out; on Windows, dragging a
 * pop-out window by its title bar pauses redraws until released.
 */
#ifndef GUI_POPOUT_H
#define GUI_POPOUT_H

#include <stdbool.h>
#include "raylib.h"

struct gui_app;

#define GUI_POPOUT_SLOTS 4   /* slot = channel * 2 + side (0 left, 1 right) */

bool gui_popout_available(void);
bool gui_popout_is_open(int channel, int side);
/* Main thread. Open / close (close returns the panel to the grid). */
bool gui_popout_open(struct gui_app *app, int channel, int side);
void gui_popout_close(int channel, int side);

/* Main thread, once per frame BEFORE BeginDrawing: pop-out input, render,
 * present. */
void gui_popout_frame(struct gui_app *app);
/* Restore the open slots saved in the settings (after the window exists). */
void gui_popout_restore(struct gui_app *app);
/* Write the open slots / geometry into the settings (before saving them). */
void gui_popout_capture_settings(struct gui_app *app);
void gui_popout_shutdown(void);

/* --popout <a|b>:<left|right>[,...]: queued, opened after the window
 * exists. False + usage on a bad value. */
bool gui_popout_apply_launch_arg(const char *arg);

/* Main-grid corner button (drawn by gui_panel.c for slots not popped). */
void gui_popout_draw_button(int channel, int side, Rectangle panel_bounds);
/* True (and acts) when the click hit a slot's pop-out button or a popped
 * slot's placeholder. */
bool gui_popout_handle_grid_click(struct gui_app *app, int channel, int side, Vector2 click,
                                  Rectangle panel_bounds);
/* Placeholder drawn in the grid for a popped slot. */
void gui_popout_draw_placeholder(int channel, int side, Rectangle bounds, const char *view_name);

/* Builds without pop-out windows (Android / no GLFW): the corner button
 * makes the panel full screen inside the app (harrypm 2026-11-26). While
 * active it is drawn over the whole layout and owns taps / wheel; its
 * corner button or Back / Esc returns it to the grid. */
bool gui_popout_fullscreen_active(void);
/* After the Clay UI render, inside the frame (and the Android canvas). */
void gui_popout_draw_fullscreen(struct gui_app *app);
/* Call in place of the UI's click handling: true = full screen owns input
 * this frame (pressed = this frame's tap). */
bool gui_popout_fullscreen_input(struct gui_app *app, bool pressed);
bool gui_popout_fullscreen_scroll(struct gui_app *app, float delta);

#endif
