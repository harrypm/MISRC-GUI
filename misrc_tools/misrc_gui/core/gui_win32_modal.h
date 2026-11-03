/*
 * MISRC GUI - Windows modal move/size loop render driver (interface)
 */

#ifndef GUI_WIN32_MODAL_H
#define GUI_WIN32_MODAL_H

#include <stdbool.h>

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

// Take (consume) a pending Aero Snap maximize request: true when the
// window was maximized via a Windows snap gesture (drag-to-top) since the
// last call. The main loop consumes this to convert the snap into the
// clean borderless fullscreen (a quick throw at the monitor top can land
// in a half/snapped state with glitched chrome instead - user report
// 2026-11-03).
bool gui_win32_modal_take_maximize_request(void);

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

#endif

#endif // GUI_WIN32_MODAL_H
