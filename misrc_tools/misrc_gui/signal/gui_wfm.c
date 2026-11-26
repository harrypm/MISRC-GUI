/*
 * MISRC GUI - Composite Waveform Monitor (accumulator + levels)
 * See gui_wfm.h.
 */
#include "gui_wfm.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WFM_CELLS ((size_t)GUI_WFM_COLS * GUI_WFM_BINS)

void gui_wfm_levels(int system, gui_wfm_levels_t *lv) {
    if (!lv) return;
    if (system == GUI_WFM_SYS_NTSC) {
        // 10-bit: tip 16, blank 240, black 282 (7.5 IRE setup), white 800,
        // peak 1019 (SMPTE 170M; 714.3 mV per 100 IRE).
        lv->sync_tip = (16.0 - 240.0) / (800.0 - 240.0) * 100.0;    // -40
        lv->blanking = 0.0;
        lv->black = (282.0 - 240.0) / (800.0 - 240.0) * 100.0;      // 7.5
        lv->white = 100.0;
        lv->peak = (1019.0 - 240.0) / (800.0 - 240.0) * 100.0;      // 139.1
        lv->mv_per_ire = 7.143;
    } else {
        // PAL / SECAM 10-bit: tip 4, blank 256 (= black), white 844,
        // peak 1019 (700 mV per 100 IRE).
        lv->sync_tip = (4.0 - 256.0) / (844.0 - 256.0) * 100.0;     // -42.86 (-300 mV)
        lv->blanking = 0.0;
        lv->black = 0.0;
        lv->white = 100.0;
        lv->peak = (1019.0 - 256.0) / (844.0 - 256.0) * 100.0;      // 129.8
        lv->mv_per_ire = 7.0;
    }
}

double gui_wfm_burst_ire(int system) {
    // PAL: 3/14 of blanking..white (300 mV p-p); NTSC: 20 IRE (40 IRE p-p).
    if (system == GUI_WFM_SYS_PAL) return 100.0 * 3.0 / 14.0;
    if (system == GUI_WFM_SYS_NTSC) return 20.0;
    return 0.0;
}

void gui_wfm_display_range(int system, int channel, double *lo_ire, double *hi_ire) {
    gui_wfm_levels_t lv;
    gui_wfm_levels(system, &lv);
    double lo, hi;
    if (channel == GUI_WFM_CH_C) {
        lo = -GUI_WFM_C_RANGE_IRE;
        hi = GUI_WFM_C_RANGE_IRE;
    } else if (channel == GUI_WFM_CH_Y) {
        lo = -100.0 / lv.mv_per_ire;
        hi = lv.white + 100.0 / lv.mv_per_ire;
    } else {
        double span = lv.peak - lv.sync_tip;
        lo = lv.sync_tip - 0.05 * span;
        hi = lv.peak + 0.05 * span;
    }
    if (lo_ire) *lo_ire = lo;
    if (hi_ire) *hi_ire = hi;
}

// Modified Bessel I0 (power series; the Kaiser window).
static double wfm_i0(double z) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 32; k++) {
        t *= (z * 0.5) * (z * 0.5) / ((double)k * (double)k);
        s += t;
        if (t < 1e-12 * s) break;
    }
    return s;
}

// Fractional-delay taps: an 8-tap Kaiser-windowed sinc (beta 5) per phase,
// unit DC gain. Linear interpolation at the decode rate's ~4 samples per
// subcarrier cycle cut the chroma by up to 29% at half-sample phases (the
// Y+C trace read the PAL burst 10% low); this kernel holds every phase
// within 0.4% at fsc and 0.2% below fs/8.
static void wfm_build_taps(gui_wfm_t *w) {
    const double beta = 5.0, half = 0.5 * GUI_WFM_FD_TAPS;
    const double i0b = wfm_i0(beta);
    for (int p = 0; p <= GUI_WFM_FD_PHASES; p++) {
        const double f = (double)p / GUI_WFM_FD_PHASES;
        double h[GUI_WFM_FD_TAPS], sum = 0.0;
        for (int j = 0; j < GUI_WFM_FD_TAPS; j++) {
            const double x = (double)(j - (GUI_WFM_FD_TAPS / 2 - 1)) - f;   // taps -3..4
            const double r = x / half;
            const double sinc = fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            const double win = fabs(r) <= 1.0 ? wfm_i0(beta * sqrt(1.0 - r * r)) / i0b : 0.0;
            h[j] = sinc * win;
            sum += h[j];
        }
        for (int j = 0; j < GUI_WFM_FD_TAPS; j++) w->fd_taps[p][j] = (float)(h[j] / sum);
    }
}

