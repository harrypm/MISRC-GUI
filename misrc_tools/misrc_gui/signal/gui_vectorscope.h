/*
 * MISRC GUI - Composite Vectorscope (accumulator + calibration)
 *
 * A COMPOSITE vectorscope: the U/V points come from demodulating the
 * composite signal's chroma against its own colour burst (the CVBS
 * decoder's burst-locked demod), exactly what a hardware composite
 * vectorscope does - not from a component (YUV/RGB) picture.
 *
 * The decoder adds one point per sample of every burst-carrying line's window
 * (every 2nd sample with the beam trace on - decode-orc's sample stride),
 * plus the beam transits between consecutive points (decoder thread). At
 * each field end the hit and transit grids, with the field's bounding box,
 * are handed to the render thread (lock-free double buffer: published only
 * when the renderer has taken the previous one; gui_vectorscope_take_swap
 * trades buffers instead of copying). Grids hold counts only inside their
 * field's box, so clears touch those rows / cells only.
 *
 * Plot units: the decoder's burst-normalised chroma, exactly what the Video
 * panel decodes (its ACC: the burst sets the gain, so capture-chain chroma
 * loss is calibrated out like on a burst-referenced scope). One unit = the
 * (white - blank) span for 625-line systems and 100 IRE for NTSC, so 75%
 * bar targets land at the BT.601 U/V vector times 0.75 (PAL) and times
 * 0.75 x 0.925 (NTSC, chroma on the 92.5 IRE setup span).
 *
 * Known decoder calibration (measured with gui_vectorscope_test on the Test
 * Gen bars): NTSC reads +1%. PAL reads +2%: the decoder's PAL burst window
 * reads the burst ~11% low, mostly cancelled by its 0.2 burst constant
 * (PAL's true burst is 3/14 of the span). The plot deliberately shows the
 * decoder's output, not a separately corrected value.
 *
 * The graticule geometry follows tbc-tools' tbc-analyse vectorscope
 * (target boxes + zones from the BT.601 matrix, 2-degree ticks), with the
 * NTSC I/Q axes at their standard angles (I 123 deg, Q 33 deg).
 */
#ifndef GUI_VECTORSCOPE_H
#define GUI_VECTORSCOPE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Density grid edge (square): fine enough that a 1024 px panel stays sharp.
#define GUI_VSCOPE_GRID 512
#define GUI_VSCOPE_GRID_SHIFT 9   // log2(GUI_VSCOPE_GRID): cell = y << SHIFT | x
// Plot units at the grid edge (the outer circle radius). 0.87 matches the
// tbc-analyse scope's full-scale radius; 100% red/cyan (0.63) stay inside.
#define GUI_VSCOPE_FULL_SCALE 0.87

// System of the published field (same numbering as cvbs_format_t).
enum {
    GUI_VSCOPE_SYS_UNKNOWN = 0,
    GUI_VSCOPE_SYS_PAL = 1,
    GUI_VSCOPE_SYS_NTSC = 2,
    GUI_VSCOPE_SYS_SECAM = 3,
};

// Acquisition mode (tbc-tools tbc-analyse's Basic / Advanced tabs, the
// Advanced one ported from decode-orc's composite measurement scope):
// - STANDARD: the decoder's view - each line burst-locked on its own and the
//   PAL V-switch undone, so bars land on one set of targets.
// - ADVANCED: the composite carrier as a hardware PAL scope sees it - one
//   slow burst-locked reference (per-line phase error shows as spread), NO
//   V-switch correction (PAL bars plot on BOTH mirrored target sets, the
//   bursts at 135 / 225 deg), a slow burst-referenced gain, a choice of line
//   window and field range, and instrument readouts.
//   SECAM (no decode-orc / tbc reference - FM chroma, no burst): the
//   transmitted view - each line plots only its OWN colour difference (D'B
//   lines along U, D'R lines along V, no receiver one-line pairing),
//   referenced to the field-tracked rest carriers, with porch readouts.
enum { GUI_VSCOPE_MODE_STANDARD = 0, GUI_VSCOPE_MODE_ADVANCED = 1 };
// Advanced line window: active picture / the whole line (sync, porches and
// burst included) / the burst window only (SECAM: the back-porch rest carrier).
enum { GUI_VSCOPE_LINES_ACTIVE = 0, GUI_VSCOPE_LINES_WHOLE = 1, GUI_VSCOPE_LINES_BURST = 2 };
// Advanced field range: picture lines / every line (VBI included).
enum { GUI_VSCOPE_FIELD_ACTIVE = 0, GUI_VSCOPE_FIELD_WHOLE = 1 };

