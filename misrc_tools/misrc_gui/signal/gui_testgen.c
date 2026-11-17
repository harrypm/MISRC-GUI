/*
 * MISRC GUI - HackTV Signal Generator Plugin
 *
 * A 1:1 PORT of hacktv's line-based video generation pipeline
 * (third_party/hacktv/src/video.c by Philip Heron, GPL-3.0-or-later).
 * This project is GPL-3.0-or-later; direct code porting is legal.
 *
 * PORTED FUNCTIONS (hacktv src/video.c):
 *   _active_video_line()     -> active line mapping (frame row of each field)
 *   _vid_next_line_raster()  -> per-line CVBS assembly (sync, burst, chroma)
 *   _render_sync_pulses()    -> pre-rendered sync pulse LUTs
 *   vid_init() level scaling -> 16-bit level computation + RGB->YUV
 *   _burstwin()              -> raised-cosine burst envelope
 *   vid_config_pal/ntsc      -> the composite config structs (verbatim values)
 *   av_test.c bars           -> 75% colour bar pattern (PAL / SECAM)
 *   NTSC composite           -> SMPTE SD colour bars (ht_smpte_px)
 *
 * ALTERED for integration:
 *   - Generates a whole frame at once into a flat int16 buffer
 *   - No pthreads/ffmpeg/audio/scrambling - just the video path
 *   - hacktv's Gaussian chroma FIR replaced with a simple window (v1)
 *   - SECAM ident/pre-emphasis omitted (v1)
 *
 * INTERLACED HALF-LINE-SLOT MODEL (replaces the strict per-line model):
 * The previous version classified whole 64us lines, which put BOTH fields'
 * first EQ pulse at an INTEGER-line offset from the last H sync. The CVBS
 * decoder derives field parity from exactly that offset (gui_cvbs.c
 * cvbs_vbi_first_eq: PAL half -> parity 0, integer -> parity 1) and
 * REQUIRES the parity to alternate. Both fields derived as the same parity,
 * so every second VBI candidate was rejected ("vertical fields repeat
 * parity"), no field anchored, and the picture rolled vertically.
 *
 * A real interlaced signal runs its VBI on a HALF-LINE (32us) slot grid.
 * This generator models that grid directly. PAL (1250 slots per frame):
 *   field 1 train: EQ x5 slots 0-4, broad x5 slots 5-9, EQ x5 slots 10-14
 *   H grid:        odd slots 15..623 (field 1), 641..1249 (field 2)
 *   field 2 train: EQ x5 slots 625-629, broad x5 slots 630-634, EQ x5 635-639
 *   blank slots:   624, 640
 * NTSC (1050 slots, 6 EQ per train): same structure, H grid on EVEN slots.
 *
 * Decoder-gate arithmetic (PAL, against cvbs_vbi_first_eq), unverified until
 * run against the live decoder:
 *   recent-H gate:   f1 first EQ at slot 0, last H at slot -1 (0.5 lines);
 *                    f2 first EQ at slot 625, last H at slot 623 (1.0 line)
 *   parity gate:     f1 half-line offset -> parity 0; f2 integer -> parity 1
 *   cadence gate:    field starts 312.0 and 313.0 lines apart (mean 312.5)
 */