bool gui_wfm_init(gui_wfm_t *w) {
    if (!w) return false;
    memset(w, 0, sizeof(*w));
    w->acc = (uint16_t *)calloc(WFM_CELLS, sizeof(uint16_t));
    w->mid = (uint16_t *)calloc(WFM_CELLS, sizeof(uint16_t));
    w->pub = (uint16_t *)calloc(WFM_CELLS, sizeof(uint16_t));
    atomic_flag_clear(&w->swap_lock);
    if (!w->acc || !w->mid || !w->pub) {
        gui_wfm_free(w);
        return false;
    }
    wfm_build_taps(w);
    atomic_init(&w->pub_ready, 0);
    atomic_init(&w->frames, 0);
    atomic_init(&w->want_range, GUI_WFM_RANGE_ACTIVE);
    atomic_init(&w->want_channel, GUI_WFM_CH_YC);
    return true;
}

void gui_wfm_free(gui_wfm_t *w) {
    if (!w) return;
    free(w->acc);
    free(w->mid);
    free(w->pub);
    w->acc = w->mid = w->pub = NULL;
}

void gui_wfm_reset(gui_wfm_t *w) {
    if (!w || !w->acc) return;
    memset(w->acc, 0, WFM_CELLS * sizeof(uint16_t));
    w->fields_in_acc = 0;
    w->acc_started = false;
    w->swing_valid = false;
    w->swing_sum = 0.0;
    w->swing_n = 0;
    w->have_blank = false;
}

// Mean of ring[a, b).
static double wfm_ring_mean(const int16_t *ring, unsigned mask, int64_t a, int64_t b) {
    int64_t s = 0;
    for (int64_t k = a; k < b; k++) s += ring[(uint64_t)k & mask];
    return (double)s / (double)(b - a);
}

// Frame start: take the UI's settings and lay out the time window.
static void wfm_start(gui_wfm_t *w, const gui_wfm_line_t *in) {
    gui_wfm_meta_t *m = &w->acc_meta;
    m->system = in->system;
    m->range = atomic_load(&w->want_range) == GUI_WFM_RANGE_FRAME ? GUI_WFM_RANGE_FRAME
                                                                  : GUI_WFM_RANGE_ACTIVE;
    const int ch = atomic_load(&w->want_channel);
    m->channel = (ch >= 0 && ch < GUI_WFM_CH_COUNT) ? ch : GUI_WFM_CH_YC;
    double t0, t1;
    if (m->range == GUI_WFM_RANGE_FRAME) {
        t0 = -GUI_WFM_FRAME_LEAD_US;
        t1 = in->line_us - GUI_WFM_FRAME_LEAD_US;
    } else {
        t0 = in->active_start_us;
        t1 = in->active_end_us;
    }
    int cols = (int)floor((t1 - t0) * in->rate * 1e-6);
    if (cols < 1) cols = 1;
    if (cols > GUI_WFM_COLS) cols = GUI_WFM_COLS;
    m->cols = cols;
    m->t0_us = t0;
    m->us_per_col = 1e6 / in->rate;
    m->rate = in->rate;
    gui_wfm_display_range(in->system, m->channel, &m->lo_ire, &m->hi_ire);
    m->lines = 0;
    w->acc_started = true;
}

