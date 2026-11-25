/* Composite waveform monitor contract test.
 *
 * Drives the Test Gen composite channel (PAL / SECAM = EBU 100/0/75/0 bars,
 * NTSC = SMPTE SD bars) through the EXACT decode chain the WFM panel uses
 * (gui_cvbs_feed: fixed-rate soxr resampler + CVBS decoder, scope-only) at
 * the simulated device's 40 MSPS / 12-bit levels, and checks the raster:
 *   - Y only, Active video: every bar's luma lands at its level (PAL/SECAM
 *     0 / 8.6 / 22.4 / 31.0 / 44.0 / 52.6 / 66.5 / 100 IRE, NTSC top bars
 *     7.5 + 92.5 x Y) within 1 IRE, and the bar columns are tight (the
 *     per-line back-porch clamp removes the generator's AC-coupling bump);
 *   - Y+C, Whole frame: the sync tip and blanking land on the normative
 *     levels (PAL -42.86 IRE, NTSC -40 IRE; 0 IRE) and the sync leading
 *     edge's 50% point sits at 0H (t = 0 us) within 0.1 us. */
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/signal/gui_testgen.h"
#include <assert.h>

// Headless stubs (see gui_cvbs_test.c): the only raylib symbols the decode
// path references.
void UnloadTexture(Texture2D texture) { (void)texture; }
void TraceLog(int logLevel, const char *text, ...) { (void)logLevel; (void)text; }

#define CELLS ((size_t)GUI_WFM_COLS * GUI_WFM_BINS)
#define RATE 40000000u

static int failures;

typedef struct {
    uint64_t *sum;
    gui_wfm_meta_t meta;
    int frames;
} raster_t;

static double bin_ire(const gui_wfm_meta_t *m, double bin) {
    return m->lo_ire + (bin + 0.5) * (m->hi_ire - m->lo_ire) / GUI_WFM_BINS;
}

// Row-major raster: bin b, column c.
static inline uint64_t cell(const raster_t *r, int c, int b) {
    return r->sum[(size_t)b * GUI_WFM_COLS + (size_t)c];
}

// Column at time t (us after 0H).
static int col_at(const gui_wfm_meta_t *m, double t_us) {
    return (int)lrint((t_us - m->t0_us) / m->us_per_col);
}

// Mode of a column, the count-weighted mean within +-3 IRE of it, and the
// share of the column's counts within +-tight IRE of that mean.
static double col_level(const raster_t *r, int c, double tight, double *share) {
    int mode = 0;
    uint64_t total = 0;
    for (int b = 0; b < GUI_WFM_BINS; b++) {
        total += cell(r, c, b);
        if (cell(r, c, b) > cell(r, c, mode)) mode = b;
    }
    double mode_ire = bin_ire(&r->meta, mode), n = 0, s = 0;
    for (int b = 0; b < GUI_WFM_BINS; b++) {
        double ire = bin_ire(&r->meta, b);
        if (fabs(ire - mode_ire) > 3.0) continue;
        n += (double)cell(r, c, b);
        s += (double)cell(r, c, b) * ire;
    }
    double mean = n > 0 ? s / n : mode_ire;
    if (share) {
        double in = 0;
        for (int b = 0; b < GUI_WFM_BINS; b++)
            if (fabs(bin_ire(&r->meta, b) - mean) <= tight) in += (double)cell(r, c, b);
        *share = total ? in / (double)total : 0;
    }
    return mean;
}

// Count-weighted mean of a column's bins within +-half IRE of centre (a
// level carrying a subcarrier: the mode sits on a sine peak, the mean on
// the level).
static double col_mean(const raster_t *r, int c, double centre, double half) {
    double n = 0, s = 0;
    for (int b = 0; b < GUI_WFM_BINS; b++) {
        double ire = bin_ire(&r->meta, b);
        if (fabs(ire - centre) > half) continue;
        n += (double)cell(r, c, b);
        s += (double)cell(r, c, b) * ire;
    }
    return n > 0 ? s / n : 1e9;
}

