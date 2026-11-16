/* Test Gen composite accuracy contract (gui_testgen_frame).
 *
 * Measures the generator's raw 40 MSPS output directly - no decoder in the
 * loop - over a stream of the colour-frame loop played twice (the way the
 * Simulated device streams it), with the AC-coupling bump subtracted:
 *
 *   - H-sync leading edges (50% of sync, sub-sample) against a straight line
 *     clock: the SCH jitter. Whole-sample line starts made NTSC alternate
 *     2542/2543-sample lines (9.3 deg rms of subcarrier).
 *   - The sync 50% point sits ON the exact line start: a one-sided edge
 *     kernel moved every edge ~3.5 samples early.
 *   - Burst 50% start and width against the standard (PAL 5.60 / 2.25 us,
 *     NTSC 5.30 / 2.50 us) and the generator's own window function.
 *   - Subcarrier continuity: consecutive lines step 0 deg (NTSC) / +-90 deg
 *     (PAL V-switch), and every step across a VBI - field boundaries and
 *     the loop seam included - matches the continuous carrier and the
 *     line-by-line V-switch.
 *   - SECAM: every bar's FM plateau within 15 kHz of rest + D x the
 *     standard deviation.
 *
 * Found with the composite vectorscope work, 2026-11-16 (see
 * misrc_gui/dev/prompt_display_pipeline_counters_readme.md). */
#include "../misrc_gui/signal/gui_testgen.c"
#include <stdio.h>
#include <assert.h>

#define RATE 40000000u

static int failures;

static void check(bool ok, const char *what) {
    if (!ok) { printf("  FAIL: %s\n", what); failures++; }
}