void gui_wfm_add_line(gui_wfm_t *w, const gui_wfm_line_t *in) {
    if (!w || !w->acc || !in || !in->ring || !(in->rate > 1e6)) return;
    if (w->acc_started && in->system != w->acc_meta.system) gui_wfm_reset(w);
    if (w->acc_started && fabs(in->rate - w->acc_meta.rate) > 0.5) {
        memset(w->acc, 0, WFM_CELLS * sizeof(uint16_t));
        w->fields_in_acc = 0;
        w->acc_started = false;
    }
    if (!w->acc_started) wfm_start(w, in);

    // Clamp + gain references from this line's own sync: the tip interior
    // (1.0-3.5 us) and the back porch (burst end .. active start, the
    // decoder's DC-restore windows).
    const double r_us = in->rate * 1e-6;
    const int64_t t_a = in->ring0 + (int64_t)ceil(in->off0h + 1.0 * r_us);
    const int64_t t_b = in->ring0 + (int64_t)floor(in->off0h + 3.5 * r_us);
    const int64_t p_a = in->ring0 + (int64_t)ceil(in->off0h + in->burst_end_us * r_us) + 2;
    const int64_t p_b = in->ring0 + (int64_t)floor(in->off0h + in->active_start_us * r_us) - 2;
    if (t_b - t_a < 4 || p_b - p_a < 8) return;
    const double tip = wfm_ring_mean(in->ring, in->ring_mask, t_a, t_b);
    const double porch = wfm_ring_mean(in->ring, in->ring_mask, p_a, p_b);
    const double swing = porch - tip;
    const double ref = w->swing_valid ? w->swing_ref : in->swing_nom;
    if (!(ref > 0.0)) return;
    // Clamp: any line whose tip window holds a sync pulse (a full H sync
    // ~1.0x, a post-EQ line ~0.5x); a broad-pulse line (porch window on the
    // tip) keeps the previous line's blanking, as the decoder's DC restore.
    if (swing > 0.35 * ref && swing < 1.6 * ref) {
        w->blank = porch;
        w->have_blank = true;
    }
    // Gain: full H syncs of picture lines only.
    if (in->picture && swing > 0.6 * ref && swing < 1.4 * ref) {
        w->swing_sum += swing;
        w->swing_n++;
    }
    if (!w->swing_valid || !w->have_blank) return;

    const gui_wfm_meta_t *m = &w->acc_meta;
    if (m->range == GUI_WFM_RANGE_ACTIVE && !in->picture) return;

    gui_wfm_levels_t lv;
    gui_wfm_levels(m->system, &lv);
    const double k = -lv.sync_tip / w->swing_ref;                 // IRE per ring unit
    const double bpi = (double)GUI_WFM_BINS / (m->hi_ire - m->lo_ire);
    const bool chroma = m->channel == GUI_WFM_CH_C;
    const bool y_only = m->channel == GUI_WFM_CH_Y;
    const float a = (float)(k * bpi);
    // C: the band-pass removes the DC (no clamp offset), centred on 0 IRE.
    const float b = (float)(((chroma ? 0.0 : -w->blank * k) - m->lo_ire) * bpi);

    // The window on the column grid: column c covers t0 + c samples after
    // 0H, +-1 sample. Each line samples its columns at one offset inside
    // that slot (a golden-ratio sequence: evenly spread, no pattern),
    // resampled by the 8-tap fractional delay (taps -3..4).
    // Equivalent-time sampling: NTSC's 227.5 subcarrier cycles per line
    // otherwise sample every column at only two subcarrier phases 180 deg
    // apart on every line - each column showed +-A sin(theta) instead of the
    // chroma envelope (the bars drew crossing rails, the burst read 4.8 of
    // its 16 IRE) - where an analogue monitor's beam sweeps continuously.
    // The slot spans half a subcarrier cycle at the ~4x fsc decode rate,
    // which with the two line polarities covers every phase (+-0.5 sample
    // read the burst 13.9 of 16). +-1 sample is +-56 ns PAL / +-70 ns NTSC,
    // under one pixel on any panel narrower than the raster.
    // Y only: tbc's 4-tap average x[i-2..i+1] (the fs/4 notch: at the
    // panel's ~4x fsc decode rate it nulls the subcarrier).
    // C: [1 0 -4 0 6 0 -4 0 1] / 16 centred (the fs/4 band-pass squared).
    const int pad = chroma ? 4 : y_only ? 2 : 0;
    const int cols = m->cols;
    const int nval = cols + (chroma ? 8 : y_only ? 3 : 0);
    w->dither = w->dither + 0.6180339887498949;
    if (w->dither >= 1.0) w->dither -= 1.0;
    const double start = in->off0h + m->t0_us * r_us - (double)pad + 2.0 * (w->dither - 0.5);
    double fl = floor(start);
    int phase = (int)lrint((start - fl) * GUI_WFM_FD_PHASES);
    const int lead = GUI_WFM_FD_TAPS / 2 - 1;
    const int64_t g = in->ring0 + (int64_t)fl - lead;
    int16_t *raw = w->raw;
    float *rawf = w->rawf;
    float *val = w->val;
    // The ring window in at most two contiguous pieces (no per-sample wrap).
    const size_t ring_len = (size_t)in->ring_mask + 1u;
    const size_t first = (size_t)((uint64_t)g & in->ring_mask);
    const size_t need = (size_t)nval + GUI_WFM_FD_TAPS;
    const size_t run = need < ring_len - first ? need : ring_len - first;
    memcpy(raw, in->ring + first, run * sizeof(int16_t));
    if (run < need) memcpy(raw + run, in->ring, (need - run) * sizeof(int16_t));
    for (size_t i = 0; i < need; i++) rawf[i] = (float)raw[i];
    const float *h = w->fd_taps[phase];
    const float h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], h5 = h[5], h6 = h[6], h7 = h[7];
    for (int i = 0; i < nval; i++) {
        const float *x = rawf + i;
        val[i] = h0 * x[0] + h1 * x[1] + h2 * x[2] + h3 * x[3] +
                 h4 * x[4] + h5 * x[5] + h6 * x[6] + h7 * x[7];
    }
    if (y_only) {
        for (int c = 0; c < cols; c++) val[c] = 0.25f * (val[c] + val[c + 1] + val[c + 2] + val[c + 3]);
    } else if (chroma) {
        for (int c = 0; c < cols; c++) {
            val[c] = 0.0625f * (val[c] + val[c + 8]) - 0.25f * (val[c + 2] + val[c + 6]) +
                     0.375f * val[c + 4];
        }
    }

    // Row-major raster: neighbouring columns at one level share cache lines.
    uint16_t *acc = w->acc;
    for (int c = 0; c < cols; c++) {
        const float bf = val[c] * a + b;
        if (bf >= 0.0f && bf < (float)GUI_WFM_BINS) acc[(size_t)(int)bf * GUI_WFM_COLS + (size_t)c]++;
    }
    w->acc_meta.lines++;
}