// Advanced instrument readouts, per field (decode-orc's set): all from the
// bursts of the field's burst-carrying lines.
typedef struct {
    bool valid;               // A burst reference was recovered
    double burst_ire;         // Mean burst peak amplitude, IRE (sync-referenced)
    double burst_percent;     // ... of the system nominal (PAL 21.43, NTSC 20 IRE)
    double jitter_deg;        // RMS per-line burst phase deviation from the reference
    double vsplit_deg;        // PAL: half the +V/-V burst angle minus 45 deg
    double chroma_burst;      // Mean picture chroma amplitude / mean burst amplitude
    uint32_t burst_lines;     // Lines that carried a usable burst (SECAM: a measured porch)
    // SECAM (Advanced), per component [0] D'B, [1] D'R, from each line's
    // back-porch rest carrier (the decoder's bell-free measurement):
    bool secam;
    double rest_err_khz[2];   // Mean porch frequency - nominal rest (4.25 / 4.40625 MHz)
    double rest_jitter_khz;   // RMS line-to-line porch frequency spread (both components)
    double porch_mv[2];       // Porch carrier peak-to-peak, mV (sync = 300 mV)
    double porch_percent[2];  // ... of the standard (2M0 = 161 mV at f0 through the bell)
    uint32_t secam_lines[2];  // Lines measured per component
    uint32_t ident_errors;    // Consecutive lines carrying the same component
} gui_vscope_meas_t;

// What one published field holds.
typedef struct {
    uint64_t points;          // Samples plotted
    uint32_t segments;        // Lines plotted (the dwell anchor's line count)
    int stride;               // Every stride-th sample plotted (decode-orc sample_stride)
    int x0, x1, y0, y1;       // Bounding box of the hit cells (x0 > x1: none); the
                              // transits run between hits, so they stay inside it
    int system;               // GUI_VSCOPE_SYS_*
    int field;                // Field parity (0 first, 1 second)
    int mode;                 // GUI_VSCOPE_MODE_*
    int lines_view, field_view;
    bool trace;               // Transit counts carried
    gui_vscope_meas_t meas;   // Advanced only
} gui_vscope_meta_t;

typedef struct gui_vectorscope {
    uint32_t *acc;            // Decoder thread: current field's hits
    uint32_t *pub;            // Handed to the renderer at field end
    uint32_t *acc_tr;         // Decoder thread: beam transits between samples
    uint32_t *pub_tr;
    uint64_t acc_points;
    uint64_t pub_points;
    int pub_system;
    int acc_system;
    gui_vscope_meta_t acc_meta, pub_meta;
    // Rows of pub / pub_tr that may hold counts (r0 > r1: all zero). The acc
    // pair is all zero at every field start; its rows are the field's box.
    int pub_r0, pub_r1, pub_t0, pub_t1;
    atomic_int pub_ready;     // 1 = pub holds a field the renderer has not taken
    atomic_uint fields;       // Fields completed (diagnostics / tests)

    // UI requests (applied at the next field start).
    atomic_int want_mode, want_lines, want_field;
    atomic_bool want_trace;

    // Decoder thread: the current field's settings and trace state.
    int mode, lines_view, field_view;
    int stride;               // 2 with the trace on (the transits join the samples), else 1
    bool trace_on;
    bool have_last;
    int last_cell;
    uint32_t run_hits, run_tr;   // Pending counts on last_cell (written on a move)
    // Decoder thread: Advanced readout accumulators for the current field.
    double m_burst_sum, m_burst_ire_sum, m_jit_sq, m_chroma_sum;
    double m_vp_s, m_vp_c, m_vn_s, m_vn_c;   // +V / -V burst unit vectors (rel. reference)
    uint64_t m_chroma_n;
    uint32_t m_lines, m_ire_lines;
    // Decoder thread: SECAM Advanced readout sums ([0] D'B, [1] D'R).
    bool m_secam;
    double m_s_sum[2], m_s_sq[2], m_s_mv[2];
    uint32_t m_s_n[2], m_s_mv_n[2], m_s_ident_err;
    int m_s_last_line;
    bool m_s_last_red;
} gui_vectorscope_t;

