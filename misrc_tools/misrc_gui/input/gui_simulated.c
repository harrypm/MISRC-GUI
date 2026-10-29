/*
 * MISRC GUI - Simulated Device
 *
 * Provides a simulated capture device for development and testing without hardware.
 * Generates PAL CVBS video on Channel A and VHS RF head signal on Channel B.
 */

//-----------------------------------------------------------------------------
// Simulation Feature Flags
//
// Set to 0 to disable non-ideal behaviors for cleaner test signals
//-----------------------------------------------------------------------------

// CVBS signal impairments
#define SIM_ENABLE_CVBS_NOISE           0   // Random noise on CVBS output

// VHS RF signal impairments
#define SIM_ENABLE_VHS_RF_NOISE         0   // Random noise on VHS RF output
#define SIM_ENABLE_VHS_HEAD_SWITCH      0   // Head switching noise at field boundaries
#define SIM_ENABLE_VHS_TRACKING_NOISE   0   // Low-level tracking jitter

// Analog circuit simulation
#define SIM_ENABLE_SOFT_CLIPPING        0   // Soft saturation near signal limits

//-----------------------------------------------------------------------------

#include "gui_simulated.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../signal/gui_testgen.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

// Define M_PI if not available (Windows compatibility)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//-----------------------------------------------------------------------------
// PAL Timing Constants (in samples at 40 MSPS)
// Using PAL-B/G standard (625 lines, 50 Hz, 4.43361875 MHz subcarrier)
//-----------------------------------------------------------------------------

#define PAL_LINE_DURATION_US     64.0      // One horizontal line (1/15625 Hz)
#define PAL_LINE_SAMPLES         ((int)(PAL_LINE_DURATION_US * 40.0))   // 2560 samples per line
#define PAL_HALF_LINE_SAMPLES    (PAL_LINE_SAMPLES / 2)                 // 1280 samples

// Horizontal timing
#define PAL_HSYNC_US             4.7       // H-sync pulse width
#define PAL_HSYNC_SAMPLES        ((int)(PAL_HSYNC_US * 40.0))           // ~188 samples
#define PAL_BACK_PORCH_US        5.7       // Back porch (includes colorburst)
#define PAL_BACK_PORCH_SAMPLES   ((int)(PAL_BACK_PORCH_US * 40.0))
#define PAL_FRONT_PORCH_US       1.65      // Front porch
#define PAL_FRONT_PORCH_SAMPLES  ((int)(PAL_FRONT_PORCH_US * 40.0))
#define PAL_COLORBURST_CYCLES    10        // Number of colorburst cycles
#define PAL_COLORBURST_FREQ      4433618.75 // 4.43361875 MHz color subcarrier

// Vertical timing (PAL interlaced)
#define PAL_LINES_PER_FRAME      625       // Total lines per frame (both fields)
#define PAL_LINES_PER_FIELD      312       // Lines per field (312.5 lines, alternating)
#define PAL_FRAME_SAMPLES        ((uint64_t)PAL_LINE_SAMPLES * PAL_LINES_PER_FRAME)

// Vertical blanking structure (per field)
#define PAL_VBLANK_PRE_EQ_LINES       2     // Pre-equalizing pulses
#define PAL_VBLANK_VSYNC_LINES        2     // Vertical sync pulses
#define PAL_VBLANK_POST_EQ_LINES      2     // Post-equalizing pulses
#define PAL_VBLANK_BLANK_LINES        17    // Remaining VBI
#define PAL_FIRST_ACTIVE_LINE         23    // First line with active video (0-indexed)

// Equalizing and serration pulse widths
#define PAL_EQ_PULSE_US              2.35   // Equalizing pulse width
#define PAL_EQ_PULSE_SAMPLES         ((int)(PAL_EQ_PULSE_US * 40.0))
#define PAL_SERR_PULSE_US            4.7    // Serration pulse width
#define PAL_SERR_PULSE_SAMPLES       ((int)(PAL_SERR_PULSE_US * 40.0))

// Use PAL constants throughout (aliased for compatibility with existing code)
#define LINE_SAMPLES             PAL_LINE_SAMPLES
#define HALF_LINE_SAMPLES        PAL_HALF_LINE_SAMPLES
#define HSYNC_SAMPLES            PAL_HSYNC_SAMPLES
#define BACK_PORCH_SAMPLES       PAL_BACK_PORCH_SAMPLES
#define FRONT_PORCH_SAMPLES      PAL_FRONT_PORCH_SAMPLES
#define LINES_PER_FRAME          PAL_LINES_PER_FRAME
#define LINES_PER_FIELD          PAL_LINES_PER_FIELD
#define FRAME_SAMPLES            PAL_FRAME_SAMPLES
#define FIRST_ACTIVE_LINE        PAL_FIRST_ACTIVE_LINE
#define EQ_PULSE_SAMPLES         PAL_EQ_PULSE_SAMPLES
#define COLORBURST_FREQ          PAL_COLORBURST_FREQ
#define COLORBURST_CYCLES        PAL_COLORBURST_CYCLES