void gui_wfm_add_decoded_line(gui_wfm_t *w, const gui_wfm_decoded_line_t *in) {
    if (!w || !w->acc || !in || !in->ire || in->count < 2 || !(in->rate > 1e6) || !(in->dt_us > 0.0)) return;
    if (w->acc_started && in->system != w->acc_meta.system) gui_wfm_reset(w);
    if (w->acc_started && fabs(in->rate - w->acc_meta.rate) > 0.5) {
        memset(w->acc, 0, WFM_CELLS * sizeof(uint16_t));
        w->fields_in_acc = 0;
        w->acc_started = false;
    }
    if (!w->acc_started) {
        gui_wfm_line_t l;
        memset(&l, 0, sizeof(l));
        l.rate = in->rate;
        l.line_us = in->line_us;
        l.active_start_us = in->active_start_us;
        l.active_end_us = in->active_end_us;
        l.system = in->system;
        wfm_start(w, &l);
    }
    const gui_wfm_meta_t *m = &w->acc_meta;
    if (m->range == GUI_WFM_RANGE_ACTIVE && !in->picture) return;
    const bool chroma = m->channel == GUI_WFM_CH_C;
    const bool y_only = m->channel == GUI_WFM_CH_Y;
    const bool colour = in->u && in->v && !y_only;
    const double bpi = (double)GUI_WFM_BINS / (m->hi_ire - m->lo_ire);
    // The same per-line column dither as the CVBS path (equivalent-time
    // sampling of the rebuilt subcarrier: the envelope, not two phases).
    w->dither = w->dither + 0.6180339887498949;
    if (w->dither >= 1.0) w->dither -= 1.0;
    const double jitter = 2.0 * (w->dither - 0.5) * m->us_per_col;
    const double wsc = 2.0 * 3.14159265358979323846 * in->fsc_hz * 1e-6;   // rad per us
    const double last = (double)(in->count - 1);
    // Subcarrier by rotation: one sin/cos per line, not per column.
    const double th0 = in->phase0 + wsc * (m->t0_us + jitter), dth = wsc * m->us_per_col;
    double sr = sin(th0), cr = cos(th0);
    const double sd = sin(dth), cd = cos(dth);
    uint16_t *acc = w->acc;
    for (int c = 0; c < m->cols; c++) {
        const float s_now = (float)sr, c_now = (float)cr;
        const double nsr = sr * cd + cr * sd;
        cr = cr * cd - sr * sd;
        sr = nsr;
        const double t = m->t0_us + c * m->us_per_col + jitter;
        const double f = (t - in->t0_us) / in->dt_us;
        if (f < 0.0 || f > last) continue;
        int i = (int)f;
        if (i >= in->count - 1) i = in->count - 2;
        const float fr = (float)(f - i);
        const float y = in->ire[i] + (in->ire[i + 1] - in->ire[i]) * fr;
        float val = chroma ? 0.0f : y;
        if (colour) {
            const float u = in->u[i] + (in->u[i + 1] - in->u[i]) * fr;
            const float v = in->v[i] + (in->v[i + 1] - in->v[i]) * fr;
            val += 100.0f * (u * s_now + v * c_now);
        }
        const float bf = (float)((val - m->lo_ire) * bpi);
        if (bf >= 0.0f && bf < (float)GUI_WFM_BINS) acc[(size_t)(int)bf * GUI_WFM_COLS + (size_t)c]++;
    }
    w->acc_meta.lines++;
}

