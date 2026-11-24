/* Synthetic baseband composite contract tests for the CVBS decoder's ported
 * profile + VBI model + burst-locked baseband colour (the gui_rf_preview_test
 * pattern applied to the baseband path). The generator mirrors the RF test's
 * exact field geometry (EQ/broad trains, parity boundaries, per-parity field
 * line counts) in the IRE/sample domain at ANY rate, with ITU baseband
 * subcarrier colour (PAL V-switch alternation included). */
// Headless harness: keep the decoder's live diagnostics (H-sync ring +
// periodic state/frame dumps to the CWD) compiled in - the GUI build has
// them off (the dump writes would run on the display thread).
#define CVBS_LIVE_DIAG 1
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/processing/gui_display_thread.h"
#include <assert.h>

// Headless stubs: gui_cvbs_cleanup is the only decode-path caller of a raylib
// symbol (UnloadTexture on a texture this harness never created), and the
// --panel plugin path traces mode transitions through TraceLog (LTO drops
// every other render-path reference).
void UnloadTexture(Texture2D texture) { (void)texture; }
void TraceLog(int logLevel, const char *text, ...) { (void)logLevel; (void)text; }

#define TEST_PI 3.14159265358979323846

// Level domain: 0 IRE = 8192 counts, 32 counts per IRE (range 3200 counts =
// 100 IRE). The decoder measures levels adaptively, so any affine mapping is
// fine; this one keeps sync-tip comfortably inside int16.
#define LEVEL_ZERO ((double)8192.0)
#define COUNTS_PER_IRE ((double)32.0)
#define SYNC_TIP_IRE (-40.0)
#define BURST_PEAK_IRE (20.0)   // 40 IRE p-p -> 20 IRE peak amplitude

static const double bars[8][3] = {
    {1,1,1},{1,1,0},{0,1,1},{0,1,0},{1,0,1},{1,0,0},{0,0,1},{0,0,0}
};

typedef struct {
    bool pal;
    double rate;           // Sample rate in Hz
    int fields;
    bool with_burst;       // false: burst-free fixture (colour suppression)
    int damaged_field;     // -1 none; else blank that field's VBI train
    size_t *out_length;
} gen_cfg_t;

static double bar0_gray(int field) { return (field & 1) ? 0.22 : 0.82; }

// Generate the synthetic baseband composite signal. Returns a malloc'd
// int16 buffer; *out_length receives the sample count.
static int16_t *gen_baseband(const gen_cfg_t *cfg) {
    bool pal = cfg->pal;
    double rate = cfg->rate;
    double line_us = pal ? 64.0 : 63.55555555555556;
    double fsc = pal ? 4433618.75 : 3579545.0;
    int eq_count = pal ? 5 : 6;
    double eq_w = pal ? 2.35 : 2.3;
    double broad_w = pal ? 27.3 : 27.1;
    int field_lines[2] = {pal ? (int)cvbs_baseband_profiles[0].field_lines[0]
                              : (int)cvbs_baseband_profiles[1].field_lines[0],
                          pal ? (int)cvbs_baseband_profiles[0].field_lines[1]
                              : (int)cvbs_baseband_profiles[1].field_lines[1]};
    size_t line_samples = (size_t)(line_us * rate / 1000000.0 + 0.5);

    // Field starts (in lines) mirror the RF generator's parity schedule.
    int frame_lines = field_lines[0] + field_lines[1];
    size_t total_lines = (size_t)(cfg->fields / 2) * frame_lines +
                         (cfg->fields & 1) * field_lines[0];
    size_t length = total_lines * line_samples;
    int16_t *sig = malloc(length * sizeof(*sig));
    assert(sig);
    if (cfg->out_length) *cfg->out_length = length;

    int v_line = 0;  // global line index (alternating V-switch per line)

    for (size_t i = 0; i < total_lines; i++) {
        int frame_line = (int)(i % frame_lines);
        int parity = frame_line >= field_lines[0];
        int field = (int)(i / frame_lines) * 2 + parity;
        int line_in_field = frame_line - parity * field_lines[0];
        // Field-local geometry, exactly the RF generator's model: the train
        // windows are measured from the FIELD start (boundary*line_us before
        // the first EQ), the pulses sit at half-line cadence, and normal
        // lines resume after vbi_end. The boundary offset differs by parity
        // (PAL 0.5/1.0, NTSC 1.0/0.5) - this is what the decoder's first-EQ
        // parity detection reads.
        double boundary = pal ? (parity ? 1.0 : 0.5) : (parity ? 0.5 : 1.0);
        double eq_begin = boundary * line_us;                    // pre-EQ train start
        double broad_begin = eq_begin + eq_count * line_us * 0.5;
        double eq2_begin = broad_begin + eq_count * line_us * 0.5;
        double vbi_end_us = eq2_begin + eq_count * line_us * 0.5;
        // The damaged-field case blanks the entire VBI (the decoder must
        // reject that field's boundary on cadence/parity).
        bool damaged = (field == cfg->damaged_field);

        for (size_t k = 0; k < line_samples; k++) {
            double t_us = (double)k * 1000000.0 / rate;  // time in the line
            double t_field = (double)line_in_field * line_us + t_us;  // field-local
            // CONTINUOUS carrier (the REAL subcarrier never resets at line
            // boundaries - PAL advances 283.75 cycles/line, NTSC 227.5): the
            // phase comes from the ABSOLUTE sample position, matching the
            // decoder's absolute-frame carrier table. A per-line theta reset
            // made the synthetic's burst-phase steps the mirror of the real
            // convention's, which would flip PAL V-switch detection between
            // the synthetic and real signals.
            double theta = 2.0 * TEST_PI * fsc
                           * (double)(i * line_samples + k) / rate;
            double ire = 0.0;    // luma
            double chroma = 0.0; // chroma, in IRE units
            bool burst_slot = false;

            if (t_field >= vbi_end_us) {
                // Normal line: sync tip, porches, burst, active bars.
                if (t_us < 4.7) {
                    ire = SYNC_TIP_IRE;
                } else if (t_us >= 5.6 && t_us < (pal ? 7.85 : 7.8) && cfg->with_burst) {
                    burst_slot = true;
                    ire = 0.0;
                } else if (t_us >= (pal ? 10.5 : 9.45) && t_us < (pal ? 62.5 : 62.5556)) {
                    // Active area: colour bars.
                    double span = (pal ? 62.5 : 62.5556) - (pal ? 10.5 : 9.45);
                    int bar = (int)((t_us - (pal ? 10.5 : 9.45)) / span * 8.0);
                    if (bar > 7) bar = 7;
                    double r, g, b;
                    if (bar == 0) {
                        // Per-parity gray marker (field identity oracle).
                        r = g = b = bar0_gray(field);
                    } else {
                        r = bars[bar][0]; g = bars[bar][1]; b = bars[bar][2];
                    }
                    double y = .299 * r + .587 * g + .114 * b;
                    double u = .492 * (b - y), v = .877 * (r - y);
                    ire = y * 100.0;  // percent luma -> IRE
                    int vs = pal ? ((v_line & 1) ? -1 : 1) : 1;
                    // Chroma in IRE (the same x100 as the luma: u,v are 0..1
                    // RGB-normalized units, so without x100 the bars carry
                    // ~0.6 IRE of chroma and decode as pure luma). REAL ITU
                    // encoder convention: C = U sin(wt) + V cos(wt) (red at
                    // ~103 deg on the vectorscope), V alternating per line
                    // on PAL (vs = the transmitted inversion state).
                    chroma = (u * sin(theta) + (pal ? (double)vs : 1.0) * v * cos(theta)) * 100.0;
                } else {
                    ire = 0.0;
                }
            } else if (!damaged) {
                // Vertical blanking: half-line cadence pulse trains in
                // FIELD-LOCAL time (pre-EQ, broad, post-EQ).
                double in_half = fmod(t_field, line_us * 0.5);
                bool pre_eq = t_field >= eq_begin && t_field < broad_begin;
                bool broad = t_field >= broad_begin && t_field < eq2_begin;
                bool post_eq = t_field >= eq2_begin && t_field < vbi_end_us;
                if ((pre_eq || post_eq) && in_half < eq_w) ire = SYNC_TIP_IRE;
                else if (broad && in_half < broad_w) ire = SYNC_TIP_IRE;
                else ire = 0.0;
            } else {
                // Damaged field: blank VBI (no train at all).
                ire = 0.0;
            }

            double value = LEVEL_ZERO + ire * COUNTS_PER_IRE;
            if (burst_slot) {
                // Burst, REAL ITU convention: NTSC sits at 180 deg = -U
                // (the -sin axis of the chroma basis); PAL swings +/-45 deg
                // about -U with the burst's V component matching the
                // transmitted V inversion (vs) - so phi_b = +90 -/+ 45 deg
                // relative to the U-on-sin chroma basis. Line-local frame.
                int vs = pal ? ((v_line & 1) ? -1 : 1) : 1;
                double phi_b = pal ? (TEST_PI / 2.0 - (double)vs * TEST_PI / 4.0)
                                   : TEST_PI / 2.0;
                value += BURST_PEAK_IRE * COUNTS_PER_IRE * cos(theta + phi_b);
            }
            value += chroma * COUNTS_PER_IRE;
            int iv = (int)(value + 0.5);
            if (iv < -32768) iv = -32768;
            if (iv > 32767) iv = 32767;
            sig[i * line_samples + k] = (int16_t)iv;
        }
        v_line++;
    }
    return sig;
}


// Feed a generated signal through the production decoder in odd-sized chunks
// (crossing every buffer/pulse boundary) at the given live rate. keep_ring0:
// zero the H-spacing ring after every buffer so the rate self-calibration
// can never commit (cvbs_rate then stays at the DECLARED rate - A/B for
// measured-rate hue bias on real signals).
// The harness feeds files faster than wall time. Let any published job
// finish between blocks so test results do not depend on OS scheduling.
static void wait_for_cvbs_scout(cvbs_decoder_t *decoder) {
    if (!decoder->receiver) return;
    cvbs_scout_worker_t *worker = decoder->receiver->worker;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (atomic_load_explicit(&worker->state, memory_order_acquire) != CVBS_SCOUT_QUEUED) return;
        thrd_sleep_ms(1);
    }
    assert(!"CVBS scout worker timed out");
}

static void run_decoder(cvbs_decoder_t *decoder, const int16_t *sig,
                        size_t length, double rate, bool keep_ring0) {
    size_t pos = 0;
    while (pos < length) {
        size_t chunk = (pos & 1) ? 4093u : 8191u;
        if (chunk > length - pos) chunk = length - pos;
        gui_cvbs_process_buffer(decoder, sig + pos, chunk, (uint32_t)rate);
        wait_for_cvbs_scout(decoder);
        if (keep_ring0) decoder->hh_ring_count = 0;
        pos += chunk;
    }
}

// Sample one bar's channel from the decoded display frame (RGB24) at an
// EXACT frame row (no vertical averaging: the parity-marker check needs the
// row's own field, and a +-1 row spread would mix both weaves).
static double frame_bar_mean(const cvbs_decoder_t *decoder, int bar,
                             int row, int channel) {
    const uint8_t *frame = decoder->display_front;
    int fw = decoder->frame_width;
    int fh = decoder->frame_height;
    assert(frame && fw > 0 && fh > 0 && row >= 0 && row < fh);
    bool ntsc = decoder->state.format == CVBS_FORMAT_NTSC;
    double active_start = ntsc ? 9.45 : 10.5;
    double active_end = ntsc ? 62.5556 : 62.5;
    double line_us = ntsc ? 63.5556 : 64.0;
    double span = active_end - active_start;
    double center_us = active_start + (bar + 0.5) / 8.0 * span;
    int px = (int)(center_us / line_us * fw);
    if (px < 0) px = 0;
    if (px > fw - 1) px = fw - 1;
    double total = 0.0;
    int count = 0;
    for (int dx = -3; dx <= 3; dx++) {
        int x = px + dx;
        if (x < 0 || x >= fw) continue;
        total += frame[(size_t)row * fw * 3u + (size_t)x * 3u + channel];
        count++;
    }
    return count ? total / count / 255.0 : 0.0;
}

static double frame_time_px_mean(const cvbs_decoder_t *decoder, double t_us,
                                 int row, int channel) {
    // Mean over +-2 px around the LINE-LOCAL time-mapped pixel (the
    // full-frame grid maps the whole line 0..line_us onto 0..frame_width).
    const uint8_t *frame = decoder->display_front;
    int fw = decoder->frame_width;
    bool ntsc = decoder->state.format == CVBS_FORMAT_NTSC;
    double line_us = ntsc ? 63.5556 : 64.0;
    int px = (int)(t_us / line_us * fw);
    if (px < 0) px = 0;
    if (px > fw - 1) px = fw - 1;
    double total = 0.0;
    int count = 0;
    for (int dx = -2; dx <= 2; dx++) {
        int x = px + dx;
        if (x < 0 || x >= fw) continue;
        total += frame[(size_t)row * fw * 3u + (size_t)x * 3u + channel];
        count++;
    }
    return count ? total / count / 255.0 : 0.0;
}

static void check_bars(cvbs_decoder_t *decoder, int row_pair,
                       const char *name, double bar7_chroma_tol) {
    double worst = 0.0;
    for (int bar = 1; bar < 8; bar++) {
        double r = frame_bar_mean(decoder, bar, row_pair, 0);
        double g = frame_bar_mean(decoder, bar, row_pair, 1);
        double b = frame_bar_mean(decoder, bar, row_pair, 2);
        for (int c = 0; c < 3; c++) {
            double got = c == 0 ? r : c == 1 ? g : b;
            double err = fabs(got - bars[bar][c]);
            if (err > worst) worst = err;
            if (err > 0.14) {
                fprintf(stderr, "FAIL %s bar %d ch %d: %.3f expected %.3f\n",
                        name, bar, c, got, bars[bar][c]);
            }
        }
    }
    printf("%s bars max error %.3f\n", name, worst);
    // The hue ORDER must be right even where amplitude is imperfect.
    for (int bar = 1; bar < 8; bar++) {
        // Bar 7 (black) carries NO chroma by construction: a dominant
        // channel cannot exist there. The contract for it is a dark,
        // grayscale bar - no chroma leak onto the no-colour bar.
        if (bar == 7) {
            double r7 = frame_bar_mean(decoder, bar, row_pair, 0);
            double g7 = frame_bar_mean(decoder, bar, row_pair, 1);
            double b7 = frame_bar_mean(decoder, bar, row_pair, 2);
            double mx7 = r7 > g7 ? (r7 > b7 ? r7 : b7) : (g7 > b7 ? g7 : b7);
            double mn7 = r7 < g7 ? (r7 < b7 ? r7 : b7) : (g7 < b7 ? g7 : b7);
            if (mx7 - mn7 > bar7_chroma_tol || mx7 > 0.45) {
                fprintf(stderr, "FAIL %s bar 7 chroma leak: "
                                "[%.3f,%.3f,%.3f] (tol %.2f)\n",
                        name, r7, g7, b7, bar7_chroma_tol);
            }
            assert(mx7 - mn7 <= bar7_chroma_tol && mx7 <= 0.45);
            continue;
        }
        double on = 0.0, off = 1.0;
        for (int c = 0; c < 3; c++) {
            double got = frame_bar_mean(decoder, bar, row_pair, c);
            if (bars[bar][c] > 0.5) { if (got > on) on = got; }
            else { if (got < off) off = got; }
        }
        if (on - off <= 0.20) {
            fprintf(stderr, "FAIL %s bar %d wrong hue: on %.3f off %.3f\n",
                    name, bar, on, off);
        }
        assert(on - off > 0.20);
    }
}

static void run_format_ex(bool pal, double rate, bool with_burst,
                          double declared_rate) {
    char name[96];
    snprintf(name, sizeof(name), "%s %s @ %.1f MHz%s",
            pal ? "PAL" : "NTSC", with_burst ? "colour" : "no-burst", rate / 1e6,
            declared_rate != rate ? " (declared 40 MHz)" : "");
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    decoder.colour_mode = CVBS_COLOUR_AUTO;

    size_t length;
    gen_cfg_t cfg = {pal, rate, 8, with_burst, -1, &length};
    int16_t *sig = gen_baseband(&cfg);
    assert(sig && length > 0);

    // The DECLARED rate is what the display thread passes; the self-
    // calibration (H-to-H spacing median) must recover the true rate from
    // the signal itself, so the wrong declaration only delays lock briefly.
    run_decoder(&decoder, sig, length, declared_rate, false);
    gui_cvbs_swap_buffers(&decoder);

    // Sync + VBI model: locked, boundaries committed, none rejected on a
    // clean generator (the damaged-field case is a separate test below).
    printf("%s VBI: locked=%d accepted=%d rejected=%d (cad %d par %d) "
           "cands=%d frames=%d reason='%s' lines=%d\n",
           name, decoder.pll.locked ? 1 : 0, decoder.vsync.fields_accepted,
           decoder.vsync.fields_rejected, decoder.vsync.reject_cadence,
           decoder.vsync.reject_parity, decoder.vsync.vsync_candidates,
           decoder.state.frames_decoded, decoder.vsync.last_reject_reason,
           decoder.pll.current_line);
    assert(decoder.pll.locked);
    assert(decoder.vsync.vsync_candidates == decoder.vsync.fields_accepted + decoder.vsync.fields_rejected);
    assert(decoder.vsync.fields_accepted >= 5);
    assert(decoder.vsync.fields_rejected == 0);
    assert(decoder.vsync.reject_cadence == 0 && decoder.vsync.reject_parity == 0);
    assert(decoder.state.frames_decoded >= 4);

    // Levels: blanking/black come from the per-line back-porch mean (true
    // 0 IRE, chroma-free) and white from the porch + 90% of the filtered
    // (luma-domain) maximum, so the measured black sits within a few IRE of
    // true blank even on 100% saturated bars (the raw histogram's second
    // peak ran to -30.5 IRE on the same fixture before the porch path),
    // and the white lands at ~85 IRE against the 100 IRE white-bar plateau.
    double black = decoder.levels.black_level;
    double white = decoder.levels.white_level;
    double measured_ire_black = (black - LEVEL_ZERO) / COUNTS_PER_IRE;
    double measured_ire_white = (white - LEVEL_ZERO) / COUNTS_PER_IRE;
    printf("%s levels: black %.1f IRE, white %.1f IRE (blank 0, sync tip -40)\n",
           name, measured_ire_black, measured_ire_white);
    assert(measured_ire_black > -5.0 && measured_ire_black < 5.0);
    assert(measured_ire_white - measured_ire_black > 80.0);

    // Field-parity marker: bar 0 alternates 0.82 (first/odd field = buffer 0)
    // vs 0.22 - if parity were inverted the woven frame would swap them.
    int mid_field_row = (pal ? 150 : 125);
    {
        // Rows come from the deinterlacer's weave: odd frame rows = field
        // buffer 0 (first/odd field), even frame rows = buffer 1. Sample
        // bar 0's EXACT rows in both weaves (a row spread would mix them).
        double odd_row = frame_bar_mean(&decoder, 0, mid_field_row * 2 + 1, 1);
        double even_row = frame_bar_mean(&decoder, 0, mid_field_row * 2, 1);
        printf("%s parity marker: odd-weave %.3f even-weave %.3f\n",
               name, odd_row, even_row);
        assert(fabs(odd_row - even_row) > 0.35);
        // The bright marker (0.82) must be in the first/odd-field weave.
        assert(odd_row > 0.55 && even_row < 0.45);
    }

    if (with_burst) {
        // The self-calibrated rate must match the true stream rate (the
        // declared value is only a bootstrap).
        double meas_mhz = (double)decoder.measured_rate_hz / 1e6;
        printf("%s measured rate: %.2f MHz (declared %.1f)\n",
               name, meas_mhz, declared_rate / 1e6);
        assert(decoder.measured_rate_valid);
        assert(fabs(meas_mhz - rate / 1e6) < 0.15);
        check_bars(&decoder, mid_field_row, name, 0.10);
    } else {
        // Burst-free: every bar decodes gray (colour suppressed).
        double worst_gray = 0.0;
        for (int bar = 1; bar < 8; bar++) {
            double r = frame_bar_mean(&decoder, bar, mid_field_row, 0);
            double g = frame_bar_mean(&decoder, bar, mid_field_row, 1);
            double b = frame_bar_mean(&decoder, bar, mid_field_row, 2);
            double err = fabs(r - g) + fabs(g - b) + fabs(r - b);
            if (err > worst_gray) worst_gray = err;
        }
        printf("%s no-burst grayscale delta %.3f\n", name, worst_gray);
        assert(worst_gray < 0.06);
    }

    gui_cvbs_cleanup(&decoder);
    free(sig);
}