#include "gui_testgen.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Clamp to int16 and round half away from zero by add-and-truncate: the
 * same samples as lrint except on exact .5 values, without a libm call per
 * sample (MinGW's lrint measured ~50 ns: five per sample were most of the
 * frame's generation time). */
static inline int16_t ht_r16(double v) {
    if (v > 32767.0) v = 32767.0;
    if (v < -32768.0) v = -32768.0;
    return (int16_t)(v + (v >= 0.0 ? 0.5 : -0.5));
}

/* --- hacktv vid_config structs (verbatim values) ------------------------- */

typedef struct {
    int lines;                  /* 625 / 525 */
    double active_width_us;     /* 51.95 / 52.90 */
    double active_left_us;      /* 10.40 / 9.20 */
    double hsync_us;            /* 4.70 */
    double vsync_short_us;      /* 2.35 / 2.30 (eq pulse) */
    double vsync_long_us;       /* 27.30 / 27.10 (broad pulse) */
    double sync_rise_us;        /* edge 10-90% rise (PASS 2 Hann pulse) */
    double white_level;         /* 0.70 / 100/140 */
    double black_level;         /* 0.00 / 7.5/140 */
    double blanking_level;      /* 0.00 */
    double sync_level;          /* -0.30 / -40/140 */
    int colour_mode;            /* PAL / NTSC / SECAM */
    double burst_width_us;      /* 2.25 / 2.50 */
    double burst_rise_us;       /* burst envelope 10-90% build-up, 0.30 */
    double burst_left_us;       /* 5.60 / 5.30 */
    double burst_level_frac;    /* 3/7 / 4/10 of white-blank */
    double fsc_hz;              /* 4433618.75 / 3579545.4545 */
    double ev_co;               /* 0.877 */
    double eu_co;               /* 0.493 */
    /* Interlaced half-line-slot geometry (see the model note above) */
    int slots;                  /* half-line slots per frame: 1250 / 1050 */
    int eq_count;               /* EQ pulses per train: 5 / 6 */
    int act_first[2];           /* first ACTIVE-picture slot, field 1/2 */
    int act_lines;              /* ACTIVE picture lines per field: 288 / 242 */
} ht_config_t;

#define HT_PAL   1
#define HT_NTSC  2
#define HT_SECAM 3

/* burst_level_frac is the fraction of the (white - blanking) span that is
 * the burst PEAK amplitude: PAL 3/14 = the ITU 0.3 V p-p burst on the 0.7
 * span; NTSC 0.20 = the 40 IRE p-p burst on the 100/140 span. The real EBU
 * HackDAC reference measures 0.221 V p-p - reproduced by the ITU frac times
 * the 0.75 capture-chain chroma gain below (measured, see the probe log). */
static const ht_config_t HT_CONF[GUI_TESTGEN_SYSTEM_COUNT] = {
    /* PAL — hacktv vid_config_pal (sync_rise 0.19 = the measured real-chain
     * edge: the EBU reference's blanking->white 10-90 is 0.19us). Active
     * line 10.5 us / 52 us (ITU-R BT.470; the live EBU bars on the MS2130
     * measure 10.54 us): hacktv's 10.40 started the picture inside the
     * decoder's back-porch level window (its profile active_start is 10.5).
     * Field 1 picture starts at slot 43, field 2 at slot 667 (288 lines). */
    { 625, 52.00, 10.50, 4.70, 2.35, 27.30, 0.19,
      0.70, 0.00, 0.00, -0.30,
      HT_PAL, 2.25, 0.30, 5.60, 3.0/14.0, 4433618.75, 0.877, 0.493,
      1250, 5, { 43, 667 }, 288 },
    /* NTSC — hacktv vid_config_ntsc (sync_rise per the same real chain).
     * active_left 9.45 = the decoder's NTSC profile's active_start: the
     * back-porch window (burst_end..active_start) must NOT overlap the
     * picture - with hacktv's 9.20 the porch sampler lands inside the
     * castellated edge strip and poisons the level commit.
     * Field 1 picture starts at slot 38, field 2 at slot 564 (242 lines). */
    { 525, 52.90, 9.45, 4.70, 2.30, 27.10, 0.19,
      100.0/140, 7.5/140, 0.00, -40.0/140,
      HT_NTSC, 2.50, 0.30, 5.30, 0.20, 3579545.4545, 0.877, 0.493,
      1050, 6, { 38, 564 }, 242 },
    /* SECAM — hacktv vid_config_secam (PAL luma geometry + FM chroma).
     * ev_co/eu_co are the SECAM D deviations in Hz per colour-difference
     * unit (hacktv's SECAM configs REPLACE the PAL 0.877/0.493 with them):
     * ev_co = -1.902*280e3 (D'R), eu_co = +1.505*230e3 (D'B). */
    { 625, 52.00, 10.50, 4.70, 2.35, 27.30, 0.19,
      0.70, 0.00, 0.00, -0.30,
      HT_SECAM, 2.25, 0.30, 5.60, 0.0, 4433618.75,
      -1.902 * 280e3, 1.505 * 230e3,
      1250, 5, { 43, 667 }, 288 },
};

/* Capture-chain chroma gain: the Simulated device emulates a live capture,
 * and the capture chain's response at the subcarrier leaves bar chroma AND
 * burst uniformly below the standard amplitude relative to the sync height.
 * Measured on live MS2130 captures (2026-11-16, per-bar complex demod
 * against each line's own sync tip / back porch, every bar and the burst
 * agreeing to 0.3%): EBU PAL bars 0.79, SMPTE NTSC bars 0.80. The earlier
 * single 0.75 came from a max/min probe of the EBU HackDAC reference FILE.
 * Burst referencing cancels it (scope and picture); the waveform shows it. */
#define HT_CHROMA_GAIN_PAL  0.79
#define HT_CHROMA_GAIN_NTSC 0.80
/* SECAM: the capture chain scales the FM subcarrier's AMPLITUDE like PAL's
 * (same band), but it cannot change the frequency deviation, which carries
 * the colour: standard D'R/D'B deviation (gain 1). The earlier 0.75 on the
 * deviation (from the EBU reference card) put every SECAM bar a quarter
 * inside its vectorscope target. */
#define HT_SECAM_DEV_GAIN 1.0

/* Chroma bandwidth: the bar colour-difference signals are band-limited like
 * a real encoder's (each colour step becomes a raised-cosine transition,
 * 10-90% below). The old renderer switched chroma instantly at every bar
 * edge: the transition's spectrum ran past the decoder's chroma band and
 * its 2 x fsc image traced a chain of loops along every transition on the
 * vectorscope, where the live EBU bars draw clean straight lines. Matched on
 * the live MS2130 captures through the vectorscope's own detector (bandpass,
 * demod, 2-cycle Hann): bar-to-bar 10-90 0.57 us PAL, 0.585 us NTSC. */
#define HT_CHROMA_RISE_PAL_US  0.34
#define HT_CHROMA_RISE_NTSC_US 0.30

static double ht_chroma_gain(int colour_mode) {
    return colour_mode == 2 ? HT_CHROMA_GAIN_NTSC : HT_CHROMA_GAIN_PAL;  /* PAL, SECAM */
}

/* AC-coupling baseline bump (measured on the EBU reference, per line): at
 * each field's VBI the whole baseline rides up and decays back (the porch:
 * -1,434 -> +3,598, decay tau ~10 lines, the sync step intact). The bump
 * PEAKS at the first picture lines (+5,024 on the white bar there - the
 * decoder's white commit (blank + 90% of the filtered luma max) is exactly
 * calibrated against that sagged early-field luma max; without it the
 * commit lands ~10% below the plateau and the top of the picture clamps
 * flat - the washed-out active area). The bump is smooth (no stream
 * discontinuity) and carries no DC bias: the calibrated -0.30/+0.70
 * waveform readout is preserved. */
#define HT_SAG_PEAK       0.152   /* composite, the bump's value at picture start */
#define HT_SAG_TAU_RISE   7.0     /* lines, rise across the VBI */
#define HT_SAG_TAU_DECAY  9.4     /* lines, decay after picture start */
#define HT_SAG_PIC_START  21.0    /* lines, first picture line after the VBI */

/* SECAM FM chain constants (hacktv video.c SECAM_* defines, verbatim) */
#define HT_SECAM_DB     4250000.0   /* 272 fH, D'b rest frequency */
#define HT_SECAM_DR     4406250.0   /* 282 fH, D'r rest frequency */
#define HT_SECAM_FM     4328125.0   /* 277 fH, FM carrier centre */
#define HT_SECAM_DEV    1000000.0   /* deviation scale: INT16_MAX = +1 MHz */
#define HT_SECAM_F0     4286000.0   /* cloche (bell) centre frequency */
#define HT_SECAM_FSYNC  350000.0    /* field-ident ramp deviation */
/* hacktv reuses the burst window as the SECAM chroma render window
 * (vid_config_secam .burst_width = 56.9us, .burst_left = 5.6us). The
 * REAL card (the decoder's reference) differs in two measured ways:
 * the carrier reaches full envelope BEFORE the decoder's porch window
 * opens at the burst position (the porch median is its per-line zero -
 * a slow rise inside that window put ~13 kHz MAD on the zero and a
 * false ~0.08 chroma on black), and the envelope's fall completes
 * inside the line. So: start at the sync end (4.7us), 0.3us edges,
 * plateau through the active area. */
#define HT_SECAM_WIN_US   57.5
#define HT_SECAM_RISE_US  0.3
#define HT_SECAM_IRT1090  2.0738786
/* Standard SECAM video pre-emphasis (the low-frequency "horn"): zero at
 * 85 kHz, pole at 255 kHz (x3 shelf). hacktv's IIR (in={1.0,-0.90456054},
 * out={2.90456054,-2.80912108}) is the bilinear transform of exactly this
 * prototype at 16 MHz; redesigned here for the 40 MSPS stream. */
#define HT_SECAM_PE_FZ  85000.0
#define HT_SECAM_PE_FP  255000.0
/* D'R / D'B pre-filter (before the pre-emphasis): Gaussian, -3 dB here
 * (fitted to a real broadcast encoder, see the FM render). The half-length
 * bound covers 4 sigma up to ~125 MSPS. */
#define HT_SECAM_DX_F3        800000.0
#define HT_SECAM_DX_HALF_MAX  128
/* Chroma D deviations per unit of colour difference (hacktv ev_co/eu_co
 * for SECAM: ev_co = -1.902*280e3 D'R, eu_co = +1.505*230e3 D'B) */
#define HT_SECAM_DR_HZ  (-1.902 * 280e3)
#define HT_SECAM_DB_HZ  (1.505 * 230e3)

/* --- Burst envelope ----------------------------------------------------------
 * Raised-cosine edges, 50% points at t = 0 (burst start) and t = width, edge
 * 10-90% build-up time = rise (ITU-R BT.1700 / SMPTE 170M: 0.3 us). t, width
 * and rise in seconds.
 * The earlier port stepped the envelope from 0 to 1 at t = rise/2 (no rise at
 * all) and from 1 to 0.5 at t = width: measured on the generator output with
 * the composite vectorscope work (2026-11-16) as a hard-edged burst starting
 * 0.15 us late and 0.15 us short (PAL 2.10 us, NTSC 2.35 us wide). */
#define HT_RC_EDGE_1090 0.590334   /* 10-90% span of a raised-cosine edge / its length */
static double ht_burstwin(double t, double width, double rise) {
    const double te = rise / HT_RC_EDGE_1090;          /* full edge length */
    const double d = (t < 0.5 * width) ? t : (width - t);   /* signed distance inside the nearer 50% point */
    if (d <= -0.5 * te) return 0.0;
    if (d >= 0.5 * te) return 1.0;
    return 0.5 + 0.5 * sin(M_PI * d / te);
}

/* --- SECAM chain (port of hacktv _vid_render_secam + filters) ------------ */

/* hacktv common.c rc_window: raised-cosine window with integrated
 * raised-cosine edges (t seconds from window start). */
static double ht_rc_window(double t, double left, double width, double rise) {
    double r;
    t -= left + width / 2;
    t = fabs(t) - (width - rise) / 2;
    if (t <= 0) {
        r = 1.0;
    } else if (t < rise) {
        t = 1.0 - t / rise * 2;
        r = 0.5 * (1.0 + t + sin(M_PI * t) / M_PI);
    } else {
        r = 0.0;
    }
    return r;
}

/* hacktv fir.c kaiser window (beta 7) + fir_low_pass / fir_band_reject,
 * in double precision (hacktv quantises to int16 taps; inaudible). */
static double ht_i_zero(double x) {
    double sum = 1.0, u = 1.0, halfx = x / 2.0, temp;
    int n = 1;
    do {
        temp = halfx / (double)n;
        n += 1;
        temp *= temp;
        u *= temp;
        sum += u;
    } while (u >= 1e-21 * sum);
    return sum;
}

static void ht_kaiser(double *taps, int ntaps, double beta) {
    double i_beta = 1.0 / ht_i_zero(beta);
    double inm1 = 1.0 / ((double)(ntaps - 1));
    taps[0] = i_beta;
    for (int i = 1; i < ntaps - 1; i++) {
        double temp = 2 * i * inm1 - 1;
        taps[i] = ht_i_zero(beta * sqrt(1.0 - temp * temp)) * i_beta;
    }
    taps[ntaps - 1] = i_beta;
}

static void ht_fir_band_reject(double *taps, int ntaps, double sample_rate,
                               double low_cutoff, double high_cutoff, double gain) {
    int M = (ntaps - 1) / 2;
    double fwT0 = 2.0 * M_PI * low_cutoff / sample_rate;
    double fwT1 = 2.0 * M_PI * high_cutoff / sample_rate;
    ht_kaiser(taps, ntaps, 7.0);
    for (int n = -M; n <= M; n++) {
        if (n == 0) taps[n + M] *= 1.0 + (fwT0 - fwT1) / M_PI;
        else taps[n + M] *= (sin(n * fwT0) - sin(n * fwT1)) / (n * M_PI);
    }
    double fmax = taps[M];
    for (int n = 1; n <= M; n++) fmax += 2 * taps[n + M];
    gain /= fmax;
    for (int n = 0; n < ntaps; n++) taps[n] *= gain;
}

/* hacktv's _secam_g (the cloche gain 0.115 (1 + j16u)/(1 + j1.26u),
 * u = f/f0 - f0/f, evaluated at the instantaneous frequency) is replaced by
 * the same network as a linear biquad filter - see the SECAM FM render. */

/* --- hacktv av_test bars (75% amplitude) --------------------------------- */

typedef struct { double r, g, b; } ht_rgb_t;
static const ht_rgb_t HT_BARS[8] = {
    { 1.00, 1.00, 1.00 }, { 0.75, 0.75, 0.00 }, { 0.00, 0.75, 0.75 },
    { 0.00, 0.75, 0.00 }, { 0.75, 0.00, 0.75 }, { 0.75, 0.00, 0.00 },
    { 0.00, 0.00, 0.75 }, { 0.00, 0.00, 0.00 },
};

/* hacktv yuv_level_lookup: map RGB -> Y/U/V with config coefficients */
static void ht_rgb_to_yuv(const ht_config_t *c, double r, double g, double b,
                           double *y, double *u, double *v) {
    *y = 0.299 * r + 0.587 * g + 0.114 * b;
    *u = (b - *y) * c->eu_co;
    *v = (r - *y) * c->ev_co;
}

/* --- SMPTE SD colour bars (NTSC composite) -------------------------------
 * SMPTE EG 1-1990 / ECR 1-1978 layout, the standard NTSC SD bar signal:
 *   top 67%:     75% bars  gray, yellow, cyan, green, magenta, red, blue
 *   middle 8%:   reverse-blue castellations
 *                blue, black, magenta, black, cyan, black, gray
 *   bottom 25%:  -I (5/4 bar), 100% white (5/4), +Q (5/4), black (5/4),
 *                PLUGE 3.5 / 7.5 / 11.5 IRE (1/3 bar each), black (1 bar)
 * Bar chroma is scaled to the (white - black) span (92.5 IRE with the 7.5 IRE
 * setup: 75% yellow = 31 IRE amplitude, the standard NTSC values); -I and +Q
 * are 20 IRE amplitude (40 IRE p-p, the burst's amplitude) on black.
 * Output: y relative to black..white, u/v in units of the (white - blank)
 * span (the caller's chroma scale), in this generator's U = sin / V = cos
 * QAM frame where the NTSC burst sits on -U (180 deg): I is at 123 deg and
 * Q at 33 deg. */
static const ht_rgb_t SMPTE_TOP[7] = {
    { 0.75, 0.75, 0.75 }, { 0.75, 0.75, 0.00 }, { 0.00, 0.75, 0.75 },
    { 0.00, 0.75, 0.00 }, { 0.75, 0.00, 0.75 }, { 0.75, 0.00, 0.00 },
    { 0.00, 0.00, 0.75 },
};
static const ht_rgb_t SMPTE_MID[7] = {
    { 0.00, 0.00, 0.75 }, { 0.00, 0.00, 0.00 }, { 0.75, 0.00, 0.75 },
    { 0.00, 0.00, 0.00 }, { 0.00, 0.75, 0.75 }, { 0.00, 0.00, 0.00 },
    { 0.75, 0.75, 0.75 },
};

static void ht_smpte_px(const ht_config_t *c, int row, int rows, double ax, double width,
                        double *y, double *u, double *v) {
    const double span = c->white_level - c->black_level;        /* 92.5 IRE */
    const double chroma_span = span / (c->white_level - c->blanking_level);
    double pos = 7.0 * ax / width;                                /* in bar widths */
    if (pos < 0.0) pos = 0.0;
    if (pos >= 7.0) pos = 6.999999;
    *y = *u = *v = 0.0;

    if (row * 100 < rows * 75) {            /* bars + castellations */
        const ht_rgb_t *bars = (row * 100 < rows * 67) ? SMPTE_TOP : SMPTE_MID;
        ht_rgb_t px = bars[(int)pos];
        double uu, vv;
        ht_rgb_to_yuv(c, px.r, px.g, px.b, y, &uu, &vv);
        *u = uu * chroma_span;
        *v = vv * chroma_span;
        return;
    }

    /* Bottom row. 20 IRE of chroma in units of the white-blank span. */
    const double iq_amp = 20.0 / 140.0 / (c->white_level - c->blanking_level);
    const double ire = 1.0 / (140.0 * span);   /* y units per IRE */
    const double i_deg = 123.0 * M_PI / 180.0, q_deg = 33.0 * M_PI / 180.0;
    if (pos < 1.25) {                       /* -I */
        *u = -iq_amp * cos(i_deg);
        *v = -iq_amp * sin(i_deg);
    } else if (pos < 2.5) {                 /* 100% white */
        *y = 1.0;
    } else if (pos < 3.75) {                /* +Q */
        *u = iq_amp * cos(q_deg);
        *v = iq_amp * sin(q_deg);
    } else if (pos < 5.0) {                 /* black */
    } else if (pos < 5.0 + 1.0 / 3.0) {     /* PLUGE 3.5 IRE (super-black) */
        *y = -4.0 * ire;
    } else if (pos < 5.0 + 2.0 / 3.0) {     /* PLUGE 7.5 IRE (black) */
    } else if (pos < 6.0) {                 /* PLUGE 11.5 IRE */
        *y = 4.0 * ire;
    }                                       /* last bar: black */
}

/* --- Public API ----------------------------------------------------------- */

int gui_testgen_clamp_system(int system) {
    if (system < 0 || system >= GUI_TESTGEN_SYSTEM_COUNT) return GUI_TESTGEN_PAL;
    return system;
}

int gui_testgen_clamp_format(int format) {
    if (format < 0 || format >= GUI_TESTGEN_FORMAT_COUNT) return GUI_TESTGEN_FORMAT_VHS;
    return format;
}

size_t gui_testgen_frame_samples(gui_testgen_system_t system, uint32_t sample_rate) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT || sample_rate == 0) {
        return 0;
    }
    /* MESECAM shares the SECAM (625-line) composite geometry. */
    if (system == GUI_TESTGEN_MESECAM) system = GUI_TESTGEN_SECAM;
    const ht_config_t *c = &HT_CONF[system];
    /* Line period: PAL 64us, NTSC 63.5555us */
    double line_us = (c->lines == 525) ? 1001.0 / 15.75 : 64.0;
    double frame_us = line_us * (double)c->lines;
    return (size_t)(frame_us * (double)sample_rate / 1e6 + 0.5);
}

