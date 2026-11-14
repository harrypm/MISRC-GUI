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

/* --- hacktv vid_config structs (verbatim values) ------------------------- */

typedef struct {
    int lines;                  /* 625 / 525 */
    double active_width_us;     /* 51.95 / 52.90 */
    double active_left_us;      /* 10.40 / 9.20 */
    double hsync_us;            /* 4.70 */
    double vsync_short_us;      /* 2.35 / 2.30 (eq pulse) */
    double vsync_long_us;       /* 27.30 / 27.10 (broad pulse) */
    double sync_rise_us;        /* 0.20 / 0.25 */
    double white_level;         /* 0.70 / 100/140 */
    double black_level;         /* 0.00 / 7.5/140 */
    double blanking_level;      /* 0.00 */
    double sync_level;          /* -0.30 / -40/140 */
    int colour_mode;            /* PAL / NTSC / SECAM */
    double burst_width_us;      /* 2.25 / 2.50 */
    double burst_rise_us;       /* 0.30 */
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
    /* PAL — hacktv vid_config_pal (sync_rise 0.32 = the measured real-chain
     * edge: the EBU reference's blanking->white 10-90 is 0.19us, i.e. a
     * raised-cosine parameter of 0.32).
     * Field 1 picture starts at slot 43, field 2 at slot 667 (288 lines). */
    { 625, 51.95, 10.40, 4.70, 2.35, 27.30, 0.32,
      0.70, 0.00, 0.00, -0.30,
      HT_PAL, 2.25, 0.30, 5.60, 3.0/14.0, 4433618.75, 0.877, 0.493,
      1250, 5, { 43, 667 }, 288 },
    /* NTSC — hacktv vid_config_ntsc (sync_rise per the same real chain).
     * active_left 9.45 = the decoder's NTSC profile's active_start: the
     * back-porch window (burst_end..active_start) must NOT overlap the
     * picture - with hacktv's 9.20 the porch sampler lands inside the
     * castellated edge strip and poisons the level commit.
     * Field 1 picture starts at slot 38, field 2 at slot 564 (242 lines). */
    { 525, 52.90, 9.45, 4.70, 2.30, 27.10, 0.32,
      100.0/140, 7.5/140, 0.00, -40.0/140,
      HT_NTSC, 2.50, 0.30, 5.30, 0.20, 3579545.4545, 0.877, 0.493,
      1050, 6, { 38, 564 }, 242 },
    /* SECAM — hacktv vid_config_secam (PAL luma geometry + FM chroma).
     * ev_co/eu_co are the SECAM D deviations in Hz per colour-difference
     * unit (hacktv's SECAM configs REPLACE the PAL 0.877/0.493 with them):
     * ev_co = -1.902*280e3 (D'R), eu_co = +1.505*230e3 (D'B). */
    { 625, 51.95, 10.40, 4.70, 2.35, 27.30, 0.32,
      0.70, 0.00, 0.00, -0.30,
      HT_SECAM, 2.25, 0.30, 5.60, 0.0, 4433618.75,
      -1.902 * 280e3, 1.505 * 230e3,
      1250, 5, { 43, 667 }, 288 },
};

/* Capture-chain chroma gain: the real EBU HackDAC reference measures its
 * bar chroma AND burst uniformly at 0.75x the standard amplitudes (yellow
 * chroma 0.176 composite vs the standard 0.235; burst 0.221 p-p vs 0.3 -
 * a 0.075-span uniform factor, direct max/min probe of the raw file).
 * The simulated device emulates a live capture through the same chain. */
#define HT_CHROMA_GAIN 0.75

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
/* Chroma D deviations per unit of colour difference (hacktv ev_co/eu_co
 * for SECAM: ev_co = -1.902*280e3 D'R, eu_co = +1.505*230e3 D'B) */
#define HT_SECAM_DR_HZ  (-1.902 * 280e3)
#define HT_SECAM_DB_HZ  (1.505 * 230e3)

/* --- hacktv _burstwin (raised-cosine envelope) --------------------------- */

