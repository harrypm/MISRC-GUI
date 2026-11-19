/*
 * MISRC GUI - Field locator (vertical-sync field boundaries in a sample run)
 *
 * Finds the field starts in a contiguous run of samples, so the Spectro
 * panel's paused window can key its field / frame markers to the signal
 * (harrypm 2026-11-19: "frame/field locked with the decoder mode that is
 * active"). The channel's Video panel mode picks the input:
 *   - CVBS (baseband composite): the samples, sync low;
 *   - FM (tape / disc RF: RF Preview): the instantaneous carrier frequency
 *     from interpolated zero crossings - every FM video format puts the
 *     sync tip at the LOWEST carrier frequency - so sync is low again.
 * Then one slicer for both: the vertical interval's broad pulses (a low run
 * longer than a quarter line) make a train; the field boundary is the
 * train's first broad pulse (625 lines: the start of line 1 / 313; 525
 * lines: 3 lines after the field start - pre-equalising - so 3 lines are
 * stepped back). The parity is the boundary's phase against the line grid
 * before it: field 1 starts on a line start, field 2 half a line off.
 *
 * Streaming: push the run's samples in order (any block sizes), then
 * finish. The trace is kept decimated (4 points a microsecond), so a
 * 25-frame window at 40 MS/s holds ~4 M floats.
 */
#ifndef GUI_FIELD_LOCATE_H
#define GUI_FIELD_LOCATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GUI_FIELD_LOCATE_CVBS = 1, GUI_FIELD_LOCATE_FM = 2 };

typedef struct {
    uint64_t pos;      // Absolute sample of the field start
    int field;         // 1 or 2 (0: parity not resolved)
} gui_field_mark_t;

typedef struct {
    int mode;                  // GUI_FIELD_LOCATE_*
    double rate;               // Samples per second
    int lines_per_frame;       // 625 or 525
    uint64_t start;            // Absolute sample of the first pushed sample
    int decim;                 // Samples per trace point
    // Trace (decimated): one value per decim samples
    float *trace;
    size_t trace_len, trace_cap;
    // Decimation accumulator
    double acc;
    int acc_n;
    uint64_t pushed;
    // FM: zero-crossing frequency estimate
    double dc;                 // Slow DC tracker
    double prev_x;
    double last_cross;         // Sample time of the last crossing (relative to start)
    bool have_prev, have_cross;
    double f_acc;
    int f_n;
    float last_f;
} gui_field_locate_t;

// Prepare a run: mode, rate, 625 / 525, the absolute position of the first
// sample, and the expected sample count (sizes the trace; may be exceeded).
bool gui_field_locate_begin(gui_field_locate_t *fl, int mode, double rate, int lines_per_frame,
                            uint64_t start, uint64_t expected);
// The run's next samples (int32 as libFLAC delivers them).
void gui_field_locate_push(gui_field_locate_t *fl, const int32_t *x, size_t n);
// Field starts found in the run, in order (at most max). Frees nothing.
size_t gui_field_locate_finish(gui_field_locate_t *fl, gui_field_mark_t *marks, size_t max);
void gui_field_locate_free(gui_field_locate_t *fl);

#endif // GUI_FIELD_LOCATE_H
