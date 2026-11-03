/*
 * MISRC GUI - Windows modal move/size loop render driver (interface)
 */

#ifndef GUI_WIN32_MODAL_H
#define GUI_WIN32_MODAL_H

#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32) || defined(_WIN64)

// Keep the window rendering while the user moves/resizes it. The Windows
// modal move/size loop inside DefWindowProc blocks raylib's event poll for
// the whole drag; this driver subclasses the window proc and calls the
// registered frame callback from a WM_TIMER inside the loop (render thread).
// See gui_win32_modal.c for the full mechanics + the rules the frame
// callback must obey (render only; never poll events).

// Subclass the current raylib (GLFW) window proc. Call once after
// InitWindow(). Returns false only when the window handle is unavailable.
bool gui_win32_modal_install(void);

// Register the render-only frame callback driven during the modal loop
// (called on the render thread, inside the loop; must not poll events).
void gui_win32_modal_set_frame(void (*frame)(void *ctx), void *ctx);

// Kill the timer and restore the original window proc. Call before
// CloseWindow().
void gui_win32_modal_shutdown(void);

// Drag-to-top snap intents, classified by how long the drag dwelled at
// the monitor's top edge (user request 2026-11-03). The wndproc only
// QUEUES these; the main loop consumes them after the event poll (acting
// on the window inside the wndproc would re-enter GLFW's window code).
//
// Take (consume) a pending quick-throw request: true when the window was
// thrown at the top edge and released quickly since the last call. The
// main loop answers with MaximizeWindow() - a normal maximized window,
// NOT borderless - which also overrides any Win11 snap-layout arrangement
// a fast gesture may have landed in.
bool gui_win32_modal_take_maximize_request(void);

// Take (consume) a pending hold request: true when the window was HELD at
// the top edge (>= 400 ms) before release. The main loop answers with
// gui_win32_modal_apply_top_half().
bool gui_win32_modal_take_top_half_request(void);

// Snap the window to the TOP HALF of its current monitor's work area
// (restoring it first if the native snap maximized it on release).
void gui_win32_modal_apply_top_half(void);

// Take (consume) the pending classification summary of the last window
// drag: fills dst with "zone=? recent=? dwell=..ms speed=.. -> verdict" and
// returns true once per drag release. Diagnostics for the snap intents
// above - display it wherever is convenient (the app shows it in the
// status bar; --debug-view's console attach does not surface output in
// every terminal).
bool gui_win32_modal_take_drag_summary(char *dst, size_t cap);

#else
// Other platforms have no modal move/size loop (X11/Cocoa keep the app loop
// running during live resize): the call sites compile to no-ops, and
// install "succeeds" (nothing is needed).
static inline bool gui_win32_modal_install(void) { return true; }
static inline void gui_win32_modal_set_frame(void (*frame)(void *ctx), void *ctx)
{
    (void)frame;
    (void)ctx;
}
static inline void gui_win32_modal_shutdown(void) {}
static inline bool gui_win32_modal_take_maximize_request(void) { return false; }
static inline bool gui_win32_modal_take_top_half_request(void) { return false; }
static inline void gui_win32_modal_apply_top_half(void) {}
static inline bool gui_win32_modal_take_drag_summary(char *dst, size_t cap)
{
    (void)dst;
    (void)cap;
    return false;
}

#endif

#endif // GUI_WIN32_MODAL_H
