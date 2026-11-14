/*
 * MISRC GUI - Composite Vectorscope Panel
 * See gui_vectorscope_panel.h and signal/gui_vectorscope.h.
 *
 * Graticule geometry ported from tbc-tools tbc-analyse vectorscopedialog.cpp
 * (GPL-3.0-or-later, (c) 2018-2025 Simon Inns, 2022 Adam Sampson): 2-degree
 * ticks, crosshair, colour-bar target boxes with zones, the per-target
 * colours, and the multi-colour point colouriser (BT.601 inverse at Y=0.5).
 * Lengths are tbc's pixel values at its 1024 px scope, scaled to the panel.
 * The NTSC I/Q axes are drawn at their standard angles (I 123 deg, Q 33 deg).
 */
#include "gui_vectorscope_panel.h"
#include "../signal/gui_cvbs.h"
#include "../signal/gui_vectorscope.h"
#include "../core/gui_app.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_chrome_hide.h"
#include "gui_text.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VS_CELLS ((size_t)GUI_VSCOPE_GRID * GUI_VSCOPE_GRID)

enum { VS_GRAT_75 = 0, VS_GRAT_100 = 1, VS_GRAT_OFF = 2, VS_GRAT_COUNT };
enum { VS_PLOT_POINTS = 0, VS_PLOT_DENSITY = 1, VS_PLOT_COUNT };
enum { VS_BTN_SYSTEM = 0, VS_BTN_GRAT, VS_BTN_PLOT, VS_BTN_COLOUR, VS_BTN_COUNT };

typedef struct {
    // Display thread: the CVBS plugin decode chain + the accumulator it feeds.
    gui_cvbs_feed_t *feed;
    gui_vectorscope_t scope;
    atomic_int pending_system;     // -1 none, else 0=PAL 1=NTSC 2=SECAM
    atomic_int decoder_system;     // The decoder's current pick (for the UI)
    atomic_bool auto_system;       // Auto: 50 Hz fields -> PAL, 59.94 -> NTSC
    uint32_t seen_measures;        // Display thread: last vsync measurement read
    int vote_system, votes;        // Display thread: consecutive mismatching fields

    // Render thread: the last two fields (one frame, like tbc's "All").
    uint32_t *field_counts[2];
    int field_slot;
    uint8_t *hue_lut;              // Multi-colour RGB per grid cell
    uint8_t *rgba;                 // Texture staging
    float *glow;                   // Per-cell intensity after the dot spread
    float built_side;              // Scope side (px) the image was built for
    Texture2D tex;
    bool tex_valid;
    bool image_dirty;
    bool have_data;
    uint64_t points;
    double last_field_time;

    // Options (tbc-analyse: graticule 75/100/none, point/density, multi-colour)
    int graticule;
    int plot;
    bool multicolour;

    Rectangle btn[VS_BTN_COUNT];
    gui_chrome_hide_t chrome_hide;
} vectorscope_panel_t;

//-----------------------------------------------------------------------------
// tbc-analyse colours and geometry
//-----------------------------------------------------------------------------

static Color vs_target_color(int rgb) {
    switch (rgb) {
        case 1: return (Color){ 70, 150, 255, 255 };  // Blue
        case 2: return (Color){ 70, 230, 120, 255 };  // Green
        case 3: return (Color){ 90, 235, 235, 255 };  // Cyan
        case 4: return (Color){ 255, 90, 90, 255 };   // Red
        case 5: return (Color){ 230, 90, 230, 255 };  // Magenta
        case 6: return (Color){ 245, 215, 80, 255 };  // Yellow
        default: return WHITE;
    }
}

static Color vs_alpha(Color c, unsigned char a) { c.a = a; return c; }

// Standard angle (deg, counter-clockwise from +U) and magnitude (px) -> screen.
static Vector2 vs_point(Vector2 centre, double deg, double mag) {
    double t = deg * M_PI / 180.0;
    return (Vector2){ (float)(centre.x + mag * cos(t)), (float)(centre.y - mag * sin(t)) };
}