//-----------------------------------------------------------------------------
// Video Signal Levels
//-----------------------------------------------------------------------------

/* REVERTED to the original values the CVBS decoder was validated against.
 * The hacktv "accurate" values (-0.30/0/0/0.70) broke the decoder's
 * level adaptation (the shallower sync depth and lack of black-blanking
 * separation caused rolling). These values decode correctly; the hacktv
 * correction needs to be applied incrementally after confirming each
 * step still decodes. */
#define SYNC_LEVEL      (-0.4)    // Sync tip
#define BLANKING_LEVEL  (0.0)     // Blanking/black level
#define BLACK_LEVEL     (0.075)   // Setup/pedestal (7.5 IRE)
#define WHITE_LEVEL     (1.0)     // Peak white (100 IRE)

//-----------------------------------------------------------------------------
// VHS RF Parameters
//-----------------------------------------------------------------------------

#define VHS_LUMA_CARRIER_SYNC    3400000.0   // 3.4 MHz at sync tip
#define VHS_LUMA_CARRIER_WHITE   4400000.0   // 4.4 MHz at white
#define VHS_CHROMA_CARRIER       629000.0    // 629 kHz down-converted chroma
#define VHS_FM_DEVIATION         (VHS_LUMA_CARRIER_WHITE - VHS_LUMA_CARRIER_SYNC)

//-----------------------------------------------------------------------------
// Signal Generation State (based on hacktv approach)
//-----------------------------------------------------------------------------

static uint64_t s_sim_sample_count = 0;
static double s_vhs_fm_phase = 0.0;  // FM phase must be accumulated (frequency varies with signal)

// Color carrier lookup table (hacktv approach)
// The table length equals sample_rate / carrier_freq in lowest terms
// For 40MHz / 3.579545MHz ≈ 11.177, but we use integer math
// hacktv uses { 39375000, 11 } = 3579545.4545... Hz exactly
// At 40 MSPS: lookup_width = 40000000 * 11 / 39375000 = 440000000 / 39375000 ≈ 11.175
// To get exact integer cycles, we'd need GCD, but for now use a large enough table
#define COLOR_CARRIER_NUM  39375000   // Numerator (frequency * 11)
#define COLOR_CARRIER_DEN  11         // Denominator
// Actual carrier freq = 39375000 / 11 = 3579545.454545... Hz

// Lookup table: stores cos and sin for each sample position in one color cycle period
// Table width = sample_rate * denominator / numerator = 40000000 * 11 / 39375000
// Simplified: 440000000 / 39375000 = 8800 / 787.5 ≈ 11.175
// We'll compute this properly at init time
static int16_t *s_colour_lookup_i = NULL;  // cos(phase) * 32767
static int16_t *s_colour_lookup_q = NULL;  // sin(phase) * 32767
static int s_colour_lookup_width = 0;
static int s_colour_lookup_offset = 0;

// Simple xorshift PRNG for noise
static uint32_t s_sim_rng_state = 12345;

static uint32_t sim_rand(void) {
    uint32_t x = s_sim_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_sim_rng_state = x;
    return x;
}

// Generate random float in range [-1, 1]
static float sim_noise(void) {
    return ((float)(sim_rand() & 0xFFFF) / 32768.0f) - 1.0f;
}

//-----------------------------------------------------------------------------
// Raw Format Encoding
//-----------------------------------------------------------------------------

// Encode int16_t samples to the raw 32-bit capture format
// This mirrors the decoding done in extract.c
// Raw format:
//   Bits 0-11:  Channel A (12-bit, stored as 2047 - sample)
//   Bits 12-19: AUX data (8 bits, we set to 0)
//   Bits 20-31: Channel B (12-bit, stored as 2047 - sample)
static inline uint32_t encode_raw_sample(int16_t sample_a, int16_t sample_b) {
    // Clamp samples to 12-bit signed range
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    // Encode: store as (2047 - sample) to match extract.c decoding
    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    uint32_t aux = 0;  // No AUX data during simulation

    return ch_a | (aux << 12) | (ch_b << 20);
}

