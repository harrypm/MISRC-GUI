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
 *
 * Standard / Advanced modes and the trace renderer follow tbc-analyse's
 * Basic / Advanced vectorscope tabs (the Advanced one ported there from
 * decode-orc's composite measurement scope):
 *  - Advanced: no V-switch correction (PAL draws both mirrored target sets,
 *    labelled upper / lower case), the burst-locked flywheel reference, line
 *    window Active / Whole / Burst, field range Active / Whole, readouts
 *    (burst level, phase jitter, V split, chroma / burst).
 *  - Both modes: decode-orc's beam renderer - each sample's hit and, with
 *    Trace on, the beam path from the previous sample spread by a Gaussian
 *    beam spot; brightness linear in dwell up to gain / anchor (the anchor is
 *    the lower of 8 samples a line on one cell and the half-dwell level), the
 *    transits held near 0.04 x gain, added over the graticule. Bars come out
 *    as bright points with dim lines between them (Stephen Neal, 2026-11-17:
 *    "the intensity should reflect the amount of content of that value").
 */
#include "gui_vectorscope_panel.h"
#include "../signal/gui_cvbs.h"
#include "../signal/gui_vectorscope.h"
#include "../core/gui_app.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_chrome_hide.h"
#include "gui_text.h"
#include "gui_oscilloscope.h"   // gui_waveform_test_mode_active
#include "../signal/gui_testgen.h"  // gui_testgen_clamp_system
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test Gen follow hold: field measurements (~5 s) before Auto may vote
// against a followed system that never arrives.
#define GEN_FOLLOW_HOLD_FIELDS 250

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VS_CELLS ((size_t)GUI_VSCOPE_GRID * GUI_VSCOPE_GRID)
// Beam spot: decode-orc's sigma = canvas / 410, on this grid ~1.25 cells.
#define VS_SPOT_SIGMA ((double)GUI_VSCOPE_GRID / 410.0)
#define VS_SPOT_R 3     // ceil(3 sigma), as decode-orc
// Spread work grids: zero-padded by two spot radii, so the separable passes
// run branch-free over the content box (the box grows by R per pass).
#define VS_PAD (2 * VS_SPOT_R)
#define VS_P (GUI_VSCOPE_GRID + 2 * VS_PAD)
#define VS_PCELLS ((size_t)VS_P * VS_P)
#define VS_PIDX(x, y) ((size_t)((y) + VS_PAD) * VS_P + (size_t)((x) + VS_PAD))
// Density law LUT: x^(1/gain) indexed by the float's exponent (2^-24..1) and
// top 7 mantissa bits (<0.8% steps) - no powf per cell.
#define VS_DENS_LUT (25 * 128)

// Both = the 75% and 100% bar targets together (one label per colour, at the
// outer 100% target), for sources whose bar level is not known in advance.
enum { VS_GRAT_75 = 0, VS_GRAT_100 = 1, VS_GRAT_BOTH = 2, VS_GRAT_OFF = 3, VS_GRAT_COUNT };
enum { VS_PLOT_POINTS = 0, VS_PLOT_DENSITY = 1, VS_PLOT_COUNT };
// Colour: decode-orc green / BT.601 hue per cell / Blend (field 1 yellow,
// field 2 cyan, added - tbc's Blend colours).
enum { VS_COL_MONO = 0, VS_COL_MULTI = 1, VS_COL_BLEND = 2, VS_COL_COUNT };
enum { VS_BTN_SYSTEM = 0, VS_BTN_MODE, VS_BTN_LINES, VS_BTN_FIELD, VS_BTN_GRAT, VS_BTN_PLOT,
       VS_BTN_COLOUR, VS_BTN_TRACE, VS_BTN_GAIN, VS_BTN_COUNT };

typedef struct {
    // Display thread: the CVBS plugin decode chain + the accumulator it feeds.
    gui_cvbs_feed_t *feed;
    gui_vectorscope_t scope;
    atomic_int pending_system;     // -1 none, else 0=PAL 1=NTSC 2=SECAM
    atomic_int decoder_system;     // The decoder's current pick (for the UI)
    atomic_bool auto_system;       // Auto: 50 Hz fields -> PAL, 59.94 -> NTSC
    uint32_t seen_measures;        // Display thread: last vsync measurement read
    int vote_system, votes;        // Display thread: consecutive mismatching fields
    // Test Gen follow (the Video panels' behaviour): the render thread
    // watches the generator's system (app->settings.video_system) in test
    // signal mode and queues it; the decode thread then HOLDS Auto voting
    // until a measured field period matches the followed system (or
    // GEN_FOLLOW_HOLD_FIELDS pass), so the old system's tail still in the
    // pipeline - and the generator's regeneration gap - cannot vote the
    // panel back (NTSC -> SECAM otherwise fell back to NTSC, then PAL).
    int follow_hold;               // Decode thread: field measurements left
    int follow_target;             // Decode thread: the followed system
    int gen_seen;                  // Render thread: last generator system followed (-1 none)

    // Render thread: the newest field of each parity (one frame).
    uint32_t *hits[2], *trans[2];
    gui_vscope_meta_t meta[2];
    bool have_slot[2];
    uint32_t *take_hits, *take_trans;   // take buffers, swapped into a slot
    // Beam spread (padded VS_P^2; fin / ftmp are zero outside what a build
    // writes and are zeroed again after it).
    float *fin, *ftmp;
    float *bh[2], *bt[2];          // Spread hits / transits: per field (Blend), else [0] = both
    int img_x0, img_x1, img_y0, img_y1;   // rgba region the last build wrote (x0 > x1: none)
    int upd_y0, upd_y1;            // rgba rows changed since the last upload
    float dens_lut[VS_DENS_LUT];
    int dens_lut_gain;             // gain the LUT holds (0: none)
    float row0[GUI_VSCOPE_GRID], row1[GUI_VSCOPE_GRID];   // Per-row levels
    uint8_t *hue_lut;              // Multi-colour RGB per grid cell
    uint8_t *rgba;                 // Texture staging
    float spot[VS_SPOT_R + 1];
    Texture2D tex;
    bool tex_valid;
    bool image_dirty;
    bool have_data;
    double last_field_time;

    // Options (tbc-analyse's Basic / Advanced controls; decode-orc's gain).
    int mode;
    int lines_view, field_view;
    int graticule;
    int plot;
    int colour;
    bool trace;
    int gain;                      // Intensity 1..10 (decode-orc default 3)

    Rectangle btn[VS_BTN_COUNT];
    gui_chrome_hide_t chrome_hide;
} vectorscope_panel_t;

// Launch state for new panels (--vector; hard rule
// dev/gui_automation_flags_rules.md).
static struct {
    int mode, lines_view, field_view, graticule, plot, colour, gain;
    bool trace;
    int system;      // -1 Auto, else 0=PAL 1=NTSC 2=SECAM
    bool from_flag;
} s_vs_launch = { GUI_VSCOPE_MODE_STANDARD, GUI_VSCOPE_LINES_WHOLE, GUI_VSCOPE_FIELD_ACTIVE,
                  VS_GRAT_75, VS_PLOT_POINTS, VS_COL_MONO, 3, true, -1, false };

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

static const char *vs_system_name(int sys) {
    static const char *names[] = { "PAL", "NTSC", "SECAM" };
    return names[(sys >= 0 && sys <= 2) ? sys : 0];
}

//-----------------------------------------------------------------------------
// Launch flag (--vector key=value,...)
//-----------------------------------------------------------------------------

static bool vs_flag_value(const char *key, const char *val, size_t len) {
    #define VS_IS(lit) (len == sizeof(lit) - 1 && strncmp(val, lit, len) == 0)
    if (strcmp(key, "mode") == 0) {
        if (VS_IS("standard")) { s_vs_launch.mode = GUI_VSCOPE_MODE_STANDARD; return true; }
        if (VS_IS("advanced")) { s_vs_launch.mode = GUI_VSCOPE_MODE_ADVANCED; return true; }
    } else if (strcmp(key, "lines") == 0) {
        if (VS_IS("active")) { s_vs_launch.lines_view = GUI_VSCOPE_LINES_ACTIVE; return true; }
        if (VS_IS("whole")) { s_vs_launch.lines_view = GUI_VSCOPE_LINES_WHOLE; return true; }
        if (VS_IS("burst") || VS_IS("porch")) {   // porch: SECAM's name for it
            s_vs_launch.lines_view = GUI_VSCOPE_LINES_BURST;
            return true;
        }
    } else if (strcmp(key, "field") == 0) {
        if (VS_IS("active")) { s_vs_launch.field_view = GUI_VSCOPE_FIELD_ACTIVE; return true; }
        if (VS_IS("whole")) { s_vs_launch.field_view = GUI_VSCOPE_FIELD_WHOLE; return true; }
    } else if (strcmp(key, "graticule") == 0) {
        if (VS_IS("75")) { s_vs_launch.graticule = VS_GRAT_75; return true; }
        if (VS_IS("100")) { s_vs_launch.graticule = VS_GRAT_100; return true; }
        if (VS_IS("both")) { s_vs_launch.graticule = VS_GRAT_BOTH; return true; }
        if (VS_IS("off")) { s_vs_launch.graticule = VS_GRAT_OFF; return true; }
    } else if (strcmp(key, "plot") == 0) {
        if (VS_IS("points")) { s_vs_launch.plot = VS_PLOT_POINTS; return true; }
        if (VS_IS("density")) { s_vs_launch.plot = VS_PLOT_DENSITY; return true; }
    } else if (strcmp(key, "colour") == 0) {
        if (VS_IS("mono")) { s_vs_launch.colour = VS_COL_MONO; return true; }
        if (VS_IS("multi")) { s_vs_launch.colour = VS_COL_MULTI; return true; }
        if (VS_IS("blend")) { s_vs_launch.colour = VS_COL_BLEND; return true; }
    } else if (strcmp(key, "trace") == 0) {
        if (VS_IS("on")) { s_vs_launch.trace = true; return true; }
        if (VS_IS("off")) { s_vs_launch.trace = false; return true; }
    } else if (strcmp(key, "intensity") == 0) {
        if (len >= 1 && len <= 2) {
            char buf[3] = { 0 };
            memcpy(buf, val, len);
            char *end = NULL;
            long g = strtol(buf, &end, 10);
            if (end && *end == 0 && g >= 1 && g <= 10) { s_vs_launch.gain = (int)g; return true; }
        }
    } else if (strcmp(key, "system") == 0) {
        if (VS_IS("auto")) { s_vs_launch.system = -1; return true; }
        if (VS_IS("pal")) { s_vs_launch.system = 0; return true; }
        if (VS_IS("ntsc")) { s_vs_launch.system = 1; return true; }
        if (VS_IS("secam")) { s_vs_launch.system = 2; return true; }
    }
    #undef VS_IS
    return false;
}

static const char *vs_mode_name(int m) { return m == GUI_VSCOPE_MODE_ADVANCED ? "Advanced" : "Standard"; }
static const char *vs_lines_name(int l, bool secam) {
    if (l == GUI_VSCOPE_LINES_BURST) return secam ? "Porch" : "Burst";
    return l == GUI_VSCOPE_LINES_WHOLE ? "Whole" : "Active";
}
static const char *vs_field_name(int f) { return f == GUI_VSCOPE_FIELD_WHOLE ? "Whole" : "Active"; }
static const char *vs_grat_name(int g) {
    static const char *grats[] = { "75%", "100%", "Both", "Off" };
    return grats[(g >= 0 && g < VS_GRAT_COUNT) ? g : 0];
}
static const char *vs_colour_name(int c) {
    return c == VS_COL_MULTI ? "Multi" : c == VS_COL_BLEND ? "Blend" : "Mono";
}

bool gui_vectorscope_panel_apply_launch_arg(const char *arg) {
    static const char *usage =
        "--vector: expected comma-separated key=value pairs: mode=standard|advanced, "
        "lines=active|whole|burst|porch, field=active|whole, graticule=75|100|both|off, "
        "plot=points|density, colour=mono|multi|blend, trace=on|off, intensity=1..10, "
        "system=auto|pal|ntsc|secam";
    if (!arg || !*arg) {
        fprintf(stderr, "%s (got '')\n", usage);
        return false;
    }
    const char *p = arg;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        char key[16];
        size_t klen = eq ? (size_t)(eq - p) : 0;
        if (!eq || klen == 0 || klen >= sizeof(key)) {
            fprintf(stderr, "%s (got '%s')\n", usage, arg);
            return false;
        }
        memcpy(key, p, klen);
        key[klen] = 0;
        if (!vs_flag_value(key, eq + 1, len - klen - 1)) {
            fprintf(stderr, "%s (got '%s')\n", usage, arg);
            return false;
        }
        p += len + (comma ? 1 : 0);
    }
    s_vs_launch.from_flag = true;
    fprintf(stderr, "[AUTO] --vector: mode=%s lines=%s field=%s graticule=%s plot=%s colour=%s "
            "trace=%s intensity=%d system=%s\n",
            vs_mode_name(s_vs_launch.mode), vs_lines_name(s_vs_launch.lines_view, false),
            vs_field_name(s_vs_launch.field_view), vs_grat_name(s_vs_launch.graticule),
            s_vs_launch.plot == VS_PLOT_DENSITY ? "Density" : "Points",
            vs_colour_name(s_vs_launch.colour), s_vs_launch.trace ? "On" : "Off",
            s_vs_launch.gain, s_vs_launch.system < 0 ? "Auto" : vs_system_name(s_vs_launch.system));
    return true;
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

static void vs_destroy(void *state_ptr);

static void vs_push_settings(vectorscope_panel_t *s) {
    atomic_store(&s->scope.want_mode, s->mode);
    atomic_store(&s->scope.want_lines, s->lines_view);
    atomic_store(&s->scope.want_field, s->field_view);
    atomic_store(&s->scope.want_trace, s->trace);
}

static void *vs_create(void) {
    vectorscope_panel_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->feed = gui_cvbs_feed_create();
    bool ok = s->feed && gui_vectorscope_init(&s->scope);
    for (int f = 0; f < 2; f++) {
        s->hits[f] = calloc(VS_CELLS, sizeof(uint32_t));
        s->trans[f] = calloc(VS_CELLS, sizeof(uint32_t));
        s->bh[f] = calloc(VS_PCELLS, sizeof(float));
        s->bt[f] = calloc(VS_PCELLS, sizeof(float));
        ok = ok && s->hits[f] && s->trans[f] && s->bh[f] && s->bt[f];
        s->meta[f].x0 = s->meta[f].y0 = GUI_VSCOPE_GRID;   // empty boxes
        s->meta[f].x1 = s->meta[f].y1 = -1;
    }
    s->take_hits = calloc(VS_CELLS, sizeof(uint32_t));
    s->take_trans = calloc(VS_CELLS, sizeof(uint32_t));
    s->fin = calloc(VS_PCELLS, sizeof(float));
    s->ftmp = calloc(VS_PCELLS, sizeof(float));
    s->hue_lut = malloc(VS_CELLS * 3);
    s->rgba = calloc(VS_CELLS, 4);
    s->img_x0 = s->img_y0 = s->upd_y0 = GUI_VSCOPE_GRID;
    s->img_x1 = s->img_y1 = s->upd_y1 = -1;
    if (!ok || !s->take_hits || !s->take_trans || !s->fin || !s->ftmp || !s->hue_lut || !s->rgba) {
        vs_destroy(s);
        return NULL;
    }
    cvbs_decoder_t *dec = gui_cvbs_feed_decoder(s->feed);
    dec->vscope = &s->scope;
    gui_cvbs_feed_set_pixels(s->feed, false);   // scope only: no picture pass
    atomic_init(&s->pending_system, -1);
    s->gen_seen = -1;
    int sys = (dec->state.format == CVBS_FORMAT_NTSC) ? 1
            : (dec->state.format == CVBS_FORMAT_SECAM) ? 2 : 0;
    atomic_init(&s->decoder_system, sys);
    atomic_init(&s->auto_system, s_vs_launch.system < 0);
    if (s_vs_launch.system >= 0) atomic_store(&s->pending_system, s_vs_launch.system);

    double half = 0.5 * GUI_VSCOPE_GRID;
    for (int y = 0; y < GUI_VSCOPE_GRID; y++) {
        for (int x = 0; x < GUI_VSCOPE_GRID; x++) {
            double u = ((double)x + 0.5 - half) / half * GUI_VSCOPE_FULL_SCALE;
            double v = (half - (double)y - 0.5) / half * GUI_VSCOPE_FULL_SCALE;
            vs_hue(u, v, s->hue_lut + ((size_t)y * GUI_VSCOPE_GRID + x) * 3);
        }
    }
    // Beam spot (normalised 1-D Gaussian; the 2-D spot is its outer product).
    double sum = 0.0;
    for (int t = 0; t <= VS_SPOT_R; t++) {
        s->spot[t] = (float)exp(-0.5 * (double)(t * t) / (VS_SPOT_SIGMA * VS_SPOT_SIGMA));
        sum += t == 0 ? s->spot[t] : 2.0 * s->spot[t];
    }
    for (int t = 0; t <= VS_SPOT_R; t++) s->spot[t] = (float)(s->spot[t] / sum);

    s->mode = s_vs_launch.mode;
    s->lines_view = s_vs_launch.lines_view;
    s->field_view = s_vs_launch.field_view;
    s->graticule = s_vs_launch.graticule;
    s->plot = s_vs_launch.plot;
    s->colour = s_vs_launch.colour;
    s->trace = s_vs_launch.trace;
    s->gain = s_vs_launch.gain;
    vs_push_settings(s);
    gui_vectorscope_reset(&s->scope);
    s->image_dirty = true;
    if (s_vs_launch.from_flag) {
        fprintf(stderr, "[AUTO] Vector panel: mode=%s lines=%s field=%s graticule=%s plot=%s "
                "colour=%s trace=%s intensity=%d system=%s\n",
                vs_mode_name(s->mode), vs_lines_name(s->lines_view, false), vs_field_name(s->field_view),
                vs_grat_name(s->graticule), s->plot == VS_PLOT_DENSITY ? "Density" : "Points",
                vs_colour_name(s->colour), s->trace ? "On" : "Off", s->gain,
                s_vs_launch.system < 0 ? "Auto" : vs_system_name(s_vs_launch.system));
    }
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
    for (int f = 0; f < 2; f++) {
        free(s->hits[f]);
        free(s->trans[f]);
        free(s->bh[f]);
        free(s->bt[f]);
    }
    free(s->take_hits);
    free(s->take_trans);
    free(s->fin);
    free(s->ftmp);
    free(s->hue_lut);
    free(s->rgba);
    free(s);
}

static void vs_clear(void *state_ptr) {
    vectorscope_panel_t *s = state_ptr;
    if (!s) return;
    s->have_slot[0] = s->have_slot[1] = false;
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
        s->follow_target = pending;
        s->follow_hold = GEN_FOLLOW_HOLD_FIELDS;
        s->votes = 0;
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
        if (s->follow_hold > 0) {
            // Held after a follow: release once the stream shows the
            // followed system's field rate (625-line covers SECAM).
            bool arrived = want == s->follow_target || (want == 0 && s->follow_target == 2);
            s->follow_hold = arrived ? 0 : s->follow_hold - 1;
            s->votes = 0;
        } else if (want < 0) {
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

// Manual System pick: queue it without touching the generator tracking,
// so it holds until the generator's system next changes (the Video
// panels' "user override once flipped" rule).
static void vs_queue_system(vectorscope_panel_t *s, int system_select) {
    if (s && system_select >= 0 && system_select <= 2) {
        atomic_store(&s->pending_system, system_select);
    }
}

static void vs_follow_system(void *state_ptr, int system_select) {
    vectorscope_panel_t *s = state_ptr;
    if (s && system_select >= 0 && system_select <= 2) {
        atomic_store(&s->pending_system, system_select);
        s->gen_seen = system_select;   // UI thread (Gen click / follow)
    }
}

// Test signal mode: follow the generator's system whatever changed it (the
// Gen button, a Video panel's System pick, --gen-system), like the Video
// panels' decoders. MESECAM (RF only) is SECAM on the composite side.
static void vs_follow_generator(vectorscope_panel_t *s, gui_app_t *app) {
    if (!gui_waveform_test_mode_active(app)) {
        s->gen_seen = -1;   // re-sync on the next entry into test mode
        return;
    }
    int sys = gui_testgen_clamp_system(app->settings.video_system);
    if (sys == 3) sys = 2;
    if (sys != s->gen_seen) vs_follow_system(s, sys);
}

//-----------------------------------------------------------------------------
// Rendering (render thread)
//-----------------------------------------------------------------------------

_Static_assert(VS_SPOT_R == 3, "vs_spread is written for a 7-tap spot");

// Zero a grid inside a field's box (the meta box: hits, and the transits of
// a traced field, are zero outside it).
static void vs_zero_box(uint32_t *grid, const gui_vscope_meta_t *m) {
    if (m->x1 < m->x0 || m->y1 < m->y0) return;
    for (int y = m->y0; y <= m->y1; y++) {
        memset(grid + ((size_t)y << GUI_VSCOPE_GRID_SHIFT) + m->x0, 0,
               (size_t)(m->x1 - m->x0 + 1) * sizeof(uint32_t));
    }
}

// Render thread: take each waiting field into its parity slot. No copy: the
// scope gets this panel's take buffers, zeroed in the outgoing field's box.
// Returns true when a field came in.
static bool vs_take_fields(vectorscope_panel_t *s) {
    gui_vscope_meta_t meta;
    bool fresh = false;
    while (gui_vectorscope_take_swap(&s->scope, &s->take_hits, &s->take_trans, &meta)) {
        // A field latched before the last Mode / Lines / Field click (up to
        // two are in flight: published, and being accumulated) is not this
        // view: drop it, its buffers zeroed back for the scope. Shown, an
        // Advanced field drew SECAM's unpaired axes under the Standard
        // graticule for a moment after Advanced -> Standard.
        const bool adv = s->mode == GUI_VSCOPE_MODE_ADVANCED;
        if (meta.mode != s->mode ||
            (adv && (meta.lines_view != s->lines_view || meta.field_view != s->field_view))) {
            vs_zero_box(s->take_hits, &meta);
            if (meta.trace) vs_zero_box(s->take_trans, &meta);
            continue;
        }
        const int f = meta.field ? 1 : 0;
        uint32_t *swap = s->hits[f]; s->hits[f] = s->take_hits; s->take_hits = swap;
        swap = s->trans[f]; s->trans[f] = s->take_trans; s->take_trans = swap;
        vs_zero_box(s->take_hits, &s->meta[f]);
        if (s->meta[f].trace) vs_zero_box(s->take_trans, &s->meta[f]);
        s->meta[f] = meta;
        s->have_slot[f] = true;
        // A field of another mode / window replaces the other slot too.
        const int o = 1 - f;
        if (s->have_slot[o] && (s->meta[o].mode != meta.mode ||
                                s->meta[o].lines_view != meta.lines_view ||
                                s->meta[o].field_view != meta.field_view ||
                                s->meta[o].system != meta.system)) {
            s->have_slot[o] = false;
        }
        fresh = true;
    }
    return fresh;
}

typedef struct {
    int cx0, cx1, cy0, cy1;   // Content (hits) box
    int ox0, ox1, oy0, oy1;   // ... grown by the spot radius: the spread's reach
} vs_box_t;

// Beam-spot spread of a (+ b; b may be NULL) into out (padded): separable,
// branch-free over the zero-padded work grids, zeroed again afterwards.
static void vs_spread(vectorscope_panel_t *s, const uint32_t *a, const uint32_t *b,
                      float *out, const vs_box_t *bx) {
    float *restrict fin = s->fin, *restrict tmp = s->ftmp;
    const int cw = bx->cx1 - bx->cx0 + 1, ow = bx->ox1 - bx->ox0 + 1;
    for (int y = bx->cy0; y <= bx->cy1; y++) {
        // Counts stay far below 2^31: int32 converts natively.
        const int32_t *ra = (const int32_t *)a + ((size_t)y << GUI_VSCOPE_GRID_SHIFT) + bx->cx0;
        float *d = fin + VS_PIDX(bx->cx0, y);
        if (b) {
            const int32_t *rb = (const int32_t *)b + ((size_t)y << GUI_VSCOPE_GRID_SHIFT) + bx->cx0;
            for (int x = 0; x < cw; x++) d[x] = (float)(ra[x] + rb[x]);
        } else {
            for (int x = 0; x < cw; x++) d[x] = (float)ra[x];
        }
    }
    const float w0 = s->spot[0], w1 = s->spot[1], w2 = s->spot[2], w3 = s->spot[3];
    for (int y = bx->cy0; y <= bx->cy1; y++) {
        const float *c = fin + VS_PIDX(bx->ox0, y);
        float *d = tmp + VS_PIDX(bx->ox0, y);
        for (int x = 0; x < ow; x++) {
            d[x] = w0 * c[x] + w1 * (c[x - 1] + c[x + 1]) + w2 * (c[x - 2] + c[x + 2])
                 + w3 * (c[x - 3] + c[x + 3]);
        }
    }
    for (int y = bx->oy0; y <= bx->oy1; y++) {
        const float *c = tmp + VS_PIDX(bx->ox0, y);
        const float *u1 = c - VS_P, *u2 = c - 2 * VS_P, *u3 = c - 3 * VS_P;
        const float *d1 = c + VS_P, *d2 = c + 2 * VS_P, *d3 = c + 3 * VS_P;
        float *d = out + VS_PIDX(bx->ox0, y);
        for (int x = 0; x < ow; x++) {
            d[x] = w0 * c[x] + w1 * (u1[x] + d1[x]) + w2 * (u2[x] + d2[x]) + w3 * (u3[x] + d3[x]);
        }
    }
    for (int y = bx->cy0; y <= bx->cy1; y++) {
        memset(fin + VS_PIDX(bx->cx0, y), 0, (size_t)cw * sizeof(float));
        memset(tmp + VS_PIDX(bx->ox0, y), 0, (size_t)ow * sizeof(float));
    }
}

static void vs_mark_rows(vectorscope_panel_t *s, int y0, int y1) {
    if (y1 < y0) return;
    if (y0 < s->upd_y0) s->upd_y0 = y0;
    if (y1 > s->upd_y1) s->upd_y1 = y1;
}

static void vs_dens_lut(vectorscope_panel_t *s) {
    if (s->dens_lut_gain == s->gain) return;
    const double ex = 1.0 / (double)s->gain;
    for (int i = 0; i < VS_DENS_LUT; i++) {
        const int e = i >> 7, m = i & 127;
        double x = ldexp(1.0 + ((double)m + 0.5) / 128.0, e + 103 - 127);
        if (x > 1.0) x = 1.0;
        s->dens_lut[i] = (float)pow(x, ex);
    }
    s->dens_lut_gain = s->gain;
}

// x in [0, 1] -> x^(1/gain).
static inline float vs_dens(const float *lut, float x) {
    union { float f; uint32_t u; } v = { x };
    const int e = (int)(v.u >> 23) - 103;
    if (e < 0) return 0.0f;    // below 2^-24 (and +0)
    int i = (e << 7) | (int)((v.u >> 16) & 0x7F);
    return lut[i < VS_DENS_LUT ? i : VS_DENS_LUT - 1];
}

// The dwell (both fields) of one output row: h0 + h1 into tmp, or the one
// plane present.
static inline const float *vs_dwell_row(const float *h0, const float *h1, size_t o, int ow,
                                        float *restrict tmp) {
    if (h0 && h1) {
        for (int x = 0; x < ow; x++) tmp[x] = h0[o + x] + h1[o + x];
        return tmp;
    }
    return (h0 ? h0 : h1) + o;
}

// Point-law level of one field's planes (h dwell, t transits; either may be
// NULL) over one output row into r.
static inline void vs_level_row(float *restrict r, const float *h, const float *t, size_t o,
                                int ow, float ds, float ts) {
    if (h && t) {
        for (int x = 0; x < ow; x++) r[x] = h[o + x] * ds + t[o + x] * ts;
    } else if (h) {
        for (int x = 0; x < ow; x++) r[x] = h[o + x] * ds;
    } else if (t) {
        for (int x = 0; x < ow; x++) r[x] = t[o + x] * ts;
    } else {
        for (int x = 0; x < ow; x++) r[x] = 0.0f;
    }
}

// Clear the rgba region the last build wrote.
static void vs_build_image_clear(vectorscope_panel_t *s) {
    const int G = GUI_VSCOPE_GRID;
    if (s->img_x1 < s->img_x0) return;
    for (int y = s->img_y0; y <= s->img_y1; y++) {
        memset(s->rgba + ((size_t)y * G + s->img_x0) * 4, 0,
               (size_t)(s->img_x1 - s->img_x0 + 1) * 4);
    }
    vs_mark_rows(s, s->img_y0, s->img_y1);
    s->img_x0 = s->img_y0 = G;
    s->img_x1 = s->img_y1 = -1;
}

// decode-orc's composite renderer (tbc-analyse getCompositeTraceImage) into
// rgba: returns false when there is nothing to draw.
static bool vs_build_image(vectorscope_panel_t *s) {
    const int G = GUI_VSCOPE_GRID, R = VS_SPOT_R;
    vs_build_image_clear(s);
    // Content box (from the decoder) and line count over both fields.
    vs_box_t bx = { G, -1, G, -1, 0, 0, 0, 0 };
    uint32_t lines = 0;
    int stride = 1;
    bool have[2] = { false, false };
    for (int f = 0; f < 2; f++) {
        const gui_vscope_meta_t *m = &s->meta[f];
        if (!s->have_slot[f] || m->x1 < m->x0 || m->y1 < m->y0) continue;
        have[f] = true;
        lines += m->segments;
        if (m->stride > stride) stride = m->stride;
        if (m->x0 < bx.cx0) bx.cx0 = m->x0;
        if (m->x1 > bx.cx1) bx.cx1 = m->x1;
        if (m->y0 < bx.cy0) bx.cy0 = m->y0;
        if (m->y1 > bx.cy1) bx.cy1 = m->y1;
    }
    if (bx.cx1 < bx.cx0 || lines == 0) return false;
    bx.ox0 = bx.cx0 - R < 0 ? 0 : bx.cx0 - R;
    bx.oy0 = bx.cy0 - R < 0 ? 0 : bx.cy0 - R;
    bx.ox1 = bx.cx1 + R >= G ? G - 1 : bx.cx1 + R;
    bx.oy1 = bx.cy1 + R >= G ? G - 1 : bx.cy1 + R;
    const bool tr[2] = { have[0] && s->meta[0].trace, have[1] && s->meta[1].trace };
    const bool any_tr = tr[0] || tr[1];

    // Spread: per field for Blend, else both fields through one spread (it
    // is linear).
    const bool blend = s->colour == VS_COL_BLEND;
    if (blend) {
        for (int f = 0; f < 2; f++) {
            if (!have[f]) continue;
            vs_spread(s, s->hits[f], NULL, s->bh[f], &bx);
            if (tr[f]) vs_spread(s, s->trans[f], NULL, s->bt[f], &bx);
        }
    } else {
        const int fa = have[0] ? 0 : 1;
        vs_spread(s, s->hits[fa], have[0] && have[1] ? s->hits[1] : NULL, s->bh[0], &bx);
        if (any_tr) {
            const int ta = tr[0] ? 0 : 1;
            vs_spread(s, s->trans[ta], tr[0] && tr[1] ? s->trans[1] : NULL, s->bt[0], &bx);
        }
    }
    // Per-field (Blend) / combined planes; NULL = absent.
    const float *h0 = blend ? (have[0] ? s->bh[0] : NULL) : s->bh[0];
    const float *h1 = blend ? (have[1] ? s->bh[1] : NULL) : NULL;
    const float *t0 = blend ? (tr[0] ? s->bt[0] : NULL) : (any_tr ? s->bt[0] : NULL);
    const float *t1 = blend ? (tr[1] ? s->bt[1] : NULL) : NULL;
    const int ow = bx.ox1 - bx.ox0 + 1;

    // Brightness anchor (decode-orc): the lower of 8 samples a line on one
    // cell and the half-dwell point. One pass first: when the dwell at or
    // above the 8-a-line level holds half the total (dense video), the
    // half-dwell point is above it and the histogram is not needed.
    const float gain = (float)s->gain;
    const float spot_peak = s->spot[0] * s->spot[0];
    const float a8 = 8.0f * (float)lines / (float)stride * spot_peak;
    float *r0 = s->row0, *r1 = s->row1;
    double total = 0.0, above = 0.0;
    float peak = 0.0f;
    for (int y = bx.oy0; y <= bx.oy1; y++) {
        const float *d = vs_dwell_row(h0, h1, VS_PIDX(bx.ox0, y), ow, r0);
        float rt = 0.0f, ra = 0.0f, rp = 0.0f;
        for (int x = 0; x < ow; x++) {
            rt += d[x];
            ra += d[x] >= a8 ? d[x] : 0.0f;
            rp = d[x] > rp ? d[x] : rp;
        }
        total += rt;
        above += ra;
        if (rp > peak) peak = rp;
    }
    float anchor = a8;
    if (peak > 0.0f && above < 0.5 * total) {
        enum { BUCKETS = 1024 };
        double bucket[BUCKETS];
        memset(bucket, 0, sizeof(bucket));
        const float to_bucket = (float)BUCKETS / peak;
        for (int y = bx.oy0; y <= bx.oy1; y++) {
            const float *d = vs_dwell_row(h0, h1, VS_PIDX(bx.ox0, y), ow, r0);
            for (int x = 0; x < ow; x++) {
                if (d[x] <= 0.0f) continue;
                const int b = (int)(d[x] * to_bucket);
                bucket[b < BUCKETS ? b : BUCKETS - 1] += d[x];
            }
        }
        double acc = 0.0;
        for (int b = BUCKETS - 1; b >= 0; b--) {
            acc += bucket[b];
            if (acc >= 0.5 * total) {
                const float half_level = (float)b / to_bucket;
                if (half_level < anchor) anchor = half_level;
                break;
            }
        }
    }
    if (anchor <= 0.0f) anchor = 1.0f;
    const float ds = gain / anchor;
    const float ts = gain * 0.04f * (float)stride / ((float)lines * spot_peak);

    // Density: (level / max)^(1 / gain) on the point law's weighting.
    const bool density = s->plot == VS_PLOT_DENSITY;
    float inv_dmax = 0.0f;
    if (density) {
        vs_dens_lut(s);
        float dmax = 0.0f;
        for (int y = bx.oy0; y <= bx.oy1; y++) {
            vs_level_row(r0, h0, t0, VS_PIDX(bx.ox0, y), ow, ds, ts);
            if (blend) {
                vs_level_row(r1, h1, t1, VS_PIDX(bx.ox0, y), ow, ds, ts);
                for (int x = 0; x < ow; x++) r0[x] += r1[x];
            }
            for (int x = 0; x < ow; x++) dmax = r0[x] > dmax ? r0[x] : dmax;
        }
        inv_dmax = dmax > 0.0f ? 1.0f / dmax : 0.0f;
    }

    // Colour, one packed RGBA word a pixel (R in the low byte). Over full
    // brightness the colour is scaled back to 1, keeping its hue.
    const bool multi = s->colour == VS_COL_MULTI;
    for (int y = bx.oy0; y <= bx.oy1; y++) {
        const size_t gi = (size_t)y * G + bx.ox0;
        uint32_t *restrict px = (uint32_t *)s->rgba + gi;
        vs_level_row(r0, h0, t0, VS_PIDX(bx.ox0, y), ow, ds, ts);
        if (blend) {
            vs_level_row(r1, h1, t1, VS_PIDX(bx.ox0, y), ow, ds, ts);
        }
        if (density) {
            for (int x = 0; x < ow; x++) {
                const float level = r0[x] + (blend ? r1[x] : 0.0f);
                const float shaped = level > 0.0f ? vs_dens(s->dens_lut, level * inv_dmax) : 0.0f;
                const float k = level > 0.0f ? shaped / level : 0.0f;
                r0[x] *= k;
                if (blend) r1[x] *= k;
            }
        }
        if (multi) {
            const uint8_t *hue = s->hue_lut + gi * 3;
            if (blend) {
                for (int x = 0; x < ow; x++) r0[x] += r1[x];
            }
            for (int x = 0; x < ow; x++) {
                const float k = (r0[x] < 1.0f ? r0[x] : 1.0f);
                const uint32_t r = (uint32_t)(hue[x * 3 + 0] * k + 0.5f);
                const uint32_t g = (uint32_t)(hue[x * 3 + 1] * k + 0.5f);
                const uint32_t b = (uint32_t)(hue[x * 3 + 2] * k + 0.5f);
                px[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        } else if (blend) {
            for (int x = 0; x < ow; x++) {
                const float level = r0[x] + r1[x];
                const float norm = level > 1.0f ? 255.0f / level : 255.0f;
                const uint32_t r = (uint32_t)(r0[x] * norm + 0.5f);   // field 1 yellow
                const uint32_t g = (uint32_t)(level * norm + 0.5f);
                const uint32_t b = (uint32_t)(r1[x] * norm + 0.5f);   // field 2 cyan
                px[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        } else {
            for (int x = 0; x < ow; x++) {
                const float g = r0[x] < 1.0f ? r0[x] : 1.0f;           // decode-orc green
                px[x] = ((uint32_t)(g * 255.0f + 0.5f) << 8) | 0xFF000000u;
            }
        }
    }
    s->img_x0 = bx.ox0;
    s->img_x1 = bx.ox1;
    s->img_y0 = bx.oy0;
    s->img_y1 = bx.oy1;
    vs_mark_rows(s, bx.oy0, bx.oy1);
    return true;
}

// One colour-bar target: the zone and box, or (label set) only its label -
// labels go on in a second pass so no zone covers one. The label sits just
// outside the zone (tbc's 48 px at 1024 px; at small sizes that fell inside
// the zones, and Advanced's mirrored set put them on the neighbours' zones).
static void vs_draw_target(Vector2 c, double u, double v, double px_per_unit, float radius,
                           float sc, Color col, const char *label, int label_font) {
    double mag = hypot(u, v) * px_per_unit;
    double deg = atan2(v, u) * 180.0 / M_PI;
    // Zone: +-13 degrees, +-14% radial (tbc). raylib angles run clockwise.
    float span = (float)(mag * 0.14);
    float inner = fmaxf(0.0f, (float)mag - span);
    float outer = fminf(radius, (float)mag + span);
    if (label) {
        const float at = fmaxf((float)mag + 48.0f * sc, outer + label_font * 0.75f + 2.0f);
        Vector2 lp = vs_point(c, deg, at);
        int w = gui_text_measure(label, label_font);
        gui_text_draw(label, lp.x - w / 2.0f, lp.y - label_font / 2.0f, label_font, col);
        return;
    }
    DrawRing(c, inner, outer, (float)(-deg - 13.0), (float)(-deg + 13.0), 24, vs_alpha(col, 52));
    DrawRingLines(c, inner, outer, (float)(-deg - 13.0), (float)(-deg + 13.0), 24, vs_alpha(col, 180));
    // Target box with crosshair (42 / 22 px at 1024).
    Vector2 p = vs_point(c, deg, mag);
    float box = 42.0f * sc, cross = 22.0f * sc;
    DrawRectangleLinesEx((Rectangle){ p.x - box / 2, p.y - box / 2, box, box },
                         fmaxf(1.0f, 2.0f * sc), vs_alpha(col, 220));
    DrawLineV((Vector2){ p.x - cross / 2, p.y }, (Vector2){ p.x + cross / 2, p.y }, vs_alpha(col, 220));
    DrawLineV((Vector2){ p.x, p.y - cross / 2 }, (Vector2){ p.x, p.y + cross / 2 }, vs_alpha(col, 220));
}

// SECAM Advanced target: a bar's D'B value on the U axis (axis 0) or its
// D'R value on the V axis (axis 1) - each SECAM line carries one of them.
// side: +1 label below (U) / right (V), -1 above / left; set per colour so
// neighbouring targets alternate. label set: draw only the label.
static void vs_draw_axis_target(Vector2 c, int axis, double val, double px_per_unit, float sc,
                                Color col, int side, const char *label, int label_font) {
    const float d = (float)(val * px_per_unit);
    const Vector2 p = axis == 0 ? (Vector2){ c.x + d, c.y } : (Vector2){ c.x, c.y - d };
    const float box = fmaxf(6.0f, 30.0f * sc), cross = fmaxf(4.0f, 16.0f * sc);
    if (label) {
        const int w = gui_text_measure(label, label_font);
        const float off = box * 0.5f + 3.0f;
        if (axis == 0) {
            const float y = side > 0 ? p.y + off : p.y - off - label_font;
            gui_text_draw(label, p.x - w / 2.0f, y, label_font, col);
        } else {
            const float x = side > 0 ? p.x + off : p.x - off - w;
            gui_text_draw(label, x, p.y - label_font / 2.0f, label_font, col);
        }
        return;
    }
    DrawRectangleRec((Rectangle){ p.x - box / 2, p.y - box / 2, box, box }, vs_alpha(col, 52));
    DrawRectangleLinesEx((Rectangle){ p.x - box / 2, p.y - box / 2, box, box },
                         fmaxf(1.0f, 2.0f * sc), vs_alpha(col, 220));
    DrawLineV((Vector2){ p.x - cross / 2, p.y }, (Vector2){ p.x + cross / 2, p.y }, vs_alpha(col, 220));
    DrawLineV((Vector2){ p.x, p.y - cross / 2 }, (Vector2){ p.x, p.y + cross / 2 }, vs_alpha(col, 220));
}

static void vs_draw_graticule(const vectorscope_panel_t *s, int system, bool advanced,
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

    if (s->graticule != VS_GRAT_OFF) {
        static const char *names[7] = { "", "B", "G", "Cy", "R", "Mg", "Yl" };
        static const char *lower[7] = { "", "b", "g", "cy", "r", "mg", "yl" };
        const bool both = s->graticule == VS_GRAT_BOTH;
        const double pcts[2] = { both ? 0.75 : (s->graticule == VS_GRAT_100 ? 1.0 : 0.75), 1.0 };
        const int pct_count = both ? 2 : 1;
        int label_font = (int)fmaxf(12.0f, 14.0f * sc * 1.33f);
        // SECAM Advanced: a line carries D'B or D'R, so the targets are each
        // bar's value on its own axis (labels alternate sides along each).
        const bool secam_axes = advanced && system == GUI_VSCOPE_SYS_SECAM;
        static const int u_side[7] = { 0, 1, 1, 1, -1, -1, -1 };    // B G Cy below, R Mg Yl above
        static const int v_side[7] = { 0, 1, -1, 1, -1, 1, -1 };    // B Cy Mg right, G R Yl left
        if (secam_axes) {
            const Color cap = { 200, 200, 200, 255 };
            gui_text_draw("D'B", c.x + radius - 30.0f * sc - gui_text_measure("D'B", label_font),
                          c.y + 4.0f, label_font, cap);
            gui_text_draw("D'R", c.x + 6.0f, c.y - radius + 22.0f * sc, label_font, cap);
        }
        // Pass 0: zones and boxes; pass 1: the labels over them.
        for (int pass = 0; pass < 2 && secam_axes; pass++) {
            for (int pi = 0; pi < pct_count; pi++) {
                if (pass == 1 && both && pi != pct_count - 1) continue;
                for (int rgb = 1; rgb < 7; rgb++) {
                    double u, v;
                    gui_vectorscope_bar_target(system, rgb, pcts[pi], &u, &v);
                    const Color col = vs_target_color(rgb);
                    vs_draw_axis_target(c, 0, u, px_per_unit, sc, col, u_side[rgb],
                                        pass ? names[rgb] : NULL, label_font);
                    vs_draw_axis_target(c, 1, v, px_per_unit, sc, col, v_side[rgb],
                                        pass ? names[rgb] : NULL, label_font);
                }
            }
        }
        for (int pass = 0; pass < 2 && !secam_axes; pass++) {
            for (int pi = 0; pi < pct_count; pi++) {
                // Both: label each colour once, at the outer (100%) target.
                if (pass == 1 && both && pi != pct_count - 1) continue;
                for (int rgb = 1; rgb < 7; rgb++) {
                    double u, v;
                    gui_vectorscope_bar_target(system, rgb, pcts[pi], &u, &v);
                    Color col = vs_target_color(rgb);
                    vs_draw_target(c, u, v, px_per_unit, radius, sc, col,
                                   pass ? names[rgb] : NULL, label_font);
                    // Advanced (composite): the -V lines' set, mirrored about
                    // U, lower-case labels (tbc / decode-orc; drawn for NTSC
                    // too as a reference, tbc Phase 22).
                    if (advanced) {
                        vs_draw_target(c, u, -v, px_per_unit, radius, sc, vs_alpha(col, 200),
                                       pass ? lower[rgb] : NULL, label_font);
                    }
                }
            }
        }
    }

    // Burst target(s), independent of the bar level: NTSC 20 IRE at 180 deg;
    // PAL 3/14 of blanking..white at 135 deg - Standard is V-switched like
    // the picture (one phase), Advanced shows both (135 / 225 deg, the +V /
    // -V lines). SECAM has no burst (FM chroma).
    if (system != GUI_VSCOPE_SYS_SECAM) {
        const bool pal = system == GUI_VSCOPE_SYS_PAL;
        const double amp = pal ? 3.0 / 14.0 : 0.2;
        const double degs[2] = { pal ? 135.0 : 180.0, 225.0 };
        const char *labels[2] = { (advanced && pal) ? "Burst" : "burst", "burst" };
        const int count = (pal && advanced) ? 2 : 1;
        const Color bc = { 230, 230, 230, 255 };
        int label_font = (int)fmaxf(12.0f, 14.0f * sc * 1.33f);
        float mag = (float)(amp * px_per_unit);
        float tick = 16.0f * sc;
        float thick = fmaxf(1.0f, 2.0f * sc);
        for (int i = 0; i < count; i++) {
            double deg = degs[i];
            DrawLineEx(vs_point(c, deg, mag - tick), vs_point(c, deg, mag + tick),
                       thick, vs_alpha(bc, 220));
            Vector2 p = vs_point(c, deg, mag);
            Vector2 a = vs_point(c, deg + 90.0, tick * 0.6f);
            Vector2 b = vs_point(c, deg - 90.0, tick * 0.6f);
            DrawLineEx((Vector2){ p.x + a.x - c.x, p.y + a.y - c.y },
                       (Vector2){ p.x + b.x - c.x, p.y + b.y - c.y }, thick, vs_alpha(bc, 220));
            DrawCircleLinesV(p, 5.0f * sc + 2.0f, vs_alpha(bc, 160));
            // PAL: outward along the burst vector (clear of the targets at
            // 135 / 225 deg). NTSC's 180 deg burst points at Yl: below it.
            Vector2 lp = pal ? vs_point(c, deg, mag + tick + label_font * 1.2f)
                             : (Vector2){ p.x, p.y + tick * 0.6f + label_font * 0.9f };
            int w = gui_text_measure(labels[i], label_font);
            gui_text_draw(labels[i], lp.x - w / 2.0f, lp.y - label_font / 2.0f, label_font,
                          vs_alpha(bc, 200));
        }
    }
}

static void vs_render(void *state_ptr, gui_app_t *app, int channel,
                      Rectangle bounds, Color channel_color) {
    (void)channel;
    (void)channel_color;
    vectorscope_panel_t *s = state_ptr;
    DrawRectangleRec(bounds, COLOR_METER_BG);
    if (!s) return;
    vs_follow_generator(s, app);

    int system = atomic_load(&s->decoder_system) == 1 ? GUI_VSCOPE_SYS_NTSC
               : atomic_load(&s->decoder_system) == 2 ? GUI_VSCOPE_SYS_SECAM
               : GUI_VSCOPE_SYS_PAL;
    double now = GetTime();
    if (vs_take_fields(s)) {
        s->have_data = true;
        s->last_field_time = now;
        s->image_dirty = true;
    } else if (s->have_data && now - s->last_field_time > 0.5) {
        vs_clear(s);      // feed stopped / lock lost: blank the trace
    }

    if (!s->tex_valid) {
        Image img = { .data = s->rgba, .width = GUI_VSCOPE_GRID, .height = GUI_VSCOPE_GRID,
                      .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        s->tex = LoadTextureFromImage(img);
        s->tex_valid = s->tex.id != 0;
        if (s->tex_valid) SetTextureFilter(s->tex, TEXTURE_FILTER_BILINEAR);
        s->image_dirty = true;
        s->upd_y0 = GUI_VSCOPE_GRID;   // the texture holds rgba as it is
        s->upd_y1 = -1;
    }
    // Square scope, centred, with a margin for the target labels.
    float side = fminf(bounds.width, bounds.height) - 16.0f;
    if (side < 32.0f) return;
    bool drawn = s->have_data;
    if (s->tex_valid && s->image_dirty) {
        drawn = s->have_data && vs_build_image(s);
        if (!s->have_data) vs_build_image_clear(s);
        // Upload only the rows that changed (rgba rows are contiguous).
        if (s->upd_y1 >= s->upd_y0) {
            UpdateTextureRec(s->tex, (Rectangle){ 0.0f, (float)s->upd_y0, (float)GUI_VSCOPE_GRID,
                                                  (float)(s->upd_y1 - s->upd_y0 + 1) },
                             s->rgba + (size_t)s->upd_y0 * GUI_VSCOPE_GRID * 4);
            s->upd_y0 = GUI_VSCOPE_GRID;
            s->upd_y1 = -1;
        }
        s->image_dirty = false;
    }
    Vector2 centre = { bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f };
    Rectangle dst = { centre.x - side * 0.5f, centre.y - side * 0.5f, side, side };
    const bool advanced = s->mode == GUI_VSCOPE_MODE_ADVANCED;
    // Graticule underneath, the trace ADDED over it (decode-orc).
    DrawRectangleRec(dst, BLACK);
    vs_draw_graticule(s, system, advanced, centre, side);
    if (s->tex_valid && drawn) {
        BeginBlendMode(BLEND_ADDITIVE);
        DrawTexturePro(s->tex, (Rectangle){ 0, 0, GUI_VSCOPE_GRID, GUI_VSCOPE_GRID },
                       dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        EndBlendMode();
    }

    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
    gui_text_draw("Vectorscope", bounds.x + 8, bounds.y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);

    // Bottom-left: the Advanced readouts (decode-orc's instrument set), or
    // the state message.
    const int rf = FONT_SIZE_DROPDOWN_OPT;
    char text[160];
    const char *msg = NULL;
    const gui_vscope_meta_t *m = s->have_slot[1] && (!s->have_slot[0] ||
                                 s->meta[1].points >= s->meta[0].points) ? &s->meta[1]
                               : s->have_slot[0] ? &s->meta[0] : NULL;
    if (!s->have_data || !m || m->points == 0) {
        msg = (system == GUI_VSCOPE_SYS_SECAM) ? "No SECAM colour" : "No burst-locked colour";
    } else if (system == GUI_VSCOPE_SYS_SECAM && !advanced) {
        msg = "SECAM: D'B/D'R as U/V (no burst reference)";
    }
    float ty = bounds.y + bounds.height - 8.0f - (float)rf;
    if (msg) {
        gui_text_draw(msg, bounds.x + 8.0f, ty, rf, COLOR_TEXT_DIM);
    } else if (advanced && m && system == GUI_VSCOPE_SYS_SECAM) {
        // SECAM Advanced: the back-porch rest carriers (no burst).
        if (!m->meas.valid || !m->meas.secam) {
            gui_text_draw("No porch carrier measured", bounds.x + 8.0f, ty, rf, COLOR_TEXT_DIM);
        } else {
            const gui_vscope_meas_t *q = &m->meas;
            const double eb = fabs(q->rest_err_khz[0]) < 0.005 ? 0.0 : q->rest_err_khz[0];
            const double er = fabs(q->rest_err_khz[1]) < 0.005 ? 0.0 : q->rest_err_khz[1];
            snprintf(text, sizeof(text), "Rest jitter %.2f kHz   Ident errors %u   %u lines",
                     q->rest_jitter_khz, q->ident_errors, q->burst_lines);
            gui_text_draw(text, bounds.x + 8.0f, ty, rf, COLOR_TEXT);
            snprintf(text, sizeof(text), "Porch D'B %.0f mV (%.0f%%)   D'R %.0f mV (%.0f%%)",
                     q->porch_mv[0], q->porch_percent[0], q->porch_mv[1], q->porch_percent[1]);
            gui_text_draw(text, bounds.x + 8.0f, ty - (float)rf - 4.0f, rf, COLOR_TEXT);
            snprintf(text, sizeof(text), "Rest D'B %+.2f kHz   D'R %+.2f kHz", eb, er);
            gui_text_draw(text, bounds.x + 8.0f, ty - 2.0f * ((float)rf + 4.0f), rf, COLOR_TEXT);
        }
    } else if (advanced && m) {
        if (!m->meas.valid) {
            gui_text_draw("No burst reference recovered", bounds.x + 8.0f, ty, rf, COLOR_TEXT_DIM);
        } else {
            const bool pal = system == GUI_VSCOPE_SYS_PAL;
            if (pal) {
                // Snap what prints as zero (no "-0.00").
                const double split = fabs(m->meas.vsplit_deg) < 0.005 ? 0.0 : m->meas.vsplit_deg;
                snprintf(text, sizeof(text), "Jitter %.2f deg   V split %+.2f deg   %u lines",
                         m->meas.jitter_deg, split, m->meas.burst_lines);
            } else {
                snprintf(text, sizeof(text), "Jitter %.2f deg   %u lines",
                         m->meas.jitter_deg, m->meas.burst_lines);
            }
            gui_text_draw(text, bounds.x + 8.0f, ty, rf, COLOR_TEXT);
            snprintf(text, sizeof(text), "Burst %.1f IRE (%.0f%%)   Chroma/burst %.2f",
                     m->meas.burst_ire, m->meas.burst_percent, m->meas.chroma_burst);
            gui_text_draw(text, bounds.x + 8.0f, ty - (float)rf - 4.0f, rf, COLOR_TEXT);
        }
    }
}

static const char *vs_button_label(const vectorscope_panel_t *s, int b, char *buf, size_t n) {
    switch (b) {
        case VS_BTN_SYSTEM: {
            int sys = atomic_load(&((vectorscope_panel_t *)s)->decoder_system);
            const char *name = vs_system_name(sys);
            if (atomic_load(&((vectorscope_panel_t *)s)->auto_system))
                snprintf(buf, n, "System: Auto (%s)", name);
            else
                snprintf(buf, n, "System: %s", name);
            break;
        }
        case VS_BTN_MODE: snprintf(buf, n, "Mode: %s", vs_mode_name(s->mode)); break;
        case VS_BTN_LINES:
            snprintf(buf, n, "Lines: %s",
                     vs_lines_name(s->lines_view,
                                   atomic_load(&((vectorscope_panel_t *)s)->decoder_system) == 2));
            break;
        case VS_BTN_FIELD: snprintf(buf, n, "Field: %s", vs_field_name(s->field_view)); break;
        case VS_BTN_GRAT: snprintf(buf, n, "Graticule: %s", vs_grat_name(s->graticule)); break;
        case VS_BTN_PLOT: snprintf(buf, n, "Plot: %s", s->plot == VS_PLOT_DENSITY ? "Density" : "Points"); break;
        case VS_BTN_COLOUR: snprintf(buf, n, "Colour: %s", vs_colour_name(s->colour)); break;
        case VS_BTN_TRACE: snprintf(buf, n, "Trace: %s", s->trace ? "On" : "Off"); break;
        default: snprintf(buf, n, "Intensity: %d", s->gain); break;
    }
    return buf;
}

// Lines / Field apply to the Advanced mode only.
static bool vs_button_shown(const vectorscope_panel_t *s, int b) {
    if (b == VS_BTN_LINES || b == VS_BTN_FIELD) return s->mode == GUI_VSCOPE_MODE_ADVANCED;
    return true;
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
        if (!vs_button_shown(s, b)) {
            s->btn[b] = (Rectangle){ 0 };
            continue;
        }
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
            if (s->btn[b].width <= 0.0f || !CheckCollisionPointRec(click, s->btn[b])) continue;
            switch (b) {
                case VS_BTN_SYSTEM: {
                    // Auto -> PAL -> NTSC -> SECAM -> Auto.
                    if (atomic_load(&s->auto_system)) {
                        atomic_store(&s->auto_system, false);
                        vs_queue_system(s, 0);
                    } else if (atomic_load(&s->decoder_system) >= 2) {
                        atomic_store(&s->auto_system, true);
                    } else {
                        vs_queue_system(s, atomic_load(&s->decoder_system) + 1);
                    }
                    break;
                }
                case VS_BTN_MODE:
                    s->mode = s->mode == GUI_VSCOPE_MODE_ADVANCED ? GUI_VSCOPE_MODE_STANDARD
                                                                  : GUI_VSCOPE_MODE_ADVANCED;
                    vs_clear(s);
                    break;
                case VS_BTN_LINES: s->lines_view = (s->lines_view + 1) % 3; break;
                case VS_BTN_FIELD: s->field_view = s->field_view == GUI_VSCOPE_FIELD_WHOLE
                                                   ? GUI_VSCOPE_FIELD_ACTIVE : GUI_VSCOPE_FIELD_WHOLE; break;
                case VS_BTN_GRAT: s->graticule = (s->graticule + 1) % VS_GRAT_COUNT; break;
                case VS_BTN_PLOT: s->plot = (s->plot + 1) % VS_PLOT_COUNT; break;
                case VS_BTN_COLOUR: s->colour = (s->colour + 1) % VS_COL_COUNT; break;
                case VS_BTN_TRACE: s->trace = !s->trace; break;
                default: s->gain = s->gain >= 10 ? 1 : s->gain + 1; break;
            }
            vs_push_settings(s);
            s->image_dirty = true;
            gui_chrome_hide_reset(&s->chrome_hide);
            return true;
        }
    }
    // Double-click elsewhere toggles the overlay chrome (shared behaviour).
    return gui_chrome_hide_feed_click(&s->chrome_hide, click);
}

// Mouse wheel over the panel: intensity (one step per scroll event, the
// sign only, as the Waveform panel's zoom; 1-10).
static bool vs_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
    vectorscope_panel_t *s = state_ptr;
    if (!s || delta == 0.0f) return false;
    if (!CheckCollisionPointRec(gui_ui_get_mouse_position(), bounds)) return false;
    int g = s->gain + (delta > 0.0f ? 1 : -1);
    if (g < 1) g = 1;
    if (g > 10) g = 10;
    s->gain = g;
    s->image_dirty = true;
    return true;
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
    .handle_scroll = vs_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_vectorscope_panel_register(void) {
    panel_register(PANEL_VIEW_VECTORSCOPE, &s_vectorscope_vtable);
    gui_cvbs_set_vectorscope_follow(vs_follow_system);
}
