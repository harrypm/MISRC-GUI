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
 *   av_test.c bars           -> 75% colour bar pattern
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
    /* SECAM — hacktv vid_config_secam (PAL luma geometry + FM chroma) */
    { 625, 51.95, 10.40, 4.70, 2.35, 27.30, 0.32,
      0.70, 0.00, 0.00, -0.30,
      HT_SECAM, 2.25, 0.30, 5.60, 0.0, 4433618.75, 0.877, 0.493,
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

/* SECAM FM centres (hacktv SECAM_* defines) */
#define HT_SECAM_DB 4250000.0
#define HT_SECAM_DR 4406250.0

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

/* --- Public API ----------------------------------------------------------- */

size_t gui_testgen_frame_samples(gui_testgen_system_t system, uint32_t sample_rate) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT || sample_rate == 0) {
        return 0;
    }
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
        default: return "PAL";
    }
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

size_t gui_testgen_frame(gui_testgen_system_t system, int16_t *out, size_t cap,
                         uint32_t sample_rate) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT) return 0;
    if (!out || sample_rate == 0) return 0;
    const ht_config_t *c = &HT_CONF[system];
    size_t need = gui_testgen_frame_samples(system, sample_rate);
    if (cap < need) return 0;

    const int slots_total = c->slots;
    const int train_len = 3 * c->eq_count;      /* EQ + broad + EQ, every slot */
    const int mid = slots_total / 2;            /* 625 / 525: field 2 boundary */
    /* H-grid slot parity. PAL's first EQ must sit at a HALF-line offset from
     * the last H (parity 0) -> H grid on ODD slots; NTSC's first EQ must sit
     * at an INTEGER offset (parity 0) -> H grid on EVEN slots. */
    const int hpar = (c->lines == 525) ? 0 : 1;
    if (need < (size_t)slots_total) return 0;

    /* Slot boundaries spread over the frame so every slot is need/slots +-1
     * samples: the line period stays exact at any rate and the frame has no
     * short last line (a half-line stream jump every frame otherwise). */
    size_t *slot_start = (size_t *)malloc((size_t)(slots_total + 1) * sizeof(size_t));
    int16_t *base_buf = (int16_t *)calloc(need, sizeof(int16_t));
    int16_t *chroma_buf = (int16_t *)calloc(need, sizeof(int16_t));
    uint8_t *slot_type = (uint8_t *)malloc((size_t)slots_total);
    int *slot_hidx = (int *)malloc((size_t)slots_total * sizeof(int));
    if (!slot_start || !base_buf || !chroma_buf || !slot_type || !slot_hidx) {
        free(slot_start);
        free(base_buf);
        free(chroma_buf);
        free(slot_type);
        free(slot_hidx);
        return 0;
    }
    for (int s = 0; s <= slots_total; s++) {
        double t = (double)need * (double)s / (double)slots_total;
        slot_start[s] = (size_t)(t + 0.5);
    }
    slot_start[0] = 0;
    slot_start[slots_total] = need;

    /* Per-slot pulse classification and per-field H-line index. The H index
     * (reset at each field boundary) drives the PAL V-switch and SECAM line
     * parity, so the line sequence matches the verified colour phase. */
    {
        int h1 = 0, h2 = 0;
        for (int s = 0; s < slots_total; s++) {
            bool in_train = (s < train_len) || (s >= mid && s < mid + train_len);
            if (in_train) {
                int k = (s < train_len) ? s : s - mid;
                slot_type[s] = (uint8_t)((k < c->eq_count) ? SL_EQ
                             : (k < 2 * c->eq_count) ? SL_BROAD : SL_EQ);
                slot_hidx[s] = -1;
            } else if ((s & 1) == hpar) {
                slot_type[s] = SL_H;
                slot_hidx[s] = (s < mid) ? h1++ : h2++;
            } else {
                slot_type[s] = SL_BLANK;
                slot_hidx[s] = -1;
            }
        }
    }

    /* hacktv vid_init: active area in samples (per 64us line) */
    int active_left = (int)(c->active_left_us * (double)sample_rate / 1e6 + 0.5);
    int active_width = (int)ceil(c->active_width_us * (double)sample_rate / 1e6);
    /* An H line spans two slots; clamp the active window inside it. */
    int line_smp = 2 * (int)(slot_start[1] - slot_start[0]);
    if (line_smp > 0 && active_left + active_width > line_smp) {
        active_width = line_smp - active_left;
    }

    /* hacktv vid_init: 16-bit signal levels */
    double level = 1.0;
    double white16 = c->white_level * level;
    double black16 = c->black_level * level;
    double blank16 = c->blanking_level * level;
    double sync16 = c->sync_level * level;

    /* hacktv vid_init: sync pulse sample counts */
    int hsync_smp = (int)(c->hsync_us * (double)sample_rate / 1e6 + 0.5);
    int eq_smp = (int)(c->vsync_short_us * (double)sample_rate / 1e6 + 0.5);
    int broad_smp = (int)(c->vsync_long_us * (double)sample_rate / 1e6 + 0.5);

    /* hacktv vid_init: burst parameters (plus the chain gain) */
    int burst_left_smp = (int)(c->burst_left_us * (double)sample_rate / 1e6 + 0.5);
    double burst_rise_s = c->burst_rise_us * 1e-6;
    double burst_width_s = c->burst_width_us * 1e-6;
    double burst_amp = c->burst_level_frac * (white16 - blank16) * HT_CHROMA_GAIN;
    if (burst_amp < 0) burst_amp = -burst_amp;

    /* hacktv vid_init: colour carrier phase increment per sample */
    double fsc_inc = 2.0 * M_PI * c->fsc_hz / (double)sample_rate;

    /* SECAM FM phase accumulator (continuous across the frame) */
    double secam_phase = 0.0;

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
                            int bar = (ax * 8) / active_width;
                            if (bar > 7) bar = 7;
                            if (bar < 0) bar = 0;

                            double y, u, v;
                            ht_rgb_to_yuv(c, HT_BARS[bar].r, HT_BARS[bar].g,
                                          HT_BARS[bar].b, &y, &u, &v);

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
                            } else {
                                /* SECAM FM, matching the decoder's verified
                                 * demod convention (the tbc-tools port, user-
                                 * verified on the real SECAM capture):
                                 *   D'B = +1.505*(B-Y), 230 kHz per unit
                                 *   D'R = -1.902*(R-Y), 280 kHz per unit
                                 * In the 0..1 U/V domain (u=0.493*(B-Y),
                                 * v=0.877*(R-Y)): D'B = +3.052*u,
                                 * D'R = -2.168*v. Amplitude: the ITU 230 mV
                                 * p-p through the chain gain. */
                                bool dr = (pal_switch > 0);
                                double span = white16 - blank16;
                                double d = dr ? -2.168 * v : 3.052 * u;
                                double centre = dr ? HT_SECAM_DR : HT_SECAM_DB;
                                double dev = dr ? 280e3 : 230e3;
                                double freq = centre + dev * d;
                                secam_phase += 2.0 * M_PI * freq
                                             / (double)sample_rate;
                                chroma += 0.1643 * span * HT_CHROMA_GAIN
                                         * sin(secam_phase);
                            }
                            }
                        } else if (c->colour_mode == HT_SECAM &&
                                   x >= (size_t)hsync_smp) {
                            /* Porch (post-sync, pre-picture): centre
                             * frequency. The subcarrier is BLANKED during
                             * the sync itself - it never rides the tip. */
                        bool dr = (pal_switch > 0);
                        double centre = dr ? HT_SECAM_DR : HT_SECAM_DB;
                        secam_phase += 2.0 * M_PI * centre
                                     / (double)sample_rate;
                        chroma += 0.1643 * (white16 - blank16)
                                 * HT_CHROMA_GAIN * sin(secam_phase);
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

    free(slot_type);
    free(slot_hidx);
    free(slot_start);

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
        double value = base_f + (double)chroma_buf[n];
        if (value > 32767.0) value = 32767.0;
        if (value < -32768.0) value = -32768.0;
        out[n] = (int16_t)lrint(value);
    }
    free(kern);
    free(base_buf);
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
