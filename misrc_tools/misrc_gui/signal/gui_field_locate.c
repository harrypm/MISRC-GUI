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
    gui_field_locate_t *keep_alt = fl->alt;
    float *keep_env = fl->env;
    size_t keep_env_cap = fl->env_cap;
    memset(fl, 0, sizeof(*fl));
    fl->trace = keep;
    fl->trace_cap = keep_cap;
    fl->alt = keep_alt;
    fl->env = keep_env;
    fl->env_cap = keep_env_cap;
    fl->auto_mode = mode == GUI_FIELD_LOCATE_AUTO;
    fl->auto_lines = lines_per_frame != 525 && lines_per_frame != 625;
    fl->mode = fl->auto_mode ? GUI_FIELD_LOCATE_FM : mode;
    fl->rate = rate;
    fl->lines_per_frame = lines_per_frame == 525 ? 525 : 625;
    if (fl->auto_mode) {
        if (!fl->alt) fl->alt = (gui_field_locate_t *)calloc(1, sizeof(gui_field_locate_t));
        if (!fl->alt ||
            !gui_field_locate_begin(fl->alt, GUI_FIELD_LOCATE_CVBS, rate, lines_per_frame, start, expected))
            return false;
    }
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
    if (fl->auto_mode && fl->alt) gui_field_locate_push(fl->alt, x, n);
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
        fl->env_acc += fabs(y);
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
            // Envelope point (same index as the trace point).
            if (fl->trace_len > fl->env_cap) {
                size_t cap = fl->trace_cap > fl->trace_len ? fl->trace_cap : fl->trace_len * 2;
                float *e = (float *)realloc(fl->env, cap * sizeof(float));
                if (e) { fl->env = e; fl->env_cap = cap; }
            }
            if (fl->env && fl->trace_len <= fl->env_cap)
                fl->env[fl->trace_len - 1] = (float)(fl->env_acc / (double)fl->decim);
            fl->env_acc = 0.0;
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

static size_t fl_find(gui_field_locate_t *fl, int lines, gui_field_mark_t *marks, size_t max);

// Consecutive field starts one field period apart (within 1%): how well a
// candidate's marks describe a real field sequence.
static int fl_score(const gui_field_locate_t *fl, int lines, const gui_field_mark_t *m, size_t n) {
    const double field = fl->rate / (lines == 525 ? 60000.0 / 1001.0 : 50.0);
    int score = 0;
    for (size_t i = 1; i < n; i++) {
        const double d = (double)(m[i].pos - m[i - 1].pos);
        if (fabs(d - field) < 0.01 * field) score += (m[i].field != 0) ? 3 : 2;
    }
    return score + (n > 0 ? 1 : 0);
}

