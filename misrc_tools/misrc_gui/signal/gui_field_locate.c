/*
 * MISRC GUI - Field locator. See gui_field_locate.h.
 */
#include "gui_field_locate.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool gui_field_locate_begin(gui_field_locate_t *fl, int mode, double rate, int lines_per_frame,
                            uint64_t start, uint64_t expected) {
    if (!fl || rate <= 0.0) return false;
    float *keep = fl->trace;
    size_t keep_cap = fl->trace_cap;
    memset(fl, 0, sizeof(*fl));
    fl->trace = keep;
    fl->trace_cap = keep_cap;
    fl->mode = mode;
    fl->rate = rate;
    fl->lines_per_frame = lines_per_frame == 525 ? 525 : 625;
    fl->start = start;
    fl->decim = (int)floor(rate * 0.25e-6 + 0.5);   // 4 trace points a microsecond
    if (fl->decim < 1) fl->decim = 1;
    size_t need = (size_t)(expected / (uint64_t)fl->decim) + 16;
    if (need > fl->trace_cap) {
        float *t = (float *)realloc(fl->trace, need * sizeof(float));
        if (!t) return false;
        fl->trace = t;
        fl->trace_cap = need;
    }
    return true;
}

static void fl_append(gui_field_locate_t *fl, float v) {
    if (fl->trace_len >= fl->trace_cap) {
        size_t cap = fl->trace_cap ? fl->trace_cap * 2 : 4096;
        float *t = (float *)realloc(fl->trace, cap * sizeof(float));
        if (!t) return;
        fl->trace = t;
        fl->trace_cap = cap;
    }
    fl->trace[fl->trace_len++] = v;
}

void gui_field_locate_push(gui_field_locate_t *fl, const int32_t *x, size_t n) {
    if (!fl || !x || !fl->mode) return;
    if (fl->mode == GUI_FIELD_LOCATE_CVBS) {
        for (size_t i = 0; i < n; i++) {
            fl->acc += (double)x[i];
            if (++fl->acc_n == fl->decim) {
                fl_append(fl, (float)(fl->acc / (double)fl->decim));
                fl->acc = 0.0;
                fl->acc_n = 0;
            }
        }
        fl->pushed += n;
        return;
    }
    // FM: instantaneous frequency from interpolated zero crossings of the
    // DC-removed RF (1 us DC tracker); each half period between crossings
    // gives one estimate, averaged per trace point.
    const double a = 1.0 / (fl->rate * 1.0e-6);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)(fl->pushed + i);
        const double xv = (double)x[i];
        fl->dc += a * (xv - fl->dc);
        const double y = xv - fl->dc;
        if (fl->have_prev && ((fl->prev_x < 0.0) != (y < 0.0))) {
            const double denom = fl->prev_x - y;
            const double tc = (t - 1.0) + (denom != 0.0 ? fl->prev_x / denom : 0.5);
            if (fl->have_cross) {
                const double h = tc - fl->last_cross;
                if (h > 0.5) {
                    fl->f_acc += fl->rate / (2.0 * h);
                    fl->f_n++;
                }
            }
            fl->last_cross = tc;
            fl->have_cross = true;
        }
        fl->prev_x = y;
        fl->have_prev = true;
        if (++fl->acc_n == fl->decim) {
            const float f = fl->f_n ? (float)(fl->f_acc / (double)fl->f_n) : fl->last_f;
            fl->last_f = f;
            fl_append(fl, f);
            fl->f_acc = 0.0;
            fl->f_n = 0;
            fl->acc_n = 0;
        }
    }
    fl->pushed += n;
}

static int fl_cmp_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

typedef struct { size_t start, len; } fl_run_t;

