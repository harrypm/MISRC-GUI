#ifndef MISRC_GUI_ANDROID_VIEWPORT_H
#define MISRC_GUI_ANDROID_VIEWPORT_H

/* Full-frame Android pinch zoom.  Only the core translation unit defines
 * GUI_ANDROID_VIEWPORT_IMPLEMENTATION; GUI/UI modules use the declarations.
 * Keeping the implementation here avoids adding a new Meson source file.
 */
#if defined(__ANDROID__)
#include <stdbool.h>
#include <raylib.h>

void gui_android_viewport_update(void);
bool gui_android_viewport_block_controls(void);
bool gui_android_viewport_click_pressed(int button);
Vector2 gui_android_viewport_pointer(void);
bool gui_android_viewport_begin_canvas(void);
void gui_android_viewport_present_canvas(bool active);
void gui_android_viewport_restore_canvas_after_texture(void);
void gui_android_viewport_shutdown(void);

#if defined(GUI_ANDROID_VIEWPORT_IMPLEMENTATION)
#include <math.h>

#define ANDROID_VIEW_MAX_ZOOM 3.0f
#define ANDROID_VIEW_DRAG_THRESHOLD 9.0f
#define ANDROID_VIEW_DOUBLE_TAP_S 0.30
#define ANDROID_VIEW_TAP_DELAY_S 0.30

typedef struct {
    float zoom, x, y;
    int last_contacts;
    bool single_active, single_drag, multi_active;
    Vector2 start, previous, pinch_mid;
    float pinch_distance;
    bool queued_tap, click_pulse, block_controls;
    Vector2 queued_screen, click_screen;
    double queued_time;
    RenderTexture2D canvas;
    int canvas_width, canvas_height;
    bool canvas_active;  // We are currently drawing the UI into the outer canvas.
} gui_android_viewport_state_t;

static gui_android_viewport_state_t s_android_view = { .zoom = 1.0f };

static float android_view_clamp(float x, float lo, float hi)
{
    return fmaxf(lo, fminf(hi, x));
}

static void android_view_limit_translation(void)
{
    const float w = (float)GetRenderWidth();
    const float h = (float)GetRenderHeight();
    gui_android_viewport_state_t *v = &s_android_view;
    if (v->zoom <= 1.001f || w <= 0 || h <= 0) {
        v->zoom = 1.0f;
        v->x = v->y = 0.0f;
    } else {
        v->x = android_view_clamp(v->x, w * (1.0f - v->zoom), 0.0f);
        v->y = android_view_clamp(v->y, h * (1.0f - v->zoom), 0.0f);
    }
}

static void android_view_zoom_at(Vector2 focus, float zoom)
{
    gui_android_viewport_state_t *v = &s_android_view;
    if (!isfinite(zoom)) return;
    const float old = v->zoom;
    zoom = android_view_clamp(zoom, 1.0f, ANDROID_VIEW_MAX_ZOOM);
    if (fabsf(zoom - old) < 0.0001f) return;
    const float local_x = (focus.x - v->x) / old;
    const float local_y = (focus.y - v->y) / old;
    v->zoom = zoom;
    v->x = focus.x - local_x * zoom;
    v->y = focus.y - local_y * zoom;
    android_view_limit_translation();
}

void gui_android_viewport_update(void)
{
    gui_android_viewport_state_t *v = &s_android_view;
    const int n = GetTouchPointCount();
    const double now = GetTime();
    v->click_pulse = false;
    v->block_controls = (n != 0);

    if (n >= 2) {
        const Vector2 a = GetTouchPosition(0);
        const Vector2 b = GetTouchPosition(1);
        const Vector2 mid = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
        const float distance = hypotf(a.x - b.x, a.y - b.y);
        v->queued_tap = false; // A gesture never becomes an accidental click.
        if (!v->multi_active || v->last_contacts < 2 || distance <= 12.0f) {
            v->pinch_mid = mid;
            v->pinch_distance = distance;
        } else {
            const float dx = mid.x - v->pinch_mid.x;
            const float dy = mid.y - v->pinch_mid.y;
            v->x += dx;
            v->y += dy;
            if (v->pinch_distance > 12.0f && distance > 12.0f) {
                float ratio = distance / v->pinch_distance;
                // A contact reorder or lost touch must not jump the view.
                if (ratio >= 0.65f && ratio <= 1.55f) {
                    android_view_zoom_at(mid, v->zoom * ratio);
                }
            }
            android_view_limit_translation();
            v->pinch_mid = mid;
            v->pinch_distance = distance;
        }
        v->single_active = false;
        v->multi_active = true;
    } else if (n == 1) {
        const Vector2 p = GetTouchPosition(0);
        if (v->multi_active) {
            // Consume the leftover finger until ALL fingers have lifted.
            v->single_active = false;
        } else if (!v->single_active) {
            v->single_active = true;
            v->single_drag = false;
            v->start = v->previous = p;
        } else {
            const float distance = hypotf(p.x - v->start.x, p.y - v->start.y);
            if (distance >= ANDROID_VIEW_DRAG_THRESHOLD) v->single_drag = true;
            if (v->single_drag && v->zoom > 1.001f) {
                v->x += p.x - v->previous.x;
                v->y += p.y - v->previous.y;
                android_view_limit_translation();
            }
            v->previous = p;
        }
    } else {
        if (v->last_contacts == 1 && v->single_active &&
            !v->single_drag && !v->multi_active) {
            if (v->queued_tap &&
                now - v->queued_time <= ANDROID_VIEW_DOUBLE_TAP_S &&
                hypotf(v->previous.x - v->queued_screen.x,
                       v->previous.y - v->queued_screen.y) <= 42.0f) {
                v->queued_tap = false;
                android_view_zoom_at((Vector2){0, 0}, 1.0f);
            } else {
                // Delay single tap slightly so the second tap can reset zoom.
                v->queued_tap = true;
                v->queued_screen = v->previous;
                v->queued_time = now;
            }
        }
        v->single_active = false;
        v->single_drag = false;
        v->multi_active = false;
        if (v->queued_tap && now - v->queued_time >= ANDROID_VIEW_TAP_DELAY_S) {
            v->click_screen = v->queued_screen;
            v->click_pulse = true;
            v->queued_tap = false;
        }
    }
    v->last_contacts = n;
    android_view_limit_translation();
    // raylib's GetMousePosition() becomes the *unzoomed* virtual coordinate,
    // so Clay hit testing and normal UI pointer math still agree with drawing.
    SetMouseOffset((int)roundf(-v->x), (int)roundf(-v->y));
    SetMouseScale(1.0f / v->zoom, 1.0f / v->zoom);
}