bool gui_vectorscope_init(gui_vectorscope_t *scope);
void gui_vectorscope_free(gui_vectorscope_t *scope);
void gui_vectorscope_reset(gui_vectorscope_t *scope);

// Decoder thread: the beam path from the previous sample's cell `a` to this
// sample's cell `b`, counted as transits - the cells strictly BETWEEN them
// (Bresenham). decode-orc's accumulateLine counts both ends too: the callers
// add those to the endpoint cells' pending run counts instead, so a beam that
// rests in one cell costs no grid write per sample. Adjacent cells have
// nothing between them.
static inline void gui_vectorscope_transit_between(uint32_t *tr, int a, int b) {
    int x0 = a & (GUI_VSCOPE_GRID - 1), y0 = a >> GUI_VSCOPE_GRID_SHIFT;
    const int x1 = b & (GUI_VSCOPE_GRID - 1), y1 = b >> GUI_VSCOPE_GRID_SHIFT;
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int ady = y1 > y0 ? y1 - y0 : y0 - y1;
    if (dx <= 1 && ady <= 1) return;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -ady;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        if (x0 == x1 && y0 == y1) break;
        tr[(y0 << GUI_VSCOPE_GRID_SHIFT) | x0]++;
    }
}

// Decoder thread: grow the field's hit box (the cvbs plot pass reduces a
// line's cells to their box first).
static inline void gui_vectorscope_grow_box(gui_vectorscope_t *scope, int x0, int x1,
                                            int y0, int y1) {
    gui_vscope_meta_t *m = &scope->acc_meta;
    if (x0 < m->x0) m->x0 = x0;
    if (x1 > m->x1) m->x1 = x1;
    if (y0 < m->y0) m->y0 = y0;
    if (y1 > m->y1) m->y1 = y1;
}

// Decoder thread: write the pending run (the points and transit ends that
// fell in last_cell since the beam arrived there) and grow the box by it.
static inline void gui_vectorscope_flush_run(gui_vectorscope_t *scope) {
    if (!scope->have_last) return;
    const int c = scope->last_cell;
    scope->acc[c] += scope->run_hits;
    if (scope->run_tr) scope->acc_tr[c] += scope->run_tr;
    const int x = c & (GUI_VSCOPE_GRID - 1), y = c >> GUI_VSCOPE_GRID_SHIFT;
    gui_vectorscope_grow_box(scope, x, x, y, y);
}

// Decoder thread: start a new trace segment (a new line, or the burst and
// picture parts of one line): the beam is not drawn across the gap.
static inline void gui_vectorscope_break(gui_vectorscope_t *scope) {
    gui_vectorscope_flush_run(scope);
    scope->have_last = false;
}

// Decoder thread: one more line plotted (call once per line that added
// points; the dwell anchor's line count).
static inline void gui_vectorscope_count_line(gui_vectorscope_t *scope) {
    scope->acc_meta.segments++;
}