const char *gui_testgen_system_name(gui_testgen_system_t system) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT) return "PAL";
    switch (system) {
        case GUI_TESTGEN_PAL: return "PAL";
        case GUI_TESTGEN_NTSC: return "NTSC";
        case GUI_TESTGEN_SECAM: return "SECAM";
        case GUI_TESTGEN_MESECAM: return "MESECAM";
        default: return "PAL";
    }
}

const char *gui_testgen_format_name(int format) {
    static const char *names[GUI_TESTGEN_FORMAT_COUNT] = {
        "VHS", "S-VHS", "Betamax", "Video8", "Hi8"
    };
    return names[gui_testgen_clamp_format(format)];
}

/* Per-slot pulse kind. The EQ/broad trains pulse at every slot across the
 * field transition; the two fields' H grids sit 625 slots (312.5 lines)
 * apart - the interlace itself. */
typedef enum {
    SL_BLANK = 0,   /* half-line with no pulse */
    SL_H,           /* normal line: H sync + burst (+ active video) */
    SL_EQ,          /* equalizing pulse (2.35us) */
    SL_BROAD        /* vertical broad pulse (27.3us) */
} sl_pulse_t;

/* --- Shared interlaced slot grid ------------------------------------------
 * Built once per frame by BOTH frame generators (composite CVBS and tape
 * RF) so their timing geometry can never drift apart. Pure extraction of
 * the setup that used to live inline in gui_testgen_frame; the composite
 * output is byte-identical (verified with a before/after harness, see the
 * prompt log). */
typedef struct {
    int slots_total;
    int mid;                  /* 625 / 525 slots: field 2 boundary */
    size_t *slot_start;       /* slots_total + 1 sample boundaries */
    uint8_t *slot_type;       /* sl_pulse_t per slot */
    int *slot_hidx;           /* per-field H index, -1 for non-H slots */
    /* Derived per-line constants (hacktv vid_init) */
    int active_left;          /* active area start, samples within the line */
    int active_width;         /* active area width in samples */
    int line_smp;             /* nominal samples per H line (two slots) */
    int hsync_smp;            /* H sync pulse width, samples */
    int eq_smp;               /* equalizing pulse width, samples */
    int broad_smp;            /* broad pulse width, samples */
} ht_grid_t;

static void ht_grid_free(ht_grid_t *g) {
    if (!g) return;
    free(g->slot_start);
    free(g->slot_type);
    free(g->slot_hidx);
    g->slot_start = NULL;
    g->slot_type = NULL;
    g->slot_hidx = NULL;
}

static bool ht_grid_build(const ht_config_t *c, size_t need,
                          uint32_t sample_rate, ht_grid_t *g) {
    const int slots_total = c->slots;
    const int train_len = 3 * c->eq_count;      /* EQ + broad + EQ, every slot */
    const int mid = slots_total / 2;            /* 625 / 525: field 2 boundary */
    /* H-grid slot parity. PAL's first EQ must sit at a HALF-line offset from
     * the last H (parity 0) -> H grid on ODD slots; NTSC's first EQ must sit
     * at an INTEGER offset (parity 0) -> H grid on EVEN slots. */
    const int hpar = (c->lines == 525) ? 0 : 1;
    if (need < (size_t)slots_total) return false;

    /* Slot boundaries spread over the frame so every slot is need/slots +-1
     * samples: the line period stays exact at any rate and the frame has no
     * short last line (a half-line stream jump every frame otherwise). */
    g->slots_total = slots_total;
    g->mid = mid;
    g->slot_start = (size_t *)malloc((size_t)(slots_total + 1) * sizeof(size_t));
    g->slot_type = (uint8_t *)malloc((size_t)slots_total);
    g->slot_hidx = (int *)malloc((size_t)slots_total * sizeof(int));
    if (!g->slot_start || !g->slot_type || !g->slot_hidx) {
        ht_grid_free(g);
        return false;
    }
    for (int s = 0; s <= slots_total; s++) {
        double t = (double)need * (double)s / (double)slots_total;
        g->slot_start[s] = (size_t)(t + 0.5);
    }
    g->slot_start[0] = 0;
    g->slot_start[slots_total] = need;

    /* Per-slot pulse classification and per-field H-line index. The H index
     * (reset at each field boundary) drives the PAL V-switch and SECAM line
     * parity, so the line sequence matches the verified colour phase. */
    {
        int h1 = 0, h2 = 0;
        for (int s = 0; s < slots_total; s++) {
            bool in_train = (s < train_len) || (s >= mid && s < mid + train_len);
            if (in_train) {
                int k = (s < train_len) ? s : s - mid;
                g->slot_type[s] = (uint8_t)((k < c->eq_count) ? SL_EQ
                                 : (k < 2 * c->eq_count) ? SL_BROAD : SL_EQ);
                g->slot_hidx[s] = -1;
            } else if ((s & 1) == hpar) {
                g->slot_type[s] = SL_H;
                g->slot_hidx[s] = (s < mid) ? h1++ : h2++;
            } else {
                g->slot_type[s] = SL_BLANK;
                g->slot_hidx[s] = -1;
            }
        }
    }

    /* hacktv vid_init: active area in samples (per 64us line) */
    g->active_left = (int)(c->active_left_us * (double)sample_rate / 1e6 + 0.5);
    g->active_width = (int)ceil(c->active_width_us * (double)sample_rate / 1e6);
    /* An H line spans two slots; clamp the active window inside it. */
    g->line_smp = 2 * (int)(g->slot_start[1] - g->slot_start[0]);
    if (g->line_smp > 0 && g->active_left + g->active_width > g->line_smp) {
        g->active_width = g->line_smp - g->active_left;
    }

    /* hacktv vid_init: sync pulse sample counts */
    g->hsync_smp = (int)(c->hsync_us * (double)sample_rate / 1e6 + 0.5);
    g->eq_smp = (int)(c->vsync_short_us * (double)sample_rate / 1e6 + 0.5);
    g->broad_smp = (int)(c->vsync_long_us * (double)sample_rate / 1e6 + 0.5);
    return true;
}

/* --- Sub-sample line model (composite) ----------------------------------------
 * Every slot starts at its EXACT position s * need / slots (fractional
 * samples): the old renderer started each line on a whole sample, so NTSC at
 * 40 MSPS (2542.22 samples/line) alternated 2542/2543-sample lines and every
 * line's sync, burst and picture moved +-0.5 sample against the (continuous)
 * subcarrier - measured as 9.3 deg rms / 16 deg peak SCH jitter, which a
 * line comb cannot cancel. Now each sample takes the AREA average of the
 * piecewise-constant line content over its own width (the one edge it may
 * straddle found by bisection), so every edge lands at its exact fractional
 * time; PASS 2's symmetric pulse then shapes it without moving it. */
typedef struct {
    int type;            /* sl_pulse_t */
    bool active;         /* H line inside the picture */
    bool ntsc;
    int row, rows;       /* picture row (SMPTE layout) */
    double pulse;        /* sync pulse width, samples */
    double act_l, act_w; /* active window, samples from the line start */
    double black, white, blank, sync;
    const ht_config_t *c;
} ht_line_t;

/* Base (sync/luma) level and bar chroma u/v (white-blank span units, before
 * the chain gain) at line-relative position x samples. Blanking outside. */
