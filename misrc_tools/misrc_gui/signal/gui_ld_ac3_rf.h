/* MISRC GUI LaserDisc AC-3 RF preview. GPL-3.0-or-later. */
#ifndef GUI_LD_AC3_RF_H
#define GUI_LD_AC3_RF_H

#include <stddef.h>
#include <stdint.h>

typedef struct gui_ld_ac3_rf gui_ld_ac3_rf_t;
typedef void (*gui_ld_ac3_rf_symbol_t)(void *context, uint8_t symbol,
                                      uint64_t end_sample);

gui_ld_ac3_rf_t *gui_ld_ac3_rf_create(void);
void gui_ld_ac3_rf_destroy(gui_ld_ac3_rf_t *decoder);
void gui_ld_ac3_rf_reset(gui_ld_ac3_rf_t *decoder);

/* Normalized 20 MSPS RF to differential QPSK symbols in the range 0–3.
 * end_sample estimates the exclusive position on the 20 MSPS RF grid.
 * An input discontinuity resets the demodulator's filters and symbol clock.
 * Callers must also reset downstream framing when changing sources or seeking. */
void gui_ld_ac3_rf_process(gui_ld_ac3_rf_t *decoder, const float *samples,
                           size_t count, uint64_t first_sample,
                           gui_ld_ac3_rf_symbol_t emit, void *context);
#endif
