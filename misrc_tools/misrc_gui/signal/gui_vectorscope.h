/*
 * MISRC GUI - Composite Vectorscope (accumulator + calibration)
 *
 * A COMPOSITE vectorscope: the U/V points come from demodulating the
 * composite signal's chroma against its own colour burst (the CVBS
 * decoder's burst-locked demod), exactly what a hardware composite
 * vectorscope does - not from a component (YUV/RGB) picture.
 *
 * The decoder adds one point per active-picture pixel of every burst-
 * carrying line (gui_vectorscope_add, decoder thread). At each field end the
 * accumulated density grid is handed to the render thread (lock-free double
 * buffer: published only when the renderer has consumed the previous one).
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

typedef struct gui_vectorscope {
    uint32_t *acc;            // Decoder thread: current field's hits
    uint32_t *pub;            // Handed to the renderer at field end
    uint64_t acc_points;
    uint64_t pub_points;
    int pub_system;
    int acc_system;
    atomic_int pub_ready;     // 1 = pub holds a field the renderer has not taken
    atomic_uint fields;       // Fields completed (diagnostics / tests)
} gui_vectorscope_t;

bool gui_vectorscope_init(gui_vectorscope_t *scope);
void gui_vectorscope_free(gui_vectorscope_t *scope);
void gui_vectorscope_reset(gui_vectorscope_t *scope);

// Decoder thread: one demodulated chroma point (the decoder's u01/v01) for
// the given system.
static inline void gui_vectorscope_add(gui_vectorscope_t *scope, int system,
                                       double u, double v) {
    double half = 0.5 * GUI_VSCOPE_GRID;
    int x = (int)(half + u * (half / GUI_VSCOPE_FULL_SCALE));
    int y = (int)(half - v * (half / GUI_VSCOPE_FULL_SCALE));
    if ((unsigned)x >= GUI_VSCOPE_GRID || (unsigned)y >= GUI_VSCOPE_GRID) return;
    scope->acc[y * GUI_VSCOPE_GRID + x]++;
    scope->acc_points++;
    scope->acc_system = system;
}

// Decoder thread, at each field end: publish (if the renderer is free) and
// start a new field.
void gui_vectorscope_end_field(gui_vectorscope_t *scope);

// Render thread: copy the latest published field into dst
// (GUI_VSCOPE_GRID^2 counts). Returns false when nothing new is waiting.
bool gui_vectorscope_take(gui_vectorscope_t *scope, uint32_t *dst,
                          uint64_t *points, int *system);

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