// Initialize the color carrier lookup table for PAL (call once at startup)
static void init_colour_lookup(void) {
    if (s_colour_lookup_i != NULL) return;  // Already initialized

    // PAL carrier: 4.43361875 MHz = 17734475/4 Hz (hacktv uses this rational)
    // At 40 MSPS: samples per cycle = 40000000 / 4433618.75 ≈ 9.022
    // For exact repeat: 40000000 * 4 / 17734475 = 160000000 / 17734475
    // GCD(160000000, 17734475) = 25 -> 6400000 / 709379
    // This doesn't simplify nicely, so use a large table
    // hacktv uses: colour_lookup_width = a.num where a = pixel_rate / carrier
    // For PAL at various sample rates, they compute the exact rational

    // For simplicity, use enough samples to cover many complete cycles
    // 709379 samples would give exact 6400000 cycles, but that's huge
    // Instead use a reasonable size that covers several lines worth
    // At 9.022 samples/cycle, 2560 samples = ~284 cycles (close enough)

    s_colour_lookup_width = 2560;  // One line width for PAL
    int total_size = s_colour_lookup_width + LINE_SAMPLES;

    s_colour_lookup_i = (int16_t *)malloc(total_size * sizeof(int16_t));
    s_colour_lookup_q = (int16_t *)malloc(total_size * sizeof(int16_t));

    if (!s_colour_lookup_i || !s_colour_lookup_q) {
        fprintf(stderr, "[SIM] Failed to allocate color lookup tables\n");
        return;
    }

    // Phase increment per sample = 2*PI * (carrier_freq / sample_rate)
    // PAL: 2*PI * 4433618.75 / 40000000
    double phase_inc = 2.0 * M_PI * PAL_COLORBURST_FREQ / (double)SIM_SAMPLE_RATE;

    for (int i = 0; i < total_size; i++) {
        double phase = phase_inc * i;
        s_colour_lookup_i[i] = (int16_t)round(cos(phase) * 32767.0);
        s_colour_lookup_q[i] = (int16_t)round(sin(phase) * 32767.0);
    }

    s_colour_lookup_offset = 0;

    fprintf(stderr, "[SIM] PAL color carrier lookup table initialized: %d samples (%.1f MHz)\n",
            s_colour_lookup_width, PAL_COLORBURST_FREQ / 1000000.0);
}

// Cleanup color lookup table
static void cleanup_colour_lookup(void) {
    if (s_colour_lookup_i) {
        free(s_colour_lookup_i);
        s_colour_lookup_i = NULL;
    }
    if (s_colour_lookup_q) {
        free(s_colour_lookup_q);
        s_colour_lookup_q = NULL;
    }
    s_colour_lookup_width = 0;
    s_colour_lookup_offset = 0;
}

//-----------------------------------------------------------------------------
// NTSC Line Type Classification
//-----------------------------------------------------------------------------

typedef enum {
    LINE_TYPE_PRE_EQ,       // Pre-equalizing pulses (half-line rate)
    LINE_TYPE_VSYNC,        // Vertical sync with serrations
    LINE_TYPE_POST_EQ,      // Post-equalizing pulses (half-line rate)
    LINE_TYPE_VBLANK,       // Vertical blanking (normal H-sync, black video)
    LINE_TYPE_ACTIVE        // Active video line
} line_type_t;

// Get the line type and field information for a given sample position
static line_type_t sim_get_line_type(uint64_t sample_index, int *out_field, int *out_line_in_field,
                                     int *out_sample_in_line, bool *out_is_half_line) {
    uint64_t sample_in_frame = sample_index % FRAME_SAMPLES;
    int line_in_frame = (int)(sample_in_frame / LINE_SAMPLES);
    int sample_in_line = (int)(sample_in_frame % LINE_SAMPLES);

    int field, line_in_field;
    bool is_half_line = false;

    if (line_in_frame < LINES_PER_FIELD) {
        field = 0;
        line_in_field = line_in_frame;
    } else if (line_in_frame == LINES_PER_FIELD) {
        if (sample_in_line < HALF_LINE_SAMPLES) {
            field = 0;
            line_in_field = 262;
            is_half_line = true;
        } else {
            field = 1;
            line_in_field = 0;
            sample_in_line -= HALF_LINE_SAMPLES;
            is_half_line = true;
        }
    } else {
        field = 1;
        line_in_field = line_in_frame - LINES_PER_FIELD;
    }

    *out_field = field;
    *out_line_in_field = line_in_field;
    *out_sample_in_line = sample_in_line;
    *out_is_half_line = is_half_line;

    if (line_in_field < PAL_VBLANK_PRE_EQ_LINES) {
        return LINE_TYPE_PRE_EQ;
    } else if (line_in_field < PAL_VBLANK_PRE_EQ_LINES + PAL_VBLANK_VSYNC_LINES) {
        return LINE_TYPE_VSYNC;
    } else if (line_in_field < PAL_VBLANK_PRE_EQ_LINES + PAL_VBLANK_VSYNC_LINES + PAL_VBLANK_POST_EQ_LINES) {
        return LINE_TYPE_POST_EQ;
    } else if (line_in_field < FIRST_ACTIVE_LINE) {
        return LINE_TYPE_VBLANK;
    } else {
        return LINE_TYPE_ACTIVE;
    }
}

