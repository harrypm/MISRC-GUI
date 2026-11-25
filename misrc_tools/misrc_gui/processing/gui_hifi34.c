#include "gui_hifi34.h"

#include "gui_rf_preview_worker.h"
#include "../core/gui_app.h"
#include "../output/gui_audio.h"
#include "../signal/gui_cvbs.h"
#include "../net/gui_net.h"
#include "../../common/threading.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define HIFI34_RING_FRAMES 131072u            // ~2.7 s of 48 kHz stereo history
#define HIFI34_PROBE_PERIOD_MS 200u           // per channel, while in use
#define HIFI34_SWITCH_MARGIN 0.15f            // a rival must beat the current pick by this
#define HIFI34_SWITCH_HOLD_MS 1000u           // ... for this long
#define HIFI34_REAL_AUDIO_MS 1500u            // device audio this recent = real CH3/4
#define HIFI34_PEAK_STALE_MS 250u

// --- shared with the extraction thread (tap) and the worker's audio thread ---
static _Atomic(gui_rf_preview_worker_t *) s_worker;
static atomic_int s_worker_channel = -1;
static atomic_uint s_readers;
static atomic_bool s_want;                    // tap probes / submits only while in use
static atomic_int s_probe_mask;               // bit 0 = RF A, bit 1 = RF B
static atomic_int s_score_x1e4[2][GUI_HIFI_FMT_COUNT];
static gui_app_t *s_app;

static float s_ring[HIFI34_RING_FRAMES * 2];
static atomic_uint_fast64_t s_ring_written;   // total frames (monotonic)
static atomic_uint_fast64_t s_last_audio_ms;

// --- extraction thread only ---
static float s_ema[2][GUI_HIFI_FMT_COUNT];
static uint64_t s_probe_next_ms[2];

// --- main thread only ---
static int s_cur_channel = -1;
static int s_cur_fmt = -1;
static int s_rival_channel = -1, s_rival_fmt = -1;
static uint64_t s_rival_since_ms;
static gui_hifi34_status_t s_status;

const char *gui_hifi34_source_name(int source) {
    return source == 1 ? "RF A" : source == 2 ? "RF B" : "Auto";
}

const char *gui_hifi34_format_name(int format) {
    switch (format) {
        case 1: return "VHS PAL";
        case 2: return "VHS NTSC";
        case 3: return "Beta";
        default: return "Auto";
    }
}

static gui_rf_profile_t hifi34_profile(gui_hifi_fmt_t fmt) {
    switch (fmt) {
        case GUI_HIFI_FMT_VHS_PAL: return GUI_RF_PROFILE_PAL_VHS_HIFI;
        case GUI_HIFI_FMT_BETA_NTSC: return GUI_RF_PROFILE_NTSC_BETAMAX_HIFI;
        default: return GUI_RF_PROFILE_NTSC_VHS_HIFI;
    }
}

//-----------------------------------------------------------------------------
// Worker audio thread: history ring, CH3/4 meters, monitor push
//-----------------------------------------------------------------------------

static void hifi34_audio(void *context, const float *stereo, size_t frames, uint64_t end_sample) {
    if (!stereo || !frames) {
        gui_audio_reset_hifi(context);
        return;
    }
    uint64_t w = atomic_load(&s_ring_written);
    float pl = 0.0f, pr = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        float l = stereo[2 * i], r = stereo[2 * i + 1];
        if (!isfinite(l)) l = 0.0f;
        if (!isfinite(r)) r = 0.0f;
        const size_t slot = (size_t)((w + i) % HIFI34_RING_FRAMES);
        s_ring[2 * slot] = l;
        s_ring[2 * slot + 1] = r;
        if (fabsf(l) > pl) pl = fabsf(l);
        if (fabsf(r) > pr) pr = fabsf(r);
    }
    atomic_store(&s_ring_written, w + frames);
    atomic_store(&s_last_audio_ms, get_time_ms());
    if (s_app) {
        atomic_store(&s_app->hifi34_peak[0], (uint32_t)(fminf(pl, 1.0f) * 8388607.0f));
        atomic_store(&s_app->hifi34_peak[1], (uint32_t)(fminf(pr, 1.0f) * 8388607.0f));
    }
    // Plays only while this worker is the selected monitor source.
    gui_audio_push_rf_checked(context, stereo, frames, end_sample,
                              gui_rf_preview_worker_audio_is_current);
}

