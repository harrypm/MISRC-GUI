/*
 * MISRC GUI - Composite Waveform Monitor (accumulator + levels)
 *
 * Port of tbc-tools' tbc-analyse waveform monitor (itself from decode-orc,
 * GPL-3.0-or-later): every line of a frame is binned into a count raster -
 * one column per sample of the line's time window, one row per amplitude
 * bin - and the panel draws the raster with an area-max brightness knee.
 *
 * Fed by the CVBS decoder (gui_cvbs.c, decoder thread): every locked line
 * arrives with its sub-sample H-sync origin, so each column is one fixed
 * time after 0H (the sync leading edge's 50% point) on every line, exactly
 * like tbc's time-base-corrected 4fsc samples.
 *
 * Levels: tbc reads its levels from the TBC metadata (sync-derived by the
 * decoder that wrote it). A live capture has no calibrated volts, so the
 * monitor uses the same reference a waveform monitor's DC restorer plus a
 * standard-sync calibration does:
 *   - each line is clamped at its own back porch (blanking = 0 IRE; the
 *     decoder's DC restore windows), so an AC-coupled baseline sag after the
 *     VBI does not smear the trace;
 *   - the gain comes from the measured sync height (tip -> blanking, the
 *     field mean over picture lines): PAL/SECAM 300 mV (42.86 IRE), NTSC
 *     40 IRE. A source with a compressed sync therefore reads high.
 * The display range, level markers and Y-only filter follow tbc exactly.
 */
#ifndef GUI_WFM_H
#define GUI_WFM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Raster: columns = samples across the window at the panel decode rate
// (PAL 64 us x 17.9 MHz = 1146), bins across the display range (PAL Y+C
// 190 IRE -> 0.37 IRE / 2.6 mV per bin; tbc uses 1 mV and area-maxes down
// to the widget, which this matches on panels up to 512 px tall).
#define GUI_WFM_COLS 1152
#define GUI_WFM_BINS 512

enum { GUI_WFM_SYS_PAL = 0, GUI_WFM_SYS_NTSC = 1, GUI_WFM_SYS_SECAM = 2 };
// tbc's Range: the active picture of picture lines, or every line whole.
enum { GUI_WFM_RANGE_ACTIVE = 0, GUI_WFM_RANGE_FRAME = 1 };
// Channel: tbc's Y+C (composite) and Y (4-tap average), plus C - the chroma
// alone, a hardware monitor's CHROMA filter: the fs/4 band-pass
// [-1 0 2 0 -1]/4 applied twice (sin^4 response at the ~4x fsc decode
// rate: unity at fsc, zero at DC and fs/2, luma -38 dB at 1 MHz; SECAM's
// 3.9-4.76 MHz carrier within 4%), displayed around 0 IRE.
enum { GUI_WFM_CH_YC = 0, GUI_WFM_CH_Y = 1, GUI_WFM_CH_C = 2, GUI_WFM_CH_COUNT };
// C display envelope (IRE around blanking): 100% bars reach +-63 IRE.
#define GUI_WFM_C_RANGE_IRE 70.0

// Column resampler: 8-tap windowed-sinc fractional delay, 64 phases (a
// 1/128-sample timing step: 0.4 ns at the PAL decode rate).
#define GUI_WFM_FD_TAPS 8
#define GUI_WFM_FD_PHASES 64

// Whole-frame window: 1 us of front porch before 0H to 1 us before the
// next line's 0H (sync, burst and the full active line in view).
#define GUI_WFM_FRAME_LEAD_US 1.0

// Normative levels in IRE (tbc's 10-bit CVBS constants: PAL tip 4, blank
// 256, white 844, peak 1019; NTSC tip 16, blank 240, black 282, white 800,
// peak 1019), with tbc's mV scale (PAL 700 mV, NTSC 714.3 mV per 100 IRE).
typedef struct {
    double sync_tip, blanking, black, white, peak;
    double mv_per_ire;
} gui_wfm_levels_t;

void gui_wfm_levels(int system, gui_wfm_levels_t *lv);
// tbc's display envelope (IRE): Y+C = sync tip - 5% .. peak + 5% of their
// span; Y only = -100 mV .. white + 100 mV; C = +-GUI_WFM_C_RANGE_IRE.
void gui_wfm_display_range(int system, int channel, double *lo_ire, double *hi_ire);
// The burst's peak amplitude (IRE) for the C markers; 0 = none (SECAM).
double gui_wfm_burst_ire(int system);

