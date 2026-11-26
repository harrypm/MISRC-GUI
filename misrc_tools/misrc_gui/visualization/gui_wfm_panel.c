/*
 * MISRC GUI - Composite Waveform Monitor Panel
 * See gui_wfm_panel.h and signal/gui_wfm.h.
 *
 * Rendering ported from tbc-tools tbc-analyse waveformmonitorwidget.cpp /
 * waveformmonitordialog.cpp (GPL-3.0-or-later; itself from decode-orc):
 * area-max raster brightness with the (count x 5 x gain + 64) / 255 knee,
 * phosphor mode (green on black), Y+C / Y channel, Active video / Whole
 * frame range, intensity 0.1-10x (default 3.0x), IRE / mV axis with major
 * ticks every 20 IRE / 100 mV, a 1-2-5 time axis (~7 divisions) and the
 * Sync tip / Blanking / Black / White / Peak level markers with tbc's dash
 * styles and alphas. Times are us after 0H (the H-sync leading edge).
 */
#include "gui_wfm_panel.h"
#include "../signal/gui_cvbs.h"
#include "../signal/gui_wfm.h"
#include "../signal/gui_rf_scopes.h"   // RF Preview feed
#include "../signal/gui_vectorscope.h"   // gui_vectorscope_field_period_system
#include "../core/gui_app.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_chrome_hide.h"
#include "gui_text.h"
#include "gui_oscilloscope.h"   // gui_waveform_test_mode_active
#include "../signal/gui_testgen.h"  // gui_testgen_clamp_system
#include "../input/gui_playback.h"   // paused hold / step-back display hold
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test Gen follow hold: field measurements (~5 s) before Auto may vote
// against a followed system that never arrives.
#define GEN_FOLLOW_HOLD_FIELDS 250

#define WF_CELLS ((size_t)GUI_WFM_COLS * GUI_WFM_BINS)
#define WF_MAX_W 4096
#define WF_MAX_H 2048
#define WF_FONT_AXIS FONT_SIZE_OSC_SCALE     // tick labels, axis titles
#define WF_FONT_MARK FONT_SIZE_VU_CLIP       // level marker labels

enum { WF_BTN_SYSTEM = 0, WF_BTN_CHANNEL, WF_BTN_RANGE, WF_BTN_UNITS, WF_BTN_PHOSPHOR,
       WF_BTN_GAIN, WF_BTN_COUNT };

typedef struct {
    // Display thread: the CVBS plugin decode chain + the raster it feeds.
    gui_cvbs_feed_t *feed;
    gui_wfm_t wfm;
    // Raster source: &wfm (this panel's composite decode) or the channel's
    // RF Preview feed (gui_rf_scopes.c) while a Video panel on the same
    // channel runs the RF Preview; rf_fed skips this panel's own decode.
    gui_wfm_t *src;
    atomic_bool rf_fed;
    atomic_int pending_system;     // -1 none, else 0=PAL 1=NTSC 2=SECAM
    atomic_int decoder_system;     // The decoder's current pick (for the UI)
    atomic_bool auto_system;       // Auto: 50 Hz fields -> PAL, 59.94 -> NTSC
    uint32_t seen_measures;
    int vote_system, votes;
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

    // Render thread.
    uint16_t *grid;                // Latest frame raster (row-major by bin)
    gui_wfm_meta_t meta;
    bool have_data;
    double last_frame_time;
    uint16_t *xmax;                // [bin * w + px]: max over px's columns
    size_t xmax_cap;
    bool bin_any[GUI_WFM_BINS];    // Raster row holds any count
    uint16_t pxmax[WF_MAX_W];      // One image row's max counts
    int px_x0[WF_MAX_W], px_x1[WF_MAX_W];
    int row_lo[WF_MAX_H], row_hi[WF_MAX_H];
    uint32_t lut[1024];            // Count -> packed RGBA (brightness knee)
    uint8_t *rgba;
    size_t rgba_cap;
    Texture2D tex;
    bool tex_valid;
    bool image_dirty;
    Color built_trace;

    // Options (tbc defaults: Y, Active video, IRE, 3.0x, phosphor off).
    int channel;                   // GUI_WFM_CH_* (Y+C / Y / C)
    int range;
    bool mv;
    bool phosphor;
    float gain;

    Rectangle btn[WF_BTN_COUNT];
    gui_chrome_hide_t chrome_hide;
    bool pause_refreshed;          // Paused with no frame: re-cue requested once
    bool refeed;                   // A decode setting changed: re-cue while paused
} wfm_panel_t;

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

static void wf_destroy(void *state_ptr);

// Launch defaults for new WFM panels (tbc's defaults unless --wfm set them;
// hard rule dev/gui_automation_flags_rules.md).
static struct {
    int channel;     // GUI_WFM_CH_*
    int range;
    bool mv;
    bool phosphor;
    float gain;
    int system;      // -1 Auto, else 0=PAL 1=NTSC 2=SECAM
    bool from_flag;  // --wfm given: log each panel's applied state
} s_wf_launch = { GUI_WFM_CH_Y, GUI_WFM_RANGE_ACTIVE, false, false, 3.0f, -1, false };

static const char *wf_channel_name(int channel) {
    return channel == GUI_WFM_CH_C ? "C" : channel == GUI_WFM_CH_Y ? "Y" : "Y+C";
}

