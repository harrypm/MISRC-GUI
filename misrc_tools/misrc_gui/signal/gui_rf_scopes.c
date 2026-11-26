/*
 * MISRC GUI - Scopes on the RF Preview's decoded output (see gui_rf_scopes.h).
 */
#include "gui_rf_scopes.h"

#include "../../common/threading.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define RF_SCOPES_IN_USE_MS 500u

typedef struct {
    gui_vectorscope_t vs;
    gui_wfm_t wfm;
    bool vs_ok, wfm_ok;                 // initialised (main thread, before any use)
    atomic_uint_fast64_t vs_ms, wfm_ms; // last time a panel used them
    bool vs_live, wfm_live;             // RF worker thread: this field feeds them
    gui_rf_scope_sink_t sink;
} rf_scope_channel_t;

static rf_scope_channel_t s_ch[2];

static bool rf_scopes_recent(atomic_uint_fast64_t *ms) {
    const uint64_t t = atomic_load(ms);
    return t && get_time_ms() - t < RF_SCOPES_IN_USE_MS;
}

//-----------------------------------------------------------------------------
// RF worker thread (the decoder's field renderer)
//-----------------------------------------------------------------------------

static void rf_scopes_begin_field(void *ctx) {
    rf_scope_channel_t *c = ctx;
    c->vs_live = c->vs_ok && rf_scopes_recent(&c->vs_ms);
    c->wfm_live = c->wfm_ok && rf_scopes_recent(&c->wfm_ms);
    if (c->vs_live) gui_vectorscope_begin_field(&c->vs);
}

static void rf_scopes_line(void *ctx, const gui_rf_scope_line_t *l) {
    rf_scope_channel_t *c = ctx;
    const bool ntsc = l->format == GUI_RF_NTSC;
    if (c->vs_live && l->color && (c->vs.field_view == GUI_VSCOPE_FIELD_WHOLE || l->picture)) {
        gui_vectorscope_t *vs = &c->vs;
        const int system = ntsc ? GUI_VSCOPE_SYS_NTSC : GUI_VSCOPE_SYS_PAL;
        const int stride = vs->stride > 0 ? vs->stride : 1;
        // Lines: Active = the active window; Whole / Burst = the whole row
        // the decoder rendered (Full frame mode: sync, burst and picture).
        const bool active_only = vs->lines_view == GUI_VSCOPE_LINES_ACTIVE;
        bool any = false;
        gui_vectorscope_break(vs);
        for (int i = 0; i < l->count; i += stride) {
            const double t = l->t0_us + i * l->dt_us;
            if (active_only && (t < l->active_start_us || t >= l->active_end_us)) continue;
            gui_vectorscope_add(vs, system, l->u[i], l->v[i]);
            any = true;
        }
        if (any) gui_vectorscope_count_line(vs);
    }
    if (c->wfm_live) {
        const double fsc = ntsc ? 315e6 / 88.0 : 4433618.75;
        const double cycles = fsc * 1e-6 * l->line_us * (double)l->line;
        const double two_pi = 6.283185307179586;
        const gui_wfm_decoded_line_t dl = {
            .ire = l->ire,
            .u = l->color ? l->u : NULL,
            .v = l->color ? l->v : NULL,
            .count = l->count,
            .t0_us = l->t0_us,
            .dt_us = l->dt_us,
            .rate = 4.0 * fsc,
            .fsc_hz = fsc,
            .phase0 = two_pi * (cycles - floor(cycles)),
            .line_us = l->line_us,
            .active_start_us = l->active_start_us,
            .active_end_us = l->active_end_us,
            .system = ntsc ? GUI_WFM_SYS_NTSC : GUI_WFM_SYS_PAL,
            .picture = l->picture,
        };
        gui_wfm_add_decoded_line(&c->wfm, &dl);
    }
}

static void rf_scopes_end_field(void *ctx, int parity, int field1_parity) {
    rf_scope_channel_t *c = ctx;
    if (c->vs_live) gui_vectorscope_end_field_parity(&c->vs, parity == field1_parity ? 0 : 1);
    if (c->wfm_live) gui_wfm_end_field_locked(&c->wfm, parity, field1_parity, false);
}

//-----------------------------------------------------------------------------
// Main thread
//-----------------------------------------------------------------------------

static rf_scope_channel_t *rf_scopes_channel(int channel) {
    if (channel < 0 || channel > 1) return NULL;
    rf_scope_channel_t *c = &s_ch[channel];
    if (!c->sink.line) {
        c->sink.ctx = c;
        c->sink.begin_field = rf_scopes_begin_field;
        c->sink.line = rf_scopes_line;
        c->sink.end_field = rf_scopes_end_field;
    }
    return c;
}

gui_vectorscope_t *gui_rf_scopes_vectorscope(int channel) {
    rf_scope_channel_t *c = rf_scopes_channel(channel);
    if (!c) return NULL;
    if (!c->vs_ok) c->vs_ok = gui_vectorscope_init(&c->vs);
    if (!c->vs_ok) return NULL;
    atomic_store(&c->vs_ms, get_time_ms());
    return &c->vs;
}

gui_wfm_t *gui_rf_scopes_wfm(int channel) {
    rf_scope_channel_t *c = rf_scopes_channel(channel);
    if (!c) return NULL;
    if (!c->wfm_ok) c->wfm_ok = gui_wfm_init(&c->wfm);
    if (!c->wfm_ok) return NULL;
    atomic_store(&c->wfm_ms, get_time_ms());
    return &c->wfm;
}

const gui_rf_scope_sink_t *gui_rf_scopes_sink(int channel) {
    rf_scope_channel_t *c = rf_scopes_channel(channel);
    if (!c) return NULL;
    const bool used = (c->vs_ok && rf_scopes_recent(&c->vs_ms)) ||
                      (c->wfm_ok && rf_scopes_recent(&c->wfm_ms));
    return used ? &c->sink : NULL;
}