static void run_format(bool pal, double rate, bool with_burst) {
    run_format_ex(pal, rate, with_burst, rate);
}

//============================================================================
// SECAM FM-chroma contracts (backlog item 4)
//============================================================================
// Reference closed forms (tbc-tools secamdecoder.cpp): bellNetwork(),
// inverseLf(), bandGain(). The decoder's time-domain equivalents must match
// these numerically - the match is the port's correctness contract.
static double ref_bell_mag(double f, double centre) {
    if (f <= 0.0) return 0.0;
    double F = f / centre - centre / f;
    double num_re = 1.0, num_im = 1.26 * F;
    double den_re = 1.0, den_im = 16.0 * F;
    double den = den_re * den_re + den_im * den_im;
    double re = (num_re * den_re + num_im * den_im) / den;
    double im = (num_im * den_re - num_re * den_im) / den;
    return sqrt(re * re + im * im);
}

static double ref_inverse_lf_mag(double f) {
    // (1 + j f/(3 f1)) / (1 + j f/f1)  (the DECODER applies this; the encoder
    // pre-correction is its inverse - the reference's exact form).
    double num_re = 1.0, num_im = fabs(f) / (3.0 * 85000.0);
    double den_re = 1.0, den_im = fabs(f) / 85000.0;
    double den = den_re * den_re + den_im * den_im;
    double re = (num_re * den_re + num_im * den_im) / den;
    double im = (num_im * den_re - num_re * den_im) / den;
    return sqrt(re * re + im * im);
}

// Evaluate the decoder's DIGITAL cloche at baseband fb (the steady-state
// of the bilinear one-pole/one-zero section implemented in gui_cvbs.c:
// H(z) = (b0 + b1 z^-1)/(1 + a1 z^-1), coefficients pre-folded).
static double decoder_bell_mag(double fb_hz, double fs_dec) {
    double w = 2.0 * TEST_PI * fb_hz / fs_dec;
    double em_re = cos(w), em_im = -sin(w);   // e^{-jw}
    // Numerator: b0 + b1*e^{-jw}
    double nr = g_secam_bell_b0_re
                + g_secam_bell_b1_re * em_re
                - g_secam_bell_b1_im * em_im;
    double ni = g_secam_bell_b0_im
                + g_secam_bell_b1_im * em_re
                + g_secam_bell_b1_re * em_im;
    // Denominator: 1 + a1*e^{-jw}
    double d_re = 1.0 + g_secam_bell_a1_re * em_re
                  - g_secam_bell_a1_im * em_im;
    double d_im = g_secam_bell_a1_im * em_re
                  + g_secam_bell_a1_re * em_im;
    double dmag2 = d_re * d_re + d_im * d_im;
    // H = num / den
    double hr = (nr * d_re + ni * d_im) / dmag2;
    double hi = (ni * d_re - nr * d_im) / dmag2;
    return sqrt(hr * hr + hi * hi);
}

static void run_secam_numeric_match(void) {
    const double rate = 4.0 * 4433618.75;   // the panel's PAL 4fsc rate
    const double centre = 0.5 * (CVBS_SECAM_FOB_HZ + CVBS_SECAM_FOR_HZ);
    cvbs_secam_build_filters(rate, centre);
    const double fs_dec = rate / (double)g_secam_decim;

    // Cloche: the digital pipeline's steady-state response vs the reference
    // closed form over the block band. The reference is FFT-exact per bin;
    // the time-domain factorisation (constant + 2-tap + complex one-pole) is
    // exact at DC, < 1% at the carrier pair, and degrades with |fb| (the F
    // linearisation is quadratic in fb + the step-invariant pole at high
    // omega-tau). MEASURED envelope (Python replication of the exact C
    // constants): 0.67% at +-78 kHz, ~6% at +-200 kHz, ~8% at +-300 kHz,
    // ~27% at the +-580 kHz skirt. Tiers set FROM that data:
    //   carriers  (|fb| <= 100 kHz, the porch reference + steady carries):
    //             < 1%   (hue/zero accuracy)
    //   deviation (|fb| <= 350 kHz, the picture FM sidebands; the rails
    //             clamp at 350/506 kHz): < 10% (chroma transition shaping)
    //   skirt     (|fb| <= 580 kHz, beyond every rail - noise shaping
    //             only, nothing decodable sits there): < 30%.
    // This is a tighter envelope than the classic analogue cloche the
    // pre-port tbc-tools decoder used for years; the bars contracts below
    // prove the end-to-end decode regardless.
    double worst_carrier = 0.0, worst_dev = 0.0, worst_skirt = 0.0;
    for (double fb = -580000.0; fb <= 580000.0; fb += 1000.0) {
        double ref = ref_bell_mag(fabs(fb + centre), CVBS_SECAM_BELL_CENTRE_HZ);
        double got = decoder_bell_mag(fb, fs_dec);
        double err = fabs(got - ref) / (ref > 1e-9 ? ref : 1.0);
        if (fabs(fb) <= 100000.0) {
            if (err > worst_carrier) worst_carrier = err;
        } else if (fabs(fb) <= 350000.0) {
            if (err > worst_dev) worst_dev = err;
        } else {
            if (err > worst_skirt) worst_skirt = err;
        }
    }
    printf("SECAM numeric bell match: carriers %.3f%% deviation %.3f%% "
           "skirt %.3f%%\n",
           worst_carrier * 100.0, worst_dev * 100.0, worst_skirt * 100.0);
    fflush(stdout);
    assert(worst_carrier < 0.01);
    assert(worst_dev < 0.10);
    assert(worst_skirt < 0.30);

    // De-emphasis: the causal bilinear lead-lag IIR's response vs the
    // reference closed form over the FULL decimated band (the reference's
    // 1.6/1.9 MHz colour-diff LPF is subsumed by the decimation's
    // +-1.107 MHz Nyquist - see the designer comment in gui_cvbs.c - so
    // the de-emphasis is the only shaping left). H(e^{jw}) evaluated
    // directly from the built coefficients. TIERED like the bell contract:
    // the deviation band (<= 350 kHz, where the encoder pre-correction's
    // inverse must be exact) is held to < 2% (the common pre-warp matched
    // at the shelf's geometric centre flattens it to ~1.5% worst, at the
    // 350 kHz band edge); above that only noise shaping remains and the
    // bilinear warp is allowed its few percent.
    double worst_deemph_dev = 0.0, worst_deemph_hi = 0.0;
    for (double f = 0.0; f <= fs_dec / 2.0; f += 25000.0) {
        double w = 2.0 * TEST_PI * f / fs_dec;
        // H = (b0 + b1 e^{-jw}) / (1 + a1 e^{-jw})
        double nr = g_secam_deemph_b0 + g_secam_deemph_b1 * cos(w);
        double ni = -g_secam_deemph_b1 * sin(w);
        double dr = 1.0 + g_secam_deemph_a1 * cos(w);
        double di = -g_secam_deemph_a1 * sin(w);
        double dmag2 = dr * dr + di * di;
        double hr = (nr * dr + ni * di) / dmag2;
        double hi = (ni * dr - nr * di) / dmag2;
        double got = sqrt(hr * hr + hi * hi);
        double ref = ref_inverse_lf_mag(f);
        double err = fabs(got - ref) / (ref > 1e-9 ? ref : 1.0);
        if (f <= 350000.0) {
            if (err > worst_deemph_dev) worst_deemph_dev = err;
        } else {
            if (err > worst_deemph_hi) worst_deemph_hi = err;
        }
    }
    printf("SECAM numeric de-emphasis match: deviation %.3f%% high %.3f%%\n",
           worst_deemph_dev * 100.0, worst_deemph_hi * 100.0);
    fflush(stdout);
    assert(worst_deemph_dev < 0.02);
    assert(worst_deemph_hi < 0.08);
}

// SECAM generator configuration.
typedef struct {
    int fields;
    double rate;
    double block_offset_hz;   // converter-style block shift (0 = studio)
    bool pre_correct;         // apply the encoder's LF pre-correction
} gen_secam_cfg_t;

// The SECAM encoder's HF pre-correction (the "bell", BT.470): a linear
// network on the FM subcarrier, A(f) = (1 + j16u)/(1 + j1.26u),
// u = f/f0 - f0/f, f0 = 4.286 MHz - i.e. with j u = s/w0 + w0/s,
// H(s) = (16 s^2 + w0 s + 16 w0^2)/(1.26 s^2 + w0 s + 1.26 w0^2), the
// bell's 0.115 floor normalised to 1 at f0. Bilinear, pre-warped at w0.
// Every real SECAM signal carries it; the decoder demodulates through
// its inverse (the bell path).
typedef struct { double n0, n1, n2, d1, d2, x1, x2, y1, y2; } secam_bell_t;
static void secam_bell_init(secam_bell_t *b, double rate) {
    const double w0 = 2.0 * TEST_PI * CVBS_SECAM_BELL_CENTRE_HZ;
    const double K = w0 / tan(w0 / (2.0 * rate)), K2 = K * K;
    const double a2 = 16.0, a1 = w0, a0 = 16.0 * w0 * w0;
    const double b2 = 1.26, b1 = w0, b0 = 1.26 * w0 * w0;
    const double d0 = b2 * K2 + b1 * K + b0;
    b->n0 = (a2 * K2 + a1 * K + a0) / d0;
    b->n1 = (-2.0 * a2 * K2 + 2.0 * a0) / d0;
    b->n2 = (a2 * K2 - a1 * K + a0) / d0;
    b->d1 = (-2.0 * b2 * K2 + 2.0 * b0) / d0;
    b->d2 = (b2 * K2 - b1 * K + b0) / d0;
    b->x1 = b->x2 = b->y1 = b->y2 = 0.0;
}
static double secam_bell_step(secam_bell_t *b, double x) {
    double y = b->n0 * x + b->n1 * b->x1 + b->n2 * b->x2 - b->d1 * b->y1 - b->d2 * b->y2;
    b->x2 = b->x1; b->x1 = x; b->y2 = b->y1; b->y1 = y;
    return y;
}

// Generate a synthetic SECAM baseband signal: PAL 625 geometry, per-line
// alternating D'R/D'B FM chroma on the bars, rest carriers across the
// porch, phase-continuous FM (the frequency track integrated per sample).
// The encoder's LF pre-correction is a real first-order lead-lag on the
// deviation waveform (bilinear-matched biquad), so the pre_correct variant
// exercises the decoder's exact de-emphasis inverse end to end.
static int16_t *gen_secam(const gen_secam_cfg_t *cfg, size_t *out_length) {
    double rate = cfg->rate;
    double line_us = 64.0;
    int field_lines[2] = {(int)cvbs_baseband_profiles[0].field_lines[0],
                          (int)cvbs_baseband_profiles[0].field_lines[1]};
    int eq_count = 5;
    double eq_w = 2.35, broad_w = 27.3;
    size_t line_samples = (size_t)(line_us * rate / 1000000.0 + 0.5);

    int starts[17];
    starts[0] = 0;
    for (int f = 0; f < cfg->fields; f++) {
        starts[f + 1] = starts[f] + field_lines[f & 1];
    }
    size_t total_lines = (size_t)starts[cfg->fields];
    size_t length = total_lines * line_samples;
    int16_t *sig = malloc(length * sizeof(*sig));
    assert(sig);
    *out_length = length;

    // LF pre-correction biquad on the deviation (Hz units): H(s) =
    // (1 + s/wz) / (1 + s/wp), wz = 2pi f1, wp = 2pi 3f1 - bilinear.
    double wz = 2.0 * TEST_PI * 85000.0;
    double wp = 2.0 * TEST_PI * 3.0 * 85000.0;
    double T = 1.0 / rate;
    // Bilinear: s = 2/T (1-z^-1)/(1+z^-1).
    double kz = 2.0 / (T * wz);
    double kp = 2.0 / (T * wp);
    double b0 = (1.0 + kz), b1 = (1.0 - kz);
    double a0 = (1.0 + kp), a1 = (1.0 - kp);
    // Normalized.
    b0 /= a0; b1 /= a0; a1 /= a0; a0 = 1.0;
    double pc_x1 = 0.0, pc_y1 = 0.0;

    double phase = 0.0;   // continuous FM phase (radians)
    int v_line = 0;
    secam_bell_t bell;
    secam_bell_init(&bell, rate);

    for (size_t i = 0; i < total_lines; i++) {
        int field = -1;
        for (int f = 0; f < cfg->fields; f++) {
            if ((int)i >= starts[f] && (int)i < starts[f + 1]) { field = f; break; }
        }
        assert(field >= 0);
        int line_in_field = (int)i - starts[field];
        int parity = field & 1;
        double boundary = parity ? 1.0 : 0.5;
        double eq_begin = boundary * line_us;
        double broad_begin = eq_begin + eq_count * line_us * 0.5;
        double eq2_begin = broad_begin + eq_count * line_us * 0.5;
        double vbi_end_us = eq2_begin + eq_count * line_us * 0.5;

        // The line's carrier + component (D'R on even GLOBAL lines).
        bool is_red = ((v_line & 1) == 0);
        double rest_hz = (is_red ? CVBS_SECAM_FOR_HZ : CVBS_SECAM_FOB_HZ)
                         + cfg->block_offset_hz;
        double dev_hz = is_red ? CVBS_SECAM_DEV_DR_HZ : CVBS_SECAM_DEV_DB_HZ;

        for (size_t k = 0; k < line_samples; k++) {
            double t_us = (double)k * 1000000.0 / rate;
            double t_field = (double)line_in_field * line_us + t_us;
            double ire = 0.0;
            double carrier_amp_ire = 0.0;
            double dev = 0.0;   // deviation in D'x units this sample

            if (t_field >= vbi_end_us) {
                if (t_us < 4.7) {
                    ire = SYNC_TIP_IRE;
                } else if (t_us >= 5.6 && t_us < 10.5) {
                    // Back porch: the REST (undeviated) carrier.
                    carrier_amp_ire = 23.0;
                    dev = 0.0;
                } else if (t_us >= 10.5 && t_us < 62.5) {
                    // Active: the bars, D'R/D'B from the colour.
                    double span = 62.5 - 10.5;
                    int bar = (int)((t_us - 10.5) / span * 8.0);
                    if (bar > 7) bar = 7;
                    double r, g, b;
                    if (bar == 0) {
                        r = g = b = bar0_gray(field);
                    } else {
                        r = bars[bar][0]; g = bars[bar][1]; b = bars[bar][2];
                    }
                    double y = .299 * r + .587 * g + .114 * b;
                    // D'R = -1.902 (R-Y), D'B = +1.505 (B-Y) (BT.470).
                    dev = is_red ? (-1.902 * (r - y)) : (1.505 * (b - y));
                    ire = y * 100.0;
                    carrier_amp_ire = 23.0;
                } else {
                    ire = 0.0;
                }
            } else {
                // Vertical blanking pulse trains (the PAL model).
                double in_half = fmod(t_field, line_us * 0.5);
                bool pre_eq = t_field >= eq_begin && t_field < broad_begin;
                bool broad = t_field >= broad_begin && t_field < eq2_begin;
                bool post_eq = t_field >= eq2_begin && t_field < vbi_end_us;
                if ((pre_eq || post_eq) && in_half < eq_w) ire = SYNC_TIP_IRE;
                else if (broad && in_half < broad_w) ire = SYNC_TIP_IRE;
                else ire = 0.0;
                // SECAM VBI: the carrier runs through the non-pulse parts of
                // the blanking lines too (the ident/bottle region reads it).
                if (ire > -1.0) {
                    carrier_amp_ire = 23.0;
                    dev = 0.0;
                    // Bottles on early VBI lines (BT.470-6 option B): rows
                    // 6-14, Dr lines deviated far positive / Db far negative.
                    if (line_in_field >= 6 && line_in_field <= 14
                        && t_us >= 10.5 && t_us < 52.0) {
                        dev = is_red ? 1.25 : -1.52;
                    }
                }
            }

            // LF pre-correction on the deviation waveform (the encoder's).
            double dev_pc = dev;
            if (cfg->pre_correct) {
                dev_pc = b0 * dev + b1 * pc_x1 - a1 * pc_y1;
                pc_x1 = dev;
                pc_y1 = dev_pc;
                // The encoder's DEVIATION LIMITER (BT.470 Table 4 maxima -
                // the same limits the decoder's rails enforce, applied to
                // the PRE-CORRECTED waveform like a real modulator). Without
                // it the pre-correction's x3 transition spikes sweep the
                // carrier up to +-1.9 MHz, far outside any receiver's band:
                // the decoder's envelope collapses, the colour killer gates
                // whole bar-heads to zero (measured: 7-8 decimated samples
                // at the cyan/magenta starts) and the de-emphasis integrates
                // the holes. A real encoder NEVER emits that.
                if (is_red) {
                    if (dev_pc < CVBS_SECAM_DEV_DR_LO)
                        dev_pc = CVBS_SECAM_DEV_DR_LO;
                    if (dev_pc > CVBS_SECAM_DEV_DR_HI)
                        dev_pc = CVBS_SECAM_DEV_DR_HI;
                } else {
                    if (dev_pc < CVBS_SECAM_DEV_DB_LO)
                        dev_pc = CVBS_SECAM_DEV_DB_LO;
                    if (dev_pc > CVBS_SECAM_DEV_DB_HI)
                        dev_pc = CVBS_SECAM_DEV_DB_HI;
                }
            }

            // FM: integrate the instantaneous frequency.
            double f_inst = rest_hz + dev_pc * dev_hz;
            phase += 2.0 * TEST_PI * f_inst / rate;

            double value = LEVEL_ZERO + ire * COUNTS_PER_IRE;
            // The carrier through the encoder's bell: ~9 IRE at f0 (the
            // Test Gen's level, ~18 IRE p-p, near the standard 23% p-p),
            // boosted up to ~3x away from f0 - the bell's HF pre-emphasis
            // (the ident bottles reach ~30 IRE).
            value += COUNTS_PER_IRE * secam_bell_step(&bell, 0.4 * carrier_amp_ire * sin(phase));
            int iv = (int)(value + 0.5);
            if (iv < -32768) iv = -32768;
            if (iv > 32767) iv = 32767;
            sig[i * line_samples + k] = (int16_t)iv;
        }
        v_line++;
    }
    return sig;
}