static void ht_line_at(const ht_line_t *L, double x, double *base, double *u, double *v) {
    *u = *v = 0.0;
    *base = L->blank;
    if (x < 0.0) return;
    if (x < L->pulse) { *base = L->sync; return; }
    if (L->type != SL_H || !L->active) return;
    double ax = x - L->act_l;
    if (ax < 0.0 || ax >= L->act_w) return;
    double y, uu, vv;
    if (L->ntsc) {
        ht_smpte_px(L->c, L->row, L->rows, ax, L->act_w, &y, &uu, &vv);
    } else {
        int bar = (int)(8.0 * ax / L->act_w);
        if (bar > 7) bar = 7;
        ht_rgb_to_yuv(L->c, HT_BARS[bar].r, HT_BARS[bar].g, HT_BARS[bar].b, &y, &uu, &vv);
    }
    *base = L->black + y * (L->white - L->black);
    *u = uu;
    *v = vv;
}

/* Area average over [x - 0.5, x + 0.5]: the line content never changes twice
 * within one sample (its shortest feature is microseconds long). */
static void ht_line_cov(const ht_line_t *L, double x, double *base, double *u, double *v) {
    double b0, u0, v0, b1, u1, v1;
    double lo = x - 0.5, hi = x + 0.5;
    ht_line_at(L, lo, &b0, &u0, &v0);
    ht_line_at(L, hi, &b1, &u1, &v1);
    if (b0 == b1 && u0 == u1 && v0 == v1) {
        *base = b0; *u = u0; *v = v0;
        return;
    }
    double a = lo, b = hi;
    for (int i = 0; i < 32; i++) {
        double m = 0.5 * (a + b), bm, um, vm;
        ht_line_at(L, m, &bm, &um, &vm);
        if (bm == b0 && um == u0 && vm == v0) a = m; else b = m;
    }
    double f = 0.5 * (a + b) - lo;          /* share of the sample at the left value */
    *base = b0 * f + b1 * (1.0 - f);
    *u = u0 * f + u1 * (1.0 - f);
    *v = v0 * f + v1 * (1.0 - f);
}

/* Band-limited bar chroma: the line's exact colour-difference steps (found
 * once per line by a coarse scan + bisection on ht_line_at), each rendered
 * as an integrated raised-cosine of pulse length T: the continuous
 * band-limited envelope, sampled at the sample centres. */
typedef struct { double x, du, dv; } ht_cstep_t;

static int ht_chroma_steps(const ht_line_t *L, double scan, ht_cstep_t *st, int max) {
    int n = 0;
    double x0 = L->act_l - 2.0, b0, u0, v0;
    ht_line_at(L, x0, &b0, &u0, &v0);
    const double end = L->act_l + L->act_w + 2.0;
    for (double x1 = x0 + scan; x0 < end && n < max; x0 = x1, x1 += scan) {
        double b1, u1, v1;
        ht_line_at(L, x1, &b1, &u1, &v1);
        if (u1 == u0 && v1 == v0) continue;
        double a = x0, b = x1;
        for (int i = 0; i < 40; i++) {
            double m = 0.5 * (a + b), bm, um, vm;
            ht_line_at(L, m, &bm, &um, &vm);
            if (um == u0 && vm == v0) a = m; else b = m;
        }
        st[n].x = 0.5 * (a + b);
        st[n].du = u1 - u0;
        st[n].dv = v1 - v0;
        n++;
        u0 = u1; v0 = v1;
    }
    return n;
}

/* Step response of a Hann pulse, t in pulse lengths (0.5 at t = 0). */
static inline double ht_hann_step(double t) {
    if (t <= -0.5) return 0.0;
    if (t >= 0.5) return 1.0;
    return t + 0.5 + sin(2.0 * M_PI * t) / (2.0 * M_PI);
}

/* Subcarrier cycles in one frame, exactly (NTSC 227.5 x 525 = 119437.5;
 * PAL 283.75 x 625 + 1 (the 25 Hz offset) = 177344.75): whole quarter
 * cycles, so a NTSC frame pair / PAL frame quad closes on itself. */
static double ht_frame_cycles(const ht_config_t *c) {
    double line_us = (c->lines == 525) ? 1001.0 / 15.75 : 64.0;
    return floor(c->fsc_hz * line_us * 1e-6 * (double)c->lines * 4.0 + 0.5) / 4.0;
}

int gui_testgen_loop_frames(gui_testgen_system_t system) {
    switch (system) {
        case GUI_TESTGEN_PAL: return 4;    /* 4-frame (8-field) PAL sequence */
        case GUI_TESTGEN_NTSC: return 2;   /* 2-frame (4-field) NTSC sequence */
        default: return 2;                 /* SECAM D'R/D'B frame cadence */
    }
}

size_t gui_testgen_frame(gui_testgen_system_t system, int16_t *out, size_t cap,
                         uint32_t sample_rate, uint32_t frame_index) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT) return 0;
    if (!out || sample_rate == 0) return 0;
    /* MESECAM composite = SECAM video (the source signal of a MESECAM VHS
     * recording); only the RF channel differs (MESECAM color-under). */
    if (system == GUI_TESTGEN_MESECAM) system = GUI_TESTGEN_SECAM;
    const ht_config_t *c = &HT_CONF[system];
    size_t need = gui_testgen_frame_samples(system, sample_rate);
    if (cap < need) return 0;

    ht_grid_t grid;
    if (!ht_grid_build(c, need, sample_rate, &grid)) return 0;

    int16_t *base_buf = (int16_t *)calloc(need, sizeof(int16_t));
    int16_t *chroma_buf = (int16_t *)calloc(need, sizeof(int16_t));
    if (!base_buf || !chroma_buf) {
        free(base_buf);
        free(chroma_buf);
        ht_grid_free(&grid);
        return 0;
    }
    /* Local aliases so the passes below keep using the original names. */
    const int slots_total = grid.slots_total;
    const int mid = grid.mid;
    size_t *slot_start = grid.slot_start;
    uint8_t *slot_type = grid.slot_type;
    int *slot_hidx = grid.slot_hidx;
    const int active_left = grid.active_left;
    const int active_width = grid.active_width;
    const int line_smp = grid.line_smp;
    const int hsync_smp = grid.hsync_smp;
    const int eq_smp = grid.eq_smp;
    const int broad_smp = grid.broad_smp;

    /* hacktv vid_init: 16-bit signal levels */
    double level = 1.0;
    double white16 = c->white_level * level;
    double black16 = c->black_level * level;
    double blank16 = c->blanking_level * level;
    double sync16 = c->sync_level * level;

    /* hacktv vid_init: burst parameters (plus the chain gain) */
    const double rate = (double)sample_rate;
    double burst_rise_s = c->burst_rise_us * 1e-6;
    double burst_width_s = c->burst_width_us * 1e-6;
    double burst_amp = c->burst_level_frac * (white16 - blank16) * ht_chroma_gain(c->colour_mode);
    if (burst_amp < 0) burst_amp = -burst_amp;

    /* Exact (fractional) geometry, samples. */
    const double slot_len = (double)need / (double)slots_total;
    const double burst_l_f = c->burst_left_us * 1e-6 * rate;
    const double act_l_f = c->active_left_us * 1e-6 * rate;
    double act_w_f = c->active_width_us * 1e-6 * rate;
    if (act_l_f + act_w_f > 2.0 * slot_len) act_w_f = 2.0 * slot_len - act_l_f;

    /* Continuous subcarrier across frames: the carrier is locked to the
     * generated line rate (exactly ht_frame_cycles per frame) and frame k
     * starts where frame k-1 ended, so consecutive frame_index values make
     * the standard colour-frame sequence (NTSC 2 frames, PAL 4). The old
     * renderer restarted the carrier at phase 0 every frame: a -169 deg
     * (NTSC) / +90 deg (PAL) carrier jump at every frame seam. */
    const double frame_cycles = ht_frame_cycles(c);
    const double cyc_per_smp = frame_cycles / (double)need;
    double cyc0 = fmod((double)frame_index * frame_cycles, 1.0);
    /* PAL V-switch: alternates on EVERY line through the frame and across
     * frames (625 is odd, so it flips frame to frame). The old per-field
     * H-index parity broke the alternation at both field boundaries.
     * Frame 0's first H line stays +V. */
    const int hpar = (c->lines == 525) ? 0 : 1;
    const int train_len = 3 * c->eq_count;
    const int first_h_slot = train_len + (((train_len & 1) != hpar) ? 1 : 0);
    const long long first_line = (first_h_slot - hpar) / 2;   /* frame 0's first H line */
    (void)eq_smp; (void)broad_smp; (void)hsync_smp; (void)active_left;
    (void)active_width; (void)line_smp;