static double ht_burstwin(double t, double width, double rise) {
    /* t in seconds from burst start; width/rise in seconds */
    if (t < 0 || t > width + rise) return 0.0;
    double w;
    if (t < rise / 2) w = 0.0;
    else if (t < width) w = 1.0;
    else if (t < width + rise / 2) w = (width + rise / 2 - t) / rise;
    else w = 0.0;
    w = fmax(0.0, fmin(1.0, w));
    return 0.5 - 0.5 * cos(M_PI * w);
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

static void ht_fir_low_pass(double *taps, int ntaps, double sample_rate,
                            double cutoff, double gain) {
    int M = (ntaps - 1) / 2;
    double fwT0 = 2.0 * M_PI * cutoff / sample_rate;
    ht_kaiser(taps, ntaps, 7.0);
    for (int n = -M; n <= M; n++) {
        if (n == 0) taps[n + M] *= fwT0 / M_PI;
        else taps[n + M] *= sin(n * fwT0) / (n * M_PI);
    }
    double fmax = taps[M];
    for (int n = 1; n <= M; n++) fmax += 2 * taps[n + M];
    gain /= fmax;
    for (int n = 0; n < ntaps; n++) taps[n] *= gain;
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

/* hacktv _secam_g: complex cloche (bell) gain of the chroma sub-carrier at
 * f Hz: 0.115 * (1 + j*16u) / (1 + j*1.26u), u = f/f0 - f0/f. */
static void ht_secam_g(double f, double *g_re, double *g_im) {
    double u = f / HT_SECAM_F0 - HT_SECAM_F0 / f;
    double lq = 16.0 * u;
    double rq = 1.26 * u;
    double d = 1.0 + rq * rq;
    *g_re = 0.115 * (1.0 + lq * rq) / d;
    *g_im = 0.115 * (lq - rq) / d;
}

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

static void ht_smpte_px(const ht_config_t *c, int row, int rows, int ax, int width,
                        double *y, double *u, double *v) {
    const double span = c->white_level - c->black_level;        /* 92.5 IRE */
    const double chroma_span = span / (c->white_level - c->blanking_level);
    double pos = 7.0 * (double)ax / (double)width;               /* in bar widths */
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
    int burst_left_smp = (int)(c->burst_left_us * (double)sample_rate / 1e6 + 0.5);
    double burst_rise_s = c->burst_rise_us * 1e-6;
    double burst_width_s = c->burst_width_us * 1e-6;
    double burst_amp = c->burst_level_frac * (white16 - blank16) * HT_CHROMA_GAIN;
    if (burst_amp < 0) burst_amp = -burst_amp;

    /* hacktv vid_init: colour carrier phase increment per sample */
    double fsc_inc = 2.0 * M_PI * c->fsc_hz / (double)sample_rate;

/* PASS 1: baseband luma/sync into base_buf, chroma into chroma_buf.
     * hacktv applies the sync_rise shaping only to the sync/level steps and
     * keeps chroma on its own path; a combined filter would attenuate the
     * 4.43 MHz subcarrier. */
    for (int s = 0; s < slots_total; s++) {
        size_t ss = slot_start[s];
        size_t se = slot_start[s + 1];
        if (ss >= need) break;

        switch (slot_type[s]) {
            case SL_EQ:
                for (size_t n = ss; n < se && n < need; n++) {
                    if (n - ss < (size_t)eq_smp) {
                        base_buf[n] = (int16_t)lrint(sync16 * 32767.0);
                    }
                }
                break;

            case SL_BROAD:
                for (size_t n = ss; n < se && n < need; n++) {
                    if (n - ss < (size_t)broad_smp) {
                        base_buf[n] = (int16_t)lrint(sync16 * 32767.0);
                    }
                }
                break;

            case SL_BLANK:
                break;  /* blanking level (buffers are zeroed) */

            case SL_H: {
                /* One full line: this slot plus the next (the active
                 * picture spans ~52us of the 64us line). */
                size_t ss2 = (s + 2 <= slots_total) ? slot_start[s + 2] : need;
                int hidx = slot_hidx[s];
                int field = (s < mid) ? 0 : 1;
                int a0 = c->act_first[field];
                int a1 = a0 + 2 * (c->act_lines - 1);
                bool active = (s >= a0 && s <= a1);
                /* PAL V-switch / SECAM line parity (even H index -> +V) */
                int pal_switch = ((hidx & 1) == 0) ? 1 : -1;
                if (c->colour_mode == HT_NTSC) pal_switch = 1;

                for (size_t n = ss; n < ss2 && n < need; n++) {
                    size_t x = n - ss;
                    size_t abs_pos = n;

                    double signal = blank16;
                    double chroma = 0.0;

                    /* H sync (hacktv _render_sync_pulses H LUT) */
                    if (x < (size_t)hsync_smp) signal = sync16;

                    if (c->colour_mode == HT_PAL || c->colour_mode == HT_NTSC) {
                        /* Burst on every H line (hacktv burst window) */
                        if (x >= (size_t)burst_left_smp) {
                            double bt = (double)(x - (size_t)burst_left_smp)
                                      / (double)sample_rate;
                            double w = ht_burstwin(bt, burst_width_s, burst_rise_s);
                            if (w > 0.0) {
                                double phase = fsc_inc * (double)abs_pos;
                                if (c->colour_mode == HT_NTSC) {
                                    /* NTSC burst: 180 degrees (hacktv: -sin) */
                                    chroma += -w * burst_amp * sin(phase);
                                } else {
                                    /* PAL burst: +-135 degrees with V-switch */
                                    double ang = (pal_switch > 0)
                                        ? 135.0 * M_PI / 180.0
                                        : 225.0 * M_PI / 180.0;
                                    chroma += w * burst_amp * sin(phase + ang);
                                }
                            }
                        }
                    }

                    /* Active video (hacktv _vid_next_line_raster) */
                    if (active) {
                        int ax = (int)x - active_left;
                        if (ax >= 0 && ax < active_width) {
                            double y, u, v;
                            if (c->colour_mode == HT_NTSC) {
                                /* NTSC: SMPTE SD colour bars */
                                ht_smpte_px(c, (s - a0) / 2, c->act_lines,
                                            ax, active_width, &y, &u, &v);
                            } else {
                                int bar = (ax * 8) / active_width;
                                if (bar > 7) bar = 7;
                                if (bar < 0) bar = 0;
                                ht_rgb_to_yuv(c, HT_BARS[bar].r, HT_BARS[bar].g,
                                              HT_BARS[bar].b, &y, &u, &v);
                            }

                            /* hacktv yuv_level_lookup: y in black..white */
                            signal = black16 + y * (white16 - black16);

                            if (c->colour_mode == HT_PAL ||
                                c->colour_mode == HT_NTSC) {
                                /* QAM on the subcarrier: U rides sin,
                                 * V rides cos with the PAL V-switch. */
                                double u_scaled = u * (white16 - blank16)
                                                * HT_CHROMA_GAIN;
                                double v_scaled = v * (white16 - blank16)
                                                * HT_CHROMA_GAIN;
                                double phase = fsc_inc * (double)abs_pos;
                                chroma += u_scaled * sin(phase) +
                                          v_scaled * pal_switch * cos(phase);
                            }
                        }
                    }

                double base_v = signal * 32767.0;
                    if (base_v > 32767.0) base_v = 32767.0;
                    if (base_v < -32768.0) base_v = -32768.0;
                    base_buf[n] = (int16_t)lrint(base_v);

                    double chroma_v16 = chroma * 32767.0;
                    if (chroma_v16 > 32767.0) chroma_v16 = 32767.0;
                    if (chroma_v16 < -32768.0) chroma_v16 = -32768.0;
                    chroma_buf[n] = (int16_t)lrint(chroma_v16);
                }
                break;
            }
        }
    }

    /* PASS 1.5 (SECAM only): the full hacktv _vid_render_secam FM chain.
     * Per H line: build the D control signal (D'b rest 4.25 MHz / D'r rest
     * 4.40625 MHz around the 4.328125 MHz FM centre, INT16_MAX = +-1 MHz),
     * vertically averaged with the previous line's opposite component
     * (hacktv chrominance_buffer second half); the first 9 H lines of each
     * field carry the field ident ramp instead (D'r +350 kHz over 15 us,
     * D'b -350 kHz over 18 us, from active_left). Then: 15-tap kaiser
     * low-pass at 1.7 MHz, the SECAM video pre-emphasis horn (85/255 kHz,
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
        const double fm_level = (white16 - blank16) * 32767.0 * HT_CHROMA_GAIN;
        const size_t chroma_left = (size_t)hsync_smp;
        const size_t chroma_len = (size_t)((HT_SECAM_WIN_US * 1e-6
                                + HT_SECAM_RISE_US * HT_SECAM_IRT1090 * 1e-6)
                                * (double)sample_rate) + 1;
        const double env_width = HT_SECAM_WIN_US * 1e-6;
        const double env_rise = HT_SECAM_RISE_US * HT_SECAM_IRT1090 * 1e-6;
        double *d_line = (double *)malloc((size_t)(line_smp + 8) * sizeof(double));
        double *d_src = (double *)malloc((size_t)(line_smp + 8) * sizeof(double));
        double lp[15];
        double pe_b0, pe_b1, pe_fb;

        if (!d_line || !d_src) {
            free(d_line); free(d_src);
            ht_grid_free(&grid);
            free(base_buf); free(chroma_buf);
            return 0;
        }
        ht_fir_low_pass(lp, 15, (double)sample_rate, 1.70e6, 1.0);
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
                    double t = (double)((ssize_t)x - active_left)
                               / (double)sample_rate / rw;
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
                    if (x < (size_t)active_left ||
                        x >= (size_t)active_left + (size_t)active_width) {
                        d_line[x] = rest;
                        continue;
                    }
                    int ax = (int)x - active_left;
                    int bar = (ax * 8) / active_width;
                    if (bar > 7) bar = 7;
                    if (bar < 0) bar = 0;
                    double y, u, v;
                    ht_rgb_to_yuv(c, HT_BARS[bar].r, HT_BARS[bar].g,
                                  HT_BARS[bar].b, &y, &u, &v);
                    /* u/v are the SECAM deviations in Hz (the SECAM
                     * config's ev_co/eu_co); the chain gain scales the
                     * deviation (0.75x, measured on the real card). */
                    double own_hz = HT_CHROMA_GAIN * (dr ? v : u);
                    d_line[x] = (own_hz + (dr ? HT_SECAM_DR : HT_SECAM_DB)
                                 - HT_SECAM_FM) / HT_SECAM_DEV;
                }
            }

            /* 15-tap kaiser low-pass, block mode (edges held) */
            memcpy(d_src, d_line, len * sizeof(double));
            for (size_t x = 0; x < len; x++) {
                double acc = 0.0;
                for (int j = -7; j <= 7; j++) {
                    ssize_t idx = (ssize_t)x + j;
                    if (idx < 0) idx = 0;
                    if ((size_t)idx >= len) idx = (ssize_t)len - 1;
                    acc += lp[j + 7] * d_src[idx];
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

            /* FM render with the cloche gain and envelope */
            phase = ((global % 3) == 0) ? 0.0 : M_PI;
            for (size_t x = chroma_left; x < len; x++) {
                size_t w = x - chroma_left;
                double env, freq, g_re, g_im, c;
                if (w >= chroma_len) break;
                env = ht_rc_window((double)w / (double)sample_rate,
                                   env_rise / 2.0, env_width, env_rise);
                freq = HT_SECAM_FM + d_line[x] * HT_SECAM_DEV;
                ht_secam_g(freq, &g_re, &g_im);
                c = fm_level * (cos(phase) * g_re - sin(phase) * g_im) * env;
                if (c > 32767.0) c = 32767.0;
                if (c < -32768.0) c = -32768.0;
                chroma_buf[ss + x] = (int16_t)lrint(c);
                phase += 2.0 * M_PI * freq / (double)sample_rate;
            }
        }
        free(d_line);
        free(d_src);
    }

    /* PASS 2: apply the sync_rise raised-cosine shaping to the BASE signal
     * only (hacktv's IRT1090 rise shape). The chroma is added unfiltered
     * afterwards. */
    int rise_n = (int)(c->sync_rise_us * (double)sample_rate / 1e6 + 0.5);
    if (rise_n < 1) rise_n = 1;
    if (rise_n > (int)need) rise_n = (int)need;
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
            /* Raised-cosine pulse (hacktv IRT1090 rise shape) */
            kern[i] = 0.5 - 0.5 * cos(M_PI * ((double)i + 0.5) / (double)rise_n);
            sum += kern[i];
        }
        for (int i = 0; i < rise_n; i++) kern[i] /= sum;
    }
    int kern_half = rise_n / 2;
    for (size_t n = 0; n < need; n++) {
        double base_f = 0.0;
        for (int i = 0; i < rise_n; i++) {
            size_t idx = (size_t)((ssize_t)n - kern_half + i);
            if (idx >= need) {
                /* Frame wrap: the stream is continuous, so fold the
                 * kernel tail onto the frame start (and vice versa). */
                idx %= need;
            }
            base_f += kern[i] * (double)base_buf[idx];
        }
        double value = base_f;
        if (value > 32767.0) value = 32767.0;
        if (value < -32768.0) value = -32768.0;
        out[n] = (int16_t)lrint(value);
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
                out[n] = (int16_t)lrint(acc);
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
        out[n] = (int16_t)lrint(value);
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
        for (size_t n = 0; n < need; n++) {
            double line = (double)n * lines_per_frame / (double)need;
            double d2 = line - half;
            double b1 = amp * (1.0 - exp(-line / HT_SAG_TAU_RISE))
                            * exp(-fmax(0.0, line - HT_SAG_PIC_START)
                                      / HT_SAG_TAU_DECAY);
            double b2 = 0.0;
            if (d2 >= 0.0) {
                b2 = amp * (1.0 - exp(-d2 / HT_SAG_TAU_RISE))
                         * exp(-fmax(0.0, d2 - HT_SAG_PIC_START)
                                   / HT_SAG_TAU_DECAY);
            }
            double value = (double)out[n] + b1 + b2;
            if (value > 32767.0) value = 32767.0;
            if (value < -32768.0) value = -32768.0;
            out[n] = (int16_t)lrint(value);
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
            out[n] = (int16_t)lrint(value);
        }
    }

    ht_grid_free(&grid);
    return need;
}