// Decode a SECAM fixture and check the bars + the calibration/ident diag.
static void run_secam_bars(double block_offset_hz, bool pre_correct,
                           const char *name) {
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, 2);   // SECAM (PAL 625 geometry)
    decoder.colour_mode = CVBS_COLOUR_AUTO;

    size_t length;
    gen_secam_cfg_t cfg = {8, 40000000.0, block_offset_hz, pre_correct};
    int16_t *sig = gen_secam(&cfg, &length);
    assert(sig && length > 0);

    run_decoder(&decoder, sig, length, cfg.rate, false);
    gui_cvbs_swap_buffers(&decoder);

    // Sync/VBI model unchanged (SECAM shares the PAL geometry).
    printf("%s VBI: locked=%d accepted=%d rejected=%d frames=%d\n",
           name, decoder.pll.locked ? 1 : 0, decoder.vsync.fields_accepted,
           decoder.vsync.fields_rejected, decoder.state.frames_decoded);
    fflush(stdout);
    assert(decoder.pll.locked);
    assert(decoder.vsync.vsync_candidates == decoder.vsync.fields_accepted + decoder.vsync.fields_rejected);
    assert(decoder.vsync.fields_accepted >= 5);
    assert(decoder.state.frames_decoded >= 4);

    // Calibration + identification: the locked pair must track the offset;
    // the ident must be confident from the porch (or bottles).
    double fob, for_, off, conf, mad;
    int dr_even, src, fcal, freuse, fnom;
    gui_cvbs_get_secam_diag(&decoder, &fob, &for_, &off, &dr_even, &conf,
                            &src, &mad, &fcal, &freuse, &fnom);
    printf("%s cal: fob=%.0f for=%.0f off=%.0f drOnEven=%d conf=%.2f "
           "src=%d mad=%.0f (cal %d reuse %d nominal %d)\n",
           name, fob, for_, off, dr_even, conf, src, mad, fcal, freuse, fnom);
    fflush(stdout);
    assert(fcal >= 3);   // most fields measured a real pair
    assert(fabs(fob - (4250000.0 + block_offset_hz)) < 60000.0);
    assert(fabs(for_ - (4406250.0 + block_offset_hz)) < 60000.0);
    assert(fabs((for_ - fob) - 156250.0) < 10000.0);   // the exact 10*fH lock
    assert(conf >= 0.9);

    // Bars: the same hue/amplitude contract as PAL (the decoder outputs
    // U/V through the same matrix; SECAM D'R/D'B map straight onto it).
    // The FLAT-deviation fixture gets a relaxed bar-7 chroma tolerance:
    // no real encoder emits flat deviations, and the causal de-emphasis
    // then legitimately tails the preceding bar's colour into the black
    // bar (the reference's behaviour too) - the pre-corrected fixtures
    // (real encoder models) hold the strict 0.10.
    check_bars(&decoder, 150, name, pre_correct ? 0.10 : 0.30);

    gui_cvbs_cleanup(&decoder);
    free(sig);
}

static void run_cadence_reject(bool pal) {
    char name[64];
    snprintf(name, sizeof(name), "%s damaged VBI cadence", pal ? "PAL" : "NTSC");
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    decoder.colour_mode = CVBS_COLOUR_AUTO;

    size_t length;
    gen_cfg_t cfg = {pal, 40000000.0, 8, true, 4, &length};
    int16_t *sig = gen_baseband(&cfg);
    assert(sig);

    run_decoder(&decoder, sig, length, cfg.rate, false);
    gui_cvbs_swap_buffers(&decoder);

    // The damaged field's boundary is rejected (cadence or parity reason);
    // the rest of the fields still decode.
    printf("%s: accepted %d rejected %d (cad %d par %d) reason '%s'\n",
           name, decoder.vsync.fields_accepted, decoder.vsync.fields_rejected,
           decoder.vsync.reject_cadence, decoder.vsync.reject_parity,
           decoder.vsync.last_reject_reason);
    assert(decoder.vsync.fields_rejected >= 1);
    assert(decoder.vsync.reject_cadence >= 1 || decoder.vsync.reject_parity >= 1);
    assert(decoder.vsync.last_reject_reason[0] != '\0');
    assert(decoder.vsync.fields_accepted >= 3);

    gui_cvbs_cleanup(&decoder);
    free(sig);
}

// Full-frame view hybrid contracts (tbc-tools analyse HYBRID mode): the
// active window of picture lines keeps the leveled studio-swing mapping;
// the background (VBI lines + the blanking/sync/burst regions of picture
// lines) uses the fixed IRE-domain hybrid mapping and KEEPS the decoded
// chroma. The synthetic generator's VBI region ends at line 8, so lines
// 8..21 carry normal sync+burst+bars while the decoder classifies them as
// VBI (the profile's picture starts at 22/20) - exactly the chroma-keeping
// background case.
static void run_full_view_hybrid(bool pal) {
    char name[64];
    snprintf(name, sizeof(name), "%s full-view hybrid", pal ? "PAL" : "NTSC");
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    gui_cvbs_set_frame_view_mode(&decoder, CVBS_FRAME_VIEW_FULL);
    decoder.colour_mode = CVBS_COLOUR_AUTO;

    size_t length;
    gen_cfg_t cfg = {pal, 40000000.0, 8, true, -1, &length};
    int16_t *sig = gen_baseband(&cfg);
    assert(sig);
    run_decoder(&decoder, sig, length, cfg.rate, false);
    gui_cvbs_swap_buffers(&decoder);

    int fw = decoder.frame_width, fh = decoder.frame_height;
    int picture_row = pal ? 300 : 240;   // field line 150/120: a picture line
    int vbi_row = 20;                    // field line 10: VBI-classified
    double active_start = pal ? 10.5 : 9.45;
    double span = (pal ? 62.5 : 62.5556) - active_start;
    double bar1_t = active_start + 1.5 / 8.0 * span;   // yellow center
    double bar7_t = active_start + 7.5 / 8.0 * span;   // black bar center
    double porch_t = active_start - 1.2;              // blanking before active
    double tip_t = 2.0;                                // sync tip middle
    double hb = pal ? RF_HYBRID_BLACK_IRE_PAL : RF_HYBRID_BLACK_IRE_NTSC;
    double hs = pal ? RF_HYBRID_SPAN_IRE_PAL : RF_HYBRID_SPAN_IRE_NTSC;
    double blank_dy = (0.0 - hb) / hs;   // 0 IRE through the hybrid mapping

    printf("%s: frame %dx%d\n", name, fw, fh);
    assert(fw == (pal ? 1135 : 910) && fh == (pal ? 625 : 525));

    // Picture row: leveled studio-swing mapping on the active window.
    double p_black = frame_time_px_mean(&decoder, bar7_t, picture_row, 1);
    double p_yr = frame_time_px_mean(&decoder, bar1_t, picture_row, 0);
    double p_yb = frame_time_px_mean(&decoder, bar1_t, picture_row, 2);
    printf("%s: picture black dy=%.3f, yellow r=%.3f b=%.3f\n",
           name, p_black, p_yr, p_yb);
    assert(p_black < 0.10);                          // lifted studio floor
    assert(p_yr > 0.55 && p_yb < 0.20);              // leveled colour bar

    // Picture row background: porch = 0 IRE -> the hybrid mapping; gray
    // (no chroma on the blanking porch).
    double porch = frame_time_px_mean(&decoder, porch_t, picture_row, 1);
    double porch_r = frame_time_px_mean(&decoder, porch_t, picture_row, 0);
    double porch_b = frame_time_px_mean(&decoder, porch_t, picture_row, 2);
    printf("%s: porch dy=%.3f (expected %.3f), r=%.3f b=%.3f\n",
           name, porch, blank_dy, porch_r, porch_b);
    assert(fabs(porch - blank_dy) < 0.05);
    assert(fabs(porch_r - porch) + fabs(porch_b - porch) < 0.08);

    // Picture row sync tip: below the hybrid black -> clamps at the floor.
    double tip = frame_time_px_mean(&decoder, tip_t, picture_row, 1);
    printf("%s: sync tip dy=%.3f (expected floor 0)\n", name, tip);
    assert(tip < 0.03);

    // VBI-classified row (normal line content below picture_start): the
    // SAME bars decode through the hybrid background mapping and KEEP the
    // decoded chroma; the black bar maps to the hybrid blanking level.
    double v_yr = frame_time_px_mean(&decoder, bar1_t, vbi_row, 0);
    double v_yb = frame_time_px_mean(&decoder, bar1_t, vbi_row, 2);
    double v_black = frame_time_px_mean(&decoder, bar7_t, vbi_row, 1);
    double v_black_r = frame_time_px_mean(&decoder, bar7_t, vbi_row, 0);
    printf("%s: VBI-row yellow r=%.3f b=%.3f, black dy=%.3f (expected %.3f)\n",
           name, v_yr, v_yb, v_black, blank_dy);
    assert(v_yr - v_yb > 0.30);              // chroma KEPT on the background
    assert(fabs(v_black - blank_dy) < 0.06);
    assert(fabs(v_black_r - v_black) < 0.06); // black bar: no chroma, gray

    gui_cvbs_cleanup(&decoder);
    free(sig);
}

// Render-path conversion contract (dev/gui_output_verification_rules.md):
// the RGB24 -> raylib Color blit the GUI render ACTUALLY calls must carry the
// decoder's channel order into Color's r,g,b,a bytes. The 2026-10-19
// incident: a hand-packed uint32 conversion (0xFF000000 | r<<16 | g<<8 | b,
// whose little-endian memory is b,g,r,a against Color's r,g,b,a) displayed
// the decoded BLUE as red and RED as blue - yellow<->cyan, red<->blue -
// while EVERY headless dump (reading the RGB24 buffers directly, bypassing
// the conversion) was green. Editing the blit REQUIRES editing this contract
// with it.
static void run_render_conversion_contract(void) {
    // 4x3 RGB24 source with distinct channels, plus the incident
    // discriminators: a yellow pixel (r=g, b=0) must stay yellow in Color
    // bytes (the swapped packing turned it cyan), a red pixel must stay red
    // (the swapped packing turned it blue).
    uint8_t src[4 * 3 * 3];
    for (int i = 0; i < 4 * 3; i++) {
        src[i * 3 + 0] = 0x12;  // R
        src[i * 3 + 1] = 0x34;  // G
        src[i * 3 + 2] = 0x56;  // B
    }
    src[(0 * 4 + 1) * 3 + 0] = 0xF0;  // pixel (1,0): yellow
    src[(0 * 4 + 1) * 3 + 1] = 0xF0;
    src[(0 * 4 + 1) * 3 + 2] = 0x00;
    src[(2 * 4 + 3) * 3 + 0] = 0xF0;  // pixel (3,2): red
    src[(2 * 4 + 3) * 3 + 1] = 0x00;
    src[(2 * 4 + 3) * 3 + 2] = 0x00;
    // Pitch 6 > width 4 (the MAX_WIDTH-style row pitch) and 2 spare rows:
    // stride handling + untouched-beyond-the-blit coverage.
    static Color dst[5 * 6];
    memset(dst, 0, sizeof(dst));
    cvbs_blit_rgb24_to_color(src, dst, 4, 3, 6);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 4; x++) {
            Color c = dst[y * 6 + x];
            if (x == 1 && y == 0) {
                assert(c.r == 0xF0 && c.g == 0xF0 && c.b == 0x00 && c.a == 255);
            } else if (x == 3 && y == 2) {
                assert(c.r == 0xF0 && c.g == 0x00 && c.b == 0x00 && c.a == 255);
            } else {
                assert(c.r == 0x12 && c.g == 0x34 && c.b == 0x56 && c.a == 255);
            }
        }
        // Beyond the row width: untouched (the pitch gap).
        assert(dst[y * 6 + 4].a == 0 && dst[y * 6 + 5].a == 0);
    }
    // Beyond the height: untouched rows.
    for (int x = 0; x < 6; x++) {
        assert(dst[3 * 6 + x].a == 0 && dst[4 * 6 + x].a == 0);
    }
    puts("render-path RGB24->Color conversion contract passed");
}

static void run_off_mode_gray(bool pal) {
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    decoder.colour_mode = CVBS_COLOUR_OFF;   // exact legacy grayscale

    size_t length;
    gen_cfg_t cfg = {pal, 40000000.0, 6, true, -1, &length};
    int16_t *sig = gen_baseband(&cfg);
    assert(sig);
    run_decoder(&decoder, sig, length, cfg.rate, false);
    gui_cvbs_swap_buffers(&decoder);

    // Every bar is gray in Off mode.
    double worst_gray = 0.0;
    for (int bar = 1; bar < 8; bar++) {
        double r = frame_bar_mean(&decoder, bar, (pal ? 150 : 125), 0);
        double g = frame_bar_mean(&decoder, bar, (pal ? 150 : 125), 1);
        double b = frame_bar_mean(&decoder, bar, (pal ? 150 : 125), 2);
        double err = fabs(r - g) + fabs(g - b) + fabs(r - b);
        if (err > worst_gray) worst_gray = err;
    }
    printf("%s Off-mode grayscale delta %.3f\n", pal ? "PAL" : "NTSC",
           worst_gray);
    assert(worst_gray < 0.06);

    gui_cvbs_cleanup(&decoder);
    free(sig);
}

// TEMP live-capture diagnostic (real-signal convention measurement; NOT
// part of the synthetic contract). Usage:
//   gui_cvbs_test.exe [--pal|--ntsc] [--rate=<MHz>] [--u8|--s16]
//                     [--full] [--fixed] [--panel] [--ppm=<path>] [--declared]
//                     <live_raw>
// --panel drives the EXACT GUI path: the panel plugin's fixed-rate soxr
// resample (to the 4x-fsc classic target - PAL 17.9 / NTSC 14.3 MHz -
// LIBSOXR_ENABLED builds) plus the panel's decoder defaults (FIXED sync,
// PAL), reproducing the GUI's decode regime headlessly.
// Decodes a raw baseband capture through the production decoder and prints
// per-bar measured U/V (inverted from the decoded RGB) so the real encoder
// convention can be fitted from data. Sample domain: unsigned 8-bit (the
// 8-bit RF FLAC family, e.g. a HackDAC EBU bars capture) or signed
// little-endian 16-bit (the MISRC recorder's 12-bit<<4 FLAC family - mod-16
// 100%: feed the s16 values as-is, the decoder is level-adaptive and so
// amplitude-invariant). --full decodes with the full-frame view enabled and
// --ppm additionally dumps the decoded display frame (P6) for offline
// inspection (signalstats / visual A-B against a reference).
static void run_live_diag(const char *path, bool pal, double rate,
                          bool s16, bool full, bool fixed, bool panel,
                          const char *ppm, bool declared_rate_only,
                          bool secam, bool receiver, bool display_chunks) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    assert(sz > 0);
    size_t bytes = s16 ? (size_t)sz & ~(size_t)1u : (size_t)sz;
    size_t count = s16 ? bytes / 2u : bytes;
    assert(count > 0);
    uint8_t *raw = malloc(bytes);
    assert(raw);
    assert(fread(raw, 1, bytes, f) == bytes);
    fclose(f);

    int16_t *sig = malloc(count * sizeof(int16_t));
    assert(sig);
    if (s16) {
        memcpy(sig, raw, count * sizeof(int16_t));  // little-endian host
    } else {
        for (size_t i = 0; i < count; i++) sig[i] = (int16_t)raw[i];  // affine ok
    }
    free(raw);

    video_cvbs_plugin_t plugin;      // only initialized in --panel mode
    cvbs_decoder_t stack_decoder;
    cvbs_decoder_t *decoder;
    if (panel) {
        // The EXACT GUI path: the panel plugin (its fixed-rate soxr resample
        // to the 4x-fsc target + the panel decoder defaults: FIXED sync,
        // PAL) driven chunk-wise like the app's display thread.
        // The panel vtable callocs the whole plugin state; zero it the same
        // way here: video_cvbs_plugin_init initializes ONLY the decoder - the
        // soxr fields must start NULL/0 or the resampler ensure dereferences
        // garbage pointers (measured: an access violation on the first
        // process call).
        memset(&plugin, 0, sizeof(plugin));
        assert(video_cvbs_plugin_init(&plugin));
        decoder = &plugin.decoder;
        // Apply the requested SYSTEM to the plugin decoder: the plugin is
        // created in the persisted default (PAL unless the settings push
        // otherwise - which the harness never does), so WITHOUT this the
        // --secam flag only changed the print label and every panel-mode
        // run decoded in PAL format (measured 2026-10-21: the LIVE secam
        // diag line never printed; the "garbage" bars were PAL's burst
        // demod locking onto the SECAM rest carrier ~4.25/4.406 MHz).
        gui_cvbs_set_format(decoder, secam ? 2 : (pal ? 0 : 1));
        if (receiver) assert(gui_cvbs_set_50ohms(decoder, true));
        if (full) gui_cvbs_set_frame_view_mode(decoder, CVBS_FRAME_VIEW_FULL);
        decoder->colour_mode = CVBS_COLOUR_AUTO;
        fprintf(stderr, "[panel] feed start\n");
        size_t pos = 0;
        while (pos < count) {
            size_t chunk = display_chunks ? DISPLAY_SAMPLE_COUNT : ((pos & 1) ? 4093u : 8191u);
            if (chunk > count - pos) chunk = count - pos;
            video_cvbs_plugin_process(&plugin, sig + pos, chunk,
                                       (uint32_t)rate);
            wait_for_cvbs_scout(decoder);
            if (declared_rate_only) decoder->hh_ring_count = 0;
            pos += chunk;
            if ((pos & 0xFFFFF) == 0) fprintf(stderr, "[panel] fed %zu\n", pos);
        }
        fprintf(stderr, "[panel] feed done\n");
    } else {
        assert(gui_cvbs_init(&stack_decoder));
        decoder = &stack_decoder;
        gui_cvbs_set_format(decoder, secam ? 2 : (pal ? 0 : 1));
        if (full) gui_cvbs_set_frame_view_mode(decoder, CVBS_FRAME_VIEW_FULL);
        if (receiver) assert(gui_cvbs_set_50ohms(decoder, true));
        if (fixed) decoder->sync_mode = CVBS_SYNC_MODE_FIXED;  // GUI default
        decoder->colour_mode = CVBS_COLOUR_AUTO;
        // declared_rate_only suppresses the H-spacing self-calibration so
        // the decode runs at the DECLARED rate (A/B for rate bias).
        run_decoder(decoder, sig, count, rate, declared_rate_only);
    }
    gui_cvbs_swap_buffers(decoder);

    printf("LIVE %s (%s @ %.1f MSPS%s%s): locked=%d accepted=%d rejected=%d "
           "(cad %d par %d) frames=%d lines=%d reason='%s'\n",
           path, secam ? "SECAM" : (pal ? "PAL" : "NTSC"), rate / 1e6,
           full ? ", full view" : "", panel ? ", panel path" : "",
           decoder->pll.locked ? 1 : 0, decoder->vsync.fields_accepted,
           decoder->vsync.fields_rejected, decoder->vsync.reject_cadence,
           decoder->vsync.reject_parity, decoder->state.frames_decoded,
           decoder->pll.current_line, decoder->vsync.last_reject_reason);
    printf("LIVE levels: black=%d white=%d tip=%d thresh=%d dc=%+.1f/%d "
           "meas_rate=%.2f MHz\n",
           (int)decoder->levels.black_level, (int)decoder->levels.white_level,
           (int)decoder->levels.sig_min, (int)decoder->levels.sync_threshold,
           decoder->dc_offset, decoder->dc_offset_valid ? 1 : 0,
           (double)decoder->measured_rate_hz / 1e6);
    printf("LIVE sync: fixed=%.3f ema=%+.4f fsl=%d gsl=%d bad=%d "
           "phase_err=%+.2f\n",
           decoder->fixed_line_period, decoder->fixed_period_ema,
           decoder->fixed_sync_locked ? 1 : 0, decoder->pll.lines_since_lock,
           decoder->pll.bad_sync_count, decoder->pll.phase_error);
    if (decoder->receiver) {
        const cvbs_receiver_t *r = decoder->receiver;
        printf("LIVE 50 ohms: acquired=%d scout_corner=%.0f Hz sync_corner=%.0f Hz video_corner=%.0f Hz\n",
               r->sync_valid, r->sync_corner, r->applied_corner, r->corner);
    }
    if (decoder->state.format == CVBS_FORMAT_SECAM) {
        double fob, for_, off, conf, mad;
        int dr_even, src, fcal, freuse, fnom;
        gui_cvbs_get_secam_diag(decoder, &fob, &for_, &off, &dr_even,
                                &conf, &src, &mad, &fcal, &freuse, &fnom);
        printf("LIVE secam: fob=%.0f for=%.0f offset=%+.0f drOnEven=%d "
               "conf=%.2f src=%d noiseMad=%.0f (cal %d reuse %d nominal %d)\n",
               fob, for_, off, dr_even, conf, src, mad, fcal, freuse, fnom);
    }
    {
        // Throughput split against the real-time budget: signal seconds
        // from the sample count at the declared rate vs CPU seconds spent
        // in the resampler, the per-sample pass and the per-line decode.
        uint64_t rs_us = 0, pr_us = 0, ln_us = 0, lines = 0;
        gui_cvbs_profile_get(&rs_us, &pr_us, &ln_us, &lines);
        double signal_s = (double)count / rate;
        printf("LIVE profile: signal %.2f s | resample %.2f s (%.0f%%) | "
               "process %.2f s (%.0f%%) of which line decode %.2f s (%.0f%%) "
               "| %.1f us/line over %llu lines | total CPU %.2f s = %.2fx "
               "real time\n",
               signal_s, rs_us / 1e6, 100.0 * rs_us / 1e6 / signal_s,
               pr_us / 1e6, 100.0 * pr_us / 1e6 / signal_s,
               ln_us / 1e6, 100.0 * ln_us / 1e6 / signal_s,
               lines ? (double)ln_us / (double)lines : 0.0,
               (unsigned long long)lines,
               (rs_us + pr_us) / 1e6, (rs_us + pr_us) / 1e6 / signal_s);
    }

    // Optional P6 dump of the decoded display frame for offline inspection
    // (signalstats / visual A-B against a reference).
    if (ppm) {
        FILE *p = fopen(ppm, "wb");
        if (p) {
            int fw = decoder->frame_width, fh = decoder->frame_height;
            fprintf(p, "P6\n%d %d\n255\n", fw, fh);
            fwrite(decoder->display_front, 1u,
                   (size_t)fw * 3u * (size_t)fh, p);
            fclose(p);
            printf("LIVE frame dump: %s (%dx%d RGB24)\n", ppm, fw, fh);
        } else {
            fprintf(stderr, "cannot write %s\n", ppm);
        }
    }

    // Per-bar measured U/V, inverted from the decoded RGB and averaged as
    // a VECTOR over many picture rows (both field weaves): line noise and
    // per-line V-switch flips shrink the vector mean, so the coherence
    // ratio |mean|/mean-amplitude exposes row disagreement (a coherent
    // decode reads ~1.0; half V-flipped rows read ~0). Clamped channels
    // still distort the per-row inversion (saturated bars underestimate
    // the clamped-off axis).
    static const char *bar_names[8] = {
        "gray?", "yellow", "cyan", "green", "magenta", "red", "blue", "black"
    };
    int row0 = 100;
    int row1 = decoder->frame_height - 100;
    for (int bar = 1; bar < 8; bar++) {
        double su = 0.0, sv = 0.0, sa = 0.0;
        int rows = 0;
        for (int row = row0; row <= row1; row++) {
            double r = frame_bar_mean(decoder, bar, row, 0);
            double g = frame_bar_mean(decoder, bar, row, 1);
            double b = frame_bar_mean(decoder, bar, row, 2);
            double dy = (g + 0.1944 * b + 0.5096 * r) / 1.7040;
            double u = (b - dy) / 2.032;
            double v = (r - dy) / 1.140;
            su += u;
            sv += v;
            sa += sqrt(u * u + v * v);
            rows++;
        }
        double mu = su / rows, mv = sv / rows, ma = sa / rows;
        double ang = atan2(mv, mu) * 180.0 / TEST_PI;
        if (ang < 0) ang += 360.0;
        double coh = ma > 1e-9 ? sqrt(mu * mu + mv * mv) / ma : 0.0;
        printf("LIVE bar %d (%s): U=%+.3f V=%+.3f angle=%.1f deg "
               "amp=%.3f coherence=%.2f (%d rows)\n",
               bar, bar_names[bar], mu, mv, ang, ma, coh, rows);
    }
    if (panel) {
#if LIBSOXR_ENABLED
        if (plugin.resample) soxr_delete(plugin.resample);
        free(plugin.resample_scratch);
        free(plugin.resample_f32);
#endif
        gui_cvbs_cleanup(&plugin.decoder);
    } else {
        gui_cvbs_cleanup(&stack_decoder);
    }
    free(sig);
}