// Head switches (FM): see gui_field_locate.h. Envelope at 1 us, step =
// |mean(6 us after) - mean(6 us before)| / level; fold over each system's
// frame; accept the system whose strongest phase has a second, strong
// phase half a frame (+-0.15 ms) away. Then every switch in the run is the
// local step peak (+-0.15 ms: wider caught picture steps, measured) at
// those two phases.
static void fl_head_switches(gui_field_locate_t *fl) {
    fl->hs_n = 0;
    fl->hs_lines = 0;
    if (!fl->env || fl->trace_len < 64) return;
    const int per = 4;                                  // trace points per us
    const size_t m = fl->trace_len / per;
    const double us = 1e-6 * fl->rate;                  // samples per us
    // Folding needs repetition: a system is only eligible when the run holds
    // 1.5 of its frames (a single partial pass let the PAL fold win on a
    // 1-frame NTSC window by chance - measured: the shared system flipped to
    // 625 and the Spectro lattice oscillated 103 lines).
    if ((double)m < 50000.0) return;
    float *e = (float *)malloc(m * sizeof(float)), *st = (float *)calloc(m, sizeof(float));
    if (!e || !st) { free(e); free(st); return; }
    for (size_t i = 0; i < m; i++) {
        double s = 0.0;
        for (int k = 0; k < per; k++) s += fl->env[i * per + k];
        e[i] = (float)(s / per);
    }
    const int w = 6;
    for (size_t i = w; i + w < m; i++) {
        double a = 0.0, b = 0.0;
        for (int k = 0; k < w; k++) { a += e[i - 1 - k]; b += e[i + k]; }
        a /= w; b /= w;
        st[i] = (float)(fabs(b - a) / (0.5 * (a + b) + 1e-9));
    }
    double best_score = 0.0;
    int best_lines = 0;
    double best_ph[2] = {0, 0}, best_frame = 0;
    for (int sys = 0; sys < 2; sys++) {
        const int lines = sys ? 525 : 625;
        const double frame_us = lines == 525 ? 1e6 * 1001.0 / 30000.0 : 1e6 / 25.0;
        const size_t P = (size_t)floor(frame_us + 0.5);
        if ((double)m < frame_us * 1.5) continue;
        double *acc = (double *)calloc(P, sizeof(double));
        int *cnt = (int *)calloc(P, sizeof(int));
        if (!acc || !cnt) { free(acc); free(cnt); continue; }
        for (size_t i = w; i + w < m; i++) {
            size_t ph = (size_t)fmod((double)i, frame_us);
            if (ph >= P) ph = P - 1;
            acc[ph] += st[i];
            cnt[ph]++;
        }
        double mean = 0.0;
        for (size_t i = 0; i < P; i++) { acc[i] = cnt[i] ? acc[i] / cnt[i] : 0.0; mean += acc[i]; }
        mean /= (double)P;
        size_t b1 = 0;
        for (size_t i = 0; i < P; i++) if (acc[i] > acc[b1]) b1 = i;
        // The other head: half a frame away (+-1 ms search), must land within 0.15 ms.
        const double want = fmod((double)b1 + frame_us * 0.5, frame_us);
        size_t b2 = (size_t)want % P;
        for (long d = -1000; d <= 1000; d++) {
            const size_t j = (size_t)(((long)want + d + (long)P) % (long)P);
            if (acc[j] > acc[b2]) b2 = j;
        }
        double sep = fabs((double)b2 - want);
        if (sep > frame_us * 0.5) sep = frame_us - sep;
        const double r1 = mean > 0 ? acc[b1] / mean : 0, r2 = mean > 0 ? acc[b2] / mean : 0;
        const double score = (sep < 150.0 && r1 >= 4.0 && r2 >= 3.0) ? r1 + r2 : 0.0;
        if (score > best_score) {
            best_score = score;
            best_lines = lines;
            best_ph[0] = (double)b1;
            best_ph[1] = (double)b2;
            best_frame = frame_us;
        }
        free(acc);
        free(cnt);
    }
    if (best_lines) {
        fl->hs_lines = best_lines;
        // Each occurrence: the local step peak within +-0.5 ms of the phase.
        for (double f0 = 0.0; f0 < (double)m && fl->hs_n < GUI_FIELD_LOCATE_MAX_HS; f0 += best_frame) {
            for (int h = 0; h < 2 && fl->hs_n < GUI_FIELD_LOCATE_MAX_HS; h++) {
                const double c = f0 + best_ph[h];
                if (c - 150.0 < (double)w || c + 150.0 >= (double)(m - w)) continue;
                size_t pk = (size_t)c;
                for (size_t i = (size_t)(c - 150.0); i <= (size_t)(c + 150.0); i++) if (st[i] > st[pk]) pk = i;
                fl->hs_pos[fl->hs_n] = fl->start + (uint64_t)((double)pk * us + 0.5);
                fl->hs_head[fl->hs_n] = h;
                fl->hs_n++;
            }
        }
        // In order of position.
        for (size_t i = 1; i < fl->hs_n; i++) {
            uint64_t p = fl->hs_pos[i];
            int hh = fl->hs_head[i];
            size_t j = i;
            while (j > 0 && fl->hs_pos[j - 1] > p) { fl->hs_pos[j] = fl->hs_pos[j - 1]; fl->hs_head[j] = fl->hs_head[j - 1]; j--; }
            fl->hs_pos[j] = p;
            fl->hs_head[j] = hh;
        }
    }
    free(e);
    free(st);
}

