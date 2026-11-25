/*
 * MISRC GUI - Hi-Fi Audio panel (see gui_hifi_panel.h).
 */
#include "gui_hifi_panel.h"

#include "../core/gui_app.h"
#include "../processing/gui_hifi34.h"
#include "../ui/gui_ui.h"
#include "gui_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HP_RATE 48000u
#define HP_MAX_FRAMES (2u * HP_RATE)       // longest span: 2 s

static const int s_span_ms[] = { 10, 20, 50, 100, 250, 500, 1000, 2000 };
#define HP_SPAN_COUNT ((int)(sizeof(s_span_ms) / sizeof(s_span_ms[0])))
#define HP_SPAN_DEFAULT 3                  // 100 ms

enum { HP_BTN_SOURCE, HP_BTN_FORMAT, HP_BTN_SPAN, HP_BTN_COUNT };

typedef struct {
    int span_idx;
    Rectangle btn[HP_BTN_COUNT];
    float *buf;                            // HP_MAX_FRAMES stereo frames
} hifi_panel_t;

static int s_launch_span = HP_SPAN_DEFAULT;

//-----------------------------------------------------------------------------
// Launch flag
//-----------------------------------------------------------------------------

bool gui_hifi_panel_apply_launch_arg(const char *arg, int *source, int *format) {
    static const char *usage = "--hifi: expected key=value[,...] with src=auto|a|b, "
                               "fmt=auto|pal|ntsc|beta, span=10|20|50|100|250|500|1000|2000 (ms)";
    if (source) *source = -1;
    if (format) *format = -1;
    if (!arg || !arg[0]) {
        fprintf(stderr, "%s\n", usage);
        return false;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", arg);
    int span = s_launch_span;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) { fprintf(stderr, "%s (got '%s')\n", usage, arg); return false; }
        *eq = '\0';
        const char *k = tok, *v = eq + 1;
        bool ok = false;
        if (strcmp(k, "src") == 0) {
            int s = strcmp(v, "auto") == 0 ? 0 : strcmp(v, "a") == 0 ? 1 : strcmp(v, "b") == 0 ? 2 : -1;
            if (s >= 0) { if (source) *source = s; ok = true; }
        } else if (strcmp(k, "fmt") == 0) {
            int f = strcmp(v, "auto") == 0 ? 0 : strcmp(v, "pal") == 0 ? 1 :
                    strcmp(v, "ntsc") == 0 ? 2 : strcmp(v, "beta") == 0 ? 3 : -1;
            if (f >= 0) { if (format) *format = f; ok = true; }
        } else if (strcmp(k, "span") == 0) {
            int ms = atoi(v);
            for (int i = 0; i < HP_SPAN_COUNT; i++)
                if (s_span_ms[i] == ms) { span = i; ok = true; }
        }
        if (!ok) { fprintf(stderr, "%s (got '%s')\n", usage, arg); return false; }
    }
    s_launch_span = span;
    fprintf(stderr, "[AUTO] --hifi: src=%s fmt=%s span=%d ms\n",
            source && *source >= 0 ? gui_hifi34_source_name(*source) : "(settings)",
            format && *format >= 0 ? gui_hifi34_format_name(*format) : "(settings)",
            s_span_ms[span]);
    return true;
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

static void *hp_create(void) {
    hifi_panel_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->buf = malloc(sizeof(float) * 2u * HP_MAX_FRAMES);
    if (!s->buf) { free(s); return NULL; }
    s->span_idx = s_launch_span;
    return s;
}

static void hp_destroy(void *state) {
    hifi_panel_t *s = state;
    if (!s) return;
    free(s->buf);
    free(s);
}

static void hp_clear(void *state) { (void)state; }

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

