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

#include <math.h>
#include <stdio.h>
#include <string.h>

#define GUI_WIN32_MODAL_TIMER_ID 0x4D49    // app-private timer id
#define GUI_WIN32_MODAL_TIMER_MS 15        // ~60 Hz while the modal loop runs

// Drag-to-top snap classification (user request 2026-11-03): a THROW of
// the window at the monitor's top edge maximizes it (a normal maximized
// window, NOT borderless - and overriding any Win11 snap-layout arrangement
// a fast gesture may have landed in); HOLDING the window parked at the top
// edge snaps it to the top half of the monitor's work area. F11/Alt+Enter
// (the main loop) remain the borderless fullscreen toggle.
//
// Throw vs hold is classified at release by BOTH the edge dwell time and
// the cursor's RELEASE VELOCITY: a throw releases while the cursor is still
// moving fast (even if it lingered at the edge for a while first - dwell
// alone misclassified those as holds, user report 2026-11-03); a hold has
// the cursor parked at the edge, slow, for >= GUI_WIN32_SNAP_HOLD_MS.
#define GUI_WIN32_SNAP_ZONE_PX 4       // cursor within this of the monitor top edge
#define GUI_WIN32_SNAP_HOLD_MS 400     // parked-at-edge dwell that means HOLD
#define GUI_WIN32_SNAP_RECENT_MS 300   // edge touch this recently still counts
#define GUI_WIN32_SNAP_THROW_PX_MS 0.5 // release speed (px/ms) that means a throw
#define GUI_WIN32_SNAP_SPEED_WINDOW_MS 100  // trailing window for that speed
#define GUI_WIN32_SNAP_HISTORY 8       // cursor samples kept for the velocity

typedef struct { ULONGLONG ms; LONG x, y; } gui_win32_snap_sample_t;

static WNDPROC s_orig_proc = NULL;
static HWND s_hwnd = NULL;
static void (*s_frame_cb)(void *ctx) = NULL;
static void *s_frame_ctx = NULL;
static bool s_in_modal_loop = false;
static unsigned s_modal_frames = 0;
static bool s_maximize_requested = false;   // throw -> MaximizeWindow()
static bool s_top_half_requested = false;   // hold at top -> top-half placement
static bool s_move_gesture = false;         // this modal loop is a caption MOVE
static bool s_syscmd_seen = false;          // a WM_SYSCOMMAND preceded this loop
static UINT s_last_syscmd = 0;              // its wParam (diagnostics)
static ULONGLONG s_top_zone_enter_ms = 0;   // 0 = cursor not in the top zone
static ULONGLONG s_top_zone_last_seen_ms = 0;  // last edge touch this drag
static gui_win32_snap_sample_t s_snap_samples[GUI_WIN32_SNAP_HISTORY];
static unsigned s_snap_sample_count = 0;
static char s_drag_summary[96];          // last release's classification
static bool s_summary_pending = false;   // waiting for the main loop

// True when the given screen position sits in the native Aero-Snap top zone
// (within GUI_WIN32_SNAP_ZONE_PX of the top edge of the monitor under it).
static bool gui_win32_point_in_top_zone(LONG x, LONG y)
{
    POINT pt = { x, y };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return x >= mi.rcMonitor.left && x < mi.rcMonitor.right &&
           y >= mi.rcMonitor.top &&
           y <= mi.rcMonitor.top + GUI_WIN32_SNAP_ZONE_PX;
}

// Sample the cursor (WM_MOUSEMOVE + the 15 ms modal timer) and keep the
// top-edge dwell state fresh. Leaving the zone resets the dwell timer, so
// dwelling on the Win11 snap-layouts flyout (which lives below the edge)
// or wandering away never counts as a hold.
static void gui_win32_snap_track_cursor(void)
{
    if (!s_move_gesture) return;
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    ULONGLONG now = GetTickCount64();
    // Trailing cursor history for the release-velocity classification.
    if (s_snap_sample_count < GUI_WIN32_SNAP_HISTORY) {
        s_snap_samples[s_snap_sample_count++] =
            (gui_win32_snap_sample_t){ now, pt.x, pt.y };
    } else {
        memmove(s_snap_samples, s_snap_samples + 1,
                sizeof(s_snap_samples) - sizeof(s_snap_samples[0]));
        s_snap_samples[GUI_WIN32_SNAP_HISTORY - 1] =
            (gui_win32_snap_sample_t){ now, pt.x, pt.y };
    }
    if (gui_win32_point_in_top_zone(pt.x, pt.y)) {
        s_top_zone_last_seen_ms = now;
        if (s_top_zone_enter_ms == 0) s_top_zone_enter_ms = now;
    } else {
        s_top_zone_enter_ms = 0;
    }
}