// The marks with the system measured from the train spacing when asked.
// The system: MEASURED when the run allows - the FM head switches, else the
// spacing of two or more V-sync trains - and the caller's lines only a
// hint for a run too short to measure (a 1-field window: one train, no
// head-switch pair). *lines_out is 0 when neither measured nor hinted
// (harrypm 2026-11-19: at a 1-field span the unmeasured default 625 was
// reported as found, the Spectro adopted it, re-requested a PAL-length
// window, measured 525, and flipped back and forth).
static size_t fl_find_system(gui_field_locate_t *fl, gui_field_mark_t *marks, size_t max, int *lines_out,
                             bool *measured_out) {
    const int hint = fl->auto_lines ? 0 : fl->lines_per_frame;
    int lines = fl->hs_lines ? fl->hs_lines : (hint ? hint : 625);
    bool measured = fl->hs_lines != 0;
    size_t n = fl_find(fl, lines, marks, max);
    if (!fl->hs_lines && n >= 2) {
        measured = true;
        // Median spacing of consecutive starts -> field rate -> system.
        double d[64];
        size_t k = 0;
        for (size_t i = 1; i < n && k < 64; i++) d[k++] = (double)(marks[i].pos - marks[i - 1].pos);
        for (size_t i = 1; i < k; i++) {
            double v = d[i];
            size_t j = i;
            while (j > 0 && d[j - 1] > v) { d[j] = d[j - 1]; j--; }
            d[j] = v;
        }
        const double period_s = d[k / 2] / fl->rate;
        const int by_spacing = period_s < 0.5 * (1.0 / 50.0 + 1001.0 / 60000.0) ? 525 : 625;
        if (by_spacing != lines) {
            lines = by_spacing;
            n = fl_find(fl, lines, marks, max);
        }
    }
    *lines_out = measured ? lines : hint;
    if (measured_out) *measured_out = measured;
    return n;
}

size_t gui_field_locate_finish(gui_field_locate_t *fl, gui_field_mark_t *marks, size_t max) {
    if (!fl || !marks || max == 0) return 0;
    fl->found_mode = 0;
    fl->found_lines = 0;
    fl->found_measured = false;
    if (max > 64) max = 64;
    if (fl->mode == GUI_FIELD_LOCATE_FM) fl_head_switches(fl);
    if (fl->hs_lines) { fl->found_lines = fl->hs_lines; fl->found_measured = true; }
    int lines = fl->lines_per_frame;
    bool meas = false;
    size_t n = fl_find_system(fl, marks, max, &lines, &meas);
    int score = fl_score(fl, lines, marks, n);
    fl->found_mode = n ? fl->mode : 0;
    fl->found_lines = lines;
    fl->found_measured = meas;
    if (fl->auto_mode && fl->alt) {
        gui_field_mark_t m2[64];
        int lines2 = lines;
        bool meas2 = false;
        const size_t n2 = fl_find_system(fl->alt, m2, max, &lines2, &meas2);
        const int score2 = fl_score(fl->alt, lines2, m2, n2);
        if (score2 > score) {
            memcpy(marks, m2, n2 * sizeof(m2[0]));
            n = n2;
            fl->found_mode = n2 ? GUI_FIELD_LOCATE_CVBS : 0;
            fl->found_lines = lines2;
            fl->found_measured = meas2;
        }
    }
    return n;
}

static size_t fl_find(gui_field_locate_t *fl, int lines, gui_field_mark_t *marks, size_t max) {
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
    const double field_rate = lines == 525 ? 60000.0 / 1001.0 : 50.0;
    const double line_pts = 2.0 / ((double)lines * field_rate) * fl->rate / (double)fl->decim;

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
        double b = train - (lines == 525 ? 3.0 * line_pts : 0.0);
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
    free(fl->env);
    fl->env = NULL;
    fl->env_cap = 0;
    if (fl->alt) {
        gui_field_locate_free(fl->alt);
        free(fl->alt);
        fl->alt = NULL;
    }
}
