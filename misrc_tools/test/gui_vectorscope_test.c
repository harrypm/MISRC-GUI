/* Composite vectorscope contract test.
 *
 * Drives the Test Gen composite channel (NTSC = SMPTE SD bars, PAL = 75%
 * bars) through the EXACT panel decode chain the Vectorscope panel uses
 * (gui_cvbs_feed: fixed-rate soxr resampler + CVBS decoder) at the
 * simulated device's 40 MSPS / 12-bit levels, and checks that the
 * burst-locked chroma lands on the graticule targets the panel draws:
 *   - every 75% bar (B, G, Cy, R, Mg, Yl) for PAL and NTSC,
 *   - the SMPTE -I and +Q patches on their axes (NTSC),
 * within 1 degree of hue and 3% of amplitude, and
 *   - SECAM: the decoded D'B/D'R (no burst) on the PAL 75% targets times the
 *     generator's 0.75 deviation (its real-card chain gain), within 4 deg
 *     and 8% (the SECAM demod's own error; the generator's FM plateaus are
 *     exact to 10 kHz, gui_testgen_cvbs_test).
 * The feed runs the Vector panel's scope-only mode (no picture pass). */
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/signal/gui_testgen.h"
#include <assert.h>

// Headless stubs (see gui_cvbs_test.c): the only raylib symbols the decode
// path references.
void UnloadTexture(Texture2D texture) { (void)texture; }
void TraceLog(int logLevel, const char *text, ...) { (void)logLevel; (void)text; }

#define CELLS ((size_t)GUI_VSCOPE_GRID * GUI_VSCOPE_GRID)
#define RATE 40000000u

static int failures;

// Centroid of the hits within `radius` plot units of (tu, tv).
static double cluster(const uint64_t *grid, double tu, double tv, double radius,
                      double *cu, double *cv) {
    double gx, gy;
    gui_vectorscope_plot_to_grid(tu, tv, &gx, &gy);
    double cell = GUI_VSCOPE_FULL_SCALE / (0.5 * GUI_VSCOPE_GRID);
    int r = (int)(radius / cell) + 1;
    double n = 0, su = 0, sv = 0;
    for (int y = (int)gy - r; y <= (int)gy + r; y++) {
        for (int x = (int)gx - r; x <= (int)gx + r; x++) {
            if (x < 0 || y < 0 || x >= GUI_VSCOPE_GRID || y >= GUI_VSCOPE_GRID) continue;
            double u = ((double)x + 0.5) * cell - GUI_VSCOPE_FULL_SCALE;
            double v = GUI_VSCOPE_FULL_SCALE - ((double)y + 0.5) * cell;
            if (hypot(u - tu, v - tv) > radius) continue;
            double w = (double)grid[(size_t)y * GUI_VSCOPE_GRID + x];
            n += w; su += w * u; sv += w * v;
        }
    }
    *cu = n > 0 ? su / n : 0;
    *cv = n > 0 ? sv / n : 0;
    return n;
}

static double g_hue_tol = 1.0, g_amp_tol = 0.03;

