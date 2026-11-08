#ifndef MISRC_GUI_ANDROID_VIEWPORT_H
#define MISRC_GUI_ANDROID_VIEWPORT_H

/* Full-frame touch pinch zoom (Android + iOS).  Only the core translation
 * unit defines GUI_ANDROID_VIEWPORT_IMPLEMENTATION; GUI/UI modules use the
 * declarations. Keeping the implementation here avoids adding a new Meson
 * source file. The implementation is plain raylib API (touch events +
 * render textures), so the same code serves Android (rcore_android.c) and
 * iOS (rcore_desktop_sdl.c) touch input unchanged.
 */
#if defined(__ANDROID__) || defined(MISRC_IOS)
#include <stdbool.h>
#include <raylib.h>

void gui_android_viewport_update(void);
bool gui_android_viewport_block_controls(void);
bool gui_android_viewport_click_pressed(int button);
Vector2 gui_android_viewport_pointer(void);
bool gui_android_viewport_begin_canvas(void);
void gui_android_viewport_present_canvas(bool active);
void gui_android_viewport_restore_canvas_after_texture(void);
/* Native-resolution render target size on iOS (the inset letterbox rect);
 * layout code uses this instead of the raw screen size. Android n/a. */
#if defined(MISRC_IOS)
void gui_ios_view_layout_size(int *w, int *h);
#endif
/* Native-resolution zoom transform, also used for Clay scissor bounds. */
void gui_android_viewport_get_render_transform(float *zoom, float *x, float *y);
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

#if defined(MISRC_IOS)
/* Letterbox: the UI canvas is rendered NATIVE-RESOLUTION into an inset
 * rect that starts at the corner curve (~64pt, clearing the rounded
 * corners and the camera cutout); the rest is black bars (the full-screen
 * clear in present_canvas). Native res = no downscale blur; the layout
 * sizes itself to the inset rect via gui_ios_view_layout_size(). */
#include <stdio.h>
#include <stdlib.h>
#define MISRC_IOS_VIEW_INSET 64
/* DPI scale: physical render px per screen POINT. Without
 * FLAG_WINDOW_HIGHDPI this is 1 (the old blurry 1x rendering). */
static float ios_view_dpi(void)
{
    const int sw = GetScreenWidth();
    return (sw > 0) ? (float)GetRenderWidth() / (float)sw : 1.0f;
}
/* Fit rect in POINT space (layout/input/pan live in points; the canvas
 * and blit rect are physical = points * dpi). */
static void ios_view_fit_rect(int *ox, int *oy, int *cw, int *ch)
{
    const int W = GetScreenWidth(), H = GetScreenHeight();
    if (W <= 0 || H <= 0) {
        *ox = *oy = 0;
        *cw = (W > 0) ? W : 1;
        *ch = (H > 0) ? H : 1;
        return;
    }
    if (W > H) {
        /* Landscape: bars on the left/right, full height. */
        *oy = 0;      *ch = H;
        *ox = MISRC_IOS_VIEW_INSET;
        *cw = W - 2 * MISRC_IOS_VIEW_INSET;
    } else {
        /* Portrait: bars top/bottom, full width. */
        *ox = 0;      *cw = W;
        *oy = MISRC_IOS_VIEW_INSET;
        *ch = H - 2 * MISRC_IOS_VIEW_INSET;
    }
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
}

void gui_ios_view_layout_size(int *w, int *h)
{
    int ox, oy;
    ios_view_fit_rect(&ox, &oy, w, h);
}

/* TEMPORARY input debug (click investigation): append input events/state
 * to <home>/misrc_input.log so the host can verify what SDL delivers
 * (finger touches vs mouse) and the coordinate mapping. */
static void ios_view_input_log(const char *what, float a_x, float a_y,
                               float b_x, float b_y, int touches, int mdown)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/misrc_input.log", home);
    FILE *f = fopen(path, "a");
    if (!f) return;
    int ox, oy, cw, ch;
    ios_view_fit_rect(&ox, &oy, &cw, &ch);
    gui_android_viewport_state_t *v = &s_android_view;
    fprintf(f, "%s A=(%.0f,%.0f) B=(%.1f,%.1f) touches=%d mdown=%d "
               "screen=(%dx%d) rect=(%d,%d %dx%d) canvas=(%dx%d) "
               "zoom=%.2f pan=(%.0f,%.0f)\n",
            what, a_x, a_y, b_x, b_y, touches, mdown,
            GetScreenWidth(), GetScreenHeight(), ox, oy, cw, ch,
            v->canvas_width, v->canvas_height, v->zoom, v->x, v->y);
    fclose(f);
}
#endif

