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
        // Burst vectors (plotted with the transmitted V): NTSC 20 IRE at
        // 180 deg; PAL 3/14 at 135 and 225 deg. Tight radius: the envelope
        // rise/fall draws the line in to the centre, which a 0.08 cluster
        // would average into the tip.
        if (ntsc) {
            check_target_r(sys_name, "burst", sum, total, -0.2, 0.0, 0.015);
        } else {
            double b = (3.0 / 14.0) * sqrt(0.5);
            check_target_r(sys_name, "bst135", sum, total, -b, b, 0.015);
            check_target_r(sys_name, "bst225", sum, total, -b, -b, 0.015);
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

int main(void) {
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
