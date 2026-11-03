/*
 * MISRC GUI - Windows modal move/size loop render driver
 *
 * On Windows a title-bar drag or edge resize enters a MODAL LOOP inside
 * DefWindowProc (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE): the message pump runs
 * inside DefWindowProc and never returns to the app for the whole drag.
 * raylib drains the OS event queue at the tail of every frame; the caption
 * mouse-down is dispatched from inside that drain, so the render loop
 * blocks for the entire drag: no frames are swapped, the panels freeze
 * (their data threads keep running) and the audio monitor stutters.
 * Linux/macOS have no modal loop, hence unaffected.
 *
 * The classic Win32 fix, adapted: subclass the GLFW window proc; on
 * WM_ENTERSIZEMOVE start a ~60 Hz timer; on each WM_TIMER - still inside
 * the modal loop, on the RENDER thread, with the GL context current - call
 * the registered frame callback, which renders one frame WITHOUT ever
 * polling events (a nested PeekMessage inside DefWindowProc's loop is
 * reentrancy; EndDrawing is also avoided for its FPS wait - the timer
 * paces). raylib's WindowSizeCallback already ran SetupViewport + updated
 * the window size on this thread (messages ARE dispatched inside the modal
 * loop), so timer frames render at the live size. On WM_EXITSIZEMOVE stop
 * the timer; the blocked frame then completes normally. Modal entry is
 * always at the event-poll dispatch stage - AFTER the current frame
 * flushed its render batch and swapped - so the nested frames start from
 * an empty batch.
 */

// raylib.h FIRST: the windows.h include below would otherwise define
// macros (Rectangle via wingdi, CloseWindow/ShowCursor via winuser) that
// collide with raylib's function/type declarations.
#include "raylib.h"            // GetWindowHandle()

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI                   // wingdi: the Rectangle/LoadImage/DrawText macros
// winuser macros that collide with raylib function names: rename for the
// duration of the windows.h include (the repo's established pattern).
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#undef ShowCursor
#undef CloseWindow

#include "gui_win32_modal.h"

#include <stdio.h>

#define GUI_WIN32_MODAL_TIMER_ID 0x4D49    // app-private timer id
#define GUI_WIN32_MODAL_TIMER_MS 15        // ~60 Hz while the modal loop runs

static WNDPROC s_orig_proc = NULL;
static HWND s_hwnd = NULL;
static void (*s_frame_cb)(void *ctx) = NULL;
static void *s_frame_ctx = NULL;
static bool s_in_modal_loop = false;
static unsigned s_modal_frames = 0;
static bool s_maximize_requested = false;

static LRESULT CALLBACK gui_win32_modal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (hwnd == s_hwnd) {
        switch (msg) {
            case WM_ENTERSIZEMOVE:
                // The modal move/size loop starts: pump render frames.
                s_in_modal_loop = true;
                s_modal_frames = 0;
                SetTimer(hwnd, GUI_WIN32_MODAL_TIMER_ID, GUI_WIN32_MODAL_TIMER_MS, NULL);
                break;

            case WM_SIZE:
                // Aero Snap maximize (drag-to-top, possibly with Win11 snap
                // layouts): request the clean-fullscreen conversion. The
                // main loop consumes this AFTER the event poll; toggling
                // fullscreen from inside the wndproc (mid message dispatch)
                // would re-enter GLFW's window code.
                if (wp == SIZE_MAXIMIZED) {
                    s_maximize_requested = true;
                }
                break;

            case WM_TIMER:
                if (wp == GUI_WIN32_MODAL_TIMER_ID && s_in_modal_loop && s_frame_cb) {
                    // Render-only frame inside the modal loop (this thread
                    // owns the GL context; the frame callback must NOT poll
                    // events - see the module doc above).
                    s_frame_cb(s_frame_ctx);
                    s_modal_frames++;
                    return 0;
                }
                break;

            case WM_EXITSIZEMOVE:
                s_in_modal_loop = false;
                KillTimer(hwnd, GUI_WIN32_MODAL_TIMER_ID);
                if (s_modal_frames > 0) {
                    fprintf(stderr,
                            "[WIN32] modal move/size: rendered %u live frames\n",
                            s_modal_frames);
                }
                break;

            default:
                break;
        }
    }
    return CallWindowProcW(s_orig_proc, hwnd, msg, wp, lp);
}

bool gui_win32_modal_install(void)
{
    HWND hwnd = (HWND)GetWindowHandle();
    if (!hwnd) return false;
    if (s_hwnd == hwnd) return true;    // already installed
    s_orig_proc = (WNDPROC)(VOID *)SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, (LONG_PTR)(VOID *)gui_win32_modal_proc);
    if (!s_orig_proc) return false;
    s_hwnd = hwnd;
    return true;
}

void gui_win32_modal_set_frame(void (*frame)(void *ctx), void *ctx)
{
    s_frame_cb = frame;
    s_frame_ctx = ctx;
}

bool gui_win32_modal_take_maximize_request(void)
{
    bool was = s_maximize_requested;
    s_maximize_requested = false;
    return was;
}

void gui_win32_modal_shutdown(void)
{
    if (s_hwnd) {
        s_in_modal_loop = false;
        s_maximize_requested = false;
        s_frame_cb = NULL;
        s_frame_ctx = NULL;
        KillTimer(s_hwnd, GUI_WIN32_MODAL_TIMER_ID);
        if (s_orig_proc) {
            SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)(VOID *)s_orig_proc);
        }
        s_hwnd = NULL;
        s_orig_proc = NULL;
    }
}