/* PASS 1: baseband luma/sync into base_buf, chroma into chroma_buf.
     * hacktv applies the sync_rise shaping only to the sync/level steps and
     * keeps chroma on its own path; a combined filter would attenuate the
     * 4.43 MHz subcarrier. Positions are fractional (see ht_line_t). */
    for (int s = 0; s < slots_total; s++) {
        size_t ss = slot_start[s];
        if (ss >= need) break;
        if (slot_type[s] == SL_BLANK) continue;  /* blanking (buffers are zeroed) */

        const bool h_line = slot_type[s] == SL_H;
        /* An H line spans this slot plus the next (the active picture spans
         * ~52us of the 64us line); EQ/broad pulses stay in their slot. */
        size_t se = h_line ? ((s + 2 <= slots_total) ? slot_start[s + 2] : need)
                           : slot_start[s + 1];
        const double ls = (double)s * slot_len;          /* exact line/slot start */
        const int field = (s < mid) ? 0 : 1;
        const int a0 = c->act_first[field];
        const int a1 = a0 + 2 * (c->act_lines - 1);

        ht_line_t L;
        L.type = slot_type[s];
        L.active = h_line && s >= a0 && s <= a1;
        L.ntsc = c->colour_mode == HT_NTSC;
        L.row = (s - a0) / 2;
        L.rows = c->act_lines;
        L.pulse = (slot_type[s] == SL_EQ ? c->vsync_short_us
                   : slot_type[s] == SL_BROAD ? c->vsync_long_us : c->hsync_us) * 1e-6 * rate;
        L.act_l = act_l_f;
        L.act_w = act_w_f;
        L.black = black16;
        L.white = white16;
        L.blank = blank16;
        L.sync = sync16;
        L.c = c;

        /* PAL V-switch / SECAM line parity: the frame-continuous line count. */
        int pal_switch = 1;
        if (h_line && c->colour_mode == HT_PAL) {
            long long line = (long long)frame_index * c->lines + (s - hpar) / 2 - first_line;
            pal_switch = ((line & 1) == 0) ? 1 : -1;
        }
        (void)slot_hidx;

        /* Coverage: this sample's right edge is the next sample's left. */
        double lb, lu, lv;
        ht_line_at(&L, (double)ss - ls - 0.5, &lb, &lu, &lv);
        /* Carrier by rotation (exact phase re-synced every 256 samples). */
        const bool qam = h_line && (c->colour_mode == HT_PAL || c->colour_mode == HT_NTSC);
        const double dph = 2.0 * M_PI * cyc_per_smp;
        const double rcd = cos(dph), rsd = sin(dph);
        /* PAL burst sin(phase + ang) = sin*cos(ang) + cos*sin(ang) */
        const double bang = (pal_switch > 0) ? 135.0 * M_PI / 180.0 : 225.0 * M_PI / 180.0;
        const double bca = cos(bang), bsa = sin(bang);
        const double chroma_k = (white16 - blank16) * ht_chroma_gain(c->colour_mode);
        double cc = 1.0, cs = 0.0;
        /* Band-limited bar chroma for picture lines (see HT_CHROMA_RISE). */
        ht_cstep_t steps[48];
        int nsteps = 0, kdone = 0;
        double ubase = 0.0, vbase = 0.0;
        const double cpl = (c->colour_mode == HT_NTSC ? HT_CHROMA_RISE_NTSC_US
                                                       : HT_CHROMA_RISE_PAL_US)
                           / 0.482199 * 1e-6 * rate;    /* Hann pulse, samples */
        if (qam && L.active) nsteps = ht_chroma_steps(&L, 0.2e-6 * rate, steps, 48);
        for (size_t n = ss; n < se && n < need; n++) {
            const double xf = (double)n - ls;   /* fractional line position */
            double signal, u, v, rb, ru, rv;
            ht_line_at(&L, xf + 0.5, &rb, &ru, &rv);
            if (lb == rb && lu == ru && lv == rv) {
                signal = lb; u = lu; v = lv;
            } else {
                ht_line_cov(&L, xf, &signal, &u, &v);
            }
            lb = rb; lu = ru; lv = rv;
            double chroma = 0.0;

            if (qam) {
                if (n == ss || ((n - ss) & 255) == 0) {
                    double cyc = cyc0 + (double)n * cyc_per_smp;
                    double phase = 2.0 * M_PI * (cyc - floor(cyc));
                    cc = cos(phase);
                    cs = sin(phase);
                }
                /* Burst on every H line, envelope at the exact position */
                double w = ht_burstwin((xf - burst_l_f) / rate, burst_width_s, burst_rise_s);
                if (w > 0.0) {
                    if (c->colour_mode == HT_NTSC) {
                        /* NTSC burst: 180 degrees (hacktv: -sin) */
                        chroma += -w * burst_amp * cs;
                    } else {
                        /* PAL burst: +-135 degrees with V-switch */
                        chroma += w * burst_amp * (cs * bca + cc * bsa);
                    }
                }
                /* QAM on the subcarrier: U rides sin, V rides cos with the
                 * PAL V-switch - the band-limited envelope from the line's
                 * steps (0 outside the picture). */
                (void)u; (void)v;
                if (nsteps) {
                    while (kdone < nsteps && xf >= steps[kdone].x + 0.5 * cpl) {
                        ubase += steps[kdone].du;
                        vbase += steps[kdone].dv;
                        kdone++;
                    }
                    double eu = ubase, ev = vbase;
                    for (int k = kdone; k < nsteps && xf > steps[k].x - 0.5 * cpl; k++) {
                        double sfrac = ht_hann_step((xf - steps[k].x) / cpl);
                        eu += steps[k].du * sfrac;
                        ev += steps[k].dv * sfrac;
                    }
                    if (eu != 0.0 || ev != 0.0)
                        chroma += chroma_k * (eu * cs + ev * pal_switch * cc);
                }
                double nc = cc * rcd - cs * rsd;
                cs = cs * rcd + cc * rsd;
                cc = nc;
            }

            double base_v = signal * 32767.0;
            if (base_v > 32767.0) base_v = 32767.0;
            if (base_v < -32768.0) base_v = -32768.0;
            base_buf[n] = ht_r16(base_v);

            double chroma_v16 = chroma * 32767.0;
            if (chroma_v16 > 32767.0) chroma_v16 = 32767.0;
            if (chroma_v16 < -32768.0) chroma_v16 = -32768.0;
            chroma_buf[n] = ht_r16(chroma_v16);
        }
    }

    /* PASS 1.5 (SECAM only): the full hacktv _vid_render_secam FM chain.
     * Per H line: build the D control signal (D'b rest 4.25 MHz / D'r rest
     * 4.40625 MHz around the 4.328125 MHz FM centre, INT16_MAX = +-1 MHz),
     * vertically averaged with the previous line's opposite component
     * (hacktv chrominance_buffer second half); the first 9 H lines of each
     * field carry the field ident ramp instead (D'r +350 kHz over 15 us,
     * D'b -350 kHz over 18 us, from active_left). Then: the D'x pre-filter
     * (Gaussian, -3 dB at 0.8 MHz, fitted to a real Philips encoder;
     * hacktv: 15-tap kaiser at 1.7 MHz), the
     * SECAM video pre-emphasis horn (85/255 kHz,
     * DC gain 1, x3 HF; hacktv's 16 MHz IIR redesigned here for the stream
     * rate), per-type deviation clamps (D'b [CB-350k, CB+506k], D'r
     * [CR-506k, CR+350k]), then FM render across the 56.9 us window
     * (5.6 us start, 1 us IRT1090 raised-cosine edges - hacktv reuses the
     * burst window constants for the SECAM chroma envelope) with the
     * complex cloche gain and a per-line phase reset (0 or pi by global
     * line % 3). */
    if (c->colour_mode == HT_SECAM) {
        const double rest_db = (HT_SECAM_DB - HT_SECAM_FM) / HT_SECAM_DEV;
        const double rest_dr = (HT_SECAM_DR - HT_SECAM_FM) / HT_SECAM_DEV;
        const double dmin_db = (HT_SECAM_DB - HT_SECAM_FM - 350e3) / HT_SECAM_DEV;
        const double dmax_db = (HT_SECAM_DB - HT_SECAM_FM + 506e3) / HT_SECAM_DEV;
        const double dmin_dr = (HT_SECAM_DR - HT_SECAM_FM - 506e3) / HT_SECAM_DEV;
        const double dmax_dr = (HT_SECAM_DR - HT_SECAM_FM + 350e3) / HT_SECAM_DEV;
        const double fm_level = (white16 - blank16) * 32767.0 * ht_chroma_gain(c->colour_mode);
        const double chroma_left_f = c->hsync_us * 1e-6 * rate;
        const size_t chroma_len = (size_t)((HT_SECAM_WIN_US * 1e-6
                                + HT_SECAM_RISE_US * HT_SECAM_IRT1090 * 1e-6)
                                * (double)sample_rate) + 1;
        const double env_width = HT_SECAM_WIN_US * 1e-6;
        const double env_rise = HT_SECAM_RISE_US * HT_SECAM_IRT1090 * 1e-6;
        double *d_line = (double *)malloc((size_t)(line_smp + 8) * sizeof(double));
        double *d_src = (double *)malloc((size_t)(line_smp + 8) * sizeof(double));
        double lp[2 * HT_SECAM_DX_HALF_MAX + 1];
        int lp_half;
        double pe_b0, pe_b1, pe_fb;

        if (!d_line || !d_src) {
            free(d_line); free(d_src);
            ht_grid_free(&grid);
            free(base_buf); free(chroma_buf);
            return 0;
        }
        /* D'R / D'B pre-filter before the pre-emphasis: Gaussian, -3 dB at
         * 0.8 MHz, no overshoot - FITTED to a real broadcast encoder: a
         * Philips PM5544 SECAM generator captured on the MS2130 (harrypm,
         * 2026-11-17). Its bar transitions, inverse-belled and aligned per
         * edge, match this filter at 11 kHz RMS deviation (Gaussian 1.3 MHz
         * 20 kHz, the old hacktv-style 15-tap 26 kHz; best 0.75-0.8 MHz).
         * hacktv's 15-tap kaiser "1.7 MHz" low-pass, redesigned at the
         * stream rate, truncated inside the sinc's first zero (+-175 ns at
         * 40 MSPS) and band-limited far less: the pre-emphasised transition
         * spikes hit the deviation stops harder than a broadcast encoder's,
         * and each clipped spike leaves a de-emphasis tail that differs
         * between D'R and D'B (the curved SECAM vectorscope transitions).
         * Yl..B vectorscope path off-path share (noise-free, de-emphasised):
         * Philips 2.9%, this filter 1.5%, the old 15-tap 2.6%. */
        {
            const double sigma_f = HT_SECAM_DX_F3 / sqrt(log(2.0));   /* |H(f3)| = 1/sqrt(2) */
            const double st = (double)sample_rate / (2.0 * M_PI * sigma_f);
            double gs = 0.0;
            lp_half = (int)ceil(4.0 * st);
            if (lp_half > HT_SECAM_DX_HALF_MAX) lp_half = HT_SECAM_DX_HALF_MAX;
            for (int j = -lp_half; j <= lp_half; j++) {
                lp[j + lp_half] = exp(-0.5 * (double)j * (double)j / (st * st));
                gs += lp[j + lp_half];
            }
            for (int j = 0; j <= 2 * lp_half; j++) lp[j] /= gs;
        }
        /* Linear cloche biquad (see the FM render), normalised by d0. */
        double bell_n0, bell_n1, bell_n2, bell_d1, bell_d2;
        {
            const double w0 = 2.0 * M_PI * HT_SECAM_F0;
            const double K = w0 / tan(w0 / (2.0 * rate)), K2 = K * K;
            const double a2 = 0.115 * 16.0, a1 = 0.115 * w0, a0 = 0.115 * 16.0 * w0 * w0;
            const double b2 = 1.26, b1 = w0, b0 = 1.26 * w0 * w0;
            const double d0 = b2 * K2 + b1 * K + b0;
            bell_n0 = (a2 * K2 + a1 * K + a0) / d0;
            bell_n1 = (-2.0 * a2 * K2 + 2.0 * a0) / d0;
            bell_n2 = (a2 * K2 - a1 * K + a0) / d0;
            bell_d1 = (-2.0 * b2 * K2 + 2.0 * b0) / d0;
            bell_d2 = (b2 * K2 - b1 * K + b0) / d0;
        }
        {
            /* hacktv's IIR (in {1.0,-0.90456054} / out {2.90456054,
             * -2.80912108}; y = x*b0 + x_prev*b1 - y_prev*a1) is the
             * bilinear transform of the 85/255 kHz horn designed at 16 MHz. */
            const double cwin = 2.0 * (double)sample_rate;
            double wz = 2.0 * M_PI * HT_SECAM_PE_FZ;
            double wp = 2.0 * M_PI * HT_SECAM_PE_FP;
            double z0 = (cwin - wz) / (cwin + wz);
            double p1 = (cwin - wp) / (cwin + wp);
            pe_b0 = (wp / wz) * (wz + cwin) / (wp + cwin);
            pe_b1 = -pe_b0 * z0;
            pe_fb = p1;
        }

        for (int s = 0; s < slots_total; s++) {
            size_t ss, ss2, len;
            int hidx, field, line;
            long long global;
            bool dr, is_ident;
            double rest, dmin, dmax, phase;

            if (slot_type[s] != SL_H || slot_hidx[s] < 0) continue;
            ss = slot_start[s];
            ss2 = (s + 2 <= slots_total) ? slot_start[s + 2] : need;
            if (ss >= need) break;
            len = (ss2 > need ? need : ss2) - ss;
            if (len == 0) continue;
            /* Sample 0 of this line sits fo samples after the exact line
             * start (0 at 40 MSPS, where every 625-line slot is whole). */
            const double fo = (double)ss - (double)s * slot_len;

            hidx = slot_hidx[s];
            field = (s < mid) ? 0 : 1;
            /* Line numbering picked so D'r lands on the decoder's EVEN
             * field rows (the real card's cadence: drOnEven=1, measured).
             * The frame stride 625 (odd) flips the D'r/D'b cadence every
             * frame, as on a real transmitter chain. */
            line = hidx + ((field == 0) ? 7 : 321);
            global = (long long)frame_index * 625 + line;
            dr = (global & 1) != 0;
            is_ident = hidx < 9;
            rest = dr ? rest_dr : rest_db;
            dmin = dr ? dmin_dr : dmin_db;
            dmax = dr ? dmax_dr : dmax_db;

            if (is_ident) {
                double dev = (dr ? 1.0 : -1.0) * HT_SECAM_FSYNC / HT_SECAM_DEV;
                double rw = (dr ? 15e-6 : 18e-6);
                for (size_t x = 0; x < len; x++) {
                    double t = ((double)x + fo - act_l_f) / rate / rw;
                    if (t < 0.0) t = 0.0;
                    if (t > 1.0) t = 1.0;
                    d_line[x] = rest + dev * t;
                }
            } else {
                /* The REAL card transmits the RAW colour difference per
                 * line (no hacktv transmit-side vertical averaging - the
                 * receiver's one-line hold does the averaging; measured on
                 * the capture: the cyan D'b plateau reads 0.75x the
                 * standard deviation with no D'r blend). */
                for (size_t x = 0; x < len; x++) {
                    double ax = (double)x + fo - act_l_f;
                    if (ax < 0.0 || ax >= act_w_f) {
                        d_line[x] = rest;
                        continue;
                    }
                    int bar = (int)(8.0 * ax / act_w_f);
                    if (bar > 7) bar = 7;
                    if (bar < 0) bar = 0;
                    double y, u, v;
                    ht_rgb_to_yuv(c, HT_BARS[bar].r, HT_BARS[bar].g,
                                  HT_BARS[bar].b, &y, &u, &v);
                    /* u/v are the SECAM deviations in Hz (the SECAM
                     * config's ev_co/eu_co), at the standard deviation. */
                    double own_hz = HT_SECAM_DEV_GAIN * (dr ? v : u);
                    d_line[x] = (own_hz + (dr ? HT_SECAM_DR : HT_SECAM_DB)
                                 - HT_SECAM_FM) / HT_SECAM_DEV;
                }
            }

            /* D'x pre-filter (Gaussian 0.8 MHz), block mode (edges held) */
            memcpy(d_src, d_line, len * sizeof(double));
            for (size_t x = 0; x < len; x++) {
                double acc = 0.0;
                for (int j = -lp_half; j <= lp_half; j++) {
                    ssize_t idx = (ssize_t)x + j;
                    if (idx < 0) idx = 0;
                    if ((size_t)idx >= len) idx = (ssize_t)len - 1;
                    acc += lp[j + lp_half] * d_src[idx];
                }
                d_line[x] = acc;
            }

            /* Video pre-emphasis horn */
            {
                double x_prev = 0.0, y_prev = 0.0;
                for (size_t x = 0; x < len; x++) {
                    double xv = d_line[x];
                    double yv = pe_b0 * xv + pe_b1 * x_prev + pe_fb * y_prev;
                    x_prev = xv;
                    y_prev = yv;
                    d_line[x] = yv;
                }
            }

            /* Deviation limit stops */
            for (size_t x = 0; x < len; x++) {
                if (d_line[x] < dmin) d_line[x] = dmin;
                if (d_line[x] > dmax) d_line[x] = dmax;
            }

            /* FM render, the cloche, the envelope (the window and the
             * per-line phase reference start at the exact chroma_left time;
             * at 40 MSPS fo is 0 and this is the whole-sample form).
             * The cloche (HF pre-correction) is the LINEAR bell network on
             * the constant-amplitude carrier: A(f) = 0.115 (1 + j16u) /
             * (1 + j1.26u), u = f/f0 - f0/f, i.e. with j u = s/w0 + w0/s
             * H(s) = 0.115 (16 s^2 + w0 s + 16 w0^2) / (1.26 s^2 + w0 s +
             * 1.26 w0^2), bilinear pre-warped at w0. hacktv evaluates A(f)
             * per sample at the INSTANTANEOUS frequency instead - right for
             * a steady carrier, but during every colour transition that
             * twists the phase in a way no real bell (and no receiver's
             * anti-bell) does: measured on the vectorscope as wiggling,
             * U/V-skewed SECAM transitions (12% of the points off the
             * straight transitions; 2.9% with the linear network). */
            phase = ((global % 3) == 0) ? 0.0 : M_PI;
            size_t x_first = (size_t)ceil(chroma_left_f - fo);
            if (x_first < len) {
                double w0 = (double)x_first + fo - chroma_left_f;
                phase += 2.0 * M_PI * (HT_SECAM_FM + d_line[x_first] * HT_SECAM_DEV) * w0 / rate;
            }
            double bx1 = 0.0, bx2 = 0.0, by1 = 0.0, by2 = 0.0;
            for (size_t x = x_first; x < len; x++) {
                double wf = (double)x + fo - chroma_left_f;
                double env, freq, cv;
                if (wf >= (double)chroma_len) break;
                env = ht_rc_window(wf / rate, env_rise / 2.0, env_width, env_rise);
                freq = HT_SECAM_FM + d_line[x] * HT_SECAM_DEV;
                double carrier = cos(phase);
                double belled = bell_n0 * carrier + bell_n1 * bx1 + bell_n2 * bx2
                              - bell_d1 * by1 - bell_d2 * by2;
                bx2 = bx1; bx1 = carrier; by2 = by1; by1 = belled;
                cv = fm_level * belled * env;
                chroma_buf[ss + x] = ht_r16(cv);
                phase += 2.0 * M_PI * freq / (double)sample_rate;
            }
        }
        free(d_line);
        free(d_src);
    }

    /* PASS 2: edge shaping of the BASE signal only (the chroma is added
     * unfiltered afterwards). A symmetric raised-cosine (Hann) PULSE of
     * length T: every level step becomes the integrated-raised-cosine
     * S-curve, centred on the step's exact (fractional) position, 10-90% =
     * 0.4822 T = sync_rise_us. The earlier kernel was a HALF-cosine ramp
     * (0 -> 1, not 0 -> 1 -> 0): one-sided, so every sync and luma edge came
     * out lopsided and ~3.5 samples (87 ns at 40 MSPS) early against the
     * chroma - measured as the burst sitting 5.85 us after the sync 50%
     * point instead of 5.60. */
    const double pulse_len = c->sync_rise_us / 0.482199 * 1e-6 * rate;
    int kern_half = (int)floor(0.5 * pulse_len);
    if (kern_half < 0) kern_half = 0;
    if (2 * kern_half + 1 > (int)need) kern_half = ((int)need - 1) / 2;
    const int rise_n = 2 * kern_half + 1;
    double *kern = (double *)malloc((size_t)rise_n * sizeof(double));
    if (!kern) {
        free(base_buf);
        free(chroma_buf);
        ht_grid_free(&grid);
        return 0;
    }
    {
        double sum = 0.0;
        for (int i = 0; i < rise_n; i++) {
            double j = (double)(i - kern_half);
            kern[i] = (pulse_len > 0.0 && fabs(j) < 0.5 * pulse_len)
                ? 0.5 + 0.5 * cos(2.0 * M_PI * j / pulse_len) : 0.0;
            sum += kern[i];
        }
        if (sum <= 0.0) { kern[kern_half] = 1.0; sum = 1.0; }
        for (int i = 0; i < rise_n; i++) kern[i] /= sum;
    }
    for (size_t n = 0; n < need; n++) {
        double base_f = 0.0;
        for (int i = 0; i < rise_n; i++) {
            /* The stream is continuous: wrap at both frame ends. */
            long long idx = (long long)n - kern_half + i;
            if (idx < 0) idx += (long long)need;
            else if (idx >= (long long)need) idx -= (long long)need;
            base_f += kern[i] * (double)base_buf[idx];
        }
        double value = base_f;
        if (value > 32767.0) value = 32767.0;
        if (value < -32768.0) value = -32768.0;
        out[n] = ht_r16(value);
    }
    free(kern);
    free(base_buf);

    /* SECAM only: the luminance video band-reject notch (hacktv
     * secam_l_fir, ~6 dB deep around the chroma band) over the active
     * area, so luma edges cannot beat with the FM subcarrier. */
    if (c->colour_mode == HT_SECAM) {
        double nt[51];
        double sum = 0.0;
        ht_fir_band_reject(nt, 51, (double)sample_rate,
                           HT_SECAM_FM - 1e6, HT_SECAM_FM + 1e6, 1.0);
        nt[25] += 0.5;
        for (int i = 0; i < 51; i++) sum += nt[i];
        for (int i = 0; i < 51; i++) nt[i] /= sum;
        for (int s = 0; s < slots_total; s++) {
            size_t ss, ss2, a0n, a1n;
            double *tmp;
            if (slot_type[s] != SL_H || slot_hidx[s] < 0) continue;
            ss = slot_start[s];
            ss2 = (s + 2 <= slots_total) ? slot_start[s + 2] : need;
            a0n = ss + (size_t)active_left;
            a1n = (ss2 > need ? need : ss2);
            if (a1n > a0n + (size_t)active_width) a1n = a0n + (size_t)active_width;
            if (a0n >= need || a1n <= a0n) continue;
            tmp = (double *)malloc((a1n - a0n) * sizeof(double));
            if (!tmp) break;
            for (size_t n = a0n; n < a1n; n++) tmp[n - a0n] = (double)out[n];
            for (size_t n = a0n; n < a1n; n++) {
                double acc = 0.0;
                for (int j = 0; j < 51; j++) {
                    ssize_t idx = (ssize_t)(n - a0n) + j - 25;
                    if (idx < 0) idx = 0;
                    if ((size_t)idx >= a1n - a0n) idx = (ssize_t)(a1n - a0n) - 1;
                    acc += nt[j] * tmp[idx];
                }
                out[n] = ht_r16(acc);
            }
            free(tmp);
        }
    }

    ht_grid_free(&grid);

    /* Chroma add (PAL/NTSC QAM from PASS 1; SECAM FM from PASS 1.5) */
    for (size_t n = 0; n < need; n++) {
        double value = (double)out[n] + (double)chroma_buf[n];
        if (value > 32767.0) value = 32767.0;
        if (value < -32768.0) value = -32768.0;
        out[n] = ht_r16(value);
    }
    free(chroma_buf);

    /* PASS 3: the AC-coupling baseline bump at each field boundary (frame
     * start and mid-frame, the two field VBI positions). The real chain's
     * input coupling charges on the broad pulses and decays after them
     * (measured on the EBU reference: the porch -1,434 -> +3,598, decay
     * tau ~10 lines, the sync step intact). Smooth everywhere, so the frame
     * seam stays continuous and the VBI pulse gates are not disturbed. The
     * bump lifts the early-field luma max, which is what the decoder's
     * white commit (blank + 90% of the filtered luma max) is calibrated
     * against; without it the commit lands 10% below the plateau and the
     * top of the picture clamps flat. No DC bias: the calibrated
     * -0.30/+0.70 waveform readout is preserved. */
    {
        double lines_per_frame = (double)c->lines;
        double half = lines_per_frame / 2.0;   /* 312.5 / 262.5 */
        double amp = HT_SAG_PEAK * 32767.0;
        /* The four exponentials advance by a constant factor per sample;
         * multiply instead of calling exp() per sample (re-synced exactly
         * every 4096 samples). exp(-(t - P)/tau) clamped at 1 equals
         * exp(-max(0, t - P)/tau). */
        const double dl = lines_per_frame / (double)need;
        const double kr = exp(-dl / HT_SAG_TAU_RISE), kd = exp(-dl / HT_SAG_TAU_DECAY);
        double er1 = 1.0, ed1 = 1.0, er2 = 1.0, ed2 = 1.0;
        for (size_t n = 0; n < need; n++) {
            double line = (double)n * dl;
            double d2 = line - half;
            if ((n & 4095) == 0) {
                er1 = exp(-line / HT_SAG_TAU_RISE);
                ed1 = exp(-(line - HT_SAG_PIC_START) / HT_SAG_TAU_DECAY);
                er2 = exp(-d2 / HT_SAG_TAU_RISE);
                ed2 = exp(-(d2 - HT_SAG_PIC_START) / HT_SAG_TAU_DECAY);
            }
            double b1 = amp * (1.0 - er1) * (ed1 < 1.0 ? ed1 : 1.0);
            double b2 = 0.0;
            if (d2 >= 0.0) {
                b2 = amp * (1.0 - er2) * (ed2 < 1.0 ? ed2 : 1.0);
            }
            er1 *= kr; ed1 *= kd; er2 *= kr; ed2 *= kd;
            double value = (double)out[n] + b1 + b2;
            if (value > 32767.0) value = 32767.0;
            if (value < -32768.0) value = -32768.0;
            out[n] = ht_r16(value);
        }
    }

    return need;
}