// tbc colorizePoint: BT.601 inverse at Y = 0.5, max-component normalised.
static void vs_hue(double u, double v, uint8_t *rgb) {
    double r = 0.5 + v / 0.877283;
    double b = 0.5 + u / 0.492111;
    double g = (0.5 - 0.299 * r - 0.114 * b) / 0.587;
    r = fmin(1.0, fmax(0.0, r));
    g = fmin(1.0, fmax(0.0, g));
    b = fmin(1.0, fmax(0.0, b));
    double m = fmax(r, fmax(g, b));
    if (m > 0.001) { r /= m; g /= m; b /= m; } else { r = g = b = 1.0; }
    rgb[0] = (uint8_t)(r * 255.0);
    rgb[1] = (uint8_t)(g * 255.0);
    rgb[2] = (uint8_t)(b * 255.0);
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

static void vs_destroy(void *state_ptr);

static void *vs_create(void) {
    vectorscope_panel_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->feed = gui_cvbs_feed_create();
    bool ok = s->feed && gui_vectorscope_init(&s->scope);
    s->field_counts[0] = calloc(VS_CELLS, sizeof(uint32_t));
    s->field_counts[1] = calloc(VS_CELLS, sizeof(uint32_t));
    s->hue_lut = malloc(VS_CELLS * 3);
    s->rgba = calloc(VS_CELLS, 4);
    s->glow = calloc(VS_CELLS, sizeof(float));
    if (!ok || !s->field_counts[0] || !s->field_counts[1] || !s->hue_lut || !s->rgba ||
        !s->glow) {
        vs_destroy(s);
        return NULL;
    }
    cvbs_decoder_t *dec = gui_cvbs_feed_decoder(s->feed);
    dec->vscope = &s->scope;
    atomic_init(&s->pending_system, -1);
    int sys = (dec->state.format == CVBS_FORMAT_NTSC) ? 1
            : (dec->state.format == CVBS_FORMAT_SECAM) ? 2 : 0;
    atomic_init(&s->decoder_system, sys);
    atomic_init(&s->auto_system, true);

    double half = 0.5 * GUI_VSCOPE_GRID;
    for (int y = 0; y < GUI_VSCOPE_GRID; y++) {
        for (int x = 0; x < GUI_VSCOPE_GRID; x++) {
            double u = ((double)x + 0.5 - half) / half * GUI_VSCOPE_FULL_SCALE;
            double v = (half - (double)y - 0.5) / half * GUI_VSCOPE_FULL_SCALE;
            vs_hue(u, v, s->hue_lut + ((size_t)y * GUI_VSCOPE_GRID + x) * 3);
        }
    }
    s->graticule = VS_GRAT_75;
    s->plot = VS_PLOT_POINTS;
    s->multicolour = false;
    return s;
}

static void vs_destroy(void *state_ptr) {
    vectorscope_panel_t *s = state_ptr;
    if (!s) return;
    if (s->feed) {
        gui_cvbs_feed_decoder(s->feed)->vscope = NULL;
        gui_cvbs_feed_destroy(s->feed);
    }
    gui_vectorscope_free(&s->scope);
    if (s->tex_valid) UnloadTexture(s->tex);
    free(s->field_counts[0]);
    free(s->field_counts[1]);
    free(s->hue_lut);
    free(s->rgba);
    free(s->glow);
    free(s);
}

static void vs_clear(void *state_ptr) {
    vectorscope_panel_t *s = state_ptr;
    if (!s) return;
    memset(s->field_counts[0], 0, VS_CELLS * sizeof(uint32_t));
    memset(s->field_counts[1], 0, VS_CELLS * sizeof(uint32_t));
    s->have_data = false;
    s->image_dirty = true;
}

//-----------------------------------------------------------------------------
// Processing (display thread)
//-----------------------------------------------------------------------------

static void vs_process(void *state_ptr, const int16_t *samples, size_t count,
                       uint32_t sample_rate) {
    vectorscope_panel_t *s = state_ptr;
    if (!s || !s->feed || !samples || count == 0) return;
    int pending = atomic_exchange(&s->pending_system, -1);
    if (pending >= 0 && pending <= 2) {
        gui_cvbs_feed_set_system(s->feed, pending);
        gui_vectorscope_reset(&s->scope);
        atomic_store(&s->decoder_system, pending);
    }
    gui_cvbs_feed_process(s->feed, samples, count, sample_rate);

    // Auto system: a real source carries no Test Gen system to follow, and a
    // wrong standard never burst-locks (a PAL source on the NTSC default
    // plotted only a centre blob, real MS2130 2026-11-14). The field period
    // is measured standard-independently (cvbs_vsync_state_t.last_field_us);
    // 3 consecutive fields of the other rate switch PAL <-> NTSC. A 625-line
    // source stays SECAM if SECAM was picked by the Test Gen follow.
    const cvbs_vsync_state_t *vs = &gui_cvbs_feed_decoder(s->feed)->vsync;
    if (atomic_load(&s->auto_system) && vs->field_measures != s->seen_measures) {
        s->seen_measures = vs->field_measures;
        int det = gui_vectorscope_field_period_system(vs->last_field_us);
        int want = det == GUI_VSCOPE_SYS_PAL ? 0 : det == GUI_VSCOPE_SYS_NTSC ? 1 : -1;
        int cur = atomic_load(&s->decoder_system);
        bool match = want == cur || (want == 0 && cur == 2);
        if (want < 0) {
            // Unclassifiable interval (a missed or false sync): no vote.
        } else if (match) {
            s->votes = 0;
        } else if (want == s->vote_system && ++s->votes >= 3) {
            s->votes = 0;
            gui_cvbs_feed_set_system(s->feed, want);
            gui_vectorscope_reset(&s->scope);
            atomic_store(&s->decoder_system, want);
        } else if (want != s->vote_system) {
            s->vote_system = want;
            s->votes = 1;
        }
    }
}

static void vs_follow_system(void *state_ptr, int system_select) {
    vectorscope_panel_t *s = state_ptr;
    if (s && system_select >= 0 && system_select <= 2) {
        atomic_store(&s->pending_system, system_select);
    }
}

//-----------------------------------------------------------------------------
// Rendering (render thread)
//-----------------------------------------------------------------------------

static void vs_build_image(vectorscope_panel_t *s, float side) {
    const uint32_t *a = s->field_counts[0], *b = s->field_counts[1];
    const Color mono = (Color){ 0, 255, 0, 255 };  // tbc point colour

    // Dot spread: one grid cell is side/512 screen px (often < 1 px), so a
    // single lit cell drew a hairline trace (real MS2130 bars, 2026-11-14:
    // "a bit too tight"). Each hit lights a disc ~2.5 px across at the
    // current panel size, full at the centre and dimmer at the rim.
    float cell_px = side > 1.0f ? side / (float)GUI_VSCOPE_GRID : 1.0f;
    int r = (int)ceilf(1.25f / cell_px);
    if (r < 1) r = 1;
    if (r > 4) r = 4;
    const float rr = (float)r + 0.5f;
    memset(s->glow, 0, VS_CELLS * sizeof(float));
    for (int y = 0; y < GUI_VSCOPE_GRID; y++) {
        for (int x = 0; x < GUI_VSCOPE_GRID; x++) {
            size_t i = (size_t)y * GUI_VSCOPE_GRID + x;
            uint32_t c = a[i] + b[i];
            if (c == 0) continue;
            // Point plot: any hit lights the dot. Density: tbc's additive
            // alpha-14 accumulation (saturates at ~18 hits).
            float k = (s->plot == VS_PLOT_DENSITY) ? fminf(1.0f, (float)c * 14.0f / 255.0f) : 1.0f;
            for (int dy = -r; dy <= r; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= GUI_VSCOPE_GRID) continue;
                for (int dx = -r; dx <= r; dx++) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= GUI_VSCOPE_GRID) continue;
                    float dist = sqrtf((float)(dx * dx + dy * dy)) / rr;
                    if (dist > 1.0f) continue;
                    float v = k * (1.0f - 0.6f * dist);
                    float *g = s->glow + (size_t)yy * GUI_VSCOPE_GRID + xx;
                    if (v > *g) *g = v;
                }
            }
        }
    }
    s->built_side = side;

    for (size_t i = 0; i < VS_CELLS; i++) {
        uint8_t *px = s->rgba + i * 4;
        float k = s->glow[i];
        if (k <= 0.0f) { px[0] = px[1] = px[2] = 0; px[3] = 255; continue; }
        const uint8_t *rgb = s->multicolour ? s->hue_lut + i * 3
                                            : (const uint8_t *)&mono;
        px[0] = (uint8_t)(rgb[0] * k);
        px[1] = (uint8_t)(rgb[1] * k);
        px[2] = (uint8_t)(rgb[2] * k);
        px[3] = 255;
    }
}