static void apply_ac_coupling(int16_t *sig, size_t length, double rate, double corner) {
    // First-order AC coupling with the ADC centered at zero.
    double baseline = sig[0];
    double alpha = 1.0 - exp(-2 * TEST_PI * corner / rate);
    for (size_t i = 0; i < length; i++) {
        baseline += alpha * (sig[i] - baseline);
        sig[i] = (int16_t)lrint(sig[i] - baseline);
    }
}

static void run_50ohm_receiver(bool pal) {
    const double rate = 40000000.0;
    const double corner = 6000.0;
    size_t length;
    gen_cfg_t cfg = {pal, rate, 12, true, -1, &length};
    int16_t *sig = gen_baseband(&cfg);
    assert(sig && length > 0);
    apply_ac_coupling(sig, length, rate, corner);

    video_cvbs_plugin_t plugin = {0};
    assert(video_cvbs_plugin_init(&plugin));
    cvbs_decoder_t *decoder = &plugin.decoder;
    gui_cvbs_set_format(decoder, pal ? 0 : 1);
    assert(gui_cvbs_set_50ohms(decoder, true));
    for (size_t pos = 0; pos < length; ) {
        size_t count = length - pos;
        if (count > DISPLAY_SAMPLE_COUNT) count = DISPLAY_SAMPLE_COUNT;
        video_cvbs_plugin_process(&plugin, sig + pos, count, (uint32_t)rate);
        wait_for_cvbs_scout(decoder);
        pos += count;
    }
    gui_cvbs_swap_buffers(decoder);
    const cvbs_receiver_t *r = decoder->receiver;
    printf("%s 50 ohms: accepted=%d acquired=%d sync=%.0f Hz video=%.0f Hz\n",
           pal ? "PAL" : "NTSC", decoder->vsync.fields_accepted,
           r->sync_valid, r->sync_corner, r->corner);
    assert(decoder->pll.locked && decoder->vsync.fields_accepted >= 5);
    assert(decoder->vsync.fields_rejected == 0);
    assert(r->sync_valid && fabs(r->applied_corner - corner) < 0.25 * corner);
    assert(fabs(r->corner - corner) < 0.25 * corner);
    const int row = decoder->frame_height / 2;
    assert(frame_bar_mean(decoder, 4, row, 0) > frame_bar_mean(decoder, 7, row, 0) + 0.35);
    assert(gui_cvbs_set_50ohms(decoder, false) && !decoder->receiver);
    gui_cvbs_set_format(decoder, 2);
    assert(decoder->state.format == CVBS_FORMAT_SECAM && cvbs_system_option_count(decoder) == 3);
    assert(gui_cvbs_set_50ohms(decoder, true));
    assert(decoder->state.format == CVBS_FORMAT_PAL && cvbs_system_option_count(decoder) == 2);
    gui_cvbs_set_format(decoder, 2);
    assert(decoder->state.format == CVBS_FORMAT_PAL);
    assert(gui_cvbs_set_50ohms(decoder, false));
    gui_cvbs_set_format(decoder, 2);
    assert(decoder->state.format == CVBS_FORMAT_SECAM && cvbs_system_option_count(decoder) == 3);

#if LIBSOXR_ENABLED
    if (plugin.resample) soxr_delete(plugin.resample);
    free(plugin.resample_scratch);
    free(plugin.resample_f32);
#endif
    gui_cvbs_cleanup(decoder);
    free(sig);
}

static void run_50ohm_vblank_acquisition(void) {
    for (int format = 0; format < 2; format++) {
        const cvbs_baseband_profile_t *profile = &cvbs_baseband_profiles[format];
        const double period = profile->line_us * 40;
        const double half = period / 2;
        const int n = profile->eq_count;
        for (int parity = 0; parity < 2; parity++) {
            const double line_zero = 8 * period;
            const double first_eq = line_zero + profile->boundary_lines[parity] * period;
            const double first_post_h = first_eq + (3 * n - 1) * half + (parity + 1) * half;
            cvbs_sync_pulse_t pulses[32];
            int count = 0;
            for (int i = 4; i >= 1; i--) {
                pulses[count++] = (cvbs_sync_pulse_t){line_zero - (i - 1) * period, 9.4};
            }
            for (int i = 0; i < 3 * n; i++) {
                if (i == 1 || i == n + 2 || i == 2 * n + 1) continue;
                double width = (i >= n && i < 2 * n) ? 26.8 : 2.8;
                pulses[count++] = (cvbs_sync_pulse_t){first_eq + i * half, width};
            }
            for (int i = 0; i < 7; i++) {
                pulses[count++] = (cvbs_sync_pulse_t){first_post_h + i * period, 9.8};
            }
            cvbs_vblank_match_t match;
            assert(cvbs_match_vblank(pulses, count, period, profile, 0, &match));
            assert(match.parity == parity);
            assert(fabs(match.equalizing_start - first_eq) < 0.01);
            assert(fabs(match.line_zero - line_zero) < 0.01);

            // A cadence match before blanking cannot confirm field phase
            // without the first H reference after blanking.
            pulses[count - 7].width_us = 2.8;
            assert(!cvbs_match_vblank(pulses, count, period, profile, 0, &match));
        }
    }
}

// A sliced train starting one half-line late can mimic the opposite
// field. The raw pulse edges on both sides still identify the real one.
static void run_raw_field_phase(int samples_per_us) {
    cvbs_decoder_t decoder = {0};
    decoder.receiver = calloc(1, sizeof(*decoder.receiver));
    assert(decoder.receiver);
    for (int system = 0; system < 2; system++) {
        decoder.state.format = system ? CVBS_FORMAT_NTSC : CVBS_FORMAT_PAL;
        const cvbs_baseband_profile_t *p = cvbs_profile(&decoder);
        const double period = samples_per_us * p->line_us;
        for (int parity = 0; parity < 2; parity++) {
            double first = CVBS_VBLANK_SAMPLES - 2 * period;
            double previous = first - p->boundary_lines[parity] * period;
            double following = first + (3 * p->eq_count + parity) * period / 2;
            int16_t *raw = decoder.receiver->raw_vblank;
            memset(raw, 0, sizeof(decoder.receiver->raw_vblank));
            for (int j = -3; j < 3 * p->eq_count + 3; j++) {
                double at, width = p->h_pulse_us;
                if (j < 0) at = previous + (j + 1) * period;
                else if (j >= 3 * p->eq_count) at = following + (j - 3 * p->eq_count) * period;
                else {
                    at = first + j * period / 2;
                    width = j >= p->eq_count && j < 2 * p->eq_count ? p->broad_pulse_us : p->eq_pulse_us;
                }
                for (int x = (int)round(at); x < (int)round(at + samples_per_us * width); x++) {
                    raw[x & (CVBS_VBLANK_SAMPLES - 1)] = -10000;
                }
            }
            cvbs_vblank_match_t result;
            assert(cvbs_raw_boundary(&decoder, first + period / 2, period, (size_t)(first + 16 * period), &result, 0));
            assert(result.parity == parity && fabs(result.equalizing_start - first) < samples_per_us);
        }
    }
    free(decoder.receiver);
}

static void run_50ohm_scout_handoff(void) {
    const uint32_t rate = 40000000;
    size_t length;
    gen_cfg_t cfg = {false, rate, 8, true, -1, &length};
    int16_t *sig = gen_baseband(&cfg);
    assert(sig && length >= (size_t)rate / 20);
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, 1);
    decoder.rate_trusted = true;
    assert(gui_cvbs_set_50ohms(&decoder, true));

    // With both pre- and post-window samples in one block, the decoder
    // must finish that block on its existing separator.
    const size_t n = rate * 28u / 1000;
    gui_cvbs_process_buffer(&decoder, sig, n, rate);
    cvbs_receiver_t *r = decoder.receiver;
    assert(r->scout_pending && !r->sync_valid && decoder.global_sample_pos == n);
    assert(r->scout_end_pos < n && r->tail_weight > 0);
    wait_for_cvbs_scout(&decoder);
    assert(r->worker->found);
    gui_cvbs_process_buffer(&decoder, sig + n, 4096, rate);
    assert(r->sync_valid && r->sync_attempt_pos == n);

    gui_cvbs_reset(&decoder);
    gui_cvbs_process_buffer(&decoder, sig, n, rate);
    assert(r->scout_pending);
    gui_cvbs_set_format(&decoder, 0);  // Invalidate the in-flight NTSC scan.
    wait_for_cvbs_scout(&decoder);
    assert(r->worker->found);
    gui_cvbs_process_buffer(&decoder, sig, 4096, rate);
    assert(!r->sync_valid && !r->scout_pending && r->sync_attempt_pos == 0);

    gui_cvbs_cleanup(&decoder);
    free(sig);
}

static void run_50ohm_comb(bool pal, uint32_t rate) {
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    assert(gui_cvbs_set_50ohms(&decoder, true));
    const cvbs_baseband_profile_t *p = cvbs_profile(&decoder);
    decoder.sample_rate_hz = rate;
    int n = cvbs_us_to_samples(&decoder, p->line_us);
    const double us = cvbs_rate(&decoder) / 1e6;
    decoder.pll.line_period = n;
    decoder.pll.locked = true;
    decoder.pll.current_line = 30;
    int16_t samples[CVBS_LINE_BUFFER_SIZE];
    uint8_t rgb[CVBS_MAX_WIDTH * 3];
    for (int line = 0; line < 4; line++) {
        size_t start = (size_t)(line + 4) * n;
        decoder.global_sample_pos = start + n;
        for (int x = 0; x < n; x++) {
            double t = (x + CVBS_SYNC_EDGE_DELAY_SAMPLES) / us;
            double carrier = cos(2 * TEST_PI * p->fsc_hz * x / cvbs_rate(&decoder));
            // Identical monochrome stripes, with a phase-alternating burst.
            double value = t < 4.7 ? -4000 : 0;
            if (t > p->burst_start_us && t < p->burst_end_us)
                value = ((line / (pal ? 2 : 1)) % 2 ? 900 : -900) * carrier;
            if (t > 14 && t < 55) value = 3200 + 1200 * carrier;
            samples[x] = (int16_t)value;
            decoder.vsync.h_ring[(start + x) & (CVBS_H_RING - 1)] = samples[x];
        }
        decode_line_to_pixels(&decoder, samples, n, &decoder.levels, rgb, decoder.frame_width);
        double peak = 0;
        for (int x = n/3; x < n/2; x++) peak = fmax(peak, fabs(g_chroma_b[x]));
        if (line == 0) assert(peak > 100);  // No corrected partner yet.
        if (line >= (pal ? 2 : 1)) assert(peak < 2);
    }
    gui_cvbs_cleanup(&decoder);
}

// Two close picture edges (or half-line EQ pulses) must not drag the PLL
// off its established phase. A real splice supplies H on the new grid.
static void check_receiver_displaced_h(void) {
    cvbs_decoder_t dec;
    assert(gui_cvbs_init(&dec));
    assert(gui_cvbs_set_50ohms(&dec, true));
    double period = cvbs_nominal_line_period(&dec);
    cvbs_receiver_t *r = dec.receiver;
    assert(cvbs_receiver_h_confirmed(r, period, 0, period));
    assert(!cvbs_receiver_h_confirmed(r, 1.20 * period, 0.20 * period, period));
    assert(!cvbs_receiver_h_confirmed(r, 1.50 * period, 0.50 * period, period));
    assert(cvbs_receiver_h_confirmed(r, 2 * period, 0, period));
    assert(!cvbs_receiver_h_confirmed(r, 3.5 * period, 0.5 * period, period));
    assert(cvbs_receiver_h_confirmed(r, 4.5 * period, 0.5 * period, period));
    dec.pll.locked = dec.fixed_sync_locked = true;
    dec.fixed_line_period = dec.pll.line_period = period;
    dec.sync_mode = CVBS_SYNC_MODE_FIXED;
    pll_process_hsync(&dec, 0.5 * period);
    assert(dec.stream_jumps == 1);
    gui_cvbs_cleanup(&dec);
}

static void put_adc_pulse(cvbs_receiver_t *r, double pos, double width) {
    for (size_t k = (size_t)llround(pos); k < (size_t)llround(pos + width); k++)
        r->raw_vblank[k & (CVBS_VBLANK_SAMPLES - 1)] = -6000;
}

// A field prediction can locate a train while the horizontal slicer is
// displaced. Only the ADC waveform can establish its existence and parity.
static void check_displaced_vblank(bool pal, int parity, bool repeated) {
    cvbs_decoder_t dec;
    assert(gui_cvbs_init(&dec));
    gui_cvbs_set_format(&dec, pal ? 0 : 1);
    assert(gui_cvbs_set_50ohms(&dec, true));
    dec.sample_rate_hz = 16000000;
    const cvbs_baseband_profile_t *p = cvbs_profile(&dec);
    cvbs_receiver_t *r = dec.receiver;
    r->sync_valid = true;
    r->sync_span = 6000;
    dec.vsync.have_field = true;
    dec.vsync.field_parity = repeated ? parity : !parity;
    dec.vsync.field_start = 10000;
    double us = cvbs_rate(&dec) / 1e6, period = us * p->line_us;
    double zero = dec.vsync.field_start + p->field_cadence_lines * period;
    double eq = zero + p->boundary_lines[parity] * period;
    size_t now = (size_t)(eq + (CVBS_RAW_TRAIN_DELAY_LINES + 0.25) * period);
    cvbs_vblank_match_t m;
    assert(!cvbs_raw_train(&dec, now, period, &m));
    for (int j = 0; j < 3 * p->eq_count; j++) {
        double width = j >= p->eq_count && j < 2 * p->eq_count ? p->broad_pulse_us : p->eq_pulse_us;
        put_adc_pulse(r, eq + j * period / 2, width * us);
    }
    assert(!cvbs_raw_train(&dec, now, period, &m));
    for (int j = 0; j < 4; j++) {
        put_adc_pulse(r, zero - j * period, p->h_pulse_us * us);
        put_adc_pulse(r, eq + (3 * p->eq_count + parity + 2 * j) * period / 2, p->h_pulse_us * us);
    }
    assert(cvbs_raw_train(&dec, now, period, &m));
    assert(m.parity == parity && fabs(m.line_zero - zero) < us);
    cvbs_recover_field(&dec, r, &m, period, now);
    assert(dec.vsync.fields_accepted == !repeated && dec.vsync.reject_parity == repeated);
    assert(cvbs_raw_post_eq(r, p, eq, period));
    // Exercise both matchers independently with the flanking H still present.
    size_t cut = (size_t)llround(eq + (p->eq_count + 2) * period / 2);
    size_t end = (size_t)llround(eq + 3 * p->eq_count * period / 2);
    for (int pattern = 0; pattern < 2; pattern++) {
        for (size_t pos = cut; pos < end; pos++) {
            double phase = (pos - eq) / period;
            double sample = pattern ? 6000 * cos(2 * TEST_PI * 16 * phase) :
                (fmod(phase, 0.5) * period < 2.3 * us ? -1200 : 0);
            r->raw_vblank[pos & (CVBS_VBLANK_SAMPLES - 1)] = (int16_t)lrint(sample);
        }
        assert(!cvbs_raw_post_eq(r, p, eq, period));
        assert(!cvbs_raw_boundary(&dec, eq, period, now, &m, 0.15 * r->sync_span));
    }
    gui_cvbs_cleanup(&dec);
}