/* --- Tape RF frame generation ---------------------------------------------
 * One full interlaced frame of VHS-family tape RF for the Simulated device's
 * Channel B, built on the SAME slot grid as the composite generator (the
 * ht_grid_t extraction above) so both channels share identical timing
 * geometry. The physical modulation follows this repo's own validated
 * synthetic RF oracles - test/gui_rf_preview_test.c make_rf_signal_profile()
 * for PAL/NTSC VHS and test/gui_rf_secam_test.c make_source() for native
 * SECAM VHS - so the generated RF decodes in the GUI's VHS FM preview panel
 * (the gui_rf_preview.c profiles):
 *
 *   - FM luma: sync tip at sync_hz, blanking at blank_hz, active luma at
 *     blank_hz + 100 * hz_ire * Y, all through the record pre-emphasis that
 *     is the inverse of the decoder's de-emphasis shelf (PAL/SECAM 100/400
 *     ns, NTSC 120/320 ns). Endpoints per the rf_profiles table: PAL/SECAM
 *     3.8/4.1/4.8 MHz at 7000 Hz/IRE, NTSC 3.4/3.6857/4.4 MHz at
 *     7142.857 Hz/IRE.
 *   - PAL/NTSC color-under: QAM at the profile carrier (PAL 626953 Hz,
 *     NTSC 629370.63 Hz) with the per-field track rotation (PAL 0 / -90
 *     degrees per line, NTSC -90 / +90), PAL swinging burst + V-switch,
 *     burst window from the composite config (PAL 5.6..7.85 us, NTSC
 *     5.3..7.8 us), amplitudes 0.72 FM + 0.20 chroma.
 *   - Native SECAM VHS: luma as PAL; line-sequential D'R/D'B through the
 *     85/255 kHz LF pre-correction one-pole with the composite ident
 *     bottles, chroma FM at the studio frequencies DIVIDED BY FOUR (rests
 *     1.0625 / 1.1015625 MHz on tape), carrier envelope from 5.6 us
 *     through the active area, amplitudes 0.72 FM + 0.15 chroma. No PAL
 *     burst or head phase rotation.
 *
 * The composite chain's HackDAC calibrations (0.75x chroma gain and the
 * AC-coupling bump) are deliberately NOT applied here: they model the
 * composite capture card, not the tape RF chain, and the RF decoder
 * calibrates its display levels from the FM deviation itself. */