// What one published frame raster holds.
typedef struct {
    int system;            // GUI_WFM_SYS_*
    int range;             // GUI_WFM_RANGE_*
    int channel;           // GUI_WFM_CH_*
    int cols;              // Columns used (<= GUI_WFM_COLS)
    double t0_us;          // Column 0 time, us after 0H
    double us_per_col;
    double lo_ire, hi_ire; // Bin 0 bottom edge .. bin GUI_WFM_BINS top edge
    uint32_t lines;        // Lines binned
    double rate;           // Decode rate the raster was built at (Hz)
} gui_wfm_meta_t;

// One decoded line (filled by the CVBS decoder).
typedef struct {
    const int16_t *ring;   // The decoder's raw-sample ring (decode-rate samples)
    unsigned ring_mask;    // Ring length - 1 (a power of two)
    int64_t ring0;         // Ring index of the line buffer's first sample
    double off0h;          // Line-buffer position (samples) of 0H
    double rate;           // Decode rate (Hz)
    double line_us, burst_end_us, active_start_us, active_end_us;
    double swing_nom;      // Decoder's tip-to-blank estimate (ring units):
                           // the gain until the monitor's own is measured
    int system;            // GUI_WFM_SYS_*
    bool picture;          // A picture line (full H sync, active content)
} gui_wfm_line_t;

typedef struct gui_wfm {
    // Rasters, row-major by amplitude bin: [bin * GUI_WFM_COLS + col]
    // (a line's increments at one level walk contiguous memory).
    // Latest-wins hand-off (harrypm 2026-11-19, WFM pause fix): the decoder
    // fills acc and swaps it with the mailbox (mid); the renderer swaps the
    // mailbox with pub and copies pub. Only the pointer swaps sit under
    // swap_lock, so neither side waits on the other's work, and the newest
    // frame - the one at a paused playhead - is never dropped.
    uint16_t *acc;         // Decoder thread: the frame being accumulated
    uint16_t *mid;         // Mailbox: the newest published frame
    uint16_t *pub;         // Render thread: the frame being copied out
    gui_wfm_meta_t acc_meta, mid_meta, pub_meta;
    atomic_flag swap_lock;
    atomic_int pub_ready;  // 1 = the mailbox holds a frame the renderer has not taken
    atomic_uint frames;    // Frames published (diagnostics / tests)

    // UI requests, applied at the next frame start.
    atomic_int want_range;
    atomic_int want_channel;   // GUI_WFM_CH_*

    // Decoder thread.
    int fields_in_acc;
    bool acc_started;
    double swing_ref;      // Measured tip-to-blank (ring units), per field
    bool swing_valid;
    double swing_sum;
    int swing_n;
    double blank;          // Last accepted back-porch level (the clamp)
    bool have_blank;
    double dither;         // Per-line column offset sequence (0..1)
    int16_t raw[GUI_WFM_COLS + 16];
    float rawf[GUI_WFM_COLS + 16];
    float val[GUI_WFM_COLS + 16];
    // Fractional-delay taps per sub-sample phase (gui_wfm_init).
    float fd_taps[GUI_WFM_FD_PHASES + 1][GUI_WFM_FD_TAPS];
} gui_wfm_t;

bool gui_wfm_init(gui_wfm_t *w);
void gui_wfm_free(gui_wfm_t *w);
// Decoder thread: drop the frame in progress and the measured references
// (a system switch).
void gui_wfm_reset(gui_wfm_t *w);

// Decoder thread.
void gui_wfm_add_line(gui_wfm_t *w, const gui_wfm_line_t *line);
// Unlocked: publish every second field, whatever they are.
void gui_wfm_end_field(gui_wfm_t *w);
// Field / frame lock (harrypm 2026-11-19): `parity` is the ending field's,
// `field1_parity` ITU field 1's (the Spectro's f1). single_field publishes
// every field alone; otherwise a frame is field 1 + field 2 of the SAME
// frame (an accumulation that starts on field 2 is discarded).
void gui_wfm_end_field_locked(gui_wfm_t *w, int parity, int field1_parity, bool single_field);

// Render thread: copy the latest published frame into dst (GUI_WFM_COLS *
// GUI_WFM_BINS counts, row-major; meta->cols columns used). False when
// nothing new waits.
bool gui_wfm_take(gui_wfm_t *w, uint16_t *dst, gui_wfm_meta_t *meta);

#endif // GUI_WFM_H