static float android_view_clamp(float x, float lo, float hi)
{
    return fmaxf(lo, fminf(hi, x));
}

static void android_view_limit_translation(void)
{
    /* Clamp pan to the logical UI bounds. Android: the canvas (== the full
     * physical screen == layout). iOS: the inset letterbox rect in POINTS
     * (pan state is point-space there). */
#if defined(MISRC_IOS)
    int ox, oy, cw, ch;
    ios_view_fit_rect(&ox, &oy, &cw, &ch);
    const float w = (float)cw;
    const float h = (float)ch;
#else
    const float w = (s_android_view.canvas_width > 0)
                        ? (float)s_android_view.canvas_width : (float)GetRenderWidth();
    const float h = (s_android_view.canvas_height > 0)
                        ? (float)s_android_view.canvas_height : (float)GetRenderHeight();
#endif
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
#if defined(MISRC_IOS)
    /* TEMPORARY: per-frame input watcher — logs when touches or mouse
     * activity exist (plus a 1/45-frame heartbeat) so the host can see
     * exactly what SDL delivers. */
    {
        static int s_watch_frame = 0;
        static int s_last_n = -1;
        static bool s_last_mdown = false;
        const bool mdown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        if (n > 0 || mdown || s_last_n != n || s_last_mdown != mdown ||
            (s_watch_frame % 45) == 0) {
            const Vector2 mp = GetMousePosition();
            const Vector2 t0 = (n > 0) ? GetTouchPosition(0) : (Vector2){ -1.0f, -1.0f };
            ios_view_input_log("WATCH", mp.x, mp.y, t0.x, t0.y, n, mdown ? 1 : 0);
        }
        s_last_n = n;
        s_last_mdown = mdown;
        s_watch_frame++;
    }
#endif

    if (n >= 2) {
        const Vector2 a = GetTouchPosition(0);
        const Vector2 b = GetTouchPosition(1);
        Vector2 mid = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
#if defined(MISRC_IOS)
        /* Focus point in canvas coordinates: shift by the letterbox
         * origin (the blit is 1:1, no scaling). */
        {
            int ox, oy, cw2, ch2;
            ios_view_fit_rect(&ox, &oy, &cw2, &ch2);
            mid.x -= (float)ox;
            mid.y -= (float)oy;
        }
#endif
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
#if defined(MISRC_IOS)
            {
                int ox, oy, cw, ch;
                ios_view_fit_rect(&ox, &oy, &cw, &ch);
                ios_view_input_log("TAP", v->click_screen.x, v->click_screen.y,
                                   (v->click_screen.x - (float)ox - v->x) / v->zoom,
                                   (v->click_screen.y - (float)oy - v->y) / v->zoom,
                                   0, 0);
            }
#endif
        }
    }
    v->last_contacts = n;
    android_view_limit_translation();
    // raylib's GetMousePosition() becomes the *unzoomed* virtual coordinate,
    // so Clay hit testing and normal UI pointer math still agree with drawing.
#if defined(MISRC_IOS)
    {
        int ox, oy, cw, ch;
        ios_view_fit_rect(&ox, &oy, &cw, &ch);
        /* 1:1 blit: canvas px == UI px; subtract the letterbox origin and
         * the pan, then divide by the pinch zoom. */
        SetMouseOffset((int)roundf(-(ox + v->x)), (int)roundf(-(oy + v->y)));
        SetMouseScale(1.0f / v->zoom, 1.0f / v->zoom);
    }
#else
    SetMouseOffset((int)roundf(-v->x), (int)roundf(-v->y));
    SetMouseScale(1.0f / v->zoom, 1.0f / v->zoom);