//-----------------------------------------------------------------------------
// Sync Pulse Generation
//-----------------------------------------------------------------------------

// Generate equalizing pulse signal (2 pulses per line at half-line rate)
static double sim_generate_eq_pulse(int sample_in_line, bool is_half_line) {
    int half_line = HALF_LINE_SAMPLES;

    if (sample_in_line < EQ_PULSE_SAMPLES) {
        return SYNC_LEVEL;
    }
    if (!is_half_line && sample_in_line >= half_line &&
        sample_in_line < half_line + EQ_PULSE_SAMPLES) {
        return SYNC_LEVEL;
    }
    return BLANKING_LEVEL;
}

// Generate vertical sync with serrations
static double sim_generate_vsync_serration(int sample_in_line, bool is_half_line) {
    int serr_start1 = HALF_LINE_SAMPLES - PAL_SERR_PULSE_SAMPLES;
    if (sample_in_line >= serr_start1 && sample_in_line < HALF_LINE_SAMPLES) {
        return BLANKING_LEVEL;
    }
    if (!is_half_line) {
        int serr_start2 = LINE_SAMPLES - PAL_SERR_PULSE_SAMPLES;
        if (sample_in_line >= serr_start2) {
            return BLANKING_LEVEL;
        }
    }
    return SYNC_LEVEL;
}

//-----------------------------------------------------------------------------
// Color Space Conversion
//-----------------------------------------------------------------------------

// RGB to YUV conversion (standard for vhs-decode compatibility)
// Same formula as hacktv: U = (B-Y) * 0.493, V = (R-Y) * 0.877
static inline void rgb_to_yuv(double r, double g, double b,
                              double *y, double *u, double *v) {
    *y = 0.299 * r + 0.587 * g + 0.114 * b;
    *u = 0.493 * (b - *y);  // (B-Y) scaled - same as hacktv eu_co
    *v = 0.877 * (r - *y);  // (R-Y) scaled - same as hacktv ev_co
}

// 75% SMPTE color bars as RGB
// Order: white, yellow, cyan, green, magenta, red, blue, black
static const double bar_rgb[8][3] = {
    {0.75, 0.75, 0.75},  // white
    {0.75, 0.75, 0.00},  // yellow
    {0.00, 0.75, 0.75},  // cyan
    {0.00, 0.75, 0.00},  // green
    {0.75, 0.00, 0.75},  // magenta
    {0.75, 0.00, 0.00},  // red
    {0.00, 0.00, 0.75},  // blue
    {0.00, 0.00, 0.00},  // black
};

//-----------------------------------------------------------------------------
// Test Pattern Generation
//-----------------------------------------------------------------------------

// Test mode: set to 1-7 to output a single solid color bar, 0 for normal bars
#define TEST_SINGLE_COLOR 0  // 0=normal bars, 1=yellow, 2=cyan, 3=green, 4=magenta, 5=red, 6=blue

// Get Y, U, V values for current bar
static void sim_get_bar_yuv(int bar, double *y, double *u, double *v) {
#if TEST_SINGLE_COLOR > 0
    bar = TEST_SINGLE_COLOR;  // Force single color for testing
#else
    bar = bar % 8;
#endif
    rgb_to_yuv(bar_rgb[bar][0], bar_rgb[bar][1], bar_rgb[bar][2], y, u, v);

    // Debug: print YUV values once
    static int printed = 0;
    if (!printed) {
        fprintf(stderr, "[SIM] Color bar %d: Y=%.3f U=%.3f V=%.3f\n", bar, *y, *u, *v);
        printed = 1;
    }
}

static double sim_generate_test_pattern(int pixel_in_line, int field, int active_width) {
    (void)field;
    int bar = (pixel_in_line * 8) / active_width;

    double y, u, v;
    sim_get_bar_yuv(bar, &y, &u, &v);
    (void)u; (void)v;  // Luma only for this function

    return y;
}

//-----------------------------------------------------------------------------
// CVBS Signal Generation (rewritten based on hacktv approach)
//-----------------------------------------------------------------------------

// Track the current line for lookup table offset management
static int s_current_line = -1;
static int s_line_lookup_offset = 0;  // Offset into lookup table for current line

// Called at the start of each line to update the lookup table offset
static void advance_to_line(int line_in_frame) {
    if (line_in_frame != s_current_line) {
        // New line - advance the lookup offset
        if (s_current_line >= 0 && line_in_frame == s_current_line + 1) {
            // Sequential line - advance by line width
            s_line_lookup_offset += LINE_SAMPLES;
            s_line_lookup_offset %= s_colour_lookup_width;
        } else {
            // Non-sequential (frame wrap or init) - compute from line number
            // Each line adds LINE_SAMPLES to the offset
            s_line_lookup_offset = (line_in_frame * LINE_SAMPLES) % s_colour_lookup_width;
        }
        s_current_line = line_in_frame;
    }
}