bool gui_android_viewport_block_controls(void)
{
    return s_android_view.block_controls;
}

bool gui_android_viewport_click_pressed(int button)
{
    // Android taps are dispatched on release, only after excluding pans,
    // pinches and double-taps. Desktop buttons never come through this path.
    return button == MOUSE_LEFT_BUTTON && s_android_view.click_pulse;
}

Vector2 gui_android_viewport_pointer(void)
{
    const gui_android_viewport_state_t *v = &s_android_view;
    if (!v->click_pulse) return GetMousePosition();
    return (Vector2){ (v->click_screen.x - v->x) / v->zoom,
                      (v->click_screen.y - v->y) / v->zoom };
}

bool gui_android_viewport_begin_canvas(void)
{
    gui_android_viewport_state_t *v = &s_android_view;
    if (v->zoom <= 1.001f) return false; // fast path; no extra GPU pass at 1x
    const int w = GetRenderWidth(), h = GetRenderHeight();
    if (w <= 0 || h <= 0) return false;
    if (w != v->canvas_width || h != v->canvas_height) {
        if (v->canvas.id) UnloadRenderTexture(v->canvas);
        v->canvas = LoadRenderTexture(w, h);
        v->canvas_width = w;
        v->canvas_height = h;
        if (v->canvas.id) SetTextureFilter(v->canvas.texture, TEXTURE_FILTER_BILINEAR);
    }
    if (!v->canvas.id) {
        v->zoom = 1.0f; v->x = v->y = 0.0f;
        SetMouseOffset(0, 0); SetMouseScale(1, 1);
        return false;
    }
    BeginTextureMode(v->canvas);
    v->canvas_active = true;
    return true;
}

void gui_android_viewport_present_canvas(bool active)
{
    if (!active) return;
    gui_android_viewport_state_t *v = &s_android_view;
    v->canvas_active = false;
    EndTextureMode();
    ClearBackground(BLACK);
    const Rectangle source = { 0, 0, (float)v->canvas_width,
                              -(float)v->canvas_height };
    const Rectangle dest = { v->x, v->y,
                             (float)v->canvas_width * v->zoom,
                             (float)v->canvas_height * v->zoom };
    DrawTexturePro(v->canvas.texture, source, dest, (Vector2){0, 0}, 0, WHITE);
}


/* EndTextureMode() does not restore the enclosing render-texture state:
 * it selects the screen framebuffer AND resets raylib's viewport, projection
 * and current-FBO bookkeeping. Rebinding only the framebuffer ID therefore
 * leaves later Clay elements with a different projection than earlier ones
 * (visible as a shifted panel after the first phosphor/FFT pass on Android).
 *
 * Re-enter the existing canvas through raylib rather than binding its FBO
 * directly. BeginTextureMode() restores all of that state WITHOUT clearing
 * or reallocating the canvas, so previous UI drawing is preserved. The
 * phosphor renderer restores the outer Clay model-view transform afterward.
 *
 * Call this only after an inner EndTextureMode() while the Android canvas
 * is active, never after gui_android_viewport_present_canvas().
 */
void gui_android_viewport_restore_canvas_after_texture(void)
{
    const gui_android_viewport_state_t *v = &s_android_view;
    if (!v->canvas_active || !v->canvas.id) return;
    BeginTextureMode(v->canvas);
}

void gui_android_viewport_shutdown(void)
{
    s_android_view.canvas_active = false;
    if (s_android_view.canvas.id) UnloadRenderTexture(s_android_view.canvas);
    s_android_view.canvas = (RenderTexture2D){0};
    SetMouseOffset(0, 0);
    SetMouseScale(1, 1);
}
#endif /* GUI_ANDROID_VIEWPORT_IMPLEMENTATION */
#endif /* __ANDROID__ */
#endif /* MISRC_GUI_ANDROID_VIEWPORT_H */