static bool wf_flag_value(const char *key, const char *val, size_t len) {
    #define WF_IS(lit) (len == sizeof(lit) - 1 && strncmp(val, lit, len) == 0)
    if (strcmp(key, "channel") == 0) {
        if (WF_IS("y")) { s_wf_launch.channel = GUI_WFM_CH_Y; return true; }
        if (WF_IS("yc")) { s_wf_launch.channel = GUI_WFM_CH_YC; return true; }
        if (WF_IS("c")) { s_wf_launch.channel = GUI_WFM_CH_C; return true; }
    } else if (strcmp(key, "range") == 0) {
        if (WF_IS("active")) { s_wf_launch.range = GUI_WFM_RANGE_ACTIVE; return true; }
        if (WF_IS("frame")) { s_wf_launch.range = GUI_WFM_RANGE_FRAME; return true; }
    } else if (strcmp(key, "units") == 0) {
        if (WF_IS("ire")) { s_wf_launch.mv = false; return true; }
        if (WF_IS("mv")) { s_wf_launch.mv = true; return true; }
    } else if (strcmp(key, "phosphor") == 0) {
        if (WF_IS("on")) { s_wf_launch.phosphor = true; return true; }
        if (WF_IS("off")) { s_wf_launch.phosphor = false; return true; }
    } else if (strcmp(key, "system") == 0) {
        if (WF_IS("auto")) { s_wf_launch.system = -1; return true; }
        if (WF_IS("pal")) { s_wf_launch.system = 0; return true; }
        if (WF_IS("ntsc")) { s_wf_launch.system = 1; return true; }
        if (WF_IS("secam")) { s_wf_launch.system = 2; return true; }
    } else if (strcmp(key, "intensity") == 0) {
        char buf[16];
        if (len == 0 || len >= sizeof(buf)) return false;
        memcpy(buf, val, len);
        buf[len] = 0;
        char *end = NULL;
        double g = strtod(buf, &end);
        if (end && *end == 0 && g >= 0.1 && g <= 10.0) {
            s_wf_launch.gain = roundf((float)g * 10.0f) / 10.0f;
            return true;
        }
    }
    #undef WF_IS
    return false;
}

bool gui_wfm_panel_apply_launch_arg(const char *arg) {
    static const char *usage =
        "--wfm: expected comma-separated key=value pairs: channel=y|yc|c, range=active|frame, "
        "units=ire|mv, phosphor=on|off, intensity=0.1..10, system=auto|pal|ntsc|secam";
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
        if (!wf_flag_value(key, eq + 1, len - klen - 1)) {
            fprintf(stderr, "%s (got '%s')\n", usage, arg);
            return false;
        }
        p += len + (comma ? 1 : 0);
    }
    s_wf_launch.from_flag = true;
    static const char *systems[] = { "PAL", "NTSC", "SECAM" };
    fprintf(stderr, "[AUTO] --wfm: channel=%s range=%s units=%s phosphor=%s intensity=%.1fx system=%s\n",
            wf_channel_name(s_wf_launch.channel),
            s_wf_launch.range == GUI_WFM_RANGE_FRAME ? "Whole frame" : "Active",
            s_wf_launch.mv ? "mV" : "IRE", s_wf_launch.phosphor ? "On" : "Off", s_wf_launch.gain,
            s_wf_launch.system < 0 ? "Auto" : systems[s_wf_launch.system]);
    return true;
}

static void *wf_create(void) {
    wfm_panel_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->feed = gui_cvbs_feed_create();
    bool ok = s->feed && gui_wfm_init(&s->wfm);
    s->grid = calloc(WF_CELLS, sizeof(uint16_t));
    if (!ok || !s->grid) {
        wf_destroy(s);
        return NULL;
    }
    cvbs_decoder_t *dec = gui_cvbs_feed_decoder(s->feed);
    dec->wfm = &s->wfm;
    s->src = &s->wfm;
    gui_cvbs_feed_set_pixels(s->feed, false);   // monitor only: no picture pass
    atomic_init(&s->pending_system, -1);
    s->gen_seen = -1;
    int sys = (dec->state.format == CVBS_FORMAT_NTSC) ? 1
            : (dec->state.format == CVBS_FORMAT_SECAM) ? 2 : 0;
    atomic_init(&s->decoder_system, sys);
    atomic_init(&s->auto_system, s_wf_launch.system < 0);
    if (s_wf_launch.system >= 0) atomic_store(&s->pending_system, s_wf_launch.system);

    s->channel = s_wf_launch.channel;
    s->range = s_wf_launch.range;
    s->mv = s_wf_launch.mv;
    s->phosphor = s_wf_launch.phosphor;
    s->gain = s_wf_launch.gain;
    atomic_store(&s->wfm.want_channel, s->channel);
    atomic_store(&s->wfm.want_range, s->range);
    s->image_dirty = true;
    if (s_wf_launch.from_flag) {
        fprintf(stderr, "[AUTO] WFM panel: channel=%s range=%s units=%s phosphor=%s intensity=%.1fx system=%s\n",
                wf_channel_name(s->channel), s->range == GUI_WFM_RANGE_FRAME ? "Whole frame" : "Active",
                s->mv ? "mV" : "IRE", s->phosphor ? "On" : "Off", s->gain,
                s_wf_launch.system < 0 ? "Auto" : s_wf_launch.system == 1 ? "NTSC"
                : s_wf_launch.system == 2 ? "SECAM" : "PAL");
    }
    return s;
}