static double sim_generate_cvbs(uint64_t sample_index, int *out_line_number, int *out_field,
                                 double *out_chroma_u, double *out_chroma_v, double *out_subcarrier_phase) {
    int field, line_in_field, sample_in_line;
    bool is_half_line;

    line_type_t line_type = sim_get_line_type(sample_index, &field, &line_in_field,
                                               &sample_in_line, &is_half_line);

    int line_in_frame = (field == 0) ? line_in_field : (LINES_PER_FIELD + line_in_field);
    if (out_line_number) *out_line_number = line_in_frame;
    if (out_field) *out_field = field;

    // Update lookup table offset for this line
    advance_to_line(line_in_frame);

    // Get carrier lookup values for this sample position
    int lut_idx = s_line_lookup_offset + sample_in_line;
    int16_t carrier_i = s_colour_lookup_i[lut_idx];  // cos(phase)
    int16_t carrier_q = s_colour_lookup_q[lut_idx];  // sin(phase)

    // Output subcarrier phase for VHS (approximate from lookup)
    if (out_subcarrier_phase) {
        *out_subcarrier_phase = atan2((double)carrier_q, (double)carrier_i);
    }

    double signal = 0.0;
    double chroma_u = 0.0;
    double chroma_v = 0.0;

    switch (line_type) {
        case LINE_TYPE_PRE_EQ:
        case LINE_TYPE_POST_EQ:
            signal = sim_generate_eq_pulse(sample_in_line, is_half_line);
            break;

        case LINE_TYPE_VSYNC:
            signal = sim_generate_vsync_serration(sample_in_line, is_half_line);
            break;

        case LINE_TYPE_VBLANK:
            if (sample_in_line < HSYNC_SAMPLES) {
                signal = SYNC_LEVEL;
            } else {
                signal = BLANKING_LEVEL;
            }
            break;

        case LINE_TYPE_ACTIVE: {
            int active_start = HSYNC_SAMPLES + BACK_PORCH_SAMPLES;
            int active_end = LINE_SAMPLES - FRONT_PORCH_SAMPLES;
            int active_width = active_end - active_start;

            if (sample_in_line < HSYNC_SAMPLES) {
                signal = SYNC_LEVEL;
            }
            else if (sample_in_line < active_start) {
                // Back porch - includes color burst
                int back_porch_pos = sample_in_line - HSYNC_SAMPLES;
                int burst_start = (int)(0.6 * 40);   // 0.6µs after hsync end (original working value)
                int burst_duration = (int)(COLORBURST_CYCLES * 40.0 / (COLORBURST_FREQ / 1000000.0));

                if (back_porch_pos >= burst_start && back_porch_pos < burst_start + burst_duration) {
                    double burst_level = 0.15;  // Original working value (about 40 IRE p-p)
                    double burst_i = -0.7071;   /* cos(135) */
                    double burst_q = 0.7071;    /* sin(135) */

                    // PAL burst phase alternation: swing ±45° on alternating lines
                    // On odd lines (when pal_switch = -1), burst swings to 225°
                    int pal_switch = ((line_in_frame) & 1) ? -1 : 1;
                    burst_q *= pal_switch;

                    // Modulation: burst = burst_i * carrier_i + burst_q * carrier_q
                    //           = burst_i * cos(wt) + burst_q * sin(wt)
                    double burst = burst_level * (burst_i * ((double)carrier_i / 32767.0)
                                                + burst_q * ((double)carrier_q / 32767.0));
                    signal = BLANKING_LEVEL + burst;
                } else {
                    signal = BLANKING_LEVEL;
                }
            }
            else if (sample_in_line < active_end) {
                // Active video region
                int pixel = sample_in_line - active_start;
                int bar = (pixel * 8) / active_width;

                // Get Y, U, V from RGB color bars
                double y, u, v;
                sim_get_bar_yuv(bar, &y, &u, &v);

                // Store U/V for VHS generation
                chroma_u = u;
                chroma_v = v;

                // PAL chroma modulation with V-axis switching:
                // Formula: chroma = cos(wt) * V * pal_switch + sin(wt) * U
                // The V component sign alternates on each line (PAL = Phase Alternation Line)
                // This is the key difference from NTSC which doesn't alternate
                int pal_switch = ((line_in_frame) & 1) ? -1 : 1;
                double chroma = ((double)carrier_i / 32767.0) * v * pal_switch
                              + ((double)carrier_q / 32767.0) * u;

                // Scale chroma (hacktv doesn't scale here, but we need to match levels)
                signal = BLACK_LEVEL + y * (WHITE_LEVEL - BLACK_LEVEL) + chroma;
            }
            else {
                signal = BLANKING_LEVEL;
            }
            break;
        }
    }

    // Output U/V components for VHS generation
    if (out_chroma_u) *out_chroma_u = chroma_u;
    if (out_chroma_v) *out_chroma_v = chroma_v;

    return signal;
}

