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

// AUTO (harrypm 2026-11-19: "should be easy to detect ... rather than format
// targeted dependency"): trace the run both ways (FM carrier frequency and
// CVBS level) and keep the one whose field starts repeat at a field period.
// With lines_per_frame 0 the system comes from the measured spacing of the
// vertical-sync trains (20 ms -> 625, 16.68 ms -> 525): the broad-pulse
// slicer's thresholds hardly differ between the two (64 vs 63.56 us lines).
enum { GUI_FIELD_LOCATE_CVBS = 1, GUI_FIELD_LOCATE_FM = 2, GUI_FIELD_LOCATE_AUTO = 3 };
#define GUI_FIELD_LOCATE_MAX_HS 32

typedef struct {
    uint64_t pos;      // Absolute sample of the field start
    int field;         // 1 or 2 (0: parity not resolved)
} gui_field_mark_t;

typedef struct gui_field_locate {
    int mode;                  // GUI_FIELD_LOCATE_* (AUTO runs FM here + CVBS in alt)
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
    // AUTO: the CVBS twin (kept across runs like the trace), auto system.
    struct gui_field_locate *alt;
    bool auto_mode, auto_lines;
    // finish(): what the marks were found with.
    int found_mode;            // GUI_FIELD_LOCATE_CVBS / _FM (0 = none)
    int found_lines;           // 625 / 525 measured (head switches / train spacing),
                               // else the caller's hint, else 0 (unknown)
    bool found_measured;       // found_lines was measured (not the hint)
    // FM: RF envelope (mean |x - dc| per trace point) and the head switches
    // found in it (harrypm 2026-11-19: "headswitch peaks are an absolute
    // irrespective of format"). The envelope steps where the heads hand
    // over; folding the step measure over a frame period stacks the switch
    // phase and averages picture content out, and only the right system
    // puts the two heads exactly half a frame apart - so the head switches
    // also give the system with nothing preset (measured on the Daimler NTSC
    // VHS RF: 9-16x / 6-10x the mean, 16.680 ms apart; at the PAL period the
    // second head fell to ~3x, 19-20 ms off).
    float *env;
    size_t env_cap;
    double env_acc;
    int env_n;
    uint64_t hs_pos[GUI_FIELD_LOCATE_MAX_HS];   // Absolute samples, in order
    int hs_head[GUI_FIELD_LOCATE_MAX_HS];       // 0 / 1: which of the two switch phases
    size_t hs_n;
    int hs_lines;              // System the head switches measured (0 = none)
} gui_field_locate_t;

// Prepare a run: mode (AUTO: both), rate, 625 / 525 (0: measured), the
// absolute position of the first sample, and the expected sample count
// (sizes the trace; may be exceeded).
bool gui_field_locate_begin(gui_field_locate_t *fl, int mode, double rate, int lines_per_frame,
                            uint64_t start, uint64_t expected);
// The run's next samples (int32 as libFLAC delivers them).
void gui_field_locate_push(gui_field_locate_t *fl, const int32_t *x, size_t n);
// Field starts found in the run, in order (at most max). Frees nothing.
size_t gui_field_locate_finish(gui_field_locate_t *fl, gui_field_mark_t *marks, size_t max);
void gui_field_locate_free(gui_field_locate_t *fl);

#endif // GUI_FIELD_LOCATE_H
