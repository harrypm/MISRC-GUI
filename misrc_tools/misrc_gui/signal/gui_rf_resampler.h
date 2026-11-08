#ifndef GUI_RF_RESAMPLER_H
#define GUI_RF_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gui_rf_resampler gui_rf_resampler_t;

/* Converts real, signed ADC PCM to normalized float RF at 20 MSPS.
 * Filter history and fractional sample position persist across calls. */
gui_rf_resampler_t *gui_rf_resampler_create(uint32_t input_rate_hz);
/* Explicit target rate: 20 or 40 MSPS. The compatibility API above retains
 * its original 20 MSPS response and fast paths. */
gui_rf_resampler_t *gui_rf_resampler_create_rate(uint32_t input_rate_hz,
                                               uint32_t output_rate_hz);
/* ED Beta / S-VHS preview: 20 MSPS with a 9.5 MHz passband. Keeps ordinary video and
 * Hi-Fi converter response unchanged. Non-native rates require libsoxr. */
gui_rf_resampler_t *gui_rf_resampler_create_ed_beta(uint32_t input_rate_hz);
size_t gui_rf_resampler_output_capacity_ed_beta(uint32_t input_rate_hz, size_t input_count);
void gui_rf_resampler_destroy(gui_rf_resampler_t *resampler);
void gui_rf_resampler_reset(gui_rf_resampler_t *resampler);
size_t gui_rf_resampler_output_capacity(uint32_t input_rate_hz, size_t input_count);
size_t gui_rf_resampler_output_capacity_rate(uint32_t input_rate_hz,
                                            uint32_t output_rate_hz, size_t input_count);
bool gui_rf_resampler_process(gui_rf_resampler_t *resampler,
                              const int16_t *input, size_t input_count,
                              float *output, size_t output_capacity,
                              size_t *output_count);
/* Signal alignment delay in the selected output-rate samples. This excludes buffered
 * availability latency, which the worker measures from its output cursor. */
double gui_rf_resampler_delay_samples(const gui_rf_resampler_t *resampler);

#endif