static void wfm_field_swing(gui_wfm_t *w) {
    if (w->swing_n >= 16) {
        double m = w->swing_sum / (double)w->swing_n;
        w->swing_ref = w->swing_valid ? 0.5 * (w->swing_ref + m) : m;
        w->swing_valid = true;
    }
    w->swing_sum = 0.0;
    w->swing_n = 0;
}

// Publish acc (latest wins) and start a fresh accumulation.
static void wfm_publish(gui_wfm_t *w) {
    w->fields_in_acc = 0;
    if (w->acc_meta.lines > 0) {
        atomic_fetch_add(&w->frames, 1u);
        while (atomic_flag_test_and_set_explicit(&w->swap_lock, memory_order_acquire)) {}
        uint16_t *swap = w->mid;
        w->mid = w->acc;
        w->acc = swap;
        w->mid_meta = w->acc_meta;
        atomic_store_explicit(&w->pub_ready, 1, memory_order_release);
        atomic_flag_clear_explicit(&w->swap_lock, memory_order_release);
    }
    memset(w->acc, 0, WFM_CELLS * sizeof(uint16_t));
    w->acc_started = false;   // The next line re-reads the UI's settings
}

void gui_wfm_end_field(gui_wfm_t *w) {
    if (!w || !w->acc) return;
    wfm_field_swing(w);
    if (!w->acc_started) return;
    // tbc plots a frame: publish every second field.
    if (++w->fields_in_acc < 2) return;
    wfm_publish(w);
}

void gui_wfm_end_field_locked(gui_wfm_t *w, int parity, int field1_parity, bool single_field) {
    if (!w || !w->acc) return;
    wfm_field_swing(w);
    if (!w->acc_started) return;
    if (single_field) {
        wfm_publish(w);
        return;
    }
    if (w->fields_in_acc == 0) {
        if (parity != field1_parity) {
            // Started on field 2: not a frame. Drop it; field 1 starts one.
            memset(w->acc, 0, WFM_CELLS * sizeof(uint16_t));
            w->acc_started = false;
            return;
        }
        w->fields_in_acc = 1;
        return;
    }
    if (parity == field1_parity) {
        // Two field 1s in a row (a field was lost): restart on this one.
        memset(w->acc, 0, WFM_CELLS * sizeof(uint16_t));
        w->acc_started = false;
        w->fields_in_acc = 0;
        return;
    }
    wfm_publish(w);   // field 1 + field 2 of one frame
}

bool gui_wfm_take(gui_wfm_t *w, uint16_t *dst, gui_wfm_meta_t *meta) {
    if (!w || !w->pub || !dst) return false;
    if (atomic_load_explicit(&w->pub_ready, memory_order_acquire) == 0) return false;
    while (atomic_flag_test_and_set_explicit(&w->swap_lock, memory_order_acquire)) {}
    uint16_t *swap = w->pub;
    w->pub = w->mid;
    w->mid = swap;
    w->pub_meta = w->mid_meta;
    atomic_store_explicit(&w->pub_ready, 0, memory_order_release);
    atomic_flag_clear_explicit(&w->swap_lock, memory_order_release);
    memcpy(dst, w->pub, WFM_CELLS * sizeof(uint16_t));
    if (meta) *meta = w->pub_meta;
    return true;
}
