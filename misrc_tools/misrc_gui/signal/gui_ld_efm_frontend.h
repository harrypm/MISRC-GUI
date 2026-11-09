/* MISRC GUI LaserDisc EFM clock recovery. GPL-3.0-or-later. */
#ifndef GUI_LD_EFM_FRONTEND_H
#define GUI_LD_EFM_FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GUI_LD_EFM_HISTORY = 128 };

typedef struct {
    float history[2 * GUI_LD_EFM_HISTORY];
    double elapsed, period, clock, phase;
    uint64_t next_sample;
    float previous;
    unsigned cursor, filled, run;
    int trend;
    bool have_input, have_crossing;
} gui_ld_efm_frontend_t;

/* A run length in [3, 11], or zero when signal transitions stop.
 * The position estimates the RF transition on the caller's 20 MSPS axis. */
typedef void (*gui_ld_efm_tvalue_t)(void *context, unsigned t_value, uint64_t rf_edge_sample);

void gui_ld_efm_frontend_reset(gui_ld_efm_frontend_t *frontend);

/* Equalize normalized 20 MSPS RF, decimate to 5 MSPS and recover EFM runs.
 * Input discontinuities clear the frontend state. The caller resets the
 * symbol/PCM decoder at the same discontinuity. */
void gui_ld_efm_frontend_process(gui_ld_efm_frontend_t *frontend, const float *rf,
                                 size_t count, uint64_t first_sample,
                                 gui_ld_efm_tvalue_t emit, void *context);

#endif
