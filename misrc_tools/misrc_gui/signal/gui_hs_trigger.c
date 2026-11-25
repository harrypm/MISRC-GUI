/*
 * MISRC GUI - head-switch trigger (Waveform "HS" trigger mode)
 *
 * harrypm 2026-11-19: "I want this lock mode to be like a sync trigger mode
 * so I can lock onto the envelope properly while playing also". On tape RF
 * the heads hand over once per field and the RF envelope steps there (the
 * paused Waveform's HS lock / gui_field_locate head switches). This finds
 * that step in ONE display frame, without the fold across frames: the
 * envelope (mean |x - mean| per microsecond), the step between the 6 us
 * either side of each point relative to the level, and the largest step
 * in the search range - accepted only well above the frame's typical step
 * (picture content and dropouts make smaller ones) and refined to the
 * sharpest 2 us edge. Frames without a switch return -1: the Waveform
 * holds its last triggered trace, as a scope does between triggers.
 */
#include "gui_hs_trigger.h"
#include <math.h>
#include <stdlib.h>

static int hs_cmp_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

ssize_t gui_hs_trigger_find(const int16_t *buf, size_t count, size_t min_index,
                            size_t max_index, uint32_t rate_hz, float ratio) {
    if (!buf || count < 64 || rate_hz < 1000000u) return -1;
    if (max_index > count) max_index = count;
    const size_t per = (size_t)(rate_hz / 1000000u);         // samples per us
    const size_t m = count / per;
    const int w = 6;                                         // us each side
    if (m < (size_t)(4 * w + 16)) return -1;
    double mean = 0.0;
    for (size_t i = 0; i < m * per; i++) mean += buf[i];
    mean /= (double)(m * per);
    float *e = (float *)malloc(m * sizeof(float));
    float *st = (float *)calloc(m, sizeof(float));
    if (!e || !st) { free(e); free(st); return -1; }
    for (size_t i = 0; i < m; i++) {
        double s = 0.0;
        const int16_t *p = buf + i * per;
        for (size_t k = 0; k < per; k++) s += fabs((double)p[k] - mean);
        e[i] = (float)(s / (double)per);
    }
    for (size_t i = (size_t)w; i + (size_t)w < m; i++) {
        double a = 0.0, b = 0.0;
        for (int k = 0; k < w; k++) { a += e[i - 1 - (size_t)k]; b += e[i + (size_t)k]; }
        a /= w; b /= w;
        st[i] = (float)(fabs(b - a) / (0.5 * (a + b) + 1e-9));
    }
    // The frame's typical step (median), the largest in the search range.
    float *sorted = (float *)malloc(m * sizeof(float));
    if (!sorted) { free(e); free(st); return -1; }
    size_t ns = 0;
    for (size_t i = (size_t)w; i + (size_t)w < m; i++) sorted[ns++] = st[i];
    qsort(sorted, ns, sizeof(float), hs_cmp_float);
    const float med = ns ? sorted[ns / 2] : 0.0f;
    free(sorted);
    const size_t lo = min_index / per, hi = max_index / per;
    size_t best = 0;
    float best_v = 0.0f;
    // A head switch moves the envelope to a new level that STAYS (the other
    // head's output); a dropout dips and recovers. Only steps whose level
    // change holds over 6..30 us either side count (measured on the Daimler
    // NTSC RF: most false triggers were dips that came back).
    const size_t sw = 30;
    for (size_t i = lo > sw ? lo : sw; i < hi && i + sw < m; i++) {
        if (st[i] <= best_v) continue;
        double a = 0.0, b = 0.0;
        for (size_t k = (size_t)w; k < sw; k++) { a += e[i - 1 - k]; b += e[i + k]; }
        a /= (double)(sw - (size_t)w);
        b /= (double)(sw - (size_t)w);
        const double held = fabs(b - a) / (0.5 * (a + b) + 1e-9);
        if (held < 0.6 * st[i]) continue;
        best_v = st[i];
        best = i;
    }
    ssize_t out = -1;
    if (best && med > 0.0f && best_v >= ratio * med) {
        // Refine: the sharpest 2 us step within +-6 us, to the sample.
        size_t fine = best;
        float fine_v = 0.0f;
        for (size_t i = best > 8 ? best - 6 : 2; i <= best + 6 && i + 2 < m; i++) {
            const float v = fabsf((e[i] + e[i + 1]) - (e[i - 1] + e[i - 2]));
            if (v > fine_v) { fine_v = v; fine = i; }
        }
        out = (ssize_t)(fine * per);
        if ((size_t)out < min_index) out = (ssize_t)min_index;
        if ((size_t)out >= max_index) out = -1;
    }
    free(e);
    free(st);
    return out;
}
