/* MISRC GUI LaserDisc EFM clock recovery. GPL-3.0-or-later. */
#include "gui_ld_efm_frontend.h"
#include "gui_ld_rf_demod.h"

#include <string.h>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define LD_EFM_SSE2 1
#else
#define LD_EFM_SSE2 0
#endif

enum { EFM_TAPS = 65, EFM_FILTER_DELAY = EFM_TAPS / 2 };
static const double efm_bit_period = (double)GUI_LD_RF_LUMA_RATE_HZ / 4321800.0;

/* Causal 20 MHz equalizer, oldest input first. The 65-tap Kaiser (beta 4)
 * truncation fits the LaserDisc EFM amplitude and phase response used by
 * ld-decode, including its 20 kHz to 1.6 MHz passband. Decimation follows
 * the dot product, where the equalized signal is safely below 2.5 MHz. */
static const float efm_taps[EFM_TAPS] = {
    -6.335466169e-04f, -6.916901330e-04f, -5.203393521e-04f, -7.923515659e-05f,
    +5.781544023e-04f, +1.291638007e-03f, +1.818808378e-03f, +1.899248222e-03f,
    +1.346722478e-03f, +1.444577792e-04f, -1.489141141e-03f, -3.095503664e-03f,
    -4.050614778e-03f, -3.717340995e-03f, -1.648219652e-03f, +2.210301813e-03f,
    +7.372598629e-03f, +1.278365124e-02f, +1.693847962e-02f, +1.815073937e-02f,
    +1.492922660e-02f, +6.385276560e-03f, -7.426201832e-03f, -2.532657608e-02f,
    -4.506113380e-02f, -6.359361857e-02f, -7.760918885e-02f, -8.413406461e-02f,
    -8.114506304e-02f, -6.803493947e-02f, -4.582433775e-02f, -1.706241444e-02f,
    +1.457451656e-02f, +4.491036385e-02f, +6.999603659e-02f, +8.678711951e-02f,
    +9.363586456e-02f, +9.050320834e-02f, +7.885968685e-02f, +6.131131947e-02f,
    +4.104364663e-02f, +2.120951377e-02f, +4.388922825e-03f, -7.777170278e-03f,
    -1.472442131e-02f, -1.687450521e-02f, -1.536636893e-02f, -1.169197448e-02f,
    -7.328980602e-03f, -3.449882846e-03f, -7.574107149e-04f, +5.412430619e-04f,
    +6.433381350e-04f, -1.168846393e-05f, -9.215328610e-04f, -1.669721678e-03f,
    -2.013462828e-03f, -1.902378863e-03f, -1.439764164e-03f, -8.108714828e-04f,
    -2.063210559e-04f, +2.362474770e-04f, +4.596651415e-04f, +4.832080158e-04f,
    +3.760180261e-04f,
};

static float ld_efm_equalize(const float *history) {
#if LD_EFM_SSE2
    __m128 sum = _mm_setzero_ps();
    for (unsigned i = 0; i < EFM_TAPS - 1; i += 4) {
        __m128 values = _mm_loadu_ps(history + i);
        __m128 taps = _mm_loadu_ps(efm_taps + i);
        sum = _mm_add_ps(sum, _mm_mul_ps(values, taps));
    }
    sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(1, 1, 1, 1)));
    return _mm_cvtss_f32(sum) + history[EFM_TAPS - 1] * efm_taps[EFM_TAPS - 1];
#else
    float sum = 0.0f;
    for (unsigned i = 0; i < EFM_TAPS; ++i) sum += history[i] * efm_taps[i];
    return sum;
#endif
}

static void ld_efm_reset_clock(gui_ld_efm_frontend_t *frontend) {
    frontend->period = efm_bit_period;
    frontend->clock = 0.0;
    frontend->phase = 0.0;
    frontend->run = 1;
    frontend->trend = 0;
}

