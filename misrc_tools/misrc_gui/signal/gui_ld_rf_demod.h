#ifndef GUI_LD_RF_DEMOD_H
#define GUI_LD_RF_DEMOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_LD_RF_INPUT_RATE_HZ 20000000u
#define GUI_LD_RF_LUMA_RATE_HZ 5000000u
#define GUI_LD_RF_DECIMATION 4u

typedef enum {
    GUI_LD_RF_PAL,
    GUI_LD_RF_NTSC
} gui_ld_rf_system_t;

typedef struct {
    float b0, b1, b2, a1, a2;
} gui_ld_rf_biquad_t;

enum {
    GUI_LD_RF_COLOR_FIR_HALF = 24,
    GUI_LD_RF_COLOR_HISTORY = 64
};

typedef struct {
    gui_ld_rf_biquad_t filter[3];
    float filter_i[3][2], filter_q[3][2];
    float taps_i[GUI_LD_RF_COLOR_FIR_HALF + 1], taps_q[GUI_LD_RF_COLOR_FIR_HALF + 1];
    float history[2 * GUI_LD_RF_COLOR_HISTORY];
    double oscillator_i, oscillator_q, step_i, step_q, delay;
    float blank_i, blank_q, ire_per_radian, scale_i, scale_q;
    float prev_i, prev_q;
    unsigned cursor, warmup;
    bool have_iq;
} gui_ld_rf_analytic_t;

typedef struct {
    gui_ld_rf_biquad_t luma_filter[3];
    float luma_state[3][2];
    double luma_delay;
    float deemph_b0, deemph_b1, deemph_a1;
    float deemph_input, deemph_output;
    uint64_t next_sample;
    bool have_input;
    gui_ld_rf_system_t system;
    gui_ld_rf_analytic_t analytic;
} gui_ld_rf_demod_t;

/* Select a physical FM calibration and clear all RF/filter history. */
void gui_ld_rf_demod_reset(gui_ld_rf_demod_t *demod, gui_ld_rf_system_t system);

/* Effective DC delay in 5 MSPS luminance samples. */
double gui_ld_rf_demod_luma_delay(const gui_ld_rf_demod_t *demod);

/* Effective DC delay of the chroma envelope in 5 MSPS samples. */
double gui_ld_rf_demod_chroma_delay(const gui_ld_rf_demod_t *demod);

/* Normalized 20 MSPS RF to 5 MSPS luminance in IRE and complex chroma
 * (2 * CVBS IRE / 100 mixed by exp(-i * 2pi * fsc * t)). All three planes
 * share the absolute grid starting at ceil(first/4). Startup emits blanking
 * and a discontinuity clears history. Capacity ceil(count/4) suffices;
 * smaller buffers return zero without consuming RF if the output won't fit. */
size_t gui_ld_rf_demod_process(gui_ld_rf_demod_t *demod,
                             const float *rf, size_t count, uint64_t first,
                             float *luma, float *chroma_i, float *chroma_q,
                             size_t capacity);

#endif