static void check_target_r(const char *system, const char *name, const uint64_t *grid,
                           uint64_t total, double tu, double tv, double radius) {
    double cu, cv;
    double n = cluster(grid, tu, tv, radius, &cu, &cv);
    double share = total ? n / (double)total : 0;
    double dhue = (atan2(cv, cu) - atan2(tv, tu)) * 180.0 / M_PI;
    while (dhue > 180) dhue -= 360;
    while (dhue < -180) dhue += 360;
    double damp = hypot(cu, cv) / hypot(tu, tv) - 1.0;
    bool ok = share > 0.005 && fabs(dhue) < g_hue_tol && fabs(damp) < g_amp_tol;
    printf("%-5s %-3s target (%+.3f,%+.3f) got (%+.3f,%+.3f) hue %+5.2f deg amp %+5.1f%% share %4.1f%% %s\n",
           system, name, tu, tv, cu, cv, dhue, damp * 100, share * 100, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static void check_target(const char *system, const char *name, const uint64_t *grid,
                         uint64_t total, double tu, double tv) {
    check_target_r(system, name, grid, total, tu, tv, 0.08);
}

// Nothing (at most max_share of the points) within radius of (tu, tv).
static void check_absent(const char *system, const char *name, const uint64_t *grid,
                         uint64_t total, double tu, double tv, double radius, double max_share) {
    double cu, cv;
    double n = cluster(grid, tu, tv, radius, &cu, &cv);
    double share = total ? n / (double)total : 0;
    bool ok = share <= max_share;
    printf("%-5s %-3s absent at (%+.3f,%+.3f): share %.3f%% %s\n", system, name, tu, tv,
           share * 100, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

// Share of points farther than 0.02 plot units from every target, the origin
// and every straight segment between any two of them. Colour-bar transitions
// through a linear chroma chain are straight lines; a hard-switched chroma
// envelope drew a chain of loops along each one instead (0.42% PAL / 0.29%
// NTSC of the points; the live MS2130 EBU / SMPTE bars read 0.009% / 0.003%).
static double off_path_share(const uint64_t *grid, uint64_t total,
                             const double (*pt)[2], int npt) {
    double cell = GUI_VSCOPE_FULL_SCALE / (0.5 * GUI_VSCOPE_GRID), off = 0;
    for (size_t i = 0; i < CELLS; i++) {
        if (!grid[i]) continue;
        double u = ((double)(i % GUI_VSCOPE_GRID) + 0.5) * cell - GUI_VSCOPE_FULL_SCALE;
        double v = GUI_VSCOPE_FULL_SCALE - ((double)(i / GUI_VSCOPE_GRID) + 0.5) * cell;
        double dmin = 1e9;
        for (int a = 0; a < npt; a++) for (int b = a; b < npt; b++) {
            double dx = pt[b][0] - pt[a][0], dy = pt[b][1] - pt[a][1], l2 = dx * dx + dy * dy;
            double s = l2 > 0 ? ((u - pt[a][0]) * dx + (v - pt[a][1]) * dy) / l2 : 0;
            if (s < 0) s = 0;
            if (s > 1) s = 1;
            double d = hypot(u - (pt[a][0] + s * dx), v - (pt[a][1] + s * dy));
            if (d < dmin) dmin = d;
        }
        if (dmin > 0.02) off += (double)grid[i];
    }
    return total ? off / (double)total : 0;
}

static void run(int system_select, uint32_t source_rate) {
    const bool ntsc = system_select == 1;
    const bool secam = system_select == 2;
    const bool off = source_rate != RATE;
    const char *sys_name = secam ? "SECAM" : ntsc ? (off ? "NTSC+500ppm" : "NTSC")
                                                 : (off ? "PAL+500ppm" : "PAL");
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed);
    gui_vectorscope_t scope;
    assert(gui_vectorscope_init(&scope));
    gui_cvbs_feed_set_system(feed, system_select);
    gui_cvbs_feed_decoder(feed)->vscope = &scope;
    gui_cvbs_feed_set_pixels(feed, false);   // the Vector panel's own feed

    gui_testgen_system_t gen = secam ? GUI_TESTGEN_SECAM : ntsc ? GUI_TESTGEN_NTSC : GUI_TESTGEN_PAL;
    size_t frame_n = gui_testgen_frame_samples(gen, source_rate);
    int16_t *frame = malloc(frame_n * sizeof(int16_t));
    int16_t *chunk = malloc(65536 * sizeof(int16_t));
    uint32_t *field = malloc(CELLS * sizeof(uint32_t));
    uint64_t *sum = calloc(CELLS, sizeof(uint64_t));
    assert(frame && chunk && field && sum);

    uint64_t total = 0;
    int fields_taken = 0;
    const int frames = 16, warmup_frames = 8;
    for (int f = 0; f < frames; f++) {
        assert(gui_testgen_frame(gen, frame, frame_n, source_rate, (uint32_t)f) == frame_n);
        for (size_t at = 0; at < frame_n; at += 65536) {
            size_t n = frame_n - at < 65536 ? frame_n - at : 65536;
            for (size_t i = 0; i < n; i++) {
                chunk[i] = (int16_t)(((double)frame[at + i] / 32767.0) * 2170.0);
            }
            gui_cvbs_feed_process(feed, chunk, n, RATE);
            uint64_t points = 0;
            int got_system = 0;
            if (gui_vectorscope_take(&scope, field, &points, &got_system) && f >= warmup_frames) {
                assert(got_system == (secam ? GUI_VSCOPE_SYS_SECAM
                                            : ntsc ? GUI_VSCOPE_SYS_NTSC : GUI_VSCOPE_SYS_PAL));
                for (size_t i = 0; i < CELLS; i++) sum[i] += field[i];
                total += points;
                fields_taken++;
            }
        }
    }
    printf("%s: %u fields decoded, %d sampled, %llu points\n", sys_name,
           atomic_load(&scope.fields), fields_taken, (unsigned long long)total);
    if (fields_taken < 4 || total == 0) {
        printf("%s: no burst-locked fields reached the vectorscope - FAIL\n", sys_name);
        failures++;
    } else if (secam) {
        static const char *names[7] = { "", "B", "G", "Cy", "R", "Mg", "Yl" };
        const double chain_gain = 1.0;    // gui_testgen.c HT_SECAM_DEV_GAIN (standard deviation)
        g_hue_tol = 4.0;
        g_amp_tol = 0.08;
        for (int rgb = 1; rgb < 7; rgb++) {
            double u, v;
            gui_vectorscope_bar_target(GUI_VSCOPE_SYS_PAL, rgb, 0.75, &u, &v);
            // Tighter cluster (0.04): the SECAM vectors are 0.75-scaled and the
            // FM demod overshoots at bar edges (the blue->black edge pulled a
            // 0.08 cluster's centroid +9%; the plateau reads +4%).
            check_target_r(sys_name, names[rgb], sum, total, u * chain_gain, v * chain_gain, 0.04);
        }
        g_hue_tol = 1.0;
        g_amp_tol = 0.03;
    } else {
        static const char *names[7] = { "", "B", "G", "Cy", "R", "Mg", "Yl" };
        int vs_system = ntsc ? GUI_VSCOPE_SYS_NTSC : GUI_VSCOPE_SYS_PAL;
        for (int rgb = 1; rgb < 7; rgb++) {
            double u, v;
            gui_vectorscope_bar_target(vs_system, rgb, 0.75, &u, &v);
            check_target(sys_name, names[rgb], sum, total, u, v);
        }
        // Burst vector, V-switched like the picture (Standard = one phase):
        // NTSC 20 IRE at 180 deg; PAL 3/14 at 135 deg and NOTHING at 225
        // (harrypm 2026-11-17: Standard PAL shows one burst phase; Advanced
        // both). Tight radius: the envelope rise/fall draws the line in to
        // the centre, which a 0.08 cluster would average into the tip.
        if (ntsc) {
            check_target_r(sys_name, "burst", sum, total, -0.2, 0.0, 0.015);
        } else {
            double b = (3.0 / 14.0) * sqrt(0.5);
            check_target_r(sys_name, "bst135", sum, total, -b, b, 0.015);
            // At most 0.2% there: the Yl -> Cy transition crosses within
            // 0.012 of that point (0.04-0.06%); a burst there held 1.5%.
            check_absent(sys_name, "bst225", sum, total, -b, -b, 0.015, 0.002);
        }
        if (ntsc) {
            // SMPTE -I / +Q: 20 IRE (0.2 plot units) on the 303 / 33 deg axes.
            check_target(sys_name, "-I", sum, total,
                         0.2 * cos(303.0 * M_PI / 180), 0.2 * sin(303.0 * M_PI / 180));
            check_target(sys_name, "+Q", sum, total,
                         0.2 * cos(33.0 * M_PI / 180), 0.2 * sin(33.0 * M_PI / 180));
        }
        // Transitions straight: the off-path share.
        double pts[12][2];
        int np = 0;
        pts[np][0] = 0; pts[np][1] = 0; np++;
        for (int rgb = 1; rgb < 7; rgb++, np++)
            gui_vectorscope_bar_target(vs_system, rgb, 0.75, &pts[np][0], &pts[np][1]);
        if (ntsc) {
            pts[np][0] = -0.2; pts[np][1] = 0; np++;
            pts[np][0] = 0.2 * cos(303.0 * M_PI / 180); pts[np][1] = 0.2 * sin(303.0 * M_PI / 180); np++;
            pts[np][0] = 0.2 * cos(33.0 * M_PI / 180); pts[np][1] = 0.2 * sin(33.0 * M_PI / 180); np++;
        } else {
            double b = (3.0 / 14.0) * sqrt(0.5);
            pts[np][0] = -b; pts[np][1] = b; np++;
            pts[np][0] = -b; pts[np][1] = -b; np++;
        }
        double off = off_path_share(sum, total, (const double (*)[2])pts, np);
        bool off_ok = off < 0.0005;
        printf("%-5s transitions: %.3f%% of points off every straight transition %s\n",
               sys_name, off * 100.0, off_ok ? "ok" : "FAIL");
        if (!off_ok) failures++;
    }

    gui_cvbs_feed_decoder(feed)->vscope = NULL;
    gui_cvbs_feed_destroy(feed);
    gui_vectorscope_free(&scope);
    free(frame); free(chunk); free(field); free(sum);
}

// Auto system detection input: the field period must measure correctly when
// the decoder runs the WRONG standard (a PAL source on the NTSC default, the
// real MS2130 case 2026-11-14), because that is when Auto has to switch.
// The field detector has its own slicer (cvbs_field_detect), so EVERY field
// must measure in range whatever the decoder's standard: on the decoder's
// slicer NTSC into a PAL decoder managed 2-9 good readings in 16 frames.
#ifndef FP_FRAMES
#define FP_FRAMES 16
#endif
static void run_field_period(int source_system, int decoder_system) {
    const bool ntsc_src = source_system == 1;
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed);
    gui_cvbs_feed_set_system(feed, decoder_system);
    gui_testgen_system_t gen = ntsc_src ? GUI_TESTGEN_NTSC : GUI_TESTGEN_PAL;
    size_t frame_n = gui_testgen_frame_samples(gen, RATE);
    int16_t *frame = malloc(frame_n * sizeof(int16_t));
    int16_t *chunk = malloc(65536 * sizeof(int16_t));
    assert(frame && chunk);
    const cvbs_vsync_state_t *vs = &gui_cvbs_feed_decoder(feed)->vsync;
    uint32_t seen = 0;
    int good = 0, bad = 0;
    for (int f = 0; f < FP_FRAMES; f++) {
        assert(gui_testgen_frame(gen, frame, frame_n, RATE, (uint32_t)f) == frame_n);
        for (size_t at = 0; at < frame_n; at += 65536) {
            size_t n = frame_n - at < 65536 ? frame_n - at : 65536;
            for (size_t i = 0; i < n; i++) {
                chunk[i] = (int16_t)(((double)frame[at + i] / 32767.0) * 2170.0);
            }
            gui_cvbs_feed_process(feed, chunk, n, RATE);
            if (vs->field_measures != seen) {
                seen = vs->field_measures;
                int want = ntsc_src ? GUI_VSCOPE_SYS_NTSC : GUI_VSCOPE_SYS_PAL;
                if (gui_vectorscope_field_period_system(vs->last_field_us) == want) good++; else bad++;
            }
        }
    }
    bool ok = bad == 0 && good >= 2 * FP_FRAMES - 4;   // ~2 fields per frame
    printf("field period: %s source on %s decoder: %d in range, %d out (last %.0f us) %s\n",
           ntsc_src ? "NTSC" : "PAL", decoder_system == 1 ? "NTSC" : "PAL",
           good, bad, vs->last_field_us, ok ? "ok" : "FAIL");
    if (!ok) failures++;
    gui_cvbs_feed_destroy(feed);
    free(frame); free(chunk);
}

// Advanced (composite measurement) mode: PAL bars on BOTH mirrored target
// sets (no V-switch correction), the bursts at their transmitted vectors,
// NTSC on the single set; the readouts from the Test Gen's bursts (its
// chroma chain gain: PAL 0.79, NTSC 0.80 of nominal).
static void run_advanced(int system_select, uint32_t source_rate) {
    const bool ntsc = system_select == 1;
    const char *sys_name = ntsc ? (source_rate != RATE ? "ADV NTSC+500ppm" : "ADV NTSC")
                                : (source_rate != RATE ? "ADV PAL+500ppm" : "ADV PAL");
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed);
    gui_vectorscope_t scope;
    assert(gui_vectorscope_init(&scope));
    atomic_store(&scope.want_mode, GUI_VSCOPE_MODE_ADVANCED);
    gui_vectorscope_reset(&scope);
    gui_cvbs_feed_set_system(feed, system_select);
    gui_cvbs_feed_decoder(feed)->vscope = &scope;
    gui_cvbs_feed_set_pixels(feed, false);

    gui_testgen_system_t gen = ntsc ? GUI_TESTGEN_NTSC : GUI_TESTGEN_PAL;
    size_t frame_n = gui_testgen_frame_samples(gen, source_rate);
    int16_t *frame = malloc(frame_n * sizeof(int16_t));
    int16_t *chunk = malloc(65536 * sizeof(int16_t));
    uint32_t *field = malloc(CELLS * sizeof(uint32_t));
    uint64_t *sum = calloc(CELLS, sizeof(uint64_t));
    assert(frame && chunk && field && sum);
    uint64_t total = 0;
    int fields_taken = 0;
    double pct = 0, jit = 0, split = 0, cb = 0;
    int nmeas = 0;
    for (int f = 0; f < 16; f++) {
        assert(gui_testgen_frame(gen, frame, frame_n, source_rate, (uint32_t)f) == frame_n);
        for (size_t at = 0; at < frame_n; at += 65536) {
            size_t n = frame_n - at < 65536 ? frame_n - at : 65536;
            for (size_t i = 0; i < n; i++) chunk[i] = (int16_t)(((double)frame[at + i] / 32767.0) * 2170.0);
            gui_cvbs_feed_process(feed, chunk, n, RATE);
            gui_vscope_meta_t meta;
            if (gui_vectorscope_take_ex(&scope, field, NULL, &meta) && f >= 8) {
                assert(meta.mode == GUI_VSCOPE_MODE_ADVANCED);
                for (size_t i = 0; i < CELLS; i++) sum[i] += field[i];
                total += meta.points;
                fields_taken++;
                if (meta.meas.valid) {
                    pct += meta.meas.burst_percent;
                    jit += meta.meas.jitter_deg;
                    split += meta.meas.vsplit_deg;
                    cb += meta.meas.chroma_burst;
                    nmeas++;
                }
            }
        }
    }
    printf("%s: %d fields sampled, %llu points, %d with readouts\n", sys_name, fields_taken,
           (unsigned long long)total, nmeas);
    if (fields_taken < 4 || total == 0 || nmeas < 4) {
        printf("%s: no advanced fields - FAIL\n", sys_name);
        failures++;
    } else {
        static const char *names[7] = { "", "B", "G", "Cy", "R", "Mg", "Yl" };
        static const char *lower[7] = { "", "b", "g", "cy", "r", "mg", "yl" };
        int vs_system = ntsc ? GUI_VSCOPE_SYS_NTSC : GUI_VSCOPE_SYS_PAL;
        for (int rgb = 1; rgb < 7; rgb++) {
            double u, v;
            gui_vectorscope_bar_target(vs_system, rgb, 0.75, &u, &v);
            check_target(sys_name, names[rgb], sum, total, u, v);
            // PAL: the -V lines' set, mirrored about U (a hardware scope's
            // second set of boxes).
            if (!ntsc) check_target(sys_name, lower[rgb], sum, total, u, -v);
        }
        if (ntsc) {
            check_target_r(sys_name, "burst", sum, total, -0.2, 0.0, 0.015);
        } else {
            double b = (3.0 / 14.0) * sqrt(0.5);
            check_target_r(sys_name, "bst135", sum, total, -b, b, 0.015);
            check_target_r(sys_name, "bst225", sum, total, -b, -b, 0.015);
        }
        pct /= nmeas; jit /= nmeas; split /= nmeas; cb /= nmeas;
        const double want_pct = ntsc ? 80.0 : 79.0;
        bool ok = fabs(pct - want_pct) < 3.0 && jit < 1.0 && (ntsc || fabs(split) < 0.5);
        printf("%-5s readouts: burst %.1f%% of nominal (want %.0f), jitter %.2f deg, V split %+.2f deg, chroma/burst %.2f %s\n",
               sys_name, pct, want_pct, jit, split, cb, ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }
    gui_cvbs_feed_decoder(feed)->vscope = NULL;
    gui_cvbs_feed_destroy(feed);
    gui_vectorscope_free(&scope);
    free(frame); free(chunk); free(field); free(sum);
}

// Feed `frames` Test Gen frames of `gen` (scaled as run()); returns nothing.
static void feed_gen(gui_cvbs_feed_t *feed, gui_testgen_system_t gen, int frames,
                     void (*on_chunk)(void *ctx, int frame), void *ctx) {
    size_t frame_n = gui_testgen_frame_samples(gen, RATE);
    int16_t *frame = malloc(frame_n * sizeof(int16_t));
    int16_t *chunk = malloc(65536 * sizeof(int16_t));
    assert(frame && chunk);
    for (int f = 0; f < frames; f++) {
        assert(gui_testgen_frame(gen, frame, frame_n, RATE, (uint32_t)f) == frame_n);
        for (size_t at = 0; at < frame_n; at += 65536) {
            size_t n = frame_n - at < 65536 ? frame_n - at : 65536;
            for (size_t i = 0; i < n; i++) chunk[i] = (int16_t)(((double)frame[at + i] / 32767.0) * 2170.0);
            gui_cvbs_feed_process(feed, chunk, n, RATE);
            if (on_chunk) on_chunk(ctx, f);
        }
    }
    free(frame); free(chunk);
}

typedef struct {
    gui_vectorscope_t *scope;
    uint32_t *field;
    uint64_t *sum;
    uint64_t total;
    int from_frame, fields, nmeas;
    double rest_err[2], rest_jit, pct[2], jit_max;
    uint32_t ident_err;
    bool all_valid;
} take_ctx_t;

static void take_fields(void *p, int frame) {
    take_ctx_t *t = p;
    gui_vscope_meta_t meta;
    if (!gui_vectorscope_take_ex(t->scope, t->field, NULL, &meta) || frame < t->from_frame) return;
    t->fields++;
    if (t->sum) {
        for (size_t i = 0; i < CELLS; i++) t->sum[i] += t->field[i];
        t->total += meta.points;
    }
    if (!meta.meas.valid) { t->all_valid = false; return; }
    t->nmeas++;
    if (meta.meas.jitter_deg > t->jit_max) t->jit_max = meta.meas.jitter_deg;
    if (meta.meas.secam) {
        for (int c = 0; c < 2; c++) {
            t->rest_err[c] += meta.meas.rest_err_khz[c];
            t->pct[c] += meta.meas.porch_percent[c];
        }
        t->rest_jit += meta.meas.rest_jitter_khz;
        t->ident_err += meta.meas.ident_errors;
    }
}

// Advanced SECAM (the transmitted view): each line's own component only -
// D'B lines along U, D'R lines along V, no one-line pairing - plus the porch
// readouts. Expected from the Test Gen samples themselves (a direct
// quadrature fit of the generated porch, 2026-11-18): D'B 132.3 mV p-p =
// 79.3% of the 166.7 mV standard, D'R 174.2 mV = 81.2% of 214.5 mV; rest
// carriers exact; strict D'R / D'B alternation.
static void run_advanced_secam(void) {
    const char *sys_name = "ADV SECAM";
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed);
    gui_vectorscope_t scope;
    assert(gui_vectorscope_init(&scope));
    atomic_store(&scope.want_mode, GUI_VSCOPE_MODE_ADVANCED);
    gui_vectorscope_reset(&scope);
    gui_cvbs_feed_set_system(feed, 2);
    gui_cvbs_feed_decoder(feed)->vscope = &scope;
    gui_cvbs_feed_set_pixels(feed, false);
    take_ctx_t t = { .scope = &scope, .from_frame = 8, .all_valid = true };
    t.field = malloc(CELLS * sizeof(uint32_t));
    t.sum = calloc(CELLS, sizeof(uint64_t));
    assert(t.field && t.sum);
    feed_gen(feed, GUI_TESTGEN_SECAM, 16, take_fields, &t);
    printf("%s: %d fields sampled, %llu points, %d with readouts\n", sys_name, t.fields,
           (unsigned long long)t.total, t.nmeas);
    if (t.fields < 4 || t.total == 0 || t.nmeas < 4) {
        printf("%s: no advanced fields - FAIL\n", sys_name);
        failures++;
    } else {
        static const char *nu[7] = { "", "B.u", "G.u", "Cy.u", "R.u", "Mg.u", "Yl.u" };
        static const char *nv[7] = { "", "B.v", "G.v", "Cy.v", "R.v", "Mg.v", "Yl.v" };
        g_hue_tol = 4.0;
        g_amp_tol = 0.08;
        for (int rgb = 1; rgb < 7; rgb++) {
            double u, v;
            gui_vectorscope_bar_target(GUI_VSCOPE_SYS_PAL, rgb, 0.75, &u, &v);
            check_target_r(sys_name, nu[rgb], t.sum, t.total, u, 0.0, 0.04);   // D'B line
            check_target_r(sys_name, nv[rgb], t.sum, t.total, 0.0, v, 0.04);   // D'R line
        }
        g_hue_tol = 1.0;
        g_amp_tol = 0.03;
        // Every point on an axis: a SECAM line carries one component.
        double cell = GUI_VSCOPE_FULL_SCALE / (0.5 * GUI_VSCOPE_GRID), off = 0;
        for (size_t i = 0; i < CELLS; i++) {
            if (!t.sum[i]) continue;
            double u = ((double)(i % GUI_VSCOPE_GRID) + 0.5) * cell - GUI_VSCOPE_FULL_SCALE;
            double v = GUI_VSCOPE_FULL_SCALE - ((double)(i / GUI_VSCOPE_GRID) + 0.5) * cell;
            if (fabs(u) > 0.02 && fabs(v) > 0.02) off += (double)t.sum[i];
        }
        off /= (double)t.total;
        const double n = (double)t.nmeas;
        const double eb = t.rest_err[0] / n, er = t.rest_err[1] / n;
        const double pb = t.pct[0] / n, pr = t.pct[1] / n, rj = t.rest_jit / n;
        bool ok = off < 0.001 && fabs(eb) < 1.0 && fabs(er) < 1.0 && rj < 1.0 &&
                  t.ident_err == 0 && fabs(pb - 79.3) < 3.0 && fabs(pr - 81.2) < 3.0;
        printf("%-5s readouts: off-axis %.3f%%, rest D'B %+.2f D'R %+.2f kHz, jitter %.2f kHz, "
               "porch D'B %.1f%% (want 79.3) D'R %.1f%% (want 81.2), ident errors %u %s\n",
               sys_name, off * 100, eb, er, rj, pb, pr, t.ident_err, ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }
    gui_cvbs_feed_decoder(feed)->vscope = NULL;
    gui_cvbs_feed_destroy(feed);
    gui_vectorscope_free(&scope);
    free(t.field); free(t.sum);
}

// A live system change in Advanced with the old system's tail still in the
// pipeline (the GUI's Test Gen switch): PAL, the decoder switched to NTSC
// while one more PAL frame arrives, then NTSC. The scope's line-length
// measurement locked onto the tail's PAL line (0.8% long at the NTSC rate)
// for good - jitter 109 deg, bars smeared into rings (harrypm's screenshot,
// 2026-11-17). It must relock.
static void run_switch_tail(void) {
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed);
    gui_vectorscope_t scope;
    assert(gui_vectorscope_init(&scope));
    atomic_store(&scope.want_mode, GUI_VSCOPE_MODE_ADVANCED);
    gui_vectorscope_reset(&scope);
    gui_cvbs_feed_set_system(feed, 0);
    gui_cvbs_feed_decoder(feed)->vscope = &scope;
    gui_cvbs_feed_set_pixels(feed, false);
    take_ctx_t t = { .scope = &scope, .from_frame = 1000, .all_valid = true };
    t.field = malloc(CELLS * sizeof(uint32_t));
    assert(t.field);
    feed_gen(feed, GUI_TESTGEN_PAL, 6, take_fields, &t);
    gui_cvbs_feed_set_system(feed, 1);
    gui_vectorscope_reset(&scope);
    feed_gen(feed, GUI_TESTGEN_PAL, 1, take_fields, &t);    // the old system's tail
    t.from_frame = 10;                                        // judge the last 4 NTSC frames
    feed_gen(feed, GUI_TESTGEN_NTSC, 14, take_fields, &t);
    bool ok = t.nmeas >= 6 && t.all_valid && t.jit_max < 1.0;
    printf("switch PAL -> NTSC with a PAL tail (Advanced): %d fields judged, max jitter %.2f deg %s\n",
           t.nmeas, t.jit_max, ok ? "ok" : "FAIL");
    if (!ok) failures++;
    gui_cvbs_feed_decoder(feed)->vscope = NULL;
    gui_cvbs_feed_destroy(feed);
    gui_vectorscope_free(&scope);
    free(t.field);
}

// decode-orc's counting (tbc-analyse getCompositeTraceImage): a hit per
// on-plot point; with the trace, accumulateLine from the previous on-plot
// point of the segment, both ends inclusive (a stay counts its cell once).
static void ref_line(uint32_t *counts, int x0, int y0, int x1, int y1) {
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        counts[(size_t)y0 * GUI_VSCOPE_GRID + x0]++;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// The accumulator's run-length counting (a beam resting in one cell is
// written once, the transit ends folded into the runs) must give exactly
// decode-orc's hit and transit grids: a random walk with stays, one-cell
// steps, long jumps, off-plot points and segment breaks.
static void run_trace_counts(void) {
    gui_vectorscope_t scope;
    assert(gui_vectorscope_init(&scope));
    atomic_store(&scope.want_trace, true);
    gui_vectorscope_reset(&scope);
    uint32_t *rh = calloc(CELLS, sizeof(uint32_t)), *rt = calloc(CELLS, sizeof(uint32_t));
    uint32_t *gh = malloc(CELLS * sizeof(uint32_t)), *gt = malloc(CELLS * sizeof(uint32_t));
    assert(rh && rt && gh && gt);
    uint32_t seed = 1;
    double u = 0.1, v = -0.2;
    bool have = false;
    int px = 0, py = 0;
    for (int i = 0; i < 200000; i++) {
        seed = seed * 1664525u + 1013904223u;
        const unsigned r = seed >> 24;
        if (r < 4) {                                   // segment break
            gui_vectorscope_break(&scope);
            have = false;
            continue;
        }
        if (r < 120) { }                                // stay
        else if (r < 220) { u += ((int)(seed >> 8 & 3) - 1.5) * 0.002; v += ((int)(seed >> 12 & 3) - 1.5) * 0.002; }
        else { u = ((int)(seed >> 4 & 1023) - 512) / 500.0; v = ((int)(seed >> 14 & 1023) - 512) / 500.0; }
        gui_vectorscope_add(&scope, GUI_VSCOPE_SYS_PAL, u, v);
        const int cell = gui_vectorscope_cell(u, v);
        if (cell < 0) continue;                         // off the plot: the beam runs on
        const int x = cell & (GUI_VSCOPE_GRID - 1), y = cell >> GUI_VSCOPE_GRID_SHIFT;
        rh[cell]++;
        if (have) ref_line(rt, px, py, x, y);
        have = true;
        px = x;
        py = y;
    }
    gui_vectorscope_end_field(&scope);
    gui_vscope_meta_t meta;
    bool took = gui_vectorscope_take_ex(&scope, gh, gt, &meta);
    size_t hit_diff = 0, tr_diff = 0;
    uint64_t hit_sum = 0;
    int bx0 = GUI_VSCOPE_GRID, bx1 = -1, by0 = GUI_VSCOPE_GRID, by1 = -1;
    for (size_t i = 0; i < CELLS; i++) {
        hit_diff += gh[i] != rh[i];
        tr_diff += gt[i] != rt[i];
        hit_sum += rh[i];
        if (rh[i]) {
            const int x = (int)(i % GUI_VSCOPE_GRID), y = (int)(i / GUI_VSCOPE_GRID);
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    const bool box_ok = meta.x0 == bx0 && meta.x1 == bx1 && meta.y0 == by0 && meta.y1 == by1;
    const bool ok = took && hit_diff == 0 && tr_diff == 0 && meta.points == hit_sum && box_ok;
    printf("trace counts vs decode-orc: %llu points, %zu hit / %zu transit cells differ, box %s %s\n",
           (unsigned long long)hit_sum, hit_diff, tr_diff, box_ok ? "ok" : "wrong", ok ? "ok" : "FAIL");
    if (!ok) failures++;
    gui_vectorscope_free(&scope);
    free(rh); free(rt); free(gh); free(gt);
}

int main(void) {
    run_trace_counts();
    run_advanced(0, RATE);
    run_advanced(1, RATE);
    run_advanced(0, 40020000u);
    run_advanced_secam();
    run_switch_tail();
    run(1, RATE);  // NTSC: SMPTE SD bars
    run(0, RATE);  // PAL: 75% bars
    // Line-locked chroma: a capture clock 500 ppm off the declared 40 MSPS
    // (a real device; the MS2130 drew arcs before the line lock).
    run(1, 40020000u);
    run(0, 40020000u);
    run(2, RATE);  // SECAM: D'B/D'R as U/V, scope-only decode
    run_field_period(0, 1);  // PAL source, NTSC decoder (Auto must flip to PAL)
    run_field_period(1, 0);  // NTSC source, PAL decoder
    run_field_period(0, 0);
    if (failures) {
        printf("%d vectorscope target failure(s)\n", failures);
        return 1;
    }
    puts("Composite vectorscope: bars, -I and +Q (and SECAM D'B/D'R) land on the graticule targets");
    return 0;
}
