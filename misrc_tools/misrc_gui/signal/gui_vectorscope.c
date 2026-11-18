/*
 * MISRC GUI - Composite Vectorscope (accumulator + calibration)
 * See gui_vectorscope.h.
 */
#include "gui_vectorscope.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VSCOPE_CELLS ((size_t)GUI_VSCOPE_GRID * GUI_VSCOPE_GRID)

_Static_assert((1 << GUI_VSCOPE_GRID_SHIFT) == GUI_VSCOPE_GRID, "grid shift");

// Zero rows [r0, r1] of a grid (r0 > r1: nothing).
static void vscope_clear_rows(uint32_t *grid, int r0, int r1) {
    if (!grid || r1 < r0) return;
    memset(grid + ((size_t)r0 << GUI_VSCOPE_GRID_SHIFT), 0,
           (size_t)(r1 - r0 + 1) * GUI_VSCOPE_GRID * sizeof(uint32_t));
}

// The field's counters, box and readout sums (not the grids).
static void vscope_restart_field(gui_vectorscope_t *scope) {
    scope->acc_points = 0;
    scope->have_last = false;
    scope->acc_meta.segments = 0;
    scope->acc_meta.x0 = scope->acc_meta.y0 = GUI_VSCOPE_GRID;
    scope->acc_meta.x1 = scope->acc_meta.y1 = -1;
    scope->m_burst_sum = scope->m_burst_ire_sum = scope->m_jit_sq = scope->m_chroma_sum = 0.0;
    scope->m_vp_s = scope->m_vp_c = scope->m_vn_s = scope->m_vn_c = 0.0;
    scope->m_chroma_n = 0;
    scope->m_lines = scope->m_ire_lines = 0;
    scope->m_secam = false;
    for (int c = 0; c < 2; c++) {
        scope->m_s_sum[c] = scope->m_s_sq[c] = scope->m_s_mv[c] = 0.0;
        scope->m_s_n[c] = scope->m_s_mv_n[c] = 0;
    }
    scope->m_s_ident_err = 0;
    scope->m_s_last_line = -1;
    scope->m_s_last_red = false;
}

// SECAM standard porch carrier, peak-to-peak mV: 2M0 = 23% of the luminance
// amplitude (161 mV) at the bell centre f0 = 4.286 MHz, times the encoder
// bell's gain |1 + j16u| / |1 + j1.26u|, u = f/f0 - f0/f (ITU-R BT.470).
static double vscope_secam_porch_standard_mv(double f) {
    const double f0 = 4286000.0, u = f / f0 - f0 / f;
    return 161.0 * hypot(1.0, 16.0 * u) / hypot(1.0, 1.26 * u);
}

static void vscope_finish_secam(gui_vectorscope_t *scope, gui_vscope_meas_t *m) {
    const uint32_t n = scope->m_s_n[0] + scope->m_s_n[1];
    if (n < 8) return;
    static const double rest[2] = { 4250000.0, 4406250.0 };
    m->valid = true;
    m->secam = true;
    m->burst_lines = n;
    m->ident_errors = scope->m_s_ident_err;
    double ss = 0.0;
    for (int c = 0; c < 2; c++) {
        m->secam_lines[c] = scope->m_s_n[c];
        if (scope->m_s_n[c] > 0) {
            const double k = (double)scope->m_s_n[c];
            const double mean = scope->m_s_sum[c] / k;
            m->rest_err_khz[c] = mean / 1000.0;
            const double var = scope->m_s_sq[c] / k - mean * mean;
            ss += (var > 0.0 ? var : 0.0) * k;
        }
        if (scope->m_s_mv_n[c] > 0) {
            m->porch_mv[c] = scope->m_s_mv[c] / (double)scope->m_s_mv_n[c];
            m->porch_percent[c] = 100.0 * m->porch_mv[c] / vscope_secam_porch_standard_mv(rest[c]);
        }
    }
    m->rest_jitter_khz = sqrt(ss / (double)n) / 1000.0;
}