static void hp_button_label(const hifi_panel_t *s, const gui_app_t *app, const gui_hifi34_status_t *st,
                            int b, char *out, size_t n) {
    switch (b) {
        case HP_BTN_SOURCE:
            if (app->settings.hifi34_source == 0 && st->running)
                snprintf(out, n, "Source: Auto (%c)", st->channel ? 'B' : 'A');
            else
                snprintf(out, n, "Source: %s", gui_hifi34_source_name(app->settings.hifi34_source));
            break;
        case HP_BTN_FORMAT:
            if (app->settings.hifi34_format == 0 && st->running)
                snprintf(out, n, "Format: Auto (%s)", st->fmt == GUI_HIFI_FMT_VHS_PAL ? "PAL" : "NTSC");
            else
                snprintf(out, n, "Format: %s", gui_hifi34_format_name(app->settings.hifi34_format));
            break;
        default:
            if (s_span_ms[s->span_idx] >= 1000) snprintf(out, n, "Span: %d s", s_span_ms[s->span_idx] / 1000);
            else snprintf(out, n, "Span: %d ms", s_span_ms[s->span_idx]);
            break;
    }
}

static float hp_dbfs(float peak) {
    return peak > 1e-6f ? 20.0f * log10f(peak) : -120.0f;
}

// One lane: centre line, -6 dBFS grid, envelope / trace, label + peak.
static void hp_draw_lane(const float *buf, size_t frames, int ch, Rectangle r, Color col, const char *name) {
    DrawRectangleLinesEx(r, 1.0f, COLOR_GRID);
    const float mid = r.y + r.height * 0.5f, half = r.height * 0.5f - 2.0f;
    DrawLine((int)r.x, (int)mid, (int)(r.x + r.width), (int)mid, COLOR_GRID);
    for (int k = -1; k <= 1; k += 2) {
        const float y = mid + (float)k * half * 0.5f;   // -6 dBFS
        for (float x = r.x; x < r.x + r.width; x += 6.0f)
            DrawLine((int)x, (int)y, (int)fminf(x + 2.0f, r.x + r.width), (int)y, COLOR_GRID);
    }
    float peak = 0.0f;
    const int cols = (int)r.width;
    if (frames >= 2 && cols > 2) {
        const double per_col = (double)frames / (double)cols;
        if (per_col >= 1.0) {
            for (int x = 0; x < cols; x++) {
                size_t a = (size_t)((double)x * per_col), b = (size_t)((double)(x + 1) * per_col);
                if (b > frames) b = frames;
                if (b <= a) b = a + 1;
                float lo = 1.0f, hi = -1.0f;
                for (size_t i = a; i < b; i++) {
                    const float v = buf[2 * i + ch];
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                    if (fabsf(v) > peak) peak = fabsf(v);
                }
                lo = fmaxf(-1.0f, lo);
                hi = fminf(1.0f, hi);
                DrawLine((int)(r.x + x), (int)(mid - hi * half), (int)(r.x + x), (int)(mid - lo * half) + 1, col);
            }
        } else {
            Vector2 prev = { r.x, mid - fmaxf(-1.0f, fminf(1.0f, buf[ch])) * half };
            for (size_t i = 1; i < frames; i++) {
                const float v = buf[2 * i + ch];
                if (fabsf(v) > peak) peak = fabsf(v);
                Vector2 p = { r.x + (float)i / (float)(frames - 1) * r.width,
                              mid - fmaxf(-1.0f, fminf(1.0f, v)) * half };
                DrawLineV(prev, p, col);
                prev = p;
            }
        }
    }
    char label[48];
    const float db = hp_dbfs(peak);
    if (db <= -119.0f) snprintf(label, sizeof(label), "%s  -inf dBFS", name);
    else snprintf(label, sizeof(label), "%s  %.1f dBFS", name, db);
    gui_text_draw(label, r.x + 4.0f, r.y + 3.0f, FONT_SIZE_DROPDOWN_OPT,
                  peak >= 0.99f ? COLOR_CLIP_RED : COLOR_TEXT);
}