static void vs_draw_graticule(const vectorscope_panel_t *s, int system,
                              Vector2 c, float side) {
    const float sc = side / 1024.0f;               // tbc pixel lengths at 1024 px
    const float radius = side * 0.5f - 1.0f;
    const double px_per_unit = (side * 0.5) / GUI_VSCOPE_FULL_SCALE;
    const Color line = vs_alpha(WHITE, 200);

    // Crosshair, outer circle, 2-degree ticks (major every 10).
    DrawLineV((Vector2){ c.x, c.y - radius }, (Vector2){ c.x, c.y + radius }, line);
    DrawLineV((Vector2){ c.x - radius, c.y }, (Vector2){ c.x + radius, c.y }, line);
    DrawCircleLinesV(c, radius, line);
    for (int deg = 0; deg < 360; deg += 2) {
        bool major = (deg % 10) == 0;
        float len = (major ? 18.0f : 10.0f) * sc;
        DrawLineEx(vs_point(c, deg, radius - len), vs_point(c, deg, radius),
                   major ? fmaxf(1.0f, 2.0f * sc) : 1.0f, line);
    }

    if (system == GUI_VSCOPE_SYS_NTSC) {
        // I/Q axes, inner 20% to the edge, at the standard angles.
        static const struct { const char *text; double deg, off; } axes[] = {
            { "I", 123.0, 4.0 }, { "-I", 303.0, -4.0 },
            { "Q", 33.0, -4.0 }, { "-Q", 213.0, 4.0 },
        };
        int font = (int)fmaxf(12.0f, 24.0f * sc * 1.33f);
        for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
            DrawLineV(vs_point(c, axes[i].deg, 0.2 * radius),
                      vs_point(c, axes[i].deg, radius), line);
            Vector2 at = vs_point(c, axes[i].deg + axes[i].off, radius - 22.0f * sc - font * 0.5f);
            int w = gui_text_measure(axes[i].text, font);
            gui_text_draw(axes[i].text, at.x - w / 2.0f, at.y - font / 2.0f, font,
                          (Color){ 200, 200, 200, 255 });
        }
    }

    if (s->graticule == VS_GRAT_OFF) return;
    static const char *names[7] = { "", "B", "G", "Cy", "R", "Mg", "Yl" };
    double pct = (s->graticule == VS_GRAT_100) ? 1.0 : 0.75;
    int label_font = (int)fmaxf(12.0f, 14.0f * sc * 1.33f);
    for (int rgb = 1; rgb < 7; rgb++) {
        double u, v;
        gui_vectorscope_bar_target(system, rgb, pct, &u, &v);
        double mag = hypot(u, v) * px_per_unit;
        double deg = atan2(v, u) * 180.0 / M_PI;
        Color col = vs_target_color(rgb);

        // Zone: +-13 degrees, +-14% radial (tbc). raylib angles run clockwise.
        float span = (float)(mag * 0.14);
        float inner = fmaxf(0.0f, (float)mag - span);
        float outer = fminf(radius, (float)mag + span);
        DrawRing(c, inner, outer, (float)(-deg - 13.0), (float)(-deg + 13.0), 24, vs_alpha(col, 52));
        DrawRingLines(c, inner, outer, (float)(-deg - 13.0), (float)(-deg + 13.0), 24, vs_alpha(col, 180));

        // Target box with crosshair (42 / 22 px at 1024).
        Vector2 p = vs_point(c, deg, mag);
        float box = 42.0f * sc, cross = 22.0f * sc;
        DrawRectangleLinesEx((Rectangle){ p.x - box / 2, p.y - box / 2, box, box },
                             fmaxf(1.0f, 2.0f * sc), vs_alpha(col, 220));
        DrawLineV((Vector2){ p.x - cross / 2, p.y }, (Vector2){ p.x + cross / 2, p.y }, vs_alpha(col, 220));
        DrawLineV((Vector2){ p.x, p.y - cross / 2 }, (Vector2){ p.x, p.y + cross / 2 }, vs_alpha(col, 220));

        Vector2 lp = vs_point(c, deg, mag + 48.0f * sc);
        int w = gui_text_measure(names[rgb], label_font);
        gui_text_draw(names[rgb], lp.x - w / 2.0f, lp.y - label_font / 2.0f, label_font, col);
    }

    // Burst target(s): the transmitted burst vector, independent of the bar
    // level (NTSC 20 IRE at 180 deg; PAL 3/14 of blanking..white at 135 and
    // 225 deg, the two V-switch phases). Matches where the burst is plotted.
    {
        const bool pal = system == GUI_VSCOPE_SYS_PAL;
        const double amp = pal ? 3.0 / 14.0 : 0.2;
        const double degs[2] = { pal ? 135.0 : 180.0, 225.0 };
        const int count = pal ? 2 : 1;
        const Color bc = { 230, 230, 230, 255 };
        float mag = (float)(amp * px_per_unit);
        float tick = 16.0f * sc;
        float thick = fmaxf(1.0f, 2.0f * sc);
        for (int i = 0; i < count; i++) {
            double deg = degs[i];
            // Short radial tick through the tip plus a cross-tick, a hardware
            // scope's burst mark.
            DrawLineEx(vs_point(c, deg, mag - tick), vs_point(c, deg, mag + tick),
                       thick, vs_alpha(bc, 220));
            Vector2 p = vs_point(c, deg, mag);
            Vector2 a = vs_point(c, deg + 90.0, tick * 0.6f);
            Vector2 b = vs_point(c, deg - 90.0, tick * 0.6f);
            DrawLineEx((Vector2){ p.x + a.x - c.x, p.y + a.y - c.y },
                       (Vector2){ p.x + b.x - c.x, p.y + b.y - c.y }, thick, vs_alpha(bc, 220));
            DrawCircleLinesV(p, 5.0f * sc + 2.0f, vs_alpha(bc, 160));
            const char *txt = "burst";
            Vector2 lp = vs_point(c, deg, mag + tick + label_font * 1.2f);
            int w = gui_text_measure(txt, label_font);
            gui_text_draw(txt, lp.x - w / 2.0f, lp.y - label_font / 2.0f, label_font,
                          vs_alpha(bc, 200));
        }
    }
}