bool gui_vectorscope_init(gui_vectorscope_t *scope) {
    if (!scope) return false;
    memset(scope, 0, sizeof(*scope));
    scope->acc = (uint32_t *)calloc(VSCOPE_CELLS, sizeof(uint32_t));
    scope->pub = (uint32_t *)calloc(VSCOPE_CELLS, sizeof(uint32_t));
    scope->acc_tr = (uint32_t *)calloc(VSCOPE_CELLS, sizeof(uint32_t));
    scope->pub_tr = (uint32_t *)calloc(VSCOPE_CELLS, sizeof(uint32_t));
    if (!scope->acc || !scope->pub || !scope->acc_tr || !scope->pub_tr) {
        gui_vectorscope_free(scope);
        return false;
    }
    scope->pub_r0 = scope->pub_t0 = GUI_VSCOPE_GRID;
    scope->pub_r1 = scope->pub_t1 = -1;
    atomic_init(&scope->pub_ready, 0);
    atomic_init(&scope->fields, 0);
    atomic_init(&scope->want_mode, GUI_VSCOPE_MODE_STANDARD);
    // Advanced default: the whole line (decode-orc's default window, and a
    // hardware scope's view - the burst is part of the picture).
    atomic_init(&scope->want_lines, GUI_VSCOPE_LINES_WHOLE);
    atomic_init(&scope->want_field, GUI_VSCOPE_FIELD_ACTIVE);
    atomic_init(&scope->want_trace, true);
    vscope_restart_field(scope);
    gui_vectorscope_begin_field(scope);
    return true;
}

void gui_vectorscope_free(gui_vectorscope_t *scope) {
    if (!scope) return;
    free(scope->acc);
    free(scope->pub);
    free(scope->acc_tr);
    free(scope->pub_tr);
    scope->acc = scope->pub = scope->acc_tr = scope->pub_tr = NULL;
}

void gui_vectorscope_begin_field(gui_vectorscope_t *scope) {
    if (!scope) return;
    const int mode = atomic_load(&scope->want_mode);
    const int lines = atomic_load(&scope->want_lines);
    const int field = atomic_load(&scope->want_field);
    scope->mode = mode == GUI_VSCOPE_MODE_ADVANCED ? mode : GUI_VSCOPE_MODE_STANDARD;
    scope->lines_view = (lines >= 0 && lines <= GUI_VSCOPE_LINES_BURST) ? lines : GUI_VSCOPE_LINES_ACTIVE;
    scope->field_view = field == GUI_VSCOPE_FIELD_WHOLE ? field : GUI_VSCOPE_FIELD_ACTIVE;
    const bool trace = atomic_load(&scope->want_trace);
    scope->trace_on = trace;
    scope->acc_meta.trace = trace;
    // decode-orc's sample stride: with the trace on, every second sample
    // (the transits join them, and the renderer's dwell / transit laws carry
    // the stride); the dots alone keep every sample.
    scope->stride = trace ? 2 : 1;
    scope->acc_meta.stride = scope->stride;
    scope->acc_meta.mode = scope->mode;
    scope->acc_meta.lines_view = scope->lines_view;
    scope->acc_meta.field_view = scope->field_view;
    scope->have_last = false;
}

void gui_vectorscope_reset(gui_vectorscope_t *scope) {
    if (!scope || !scope->acc) return;
    // Mid-field: the grids may hold counts anywhere the field reached.
    memset(scope->acc, 0, VSCOPE_CELLS * sizeof(uint32_t));
    memset(scope->acc_tr, 0, VSCOPE_CELLS * sizeof(uint32_t));
    vscope_restart_field(scope);
    gui_vectorscope_begin_field(scope);
}