static void run_rejected_field_scout(void) {
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, 1);
    assert(gui_cvbs_set_50ohms(&decoder, true));
    decoder.receiver->sync_valid = true;
    decoder.vsync.have_field = true;
    decoder.vsync.field_parity = 0;
    double period = cvbs_nominal_line_period(&decoder);
    decoder.pll.line_period = period;
    static const int16_t samples[4096];
    for (int i = 1; i <= 20; i++) {
        size_t zero = (size_t)(i * 262.5 * period);
        size_t eq = zero + (size_t)period;
        decoder.global_sample_pos = eq + (size_t)(14 * period);
        cvbs_vbi_first_eq(&decoder, eq, zero, decoder.global_sample_pos);
        cvbs_receiver_feed_scout(&decoder, decoder.receiver, samples, 4096, cvbs_rate(&decoder));
    }
    assert(decoder.vsync.fields_rejected == 20 && decoder.receiver->anchored_pos == 0);
    assert(decoder.receiver->scout_count > 0);
    gui_cvbs_cleanup(&decoder);
}

// Drive the same resampler and display-sized chunks as the Video panel.
static void feed_cvbs_40mhz(gui_cvbs_feed_t *feed, const int16_t *samples, size_t length) {
    for (size_t pos = 0; pos < length; ) {
        size_t chunk = length - pos;
        if (chunk > DISPLAY_SAMPLE_COUNT) chunk = DISPLAY_SAMPLE_COUNT;
        gui_cvbs_feed_process(feed, samples + pos, chunk, 40000000);
        wait_for_cvbs_scout(gui_cvbs_feed_decoder(feed));
        pos += chunk;
    }
}

// Exercise the panel resampler, slicer, PLL and boundary publication together.
static void check_receiver_sync_continuity(bool pal) {
    enum { narrow_step, mixed_step, gap_step, wide_first_gap, reverse_step, reverse_gap_step, reverse_wide_first_gap,
           stable_h, interfering_pair, full_train, truncated, single_post_eq,
           truncated_with_sine, truncated_with_pulses, truncated_with_16h, weak_noisy_tail };
    const cvbs_baseband_profile_t *profile = &cvbs_baseband_profiles[pal ? 0 : 1];
    size_t line = (size_t)llround(profile->line_us * 40);
    size_t frame = (profile->field_lines[0] + profile->field_lines[1]) * line;
    double reference_phase = 0;
    size_t length;
    gen_cfg_t cfg = {pal, 40000000, 10, false, -1, &length};
    int16_t *original = gen_baseband(&cfg);
    int16_t *samples = malloc(length * sizeof(*samples));
    assert(samples);
    for (int i_case = narrow_step; i_case < weak_noisy_tail + 80; i_case++) {
        int scenario = i_case < weak_noisy_tail ? i_case : weak_noisy_tail;
        gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
        assert(feed);
        gui_cvbs_feed_set_system(feed, pal ? 0 : 1);
        cvbs_decoder_t *d = gui_cvbs_feed_decoder(feed);
        assert(gui_cvbs_set_50ohms(d, true));
        bool test_h = scenario <= interfering_pair;
        bool displaced = scenario <= reverse_wide_first_gap;
        size_t field = (displaced ? 4 : 2) * frame;
        size_t begin = field + (test_h ? 40 * line : 0);
        size_t span = (test_h ? 60 : 40) * line;
        assert(begin + span <= length);
        memcpy(samples + begin, original + begin, span * sizeof(*samples));
        feed_cvbs_40mhz(feed, original, begin);
        assert(d->pll.locked && d->vsync.fields_accepted > 0);
        int jumps = d->stream_jumps;
        int accepted = d->vsync.fields_accepted;
        int rejected = d->vsync.fields_rejected;
        if (test_h) {
            bool reverse = scenario >= reverse_step && displaced;
            size_t edge = displaced ? (size_t)ceil((reverse ? 0.6 : 0.4) * line) : 0;
            size_t interference = (size_t)ceil(0.39 * line);
            for (size_t row = 0; row < span / line; row++) {
                bool wide = (scenario == mixed_step && (row & 1)) || (!displaced && row == 13);
                size_t width = wide ? 400 : 188;  // 10 or 4.7 us
                bool wide_first = scenario == wide_first_gap || scenario == reverse_wide_first_gap;
                if (wide_first || scenario == gap_step || scenario == reverse_gap_step) {
                    static const size_t widths[] = {188, 400, 0};
                    size_t slot = row % 3;
                    if (wide_first && slot < 2) slot = 1 - slot;
                    width = widths[slot];
                }
                for (size_t x = 0; x < line; x++) {
                    bool tip = x >= edge && x < edge + width;
                    if (scenario == interfering_pair && (row == 12 || row == 13) &&
                        x >= interference && x < interference + width) tip = true;
                    samples[begin + row * line + x] =
                        (int16_t)(LEVEL_ZERO + (tip ? SYNC_TIP_IRE * COUNTS_PER_IRE : 0));
                }
            }
        } else {
            size_t eq = field + (size_t)llround(profile->boundary_lines[0] * line);
            size_t tail = eq + profile->eq_count * line;
            size_t end = tail + profile->eq_count * line / 2;
            if (scenario == weak_noisy_tail) {
                int noise_case = i_case - weak_noisy_tail;
                uint32_t seed = 12345 + 427 * (noise_case % 16);
                double amplitude = 240 + 180 * (noise_case / 16);
                double noise = 0;
                for (size_t i = tail; i < end; i++) {
                    seed = seed * 1664525u + 1013904223u;
                    noise += 0.065 * ((seed >> 8) / 8388608.0 - 1 - noise);
                    samples[i] = (int16_t)lrint(LEVEL_ZERO +
                        0.1 * (samples[i] - LEVEL_ZERO) + amplitude * noise);
                }
            } else if (scenario >= truncated) {
                size_t cut = eq + (profile->eq_count + 2) * line / 2;
                for (size_t i = cut; i < end; i++) {
                    if (scenario == single_post_eq && i >= tail && i < tail + line / 2) continue;
                    double phase = 2 * TEST_PI * 14 * (i - cut) / line + TEST_PI / 3;
                    double signal = 0;
                    if (scenario == truncated_with_sine) signal = 240 * sin(phase);
                    if (scenario == truncated_with_16h)
                        signal = 1260 * cos(2 * TEST_PI * 16 * (i - eq) / line);
                    if (scenario == truncated_with_pulses &&
                        fmod((double)(i - eq), line / 2.0) < 2.3 * 40) signal = -240;
                    samples[i] = (int16_t)lrint(LEVEL_ZERO + signal);
                }
            }
        }
        feed_cvbs_40mhz(feed, samples + begin, span);
        if (test_h) {
            assert(d->pll.locked && d->stream_jumps - jumps == displaced);
            double phase = d->pll.phase / d->pll.line_period;
            if (scenario == narrow_step || scenario == reverse_step || scenario == stable_h) reference_phase = phase;
            else assert(fabs(remainder(phase - reference_phase, 1)) < 0.02);
        } else {
            bool intact = scenario == full_train || scenario == weak_noisy_tail;
            assert(d->vsync.fields_accepted - accepted == intact);
            if (intact) assert(d->vsync.fields_rejected == rejected);
        }
        gui_cvbs_feed_destroy(feed);
    }
    free(original);
    free(samples);
}

static void run_fixed_rgb_black(void) {
    size_t length;
    gen_cfg_t cfg = {false, 40000000, 12, false, -1, &length};
    int16_t *samples = gen_baseband(&cfg);
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(samples && feed);
    cvbs_decoder_t *d = gui_cvbs_feed_decoder(feed);
    gui_cvbs_feed_set_system(feed, 1);
    assert(gui_cvbs_set_50ohms(d, true));
    feed_cvbs_40mhz(feed, samples, length);
    gui_cvbs_swap_buffers(d);
    cvbs_decoder_set_level_mode(d, CVBS_LEVEL_MODE_FIXED, "RGB black test");
    double gain = d->receiver->fixed_gain_span;
    const int before = d->state.frames_decoded;
    assert(gain > 0 && before > 0);

    // Keep black exactly at the local porch on the stressed lines. The
    // whole 20-line stripe has twice the swing and a shifted DC baseline.
    size_t line = (size_t)llround(cvbs_profile(d)->line_us * 40);
    for (size_t pos = 0; pos < length; pos += line) {
        size_t row = pos / line % 525;
        if (!((row >= 110 && row < 130) || (row >= 373 && row < 393))) continue;
        for (size_t x = 0; x < line; x++) {
            samples[pos + x] = (int16_t)(LEVEL_ZERO + 1600 + 2 * (samples[pos + x] - LEVEL_ZERO));
        }
    }
    feed_cvbs_40mhz(feed, samples, length);
    gui_cvbs_swap_buffers(d);
    assert(d->state.frames_decoded > before && d->receiver->fixed_gain_span == gain);
    for (int ch = 0; ch < 3; ch++) {
        double black = frame_bar_mean(d, 7, 200, ch);
        double stripe = frame_bar_mean(d, 7, 240, ch);
        assert(black < 0.1 && fabs(stripe - black) < 0.025);
    }
    assert(frame_bar_mean(d, 1, 240, 0) > 0.5);
    gui_cvbs_feed_destroy(feed);
    free(samples);
}

static void offset_field_stripe(int16_t *samples, size_t length, size_t line,
                               const cvbs_baseband_profile_t *profile, int first_row, int offset) {
    for (size_t pos = 0; pos + line <= length; pos += line) {
        int row = (int)(pos / line % (profile->field_lines[0] + profile->field_lines[1]));
        if (row >= profile->field_lines[0]) row -= profile->field_lines[0];
        if (row >= first_row && row < first_row + 20) {
            for (size_t x = 0; x < line; x++) samples[pos + x] += offset;
        }
    }
}

// Check black at the beginning, middle and end of the displayed picture
// while a fixed-gain receiver is actively undoing AC coupling.
static void run_coupled_rgb_black(bool pal) {
    size_t length;
    gen_cfg_t cfg = {pal, 40000000, 12, false, -1, &length};
    int16_t *samples = gen_baseband(&cfg);
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(samples && feed);
    cvbs_decoder_t *d = gui_cvbs_feed_decoder(feed);
    gui_cvbs_feed_set_system(feed, pal ? 0 : 1);
    assert(gui_cvbs_set_50ohms(d, true));
    const cvbs_baseband_profile_t *profile = cvbs_profile(d);
    size_t line = (size_t)llround(profile->line_us * 40);
    double baseline = samples[0];
    double alpha = 1 - exp(-2 * TEST_PI * 6000 / cfg.rate);
    for (size_t i = 0; i < length; i++) {
        size_t x = i % line;
        int row = (int)(i / line % (profile->field_lines[0] + profile->field_lines[1]));
        if (row >= profile->field_lines[0]) row -= profile->field_lines[0];
        if (row >= 100 && row < 140) {
            // Leave bright bars between the three black patches.
            if ((x >= 500 && x < 720) || (x >= 1290 && x < 1520) || (x >= 2220 && x < 2460))
                samples[i] = (int16_t)LEVEL_ZERO;
        }
        baseline += alpha * (samples[i] - baseline);
        samples[i] = (int16_t)lrint(samples[i] - baseline);
    }
    feed_cvbs_40mhz(feed, samples, length);
    gui_cvbs_swap_buffers(d);
    cvbs_decoder_set_level_mode(d, CVBS_LEVEL_MODE_FIXED, "coupled RGB black");
    double gain = d->receiver->fixed_gain_span;
    assert(gain > 0 && d->receiver->corner > 4000);
    const double times[] = {15, 35, 58};
    int bottom = profile->picture_start_line + profile->picture_lines - 21;
    const struct { int row, offset; } bursts[] = {
        {110, 400}, {110, 800}, {110, -400}, {110, -800},
        {185, -800}, {218, -800}, {bottom, 400},
        {bottom, 800}, {bottom, -800}, {bottom, 800}, {bottom, -800}, {0, -800}
    };
    for (size_t pass = 0; pass < sizeof(bursts) / sizeof(bursts[0]); pass++) {
        if (pass) offset_field_stripe(samples, length, line, profile, bursts[pass-1].row, -bursts[pass-1].offset);
        offset_field_stripe(samples, length, line, profile, bursts[pass].row, bursts[pass].offset);
        // Settle after replaying the fixture, then count six uninterrupted
        // fields. A held frame or overflow fallback cannot hide a lost VBI.
        size_t half = length / 2;
        feed_cvbs_40mhz(feed, samples, half);
        int frames = d->state.frames_decoded;
        int accepted = d->vsync.fields_accepted;
        int rejected = d->vsync.fields_rejected;
        if (pass && bursts[pass].row == bottom && bursts[pass - 1].row == bottom &&
            bursts[pass].offset == -bursts[pass - 1].offset) {
            gui_cvbs_swap_buffers(d);
            size_t pos = half;
            for (int field = 0; field < 6; field++) {
                int prior_parity = d->vsync.field_parity;
                size_t count = (size_t)profile->field_lines[field & 1] * line;
                feed_cvbs_40mhz(feed, samples + pos, count);
                pos += count;
                assert(d->pll.locked && d->vsync.have_field && !d->hold_frame_for_relock);
                assert(d->vsync.field_parity != prior_parity);
                assert(d->vsync.fields_accepted == accepted + field + 1);
                assert(d->state.frames_decoded == frames + field + 1);
                assert(atomic_load(&d->display_ready));
                gui_cvbs_swap_buffers(d);
            }
            assert(pos == length);
        } else {
            feed_cvbs_40mhz(feed, samples + half, length - half);
        }
        gui_cvbs_swap_buffers(d);
        assert(d->vsync.fields_accepted == accepted + 6);
        assert(d->vsync.fields_rejected == rejected);
        assert(d->state.frames_decoded == frames + 6 && d->receiver->corner > 4000);
        assert(d->receiver->fixed_gain_span == gain);
        if (bursts[pass].row >= profile->picture_start_line) {
            // Probe both field parities in the offset band, including the
            // bottom rows previously excluded from DC ramp correction.
            for (int row = 2 * (bursts[pass].row + 11); row < 2 * (bursts[pass].row + 18); row++) {
                for (int ch = 0; ch < 3; ch++) {
                    double black = frame_bar_mean(d, 7, row, ch);
                    double reference = frame_bar_mean(d, 7, 180, ch);
                    assert(reference < 0.1 && fabs(black - reference) < 0.025);
                }
                assert(frame_bar_mean(d, 1, row, 0) > 0.4);
            }
        }
        for (int i = 0; i < 3; i++) {
            for (int ch = 0; ch < 3; ch++) {
                double black = frame_time_px_mean(d, times[i], 210, ch);
                double stripe = frame_time_px_mean(d, times[i], 240, ch);
                assert(black < 0.1 && fabs(stripe - black) < 0.025);
            }
        }
        assert(frame_time_px_mean(d, 22, 240, 0) > 0.4);
    }
    gui_cvbs_feed_destroy(feed);
    free(samples);
}

// No clean pre-roll: the very first vertical interval must be acquired
// from the same offset-burdened input as every subsequent field.
static void run_cold_field_end_offset(bool pal, int corner_hz, int damaged_field) {
    size_t length;
    gen_cfg_t cfg = {pal, 40000000, damaged_field < 0 ? 48 : damaged_field + 6,
                     false, damaged_field, &length};
    int16_t *samples = gen_baseband(&cfg);
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(samples && feed);
    gui_cvbs_feed_set_system(feed, pal ? 0 : 1);
    cvbs_decoder_t *d = gui_cvbs_feed_decoder(feed);
    assert(gui_cvbs_set_50ohms(d, true));
    cvbs_decoder_set_level_mode(d, CVBS_LEVEL_MODE_FIXED, "cold field-end offset");
    const cvbs_baseband_profile_t *profile = cvbs_profile(d);
    size_t line = (size_t)llround(profile->line_us * cfg.rate / 1e6);
    apply_ac_coupling(samples, length, cfg.rate, corner_hz);
    offset_field_stripe(samples, length, line, profile,
                        profile->picture_start_line + profile->picture_lines - 21, -800);

    int first = -1;
    size_t pos = 0;
    for (int field = 0; field < cfg.fields; field++) {
        size_t count = line * profile->field_lines[field & 1];
        int frames = d->state.frames_decoded;
        int accepted = d->vsync.fields_accepted;
        int parity = d->vsync.field_parity;
        feed_cvbs_40mhz(feed, samples + pos, count);
        pos += count;
        assert(d->vsync.vsync_candidates == d->vsync.fields_accepted + d->vsync.fields_rejected);
        if (damaged_field >= 0) {
            // Blanking one VBI leaves a two-field gap at the next anchor.
            // A one-field display bridge may run, but neither boundary is accepted.
            int gaps = field > damaged_field ? 1 : 0;
            assert(d->vsync.fields_rejected == gaps && d->vsync.reject_cadence == gaps);
            if (field == damaged_field || field == damaged_field + 1) {
                assert(d->vsync.fields_accepted == accepted);
                gui_cvbs_swap_buffers(d);
                continue;
            }
        }
        if (first < 0 && !atomic_load(&d->display_ready)) continue;
        assert(atomic_load(&d->display_ready));
        assert(d->pll.locked && d->vsync.have_field && !d->hold_frame_for_relock);
        assert(d->vsync.fields_accepted == accepted + 1 && d->state.frames_decoded == frames + 1);
        if (first >= 0) assert(d->vsync.field_parity != parity);
        else {
            first = field;
            // The live fit can eventually mask a bad scout choice. Check the
            // startup estimate itself to within one 2 kHz search step.
            if (pal && corner_hz == 8000)
                assert(fabs(d->receiver->sync_corner - corner_hz) <= 2000);
        }
        gui_cvbs_swap_buffers(d);
        assert(frame_bar_mean(d, 1, 240, 0) > frame_bar_mean(d, 7, 240, 0) + 0.35);
    }
    printf("%s %d Hz cold offset: first field %d, displayed %d, rejected %d\n",
           pal ? "PAL" : "NTSC", corner_hz, first + 1, d->state.frames_decoded, d->vsync.fields_rejected);
    assert(pos == length && first >= 0 && first < 10);
    assert(d->vsync.fields_rejected == (damaged_field >= 0 ? 1 : 0));
    gui_cvbs_feed_destroy(feed);
    free(samples);
}