size_t gui_field_locate_finish(gui_field_locate_t *fl, gui_field_mark_t *marks, size_t max) {
    if (!fl || !fl->trace || fl->trace_len < 64 || !marks || max == 0) return 0;
    const size_t n = fl->trace_len;
    // Centred 1 us moving average (no delay on the edges).
    float *sm = (float *)malloc(n * sizeof(float));
    if (!sm) return 0;
    for (size_t i = 0; i < n; i++) {
        const size_t a = i >= 2 ? i - 2 : 0, b = i + 1 < n ? i + 1 : n - 1;
        double sum = 0.0;
        for (size_t j = a; j <= b; j++) sum += fl->trace[j];
        sm[i] = (float)(sum / (double)(b - a + 1));
    }
    // Levels: the 0.5 / 99.5 % points (sync tip / peak); the slice 22% up.
    size_t ns = n / 8 + 1;
    float *q = (float *)malloc(ns * sizeof(float));
    if (!q) { free(sm); return 0; }
    size_t k = 0;
    for (size_t i = 0; i < n && k < ns; i += 8) q[k++] = sm[i];
    qsort(q, k, sizeof(float), fl_cmp_float);
    const float lo = q[(size_t)(0.005 * (double)(k - 1))];
    const float hi = q[(size_t)(0.995 * (double)(k - 1))];
    free(q);
    if (!(hi > lo)) { free(sm); return 0; }
    const float thr = lo + 0.22f * (hi - lo);

    // Line length in trace points.
    const double field_rate = fl->lines_per_frame == 525 ? 60000.0 / 1001.0 : 50.0;
    const double line_pts = 2.0 / ((double)fl->lines_per_frame * field_rate) * fl->rate / (double)fl->decim;

    // Low runs.
    size_t cap = 4096, nr = 0;
    fl_run_t *runs = (fl_run_t *)malloc(cap * sizeof(fl_run_t));
    if (!runs) { free(sm); return 0; }
    size_t i = 0;
    while (i < n) {
        if (sm[i] >= thr) { i++; continue; }
        size_t s0 = i;
        while (i < n && sm[i] < thr) i++;
        if (nr == cap) {
            fl_run_t *r = (fl_run_t *)realloc(runs, cap * 2 * sizeof(fl_run_t));
            if (!r) break;
            runs = r;
            cap *= 2;
        }
        runs[nr].start = s0;
        runs[nr].len = i - s0;
        nr++;
    }
    free(sm);

    // Broad-pulse trains -> field starts.
    size_t nm = 0;
    size_t r = 0;
    const double broad_min = 0.25 * line_pts, broad_max = 0.62 * line_pts;
    const double hs_min = 0.055 * line_pts, hs_max = 0.11 * line_pts;   // 3.5 .. 7 us H syncs
    while (r < nr && nm < max) {
        const double len = (double)runs[r].len;
        if (len < broad_min || len > broad_max) { r++; continue; }
        // A train: broads starting within 0.7 line of the previous one.
        size_t first = r, count = 1, last = r;
        size_t j = r + 1;
        while (j < nr) {
            const double lj = (double)runs[j].len;
            const double gap = (double)runs[j].start - (double)runs[last].start;
            if (gap > 0.7 * line_pts) break;
            if (lj >= broad_min && lj <= broad_max) { count++; last = j; }
            j++;
        }
        r = j;
        // A whole train only: at least 3 broads, a line clear of the run's
        // start (a window opening mid-train would misplace it).
        if (count < 3 || (double)runs[first].start < line_pts) continue;
        const double train = (double)runs[first].start;
        // Parity: the last regular H sync within 12 lines before the train.
        int parity = 0;
        for (size_t h = first; h > 0; h--) {
            const fl_run_t *hr = &runs[h - 1];
            if (train - (double)hr->start > 12.0 * line_pts) break;
            const double hl = (double)hr->len;
            if (hl < hs_min || hl > hs_max) continue;
            double ph = fmod((train - (double)hr->start) / line_pts, 1.0);
            parity = fabs(ph - 0.5) < 0.25 ? 2 : 1;
            break;
        }
        double b = train - (fl->lines_per_frame == 525 ? 3.0 * line_pts : 0.0);
        if (b < 0.0) continue;
        marks[nm].pos = fl->start + (uint64_t)(b * (double)fl->decim + 0.5);
        marks[nm].field = parity;
        nm++;
    }
    free(runs);
    return nm;
}

void gui_field_locate_free(gui_field_locate_t *fl) {
    if (!fl) return;
    free(fl->trace);
    fl->trace = NULL;
    fl->trace_cap = fl->trace_len = 0;
}
