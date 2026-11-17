/*
 * MISRC GUI - Panel Registry
 *
 * Manages registration of panel vtables and provides factory functions
 * for creating panel instances.
 */

#include "panel_interface.h"
#include "../signal/gui_cvbs.h"
#include "../../common/threading.h"
#include "../../common/buffer_manager.h"
#include "../../common/misrc_debug.h"
#include <stdio.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Registry Storage
//-----------------------------------------------------------------------------

static const panel_vtable_t *s_vtables[PANEL_VIEW_COUNT] = {0};
static bool s_initialized = false;

//-----------------------------------------------------------------------------
// Registry API
//-----------------------------------------------------------------------------

void panel_registry_init(void) {
    if (s_initialized) return;

    memset(s_vtables, 0, sizeof(s_vtables));
    s_initialized = true;
}

void panel_register(panel_view_type_t type, const panel_vtable_t *vtable) {
    if (!s_initialized) {
        panel_registry_init();
    }

    if (type >= 0 && type < PANEL_VIEW_COUNT) {
        s_vtables[type] = vtable;
    }
}

const panel_vtable_t *panel_get_vtable(panel_view_type_t type) {
    if (!s_initialized || type < 0 || type >= PANEL_VIEW_COUNT) {
        return NULL;
    }
    return s_vtables[type];
}

//-----------------------------------------------------------------------------
// Panel Instance Lifecycle
//-----------------------------------------------------------------------------

panel_instance_t panel_instance_create(panel_view_type_t type) {
    panel_instance_t instance = {0};
    instance.type = type;
    instance.vtable = panel_get_vtable(type);
    instance.state = NULL;
    instance.last_bounds = (Rectangle){0};

    if (instance.vtable && instance.vtable->create) {
        instance.state = instance.vtable->create();
    }

    return instance;
}

void panel_instance_destroy(panel_instance_t *instance) {
    if (!instance) return;

    if (instance->vtable && instance->vtable->destroy && instance->state) {
        instance->vtable->destroy(instance->state);
    }

    instance->state = NULL;
    instance->vtable = NULL;
    instance->type = PANEL_VIEW_WAVEFORM;
    instance->last_bounds = (Rectangle){0};
}

void panel_instance_clear(panel_instance_t *instance) {
    if (!instance || !instance->vtable || !instance->vtable->clear || !instance->state) {
        return;
    }
    instance->vtable->clear(instance->state);
}

//-----------------------------------------------------------------------------
// Panel Instance Operations
//-----------------------------------------------------------------------------

void panel_instance_process(panel_instance_t *instance, const int16_t *samples, size_t count, uint32_t sample_rate) {
    if (!instance || !instance->vtable || !instance->vtable->process || !instance->state) {
        return;
    }
    instance->vtable->process(instance->state, samples, count, sample_rate);
}

void panel_instance_render(panel_instance_t *instance, gui_app_t *app, int channel,
                           Rectangle bounds, Color channel_color) {
    if (!instance) return;

    // Cache bounds for click handling
    instance->last_bounds = bounds;

    if (!instance->vtable || !instance->vtable->render) {
        // No vtable or no render function - draw placeholder
        DrawRectangleRec(bounds, (Color){20, 20, 20, 255});
        DrawRectangleLinesEx(bounds, 1, (Color){60, 60, 60, 255});
        return;
    }

    instance->vtable->render(instance->state, app, channel, bounds, channel_color);
}

void panel_instance_render_overlay(panel_instance_t *instance, Rectangle bounds) {
    if (!instance || !instance->vtable || !instance->vtable->render_overlay) {
        return;
    }
    instance->vtable->render_overlay(instance->state, bounds);
}

bool panel_instance_handle_click(panel_instance_t *instance, gui_app_t *app, int channel,
                                 Vector2 click, Rectangle bounds) {
    if (!instance || !instance->vtable || !instance->vtable->handle_click || !instance->state) {
        return false;
    }
    return instance->vtable->handle_click(instance->state, app, channel, click, bounds);
}

bool panel_instance_handle_scroll(panel_instance_t *instance, float delta, Rectangle bounds) {
    if (!instance || !instance->vtable || !instance->vtable->handle_scroll || !instance->state) {
        return false;
    }
    return instance->vtable->handle_scroll(instance->state, delta, bounds);
}

size_t panel_instance_get_menu_count(panel_instance_t *instance) {
    if (!instance || !instance->vtable || !instance->vtable->get_menu_count || !instance->state) {
        return 0;
    }
    return instance->vtable->get_menu_count(instance->state);
}

panel_menu_t panel_instance_get_menu(panel_instance_t *instance, size_t index) {
    panel_menu_t empty = {0};

    if (!instance || !instance->vtable || !instance->vtable->get_menu || !instance->state) {
        return empty;
    }

    size_t count = panel_instance_get_menu_count(instance);
    if (index >= count) {
        return empty;
    }

    return instance->vtable->get_menu(instance->state, index);
}

//-----------------------------------------------------------------------------
// Batch Processing
//-----------------------------------------------------------------------------