static void run_repeated_field_counts(void) {
    size_t length;
    gen_cfg_t cfg = {false, 40000000, 2, false, -1, &length};
    int16_t *samples = gen_baseband(&cfg);
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(samples && feed);
    cvbs_decoder_t *d = gui_cvbs_feed_decoder(feed);
    gui_cvbs_feed_set_system(feed, 1);
    assert(gui_cvbs_set_50ohms(d, true));
    size_t first_field = length / 525 * 263;
    for (int i = 0; i < 12; i++) feed_cvbs_40mhz(feed, samples, first_field);
    int rejected = d->vsync.reject_parity;
    assert(d->pll.locked && rejected > 0);
    for (int i = 0; i < 8; i++) feed_cvbs_40mhz(feed, samples, first_field);
    assert(d->vsync.fields_accepted == 0 && d->state.frames_decoded == 0);
    assert(d->vsync.reject_parity == rejected + 8 && d->vsync.reject_cadence == 0);
    assert(d->vsync.fields_rejected == d->vsync.reject_parity);
    for (int i = 0; i < 4; i++) feed_cvbs_40mhz(feed, samples, length);
    assert(d->vsync.fields_accepted >= 6 && d->state.frames_decoded >= 6);
    gui_cvbs_feed_destroy(feed);
    free(samples);
}

static void run_50ohm_fixed_levels(bool pal) {
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    assert(gui_cvbs_set_50ohms(&decoder, true));
    decoder.pll.locked = true;
    decoder.pll.current_line = 30;

    const cvbs_baseband_profile_t *profile = cvbs_profile(&decoder);
    const int count = cvbs_us_to_samples(&decoder, profile->line_us);
    const int tip_end = cvbs_us_to_samples(&decoder, profile->h_pulse_us) - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    const int active_start = cvbs_us_to_samples(&decoder, profile->active_start_us);
    const int active_end = cvbs_us_to_samples(&decoder, profile->active_end_us);
    const int picture = (active_start + active_end) / 2;
    const int porch = active_start - CVBS_SYNC_EDGE_DELAY_SAMPLES - 10;
    int16_t samples[CVBS_LINE_BUFFER_SIZE], restored[CVBS_LINE_BUFFER_SIZE];
    cvbs_levels_t levels = {0};
    for (int x = 0; x < count; x++) {
        samples[x] = x < tip_end ? -4000 : x >= active_start && x < active_end ? 3000 : 0;
    }
    assert(cvbs_receiver_restore(&decoder, samples, count, restored, &levels));
    int initial_luma = restored[picture];
    cvbs_decoder_set_level_mode(&decoder, CVBS_LEVEL_MODE_FIXED, "test");
    assert(decoder.receiver->fixed_gain_span == 4000);

    // Moving the input baseline leaves the displayed black and luma alone.
    for (int x = 0; x < count; x++) samples[x] += 1200;
    assert(cvbs_receiver_restore(&decoder, samples, count, restored, &levels));
    assert(restored[porch] == 0 && restored[picture] == initial_luma);

    // Raising the input amplitude changes the luma by the same ratio while
    // Fixed holds the output gain. Keep collecting span measurements for Auto.
    for (int x = 0; x < count; x++) {
        samples[x] = (int16_t)(1600 + (samples[x] - 1200) * 5 / 4);
    }
    for (int line = 0; line < 6; line++) {
        assert(cvbs_receiver_restore(&decoder, samples, count, restored, &levels));
        assert(restored[porch] == 0);
        assert(abs(restored[picture] - initial_luma * 5 / 4) <= 1);
    }

    cvbs_decoder_set_level_mode(&decoder, CVBS_LEVEL_MODE_AUTO, "test");
    assert(decoder.receiver->fixed_gain_span == 0);
    assert(cvbs_receiver_restore(&decoder, samples, count, restored, &levels));
    assert(restored[porch] == 0 && restored[picture] == initial_luma);
    cvbs_decoder_set_level_mode(&decoder, CVBS_LEVEL_MODE_FIXED, "test");
    assert(decoder.receiver->fixed_gain_span == 5000);
    gui_cvbs_cleanup(&decoder);
}

// Model an unplug/replug with constant input during the outage. The
// returning ADC signal sits entirely above the pre-outage sync threshold.
static void run_signal_recovery(bool receiver) {
    const double rate = 40000000.0;
    size_t length;
    gen_cfg_t cfg = {false, rate, 12, true, -1, &length};
    int16_t *signal = gen_baseband(&cfg);
    assert(signal && length > 0);
    for (size_t i = 0; i < length; i++) signal[i] -= 14000;

    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, 1);
    decoder.sync_mode = CVBS_SYNC_MODE_FIXED;
    assert(gui_cvbs_set_50ohms(&decoder, receiver));
    run_decoder(&decoder, signal, length, rate, true);
    assert(decoder.pll.locked && decoder.state.frames_decoded > 0);

    cvbs_decoder_set_level_mode(&decoder, CVBS_LEVEL_MODE_FIXED, "recovery test");
    assert(decoder.fixed_levels_valid);
    int16_t black = decoder.fixed_black_level, white = decoder.fixed_white_level;
    double locked_span = 0;
    if (receiver) {
        locked_span = decoder.receiver->fixed_gain_span;
        assert(locked_span > 0);
    }

    // Carry both kinds of diagnostic history through a real signal timeout.
    decoder.vsync.reject_cadence += 2;
    decoder.vsync.reject_parity++;
    decoder.vsync.fields_rejected += 3;
    decoder.vsync.vsync_candidates += 3;
    strcpy(decoder.vsync.last_reject_reason, "vertical fields repeat parity");
    int accepted_before_loss = decoder.vsync.fields_accepted;
    static const int16_t no_signal[8192];
    // Wait beyond the 50 ms loss timeout. A field already in progress
    // at the splice can still finish before the outage is detected.
    for (int i = 0; i < 300; i++) {
        int accepted = decoder.vsync.fields_accepted;
        int rejected = decoder.vsync.fields_rejected;
        int cadence = decoder.vsync.reject_cadence;
        int parity = decoder.vsync.reject_parity;
        int candidates = decoder.vsync.vsync_candidates;
        gui_cvbs_process_buffer(&decoder, no_signal, 8192, (uint32_t)rate);
        assert(decoder.vsync.fields_accepted >= accepted && decoder.vsync.fields_rejected >= rejected);
        assert(decoder.vsync.reject_cadence >= cadence && decoder.vsync.reject_parity >= parity);
        assert(decoder.vsync.vsync_candidates >= candidates && decoder.vsync.last_reject_reason[0]);
    }
    gui_cvbs_swap_buffers(&decoder);
    assert(!decoder.pll.locked);
    if (receiver) assert(decoder.receiver->fixed_gain_span == locked_span);
    const size_t frame_bytes = (size_t)decoder.frame_width * decoder.frame_height * 3u;
    uint8_t *frozen = malloc(frame_bytes);
    assert(frozen);
    memcpy(frozen, decoder.display_front, frame_bytes);

    for (int i = 0; i < 700; i++) {
        gui_cvbs_process_buffer(&decoder, no_signal, 8192, (uint32_t)rate);
    }
    gui_cvbs_swap_buffers(&decoder);
    assert(memcmp(frozen, decoder.display_front, frame_bytes) == 0);

    free(signal);
    cfg.with_burst = false;
    signal = gen_baseband(&cfg);
    assert(signal && length > 0);
    for (size_t i = 0; i < length; i++) signal[i] += 2000;
    const int completed = decoder.state.frames_decoded;
    run_decoder(&decoder, signal, length, rate, true);
    gui_cvbs_swap_buffers(&decoder);
    fprintf(stderr, "CVBS recovery, receiver=%d: H age=%zu accepted=%d completed=%d/%d\n",
            receiver, decoder.global_sample_pos - decoder.vsync.last_h_pos,
            decoder.vsync.fields_accepted, completed, decoder.state.frames_decoded);
    assert(decoder.pll.locked);
    assert(decoder.global_sample_pos - decoder.vsync.last_h_pos < 40000);
    assert(decoder.vsync.fields_accepted > accepted_before_loss);
    assert(decoder.state.frames_decoded > completed);
    assert(decoder.fixed_levels_valid && decoder.fixed_black_level == black && decoder.fixed_white_level == white);
    assert(memcmp(frozen, decoder.display_front, frame_bytes) != 0);
    if (receiver) {
        // H survives a missing vertical interval. Permit one predicted field,
        // then require verified boundaries before publishing again.
        const size_t line = (size_t)round(cvbs_profile(&decoder)->line_us * rate / 1e6);
        const int16_t *active_line = signal + 36 * line;
        for (int i = 0; i < 900; i++) {
            gui_cvbs_process_buffer(&decoder, active_line, line, (uint32_t)rate);
            wait_for_cvbs_scout(&decoder);
        }
        assert(decoder.pll.locked && decoder.hold_frame_for_relock);
        gui_cvbs_swap_buffers(&decoder);
        memcpy(frozen, decoder.display_front, frame_bytes);
        int frames = decoder.state.frames_decoded;
        int accepted = decoder.vsync.fields_accepted;
        for (int i = 0; i < 900; i++) {
            gui_cvbs_process_buffer(&decoder, active_line, line, (uint32_t)rate);
            wait_for_cvbs_scout(&decoder);
        }
        gui_cvbs_swap_buffers(&decoder);
        assert(decoder.state.frames_decoded == frames && decoder.vsync.fields_accepted == accepted);
        assert(memcmp(frozen, decoder.display_front, frame_bytes) == 0);
        run_decoder(&decoder, signal, length, rate, true);
        assert(!decoder.hold_frame_for_relock && decoder.state.frames_decoded > frames);
        assert(decoder.vsync.fields_accepted > accepted);
        assert(decoder.receiver->fixed_gain_span == locked_span);
        gui_cvbs_process_buffer(&decoder, signal, 256, 80000000);
        assert(decoder.receiver->fixed_gain_span == locked_span);
    }
    gui_cvbs_cleanup(&decoder);
    free(frozen);
    free(signal);
}

static void run_wide_hsync(bool cold) {
    const int line = 2542;  // NTSC at 40 MSPS
    int16_t samples[2542];
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, 1);
    assert(gui_cvbs_set_50ohms(&decoder, true));
    double baseline = 0;
    const double alpha = 1 - exp(-2 * TEST_PI * 10000 / 40000000);
    for (int field = 0; field < 16; field++) {
        int first_eq = field % 2 ? line / 2 : line;
        int h_width = !cold && field < 5 ? 188 : 392;  // 4.7 then 9.8 us
        for (int y = 0; y < (field % 2 ? 262 : 263); y++) {
            for (int x = 0; x < line; x++) {
                int offset = y * line + x - first_eq;
                if (offset >= 0 && offset < 9 * line) {
                    int width = offset >= 3 * line && offset < 6 * line ? 1072 : 112;
                    samples[x] = offset % (line / 2) < width ? -16000 : 0;
                } else {
                    samples[x] = x < h_width ? -16000 : (y >= 10 && x >= 560 && x < 2240 ? 5000 : 0);
                }
                if (cold) {
                    baseline += alpha * (samples[x] - baseline);
                    samples[x] = (int16_t)lrint(samples[x] - baseline);
                }
            }
            gui_cvbs_process_buffer(&decoder, samples, line, 40000000);
            wait_for_cvbs_scout(&decoder);
        }
    }
    fprintf(stderr, "Wide H cold=%d: acquired=%d locked=%d accepted=%d frames=%d\n", cold, decoder.receiver->sync_valid, decoder.pll.locked, decoder.vsync.fields_accepted, decoder.state.frames_decoded);
    if (cold) {
        assert(decoder.receiver->sync_valid);
        // The generator's sync tip is 16000 counts below blank. Recover that
        // swing even though the wide tip occupies the nominal back porch.
        assert(fabs(decoder.receiver->gain_span - 16000) < 2400);
    }
    assert(decoder.pll.locked && decoder.vsync.fields_accepted >= 8);
    assert(decoder.state.frames_decoded >= 8);
    gui_cvbs_cleanup(&decoder);
}

// Check the displayed buffer at every publication opportunity, including
// the first free-running field after switching off the corrected receiver.
static void check_sparse_raw_sync(cvbs_decoder_t *decoder, size_t line, uint32_t rate) {
    const size_t bytes = (size_t)decoder->frame_width * decoder->frame_height * 3;
    uint8_t *saved = malloc(bytes);
    assert(saved);
    memcpy(saved, decoder->display_front, bytes);
    int frames = decoder->state.frames_decoded;
    int16_t pulse[2560] = {0}, quiet[2560] = {0};
    assert(line <= sizeof(pulse) / sizeof(*pulse));
    for (int i = 0; i < 188; i++) pulse[i] = -12000;
    for (int i = 0; i < 1200; i++) {
        gui_cvbs_process_buffer(decoder, i % 19 == 1 ? pulse : quiet, line, rate);
        if (atomic_load(&decoder->display_ready)) {
            gui_cvbs_swap_buffers(decoder);
            assert(memcmp(saved, decoder->display_front, bytes) == 0);
        }
    }
    assert(memcmp(saved, decoder->display_front, bytes) == 0);
    assert(decoder->state.frames_decoded == frames);
    free(saved);
}

// Five bright stripes interrupted by blanking. The dark stripes are wide
// enough to pass the pulse-width check even when the real H tip is absent.
static int16_t cvbs_test_stripes(unsigned x, unsigned line, bool with_h) {
    if (with_h && x < 188) return -6000;
    bool active = x > 400 && x < line - 80;
    bool bright = (x * 5 + line * 21 / 100) % line < line * 7 / 10;
    return active && bright ? 20000 : 2000;
}

// Keep a known-good line clock, then place the fixed slice below the ADC tip.
static void check_unsliced_widths(cvbs_decoder_t *decoder, size_t line, uint32_t rate) {
    const int widths[] = {148, 188, 240, 280}; // 3.7, 4.7, 6.0, 7.0 us at 40 MSPS
    const size_t bytes = (size_t)decoder->frame_width * decoder->frame_height * 3;
    uint8_t *previous = malloc(bytes);
    assert(previous);
    int16_t samples[2560];
    assert(line <= sizeof(samples) / sizeof(*samples));
    int16_t threshold = decoder->fixed_sync_threshold;
    for (size_t w = 0; w < sizeof(widths) / sizeof(*widths); w++) {
        for (size_t x = 0; x < line; x++) samples[x] = (int)x < widths[w] ? -6000 : 2000;
        decoder->fixed_sync_locked = true;
        decoder->fixed_sync_threshold = -2000;
        for (int i = 0; i < 100; i++) gui_cvbs_process_buffer(decoder, samples, line, rate);
        assert(decoder->global_sample_pos - decoder->vsync.last_h_pos <= 2 * line);
        decoder->fixed_sync_threshold = -30000;
        size_t sliced = decoder->vsync.last_h_pos;
        // Acquire independent ADC coverage before measuring steady field cadence.
        for (int i = 0; i < 80; i++) gui_cvbs_process_buffer(decoder, samples, line, rate);
        assert(cvbs_recent_adc_h(decoder, decoder->global_sample_pos));
        gui_cvbs_swap_buffers(decoder);
        memcpy(previous, decoder->display_front, bytes);
        if (widths[w] == 188) {
            for (size_t x = 0; x < line; x++)
                samples[x] = cvbs_test_stripes((unsigned)x, (unsigned)line, true);
        }
        int updates = 0, last = 0;
        for (int i = 1; i <= 690; i++) {
            // Moving vertical bars repeat within the picture. The sync tip
            // still occurs only once per line, and each displayed RGB field differs.
            size_t spacing = line / 10;
            if (widths[w] == 188) {
                // Vary only the whites so the same stripe geometry produces distinct frames.
                for (size_t x = 400; x < line - 80; x++)
                    if (samples[x] > 2000) samples[x] = (int16_t)(18000 + i * 4);
            } else {
                for (size_t x = 400; x < line - 80; x++)
                    samples[x] = (x + i) % spacing < spacing / 2 ? 20000 : 2000;
            }
            gui_cvbs_process_buffer(decoder, samples, line, rate);
            if (!atomic_load(&decoder->display_ready)) continue;
            gui_cvbs_swap_buffers(decoder);
            assert(memcmp(previous, decoder->display_front, bytes) != 0);
            memcpy(previous, decoder->display_front, bytes);
            assert(i - last <= decoder->field_height + 32);
            last = i;
            updates++;
        }
        assert(decoder->vsync.last_h_pos == sliced && cvbs_recent_adc_h(decoder, decoder->global_sample_pos));
        assert(updates >= 2 && 690 - last <= decoder->field_height + 32);
        assert(decoder->vsync.fields_accepted == 0);
    }
    decoder->fixed_sync_threshold = threshold;
    free(previous);
}

// Exercise the two publication paths independently while keeping the same
// live decoder, RGB buffers and field clock through loss and recovery.
static void check_rolling_picture_rgb(cvbs_decoder_t *decoder, size_t line, uint32_t rate) {
    const size_t bytes = (size_t)decoder->frame_width * decoder->frame_height * 3;
    uint8_t *previous = malloc(bytes);
    assert(previous && line <= 2560);
    int16_t samples[2560], drift[2560];
    for (size_t x = 0; x < line; x++)
        drift[x] = (int16_t)(6000 * cos(2 * M_PI * x / line));
    int16_t saved_threshold = decoder->fixed_sync_threshold;
    bool saved_lock = decoder->fixed_sync_locked;
    enum { GREY, NARROW_H, NARROW_NO_H, RESTORED };
    for (int adc_only = 0; adc_only <= 1; adc_only++) {
        for (int scene = GREY; scene <= RESTORED; scene++) {
            unsigned sliced_lines = 0;
            gui_cvbs_swap_buffers(decoder);
            memcpy(previous, decoder->display_front, bytes);
            int updates = 0, last = 0;
            for (int row = 1; row <= 1000; row++) {
                for (size_t x = 0; x < line; x++) {
                    if (scene == GREY) {
                        int v = cvbs_test_stripes((unsigned)x, (unsigned)line, true);
                        samples[x] = (int16_t)((v > 2000 ? 3000 + row / 2 : v) + (adc_only ? drift[x] : -drift[x]));
                    } else {
                        bool white = x > 376 && x < line - 60 && (x < 1250 || x >= 1438);
                        samples[x] = x < 188 && scene != NARROW_NO_H ? -6000 :
                                     white ? (int16_t)(16000 + row * 3) : 2000;
                    }
                }
                // One window alone cannot satisfy the ADC publication gate.
                if (adc_only) decoder->vsync.lines_at_h_cadence = 0;
                else decoder->vsync.raw_h_probe.matched_windows = 0;
                decoder->fixed_sync_locked = true;
                // During loss, put the slice between white and blanking so
                // the picture stripe itself produces classified H pulses.
                int threshold = scene == GREY ? -8000 : (scene == NARROW_NO_H ? 10000 : -2000);
                if (decoder->pll.locked && decoder->dc_offset_valid)
                    threshold -= (int)lrint(decoder->dc_offset);
                decoder->fixed_sync_threshold = adc_only ? -30000 : (int16_t)threshold;
                size_t prior_h = decoder->vsync.last_h_pos;
                gui_cvbs_process_buffer(decoder, samples, line, rate);
                if (decoder->vsync.last_h_pos > prior_h) sliced_lines++;
                if (!atomic_load(&decoder->display_ready)) continue;
                gui_cvbs_swap_buffers(decoder);
                assert(scene != NARROW_NO_H);
                assert(memcmp(previous, decoder->display_front, bytes) != 0);
                memcpy(previous, decoder->display_front, bytes);
                assert(row - last <= decoder->field_height + 64);
                last = row;
                // Remove H immediately after publication, with a full field
                // of the identical stripe geometry ahead of the next deadline.
                if (++updates == 2 && scene == NARROW_H) break;
            }
            if (scene == NARROW_NO_H) {
                assert(updates == 0 && memcmp(previous, decoder->display_front, bytes) == 0);
            } else {
                assert(updates >= 2);
                if (scene != NARROW_H) assert(1000 - last <= decoder->field_height + 32);
            }
            // A timeout may bootstrap a slice for one line. It must never
            // accumulate enough sliced lines to authorize an ADC-only frame.
            if (adc_only) assert(sliced_lines < 8);
            else assert(sliced_lines >= 100);
        }
    }
    decoder->fixed_sync_locked = saved_lock;
    decoder->fixed_sync_threshold = saved_threshold;
    free(previous);
}