static void hp_render(void *state, gui_app_t *app, int channel, Rectangle bounds, Color color) {
    (void)channel;
    (void)color;
    hifi_panel_t *s = state;
    if (!s || !app) return;
    DrawRectangleRec(bounds, COLOR_METER_BG);
    gui_hifi34_status_t st;
    gui_hifi34_get_status(&st);

    gui_text_draw("Hi-Fi Audio", bounds.x + 8, bounds.y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);
    char line[160];
    Color line_col = COLOR_TEXT_DIM;
    // Status line room: left of the button stack (widest label + margins).
    const float line_room = bounds.width - 16.0f - 150.0f;
    if (st.running) {
        const char *where = st.owns_ch34 ? (st.monitored ? "  CH3/4 monitored" : "  on CH3/4") :
                            st.real_ch34 ? "  (CH3/4 = device audio)" : "";
        // Longest form first; drop the trailing parts while it does not fit.
        for (int form = 0; form < 3; form++) {
            if (form == 0)
                snprintf(line, sizeof(line), "RF %c  %s  %s  CPU %.0f%%%s", st.channel ? 'B' : 'A',
                         gui_hifi_fmt_name(st.fmt), st.locked ? "Locked" : "Acquiring", st.cpu_percent, where);
            else if (form == 1)
                snprintf(line, sizeof(line), "RF %c  %s  %s  CPU %.0f%%", st.channel ? 'B' : 'A',
                         gui_hifi_fmt_name(st.fmt), st.locked ? "Locked" : "Acquiring", st.cpu_percent);
            else
                snprintf(line, sizeof(line), "RF %c  %s  %s", st.channel ? 'B' : 'A',
                         gui_hifi_fmt_name(st.fmt), st.locked ? "Locked" : "Acquiring");
            if ((float)gui_text_measure(line, FONT_SIZE_DROPDOWN_OPT) <= line_room) break;
        }
        line_col = st.locked ? COLOR_SYNC_GREEN : COLOR_METER_YELLOW;
    } else if (!st.wanted) {
        snprintf(line, sizeof(line), "Starts with playback / capture");
    } else if (st.rate_unsupported) {
        snprintf(line, sizeof(line), "RF sample rate not supported for Hi-Fi decode");
        line_col = COLOR_CLIP_RED;
    } else {
        // Scores of the format(s) the Format setting allows (a forced format
        // that does not match the tape reads low, not the other pair's score).
        float sc[2];
        for (int ch = 0; ch < 2; ch++) {
            const int f = app->settings.hifi34_format;
            sc[ch] = f == 1 ? st.score[ch][GUI_HIFI_FMT_VHS_PAL] :
                     f == 2 ? st.score[ch][GUI_HIFI_FMT_VHS_NTSC] :
                     f == 3 ? st.score[ch][GUI_HIFI_FMT_BETA_NTSC] :
                     fmaxf(st.score[ch][GUI_HIFI_FMT_VHS_NTSC], st.score[ch][GUI_HIFI_FMT_VHS_PAL]);
        }
        snprintf(line, sizeof(line), "Searching for %s carriers  (A %.2f / B %.2f)",
                 app->settings.hifi34_format ? gui_hifi34_format_name(app->settings.hifi34_format) : "Hi-Fi",
                 sc[0], sc[1]);
        line_col = COLOR_METER_YELLOW;
    }
    gui_text_draw(line, bounds.x + 8, bounds.y + 4 + FONT_SIZE_OSC_LABEL + 4, FONT_SIZE_DROPDOWN_OPT, line_col);

    // Right-aligned button stack (the Vector / Video panels' style).
    const float btn_h = 18.0f, gap = 6.0f;
    float by = bounds.y + 8.0f, stack_w = 0.0f;
    const Vector2 mouse = gui_ui_get_mouse_position();
    for (int b = 0; b < HP_BTN_COUNT; b++) {
        char label[48];
        hp_button_label(s, app, &st, b, label, sizeof(label));
        const float tw = (float)gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT);
        const float w = fmaxf(130.0f, tw + 16.0f);
        Rectangle r = { bounds.x + bounds.width - 8.0f - w, by, w, btn_h };
        s->btn[b] = r;
        DrawRectangleRounded(r, 0.15f, 4, CheckCollisionPointRec(mouse, r) ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
        gui_text_draw(label, r.x + (w - tw) / 2.0f, r.y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        if (w > stack_w) stack_w = w;
        by += btn_h + gap;
    }

    // Plot area: below the header; beside the buttons when wide, else below them.
    const float header = 4.0f + FONT_SIZE_OSC_LABEL + 4.0f + FONT_SIZE_DROPDOWN_OPT + 8.0f;
    Rectangle plot = { bounds.x + 8.0f, bounds.y + header, bounds.width - 16.0f, bounds.height - header - 8.0f };
    if (bounds.width - stack_w - 24.0f >= 240.0f) {
        plot.width = bounds.width - stack_w - 24.0f;
    } else {
        plot.y = by + 4.0f;
        plot.height = bounds.y + bounds.height - 8.0f - plot.y;
    }
    if (plot.height < 40.0f || plot.width < 60.0f) return;
    const size_t want = (size_t)s_span_ms[s->span_idx] * HP_RATE / 1000u;
    const size_t frames = st.running ? gui_hifi34_copy_recent(s->buf, want) : 0;
    const float lane_gap = 6.0f, lane_h = (plot.height - lane_gap) * 0.5f;
    hp_draw_lane(s->buf, frames, 0, (Rectangle){ plot.x, plot.y, plot.width, lane_h },
                 (Color){ 90, 200, 255, 255 }, "L");
    hp_draw_lane(s->buf, frames, 1, (Rectangle){ plot.x, plot.y + lane_h + lane_gap, plot.width, lane_h },
                 (Color){ 255, 120, 110, 255 }, "R");
}

//-----------------------------------------------------------------------------
// Interaction
//-----------------------------------------------------------------------------

static bool hp_click(void *state, gui_app_t *app, int channel, Vector2 click, Rectangle bounds) {
    (void)channel;
    (void)bounds;
    hifi_panel_t *s = state;
    if (!s || !app) return false;
    for (int b = 0; b < HP_BTN_COUNT; b++) {
        if (s->btn[b].width <= 0.0f || !CheckCollisionPointRec(click, s->btn[b])) continue;
        if (b == HP_BTN_SOURCE) {
            app->settings.hifi34_source = (app->settings.hifi34_source + 1) % 3;
            gui_settings_save(&app->settings);
        } else if (b == HP_BTN_FORMAT) {
            app->settings.hifi34_format = (app->settings.hifi34_format + 1) % 4;
            gui_settings_save(&app->settings);
        } else {
            s->span_idx = (s->span_idx + 1) % HP_SPAN_COUNT;
        }
        return true;
    }
    return false;
}

static bool hp_scroll(void *state, float delta, Rectangle bounds) {
    (void)bounds;
    hifi_panel_t *s = state;
    if (!s || delta == 0.0f) return false;
    // Wheel up = shorter span (zoom in), as the Spectro / Waveform spans.
    int idx = s->span_idx + (delta > 0.0f ? -1 : 1);
    if (idx < 0) idx = 0;
    if (idx >= HP_SPAN_COUNT) idx = HP_SPAN_COUNT - 1;
    s->span_idx = idx;
    return true;
}

static const panel_vtable_t s_hifi_vtable = {
    .name = "HiFi",
    .create = hp_create,
    .destroy = hp_destroy,
    .clear = hp_clear,
    .process = NULL,          // fed by the CH3/4 Hi-Fi decoder, not the display thread
    .render = hp_render,
    .render_overlay = NULL,
    .handle_click = hp_click,
    .handle_scroll = hp_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_hifi_panel_register(void) {
    panel_register(PANEL_VIEW_HIFI, &s_hifi_vtable);
}