// Decoder thread: one point in grid cell `cell` (y << GUI_VSCOPE_GRID_SHIFT | x).
// Counts as decode-orc: a hit, and with the trace on the transit from the
// previous point of the segment (both ends; a stay counts its cell once).
static inline void gui_vectorscope_add_cell(gui_vectorscope_t *scope, int cell) {
    if (scope->have_last) {
        if (cell == scope->last_cell) {
            scope->run_hits++;
            scope->run_tr += scope->trace_on ? 1u : 0u;
            return;
        }
        if (scope->trace_on) {
            scope->run_tr++;                                    // the transit's start
            gui_vectorscope_transit_between(scope->acc_tr, scope->last_cell, cell);
        }
        gui_vectorscope_flush_run(scope);
        scope->run_tr = scope->trace_on ? 1u : 0u;              // ... and its end
    } else {
        scope->run_tr = 0;
    }
    scope->have_last = true;
    scope->last_cell = cell;
    scope->run_hits = 1;
}

// Grid cell of plot point (u, v), or -1 off the plot.
static inline int gui_vectorscope_cell(double u, double v) {
    const double half = 0.5 * GUI_VSCOPE_GRID;
    const int x = (int)(half + u * (half / GUI_VSCOPE_FULL_SCALE));
    const int y = (int)(half - v * (half / GUI_VSCOPE_FULL_SCALE));
    if ((unsigned)x >= GUI_VSCOPE_GRID || (unsigned)y >= GUI_VSCOPE_GRID) return -1;
    return (y << GUI_VSCOPE_GRID_SHIFT) | x;
}

// Decoder thread: one demodulated chroma point (the decoder's u01/v01) for
// the given system. A point off the plot is skipped without breaking the
// trace (decode-orc: the beam runs on to the next point on the plot).
static inline void gui_vectorscope_add(gui_vectorscope_t *scope, int system,
                                       double u, double v) {
    const int cell = gui_vectorscope_cell(u, v);
    if (cell < 0) return;
    gui_vectorscope_add_cell(scope, cell);
    scope->acc_points++;
    scope->acc_system = system;
}

// Decoder thread, at each field end (field = the parity that ended):
// publish (if the renderer is free) and start a new field.
void gui_vectorscope_end_field(gui_vectorscope_t *scope);
void gui_vectorscope_end_field_parity(gui_vectorscope_t *scope, int field);

// Decoder thread, at the start of a field's first plotted line: latch the
// UI's settings for the field.
void gui_vectorscope_begin_field(gui_vectorscope_t *scope);

// Render thread: copy the latest published field into dst
// (GUI_VSCOPE_GRID^2 counts). Returns false when nothing new is waiting.
bool gui_vectorscope_take(gui_vectorscope_t *scope, uint32_t *dst,
                          uint64_t *points, int *system);
// The same with the transit counts (dst_tr, may be NULL) and the field's
// metadata (meta, may be NULL).
bool gui_vectorscope_take_ex(gui_vectorscope_t *scope, uint32_t *dst, uint32_t *dst_tr,
                             gui_vscope_meta_t *meta);
// Render thread, no copy: swap the caller's GUI_VSCOPE_GRID^2 buffers
// (*hits, *trans: the caller's own allocations, ALL ZERO - handed to the
// scope) for the published field's. *trans is all zero unless meta->trace;
// both hold counts only inside the meta box.
bool gui_vectorscope_take_swap(gui_vectorscope_t *scope, uint32_t **hits, uint32_t **trans,
                               gui_vscope_meta_t *meta);

// Colour-bar target (rgb: bit2 R, bit1 G, bit0 B; 1..6) in plot units for
// the given system and bar amplitude (0.75 or 1.0).
void gui_vectorscope_bar_target(int system, int rgb, double percent,
                                double *u, double *v);

// Plot coordinates -> grid cell (fractional), for tests and the renderer.
void gui_vectorscope_plot_to_grid(double u, double v, double *gx, double *gy);

// Auto system detection: classify a measured vertical-sync interval (us,
// cvbs_vsync_state_t.last_field_us) as 625/50 (GUI_VSCOPE_SYS_PAL) or
// 525/59.94 (GUI_VSCOPE_SYS_NTSC); -1 = neither. Accepts 1-3 whole field
// periods (a decoder on the wrong standard can miss alternate vertical
// syncs: NTSC on PAL measured 33367 us); the six values stay >1.5% apart.
int gui_vectorscope_field_period_system(double field_us);

#endif // GUI_VECTORSCOPE_H