//-----------------------------------------------------------------------------
// VHS RF Signal Generation
//-----------------------------------------------------------------------------

// VHS color-under: generate 629 kHz QAM chroma signal from U/V components

static double sim_generate_vhs_rf(double cvbs_luma, double chroma_u, double chroma_v,
                                   int line_in_frame, int field, int sample_in_line) {
    (void)field;

    // === FM Luminance ===
    // VHS FM-encodes the luminance signal
    double normalized = (cvbs_luma - SYNC_LEVEL) / (WHITE_LEVEL - SYNC_LEVEL);
    if (normalized < 0) normalized = 0;
    if (normalized > 1) normalized = 1;

    double fm_freq = VHS_LUMA_CARRIER_SYNC + normalized * VHS_FM_DEVIATION;

    // FM phase must be accumulated since frequency varies with signal
    double fm_signal = sin(s_vhs_fm_phase) * 0.7;
    s_vhs_fm_phase += 2.0 * M_PI * fm_freq / SIM_SAMPLE_RATE;
    if (s_vhs_fm_phase > 2.0 * M_PI) s_vhs_fm_phase -= 2.0 * M_PI;

    // === VHS Color-Under (629 kHz QAM) ===
    // VHS color-under is a quadrature amplitude modulated signal at 629 kHz.
    // Per-line phase for 629 kHz carrier
    double vhs_cycles_in_line = VHS_CHROMA_CARRIER * (double)sample_in_line / (double)SIM_SAMPLE_RATE;
    double vhs_phase = 2.0 * M_PI * vhs_cycles_in_line;

    // VHS 90° phase rotation on odd lines for crosstalk cancellation
    double vhs_line_rotation = (line_in_frame % 2) ? (M_PI / 2.0) : 0.0;
    vhs_phase += vhs_line_rotation;

    // Generate 629 kHz QAM chroma: cos(wt)*V + sin(wt)*U
    // Note: PAL V-axis switching was already applied in the CVBS generation
    double chroma_under = cos(vhs_phase) * chroma_v + sin(vhs_phase) * chroma_u;

    // Scale down VHS chroma (color-under level is lower than NTSC broadcast)
    chroma_under *= 0.5;

    double head_noise = 0.0;
#if SIM_ENABLE_VHS_HEAD_SWITCH
    int line_in_field = (field == 0) ? line_in_frame : (line_in_frame - LINES_PER_FIELD);
    if (line_in_field <= 6) {
        double switch_intensity = 1.0 - (line_in_field / 6.0);
        head_noise = sim_noise() * 0.4 * switch_intensity;

        if (line_in_field <= 2 && (sim_rand() % 100) < 5) {
            head_noise += (sim_noise() > 0 ? 0.5 : -0.5);
        }
    }
#endif

    double tracking_noise = 0.0;
#if SIM_ENABLE_VHS_TRACKING_NOISE
    tracking_noise = sim_noise() * 0.02 * (field == 0 ? 1.0 : 1.1);
#endif

    return fm_signal + chroma_under + head_noise + tracking_noise;
}

//-----------------------------------------------------------------------------
// Simulated Capture Thread
//-----------------------------------------------------------------------------

/* Channel A: hacktv-accurate CVBS stream. One full interlaced frame is
 * pre-generated per system (PAL/NTSC/SECAM) and streamed continuously -
 * a live capture signal through the full pipeline (waveform, FFT, Video
 * panel decode, recording), not a static snapshot. The system follows
 * app->settings.video_system (0=PAL, 1=NTSC, 2=SECAM); a change detected
 * in the capture loop regenerates the frame and restarts the stream from
 * its beginning. */
static int simulated_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;

    int16_t *buf_a = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));
    int16_t *buf_b = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));

    if (!buf_a || !buf_b) {
        fprintf(stderr, "[SIM] Failed to allocate buffers\n");
        free(buf_a);
        free(buf_b);
        return -1;
    }

    /* Pre-generated hacktv-accurate CVBS frame (channel A) */
    int sim_system = -1;   /* force first generation */
    int16_t *cvbs_frame = NULL;
    size_t cvbs_frame_samples = 0;
    uint64_t cvbs_frame_pos = 0;   /* streaming position within the frame */

    fprintf(stderr, "[SIM] Simulated capture thread started at %d MSPS (writing to BUF_CAPTURE_RF)\n", SIM_SAMPLE_RATE / 1000000);
    fprintf(stderr, "[SIM] Channel A: hacktv-accurate CVBS (video_system setting)\n");
    fprintf(stderr, "[SIM] Channel B: VHS RF head signal (FM luma + 629kHz chroma)\n");