static LRESULT CALLBACK gui_win32_modal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (hwnd == s_hwnd) {
        switch (msg) {
            case WM_SYSCOMMAND:
                // Track whether the upcoming modal loop is a caption MOVE
                // (drag) - the top-edge snap intents only apply to moves,
                // never to border resizes or the min/max/close buttons.
                // wParam's low 4 bits are system-internal: mask them off.
                s_move_gesture = ((wp & 0xFFF0) == SC_MOVE);
                s_syscmd_seen = true;
                s_last_syscmd = (UINT)wp;
                break;

            case WM_ENTERSIZEMOVE:
                // The modal move/size loop starts: pump render frames.
                // Caption drags normally arrive as WM_SYSCOMMAND(SC_MOVE)
                // -> WM_ENTERSIZEMOVE; if NO WM_SYSCOMMAND preceded the
                // loop, assume a MOVE anyway (a missing SYSCOMMAND would
                // otherwise silently disable the whole classifier - resizes
                // DO see SC_SIZE first, so they keep their precise gate).
                s_in_modal_loop = true;
                s_modal_frames = 0;
                if (!s_syscmd_seen) s_move_gesture = true;
                s_syscmd_seen = false;
                s_top_zone_enter_ms = 0;
                s_top_zone_last_seen_ms = 0;
                s_snap_sample_count = 0;
                SetTimer(hwnd, GUI_WIN32_MODAL_TIMER_ID, GUI_WIN32_MODAL_TIMER_MS, NULL);
                break;

            case WM_MOUSEMOVE:
                // Fresh top-edge zone state at high frequency so the state
                // is current at drag release (the 15 ms timer poll alone
                // can be stale on a fast throw).
                if (s_in_modal_loop) gui_win32_snap_track_cursor();
                break;

            case WM_TIMER:
                if (wp == GUI_WIN32_MODAL_TIMER_ID && s_in_modal_loop && s_frame_cb) {
                    // Render-only frame inside the modal loop (this thread
                    // owns the GL context; the frame callback must NOT poll
                    // events - see the module doc above).
                    gui_win32_snap_track_cursor();
                    s_frame_cb(s_frame_ctx);
                    s_modal_frames++;
                    return 0;
                }
                break;

            case WM_EXITSIZEMOVE:
                s_in_modal_loop = false;
                KillTimer(hwnd, GUI_WIN32_MODAL_TIMER_ID);
                // Classify the drag-to-top release. The cursor position is
                // queried FRESH here: on a fast flick the last WM_MOUSEMOVE
                // can lag the true release position by tens of pixels and
                // miss the edge zone. An edge touch within the last
                // GUI_WIN32_SNAP_RECENT_MS also counts (the cursor can end
                // slightly below the edge, or cross into a monitor above).
                // The main loop consumes the queued request AFTER the event
                // poll - acting on the window from inside the wndproc (mid
                // message dispatch) would re-enter GLFW's window code.
                if (s_move_gesture) {
                    POINT pt;
                    bool have_pt = GetCursorPos(&pt);
                    ULONGLONG now = GetTickCount64();
                    bool in_zone = have_pt &&
                                   gui_win32_point_in_top_zone(pt.x, pt.y);
                    bool recent_zone = s_top_zone_last_seen_ms != 0 &&
                                       now - s_top_zone_last_seen_ms <=
                                           GUI_WIN32_SNAP_RECENT_MS;
                    bool top_gesture = in_zone || recent_zone;
                    // Release velocity over the trailing speed window: the
                    // oldest sample still inside the window (all samples
                    // stale -> the newest). The throw test itself uses the
                    // squared comparison; sqrt is only for the log readout.
                    double speed_px_ms = 0.0;
                    bool fast_release = false;
                    if (have_pt && s_snap_sample_count > 0) {
                        const gui_win32_snap_sample_t *base = NULL;
                        for (int i = (int)s_snap_sample_count - 1; i >= 0; --i) {
                            if (now - s_snap_samples[i].ms <=
                                    GUI_WIN32_SNAP_SPEED_WINDOW_MS) {
                                base = &s_snap_samples[i];
                            }
                        }
                        if (!base) {
                            base = &s_snap_samples[s_snap_sample_count - 1];
                        }
                        double dt = (double)(now - base->ms);
                        if (dt > 0.0) {
                            double dx = (double)pt.x - (double)base->x;
                            double dy = (double)pt.y - (double)base->y;
                            speed_px_ms = sqrt(dx * dx + dy * dy) / dt;
                            double limit = GUI_WIN32_SNAP_THROW_PX_MS * dt;
                            fast_release = (dx * dx + dy * dy) >
                                           (limit * limit);
                        }
                    }
                    ULONGLONG dwell_ms = s_top_zone_enter_ms
                        ? now - s_top_zone_enter_ms : 0;
                    bool hold = top_gesture && in_zone && !fast_release &&
                                dwell_ms >= GUI_WIN32_SNAP_HOLD_MS;
                    // One diagnostic line per drag release (run with
                    // --debug-view to see it): every classifier input plus
                    // the verdict, so a misbehaving gesture is diagnosable
                    // from the log alone.
                    const char *verdict = !top_gesture ? "ignore" :
                                           hold ? "top-half" : "maximize";
                    fprintf(stderr,
                            "[WIN32] snap: syscmd=0x%04X zone=%d recent=%d "
                            "dwell=%ums speed=%.2fpx/ms -> %s\n",
                            (unsigned)s_last_syscmd, in_zone ? 1 : 0,
                            recent_zone ? 1 : 0, (unsigned)dwell_ms,
                            speed_px_ms, verdict);
                    // Same data queued for the STATUS BAR: --debug-view's
                    // console attach does not surface output in every
                    // terminal (user report 2026-11-03), but the in-app
                    // status bar always shows it. The main loop takes this
                    // right after the event poll and displays it.
                    snprintf(s_drag_summary, sizeof(s_drag_summary),
                             "Snap: zone=%d recent=%d dwell=%ums "
                             "speed=%.2f -> %s",
                             in_zone ? 1 : 0, recent_zone ? 1 : 0,
                             (unsigned)dwell_ms, speed_px_ms, verdict);
                    s_summary_pending = true;
                    if (hold) {
                        s_top_half_requested = true;
                    } else if (top_gesture) {
                        s_maximize_requested = true;
                    }
                }
                s_top_zone_enter_ms = 0;
                s_top_zone_last_seen_ms = 0;
                s_snap_sample_count = 0;
                s_move_gesture = false;
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

bool gui_win32_modal_take_top_half_request(void)
{
    bool was = s_top_half_requested;
    s_top_half_requested = false;
    return was;
}

bool gui_win32_modal_take_drag_summary(char *dst, size_t cap)
{
    if (!s_summary_pending) return false;
    s_summary_pending = false;
    if (dst && cap > 0) {
        snprintf(dst, cap, "%s", s_drag_summary);
    }
    return true;
}

void gui_win32_modal_apply_top_half(void)
{
    if (!s_hwnd) return;
    // The native snap may already have maximized the window on release -
    // restore first so the half placement is a normal windowed rect.
    WINDOWPLACEMENT wp = { .length = sizeof(wp) };
    if (GetWindowPlacement(s_hwnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
        ShowWindow(s_hwnd, SW_RESTORE);
    }
    // Snap to the TOP HALF of the current monitor's work area (taskbar
    // excluded). Same-thread SetWindowPos (the main loop consumes the
    // request); GLFW's own wndproc stays in the chain and observes the
    // resulting WM_SIZE/WM_MOVE notifications, keeping its state cache
    // consistent.
    HMONITOR mon = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        SetWindowPos(s_hwnd, HWND_TOP,
                     mi.rcWork.left, mi.rcWork.top,
                     mi.rcWork.right - mi.rcWork.left,
                     (mi.rcWork.bottom - mi.rcWork.top) / 2,
                     SWP_NOACTIVATE);
    }
}

void gui_win32_modal_shutdown(void)
{
    if (s_hwnd) {
        s_in_modal_loop = false;
        s_maximize_requested = false;
        s_top_half_requested = false;
        s_top_zone_enter_ms = 0;
        s_top_zone_last_seen_ms = 0;
        s_snap_sample_count = 0;
        s_move_gesture = false;
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