// The field's readouts from the decoder's per-line sums.
static void vscope_finish_meas(gui_vectorscope_t *scope, gui_vscope_meas_t *m) {
    memset(m, 0, sizeof(*m));
    if (scope->mode != GUI_VSCOPE_MODE_ADVANCED) return;
    if (scope->m_secam) {
        vscope_finish_secam(scope, m);
        return;
    }
    if (scope->m_lines < 8) return;
    const bool pal = scope->acc_system != GUI_VSCOPE_SYS_NTSC;
    const double nominal = pal ? 100.0 * 3.0 / 14.0 : 20.0;
    m->valid = true;
    m->burst_lines = scope->m_lines;
    if (scope->m_ire_lines > 0) {
        m->burst_ire = scope->m_burst_ire_sum / (double)scope->m_ire_lines;
        m->burst_percent = 100.0 * m->burst_ire / nominal;
    }
    m->jitter_deg = sqrt(scope->m_jit_sq / (double)scope->m_lines) * 180.0 / M_PI;
    if (pal && (scope->m_vp_s != 0.0 || scope->m_vp_c != 0.0) &&
        (scope->m_vn_s != 0.0 || scope->m_vn_c != 0.0)) {
        double ap = atan2(scope->m_vp_s, scope->m_vp_c);
        double an = atan2(scope->m_vn_s, scope->m_vn_c);
        double split = ap - an;
        while (split > M_PI) split -= 2.0 * M_PI;
        while (split < -M_PI) split += 2.0 * M_PI;
        m->vsplit_deg = fabs(split) * 0.5 * 180.0 / M_PI - 45.0;
    }
    if (scope->m_chroma_n > 0 && scope->m_burst_sum > 0.0) {
        m->chroma_burst = (scope->m_chroma_sum / (double)scope->m_chroma_n) /
                          (scope->m_burst_sum / (double)scope->m_lines);
    }
}

void gui_vectorscope_end_field_parity(gui_vectorscope_t *scope, int field) {
    if (!scope || !scope->acc) return;
    atomic_fetch_add(&scope->fields, 1u);
    gui_vectorscope_break(scope);
    // This field's rows: the acc pair was all zero at its start.
    int r0 = scope->acc_meta.y0, r1 = scope->acc_meta.y1;
    int t0 = scope->trace_on ? r0 : GUI_VSCOPE_GRID, t1 = scope->trace_on ? r1 : -1;
    // Renderer still holds the previous field: drop this one (the display
    // only needs the latest; the decoder never waits on the render thread).
    if (atomic_load_explicit(&scope->pub_ready, memory_order_acquire) == 0) {
        uint32_t *swap = scope->pub;
        scope->pub = scope->acc;
        scope->acc = swap;
        swap = scope->pub_tr;
        scope->pub_tr = scope->acc_tr;
        scope->acc_tr = swap;
        // The acc pair is now the old pub pair: clear ITS rows below.
        int k;
        k = scope->pub_r0; scope->pub_r0 = r0; r0 = k;
        k = scope->pub_r1; scope->pub_r1 = r1; r1 = k;
        k = scope->pub_t0; scope->pub_t0 = t0; t0 = k;
        k = scope->pub_t1; scope->pub_t1 = t1; t1 = k;
        scope->pub_points = scope->acc_points;
        scope->pub_system = scope->acc_system;
        scope->acc_meta.points = scope->acc_points;
        scope->acc_meta.system = scope->acc_system;
        scope->acc_meta.field = field;
        vscope_finish_meas(scope, &scope->acc_meta.meas);
        scope->pub_meta = scope->acc_meta;
        atomic_store_explicit(&scope->pub_ready, 1, memory_order_release);
    }
    vscope_clear_rows(scope->acc, r0, r1);
    vscope_clear_rows(scope->acc_tr, t0, t1);
    vscope_restart_field(scope);
    gui_vectorscope_begin_field(scope);
}

void gui_vectorscope_end_field(gui_vectorscope_t *scope) {
    gui_vectorscope_end_field_parity(scope, 0);
}