typedef enum {
    END_SILENCE, END_NOISE, END_FIVE_H, END_FILTERED_NOISE, END_FINE_DETAIL,
    END_STRONG_NOISE, END_FULL_NOISE, END_NARROW_NOISE, END_NOISY_VIDEO, END_ALTERNATING_BARS,
    END_ALTERNATING_NO_H, END_LOW_BARS, END_LOW_BARS_NO_H, END_ATTENUATED, END_LOW_DEPTH
} field_end_input_t;

static int field_end_noise(uint32_t *seed, int *filtered, int time_constant) {
    *seed = 1664525u * *seed + 1013904223u;
    *filtered += ((int)(*seed >> 16) - 32768 - *filtered) / time_constant;
    return *filtered;
}

// Splice the disturbance just before a free-running field is published.
static void check_raw_field_end_rgb(bool pal, uint32_t rate, int remaining, field_end_input_t input,
                                     bool stale_slice, uint32_t seed) {
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    decoder.sync_mode = CVBS_SYNC_MODE_FIXED;
    const cvbs_baseband_profile_t *profile = cvbs_profile(&decoder);
    int line = (int)lround(profile->line_us * rate / 1e6);
    int width = (int)lround(4.7e-6 * rate);
    int active = (int)lround(10e-6 * rate);
    int porch = (int)lround(2e-6 * rate);
    int16_t samples[2560];
    assert(line <= (int)(sizeof(samples) / sizeof(*samples)));
    bool gain_step = input == END_ATTENUATED || input == END_LOW_DEPTH;
    int scale = input == END_LOW_DEPTH ? 8 : 1;
    bool full_noise = input == END_FULL_NOISE || input == END_NARROW_NOISE;
    bool low_bars = input == END_LOW_BARS || input == END_LOW_BARS_NO_H;
    bool remove_h = input == END_ALTERNATING_NO_H || input == END_LOW_BARS_NO_H;
    bool alternating = input == END_ALTERNATING_BARS || remove_h || low_bars;
    int bar_high = low_bars ? 3000 : 16000;
    bool noisy_video = input == END_NOISY_VIDEO || alternating;
    bool seeded_noise = full_noise || noisy_video;
    int filtered = 0, time_constant = (int)(rate / (input == END_NARROW_NOISE ? 500000u : 2000000u));
    int outputs = 0, rolling_rows = 0, source_row = 0;
    for (; ; source_row++) {
        assert(source_row < 3000);
        for (int x = 0; x < line; x++) {
            samples[x] = x < width ? -6000 : (x > active && x < line - porch ? 10000 + (source_row % 1200) * 5 : 2000);
            if (alternating && x > active && x < line - porch)
                samples[x] = ((x < line / 2) != (source_row & 1)) ? bar_high : 2200;
            samples[x] /= scale;
            if (input == END_LOW_DEPTH) samples[x] -= 250; // Tip -1000, porch 0.
            if (input == END_STRONG_NOISE || seeded_noise) field_end_noise(&seed, &filtered, time_constant);
        }
        decoder.fixed_sync_locked = true;
        decoder.fixed_sync_threshold = stale_slice && outputs >= 3 ? -30000 : -2000 / scale;
        if (outputs >= 3) rolling_rows++;
        gui_cvbs_process_buffer(&decoder, samples, line, rate);
        if (atomic_load(&decoder.display_ready)) {
            gui_cvbs_swap_buffers(&decoder);
            outputs++;
        }
        int end = profile->field_lines[decoder.state.current_field ? 1 : 0] + 25;
        // Keep the seeded splice at the same source position as the captured failure.
        bool warm = seeded_noise ? source_row + 1 >= (pal ? 1950 : 1670) :
                    (uint64_t)rolling_rows * line >= rate / 20;
        if (warm && end - decoder.pll.current_line == remaining) break;
    }
    if (stale_slice) assert(cvbs_recent_adc_h(&decoder, decoder.global_sample_pos));
    else assert(decoder.global_sample_pos - decoder.vsync.last_h_pos <= 2u * line);
    const size_t bytes = (size_t)decoder.frame_width * decoder.frame_height * 3;
    uint8_t *frozen = malloc(bytes);
    assert(frozen);
    memcpy(frozen, decoder.display_front, bytes);
    for (int x = 0; x < line; x++) {
        if (input == END_FINE_DETAIL) {
            bool picture = x > active && x < line - porch;
            samples[x] = x < width ? -6000 : picture ?
                         (int16_t)(11000 + 8500 * cos(2 * M_PI * 1.8e6 * x / rate)) : 2000;
        } else {
            samples[x] = input == END_FIVE_H ? ((5 * x) % line < 5 * width ? -6000 : 2000) : 0;
        }
    }
    bool live = input == END_FINE_DETAIL || gain_step || (noisy_video && !remove_h);
    int frames = decoder.state.frames_decoded;
    int changes = 0, last_update = 0;
    // In the low-contrast cycle only H changes. Keep the same picture,
    // noise generator and decoder running across loss and recovery.
    bool cycle = input == END_LOW_BARS;
    int duration = cycle ? 2300 : live ? 800 : 400;
    int recovered = 0;
    for (int row = 0; row < duration; row++) {
        bool outage = cycle && row >= 800 && row < 1500;
        if (cycle && row == 800) {
            assert(changes >= 3);
            frames = decoder.state.frames_decoded;
        }
        if (input == END_FINE_DETAIL) {
            // Modulate a patch so consecutive displayed fields differ.
            for (int x = line / 2; x < line / 2 + width; x++) samples[x] = (int16_t)(4000 + 16 * row);
        }
        if (gain_step || noisy_video) {
            // Scale the whole original waveform, including both porches.
            int white = 10000 + ((source_row + row + 1) % 1200) * 5;
            for (int x = 0; x < line; x++) {
                int value = x < width ? -6000 : x > active && x < line - porch ? white : 2000;
                if (alternating && x > active && x < line - porch)
                    value = ((x < line / 2) != ((source_row + row + 1) & 1)) ? bar_high : 2200;
                if ((remove_h || outage) && x < width) value = 2000;
                if (input == END_LOW_DEPTH) value = value / scale - 250 - 8 * (row & 1);
                samples[x] = gain_step ? value / 16 :
                             value + field_end_noise(&seed, &filtered, time_constant) / 2;
            }
            if (input == END_LOW_DEPTH) assert(samples[line - 1] - samples[0] == 62 + (row & 1));
        }
        if (input == END_NOISE || input == END_FILTERED_NOISE || input == END_STRONG_NOISE || full_noise) {
            for (int x = 0; x < line; x++) {
                int noise = field_end_noise(&seed, &filtered, time_constant);
                samples[x] = input == END_NOISE ? (int16_t)((int)(seed >> 23) - 256) :
                             (int16_t)(full_noise ? noise : noise / (input == END_STRONG_NOISE ? 6 : 32));
            }
        }
        gui_cvbs_process_buffer(&decoder, samples, line, rate);
        if (atomic_load(&decoder.display_ready)) {
            gui_cvbs_swap_buffers(&decoder);
            if (live && !outage) {
                if ((gain_step || noisy_video) && changes == 0) {
                    assert(row + 1 == remaining);
                    assert(memcmp(frozen, decoder.display_front, bytes) != 0);
                }
                if (cycle && row >= 1500 && memcmp(frozen, decoder.display_front, bytes) != 0) recovered++;
                if (!cycle || row < 1500 || recovered > 1)
                    assert(row - last_update <= decoder.field_height + 32);
                last_update = row;
                if (memcmp(frozen, decoder.display_front, bytes) != 0) changes++;
                memcpy(frozen, decoder.display_front, bytes);
            } else {
                assert(memcmp(frozen, decoder.display_front, bytes) == 0);
            }
        }
        if (!live || outage) assert(decoder.state.frames_decoded == frames);
    }
    if (cycle) assert(recovered >= 2);
    if (live) {
        assert(changes >= 2 && duration - last_update <= decoder.field_height + 32);
    } else {
        assert(memcmp(frozen, decoder.display_front, bytes) == 0);
    }

    size_t length;
    gen_cfg_t cfg = {pal, rate, 12, true, -1, &length};
    int16_t *signal = gen_baseband(&cfg);
    assert(signal);
    int accepted = decoder.vsync.fields_accepted;
    int first = -1, last = -1, boundaries = 0;
    for (size_t pos = 0; pos + line <= length; pos += line) {
        gui_cvbs_process_buffer(&decoder, signal + pos, line, rate);
        if (decoder.vsync.fields_accepted == accepted) continue;
        assert(decoder.vsync.fields_accepted == accepted + 1);
        accepted++;
        int row = (int)(pos / line);
        if (!boundaries) first = row;
        else assert(fabs((row - last) - profile->field_cadence_lines) <= 1);
        last = row;
        boundaries++;
    }
    // First confirmed boundary with immediate slicer reacquisition on this
    // generator: row 628 (PAL), 792 (NTSC). Rolling RGB alone cannot pass.
    assert(boundaries >= 6);
    if (input == END_SILENCE) assert(first <= (pal ? 628 : 792));
    gui_cvbs_swap_buffers(&decoder);
    assert(memcmp(frozen, decoder.display_front, bytes) != 0);
    free(signal);
    free(frozen);
    gui_cvbs_cleanup(&decoder);
}

static void run_50ohm_transition(bool pal, bool cold) {
    const double rate = 40000000.0;
    size_t length;
    gen_cfg_t cfg = {pal, rate, 12, true, -1, &length};
    int16_t *signal = gen_baseband(&cfg);
    assert(signal && length > 0);

    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, pal ? 0 : 1);
    decoder.sync_mode = CVBS_SYNC_MODE_FIXED;
    if (!cold) {
        run_decoder(&decoder, signal, length, rate, true);
        gui_cvbs_swap_buffers(&decoder);
        assert(decoder.pll.locked && decoder.state.frames_decoded > 0);
    }

    size_t frame_bytes = (size_t)decoder.frame_width * decoder.frame_height * 3u;
    uint8_t *previous = malloc(frame_bytes);
    assert(previous);
    for (int enabled = 1; enabled >= 0; enabled--) {
        memcpy(previous, decoder.display_front, frame_bytes);
        memset(decoder.display_back, 0, frame_bytes);
        atomic_store(&decoder.display_ready, 1);  // Old mode's pending frame
        int frames_before_switch = decoder.state.frames_decoded;

        assert(gui_cvbs_set_50ohms(&decoder, enabled != 0));
        assert(!decoder.pll.locked);
        gui_cvbs_swap_buffers(&decoder);
        assert(memcmp(previous, decoder.display_front, frame_bytes) == 0);

        // Silence alone must not replace the last visible frame in either mode.
        int16_t silence[8192] = {0};
        for (int i = 0; i < 220; i++) {
            gui_cvbs_process_buffer(&decoder, silence, 8192, (uint32_t)rate);
            wait_for_cvbs_scout(&decoder);
        }
        assert(!atomic_load(&decoder.display_ready));
        assert(decoder.state.frames_decoded == frames_before_switch);

        int16_t h_only[2560];
        size_t line = (size_t)round(cvbs_profile(&decoder)->line_us * rate / 1e6);
        if (!enabled) check_sparse_raw_sync(&decoder, line, (uint32_t)rate);
        for (size_t x = 0; x < line; x++) {
            h_only[x] = x < 188 ? -12000 : (x > 500 && x < 2200 ? 8000 : 0);
        }
        for (int i = 0; i < 1500; i++) {
            gui_cvbs_process_buffer(&decoder, h_only, line, (uint32_t)rate);
            wait_for_cvbs_scout(&decoder);
        }
        assert(decoder.pll.locked && decoder.vsync.fields_accepted == 0);
        if (enabled) {
            assert(decoder.hold_frame_for_relock && !atomic_load(&decoder.display_ready));
            assert(decoder.state.frames_decoded == frames_before_switch);
            assert(memcmp(previous, decoder.display_front, frame_bytes) == 0);
        } else {
            assert(atomic_load(&decoder.display_ready));
            assert(decoder.state.frames_decoded >= frames_before_switch + 2);
            gui_cvbs_swap_buffers(&decoder);
            assert(memcmp(previous, decoder.display_front, frame_bytes) != 0);
            // Change the pattern while H continues without VBI: a second
            // distinct raw RGB image must reach the front buffer.
            memcpy(previous, decoder.display_front, frame_bytes);
            for (size_t x = 188; x < line; x++)
                h_only[x] = x > 500 && x < 1000 ? 8000 : 0;
            for (int i = 0; i < 1000; i++) {
                // Some picture lines have a second H-width excursion.
                for (int x = 1550; x < 1738; x++) h_only[x] = i % 3 == 0 ? -12000 : 0;
                gui_cvbs_process_buffer(&decoder, h_only, line, (uint32_t)rate);
            }
            gui_cvbs_swap_buffers(&decoder);
            assert(memcmp(previous, decoder.display_front, frame_bytes) != 0);
            assert(decoder.vsync.fields_accepted == 0);
            check_unsliced_widths(&decoder, line, (uint32_t)rate);
            check_sparse_raw_sync(&decoder, line, (uint32_t)rate);
        }

        for (size_t pos = 0; pos + 8192 <= length; pos += 8192) {
            gui_cvbs_process_buffer(&decoder, signal + pos, 8192, (uint32_t)rate);
            wait_for_cvbs_scout(&decoder);
            if (atomic_load(&decoder.display_ready)) break;
            if (enabled) assert(decoder.hold_frame_for_relock);
        }
        assert(atomic_load(&decoder.display_ready));
        assert(decoder.pll.locked && !decoder.hold_frame_for_relock);
        assert(decoder.state.frames_decoded > frames_before_switch);
        gui_cvbs_swap_buffers(&decoder);
        if (!enabled) {
            size_t line = (size_t)round(cvbs_profile(&decoder)->line_us * rate / 1e6);
            check_rolling_picture_rgb(&decoder, line, (uint32_t)rate);
        }
    }

    gui_cvbs_cleanup(&decoder);
    free(previous);
    free(signal);
}

// Splice interference into a running panel feed, then inspect every front
// buffer through a full second (including the first overflow after loss).
static void run_off_interference_rgb(bool pal) {
    const unsigned rate = 40000000;
    size_t length;
    gen_cfg_t cfg = {pal, rate, 12, true, -1, &length};
    int16_t *video = gen_baseband(&cfg);
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed && video);
    gui_cvbs_feed_set_system(feed, pal ? 0 : 1);
    cvbs_decoder_t *decoder = gui_cvbs_feed_decoder(feed);
    assert(gui_cvbs_set_50ohms(decoder, true));
    feed_cvbs_40mhz(feed, video, length);
    gui_cvbs_swap_buffers(decoder);
    assert(gui_cvbs_set_50ohms(decoder, false));
    const size_t bytes = (size_t)decoder->frame_width * decoder->frame_height * 3;
    uint8_t *saved = malloc(bytes);
    assert(saved);
    double period = cvbs_profile(decoder)->line_us * rate / 1e6;
    // Nine NTSC line periods span an integer number of 40 MHz samples.
    enum { SINE, TAIL_BURST, ISOLATED, SHORT_BURST, RECTANGULAR, DRIFT, STRIPES, STRIPES_WITH_H };
    const unsigned cycle_lines[] = {9, 285, 19, 40, 9, 9, 1, 1};
    int16_t drift[5000]; // One 8 kHz cycle at 40 MSPS.
    for (unsigned i = 0; i < 5000; i++) drift[i] = (int16_t)(6000 * sin(2 * M_PI * i / 5000));
    for (unsigned k = 0; k < sizeof(cycle_lines) / sizeof(*cycle_lines); k++) {
        int before = decoder->state.frames_decoded;
        feed_cvbs_40mhz(feed, video, length);
        gui_cvbs_swap_buffers(decoder);
        assert(decoder->state.frames_decoded > before);
        memcpy(saved, decoder->display_front, bytes);
        int frames = decoder->state.frames_decoded;
        unsigned rows = cycle_lines[k];
        unsigned cycle = (unsigned)lround(rows * period);
        int16_t *pattern = calloc(cycle, sizeof(*pattern));
        assert(pattern);
        if (k == SINE) {
            for (unsigned i = 0; i < cycle; i++)
                pattern[i] = (int16_t)(3500 * cos(2 * M_PI * 7 * rows * i / cycle));
        } else if (k == STRIPES || k == STRIPES_WITH_H) {
            for (unsigned i = 0; i < cycle; i++)
                pattern[i] = cvbs_test_stripes(i, cycle, k == STRIPES_WITH_H);
        } else if (k == RECTANGULAR || k == DRIFT) {
            for (unsigned i = 0; i < cycle; i++)
                pattern[i] = fmod((double)i, period / 5) < 188 ? -8000 : 2000;
        } else {
            unsigned first = k == TAIL_BURST ? rows - 26 : (k == ISOLATED ? 1 : 0);
            unsigned end = k == TAIL_BURST ? rows : (k == SHORT_BURST ? 5 : first + 1);
            for (unsigned row = first; row < end; row++) {
                unsigned start = (unsigned)lround(row * period);
                for (unsigned j = 0; j < 188; j++) pattern[start + j] = -8000;
            }
        }
        unsigned changes = 0;
        int16_t chunk[DISPLAY_SAMPLE_COUNT];
        for (unsigned pos = 0; pos < rate;) {
            unsigned n = rate - pos;
            if (n > DISPLAY_SAMPLE_COUNT) n = DISPLAY_SAMPLE_COUNT;
            for (unsigned j = 0; j < n; j++) {
                chunk[j] = pattern[(pos + j) % cycle];
                if (k == DRIFT) chunk[j] += drift[(pos + j) % 5000];
            }
            gui_cvbs_feed_process(feed, chunk, n, rate);
            if (atomic_load(&decoder->display_ready)) {
                gui_cvbs_swap_buffers(decoder);
                if (memcmp(saved, decoder->display_front, bytes) != 0) {
                    assert(k == STRIPES_WITH_H);
                    memcpy(saved, decoder->display_front, bytes);
                    changes++;
                }
            }
            pos += n;
        }
        if (k == STRIPES_WITH_H) {
            assert(decoder->state.frames_decoded >= frames + 40 && changes >= 2);
        } else {
            assert(decoder->state.frames_decoded == frames);
            assert(memcmp(saved, decoder->display_front, bytes) == 0);
        }
        free(pattern);
    }
    free(saved);
    gui_cvbs_feed_destroy(feed);
    free(video);
}