//-----------------------------------------------------------------------------
// Extraction thread: carrier probe + decoder feed
//-----------------------------------------------------------------------------

static void hifi34_tap(gui_app_t *app, const int16_t *samples_a, const int16_t *samples_b,
                       size_t count_a, size_t count_b, const gui_rf_block_info_t info[2],
                       bool input_supported) {
    (void)app;
    if (!atomic_load(&s_want) || !info) return;
    const int16_t *samples[2] = { samples_a, samples_b };
    const size_t counts[2] = { count_a, count_b };
    const int mask = atomic_load(&s_probe_mask);
    const uint64_t now = get_time_ms();
    for (int ch = 0; ch < 2; ch++) {
        if (!(mask & (1 << ch)) || !samples[ch] || counts[ch] < GUI_HIFI_PROBE_SAMPLES ||
            now < s_probe_next_ms[ch]) continue;
        s_probe_next_ms[ch] = now + HIFI34_PROBE_PERIOD_MS;
        gui_hifi_probe_result_t r;
        if (!gui_hifi_probe_block(samples[ch], counts[ch], (double)info[ch].sample_rate_hz, &r)) continue;
        for (int f = 0; f < GUI_HIFI_FMT_COUNT; f++) {
            // Single blocks dip at dropouts / head switches (measured: a
            // Hi-Fi file's mean 0.86 with a 0.001 minimum): smooth.
            s_ema[ch][f] = 0.7f * s_ema[ch][f] + 0.3f * r.score[f];
            atomic_store(&s_score_x1e4[ch][f], (int)(s_ema[ch][f] * 10000.0f));
        }
    }
    atomic_fetch_add(&s_readers, 1);
    gui_rf_preview_worker_t *w = atomic_load(&s_worker);
    const int ch = atomic_load(&s_worker_channel);
    if (w && ch >= 0 && ch < 2) {
        if (info[ch].discontinuity && !counts[ch]) gui_rf_preview_worker_reset(w);
        if (input_supported && samples[ch] && counts[ch])
            gui_rf_preview_worker_submit_with_audio_reset(w, samples[ch], counts[ch], &info[ch], NULL);
    }
    atomic_fetch_sub(&s_readers, 1);
}

static gui_rf_preview_worker_t *hifi34_monitor_override(gui_app_t *app) {
    (void)app;
    return s_status.monitored ? atomic_load(&s_worker) : NULL;
}

//-----------------------------------------------------------------------------
// Main thread: when, where and what to decode
//-----------------------------------------------------------------------------

static void hifi34_stop_worker(void) {
    gui_rf_preview_worker_t *w = atomic_exchange(&s_worker, NULL);
    atomic_store(&s_worker_channel, -1);
    if (!w) return;
    while (atomic_load(&s_readers)) thrd_sleep_ms(1);
    gui_video_forget_rf_audio_source(w);
    gui_rf_preview_worker_set_audio_enabled(w, false);
    gui_rf_preview_worker_destroy(w);
    s_cur_channel = -1;
    s_cur_fmt = -1;
}

static bool hifi34_start_worker(int channel, gui_hifi_fmt_t fmt) {
    hifi34_stop_worker();
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create_profile(hifi34_profile(fmt));
    if (!w) return false;
    gui_rf_preview_worker_set_audio_callback(w, hifi34_audio, w);
    gui_rf_preview_worker_set_audio_enabled(w, true);
    atomic_store(&s_worker_channel, channel);
    atomic_store(&s_worker, w);
    s_cur_channel = channel;
    s_cur_fmt = (int)fmt;
    fprintf(stderr, "[HIFI34] decoding RF %c as %s\n", channel ? 'B' : 'A', gui_hifi_fmt_name(fmt));
    return true;
}