static void wf_destroy(void *state_ptr) {
    wfm_panel_t *s = state_ptr;
    if (!s) return;
    if (s->feed) {
        gui_cvbs_feed_decoder(s->feed)->wfm = NULL;
        gui_cvbs_feed_destroy(s->feed);
    }
    gui_wfm_free(&s->wfm);
    if (s->tex_valid) UnloadTexture(s->tex);
    free(s->grid);
    free(s->xmax);
    free(s->rgba);
    free(s);
}

static void wf_clear(void *state_ptr) {
    wfm_panel_t *s = state_ptr;
    if (!s) return;
    s->have_data = false;
    s->image_dirty = true;
}

//-----------------------------------------------------------------------------
// Processing (display thread)
//-----------------------------------------------------------------------------

static void wf_process(void *state_ptr, const int16_t *samples, size_t count,
                       uint32_t sample_rate) {
    wfm_panel_t *s = state_ptr;
    if (!s || !s->feed || !samples || count == 0) return;
    if (atomic_load(&s->rf_fed)) return;   // fed by the RF Preview's decoder
    int pending = atomic_exchange(&s->pending_system, -1);
    if (pending >= 0 && pending <= 2) {
        s->follow_target = pending;
        s->follow_hold = GEN_FOLLOW_HOLD_FIELDS;
        s->votes = 0;
        gui_cvbs_feed_set_system(s->feed, pending);
        gui_wfm_reset(&s->wfm);
        atomic_store(&s->decoder_system, pending);
    }
    gui_cvbs_feed_process(s->feed, samples, count, sample_rate);

    // Auto system, as the Vector panel: the field period is measured
    // standard-independently; 3 consecutive fields of the other rate switch
    // PAL <-> NTSC (a 625-line source stays SECAM if SECAM was picked).
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
            // Unclassifiable interval: no vote.
        } else if (match) {
            s->votes = 0;
        } else if (want == s->vote_system && ++s->votes >= 3) {
            s->votes = 0;
            gui_cvbs_feed_set_system(s->feed, want);
            gui_wfm_reset(&s->wfm);
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
static void wf_queue_system(wfm_panel_t *s, int system_select) {
    if (s && system_select >= 0 && system_select <= 2) {
        atomic_store(&s->pending_system, system_select);
    }
}

static void wf_follow_system(void *state_ptr, int system_select) {
    wfm_panel_t *s = state_ptr;
    if (s && system_select >= 0 && system_select <= 2) {
        atomic_store(&s->pending_system, system_select);
        s->gen_seen = system_select;   // UI thread (Gen click / follow)
    }
}

// Test signal mode: follow the generator's system whatever changed it (the
// Gen button, a Video panel's System pick, --gen-system), like the Video
// panels' decoders. MESECAM (RF only) is SECAM on the composite side.
static void wf_follow_generator(wfm_panel_t *s, gui_app_t *app) {
    if (!gui_waveform_test_mode_active(app)) {
        s->gen_seen = -1;   // re-sync on the next entry into test mode
        return;
    }
    int sys = gui_testgen_clamp_system(app->settings.video_system);
    if (sys == 3) sys = 2;
    if (sys != s->gen_seen) wf_follow_system(s, sys);
}

//-----------------------------------------------------------------------------
// Rendering (render thread)
//-----------------------------------------------------------------------------

static Color wf_mix(Color a, Color b, float t) {
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                    (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}

static Color wf_back(const wfm_panel_t *s) { return s->phosphor ? (Color){ 0, 0, 0, 255 } : COLOR_METER_BG; }
static Color wf_axis(const wfm_panel_t *s) { return s->phosphor ? WHITE : COLOR_TEXT; }
static Color wf_grid(const wfm_panel_t *s) {
    return s->phosphor ? (Color){ 55, 55, 55, 255 } : (Color){ 60, 60, 74, 255 };
}

// tbc rebuildImage: each image pixel takes the MAX count of the raster
// cells it covers (column range x row range), brightness
// min(1, (max * 5 * gain + 64) / 255), blended background -> trace.
// Empty pixels stay transparent (the grid shows through).
// Two sequential passes over the row-major raster: (1) per amplitude bin,
// each pixel column's max over its raster columns (empty bins skipped);
// (2) per image row, the max over its bins, written out as one RGBA row.
static bool wf_build_image(wfm_panel_t *s, int w, int h, Color trace) {
    const int cols = s->meta.cols;
    const int bins = GUI_WFM_BINS;
    const size_t need = (size_t)bins * (size_t)w;
    if (need > s->xmax_cap) {
        uint16_t *n = realloc(s->xmax, need * sizeof(uint16_t));
        if (!n) return false;
        s->xmax = n;
        s->xmax_cap = need;
    }
    // Count -> colour (tbc's knee), saturating well inside the table:
    // full brightness at (255 - 64) / (5 x 0.1) = 382 counts.
    const float k = 5.0f * s->gain;
    const Color back = wf_back(s);
    for (int m = 0; m < 1024; m++) {
        float br = ((float)m * k + 64.0f) / 255.0f;
        if (br > 1.0f) br = 1.0f;
        Color c = wf_mix(back, trace, br);
        s->lut[m] = (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | 0xFF000000u;
    }
    for (int px = 0; px < w; px++) {
        int x0 = (int)((double)px / w * cols);
        int x1 = (int)((double)(px + 1) / w * cols);
        if (x1 > cols - 1) x1 = cols - 1;
        if (x0 > x1) x0 = x1;
        s->px_x0[px] = x0;
        s->px_x1[px] = x1;
    }
    for (int py = 0; py < h; py++) {
        int lo = (int)((double)(h - py - 1) / h * bins);
        int hi = (int)((double)(h - py) / h * bins);
        if (hi > bins - 1) hi = bins - 1;
        s->row_lo[py] = lo;
        s->row_hi[py] = hi;
    }

    // Pass 1.
    for (int b = 0; b < bins; b++) {
        const uint16_t *row = s->grid + (size_t)b * GUI_WFM_COLS;
        int first = 0, last = cols - 1;
        while (first < cols && !row[first]) first++;
        s->bin_any[b] = first < cols;
        if (!s->bin_any[b]) continue;
        while (last > first && !row[last]) last--;
        uint16_t *out = s->xmax + (size_t)b * w;
        memset(out, 0, (size_t)w * sizeof(uint16_t));
        int p0 = (int)((double)first * w / cols) - 1;
        int p1 = (int)((double)(last + 1) * w / cols) + 1;
        if (p0 < 0) p0 = 0;
        if (p1 > w - 1) p1 = w - 1;
        for (int px = p0; px <= p1; px++) {
            uint16_t m = row[s->px_x0[px]];
            for (int xi = s->px_x0[px] + 1; xi <= s->px_x1[px]; xi++) if (row[xi] > m) m = row[xi];
            out[px] = m;
        }
    }

    // Pass 2.
    for (int py = 0; py < h; py++) {
        uint32_t *dst = (uint32_t *)(void *)(s->rgba + (size_t)py * w * 4);
        bool any = false;
        for (int b = s->row_lo[py]; b <= s->row_hi[py]; b++) {
            if (!s->bin_any[b]) continue;
            const uint16_t *src = s->xmax + (size_t)b * w;
            if (!any) {
                memcpy(s->pxmax, src, (size_t)w * sizeof(uint16_t));
                any = true;
            } else {
                for (int px = 0; px < w; px++) if (src[px] > s->pxmax[px]) s->pxmax[px] = src[px];
            }
        }
        if (!any) {
            memset(dst, 0, (size_t)w * 4);
            continue;
        }
        for (int px = 0; px < w; px++) {
            uint16_t m = s->pxmax[px];
            dst[px] = m ? s->lut[m < 1024 ? m : 1023] : 0u;
        }
    }
    return true;
}

static float wf_ire_to_y(const gui_wfm_meta_t *m, Rectangle pa, double ire) {
    double t = (ire - m->lo_ire) / (m->hi_ire - m->lo_ire);
    return (float)(pa.y + pa.height - t * pa.height);
}

// raylib has no dashed pen: segments in the given on/off pattern (px).
static void wf_dashed_hline(float x0, float x1, float y, const float *pat, int npat, Color c) {
    float x = x0;
    int i = 0;
    while (x < x1) {
        float len = pat[i % npat];
        if ((i % 2) == 0) {
            float e = fminf(x + len, x1);
            DrawLineV((Vector2){ x, y + 0.5f }, (Vector2){ e, y + 0.5f }, c);
        }
        x += len;
        i++;
    }
}

// 1-2-5 tick step giving about `divs` divisions over `span`.
static double wf_nice_step(double span, double divs) {
    double raw = span / divs;
    double mag = pow(10.0, floor(log10(raw)));
    if (raw / mag >= 5.0) return mag * 5.0;
    if (raw / mag >= 2.0) return mag * 2.0;
    return mag;
}

static void wf_format_amp(const wfm_panel_t *s, double ire, double mv_per_ire, char *buf, size_t n) {
    if (s->mv) snprintf(buf, n, "%.1f mV", ire * mv_per_ire);
    else snprintf(buf, n, "%.1f IRE", ire);
}

static const char *wf_button_label(const wfm_panel_t *s, int b, char *buf, size_t n) {
    static const char *systems[] = { "PAL", "NTSC", "SECAM" };
    switch (b) {
        case WF_BTN_SYSTEM: {
            int sys = atomic_load(&((wfm_panel_t *)s)->decoder_system);
            const char *name = systems[(sys >= 0 && sys <= 2) ? sys : 0];
            if (atomic_load(&((wfm_panel_t *)s)->rf_fed))
                snprintf(buf, n, "System: RF (%s)", name);
            else if (atomic_load(&((wfm_panel_t *)s)->auto_system))
                snprintf(buf, n, "System: Auto (%s)", name);
            else
                snprintf(buf, n, "System: %s", name);
            break;
        }
        case WF_BTN_CHANNEL: snprintf(buf, n, "Channel: %s", wf_channel_name(s->channel)); break;
        case WF_BTN_RANGE: snprintf(buf, n, "Range: %s", s->range == GUI_WFM_RANGE_FRAME ? "Whole frame" : "Active"); break;
        case WF_BTN_UNITS: snprintf(buf, n, "Units: %s", s->mv ? "mV" : "IRE"); break;
        case WF_BTN_PHOSPHOR: snprintf(buf, n, "Phosphor: %s", s->phosphor ? "On" : "Off"); break;
        default: snprintf(buf, n, "Intensity: %.1fx", s->gain); break;
    }
    return buf;
}

// Right-aligned rows of overlay buttons in the panel's top band (wrapping
// on narrow panels). Returns the band's height below bounds.y.
static float wf_layout_buttons(wfm_panel_t *s, Rectangle bounds, float title_w) {
    const float btn_h = 18.0f, gap = 6.0f, pad = 8.0f;
    if (gui_chrome_hide_hidden(&s->chrome_hide)) {
        for (int b = 0; b < WF_BTN_COUNT; b++) s->btn[b] = (Rectangle){ 0 };
        return 0.0f;
    }
    float right = bounds.x + bounds.width - pad;
    float row_left = bounds.x + pad + title_w + 12.0f;   // row 0 shares the title's line
    float x = right, y = bounds.y + pad;
    int rows = 1;
    for (int b = WF_BTN_COUNT - 1; b >= 0; b--) {
        char label[40];
        wf_button_label(s, b, label, sizeof(label));
        float w = (float)gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT) + 16.0f;
        if (x - w < row_left && x < right) {
            x = right;
            y += btn_h + gap;
            row_left = bounds.x + pad;
            rows++;
        }
        s->btn[b] = (Rectangle){ x - w, y, w, btn_h };
        x -= w + gap;
    }
    return pad + rows * (btn_h + gap);
}

static void wf_render(void *state_ptr, gui_app_t *app, int channel,
                      Rectangle bounds, Color channel_color) {
    wfm_panel_t *s = state_ptr;
    if (!s) {
        DrawRectangleRec(bounds, COLOR_METER_BG);
        return;
    }
    // RF Preview on this channel (harrypm 2026-11-26): its decoded luma /
    // chroma instead of decoding the raw RF as composite.
    const int rf_format = app ? gui_video_channel_rf_format(app, channel) : -1;
    gui_wfm_t *rf_src = rf_format >= 0 ? gui_rf_scopes_wfm(channel) : NULL;
    gui_wfm_t *want_src = rf_src ? rf_src : &s->wfm;
    if (want_src != s->src) {
        s->src = want_src;
        s->have_data = false;
        s->image_dirty = true;
    }
    // Channel / Range requests go to the active source every frame.
    atomic_store(&s->src->want_channel, s->channel);
    atomic_store(&s->src->want_range, s->range);
    atomic_store(&s->rf_fed, rf_src != NULL);
    if (rf_src) atomic_store(&s->decoder_system, rf_format == 1 ? 1 : 0);   // GUI_RF_NTSC = 1
    else wf_follow_generator(s, app);
    DrawRectangleRec(bounds, wf_back(s));

    // Paused playback (harrypm 2026-11-19 "fix pause ... of the WFM"): the
    // feed stops on purpose, so the last frame - the one at the playhead -
    // stays instead of blanking as a lost feed. A step-back lead-in is
    // decoded but not shown (as the Video panel): no take until it lands.
    const bool pb = app && gui_playback_is_running(app);
    const bool paused = pb && (gui_playback_get_state(app) != PLAYBACK_STATE_PLAYING ||
                               gui_playback_is_channel_paused(app, channel));
    const bool step_hold = pb && gui_playback_display_hold_channel(app, channel);
    double now = GetTime();
    if (!step_hold && gui_wfm_take(s->src ? s->src : &s->wfm, s->grid, &s->meta)) {
        s->have_data = s->meta.cols > 0;
        s->last_frame_time = now;
        s->image_dirty = true;
    } else if (paused || step_hold) {
        s->last_frame_time = now;   // hold the shown frame
    }
    // Enabled (or emptied) while paused: this panel's own decoder has had no
    // feed - re-cue the channel once so the lead-in brings the paused frame.
    // A decode setting picked while paused (System / ...) only applies when
    // the panel's decoder sees samples: re-cue once so the lead-in replays
    // through it (harrypm 2026-11-19: "can't cycle TV System on vectorscope
    // in player mode").
    if (!paused) {
        s->pause_refreshed = false;
        s->refeed = false;
    } else if (!step_hold && (s->refeed || (!s->have_data && !s->pause_refreshed))) {
        gui_playback_refresh_channel(app, channel);
        s->pause_refreshed = true;
        s->refeed = false;
    } else if (s->have_data && now - s->last_frame_time > 0.5) {
        s->have_data = false;      // feed stopped / lock lost: blank the trace
        s->image_dirty = true;
    }

    // Axis frame: the published raster's own geometry, or the selected
    // system's before any data.
    gui_wfm_meta_t m = s->meta;
    if (!s->have_data) {
        int sys = atomic_load(&s->decoder_system);
        m.system = sys;
        m.channel = s->channel;
        m.range = s->range;
        gui_wfm_display_range(sys, s->channel, &m.lo_ire, &m.hi_ire);
        double line_us = sys == 1 ? 63.55555555555556 : 64.0;
        m.t0_us = s->range == GUI_WFM_RANGE_FRAME ? -GUI_WFM_FRAME_LEAD_US : (sys == 1 ? 9.45 : 10.5);
        double t1 = s->range == GUI_WFM_RANGE_FRAME ? line_us - GUI_WFM_FRAME_LEAD_US
                                                     : (sys == 1 ? 62.55555555555556 : 62.5);
        m.us_per_col = 0.05;
        m.cols = (int)((t1 - m.t0_us) / m.us_per_col);
    }
    gui_wfm_levels_t lv;
    gui_wfm_levels(m.system, &lv);
    const double mvpi = lv.mv_per_ire;

    // Layout: title + overlay buttons on top, tick labels + time title below,
    // amplitude labels on the left.
    const char *title = "Waveform Monitor";
    const int ffont = WF_FONT_AXIS;
    float title_w = (float)gui_text_measure(title, FONT_SIZE_OSC_LABEL);
    float band = wf_layout_buttons(s, bounds, title_w);
    float top = fmaxf(band, 8.0f + FONT_SIZE_OSC_LABEL) + 6.0f + ffont + 4.0f;  // + unit label row
    char lab[48];
    snprintf(lab, sizeof(lab), "%d", s->mv ? (int)lrint(m.hi_ire * mvpi) : (int)lrint(m.hi_ire));
    int lw = gui_text_measure(lab, ffont);
    snprintf(lab, sizeof(lab), "%d", s->mv ? (int)lrint(m.lo_ire * mvpi) : (int)lrint(m.lo_ire));
    int lw2 = gui_text_measure(lab, ffont);
    float left = 8.0f + (float)(lw > lw2 ? lw : lw2) + 10.0f;
    float bottom = 5.0f + ffont + 2.0f + ffont + 8.0f;
    Rectangle pa = { floorf(bounds.x + left), floorf(bounds.y + top),
                     floorf(bounds.width - left - 10.0f), floorf(bounds.height - top - bottom) };
    gui_text_draw(title, bounds.x + 8, bounds.y + 6, FONT_SIZE_OSC_LABEL, COLOR_TEXT);
    if (pa.width < 40.0f || pa.height < 30.0f) return;
    if (pa.width > WF_MAX_W) pa.width = WF_MAX_W;
    if (pa.height > WF_MAX_H) pa.height = WF_MAX_H;
    const int iw = (int)pa.width, ih = (int)pa.height;
    const Color trace = s->phosphor ? (Color){ 0, 230, 0, 255 } : channel_color;
    const Color axis = wf_axis(s);

    // Grid (behind the trace): major amplitude ticks and the time ticks.
    const double ystep = s->mv ? 100.0 : 20.0;
    const double dmin = s->mv ? m.lo_ire * mvpi : m.lo_ire;
    const double dmax = s->mv ? m.hi_ire * mvpi : m.hi_ire;
    const double total_us = m.cols * m.us_per_col;
    const double xstep = total_us > 0.0 ? wf_nice_step(total_us, 7.0) : 1.0;
    const Color grid = wf_grid(s);
    for (double dv = ceil(dmin / ystep) * ystep; dv <= dmax + ystep * 0.01; dv += ystep) {
        float py = wf_ire_to_y(&m, pa, s->mv ? dv / mvpi : dv);
        if (py < pa.y || py > pa.y + pa.height) continue;
        DrawLineV((Vector2){ pa.x, floorf(py) + 0.5f }, (Vector2){ pa.x + pa.width, floorf(py) + 0.5f }, grid);
    }
    for (double t = ceil(m.t0_us / xstep) * xstep; total_us > 0.0 && t <= m.t0_us + total_us + xstep * 0.01; t += xstep) {
        float px = (float)(pa.x + (t - m.t0_us) / total_us * pa.width);
        if (px < pa.x || px > pa.x + pa.width) continue;
        DrawLineV((Vector2){ floorf(px) + 0.5f, pa.y }, (Vector2){ floorf(px) + 0.5f, pa.y + pa.height }, grid);
    }

    // The raster at 1:1 pixels.
    if (s->have_data) {
        size_t need = (size_t)iw * (size_t)ih * 4;
        if (need > s->rgba_cap) {
            uint8_t *n = realloc(s->rgba, need);
            if (!n) return;
            s->rgba = n;
            s->rgba_cap = need;
        }
        if (s->tex_valid && (s->tex.width != iw || s->tex.height != ih)) {
            UnloadTexture(s->tex);
            s->tex_valid = false;
        }
        if (memcmp(&trace, &s->built_trace, sizeof(Color)) != 0) s->image_dirty = true;
        if (!s->tex_valid) {
            memset(s->rgba, 0, need);
            Image img = { .data = s->rgba, .width = iw, .height = ih, .mipmaps = 1,
                          .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            s->tex = LoadTextureFromImage(img);
            s->tex_valid = s->tex.id != 0;
            if (s->tex_valid) SetTextureFilter(s->tex, TEXTURE_FILTER_POINT);
            s->image_dirty = true;
        }
        if (s->tex_valid && s->image_dirty && wf_build_image(s, iw, ih, trace)) {
            UpdateTexture(s->tex, s->rgba);
            s->built_trace = trace;
            s->image_dirty = false;
        }
        if (s->tex_valid) DrawTexture(s->tex, (int)pa.x, (int)pa.y, WHITE);
    } else {
        const char *msg = "No data";
        int w = gui_text_measure(msg, FONT_SIZE_DROPDOWN_OPT);
        gui_text_draw(msg, pa.x + (pa.width - w) / 2.0f, pa.y + (pa.height - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                      FONT_SIZE_DROPDOWN_OPT, axis);
    }

    // Level markers (tbc drawLevelMarkers): dash style + alpha per level;
    // Black only where it differs from blanking (NTSC 7.5 IRE setup).
    static const float dash[] = { 4.0f, 2.0f };
    static const float dashdot[] = { 4.0f, 2.0f, 1.0f, 2.0f };
    const struct { double ire; const char *name; bool dd; float alpha; } marks[] = {
        { lv.sync_tip, "Sync tip", false, 0.60f },
        { lv.blanking, "Blanking", false, 0.35f },
        { lv.black, "Black", true, 0.50f },
        { lv.white, "White", false, 0.70f },
        { lv.peak, "Peak", false, 0.55f },
    };
    const size_t nmarks = sizeof(marks) / sizeof(marks[0]);
    float mark_y[sizeof(marks) / sizeof(marks[0])];
    for (size_t i = 0; i < nmarks; i++) {
        float py = floorf(wf_ire_to_y(&m, pa, marks[i].ire));
        bool shown = !(i == 2 && lv.black == lv.blanking) && py >= pa.y && py <= pa.y + pa.height;
        // C (chroma around 0): only the zero line from tbc's set.
        if (m.channel == GUI_WFM_CH_C && i != 1) shown = false;
        mark_y[i] = shown ? py : -1.0f;
    }
    if (m.channel == GUI_WFM_CH_C) {
        // +-burst peak: the burst's own amplitude reference on the C trace.
        const double burst = gui_wfm_burst_ire(m.system);
        const float pat[] = { 4.0f, 2.0f };
        for (int sgn = -1; burst > 0.0 && sgn <= 1; sgn += 2) {
            float py = floorf(wf_ire_to_y(&m, pa, sgn * burst));
            if (py < pa.y || py > pa.y + pa.height) continue;
            Color base = s->phosphor ? WHITE : COLOR_TEXT;
            Color lc = { base.r, base.g, base.b, (unsigned char)(255.0f * 0.55f) };
            wf_dashed_hline(pa.x, pa.x + pa.width, py, pat, 2, lc);
            char amp[32], text[64];
            wf_format_amp(s, sgn * burst, mvpi, amp, sizeof(amp));
            snprintf(text, sizeof(text), "%sBurst (%s)", sgn > 0 ? "+" : "-", amp);
            gui_text_draw(text, pa.x + 5.0f, sgn > 0 ? py - 3.0f - WF_FONT_MARK : py + 3.0f,
                          WF_FONT_MARK, lc);
        }
    }
    for (size_t i = 0; i < nmarks; i++) {
        float py = mark_y[i];
        if (py < 0.0f) continue;
        Color base = s->phosphor ? WHITE : COLOR_TEXT;
        Color lc = { base.r, base.g, base.b, (unsigned char)(255.0f * marks[i].alpha) };
        if (marks[i].dd) wf_dashed_hline(pa.x, pa.x + pa.width, py, dashdot, 4, lc);
        else wf_dashed_hline(pa.x, pa.x + pa.width, py, dash, 2, lc);
        char amp[32], text[64];
        wf_format_amp(s, marks[i].ire, mvpi, amp, sizeof(amp));
        snprintf(text, sizeof(text), "%s (%s)", marks[i].name, amp);
        // Label above the line (tbc), or below it when the next level up is
        // too close for both labels (NTSC blanking vs the 7.5 IRE setup).
        float above = py - 3.0f - WF_FONT_MARK;
        float ly = above;
        for (size_t j = i + 1; j < nmarks; j++) {
            if (mark_y[j] < 0.0f) continue;
            if (py - mark_y[j] < WF_FONT_MARK + 4.0f) ly = py + 3.0f;
            break;
        }
        gui_text_draw(text, pa.x + 5.0f, ly, WF_FONT_MARK, lc);
    }

    // Axes: amplitude ticks + labels (left), time ticks + labels + title.
    DrawLineV((Vector2){ pa.x - 0.5f, pa.y }, (Vector2){ pa.x - 0.5f, pa.y + pa.height }, axis);
    DrawLineV((Vector2){ pa.x, pa.y + pa.height + 0.5f }, (Vector2){ pa.x + pa.width, pa.y + pa.height + 0.5f }, axis);
    for (double dv = ceil(dmin / ystep) * ystep; dv <= dmax + ystep * 0.01; dv += ystep) {
        float py = floorf(wf_ire_to_y(&m, pa, s->mv ? dv / mvpi : dv)) + 0.5f;
        if (py < pa.y || py > pa.y + pa.height + 1.0f) continue;
        DrawLineV((Vector2){ pa.x - 4.0f, py }, (Vector2){ pa.x, py }, axis);
        snprintf(lab, sizeof(lab), "%d", (int)lrint(dv));
        int w = gui_text_measure(lab, ffont);
        gui_text_draw(lab, pa.x - 6.0f - w, py - ffont / 2.0f, ffont, axis);
    }
    const char *unit = s->mv ? "mV" : "IRE";
    gui_text_draw(unit, bounds.x + 8.0f, pa.y - ffont - 6.0f, ffont, axis);
    for (double t = ceil(m.t0_us / xstep) * xstep; total_us > 0.0 && t <= m.t0_us + total_us + xstep * 0.01; t += xstep) {
        float px = floorf((float)(pa.x + (t - m.t0_us) / total_us * pa.width)) + 0.5f;
        if (px < pa.x || px > pa.x + pa.width + 1.0f) continue;
        DrawLineV((Vector2){ px, pa.y + pa.height }, (Vector2){ px, pa.y + pa.height + 4.0f }, axis);
        snprintf(lab, sizeof(lab), "%.1f", fabs(t) < 1e-9 ? 0.0 : t);
        int w = gui_text_measure(lab, ffont);
        gui_text_draw(lab, px - w / 2.0f, pa.y + pa.height + 5.0f, ffont, axis);
    }
    const char *ttl = "Time (us after 0H)";
    int tw = gui_text_measure(ttl, ffont);
    gui_text_draw(ttl, pa.x + (pa.width - tw) / 2.0f, pa.y + pa.height + 5.0f + ffont + 2.0f, ffont, axis);

    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
}

static void wf_render_overlay(void *state_ptr, Rectangle bounds) {
    wfm_panel_t *s = state_ptr;
    (void)bounds;
    if (!s || gui_chrome_hide_hidden(&s->chrome_hide)) return;
    for (int b = 0; b < WF_BTN_COUNT; b++) {
        Rectangle r = s->btn[b];
        if (r.width <= 0.0f) continue;
        char label[40];
        wf_button_label(s, b, label, sizeof(label));
        float tw = (float)gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT);
        bool hover = CheckCollisionPointRec(gui_ui_get_mouse_position(), r);
        DrawRectangleRounded(r, 0.15f, 4, hover ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
        gui_text_draw(label, r.x + (r.width - tw) / 2.0f, r.y + (r.height - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    }
}

static void wf_set_gain(wfm_panel_t *s, float g) {
    if (g < 0.1f) g = 0.1f;
    if (g > 10.0f) g = 10.0f;
    s->gain = roundf(g * 10.0f) / 10.0f;   // tbc's slider: 0.1 steps
    s->image_dirty = true;
}

static bool wf_handle_click(void *state_ptr, gui_app_t *app, int channel,
                            Vector2 click, Rectangle bounds) {
    (void)app;
    (void)channel;
    wfm_panel_t *s = state_ptr;
    if (!s) return false;
    if (!CheckCollisionPointRec(click, bounds)) {
        gui_chrome_hide_reset(&s->chrome_hide);
        return false;
    }
    if (!gui_chrome_hide_hidden(&s->chrome_hide)) {
        for (int b = 0; b < WF_BTN_COUNT; b++) {
            if (s->btn[b].width <= 0.0f || !CheckCollisionPointRec(click, s->btn[b])) continue;
            switch (b) {
                case WF_BTN_SYSTEM:
                    if (atomic_load(&s->rf_fed)) break;   // the RF Preview sets the system
                    s->refeed = true;
                    // Auto -> PAL -> NTSC -> SECAM -> Auto (the Vector panel's cycle).
                    if (atomic_load(&s->auto_system)) {
                        atomic_store(&s->auto_system, false);
                        wf_queue_system(s, 0);
                    } else if (atomic_load(&s->decoder_system) >= 2) {
                        atomic_store(&s->auto_system, true);
                    } else {
                        wf_queue_system(s, atomic_load(&s->decoder_system) + 1);
                    }
                    break;
                case WF_BTN_CHANNEL:
                    // Y -> Y+C -> C -> Y
                    s->channel = s->channel == GUI_WFM_CH_Y ? GUI_WFM_CH_YC
                               : s->channel == GUI_WFM_CH_YC ? GUI_WFM_CH_C : GUI_WFM_CH_Y;
                    atomic_store(&s->wfm.want_channel, s->channel);
                    s->refeed = true;
                    break;
                case WF_BTN_RANGE:
                    s->range = s->range == GUI_WFM_RANGE_FRAME ? GUI_WFM_RANGE_ACTIVE : GUI_WFM_RANGE_FRAME;
                    atomic_store(&s->wfm.want_range, s->range);
                    s->refeed = true;
                    break;
                case WF_BTN_UNITS: s->mv = !s->mv; break;
                case WF_BTN_PHOSPHOR: s->phosphor = !s->phosphor; break;
                default: {
                    // Presets up the tbc slider's range; the wheel fine-tunes.
                    static const float presets[] = { 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f };
                    float next = presets[0];
                    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
                        if (presets[i] > s->gain + 0.05f) { next = presets[i]; break; }
                    }
                    wf_set_gain(s, next);
                    break;
                }
            }
            s->image_dirty = true;
            gui_chrome_hide_reset(&s->chrome_hide);
            return true;
        }
    }
    // Double-click elsewhere toggles the overlay chrome (shared behaviour).
    return gui_chrome_hide_feed_click(&s->chrome_hide, click);
}

// Mouse wheel over the panel: intensity, one x1.1 step per scroll event
// (the sign only, as the Waveform panel's zoom: wheel/touchpad deltas vary
// in size), at least 0.1 per step, 0.1-10x.
static bool wf_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
    wfm_panel_t *s = state_ptr;
    if (!s || delta == 0.0f) return false;
    if (!CheckCollisionPointRec(gui_ui_get_mouse_position(), bounds)) return false;
    float g = delta > 0.0f ? s->gain * 1.1f : s->gain / 1.1f;
    if (fabsf(g - s->gain) < 0.1f) g = s->gain + (delta > 0.0f ? 0.1f : -0.1f);
    wf_set_gain(s, g);
    return true;
}

static const panel_vtable_t s_wfm_vtable = {
    .name = "Waveform Monitor",
    .create = wf_create,
    .destroy = wf_destroy,
    .clear = wf_clear,
    .process = wf_process,
    .render = wf_render,
    .render_overlay = wf_render_overlay,
    .handle_click = wf_handle_click,
    .handle_scroll = wf_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_wfm_panel_register(void) {
    panel_register(PANEL_VIEW_WFM, &s_wfm_vtable);
    gui_cvbs_set_feed_follow(PANEL_VIEW_WFM, wf_follow_system);
}