// 40 MSPS NTSC mono s16 excerpt beginning at 18:49 of the 02:39:26 capture.
// Switch Off at 18:56 and back On at 18:59, using the panel feed path.
static void run_raw_h_replay(const char *path) {
    const unsigned rate = 40000000;
    FILE *file = fopen(path, "rb");
    assert(file);
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    assert(feed);
    gui_cvbs_feed_set_system(feed, 1);
    cvbs_decoder_t *decoder = gui_cvbs_feed_decoder(feed);
    assert(gui_cvbs_set_50ohms(decoder, true));
    size_t bytes = (size_t)decoder->frame_width * decoder->frame_height * 3;
    uint8_t *previous = malloc(bytes);
    assert(previous);
    memcpy(previous, decoder->display_front, bytes);
    int16_t samples[DISPLAY_SAMPLE_COUNT];
    for (int phase = 0; phase < 5; phase++) {
        if (phase == 1 || phase == 4) assert(gui_cvbs_set_50ohms(decoder, phase == 4));
        unsigned span = phase == 0 ? 7 * rate : rate;
        unsigned last = 0, longest = 0, changed = 0;
        for (unsigned pos = 0; pos < span;) {
            unsigned count = span - pos;
            if (count > DISPLAY_SAMPLE_COUNT) count = DISPLAY_SAMPLE_COUNT;
            size_t received = fread(samples, sizeof(*samples), count, file);
            assert(received == count);
            gui_cvbs_feed_process(feed, samples, count, rate);
            wait_for_cvbs_scout(decoder);
            pos += count;
            if (!atomic_load(&decoder->display_ready)) continue;
            gui_cvbs_swap_buffers(decoder);
            if (memcmp(previous, decoder->display_front, bytes) == 0) continue;
            memcpy(previous, decoder->display_front, bytes);
            if (pos - last > longest) longest = pos - last;
            last = pos;
            changed++;
        }
        if (span - last > longest) longest = span - last;
        fprintf(stderr, "18:%02d RGB changes=%u longest gap=%.1f ms\n",
                phase ? 55 + phase : 49, changed, longest * 1000.0 / rate);
        if (phase == 2) {
            assert(changed >= 55 && longest <= rate / 50);
        } else {
            assert(changed >= 40 && longest <= rate * 0.075);
        }
    }
    free(previous);
    gui_cvbs_feed_destroy(feed);
    fclose(file);
}

// Exercise consecutive, repeated and skipped samples and the scalar tail.
static void check_ntsc_rgb_kernels(void) {
    if (!cvbs_ntsc_vector_supported()) return;
    uint8_t mask[256], rgb[2][3 * 91 + 1];
    int16_t history[2][91];
    uint32_t seed = 52;
    g_pf_y[0] = g_pf_i[0] = g_pf_q[0] = 0;
    for (int k = 0; k < 255; k++) {
        seed = seed * 1664525u + 1013904223u;
        int v = (int)(seed >> 16) - 23000;
        g_pf_y[k + 1] = g_pf_y[k] + v;
        g_pf_i[k + 1] = g_pf_i[k] + ((k & 1) ? v / 4 : -v / 2);
        g_pf_q[k + 1] = g_pf_q[k] + ((k & 2) ? v / 3 : -v / 5);
        mask[k] = (k % 7) < 4;
    }
    for (int direction = -1; direction <= 1; direction++) {
        memset(rgb, 0xaa, sizeof(rgb));
        for (int k = 0; k < 91; k++)
            history[0][k] = history[1][k] = 5000 + 11 * k;
        for (int v = 0; v < 2; v++)
            cvbs_ntsc_active_rgb(rgb[v], 91, (20 << 16) + 23456, 65536 + direction * 650, 20.56,
                                 1.0 + direction * 0.01, 4, 8, 20000, -200, 0.6, 0.8, 0.00008, mask,
                                 history[v], v != 0);
        for (int k = 0; k < 3 * 91; k++)
            assert(abs((int)rgb[0][k] - rgb[1][k]) <= 1);
        assert(rgb[0][3 * 91] == 0xaa && rgb[1][3 * 91] == 0xaa);
        for (int k = 0; k < 91; k++)
            assert(abs(history[0][k] - history[1][k]) <= 1);
    }
}

// The shorter-looking text edge must retain the carrier zero. The temporal
// stabilizer must discard history immediately at motion or a brightness cut.
static void check_ntsc_luma_aperture(void) {
    static const int burst[] = {1200, 0, -1200, 0};
    for (int pattern = 0; pattern < 2; pattern++) {
        g_pf_y[0] = 0;
        for (int k = 0; k < 32; k++) {
            int sample = 6000 + (pattern ? (k >= 12 && k < 16 ? 800 : 0) : burst[k % 4]);
            g_pf_y[k + 1] = g_pf_y[k] + sample;
        }
        double mean = (g_pf_y[16] - g_pf_y[12]) / 4;
        double y = cvbs_ntsc_text_luma(mean, 12, 4, 0, 0.25, 250);
        if (pattern)
            assert(y > mean && y <= mean + 250);
        else
            assert(y == 6000);
    }
    int16_t last = 6000;
    double settled = cvbs_ntsc_settle_y(6100, &last, true, 16000);
    assert(settled > 6000 && settled < 6100);
    assert(cvbs_ntsc_settle_y(6200, &last, false, 16000) == 6200);
    assert(cvbs_ntsc_settle_y(8000, &last, true, 16000) == 8000);
}

// Same interlaced row in three successive NTSC frames. The left patch is
// static. The middle changes only luma; the right changes only chroma.
static void check_ntsc_frame_comb_motion(void) {
    cvbs_decoder_t decoder;
    assert(gui_cvbs_init(&decoder));
    gui_cvbs_set_format(&decoder, 1);
    decoder.sample_rate_hz = 14318180;
    decoder.vsync.have_field = true;
    decoder.ntsc_wait_for_anchor = false;
    decoder.pll.current_line = 60;
    int16_t src[910];
    const uint8_t *stationary = NULL;
    double chroma[910];
    static const int carrier[] = {1800, 0, -1800, 0};
    for (int frame = 0; frame < 3; frame++) {
        int sign = frame == 1 ? -1 : 1;
        for (int x = 0; x < 910; x++) {
            int level = 6000 + (x >= 450 && x < 640 && frame == 2 ? 5000 : 0);
            int phase = ((frame == 2 && x >= 640 && x < 750) || (frame == 1 && x >= 750)) ? 2 : 0;
            src[x] = level + sign * carrier[(x + phase) % 4];
            chroma[x] = sign * carrier[(x + phase) % 4];
        }
        stationary = cvbs_ntsc_frame_comb(&decoder, src, 910, 1000 + frame * 477750, 140, 888, 2, 16000,
                                          false, chroma);
        assert((stationary != NULL) == (frame == 2));
    }
    for (int x = 180; x < 420; x++) {
        assert(stationary[x]);
        assert(fabs(chroma[x] - carrier[x % 4]) <= 1);
    }
    for (int x = 470; x < 870; x++) {
        assert(!stationary[x]);
        assert(chroma[x] == carrier[(x + (x >= 640 && x < 750 ? 2 : 0)) % 4]);
    }
    gui_cvbs_reset(&decoder);
    assert(decoder.ntsc_history == NULL);
    gui_cvbs_cleanup(&decoder);
}

// Render both field parities against an independent spatial-only decoder.
// The fine luma grating differs on the preceding scanline, so spatial and
// temporal decoding produce observably different RGB even on a static frame.
static void check_ntsc_temporal_rgb(void) {
    enum { width = CVBS_NTSC_FULL_WIDTH_4FSC, n = 910 };
    cvbs_decoder_t *dec = calloc(2, sizeof(*dec));
    assert(dec);
    for (int j = 0; j < 2; j++) {
        assert(gui_cvbs_init(&dec[j]));
        gui_cvbs_set_format(&dec[j], 1);
        dec[j].sample_rate_hz = 14318180;
        dec[j].levels.black_level = 0;
        dec[j].levels.white_level = 16000;
        dec[j].pll.line_period = dec[j].pll.phase = n;
        dec[j].pll.locked = true;
        dec[j].vsync.have_field = j == 0;
        dec[j].ntsc_wait_for_anchor = false;
    }
    int active = cvbs_us_to_samples(dec, cvbs_profile(dec)->active_start_us) - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    int edge = active + 16 * 20;
    const int carrier[] = {1400, 0, -1400, 0};
    int previous_gray = 0;
    for (int frame = 0; frame <= 9; frame++) {
        size_t base = 30000 + 477750 * frame;
        size_t first_eq = base - 60 * n + n;
        if (frame == 6) {
            cvbs_vbi_first_eq(dec, first_eq - 4000, first_eq - 6730, first_eq - 3090);
            assert(!dec->vsync.have_field);
            assert(dec->vsync.fields_rejected == 1 && dec->vsync.vsync_candidates == 1);
            cvbs_vbi_first_eq(dec, first_eq, first_eq - n, first_eq + n);
            assert(dec->vsync.have_field && dec->ntsc_wait_for_anchor);
            assert(dec->vsync.fields_accepted == 0 && dec->vsync.vsync_candidates == 1);
        }
        for (int parity = 0; parity < 2; parity++) {
            if (frame == 6 && parity == 1) {
                size_t next_eq = first_eq + 238875 - n / 2;
                cvbs_vbi_first_eq(dec, next_eq, next_eq - n / 2, next_eq + n);
                assert(!dec->ntsc_wait_for_anchor);
                assert(dec->vsync.fields_accepted == 1 && dec->vsync.vsync_candidates == 2);
            }
            int16_t samples[n];
            uint8_t rgb[2][width * 3];
            int sign = frame % 2 ? -1 : 1;
            size_t start = base + parity * 238875;
            for (int j = 0; j < 2; j++) {
                dec[j].state.current_field = parity;
                dec[j].pll.current_line = 60;
                dec[j].global_sample_pos = start + n;
                for (int x = 0; x < n; x++) {
                    int level = x < active ? 0 : 6000;
                    if (frame >= 4 && x > 200 && x < 350) level += 480;
                    int chroma = sign * carrier[x % 4];
                    if (frame == 3 && x >= edge && x < edge + 4) chroma = -chroma;
                    int grating = x > 600 && x < 800 ? carrier[x % 4] / 2 : 0;
                    samples[x] = level + chroma + grating;
                    dec[j].vsync.h_ring[(start - n + x) & (CVBS_H_RING - 1)] = level - chroma - grating;
                }
                decode_line_to_pixels(&dec[j], samples, n, &dec[j].levels, rgb[j], width);
            }
            if (frame == 2 && parity == 0) previous_gray = rgb[0][3 * 260];
            if (frame == 4) {
                assert(rgb[1][3 * 260] >= previous_gray + 4);
                assert(!memcmp(rgb[0] + 3 * 240, rgb[1] + 3 * 240, 60 * 3));
            }
            if (frame == 3 || frame == 4)
                assert(!memcmp(rgb[0] + 3 * (edge - 4), rgb[1] + 3 * (edge - 4), 12 * 3));
            if (frame >= 5) {
                bool matches_spatial = !memcmp(rgb[0] + 3 * 630, rgb[1] + 3 * 630, 120 * 3);
                assert(matches_spatial == (frame >= 6 && frame < (parity ? 8 : 9)));
            }
        }
    }
    for (int j = 0; j < 2; j++) gui_cvbs_cleanup(&dec[j]);
    free(dec);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--raw-h-replay")) {
        run_raw_h_replay(argv[2]);
        return 0;
    }
    if (argc > 1) {
        // TEMP live diagnostic mode: gui_cvbs_test [flags] <live_raw>
        bool pal = true, s16 = false, full = false, fixed = false;
        bool panel = false, declared = false, secam = false, receiver = false;
        bool display_chunks = false;
        double rate_mhz = 20.0;
        const char *path = NULL, *ppm = NULL;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--secam")) secam = true;
            else if (!strcmp(argv[i], "--ntsc")) pal = false;
            else if (!strcmp(argv[i], "--pal")) pal = true;
            else if (!strcmp(argv[i], "--s16")) s16 = true;
            else if (!strcmp(argv[i], "--u8")) s16 = false;
            else if (!strcmp(argv[i], "--full")) full = true;
            else if (!strcmp(argv[i], "--fixed")) fixed = true;
            else if (!strcmp(argv[i], "--panel")) panel = true;
            else if (!strcmp(argv[i], "--50ohms")) receiver = true;
            else if (!strcmp(argv[i], "--display-chunks")) display_chunks = true;
            else if (!strcmp(argv[i], "--declared")) declared = true;
            else if (!strncmp(argv[i], "--rate=", 7)) rate_mhz = atof(argv[i] + 7);
            else if (!strncmp(argv[i], "--ppm=", 6)) ppm = argv[i] + 6;
            else path = argv[i];
        }
        if (!path) {
            fprintf(stderr, "usage: gui_cvbs_test [--pal|--ntsc] [--rate=<MHz>] "
                            "[--u8|--s16] [--full] [--fixed] [--panel] "
                            "[--ppm=<file>] [--declared] [--50ohms] [--display-chunks] <live_raw>\n");
            return 1;
        }
        run_live_diag(path, pal, rate_mhz * 1000000.0, s16, full, fixed,
                      panel, ppm, declared, secam, receiver, display_chunks);
        return 0;
    }
    check_receiver_displaced_h();
    check_receiver_sync_continuity(true);
    check_receiver_sync_continuity(false);
    for (int system = 0; system < 2; system++)
        for (int parity = 0; parity < 2; parity++) {
            check_displaced_vblank(system == 0, parity, false);
            check_displaced_vblank(system == 0, parity, true);
        }
    check_ntsc_temporal_rgb();
    check_ntsc_frame_comb_motion();
    check_ntsc_luma_aperture();
    check_ntsc_rgb_kernels();
    // Rate awareness: the SAME geometry decodes at 40 MSPS and 25 MSPS
    // (the legacy decoder only worked at exactly 40 MSPS).
    run_format(true, 40000000.0, true);
    run_format(false, 40000000.0, true);
    run_format(true, 25000000.0, true);
    run_format(false, 25000000.0, true);
    // Self-calibration: the DECLARED rate is wrong (the legacy 40 MSPS
    // default stuck on live paths) while the stream is 20 MSPS - the
    // H-spacing median must recover the true rate and the hues must still
    // decode (a wrong declared rate is geometry scale-invariant but runs
    // the demod carrier at the wrong frequency, trashing every hue).
    run_format_ex(true, 20000000.0, true, 40000000.0);
    run_format_ex(false, 20000000.0, true, 40000000.0);
    // Colour suppression without a burst + the exact grayscale Off mode.
    run_format(true, 40000000.0, false);
    run_format(false, 40000000.0, false);
    run_off_mode_gray(true);
    run_off_mode_gray(false);
    // VBI model rejects a damaged train instead of mis-framing.
    run_cadence_reject(true);
    run_cadence_reject(false);
    run_wide_hsync(false);
    run_wide_hsync(true);
    run_50ohm_vblank_acquisition();
    run_raw_field_phase(16);
    run_raw_field_phase(80);
    run_raw_field_phase(200);
    run_50ohm_scout_handoff();
    run_50ohm_receiver(true);
    run_50ohm_receiver(false);
    run_rejected_field_scout();
    run_50ohm_comb(true, 40000000);
    run_50ohm_comb(true, 80000000);
    run_50ohm_comb(false, 40000000);
    run_50ohm_comb(false, 80000000);
    run_cold_field_end_offset(false, 6000, -1);
    run_cold_field_end_offset(false, 100, -1);
    run_cold_field_end_offset(true, 6000, -1);
    run_cold_field_end_offset(true, 8000, 48);
    run_repeated_field_counts();
    run_fixed_rgb_black();
    run_coupled_rgb_black(false);
    run_coupled_rgb_black(true);
    run_50ohm_fixed_levels(true);
    run_50ohm_fixed_levels(false);
    run_signal_recovery(false);
    run_signal_recovery(true);
    run_off_interference_rgb(true);
    run_off_interference_rgb(false);
    for (int remaining = 2; remaining <= 8; remaining += 6) {
        for (int system = 0; system < 2; system++) {
            check_raw_field_end_rgb(system == 0, 40000000, remaining, END_SILENCE, true, 123);
            for (uint32_t rate = 20000000; rate <= 40000000; rate += 20000000) {
                check_raw_field_end_rgb(system == 0, rate, remaining, END_NOISE, true, 123);
                check_raw_field_end_rgb(system == 0, rate, remaining, END_FIVE_H, true, 123);
                check_raw_field_end_rgb(system == 0, rate, remaining, END_FILTERED_NOISE, false, 123);
                check_raw_field_end_rgb(system == 0, rate, remaining, END_STRONG_NOISE, true, 123);
                check_raw_field_end_rgb(system == 0, rate, remaining, END_ATTENUATED, true, 123);
                for (int stale = 0; stale <= 1; stale++) {
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_LOW_DEPTH, stale, 123);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_FULL_NOISE, stale, 4);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_FULL_NOISE, stale, 7);
                }
                if (remaining == 2) {
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_FINE_DETAIL, true, 123);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_FULL_NOISE, false, 17);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_NARROW_NOISE, true, 9);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_NOISY_VIDEO, true, 11);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_ALTERNATING_BARS, true, 7);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_ALTERNATING_NO_H, true, 7);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_LOW_BARS, true, 7);
                    check_raw_field_end_rgb(system == 0, rate, remaining, END_LOW_BARS_NO_H, true, 7);
                }
            }
        }
    }
    run_50ohm_transition(true, false);
    run_50ohm_transition(false, false);
    run_50ohm_transition(false, true);
    // Full-frame view hybrid (tbc-tools analyse HYBRID mode) contracts.
    run_full_view_hybrid(true);
    run_full_view_hybrid(false);
    // Render-path conversion contract (dev/gui_output_verification_rules.md):
    // the GUI-consumed RGB24 -> Color blit is code under test.
    run_render_conversion_contract();
    // SECAM FM chroma (backlog item 4): the numeric port contract first (the
    // time-domain filters vs the reference closed forms), then the decode
    // contracts. A REAL SECAM encoder always LF pre-corrects its deviation
    // waveform, so the studio and converter-offset fixtures are PRE-CORRECTED
    // (the decoder's causal de-emphasis IIR must invert the encoder's
    // pre-correction end to end); the flat-deviation fixture is the
    // robustness case - no real encoder emits it, and the de-emphasis then
    // shows its own true transient behaviour (the reference's too). Every
    // fixture carries the encoder's HF bell: the decoder demodulates through
    // its inverse (2026-11-16 - demodulating a belled signal without it
    // bent every bar transition on the vectorscope).
    run_secam_numeric_match();
    run_secam_bars(0.0, true, "SECAM studio");
    run_secam_bars(105000.0, true, "SECAM offset +105 kHz");
    run_secam_bars(0.0, false, "SECAM flat deviation");
    puts("CVBS baseband profile, VBI sync model and burst-locked colour contracts passed");
    return 0;
} 