static void vs_render(void *state_ptr, gui_app_t *app, int channel,
                      Rectangle bounds, Color channel_color) {
    (void)app;
    (void)channel;
    (void)channel_color;
    vectorscope_panel_t *s = state_ptr;
    DrawRectangleRec(bounds, COLOR_METER_BG);
    if (!s) return;

    int system = atomic_load(&s->decoder_system) == 1 ? GUI_VSCOPE_SYS_NTSC
               : atomic_load(&s->decoder_system) == 2 ? GUI_VSCOPE_SYS_SECAM
               : GUI_VSCOPE_SYS_PAL;
    uint64_t points = 0;
    int field_system = 0;
    double now = GetTime();
    s->field_slot ^= 1;
    if (gui_vectorscope_take(&s->scope, s->field_counts[s->field_slot], &points, &field_system)) {
        s->have_data = true;
        s->points = points;
        s->last_field_time = now;
        s->image_dirty = true;
    } else {
        s->field_slot ^= 1;   // nothing new: keep the slot order
        if (s->have_data && now - s->last_field_time > 0.5) {
            vs_clear(s);      // feed stopped / lock lost: blank the trace
        }
    }

    if (!s->tex_valid) {
        Image img = { .data = s->rgba, .width = GUI_VSCOPE_GRID, .height = GUI_VSCOPE_GRID,
                      .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        s->tex = LoadTextureFromImage(img);
        s->tex_valid = s->tex.id != 0;
        if (s->tex_valid) SetTextureFilter(s->tex, TEXTURE_FILTER_BILINEAR);
        s->image_dirty = true;
    }
    // Square scope, centred, with a margin for the target labels.
    float side = fminf(bounds.width, bounds.height) - 16.0f;
    if (side < 32.0f) return;
    if (side != s->built_side) s->image_dirty = true;   // Dot size follows the panel
    if (s->tex_valid && s->image_dirty) {
        vs_build_image(s, side);
        UpdateTexture(s->tex, s->rgba);
        s->image_dirty = false;
    }
    Vector2 centre = { bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f };
    Rectangle dst = { centre.x - side * 0.5f, centre.y - side * 0.5f, side, side };
    if (s->tex_valid) {
        DrawTexturePro(s->tex, (Rectangle){ 0, 0, GUI_VSCOPE_GRID, GUI_VSCOPE_GRID },
                       dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
    }
    if (system != GUI_VSCOPE_SYS_SECAM) vs_draw_graticule(s, system, centre, side);

    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
    gui_text_draw("Vectorscope", bounds.x + 8, bounds.y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);

    const char *msg = NULL;
    if (system == GUI_VSCOPE_SYS_SECAM) msg = "SECAM: FM chroma has no burst phase";
    else if (!s->have_data || s->points == 0) msg = "No burst-locked colour";
    if (msg) {
        int w = gui_text_measure(msg, FONT_SIZE_DROPDOWN_OPT);
        gui_text_draw(msg, centre.x - w / 2.0f, bounds.y + bounds.height - 28.0f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);
    }
}

static const char *vs_button_label(const vectorscope_panel_t *s, int b, char *buf, size_t n) {
    static const char *systems[] = { "PAL", "NTSC", "SECAM" };
    static const char *grats[] = { "75%", "100%", "Off" };
    switch (b) {
        case VS_BTN_SYSTEM: {
            int sys = atomic_load(&((vectorscope_panel_t *)s)->decoder_system);
            const char *name = systems[(sys >= 0 && sys <= 2) ? sys : 0];
            if (atomic_load(&((vectorscope_panel_t *)s)->auto_system))
                snprintf(buf, n, "System: Auto (%s)", name);
            else
                snprintf(buf, n, "System: %s", name);
            break;
        }
        case VS_BTN_GRAT: snprintf(buf, n, "Graticule: %s", grats[s->graticule]); break;
        case VS_BTN_PLOT: snprintf(buf, n, "Plot: %s", s->plot == VS_PLOT_DENSITY ? "Density" : "Points"); break;
        default: snprintf(buf, n, "Colour: %s", s->multicolour ? "Multi" : "Mono"); break;
    }
    return buf;
}

static void vs_render_overlay(void *state_ptr, Rectangle bounds) {
    vectorscope_panel_t *s = state_ptr;
    if (!s) return;
    if (gui_chrome_hide_hidden(&s->chrome_hide)) {
        for (int b = 0; b < VS_BTN_COUNT; b++) s->btn[b] = (Rectangle){ 0 };
        return;
    }
    // Right-aligned vertical stack, the Video panel's overlay button style.
    const float btn_h = 18.0f, gap = 6.0f;
    float y = bounds.y + 8.0f;
    for (int b = 0; b < VS_BTN_COUNT; b++) {
        char label[40];
        vs_button_label(s, b, label, sizeof(label));
        float tw = (float)gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT);
        float w = fmaxf(120.0f, tw + 16.0f);
        Rectangle r = { bounds.x + bounds.width - 8.0f - w, y, w, btn_h };
        s->btn[b] = r;
        bool hover = CheckCollisionPointRec(gui_ui_get_mouse_position(), r);
        DrawRectangleRounded(r, 0.15f, 4, hover ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
        gui_text_draw(label, r.x + (w - tw) / 2.0f, r.y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        y += btn_h + gap;
    }
}

static bool vs_handle_click(void *state_ptr, gui_app_t *app, int channel,
                            Vector2 click, Rectangle bounds) {
    (void)app;
    (void)channel;
    vectorscope_panel_t *s = state_ptr;
    if (!s) return false;
    if (!CheckCollisionPointRec(click, bounds)) {
        gui_chrome_hide_reset(&s->chrome_hide);
        return false;
    }
    if (!gui_chrome_hide_hidden(&s->chrome_hide)) {
        for (int b = 0; b < VS_BTN_COUNT; b++) {
            if (!CheckCollisionPointRec(click, s->btn[b])) continue;
            switch (b) {
                case VS_BTN_SYSTEM: {
                    // Auto -> PAL -> NTSC -> SECAM -> Auto.
                    if (atomic_load(&s->auto_system)) {
                        atomic_store(&s->auto_system, false);
                        vs_follow_system(s, 0);
                    } else if (atomic_load(&s->decoder_system) >= 2) {
                        atomic_store(&s->auto_system, true);
                    } else {
                        vs_follow_system(s, atomic_load(&s->decoder_system) + 1);
                    }
                    break;
                }
                case VS_BTN_GRAT: s->graticule = (s->graticule + 1) % VS_GRAT_COUNT; break;
                case VS_BTN_PLOT: s->plot = (s->plot + 1) % VS_PLOT_COUNT; break;
                default: s->multicolour = !s->multicolour; break;
            }
            s->image_dirty = true;
            gui_chrome_hide_reset(&s->chrome_hide);
            return true;
        }
    }
    // Double-click elsewhere toggles the overlay chrome (shared behaviour).
    return gui_chrome_hide_feed_click(&s->chrome_hide, click);
}

static const panel_vtable_t s_vectorscope_vtable = {
    .name = "Vectorscope",
    .create = vs_create,
    .destroy = vs_destroy,
    .clear = vs_clear,
    .process = vs_process,
    .render = vs_render,
    .render_overlay = vs_render_overlay,
    .handle_click = vs_handle_click,
    .handle_scroll = NULL,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_vectorscope_panel_register(void) {
    panel_register(PANEL_VIEW_VECTORSCOPE, &s_vectorscope_vtable);
    gui_cvbs_set_vectorscope_follow(vs_follow_system);
}