static double wrap180(double d) {
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

static double bump_at(const ht_config_t *c, size_t n, size_t need) {
    double lpf = (double)c->lines, half = lpf / 2.0, amp = HT_SAG_PEAK * 32767.0;
    double line = (double)n * lpf / (double)need, d2 = line - half;
    double b1 = amp * (1.0 - exp(-line / HT_SAG_TAU_RISE))
              * exp(-fmax(0.0, line - HT_SAG_PIC_START) / HT_SAG_TAU_DECAY);
    double b2 = 0.0;
    if (d2 >= 0.0) b2 = amp * (1.0 - exp(-d2 / HT_SAG_TAU_RISE))
                     * exp(-fmax(0.0, d2 - HT_SAG_PIC_START) / HT_SAG_TAU_DECAY);
    return b1 + b2;
}

/* FM frequency of x[a..e) by mean phase increment against fc (1 us boxcar). */
static double fm_freq(const double *x, size_t a, size_t e, double fc, double rate) {
    int box = (int)(1e-6 * rate);
    double prev = 0.0, sum = 0.0;
    int n = 0;
    bool have = false;
    for (size_t i = a; i + (size_t)box < e; i += 4) {
        double I = 0.0, Q = 0.0, mean = 0.0;
        for (int j = 0; j < box; j++) mean += x[i + j];
        mean /= box;
        for (int j = 0; j < box; j++) {
            double ph = 2.0 * M_PI * fc * (double)(i + j) / rate;
            I += (x[i + j] - mean) * cos(ph);
            Q -= (x[i + j] - mean) * sin(ph);
        }
        double p = atan2(Q, I);
        if (have) {
            double d = p - prev;
            while (d > M_PI) d -= 2.0 * M_PI;
            while (d < -M_PI) d += 2.0 * M_PI;
            sum += d;
            n++;
        }
        prev = p;
        have = true;
    }
    return fc + (n ? sum / n : 0.0) / (2.0 * M_PI * 4.0) * rate;
}

static void run(gui_testgen_system_t sys) {
    const ht_config_t *c = &HT_CONF[sys];
    const double rate = RATE;
    const size_t need = gui_testgen_frame_samples(sys, RATE);
    const int loop = gui_testgen_loop_frames(sys);
    const size_t total = need * (size_t)loop * 2;
    double *x = malloc(total * sizeof(double));
    int16_t *fr = malloc(need * sizeof(int16_t));
    assert(x && fr);
    for (int f = 0; f < loop; f++) {
        assert(gui_testgen_frame(sys, fr, need, RATE, (uint32_t)f) == need);
        for (int rep = 0; rep < 2; rep++) {
            size_t base = ((size_t)rep * loop + f) * need;
            for (size_t n = 0; n < need; n++) x[base + n] = (double)fr[n] - bump_at(c, n, need);
        }
    }
    printf("%s (%d-frame loop, played twice):\n", gui_testgen_system_name(sys), loop);

    /* H-sync leading edges: falling through 50% of sync, low 3.5-6 us. */
    const double thr = 0.5 * c->sync_level * 32767.0;
    int cap = (int)(total / 1000), ne = 0;
    double *te = malloc(sizeof(double) * (size_t)cap);
    assert(te);
    for (size_t i = 1; i + 400 < total && ne < cap; i++) {
        if (x[i - 1] > thr && x[i] <= thr) {
            double t = (double)(i - 1) + (x[i - 1] - thr) / (x[i - 1] - x[i]);
            size_t j = i;
            while (j < total && x[j] <= thr) j++;
            double w = (double)(j - i) / rate * 1e6;
            if (w > 3.5 && w < 6.0) te[ne++] = t;
            i = j;
        }
    }
    const double L = (double)need / (double)c->lines;
    /* Residual against the EXACT half-line slot grid (an H line starts on a
     * slot boundary s * L / 2: odd slots for 625, even for 525) - no fit,
     * so a constant edge offset (the lopsided kernel) counts too. */
    const double S = 0.5 * L;
    double rr = 0.0, rmax = 0.0, rmean = 0.0;
    for (int k = 0; k < ne; k++) {
        double r = te[k] - S * round(te[k] / S);
        rr += r * r;
        rmean += r;
        if (fabs(r) > rmax) rmax = fabs(r);
    }
    double rms = sqrt(rr / ne);
    rmean /= ne;
    double deg = 360.0 * c->fsc_hz / rate;
    printf("  sync 50%% vs exact line start: mean %+.3f smp, rms %.3f smp (%.2f deg), max %.3f smp over %d lines\n",
           rmean, rms, rms * deg, rmax, ne);
    check(fabs(rmean) < 0.05, "sync edge centred on the exact line start");
    check(rms < 0.05 && rmax < 0.1, "line clock jitter under 0.05 samples rms");

    if (c->colour_mode == HT_SECAM) {
        double al = c->active_left_us * 1e-6 * rate, aw = c->active_width_us * 1e-6 * rate;
        int lines[2] = { 0, 0 };
        double worst = 0.0;
        for (int k = ne / 8; k < ne && (lines[0] < 20 || lines[1] < 20); k++) {
            double t0 = te[k];
            double fp = fm_freq(x, (size_t)(t0 + 6.0e-6 * rate), (size_t)(t0 + 9.5e-6 * rate), HT_SECAM_FM, rate);
            if (fabs(fp - HT_SECAM_DB) > 60e3 && fabs(fp - HT_SECAM_DR) > 60e3) continue;
            if (x[(size_t)(t0 + al + aw / 16)] < 0.3 * 32767.0) continue;   /* white bar: picture line */
            int dr = fabs(fp - HT_SECAM_DR) < fabs(fp - HT_SECAM_DB);
            if (lines[dr] >= 20) continue;
            lines[dr]++;
            for (int b = 0; b < 8; b++) {
                double f = fm_freq(x, (size_t)(t0 + al + (b + 0.25) * aw / 8),
                                   (size_t)(t0 + al + (b + 0.75) * aw / 8), HT_SECAM_FM, rate);
                double y, u, v;
                ht_rgb_to_yuv(c, HT_BARS[b].r, HT_BARS[b].g, HT_BARS[b].b, &y, &u, &v);
                double want = dr ? HT_SECAM_DR + HT_SECAM_DEV_GAIN * v : HT_SECAM_DB + HT_SECAM_DEV_GAIN * u;
                double lo = dr ? HT_SECAM_DR - 506e3 : HT_SECAM_DB - 350e3;
                double hi = dr ? HT_SECAM_DR + 350e3 : HT_SECAM_DB + 506e3;
                if (want < lo) want = lo;
                if (want > hi) want = hi;
                if (fabs(f - want) > worst) worst = fabs(f - want);
            }
        }
        printf("  SECAM bar FM plateaus: %d D'B + %d D'R lines, worst error %.1f kHz\n",
               lines[0], lines[1], worst / 1e3);
        check(lines[0] >= 10 && lines[1] >= 10, "SECAM D'B and D'R picture lines found");
        /* The standard LF pre-emphasis (85/255 kHz horn) is still settling
         * in a bar's centre half; its residual scales with the step (10.4 kHz
         * worst at full deviation = under 4% of the D'x deviation; the
         * receiver's de-emphasis takes it out). */
        check(worst < 15e3, "SECAM bar plateaus within 15 kHz");
        free(x); free(fr); free(te);
        return;
    }

    /* Burst window: the generator's own function, then the output. */
    {
        double prevw = 0.0, t50[2] = { -1.0, -1.0 };
        for (int i = 0; i <= 40000; i++) {
            double t = (c->burst_width_us + 2.0) * 1e-6 * i / 40000.0 - 1e-6;
            double w = ht_burstwin(t, c->burst_width_us * 1e-6, c->burst_rise_us * 1e-6);
            if (prevw < 0.5 && w >= 0.5 && t50[0] < 0) t50[0] = t * 1e6;
            if (prevw >= 0.5 && w < 0.5) t50[1] = t * 1e6;
            prevw = w;
        }
        printf("  burst window 50%% points %+.3f / %.3f us (width spec %.2f)\n", t50[0], t50[1], c->burst_width_us);
        check(fabs(t50[0]) < 0.01 && fabs(t50[1] - c->burst_width_us) < 0.01, "burst window 50% points at 0 and width");
    }
    /* Burst carrier phase per line in the ABSOLUTE stream frame. */
    {
        double b0 = c->burst_left_us * 1e-6 * rate, bw = c->burst_width_us * 1e-6 * rate;
        double prevph = 0.0, prevt = 0.0, ess = 0.0, amp_sum = 0.0;
        bool have = false;
        int steps = 0, breaks = 0, vbi = 0, vbi_breaks = 0, nb = 0;
        const bool pal = c->colour_mode == HT_PAL;
        for (int k = 0; k < ne; k++) {
            size_t a = (size_t)(te[k] + b0 + 0.5e-6 * rate);
            size_t e = (size_t)(te[k] + b0 + bw - 0.5e-6 * rate);
            if (e >= total) break;
            double mean = 0.0, I = 0.0, Q = 0.0;
            for (size_t n = a; n < e; n++) mean += x[n];
            mean /= (double)(e - a);
            for (size_t n = a; n < e; n++) {
                double ph = 2.0 * M_PI * c->fsc_hz * (double)n / rate;
                I += (x[n] - mean) * cos(ph);
                Q += (x[n] - mean) * sin(ph);
            }
            double amp = 2.0 * hypot(I, Q) / (double)(e - a);
            if (amp < 0.02 * 32767.0) continue;
            amp_sum += amp;
            nb++;
            double ph = atan2(Q, I) * 180.0 / M_PI;
            if (have) {
                int g = (int)round((te[k] - prevt) / L);
                double step = wrap180(ph - prevph), err;
                if (!pal || (g % 2) == 0) err = step;
                else err = wrap180(step - (step >= 0 ? 90.0 : -90.0));
                if (g == 1) {
                    if (fabs(err) < 5.0) { ess += err * err; steps++; } else breaks++;
                } else {
                    vbi++;
                    if (fabs(err) > 5.0) vbi_breaks++;
                }
            }
            prevph = ph;
            prevt = te[k];
            have = true;
        }
        double want_amp = c->burst_level_frac * (c->white_level - c->blanking_level)
                        * ht_chroma_gain(c->colour_mode) * 32767.0;
        printf("  burst: %d lines, amplitude %+.2f%%, line step error rms %.3f deg, %d breaks, VBI/seam steps %d of %d broken\n",
               nb, (amp_sum / nb / want_amp - 1.0) * 100.0, steps ? sqrt(ess / steps) : -1.0, breaks, vbi_breaks, vbi);
        check(steps > 1000, "burst lines measured");
        check(sqrt(ess / steps) < 1.0 && breaks == 0, "subcarrier continuous line to line");
        check(vbi >= 4 * loop - 1 && vbi_breaks == 0, "subcarrier and V-switch continuous across every VBI and the loop seam");
        check(fabs(amp_sum / nb / want_amp - 1.0) < 0.02, "burst amplitude within 2%");
    }
    /* Burst envelope 50% start, measured on a picture line (one-cycle
     * boxcar demod, searched after the sync's rising edge). */
    {
        double t0 = te[ne / 4];
        double b0 = c->burst_left_us * 1e-6 * rate, bw = c->burst_width_us * 1e-6 * rate;
        int spc = (int)(rate / c->fsc_hz + 0.5);
        size_t from = (size_t)(t0 + (c->hsync_us + 0.4) * 1e-6 * rate);
        size_t to = (size_t)(t0 + b0 + bw + 0.8e-6 * rate);
        double pk = 0.0, prev = 0.0, start = -1.0, end = -1.0;
        double env[1200];
        int m = 0;
        for (size_t n = from; n < to && m < 1200; n++, m++) {
            double I = 0.0, Q = 0.0;
            for (int j = -spc / 2; j <= spc / 2; j++) {
                double ph = 2.0 * M_PI * c->fsc_hz * (double)(n + j) / rate;
                I += x[n + j] * cos(ph);
                Q += x[n + j] * sin(ph);
            }
            env[m] = 2.0 * hypot(I, Q) / (double)(2 * (spc / 2) + 1);
            if (env[m] > pk) pk = env[m];
        }
        for (int i = 1; i < m; i++) {
            if (start < 0 && env[i - 1] < 0.5 * pk && env[i] >= 0.5 * pk)
                start = (double)(i - 1) + (0.5 * pk - env[i - 1]) / (env[i] - env[i - 1]);
            if (env[i - 1] >= 0.5 * pk && env[i] < 0.5 * pk)
                end = (double)(i - 1) + (env[i - 1] - 0.5 * pk) / (env[i - 1] - env[i]);
        }
        prev = (double)from - t0;
        double s_us = (start + prev) / rate * 1e6, w_us = (end - start) / rate * 1e6;
        printf("  burst on the line: 50%% start %.3f us after sync 50%% (spec %.2f), 50%% width %.3f us (spec %.2f)\n",
               s_us, c->burst_left_us, w_us, c->burst_width_us);
        check(fabs(s_us - c->burst_left_us) < 0.05, "burst starts at the standard time after sync");
        check(fabs(w_us - c->burst_width_us) < 0.05, "burst has the standard width");
    }
    free(x); free(fr); free(te);
}

int main(void) {
    assert(gui_testgen_loop_frames(GUI_TESTGEN_PAL) == 4);
    assert(gui_testgen_loop_frames(GUI_TESTGEN_NTSC) == 2);
    run(GUI_TESTGEN_NTSC);
    run(GUI_TESTGEN_PAL);
    run(GUI_TESTGEN_SECAM);
    if (failures) {
        printf("%d Test Gen composite accuracy failure(s)\n", failures);
        return 1;
    }
    puts("Test Gen composite: line clock, sync edges, burst window and subcarrier sequence exact");
    return 0;
}