size_t gui_testgen_rf_frame(gui_testgen_system_t system, gui_testgen_format_t format,
                            int16_t *out, size_t cap,
                             uint32_t sample_rate, uint32_t frame_index) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT) return 0;
    if (!out || sample_rate == 0) return 0;

    /* MESECAM: SECAM (625-line) geometry + studio chroma, but the color-under
     * lands at the MESECAM mixed-down frequencies (see the chroma branch). */
    const bool mesecam = (system == GUI_TESTGEN_MESECAM);
    if (mesecam) system = GUI_TESTGEN_SECAM;
    format = (gui_testgen_format_t)gui_testgen_clamp_format(format);
    /* SECAM/MESECAM are VHS-family only (the decoder profiles define no
     * SECAM Beta/8mm variants); force VHS for those systems. */
    if (system == GUI_TESTGEN_SECAM) format = GUI_TESTGEN_FORMAT_VHS;

    const ht_config_t *c = &HT_CONF[system];
    size_t need = gui_testgen_frame_samples(system, sample_rate);
    if (cap < need) return 0;

    ht_grid_t grid;
    if (!ht_grid_build(c, need, sample_rate, &grid)) return 0;

    const bool pal_family = (c->lines == 625);
    const bool secam = (c->colour_mode == HT_SECAM);

    /* Per (family, format) physical endpoints from the tape-decode-rs
     * profiles: ire0/blank (0 IRE), sync tip (-40 IRE), Hz/IRE (white =
     * blank + 100*hz_ire), color-under carrier, and the per-field chroma
     * track rotation in quarter-turns per line (field0, field1). The VHS
     * entries match the previously hardcoded values exactly. */
    typedef struct {
        double blank_hz, sync_hz, hz_ire, cc_hz;
        int rot_a, rot_b;
    } rf_fmt_params_t;
    static const rf_fmt_params_t RF_PARAMS[2][GUI_TESTGEN_FORMAT_COUNT] = {
        /* 625-line family (PAL luma geometry) */
        {
            /*                    blank      sync      Hz/IRE   cc           rotA rotB */
            [GUI_TESTGEN_FORMAT_VHS]     = { 4100000.0, 3820000.0, 7000.0,  626953.0,    0,  -1 },
            [GUI_TESTGEN_FORMAT_SVHS]    = { 5880000.0, 5432000.0, 11200.0, 626953.0,    0,  -1 },
            [GUI_TESTGEN_FORMAT_BETAMAX] = { 4220000.0, 3828000.0, 9800.0,  687500.0,    0,   0 },
            [GUI_TESTGEN_FORMAT_VIDEO8]  = { 4560000.0, 4224000.0, 8400.0,  732421.875,  0,  -1 },
            [GUI_TESTGEN_FORMAT_HI8]     = { 6300000.0, 5740000.0, 14000.0, 732421.875,  0,  -1 },
        },
        /* 525-line family (NTSC) */
        {
            [GUI_TESTGEN_FORMAT_VHS]     = { 3685714.2857142854, 3400000.0, 7142.857142857143, 629370.63,  -1,  1 },
            [GUI_TESTGEN_FORMAT_SVHS]    = { 5857142.857142857,   5400000.0, 11428.57142857143,  629370.63,  -1,  1 },
            [GUI_TESTGEN_FORMAT_BETAMAX] = { 3942857.1428571427,  3600000.0, 8571.42857142857,   688374.13,   2,  0 },
            [GUI_TESTGEN_FORMAT_VIDEO8]  = { 4542857.142857143,   4200000.0, 8571.42857142857,   743444.06,   0,  2 },
            [GUI_TESTGEN_FORMAT_HI8]     = { 6271428.571428571,   5700000.0, 14285.71428571429,  743444.06,   0,  2 },
        },
    };
    const rf_fmt_params_t *rfp = &RF_PARAMS[pal_family ? 0 : 1][format];

    const double blank_hz = rfp->blank_hz;
    const double sync_hz  = rfp->sync_hz;
    const double hz_ire   = rfp->hz_ire;
    const double carrier  = rfp->cc_hz;
    const double fm_amp = 0.72;
    const double cu_amp = 0.20;   /* PAL/NTSC color-under amplitude */
    const double se_amp = 0.15;   /* SECAM/MESECAM chroma FM amplitude */

    /* Record pre-emphasis: the inverse of the decoder's de-emphasis shelf
     * (first-order bilinear, zero/pole swapped). PAL/SECAM 100/400 ns,
     * NTSC 120/320 ns - the oracle constants. */
    const double tau_zero = pal_family ? 100e-9 : 120e-9;
    const double tau_pole = pal_family ? 400e-9 : 320e-9;
    const double pk = 2.0 * (double)sample_rate;
    const double pe_den = 1.0 + pk * tau_zero;
    const double pe_b0 = (1.0 + pk * tau_pole) / pe_den;
    const double pe_b1 = (1.0 - pk * tau_pole) / pe_den;
    const double pe_a1 = (1.0 - pk * tau_zero) / pe_den;

    /* Native SECAM chroma chain constants: LF pre-correction one-pole at
     * 255 kHz; studio rests/deviations; divided-by-four tape frequencies. */
    const double secam_decay = exp(-2.0 * M_PI * HT_SECAM_PE_FP
                                   / (double)sample_rate);
    const double cu_burst_u = pal_family ? -(150.0 / 700.0) / 1.41421356237309515
                                         : -(150.0 / 700.0);
    const double cu_burst_v = pal_family ?  (150.0 / 700.0) / 1.41421356237309515
                                         : 0.0;
    const double burst_end_us = c->burst_left_us + c->burst_width_us;

    /* Per-sample state (frame-local). Phases and filter states restart at
     * each frame boundary; the FM demod is phase-insensitive and the
     * color-under decode is burst-referenced per line, exactly as with the
     * composite generator's per-frame subcarrier phase. */
    double prev_base = 0.0, prev_pre = 0.0;
    double fm_phase = 0.0;
    double cc_phase = 0.0;
    double colour_state = 0.0;
    const double dt = 1.0 / (double)sample_rate;

    for (int s = 0; s < grid.slots_total; s++) {
        const size_t ss = grid.slot_start[s];
        if (ss >= need) break;
        const size_t se = grid.slot_start[s + 1];

        /* The SL_H pass below spans TWO slots (the line plus the following
         * blank slot). Unlike the composite generator - where blank slots
         * write nothing - this generator must write every sample (the FM
         * carriers are continuous), so a blank slot that is already covered
         * by the preceding H pass must be skipped here or it would overwrite
         * the second half of every H line with blank-level RF. Standalone
         * blank slots (field boundaries) still write. */
        if (grid.slot_type[s] == SL_BLANK && s > 0 &&
            grid.slot_type[s - 1] == SL_H) {
            continue;
        }

        /* Per-slot sync-tip window and line extent. */
        size_t sync_width;
        size_t line_end;
        int hidx = -1;
        int field = 0;
        bool picture = false;
        switch (grid.slot_type[s]) {
            case SL_EQ:    sync_width = (size_t)grid.eq_smp;    line_end = se; break;
            case SL_BROAD: sync_width = (size_t)grid.broad_smp; line_end = se; break;
            case SL_BLANK: sync_width = 0;                      line_end = se; break;
            default: {  /* SL_H: one full line across two slots */
                sync_width = (size_t)grid.hsync_smp;
                hidx = grid.slot_hidx[s];
                field = (s < grid.mid) ? 0 : 1;
                line_end = (s + 2 <= grid.slots_total) ? grid.slot_start[s + 2]
                                                       : need;
                int a0 = c->act_first[field];
                int a1 = a0 + 2 * (c->act_lines - 1);
                picture = (s >= a0 && s <= a1);
                break;
            }
        }
        if (line_end > need) line_end = need;

        /* Per-line constants. SECAM D'R/D'B cadence: the RF decoder's
         * identification flywheel (gui_rf_preview.c, matching the synthetic
         * RF oracle and ITU-R BT.470-6) expects the line parity to be the
         * SAME for both fields of a frame and to flip with the odd 625-line
         * frame stride. The RF decoder numbers each field's H lines with a
         * one-line interlace offset (field 1's hidx k sits one decoder line
         * later than field 0's), so (hidx + field) is the decoder-line
         * parity basis; the composite CH1 generator uses its own validated
         * cadence, which is why the two channels differ here. */
        const bool ident = (hidx >= 0 && hidx < 9);
        bool dr = false;
        if (hidx >= 0) {
            bool parity_basis = ((hidx + field) & 1) == 0;
            bool frame_flip = (frame_index & 1) != 0;
            dr = parity_basis != frame_flip;
        }
        const int vswitch = (pal_family && hidx > 0 && (hidx & 1)) ? -1 : 1;
        /* Track rotation from the format table: quarter-turns per line,
         * per field (PAL VHS 0/-1, NTSC VHS -1/+1, NTSC Betamax 180/0,
         * NTSC Video8/Hi8 0/+180, PAL Betamax none). */
        const int rot_units = (hidx < 0) ? 0
            : (field ? rfp->rot_b : rfp->rot_a);
        const double track_rot = (double)rot_units * (M_PI / 2.0);

        for (size_t n = ss; n < line_end && n < need; n++) {
            const size_t x = n - ss;
            const bool in_sync = (x < sync_width);
            const double x_us = (double)x * 1e6 / (double)sample_rate;
            const double t = (double)n * dt;

            /* Active picture content: the same 75% bars as CH1. */
            double y = 0.0, u = 0.0, v = 0.0, dr_y = 0.0, db_y = 0.0;
            bool active_px = false;
            if (hidx >= 0 && picture && !in_sync &&
                x >= (size_t)grid.active_left &&
                x < (size_t)grid.active_left + (size_t)grid.active_width) {
                int ax = (int)x - grid.active_left;
                int bar = (ax * 8) / grid.active_width;
                if (bar > 7) bar = 7;
                if (bar < 0) bar = 0;
                double r = HT_BARS[bar].r, g = HT_BARS[bar].g, b = HT_BARS[bar].b;
                y = 0.299 * r + 0.587 * g + 0.114 * b;
                u = 0.492 * (b - y);
                v = 0.877 * (r - y);
                dr_y = r - y;
                db_y = b - y;
                active_px = true;
            }

            /* --- FM luma through the record pre-emphasis --- */
            double base = in_sync ? (sync_hz - blank_hz) : (100.0 * hz_ire * y);
            double pre = pe_b0 * base + pe_b1 * prev_base - pe_a1 * prev_pre;
            prev_base = base;
            prev_pre = pre;
            double fm_hz = blank_hz + pre;
            if (fm_hz < 2400000.0) fm_hz = 2400000.0;
            if (fm_hz > (secam ? 6500000.0 : 6800000.0)) {
                fm_hz = secam ? 6500000.0 : 6800000.0;
            }
            fm_phase += 2.0 * M_PI * fm_hz * dt;
            if (fm_phase > 2.0 * M_PI) fm_phase = fmod(fm_phase, 2.0 * M_PI);
            double rf = fm_amp * cos(fm_phase);

            if (secam) {
                /* --- native SECAM VHS chroma FM, divided by four --- */
                double d = active_px ? (dr ? -1.902 * dr_y : 1.505 * db_y) : 0.0;
                colour_state = secam_decay * colour_state
                             + (1.0 - secam_decay) * d;
                double pre_d = 3.0 * d - 2.0 * colour_state;
                /* Ident bottle ramp on the composite ident lines (inserted
                 * after LF pre-correction, from the active area start). */
                if (ident && x_us >= c->active_left_us) {
                    double ramp = (x_us - c->active_left_us)
                                / (dr ? 15.0 : 18.0);
                    if (ramp < 0.0) ramp = 0.0;
                    if (ramp > 1.0) ramp = 1.0;
                    pre_d = dr ? 1.25 * ramp : -1.52 * ramp;
                }
                double nominal = dr ? 4406250.0 : 4250000.0;
                double deviation = dr ? 280000.0 : 230000.0;
                double studio_hz = nominal + deviation * pre_d;
                if (studio_hz < 3900000.0) studio_hz = 3900000.0;
                if (studio_hz > 4756250.0) studio_hz = 4756250.0;
                /* Native SECAM: the studio chroma FM divided by four on tape
                 * (rests 1.0625 / 1.1015625 MHz). MESECAM: the PAL
                 * converter's LO (5060571.875 Hz) mixes the SAME studio
                 * chroma down - f_out = LO - f_in lands D'R at
                 * 654321.875 Hz and D'B at 810571.875 Hz with the deviation
                 * inverted, exactly the decoder's MESECAM profile. */
                double chroma_hz = mesecam ? (5060571.875 - studio_hz)
                                           : (studio_hz / 4.0);
                cc_phase += 2.0 * M_PI * chroma_hz * dt;
                if (cc_phase > 2.0 * M_PI) cc_phase = fmod(cc_phase, 2.0 * M_PI);
                /* Carrier envelope: ident + picture lines, 5.6 us through
                 * the active area (the porch carries the rest frequency,
                 * which is the decoder's H identification). */
                if (hidx >= 0 && !in_sync && (ident || picture) &&
                    x_us >= 5.6 && x_us < 62.5) {
                    rf += se_amp * cos(cc_phase);
                }
            } else {
                /* --- PAL/NTSC VHS color-under QAM --- */
                double cu = 0.0, cv = 0.0;
                bool cu_on = false;
                if (hidx >= 0 && !in_sync) {
                    if (x_us >= c->burst_left_us && x_us < burst_end_us) {
                        /* Swinging burst (PAL) / 180-degree burst (NTSC),
                         * down-converted into the color-under band. */
                        cu = cu_burst_u;
                        cv = cu_burst_v;
                        cu_on = true;
                    } else if (active_px) {
                        cu = u;
                        cv = v;
                        cu_on = true;
                    }
                }
                if (cu_on) {
                    double phase = 2.0 * M_PI * carrier * t
                                 + track_rot * (double)hidx + 0.37;
                    double color = cu * cos(phase)
                                 + (pal_family ? -(double)vswitch : 1.0)
                                   * cv * sin(phase);
                    rf += cu_amp * color;
                }
            }

            double value = rf * 32767.0;
            if (value > 32767.0) value = 32767.0;
            if (value < -32768.0) value = -32768.0;
            out[n] = ht_r16(value);
        }
    }

    ht_grid_free(&grid);
    return need;
}