// 1% / 99% percentiles of a column's counts within [lo, hi] IRE.
static void col_pct(const raster_t *r, int c, double lo, double hi, double *p1, double *p99) {
    uint64_t total = 0, acc = 0;
    for (int b = 0; b < GUI_WFM_BINS; b++) {
        double ire = bin_ire(&r->meta, b);
        if (ire >= lo && ire <= hi) total += cell(r, c, b);
    }
    *p1 = *p99 = 0.0;
    bool got1 = false;
    for (int b = 0; b < GUI_WFM_BINS; b++) {
        double ire = bin_ire(&r->meta, b);
        if (ire < lo || ire > hi) continue;
        acc += cell(r, c, b);
        if (!got1 && acc * 100 >= total) { *p1 = ire; got1 = true; }
        if (acc * 100 >= total * 99) { *p99 = ire; break; }
    }
}

static void check_amp(const char *sys, const char *what, double got, double want, double rel) {
    bool ok = fabs(got / want - 1.0) <= rel;
    printf("%-5s %-20s %7.2f IRE peak (want %6.2f, tol %.0f%%) %s\n", sys, what, got, want,
           rel * 100.0, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static void check_level(const char *sys, const char *what, double got, double want, double tol,
                        double share, double min_share) {
    bool ok = fabs(got - want) <= tol && share >= min_share;
    printf("%-5s %-20s %+7.2f IRE (want %+7.2f, tol %.1f) tight %5.1f%% %s\n",
           sys, what, got, want, tol, share * 100.0, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static void collect(int system_select, int channel, int range, raster_t *r) {
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed);
    gui_wfm_t wfm;
    assert(gui_wfm_init(&wfm));
    atomic_store(&wfm.want_channel, channel);
    atomic_store(&wfm.want_range, range);
    gui_cvbs_feed_set_system(feed, system_select);
    gui_cvbs_feed_decoder(feed)->wfm = &wfm;
    gui_cvbs_feed_set_pixels(feed, false);   // the WFM panel's own feed

    gui_testgen_system_t gen = system_select == 2 ? GUI_TESTGEN_SECAM
                             : system_select == 1 ? GUI_TESTGEN_NTSC : GUI_TESTGEN_PAL;
    size_t frame_n = gui_testgen_frame_samples(gen, RATE);
    int16_t *frame = malloc(frame_n * sizeof(int16_t));
    int16_t *chunk = malloc(65536 * sizeof(int16_t));
    uint16_t *grid = malloc(CELLS * sizeof(uint16_t));
    r->sum = calloc(CELLS, sizeof(uint64_t));
    r->frames = 0;
    assert(frame && chunk && grid && r->sum);

    const int frames = 14, warmup_frames = 6;
    for (int f = 0; f < frames; f++) {
        assert(gui_testgen_frame(gen, frame, frame_n, RATE, (uint32_t)f) == frame_n);
        for (size_t at = 0; at < frame_n; at += 65536) {
            size_t n = frame_n - at < 65536 ? frame_n - at : 65536;
            for (size_t i = 0; i < n; i++) {
                chunk[i] = (int16_t)(((double)frame[at + i] / 32767.0) * 2170.0);
            }
            gui_cvbs_feed_process(feed, chunk, n, RATE);
            gui_wfm_meta_t meta;
            if (gui_wfm_take(&wfm, grid, &meta) && f >= warmup_frames) {
                assert(meta.channel == channel && meta.range == range);
                r->meta = meta;
                for (size_t i = 0; i < CELLS; i++) r->sum[i] += grid[i];
                r->frames++;
            }
        }
    }
    gui_cvbs_feed_decoder(feed)->wfm = NULL;
    gui_cvbs_feed_destroy(feed);
    gui_wfm_free(&wfm);
    free(frame);
    free(chunk);
    free(grid);
}

static void run(int system_select) {
    const bool ntsc = system_select == 1;
    const bool secam = system_select == 2;
    const char *sys = system_select == 2 ? "SECAM" : ntsc ? "NTSC" : "PAL";
    gui_wfm_levels_t lv;
    gui_wfm_levels(system_select, &lv);

    // --- Y only, Active video: bar luma.
    raster_t r;
    collect(system_select, GUI_WFM_CH_Y, GUI_WFM_RANGE_ACTIVE, &r);
    printf("%s: Y/Active %d frames, %u lines/frame, %d cols from %.2f us\n", sys, r.frames,
           r.meta.lines, r.meta.cols, r.meta.t0_us);
    if (r.frames < 3 || r.meta.lines < (ntsc ? 400u : 500u)) {
        printf("%s: too few frames/lines reached the monitor - FAIL\n", sys);
        failures++;
    } else if (ntsc) {
        static const char *names[7] = { "75% gray", "yellow", "cyan", "green", "magenta", "red", "blue" };
        static const double rgb[7][3] = { { .75, .75, .75 }, { .75, .75, 0 }, { 0, .75, .75 },
                                          { 0, .75, 0 }, { .75, 0, .75 }, { .75, 0, 0 }, { 0, 0, .75 } };
        for (int i = 0; i < 7; i++) {
            double y = 0.299 * rgb[i][0] + 0.587 * rgb[i][1] + 0.114 * rgb[i][2];
            double t = 9.45 + (i + 0.5) * 52.9 / 7.0;
            double share;
            double got = col_level(&r, col_at(&r.meta, t), 2.0, &share);
            char what[32];
            snprintf(what, sizeof(what), "bar %s", names[i]);
            // The SMPTE top bars are 67% of the picture lines.
            check_level(sys, what, got, 7.5 + 92.5 * y, 1.0, share, 0.60);
        }
    } else {
        static const char *names[8] = { "white", "yellow", "cyan", "green", "magenta", "red", "blue", "black" };
        static const double rgb[8][3] = { { 1, 1, 1 }, { .75, .75, 0 }, { 0, .75, .75 }, { 0, .75, 0 },
                                          { .75, 0, .75 }, { .75, 0, 0 }, { 0, 0, .75 }, { 0, 0, 0 } };
        for (int i = 0; i < 8; i++) {
            double y = 0.299 * rgb[i][0] + 0.587 * rgb[i][1] + 0.114 * rgb[i][2];
            double t = 10.5 + (i + 0.5) * 6.5;
            double share;
            double got = col_level(&r, col_at(&r.meta, t), 2.0, &share);
            char what[32];
            snprintf(what, sizeof(what), "bar %s", names[i]);
            // SECAM: tbc's 4-tap Y average nulls fs/4 (4.47 MHz); the lower
            // FM carriers of the D'B-negative bars (yellow ~3.9 MHz) pass at
            // about -16 dB, so those columns carry a few IRE of carrier
            // (each column sees every carrier phase: about half the counts
            // sit within +-2 IRE). The level (mean) is still exact.
            check_level(sys, what, got, 100.0 * y, 1.0, share, secam ? 0.45 : 0.90);
        }
    }
    free(r.sum);

    // --- Y+C, Whole frame: sync tip, blanking and the 0H edge.
    collect(system_select, GUI_WFM_CH_YC, GUI_WFM_RANGE_FRAME, &r);
    printf("%s: Y+C/Frame %d frames, %u lines/frame, %d cols from %.2f us\n", sys, r.frames,
           r.meta.lines, r.meta.cols, r.meta.t0_us);
    if (r.frames < 3) {
        printf("%s: too few frames reached the monitor - FAIL\n", sys);
        failures++;
    } else {
        double share;
        double tip = col_level(&r, col_at(&r.meta, 2.35), 1.0, &share);
        check_level(sys, "sync tip (2.35 us)", tip, lv.sync_tip, 0.5, share, 0.80);
        double porch_t = ntsc ? 8.6 : 9.2;
        if (secam) {
            // SECAM's FM carrier runs through the back porch: the level is
            // the carrier's mean over several cycles (one column reads the
            // line-to-line 0/0/180 phase pattern's residual, -2 IRE) with the
            // VBI broad pulses excluded.
            double acc = 0;
            int nc = 0;
            for (int c = col_at(&r.meta, 8.2); c <= col_at(&r.meta, 10.2); c++, nc++)
                acc += col_mean(&r, c, 0.0, 20.0);
            double blank = acc / nc;
            check_level(sys, "back porch (mean)", blank, 0.0, 0.5, 1.0, 0.0);
        } else {
            double blank = col_level(&r, col_at(&r.meta, porch_t), 1.0, &share);
            check_level(sys, "back porch", blank, 0.0, 0.5, share, 0.80);
        }
        // The leading edge's 50% point: the first column whose mean (over
        // the tip..blanking range; each line samples its own offset inside
        // the column slot) is at or below half the tip level, interpolated
        // against the column before it.
        double half = 0.5 * lv.sync_tip, edge = 1e9;
        double prev = col_mean(&r, col_at(&r.meta, -0.8), half, 30.0);
        for (int c = col_at(&r.meta, -0.8) + 1; c <= col_at(&r.meta, 0.8); c++) {
            double v = col_mean(&r, c, half, 30.0);
            if (prev > half && v <= half) {
                double frac = (prev - half) / (prev - v);
                edge = r.meta.t0_us + (c - 1 + frac) * r.meta.us_per_col;
                break;
            }
            prev = v;
        }
        bool ok = fabs(edge) <= 0.1;
        printf("%-5s sync edge 50%%        %+7.3f us (want 0H, tol 0.100) %s\n", sys, edge,
               ok ? "ok" : "FAIL");
        if (!ok) failures++;
        if (!secam) {
            // Burst amplitude (VBI sync-level lines excluded): the
            // generator's burst (PAL 3/14 x 100 IRE, NTSC 20 IRE) times its
            // chroma chain gain (gui_testgen.c HT_CHROMA_GAIN_PAL / _NTSC).
            double p1, p99;
            col_pct(&r, col_at(&r.meta, ntsc ? 6.55 : 6.725), -30.0, 30.0, &p1, &p99);
            double want = ntsc ? 20.0 * 0.80 : 100.0 * 3.0 / 14.0 * 0.79;
            check_amp(sys, "burst", 0.5 * (p99 - p1), want, 0.03);
        }
    }
    free(r.sum);

    // --- Y+C, Active video (PAL: full-field bars): bar chroma amplitude.
    // The column resampler must hold the subcarrier at every sub-sample
    // phase (linear interpolation read these bars 6% low).
    if (!ntsc && !secam) {
        collect(system_select, GUI_WFM_CH_YC, GUI_WFM_RANGE_ACTIVE, &r);
        static const char *names[2] = { "yellow", "cyan" };
        static const double rgb[2][3] = { { .75, .75, 0 }, { 0, .75, .75 } };
        for (int i = 0; i < 2; i++) {
            double y = 0.299 * rgb[i][0] + 0.587 * rgb[i][1] + 0.114 * rgb[i][2];
            double u = 0.493 * (rgb[i][2] - y), v = 0.877 * (rgb[i][0] - y);
            double p1, p99;
            col_pct(&r, col_at(&r.meta, 10.5 + (i + 1.5) * 6.5), -60.0, 160.0, &p1, &p99);
            char what[32];
            snprintf(what, sizeof(what), "bar %s chroma", names[i]);
            check_amp(sys, what, 0.5 * (p99 - p1), 100.0 * hypot(u, v) * 0.79, 0.03);
        }
        free(r.sum);
    }

    // --- C (chroma band-pass), Active video (PAL full-field bars): the bar
    // chroma at full amplitude around 0 IRE (no luma: the band-pass zero at
    // DC), the white bar empty.
    if (!ntsc && !secam) {
        collect(system_select, GUI_WFM_CH_C, GUI_WFM_RANGE_ACTIVE, &r);
        static const char *names[2] = { "yellow", "cyan" };
        static const double rgb[2][3] = { { .75, .75, 0 }, { 0, .75, .75 } };
        for (int i = 0; i < 2; i++) {
            double y = 0.299 * rgb[i][0] + 0.587 * rgb[i][1] + 0.114 * rgb[i][2];
            double u = 0.493 * (rgb[i][2] - y), v = 0.877 * (rgb[i][0] - y);
            int c = col_at(&r.meta, 10.5 + (i + 1.5) * 6.5);
            double p1, p99;
            col_pct(&r, c, -70.0, 70.0, &p1, &p99);
            char what[32];
            snprintf(what, sizeof(what), "C bar %s", names[i]);
            check_amp(sys, what, 0.5 * (p99 - p1), 100.0 * hypot(u, v) * 0.79, 0.04);
            snprintf(what, sizeof(what), "C bar %s mean", names[i]);
            check_level(sys, what, col_mean(&r, c, 0.0, 70.0), 0.0, 1.0, 1.0, 0.0);
        }
        double share;
        double w = col_level(&r, col_at(&r.meta, 10.5 + 0.5 * 6.5), 2.0, &share);
        check_level(sys, "C white bar (none)", w, 0.0, 1.0, share, 0.95);
        free(r.sum);
    }
    // --- C, Whole frame: the burst at its amplitude (PAL / NTSC).
    if (!secam) {
        collect(system_select, GUI_WFM_CH_C, GUI_WFM_RANGE_FRAME, &r);
        double p1, p99;
        col_pct(&r, col_at(&r.meta, ntsc ? 6.55 : 6.725), -70.0, 70.0, &p1, &p99);
        double want = ntsc ? 20.0 * 0.80 : 100.0 * 3.0 / 14.0 * 0.79;
        check_amp(sys, "C burst", 0.5 * (p99 - p1), want, 0.04);
        free(r.sum);
    }
}

// Field / frame lock and the latest-wins hand-off (harrypm 2026-11-19: fix
// pause and field/frame lock of the WFM). One stand-in "field" = one line
// marked with an id in cell 0.
static void lock_field(gui_wfm_t *w, int parity, uint16_t id, bool single) {
    if (!w->acc_started) {
        w->acc_meta.lines = 0;
        w->acc_started = true;
    }
    w->acc_meta.lines++;
    w->acc[0] = (uint16_t)(w->acc[0] * 16u + id);
    gui_wfm_end_field_locked(w, parity, 0, single);
}

static void check_lock_contract(void) {
    gui_wfm_t w;
    assert(gui_wfm_init(&w));
    uint16_t *grid = malloc(CELLS * sizeof(uint16_t));
    gui_wfm_meta_t meta;
    assert(grid);
    int bad = 0;
    // Starts on field 2 (parity 1): no frame.
    lock_field(&w, 1, 1, false);
    if (atomic_load(&w.frames) != 0 || gui_wfm_take(&w, grid, &meta)) bad++;
    // Field 1 + field 2: one frame holding both (ids 2 then 3).
    lock_field(&w, 0, 2, false);
    lock_field(&w, 1, 3, false);
    if (!gui_wfm_take(&w, grid, &meta) || meta.lines != 2 || grid[0] != 2 * 16 + 3) bad++;
    // Two field 1s in a row: the frame restarts on the second.
    lock_field(&w, 0, 4, false);
    lock_field(&w, 0, 5, false);
    lock_field(&w, 0, 6, false);
    lock_field(&w, 1, 7, false);
    if (!gui_wfm_take(&w, grid, &meta) || grid[0] != 6 * 16 + 7) bad++;
    // Latest wins: two frames before the renderer takes -> it gets the
    // second (a paused playhead's frame is never the one dropped).
    lock_field(&w, 0, 8, false);
    lock_field(&w, 1, 9, false);
    lock_field(&w, 0, 10, false);
    lock_field(&w, 1, 11, false);
    if (!gui_wfm_take(&w, grid, &meta) || grid[0] != 10 * 16 + 11) bad++;
    if (gui_wfm_take(&w, grid, &meta)) bad++;   // nothing newer waits
    // Field view: every field alone.
    lock_field(&w, 1, 12, true);
    if (!gui_wfm_take(&w, grid, &meta) || meta.lines != 1 || grid[0] != 12) bad++;
    printf("WFM lock: field 2 start dropped, f1+f2 frames, restart on f1, latest wins, single field: %s\n",
           bad ? "FAIL" : "ok");
    if (bad) failures++;
    gui_wfm_free(&w);
    free(grid);
}

int main(void) {
    check_lock_contract();
    run(0);
    run(1);
    run(2);
    if (failures) {
        printf("gui_wfm_test: %d FAILED\n", failures);
        return 1;
    }
    printf("gui_wfm_test: all checks passed\n");
    return 0;
}