// Display-thread throughput profile (MISRC_DEBUG): microseconds spent in
// each panel type's process(), the panel-config lock wait, and the frame
// count, printed every ~2 s with the display FIFO fill and drop count. The
// per-frame budget at 40 MSPS is 1638 us (65536 samples); when the sum of
// the per-type averages exceeds it the FIFO fills (1024 frames = 1.7 s of
// latency) and then drops a trickle of blocks - every drop is a stream jump
// for the decoders (dev/prompt_display_pipeline_counters_readme.md).
static const char *const s_prof_names[PANEL_VIEW_COUNT] = {
    [PANEL_VIEW_WAVEFORM] = "wave", [PANEL_VIEW_FFT] = "fft",
    [PANEL_VIEW_CVBS] = "video", [PANEL_VIEW_HISTOGRAM] = "hist",
    [PANEL_VIEW_WATERFALL] = "wfall", [PANEL_VIEW_SPECTROGRAPH] = "spectro",
    [PANEL_VIEW_DEMOD] = "demod", [PANEL_VIEW_VECTORSCOPE] = "vector",
    [PANEL_VIEW_WFM] = "wfm",
};
static uint64_t s_prof_us[PANEL_VIEW_COUNT];
static uint32_t s_prof_calls[PANEL_VIEW_COUNT];
static uint64_t s_prof_lock_us, s_prof_frames, s_prof_last_ms;

static void panel_prof_tick(gui_app_t *app) {
    if (!misrc_debug_enabled()) return;
    uint64_t now = get_time_ms();
    if (s_prof_last_ms == 0) { s_prof_last_ms = now; return; }
    if (now - s_prof_last_ms < 2000) return;
    double secs = (double)(now - s_prof_last_ms) / 1000.0;
    char line[512];
    int len = snprintf(line, sizeof(line), "[PANELPROF] %.0f frames/s:",
                       (double)s_prof_frames / secs);
    double sum = 0.0;
    for (int t = 0; t < PANEL_VIEW_COUNT; t++) {
        if (!s_prof_calls[t]) continue;
        double per = (double)s_prof_us[t] / (double)s_prof_calls[t];
        sum += (double)s_prof_us[t] / (double)(s_prof_frames ? s_prof_frames : 1);
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %s=%.0fus(x%u)",
                        s_prof_names[t] ? s_prof_names[t] : "?", per,
                        (unsigned)(s_prof_calls[t] / (s_prof_frames ? s_prof_frames : 1)));
    }
    size_t cap = app->buffers.buffers[BUF_DISPLAY].buffer_size;
    size_t fill = cap ? bufmgr_fill_level(&app->buffers, BUF_DISPLAY) : 0;
    snprintf(line + len, sizeof(line) - (size_t)len,
             " | per-frame %.0fus lockwait %.0fus | DISP %u%% drops %u",
             sum, (double)s_prof_lock_us / (double)(s_prof_frames ? s_prof_frames : 1),
             cap ? (unsigned)(fill * 100 / cap) : 0u,
             (unsigned)atomic_load(&app->buffers.stats[BUF_DISPLAY].write_drops));
    fprintf(stderr, "%s\n", line);
    memset(s_prof_us, 0, sizeof(s_prof_us));
    memset(s_prof_calls, 0, sizeof(s_prof_calls));
    s_prof_lock_us = 0;
    s_prof_frames = 0;
    s_prof_last_ms = now;
}

static inline void process_timed(panel_view_type_t view, void *state,
                                 const int16_t *samples, size_t count,
                                 uint32_t sample_rate) {
    const panel_vtable_t *vt = panel_get_vtable(view);
    if (!vt || !vt->process || !state) return;
    uint64_t t0 = get_time_us();
    vt->process(state, samples, count, sample_rate);
    if (view >= 0 && view < PANEL_VIEW_COUNT) {
        s_prof_us[view] += get_time_us() - t0;
        s_prof_calls[view]++;
    }
}

// Helper to process a single panel config's panels
static void process_config_panels(channel_panel_config_t *config,
                                  const int16_t *samples, size_t count,
                                  uint32_t sample_rate) {
    if (!config || !samples || count == 0) return;
    process_timed(config->left_view, config->left_state, samples, count, sample_rate);
    if (config->split) {
        process_timed(config->right_view, config->right_state, samples, count, sample_rate);
    }
}

static inline void panel_cfg_lock(gui_app_t *app) {
    while (atomic_flag_test_and_set(&app->panel_config_lock)) {
        /* The UI may hold this lock while joining a below-normal RF worker.
         * A capture-priority display thread must let that worker run to exit. */
        thrd_sleep_ms(1);
    }
}

static inline void panel_cfg_unlock(gui_app_t *app) {
    atomic_flag_clear(&app->panel_config_lock);
}

void panel_process_all(gui_app_t *app,
                       const int16_t *samples_a,
                       const int16_t *samples_b,
                       size_t count,
                       uint32_t sample_rate) {
    if (!app || count == 0) return;

    g_cvbs_display_frame_seq++;   // per-frame resample cache key (gui_cvbs.c)
    uint64_t t_lock = get_time_us();
    panel_cfg_lock(app);
    s_prof_lock_us += get_time_us() - t_lock;
    s_prof_frames++;

    // Process channel A panels
    if (samples_a) {
        process_config_panels(&app->panel_config_a, samples_a, count, sample_rate);
    }

    // Process channel B panels
    if (samples_b) {
        process_config_panels(&app->panel_config_b, samples_b, count, sample_rate);
    }

    panel_cfg_unlock(app);
    panel_prof_tick(app);
}