#endif
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
#if defined(MISRC_IOS)
    {
        /* click_screen comes from SDL touch/mouse events in WINDOW POINT
         * space (UpdateTouchPointsSDL scales by screen.width/height which
         * are points; SDL mouse coords are window points). The 1:1 blit
         * maps points->points, so mapping = subtract the letterbox origin
         * and pan, divide by zoom. NO dpi division here (verified: the
         * dpi-dividing revision landed taps at 1/3 coordinates). */
        int ox, oy, cw, ch;
        ios_view_fit_rect(&ox, &oy, &cw, &ch);
        return (Vector2){ (v->click_screen.x - (float)ox - v->x) / v->zoom,
                          (v->click_screen.y - (float)oy - v->y) / v->zoom };
    }
#else
    return (Vector2){ (v->click_screen.x - v->x) / v->zoom,
                      (v->click_screen.y - v->y) / v->zoom };
#endif
}

/* Draw the *elements* at the final pinch zoom instead of magnifying a
 * previously rasterized screen-sized bitmap. The two bundled fonts already
 * use 64px atlases, so their glyphs keep their detail at normal pinch zooms.
 * Keep the off-screen canvas at physical display resolution: no zoom^2 GPU
 * memory consumption and no change to Clay's logical layout or pointer map.
 */
static void android_view_apply_canvas_transform(const gui_android_viewport_state_t *v)
{
#if defined(MISRC_IOS)
    /* Canvas is PHYSICAL px; UI layout and pan state are POINTS: scale by
     * the DPI factor so the point-space UI fills the physical canvas. */
    const float dpi = ios_view_dpi();
    rlTranslatef(v->x * dpi, v->y * dpi, 0.0f);
    rlScalef(v->zoom * dpi, v->zoom * dpi, 1.0f);
#else
    rlTranslatef(v->x, v->y, 0.0f);
    rlScalef(v->zoom, v->zoom, 1.0f);
#endif
}

void gui_android_viewport_get_render_transform(float *zoom, float *x, float *y)
{
    if (zoom) *zoom = s_android_view.zoom;
    if (x) *x = s_android_view.x;
    if (y) *y = s_android_view.y;
}

bool gui_android_viewport_begin_canvas(void)
{
    gui_android_viewport_state_t *v = &s_android_view;
    /* Always draw through the canvas, including 1x. The direct-screen path
     * previously produced the clipping seen on some Android handsets. */
#if defined(MISRC_IOS)
    /* PHYSICAL-resolution canvas: fit rect (points) * DPI. The UI layout
     * stays point-space (gui_ios_view_layout_size) and is scaled up by
     * the DPI factor inside the canvas transform. */
    int fit_w, fit_h;
    {
        int ox, oy;
        ios_view_fit_rect(&ox, &oy, &fit_w, &fit_h);
    }
    const float dpi = ios_view_dpi();
    const int w = (int)(fit_w * dpi), h = (int)(fit_h * dpi);
#else
    const int w = GetRenderWidth(), h = GetRenderHeight();
#endif
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
    android_view_apply_canvas_transform(v);
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
    /* Already drawn at pinch zoom; present at native pixel size. Scaling
     * the finished canvas again would reintroduce blurred text. */
#if defined(MISRC_IOS)
    /* Letterbox: blit the physical canvas 1:1 into the PHYSICAL inset
     * rect (fit points * DPI); the full-screen clear leaves the bars
     * starting at the corner curve. */
    {
        int ox, oy, cw, ch;
        ios_view_fit_rect(&ox, &oy, &cw, &ch);
        const float dpi = ios_view_dpi();
        const Rectangle dest = { (float)ox * dpi, (float)oy * dpi,
                                 (float)cw * dpi, (float)ch * dpi };
        DrawTexturePro(v->canvas.texture, source, dest, (Vector2){0, 0}, 0, WHITE);
    }
#else
    const Rectangle dest = { 0, 0,
                             (float)v->canvas_width,
                             (float)v->canvas_height };
    DrawTexturePro(v->canvas.texture, source, dest, (Vector2){0, 0}, 0, WHITE);
#endif
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
    /* Nested phosphor/FFT textures reset raylib's framebuffer AND its
     * projection/modelview. Restore the exact outer canvas transform. */
    android_view_apply_canvas_transform(v);
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
#endif /* __ANDROID__ || MISRC_IOS */
#endif /* MISRC_GUI_ANDROID_VIEWPORT_H */
