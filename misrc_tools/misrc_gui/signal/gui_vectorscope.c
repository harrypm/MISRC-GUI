/*
 * MISRC GUI - Composite Vectorscope (accumulator + calibration)
 * See gui_vectorscope.h.
 */
#include "gui_vectorscope.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VSCOPE_CELLS ((size_t)GUI_VSCOPE_GRID * GUI_VSCOPE_GRID)

bool gui_vectorscope_init(gui_vectorscope_t *scope) {
    if (!scope) return false;
    memset(scope, 0, sizeof(*scope));
    scope->acc = (uint32_t *)calloc(VSCOPE_CELLS, sizeof(uint32_t));
    scope->pub = (uint32_t *)calloc(VSCOPE_CELLS, sizeof(uint32_t));
    if (!scope->acc || !scope->pub) {
        gui_vectorscope_free(scope);
        return false;
    }
    atomic_init(&scope->pub_ready, 0);
    atomic_init(&scope->fields, 0);
    return true;
}

void gui_vectorscope_free(gui_vectorscope_t *scope) {
    if (!scope) return;
    free(scope->acc);
    free(scope->pub);
    scope->acc = scope->pub = NULL;
}

void gui_vectorscope_reset(gui_vectorscope_t *scope) {
    if (!scope || !scope->acc) return;
    memset(scope->acc, 0, VSCOPE_CELLS * sizeof(uint32_t));
    scope->acc_points = 0;
}

void gui_vectorscope_end_field(gui_vectorscope_t *scope) {
    if (!scope || !scope->acc) return;
    atomic_fetch_add(&scope->fields, 1u);
    // Renderer still holds the previous field: drop this one (the display
    // only needs the latest; the decoder never waits on the render thread).
    if (atomic_load_explicit(&scope->pub_ready, memory_order_acquire) == 0) {
        uint32_t *swap = scope->pub;
        scope->pub = scope->acc;
        scope->acc = swap;
        scope->pub_points = scope->acc_points;
        scope->pub_system = scope->acc_system;
        atomic_store_explicit(&scope->pub_ready, 1, memory_order_release);
    }
    memset(scope->acc, 0, VSCOPE_CELLS * sizeof(uint32_t));
    scope->acc_points = 0;
}

bool gui_vectorscope_take(gui_vectorscope_t *scope, uint32_t *dst,
                          uint64_t *points, int *system) {
    if (!scope || !scope->pub || !dst) return false;
    if (atomic_load_explicit(&scope->pub_ready, memory_order_acquire) == 0) return false;
    memcpy(dst, scope->pub, VSCOPE_CELLS * sizeof(uint32_t));
    if (points) *points = scope->pub_points;
    if (system) *system = scope->pub_system;
    atomic_store_explicit(&scope->pub_ready, 0, memory_order_release);
    return true;
}

void gui_vectorscope_bar_target(int system, int rgb, double percent,
                                double *u, double *v) {
    double r = percent * (double)((rgb >> 2) & 1);
    double g = percent * (double)((rgb >> 1) & 1);
    double b = percent * (double)(rgb & 1);
    // BT.601 U/V (the tbc-analyse graticule matrix).
    double uu = r * -0.147141 + g * -0.288869 + b * 0.436010;
    double vv = r * 0.614975 + g * -0.514965 + b * -0.100010;
    // NTSC chroma rides the 92.5 IRE (7.5 IRE setup) span; the plot unit is
    // 100 IRE.
    double k = (system == GUI_VSCOPE_SYS_NTSC) ? 0.925 : 1.0;
    if (u) *u = uu * k;
    if (v) *v = vv * k;
}

int gui_vectorscope_field_period_system(double field_us) {
    static const double periods[2] = { 20000.0, 1e6 * 1001.0 / 60000.0 };
    static const int systems[2] = { GUI_VSCOPE_SYS_PAL, GUI_VSCOPE_SYS_NTSC };
    for (int k = 1; k <= 3; k++) {
        for (int i = 0; i < 2; i++) {
            double want = periods[i] * k;
            if (fabs(field_us - want) < want * 0.015) return systems[i];
        }
    }
    return -1;
}

void gui_vectorscope_plot_to_grid(double u, double v, double *gx, double *gy) {
    double half = 0.5 * GUI_VSCOPE_GRID;
    if (gx) *gx = half + u * (half / GUI_VSCOPE_FULL_SCALE);
    if (gy) *gy = half - v * (half / GUI_VSCOPE_FULL_SCALE);
}