// The device streams its own CH3/4 audio: HSDAOH (MISRC V2.5 4-ch audio)
// or CXADC (clockgen CH3 = head switch), while its audio is arriving.
static bool hifi34_real_ch34(gui_app_t *app) {
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    const device_type_t type = app->devices[app->selected_device].type;
    if (type != DEVICE_TYPE_HSDAOH && type != DEVICE_TYPE_CXADC) return false;
    const uint64_t rx = atomic_load(&app->audio_rx_ms);
    return rx && get_time_ms() - rx < HIFI34_REAL_AUDIO_MS;
}

static bool hifi34_panel_shown(const gui_app_t *app) {
    const channel_panel_config_t *cfgs[2] = { &app->panel_config_a, &app->panel_config_b };
    for (int i = 0; i < 2; i++) {
        if (cfgs[i]->left_view == PANEL_VIEW_HIFI) return true;
        if (cfgs[i]->split && cfgs[i]->right_view == PANEL_VIEW_HIFI) return true;
    }
    return false;
}

void gui_hifi34_init(gui_app_t *app) {
    s_app = app;
    gui_video_set_rf_tap(hifi34_tap, hifi34_monitor_override);
}

void gui_hifi34_shutdown(void) {
    atomic_store(&s_want, false);
    hifi34_stop_worker();
}