void gui_ld_efm_frontend_reset(gui_ld_efm_frontend_t *frontend) {
    memset(frontend, 0, sizeof(*frontend));
    ld_efm_reset_clock(frontend);
}

static void ld_efm_edge(gui_ld_efm_frontend_t *frontend, double elapsed, uint64_t sample,
                        gui_ld_efm_tvalue_t emit, void *context) {
    /* Interpolated zero crossings drive a clock which can follow disc wow.
     * Two consecutive phase errors in one direction adjust its frequency. */
    double period = frontend->period;
    double clock = frontend->clock;
    double phase = frontend->phase;
    unsigned run = frontend->run;
    int trend = frontend->trend;
    const double period_step = efm_bit_period * .0001;
    while (elapsed >= clock) {
        clock += period + phase;
        if ((elapsed > clock || run < 3) && run < 11) {
            phase = 0.0;
            ++run;
            continue;
        }
        double error = elapsed - (clock - period * .5);
        phase = error * .005;
        if (error < 0.0) trend = trend < 0 ? -2 : -1;
        else if (error > 0.0) trend = trend > 0 ? 2 : 1;
        else trend = 0;
        if (trend == -2 || trend == 2) {
            double adjust = period_step * error / period;
            if (adjust < -period_step) adjust = -period_step;
            if (adjust > period_step) adjust = period_step;
            period += adjust;
            if (period < efm_bit_period * .90) period = efm_bit_period * .90;
            if (period > efm_bit_period * 1.10) period = efm_bit_period * 1.10;
        }
        emit(context, run, sample);
        run = 1;
    }
    frontend->period = period;
    frontend->clock = clock - elapsed;
    frontend->phase = phase;
    frontend->run = run;
    frontend->trend = trend;
}

void gui_ld_efm_frontend_process(gui_ld_efm_frontend_t *frontend, const float *rf,
                                 size_t count, uint64_t first_sample,
                                 gui_ld_efm_tvalue_t emit, void *context) {
    if (!count) return;
    if (!frontend->have_input || frontend->next_sample != first_sample)
        gui_ld_efm_frontend_reset(frontend);
    frontend->have_input = true;
    frontend->next_sample = first_sample + count;
    for (size_t i = 0; i < count; ++i) {
        unsigned cursor = frontend->cursor;
        frontend->history[cursor] = rf[i];
        frontend->history[cursor + GUI_LD_EFM_HISTORY] = rf[i];
        frontend->cursor = (cursor + 1u) & (GUI_LD_EFM_HISTORY - 1u);
        if (frontend->filled < EFM_TAPS) ++frontend->filled;
        uint64_t sample = first_sample + i;
        if ((sample & (GUI_LD_RF_DECIMATION - 1u)) != 0 || frontend->filled < EFM_TAPS)
            continue;
        unsigned oldest = (cursor + GUI_LD_EFM_HISTORY + 1u - EFM_TAPS) & (GUI_LD_EFM_HISTORY - 1u);
        float current = ld_efm_equalize(frontend->history + oldest);
        float previous = frontend->previous;
        frontend->previous = current;
        if ((previous < 0.0f) != (current < 0.0f)) {
            double fraction = -(double)previous / ((double)current - previous);
            uint64_t correction = EFM_FILTER_DELAY + (uint64_t)((1.0 - fraction) * GUI_LD_RF_DECIMATION + .5);
            if (frontend->have_crossing)
                ld_efm_edge(frontend, frontend->elapsed + fraction, sample - correction, emit, context);
            frontend->have_crossing = true;
            frontend->elapsed = 1.0 - fraction;
        } else if (frontend->have_crossing) {
            frontend->elapsed += 1.0;
            /* Bound the work when the next crossing arrives after silence. */
            if (frontend->elapsed >= 128.0 * efm_bit_period) {
                frontend->have_crossing = false;
                ld_efm_reset_clock(frontend);
                emit(context, 0, sample - EFM_FILTER_DELAY);
            }
        }
    }
}