#if SIM_ENABLE_CVBS_NOISE
    double cvbs_noise = 0.02;
#endif
#if SIM_ENABLE_VHS_RF_NOISE
    double rf_noise = 0.03;
#endif

    s_sim_rng_state = (uint32_t)get_time_ms();

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);

    uint64_t batch_count = 0;

    while (atomic_load(&app->sim_running)) {
        /* System (PAL/NTSC/SECAM) follows the app's video_system setting;
         * detect a change and regenerate the frame. */
        int want_system = app->settings.video_system;
        if (want_system < 0 || want_system >= GUI_TESTGEN_SYSTEM_COUNT) {
            want_system = GUI_TESTGEN_PAL;
        }
        if (want_system != sim_system) {
            sim_system = want_system;
            size_t need = gui_testgen_frame_samples(
                (gui_testgen_system_t)sim_system, SIM_SAMPLE_RATE);
            if (need > 0) {
                /* TWO pre-generated frames, streamed alternately: SECAM's
                 * D'r/D'b line cadence and ident type alternate every frame
                 * (hacktv global-line parity), so a single repeated frame
                 * would hold one cadence and break field identification. */
                int16_t *grown = (int16_t *)realloc(cvbs_frame,
                                                    2 * need * sizeof(int16_t));
                if (grown) {
                    cvbs_frame = grown;
                    cvbs_frame_samples = 2 * need;
                    gui_testgen_frame(
                        (gui_testgen_system_t)sim_system, cvbs_frame, need,
                        SIM_SAMPLE_RATE, 0);
                    gui_testgen_frame(
                        (gui_testgen_system_t)sim_system, cvbs_frame + need,
                        need, SIM_SAMPLE_RATE, 1);
                    cvbs_frame_pos = 0;
                    fprintf(stderr, "[SIM] Channel A: %s hacktv frames "
                            "(2x %zu samples at %u MSPS)\n",
                            gui_testgen_system_name((gui_testgen_system_t)sim_system),
                            need, SIM_SAMPLE_RATE / 1000000);
                }
            }
        }

        for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
            /* Channel A: stream from the hacktv plugin frame (continuous
             * live signal through the full capture pipeline). The frame is
             * hacktv's int16 output; scale to the 12-bit capture domain.
             * hacktv PAL: sync -0.30*32767=-9830, white 0.70*32767=22937.
             * 12-bit domain: -2048..2047. Scale by 2048/32767 ≈ 0.0625. */
            double cvbs;
            if (cvbs_frame && cvbs_frame_samples > 0) {
                int16_t raw = cvbs_frame[cvbs_frame_pos % cvbs_frame_samples];
                cvbs = (double)raw / 32767.0;
                cvbs_frame_pos++;
            } else {
                cvbs = 0.0;
            }

            /* Channel B: VHS RF from the old per-sample generator. */
            int line_in_frame = 0;
            int field = 0;
            double chroma_u = 0.0;
            double chroma_v = 0.0;
            {
                int line_in_field;
                int sample_in_line_vhs;
                bool is_half;
                sim_get_line_type(s_sim_sample_count, &field, &line_in_field,
                                  &sample_in_line_vhs, &is_half);
                line_in_frame = (field == 0) ? line_in_field
                               : (LINES_PER_FIELD + line_in_field);
            }
            /* Derive chroma from the bar for VHS (same layout as hacktv) */
            int sample_in_line = (int)(s_sim_sample_count % LINE_SAMPLES);
            if (sample_in_line > HSYNC_SAMPLES + BACK_PORCH_SAMPLES &&
                sample_in_line < LINE_SAMPLES - FRONT_PORCH_SAMPLES) {
                int active_start = HSYNC_SAMPLES + BACK_PORCH_SAMPLES;
                int active_end = LINE_SAMPLES - FRONT_PORCH_SAMPLES;
                int active_width = active_end - active_start;
                int pixel = sample_in_line - active_start;
                int bar = (pixel * 8) / active_width;
                if (bar > 7) bar = 7;
                if (bar < 0) bar = 0;
                double y;
                sim_get_bar_yuv(bar, &y, &chroma_u, &chroma_v);
            }
            double vhs_rf = sim_generate_vhs_rf(cvbs, chroma_u, chroma_v,
                                                line_in_frame, field,
                                                sample_in_line);

            /* Channel A (CVBS): 2170 scale — measured from the real EBU
             * HackDac reference FLAC (per-bar plateau analysis of the
             * decoded s16 data, converted to the 12-bit tool domain):
             *   real sync tip    = -10500 int16 = -41 8-bit = -656 12-bit
 *                      -> reads -0.32
             *   real blanking    = ~0 int16                -> reads 0.00
             *   real white bar   = +24300 int16 = +95 8-bit = +1519 12-bit
             *                      -> reads +0.74
             *   real chroma peak = +32512 int16 = +127 8-bit (8-bit MAX —
             *                      the yellow/cyan composite peaks CLIP)
 *                      -> reads +0.99
             * buf_a = hacktv_value * 2170:
             *   sync  -0.30*2170 = -651  (reads -0.32, real -0.32)
             *   blank  0.00*2170 =    0  (reads  0.00, real  0.00)
             *   white  0.70*2170 = 1519  (reads +0.74, real +0.74)
             *   yellow 0.934*2170 = 2027 (reads +0.99, real clips +0.99)
             * Channel B (VHS RF): ORIGINAL 1024 scale — DO NOT CHANGE. */
            buf_a[i] = (int16_t)(cvbs * 2170.0);
            buf_b[i] = (int16_t)(vhs_rf * 1024.0);

            s_sim_sample_count++;
        }

        // Encode generated samples to raw 32-bit format and write to BUF_CAPTURE_RF
        // The extraction thread will read this, update statistics, display, CVBS, and recording
        size_t raw_bytes = SIM_BUFFER_SIZE * sizeof(uint32_t);
        uint32_t *raw_buf = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                                           raw_bytes, NULL);
        if (raw_buf) {
            // Encode int16 samples to raw format
            for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
                raw_buf[i] = encode_raw_sample(buf_a[i], buf_b[i]);
            }
            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, raw_bytes);
        } else {
            // Buffer full - this shouldn't happen often with proper sizing
            fprintf(stderr, "[SIM] Warning: BUF_CAPTURE_RF full, frame dropped\n");
        }

        // Update timestamp (extraction thread updates other statistics)
        atomic_store(&app->last_callback_time_ms, get_time_ms());

        batch_count++;
        thrd_sleep_ms(SIM_UPDATE_INTERVAL_MS);
    }

    fprintf(stderr, "[SIM] Simulated capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(buf_a);
    free(buf_b);
    free(cvbs_frame);

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_simulated_start(gui_app_t *app) {
    fprintf(stderr, "[SIM] Starting simulated capture\n");
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    // Reset statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // Reset display buffers
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Initialize color carrier lookup table
    init_colour_lookup();

    // Reset signal generation state
    s_sim_sample_count = 0;
    s_vhs_fm_phase = 0.0;
    s_current_line = -1;
    s_line_lookup_offset = 0;

    // Start thread
    atomic_store(&app->sim_running, true);
    app->is_capturing = true;

    // Start extraction thread - reads from BUF_CAPTURE_RF, writes to BUF_DISPLAY
    // Also handles statistics, peak detection, and recording if enabled
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[SIM] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        atomic_store(&app->sim_running, false);
        app->is_capturing = false;
        return -1;
    }

    // Start display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[SIM] Failed to start display thread (non-fatal)\n");
            // Non-fatal - display will use legacy path
        }
    }

    // Start simulated capture thread - generates samples and writes to BUF_CAPTURE_RF
    thrd_t thread;
    if (thrd_create(&thread, simulated_capture_thread, app) != thrd_success) {
        fprintf(stderr, "[SIM] Failed to create simulated capture thread\n");
        gui_extract_stop();
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        atomic_store(&app->sim_running, false);
        app->is_capturing = false;
        return -1;
    }
    app->sim_thread = (void *)(uintptr_t)thread;

    gui_app_set_status(app, "Simulated capture running");

    return 0;
}

void gui_simulated_stop(gui_app_t *app) {
    if (!atomic_load(&app->sim_running)) return;

    fprintf(stderr, "[SIM] Stopping simulated capture\n");

    // Set is_capturing to false BEFORE stopping extraction thread
    // The extraction thread checks this flag to know when to exit
    app->is_capturing = false;

    atomic_store(&app->sim_running, false);

    // Stop simulated capture thread first (it writes to BUF_CAPTURE_RF)
    if (app->sim_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->sim_thread;
        thrd_join(thread, NULL);
        app->sim_thread = NULL;
    }

    // Stop display thread (reads from BUF_DISPLAY written by extraction)
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop extraction thread (reads BUF_CAPTURE_RF, writes BUF_DISPLAY)
    gui_extract_stop();

    atomic_store(&app->stream_synced, false);

    gui_app_set_status(app, "Simulated capture stopped");
}

bool gui_simulated_is_running(gui_app_t *app) {
    return atomic_load(&app->sim_running);
}