void gui_hifi34_update(gui_app_t *app) {
    if (!app) return;
    gui_hifi34_status_t st = {0};
    st.channel = -1;
    const bool source_running = app->is_capturing && !atomic_load(&app->rf_source_is_rtlsdr) &&
                                app->settings.net_mode != GUI_NET_MODE_MONITOR_CLIENT;
    st.real_ch34 = hifi34_real_ch34(app);
    const bool ch34_selected = app->settings.audio_monitor_ch34;
    st.wanted = source_running && (hifi34_panel_shown(app) || (ch34_selected && !st.real_ch34));
    atomic_store(&s_want, st.wanted);

    const int source = app->settings.hifi34_source;
    const int format = app->settings.hifi34_format;
    atomic_store(&s_probe_mask, source == 1 ? 1 : source == 2 ? 2 : 3);
    for (int ch = 0; ch < 2; ch++)
        for (int f = 0; f < GUI_HIFI_FMT_COUNT; f++)
            st.score[ch][f] = (float)atomic_load(&s_score_x1e4[ch][f]) / 10000.0f;

    if (!st.wanted) {
        hifi34_stop_worker();
        s_rival_channel = -1;
    } else {
        // Candidate (channel, format) from the settings and the probe.
        bool fmt_ok[GUI_HIFI_FMT_COUNT] = {0};
        if (format == 1) fmt_ok[GUI_HIFI_FMT_VHS_PAL] = true;
        else if (format == 2) fmt_ok[GUI_HIFI_FMT_VHS_NTSC] = true;
        else if (format == 3) fmt_ok[GUI_HIFI_FMT_BETA_NTSC] = true;
        else fmt_ok[GUI_HIFI_FMT_VHS_PAL] = fmt_ok[GUI_HIFI_FMT_VHS_NTSC] = true;
        int best_ch = -1, best_fmt = -1;
        float best = -1.0f;
        for (int ch = 0; ch < 2; ch++) {
            if ((source == 1 && ch != 0) || (source == 2 && ch != 1)) continue;
            for (int f = 0; f < GUI_HIFI_FMT_COUNT; f++) {
                if (fmt_ok[f] && st.score[ch][f] > best) {
                    best = st.score[ch][f];
                    best_ch = ch;
                    best_fmt = f;
                }
            }
        }
        const bool forced = source != 0 && format != 0;
        int want_ch = -1, want_fmt = -1;
        if (forced) {
            want_ch = source - 1;
            want_fmt = best_fmt;   // the one allowed format
        } else if (best >= GUI_HIFI_PROBE_PRESENT) {
            want_ch = best_ch;
            want_fmt = best_fmt;
        }
        const uint64_t now = get_time_ms();
        if (want_ch >= 0 && (want_ch != s_cur_channel || want_fmt != s_cur_fmt)) {
            bool take = s_cur_channel < 0 || forced;
            if (!take) {
                // Hysteresis: a rival must clearly beat the current pick for a while.
                const float cur = st.score[s_cur_channel][s_cur_fmt];
                const bool allowed_now = fmt_ok[s_cur_fmt] &&
                    !((source == 1 && s_cur_channel != 0) || (source == 2 && s_cur_channel != 1));
                if (!allowed_now) {
                    take = true;   // the settings changed under it
                } else if (best > cur + HIFI34_SWITCH_MARGIN) {
                    if (s_rival_channel != want_ch || s_rival_fmt != want_fmt) {
                        s_rival_channel = want_ch;
                        s_rival_fmt = want_fmt;
                        s_rival_since_ms = now;
                    } else if (now - s_rival_since_ms >= HIFI34_SWITCH_HOLD_MS) {
                        take = true;
                    }
                } else {
                    s_rival_channel = -1;
                }
            }
            if (take) {
                const gui_rf_profile_t profile = hifi34_profile((gui_hifi_fmt_t)want_fmt);
                const uint32_t rate = (uint32_t)atomic_load(&app->rf_source_rate_hz[want_ch]);
                if (rate && !gui_rf_preview_worker_rate_supported(profile, rate)) {
                    hifi34_stop_worker();
                    st.rate_unsupported = true;
                } else {
                    hifi34_start_worker(want_ch, (gui_hifi_fmt_t)want_fmt);
                }
                s_rival_channel = -1;
            }
        } else if (want_ch < 0 && s_cur_channel >= 0) {
            // Carriers gone below the threshold: keep the current pick (a
            // dropout must not stop the decoder); a rival switches it.
        }
    }

    gui_rf_preview_worker_t *w = atomic_load(&s_worker);
    st.running = w != NULL;
    st.channel = s_cur_channel;
    st.fmt = s_cur_fmt >= 0 ? (gui_hifi_fmt_t)s_cur_fmt : GUI_HIFI_FMT_VHS_NTSC;
    st.owns_ch34 = st.running && !st.real_ch34;
    st.monitored = st.owns_ch34 && ch34_selected;
    if (w) {
        gui_rf_preview_stats_t ws;
        gui_rf_preview_worker_get_stats(w, &ws);
        st.locked = ws.hifi.locked;
        st.cpu_percent = ws.cpu_percent;
        static uint32_t s_logged_rate;
        if (ws.vhs_hifi_rf_rate_hz && ws.vhs_hifi_rf_rate_hz != s_logged_rate) {
            s_logged_rate = ws.vhs_hifi_rf_rate_hz;
            fprintf(stderr, "[HIFI34] decoder input %.3f MSPS (source %.3f MSPS)\n",
                    ws.vhs_hifi_rf_rate_hz / 1e6, ws.source_rate_hz / 1e6);
        }
        snprintf(st.reason, sizeof(st.reason), "%s", ws.hifi.reason[0] ? ws.hifi.reason : ws.decode.reason);
    }
    if (!st.rate_unsupported && s_status.rate_unsupported && st.wanted && !st.running)
        st.rate_unsupported = true;   // keep the verdict until the pick changes
    s_status = st;

    atomic_store(&app->hifi34_owns_ch34, st.owns_ch34);
    const uint64_t last = atomic_load(&s_last_audio_ms);
    if (!st.running || !last || get_time_ms() - last > HIFI34_PEAK_STALE_MS) {
        atomic_store(&app->hifi34_peak[0], 0);
        atomic_store(&app->hifi34_peak[1], 0);
    }
}

void gui_hifi34_get_status(gui_hifi34_status_t *status) {
    if (status) *status = s_status;
}

uint64_t gui_hifi34_frames_total(void) {
    return atomic_load(&s_ring_written);
}

size_t gui_hifi34_copy_recent(float *stereo, size_t frames) {
    if (!stereo || !frames) return 0;
    const uint64_t w = atomic_load(&s_ring_written);
    if (frames > HIFI34_RING_FRAMES / 2) frames = HIFI34_RING_FRAMES / 2;   // stay clear of the writer
    if ((uint64_t)frames > w) frames = (size_t)w;
    const uint64_t start = w - frames;
    for (size_t i = 0; i < frames; i++) {
        const size_t slot = (size_t)((start + i) % HIFI34_RING_FRAMES);
        stereo[2 * i] = s_ring[2 * slot];
        stereo[2 * i + 1] = s_ring[2 * slot + 1];
    }
    return frames;
}
