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
    int starts[17];
    starts[0] = 0;
    for (int f = 0; f < cfg->fields; f++) {
        starts[f + 1] = starts[f] + field_lines[f & 1];
    }
    size_t total_lines = (size_t)starts[cfg->fields];
    size_t length = total_lines * line_samples;
    int16_t *sig = malloc(length * sizeof(*sig));
    assert(sig);
    if (cfg->out_length) *cfg->out_length = length;

    int v_line = 0;  // global line index (alternating V-switch per line)

    for (size_t i = 0; i < total_lines; i++) {
        int field = -1;
        for (int f = 0; f < cfg->fields; f++) {
            if ((int)i >= starts[f] && (int)i < starts[f + 1]) { field = f; break; }
        }
        assert(field >= 0);
        int line_in_field = (int)i - starts[field];
        int parity = field & 1;
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
static void run_decoder(cvbs_decoder_t *decoder, const int16_t *sig,
                        size_t length, double rate, bool keep_ring0) {
    size_t pos = 0;
    while (pos < length) {
        size_t chunk = (pos & 1) ? 4093u : 8191u;
        if (chunk > length - pos) chunk = length - pos;
        gui_cvbs_process_buffer(decoder, sig + pos, chunk, (uint32_t)rate);
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
                          bool secam) {
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
        if (full) gui_cvbs_set_frame_view_mode(decoder, CVBS_FRAME_VIEW_FULL);
        decoder->colour_mode = CVBS_COLOUR_AUTO;
        fprintf(stderr, "[panel] feed start\n");
        size_t pos = 0;
        while (pos < count) {
            size_t chunk = (pos & 1) ? 4093u : 8191u;
            if (chunk > count - pos) chunk = count - pos;
            video_cvbs_plugin_process(&plugin, sig + pos, chunk,
                                       (uint32_t)rate);
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

int main(int argc, char **argv) {
    if (argc > 1) {
        // TEMP live diagnostic mode: gui_cvbs_test [flags] <live_raw>
        bool pal = true, s16 = false, full = false, fixed = false;
        bool panel = false, declared = false, secam = false;
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
            else if (!strcmp(argv[i], "--declared")) declared = true;
            else if (!strncmp(argv[i], "--rate=", 7)) rate_mhz = atof(argv[i] + 7);
            else if (!strncmp(argv[i], "--ppm=", 6)) ppm = argv[i] + 6;
            else path = argv[i];
        }
        if (!path) {
            fprintf(stderr, "usage: gui_cvbs_test [--pal|--ntsc] [--rate=<MHz>] "
                            "[--u8|--s16] [--full] [--fixed] [--panel] "
                            "[--ppm=<file>] [--declared] <live_raw>\n");
            return 1;
        }
        run_live_diag(path, pal, rate_mhz * 1000000.0, s16, full, fixed,
                      panel, ppm, declared, secam);
        return 0;
    }
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