bool gui_vectorscope_take_ex(gui_vectorscope_t *scope, uint32_t *dst, uint32_t *dst_tr,
                             gui_vscope_meta_t *meta) {
    if (!scope || !scope->pub || !dst) return false;
    if (atomic_load_explicit(&scope->pub_ready, memory_order_acquire) == 0) return false;
    memcpy(dst, scope->pub, VSCOPE_CELLS * sizeof(uint32_t));
    if (dst_tr) {
        if (scope->pub_meta.trace) memcpy(dst_tr, scope->pub_tr, VSCOPE_CELLS * sizeof(uint32_t));
        else memset(dst_tr, 0, VSCOPE_CELLS * sizeof(uint32_t));
    }
    if (meta) *meta = scope->pub_meta;
    atomic_store_explicit(&scope->pub_ready, 0, memory_order_release);
    return true;
}

bool gui_vectorscope_take_swap(gui_vectorscope_t *scope, uint32_t **hits, uint32_t **trans,
                               gui_vscope_meta_t *meta) {
    if (!scope || !scope->pub || !hits || !*hits || !trans || !*trans) return false;
    if (atomic_load_explicit(&scope->pub_ready, memory_order_acquire) == 0) return false;
    // The decoder only touches pub while pub_ready == 0 (its publish swap):
    // here it is 1, so the render thread owns pub until the release below.
    uint32_t *swap = scope->pub;
    scope->pub = *hits;
    *hits = swap;
    swap = scope->pub_tr;
    scope->pub_tr = *trans;
    *trans = swap;
    // The caller's buffers are all zero (the contract).
    scope->pub_r0 = scope->pub_t0 = GUI_VSCOPE_GRID;
    scope->pub_r1 = scope->pub_t1 = -1;
    if (meta) *meta = scope->pub_meta;
    atomic_store_explicit(&scope->pub_ready, 0, memory_order_release);
    return true;
}

bool gui_vectorscope_take(gui_vectorscope_t *scope, uint32_t *dst,
                          uint64_t *points, int *system) {
    gui_vscope_meta_t meta;
    if (!gui_vectorscope_take_ex(scope, dst, NULL, &meta)) return false;
    if (points) *points = meta.points;
    if (system) *system = meta.system;
    return true;
}

void gui_vectorscope_bar_target(int system, int rgb, double percent,
                                double *u, double *v) {
    double r = percent * (double)((rgb >> 2) & 1);
    double g = percent * (double)((rgb >> 1) & 1);
    double b = percent * (double)(rgb & 1);
    // BT.601 U/V (the tbc-analyse graticule matrix).
    double uu = r * -0.147141 + g * -0.288869 + b * 0.436010;
    double vv = r * 0.614975 + g * -0.514965 + b * -0.100010;
    // NTSC chroma rides the 92.5 IRE (7.5 IRE setup) span; the plot unit is
    // 100 IRE.
    double k = (system == GUI_VSCOPE_SYS_NTSC) ? 0.925 : 1.0;
    if (u) *u = uu * k;
    if (v) *v = vv * k;
}

int gui_vectorscope_field_period_system(double field_us) {
    static const double periods[2] = { 20000.0, 1e6 * 1001.0 / 60000.0 };
    static const int systems[2] = { GUI_VSCOPE_SYS_PAL, GUI_VSCOPE_SYS_NTSC };
    for (int k = 1; k <= 3; k++) {
        for (int i = 0; i < 2; i++) {
            double want = periods[i] * k;
            if (fabs(field_us - want) < want * 0.015) return systems[i];
        }
    }
    return -1;
}

void gui_vectorscope_plot_to_grid(double u, double v, double *gx, double *gy) {
    double half = 0.5 * GUI_VSCOPE_GRID;
    if (gx) *gx = half + u * (half / GUI_VSCOPE_FULL_SCALE);
    if (gy) *gy = half - v * (half / GUI_VSCOPE_FULL_SCALE);
}
