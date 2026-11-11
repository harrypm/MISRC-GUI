/* Production pooled UI routes: lazy creation, duplicate views, independent
 * systems/channels, pressure gating and last-subscriber teardown during copy. */
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/processing/gui_rf_preview_pressure.c"
#include "../misrc_gui/visualization/gui_panel.c"
/* Exercise the production profile selectors while retaining the route fixture's
 * lightweight binder below. LTO drops the unused texture/render functions. */
#define gui_vhs_fm_bind_worker route_production_bind_worker
#include "../misrc_gui/signal/gui_vhs_fm.c"
#undef gui_vhs_fm_bind_worker
#include <assert.h>
typedef struct {
    gui_rf_format_t format;
    gui_rf_profile_t profile;
    gui_rf_ld_audio_t ld_audio;
    bool ld_sync;
    atomic_bool paused, audio_enabled;
    atomic_int submits;
} route_worker_t;
static const void *monitor_source;
static bool transport_running,transport_paused[2];
static bool ld_dts_available = true;
static playback_speed_t transport_speed = PLAYBACK_SPEED_1X;
static unsigned monitor_resets;
static unsigned seek_monitor_epoch_floor;
static atomic_int route_calls, creates, destroys, resets, binds;
static atomic_bool hold_copy, copy_entered, detach_done;
static vhs_fm_decoder_t *detach_decoder;
static gui_app_t route_app;
static size_t simulated_fill;
static int16_t route_a[2]={123,-456}, route_b[2]={789,-123};
static gui_rf_block_info_t route_info[2]={{40000000,123456,42,false},{20000000,654321,43,true}};

enum {
    OVERLAY_LD_FORMAT = 1u << 0,
    OVERLAY_LD_AUDIO = 1u << 1,
    OVERLAY_LD_OPTION = 1u << 2,
    OVERLAY_OSD_OPTION = 1u << 3,
    OVERLAY_RF_DECODER = 1u << 4,
    OVERLAY_PAL_SYSTEM = 1u << 5,
    OVERLAY_NTSC_SYSTEM = 1u << 6,
    OVERLAY_HIFI_PRESET = 1u << 7,
    OVERLAY_LD_DTS = 1u << 8
};
static struct {
    Rectangle panel, button;
    float scale;
    unsigned labels, seen;
    bool awaiting_label;
} overlay_draw;

float gui_ui_get_scale_factor(void) { return overlay_draw.scale; }
Vector2 gui_ui_get_mouse_position(void) { return (Vector2){-1,-1}; }
bool CheckCollisionPointRec(Vector2 point, Rectangle rect) {
    return point.x>=rect.x && point.x<rect.x+rect.width &&
           point.y>=rect.y && point.y<rect.y+rect.height;
}
Color gui_dropdown_option_color(bool selected, bool hovered) {
    return selected ? COLOR_BUTTON_ACTIVE : hovered ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
}
int gui_text_measure(const char *text, int size) { return (int)strlen(text)*size/2; }

static void overlay_record_button(Rectangle rect) {
    assert(!overlay_draw.awaiting_label && rect.width>0 && rect.height>0);
    float scale=overlay_draw.scale;
    Rectangle panel=overlay_draw.panel;
    assert(rect.x*scale>=panel.x*scale-.01f && rect.y*scale>=panel.y*scale-.01f);
    assert((rect.x+rect.width)*scale<=(panel.x+panel.width-8)*scale+.01f);
    assert((rect.y+rect.height)*scale<=(panel.y+panel.height)*scale+.01f);
    overlay_draw.button=rect;
    overlay_draw.awaiting_label=true;
}
void DrawRectangleRounded(Rectangle rect, float roundness, int segments, Color color) {
    (void)roundness;(void)segments;(void)color;
    overlay_record_button(rect);
}
void DrawRectangleRec(Rectangle rect, Color color) {
    (void)color;
    overlay_record_button(rect);
}
void DrawTriangle(Vector2 a, Vector2 b, Vector2 c, Color color) {
    (void)a;(void)b;(void)c;(void)color;
}
void gui_text_draw(const char *text, float x, float y, int size, Color color) {
    (void)color;
    assert(overlay_draw.awaiting_label);
    Rectangle button=overlay_draw.button;
    float scale=overlay_draw.scale;
    /* The Clay transform scales the rectangle and the glyphs together. */
    float right=(x+gui_text_measure(text,size))*scale;
    if (right>(button.x+button.width)*scale+.01f)
        fprintf(stderr,"RF overlay %.0f%% label %s: right %.2f exceeds button %.2f\n",
                scale*100,text,right,(button.x+button.width)*scale);
    assert(x*scale>=button.x*scale-.01f && right<=(button.x+button.width)*scale+.01f);
    assert(y*scale>=button.y*scale-.01f && (y+size)*scale<=(button.y+button.height)*scale+.01f);
    if (!strcmp(text,"Format: LaserDisc")) overlay_draw.seen|=OVERLAY_LD_FORMAT;
    if (!strcmp(text,"Audio: Digital PCM")) overlay_draw.seen|=OVERLAY_LD_AUDIO;
    if (!strcmp(text,"Audio: DTS")) overlay_draw.seen|=OVERLAY_LD_DTS;
    if (!strcmp(text,"LaserDisc")) overlay_draw.seen|=OVERLAY_LD_OPTION;
    if (!strcmp(text,"Basic")) overlay_draw.seen|=OVERLAY_OSD_OPTION;
    if (!strcmp(text,"Decoder: RF Preview")) overlay_draw.seen|=OVERLAY_RF_DECODER;
    if (!strcmp(text,"System: PAL")) overlay_draw.seen|=OVERLAY_PAL_SYSTEM;
    if (!strcmp(text,"System: NTSC")) overlay_draw.seen|=OVERLAY_NTSC_SYSTEM;
    if (!strcmp(text,"Preset: Hi-Fi")) overlay_draw.seen|=OVERLAY_HIFI_PRESET;
    ++overlay_draw.labels;
    overlay_draw.awaiting_label=false;
}

static void test_rf_overlay_scale(void) {
    video_panel_state_t panel={0};
    vhs_fm_decoder_t *d=&panel.vhs_fm.decoder;
    assert(gui_vhs_fm_init(d));
    d->overlay.selected_tape=GUI_RF_FAMILY_LASERDISC;
    d->overlay.ld_audio=GUI_RF_LD_AUDIO_DIGITAL;
    gui_vhs_fm_set_format(d,1);
    const float scales[]={1.0f,1.5f,2.0f};
    for (size_t i=0;i<sizeof(scales)/sizeof(scales[0]);++i) {
        float scale=scales[i];
        Rectangle bounds={96/scale,24/scale,520/scale,432/scale};
        for (int menu=0;menu<2;++menu) {
            d->overlay.tape_dropdown_open=menu==0;
            d->overlay.stats_dropdown_open=menu==1;
            overlay_draw.panel=bounds;
            overlay_draw.scale=scale;
            overlay_draw.labels=overlay_draw.seen=0;
            render_vhs_decoder_overlay(&panel,&route_app,bounds);
            unsigned expected=OVERLAY_LD_FORMAT|OVERLAY_LD_AUDIO|OVERLAY_RF_DECODER|OVERLAY_NTSC_SYSTEM|
                (menu==0 ? OVERLAY_LD_OPTION : OVERLAY_OSD_OPTION);
            assert(!overlay_draw.awaiting_label && overlay_draw.labels==10);
            assert(overlay_draw.seen==expected);
            assert(d->overlay.tape_rect.y < d->overlay.format_rect.y &&
                   d->overlay.format_rect.y < d->overlay.ld_audio_rect.y);
        }
    }
    d->overlay.tape_dropdown_open = d->overlay.stats_dropdown_open = false;
    d->overlay.ld_audio = GUI_RF_LD_AUDIO_DTS;
    for (int system = 0; system < 2; ++system) {
        gui_vhs_fm_set_format(d, system);
        overlay_draw.labels = overlay_draw.seen = 0;
        render_vhs_decoder_overlay(&panel, &route_app, overlay_draw.panel);
        unsigned expected = OVERLAY_LD_FORMAT | OVERLAY_LD_DTS | OVERLAY_RF_DECODER |
            (system ? OVERLAY_NTSC_SYSTEM : OVERLAY_PAL_SYSTEM);
        assert(!overlay_draw.awaiting_label && overlay_draw.seen == expected);
    }

    d->overlay.selected_tape = GUI_RF_FAMILY_VHS;
    gui_vhs_fm_set_vhs_preset(d, GUI_VHS_PRESET_HIFI);
    for (int system = 0; system < 2; ++system) {
        gui_vhs_fm_set_format(d, system);
        overlay_draw.panel = (Rectangle){64, 16, 346, 288};
        overlay_draw.scale = 1.5f;
        overlay_draw.labels = overlay_draw.seen = 0;
        render_vhs_decoder_overlay(&panel, &route_app, overlay_draw.panel);
        unsigned expected = OVERLAY_RF_DECODER | OVERLAY_HIFI_PRESET |
            (system ? OVERLAY_NTSC_SYSTEM : OVERLAY_PAL_SYSTEM);
        assert(!overlay_draw.awaiting_label && overlay_draw.seen == expected);
        assert(d->overlay.tape_rect.y < d->overlay.vhs_preset_rect.y &&
               d->overlay.vhs_preset_rect.y < d->overlay.format_rect.y);
    }

    d->overlay.selected_tape = GUI_RF_FAMILY_BETAMAX;
    overlay_draw.labels = overlay_draw.seen = 0;
    render_vhs_decoder_overlay(&panel, &route_app, overlay_draw.panel);
    assert(d->overlay.tape_rect.y < d->overlay.beta_preset_rect.y &&
           d->overlay.beta_preset_rect.y < d->overlay.format_rect.y);
}

static void test_rf_overlay_short_panes(void) {
    video_panel_state_t panel = {0};
    vhs_fm_decoder_t *decoder = &panel.vhs_fm.decoder;
    assert(gui_vhs_fm_init(decoder));
    const float heights[] = {150, 96};
    for (int mode = 0; mode < 3; ++mode) {
        decoder->overlay.selected_tape = mode == 2 ? GUI_RF_FAMILY_LASERDISC : GUI_RF_FAMILY_VHS;
        gui_vhs_fm_set_format(decoder, 1);
        gui_vhs_fm_set_vhs_preset(decoder, mode == 1 ? GUI_VHS_PRESET_HIFI : GUI_VHS_PRESET_SVHS);
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h) {
            Rectangle bounds = {48, 12, 260, heights[h]};
            for (int menu = 0; menu < 2; ++menu) {
                decoder->overlay.stats_dropdown_open = menu == 0;
                decoder->overlay.tape_dropdown_open = menu == 1;
                overlay_draw.panel = bounds;
                overlay_draw.scale = 2.0f;
                overlay_draw.labels = overlay_draw.seen = 0;
                render_vhs_decoder_overlay(&panel, &route_app, bounds);

                Rectangle osd = decoder->overlay.stats_btn_rect;
                Vector2 osd_center = {osd.x + osd.width / 2, osd.y + osd.height / 2};
                assert(decoder->overlay.is_visible && CheckCollisionPointRec(osd_center, bounds));
                if (mode != 2) {
                    assert(decoder->overlay.vhs_preset_rect.y < decoder->overlay.format_rect.y);
                } else {
                    assert(decoder->overlay.format_rect.y < decoder->overlay.ld_audio_rect.y);
                }
                assert(!overlay_draw.awaiting_label && overlay_draw.labels == (mode == 1 ? 9 : 10));
                assert(overlay_draw.seen & (menu == 0 ? OVERLAY_OSD_OPTION : OVERLAY_LD_OPTION));
            }
        }
    }
}

gui_rf_preview_worker_t *gui_rf_preview_worker_create(gui_rf_format_t format) {
    route_worker_t *w=calloc(1,sizeof(*w)); assert(w); w->format=format;
    atomic_init(&w->audio_enabled,true);
    atomic_init(&w->submits,0);
    atomic_fetch_add(&creates,1); return (gui_rf_preview_worker_t*)w;
}
bool gui_rf_profile_has_hifi(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI || profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI ||
        profile==GUI_RF_PROFILE_NTSC_ED_BETA || gui_rf_profile_is_audio_only(profile);
}
gui_rf_format_t gui_rf_profile_format(gui_rf_profile_t p) {
    return p==GUI_RF_PROFILE_PAL_VHS || p==GUI_RF_PROFILE_SECAM_VHS ||
        p==GUI_RF_PROFILE_MESECAM_VHS || p==GUI_RF_PROFILE_PAL_SVHS ||
        p==GUI_RF_PROFILE_PAL_LASERDISC || p==GUI_RF_PROFILE_PAL_VHS_HIFI ? GUI_RF_PAL : GUI_RF_NTSC;
}
gui_rf_preview_worker_t *gui_rf_preview_worker_create_profile(gui_rf_profile_t profile) {
    gui_rf_preview_worker_t *worker=gui_rf_preview_worker_create(gui_rf_profile_format(profile));
    ((route_worker_t*)worker)->profile=profile;
    return worker;
}
void gui_rf_preview_worker_set_audio_callback(gui_rf_preview_worker_t *w, gui_rf_preview_audio_callback_t cb, void *ctx) {
    assert(w && cb && ctx==w);
}
bool gui_rf_preview_worker_rate_supported(gui_rf_profile_t profile, uint32_t rate) {
    return rate >= (gui_rf_profile_is_laserdisc(profile) ? 20000000u : 16000000u) ||
           (gui_rf_profile_is_audio_only(profile) &&
            (rate == 8000000u || rate == 8192000u || rate == 10000000u));
}
void gui_rf_preview_worker_set_audio_enabled(gui_rf_preview_worker_t *w,bool enabled) {
    assert(w); atomic_store(&((route_worker_t*)w)->audio_enabled,enabled);
}
static bool finish_worker_ready = true, finish_monitor_ready = true;
static unsigned finish_worker_calls, finish_monitor_calls, finish_transport_calls;
static unsigned finish_video_calls, transport_ld_video_channels;
static gui_rf_preview_worker_t *finish_video_pending;
static vhs_fm_decoder_t *end_frame_panel[2];
static uint64_t end_frame_generation[2];
static int transport_ld_audio_channel = -1;
static playback_rf_end_t transport_rf_end[2];
bool gui_rf_preview_worker_finish_ld_audio(gui_rf_preview_worker_t *w) {
    assert(w);
    ++finish_worker_calls;
    return finish_worker_ready;
}
bool gui_rf_preview_worker_finish_ld_video(gui_rf_preview_worker_t *w) {
    assert(w);
    ++finish_video_calls;
    return w != finish_video_pending;
}
bool gui_rf_preview_worker_take_frame(gui_rf_preview_worker_t *worker,
                                      uint8_t *rgba, size_t capacity, uint64_t *generation,
                                      int *width, int *height, gui_rf_preview_stats_t *stats) {
    (void)stats;
    for (int ch = 0; ch < 2; ++ch) {
        if (end_frame_panel[ch] && worker == end_frame_panel[ch]->worker &&
            *generation != end_frame_generation[ch]) {
            assert(rgba && capacity >= 4);
            *generation = end_frame_generation[ch];
            *width = *height = 1;
            rgba[0] = (uint8_t)*generation;
            return true;
        }
    }
    return false;
}
bool gui_rf_preview_worker_take_ld_frame(gui_rf_preview_worker_t *worker,
                                         uint8_t *rgba, size_t capacity, uint64_t *generation,
                                         int *width, int *height, gui_rf_preview_stats_t *stats,
                                         bool clock_valid, uint64_t audio_sample) {
    (void)clock_valid;
    (void)audio_sample;
    return gui_rf_preview_worker_take_frame(worker, rgba, capacity, generation, width, height, stats);
}
bool gui_audio_get_ld_position(const void *source, uint64_t *sample) {
    (void)source;
    (void)sample;
    return false;
}
bool gui_rf_preview_worker_ld_dts_available(void) { return ld_dts_available; }
bool gui_rf_preview_worker_set_ld_audio(gui_rf_preview_worker_t *w,gui_rf_ld_audio_t mode) {
    assert(w && gui_rf_profile_is_laserdisc(((route_worker_t*)w)->profile));
    bool changed = ((route_worker_t*)w)->ld_audio != mode;
    ((route_worker_t*)w)->ld_audio=mode;
    return changed;
}
void gui_rf_preview_worker_set_ld_sync(gui_rf_preview_worker_t *w, bool enabled) {
    assert(w && gui_rf_profile_is_laserdisc(((route_worker_t*)w)->profile));
    ((route_worker_t*)w)->ld_sync = enabled;
}
static bool monitor_burst_audio, monitor_ld_sync, monitor_output_unavailable;
static gui_audio_rf_mode_t monitor_rf_mode;
bool gui_audio_rf_output_unavailable(void) { return monitor_output_unavailable; }
bool gui_audio_finish_rf_source(const void *source) {
    assert(source == monitor_source);
    ++finish_monitor_calls;
    return finish_monitor_ready;
}
void gui_audio_set_rf_source(const void *source, bool burst_audio, gui_audio_rf_mode_t mode) {
    monitor_burst_audio = burst_audio;
    monitor_ld_sync = mode == GUI_AUDIO_RF_LD_TIMED;
    monitor_rf_mode = mode;
    monitor_source = source;
}
void gui_audio_set_hifi_source(const void *source) {
    gui_audio_set_rf_source(source, false, GUI_AUDIO_RF_HIFI);
}
bool gui_playback_is_running(gui_app_t *app) {
    assert(app==&route_app);
    return transport_running;
}
playback_speed_t gui_playback_get_speed(gui_app_t *app) {
    assert(app == &route_app);
    return transport_speed;
}
playback_rf_end_t gui_playback_get_channel_rf_end(gui_app_t *app,int channel, uint64_t *token) {
    assert(app == &route_app && channel >= 0 && channel < 2);
    if (token) *token = transport_rf_end[channel] != PLAYBACK_RF_END_NONE ? 1 : 0;
    return transport_rf_end[channel];
}
void gui_playback_set_ld_audio_channel(gui_app_t *app, int channel) {
    assert(app == &route_app && channel >= -1 && channel < 2);
    transport_ld_audio_channel = channel;
}
void gui_playback_set_ld_video_channels(gui_app_t *app, unsigned channels) {
    assert(app == &route_app && channels <= 3);
    transport_ld_video_channels = channels;
}
void gui_playback_finish_ld_tail(gui_app_t *app, int channel, uint64_t token) {
    assert(app == &route_app && token == 1 && (transport_ld_video_channels & (1u << channel)) &&
           transport_rf_end[channel] == PLAYBACK_RF_END_READY);
    if (end_frame_panel[channel]) {
        const vhs_fm_decoder_t *panel = end_frame_panel[channel];
        assert(panel->ld_tail_latched && panel->texture_dirty);
        assert(panel->generation == end_frame_generation[channel]);
        // Simulate the new lap reaching the worker before the next draw.
        end_frame_generation[channel]++;
    }
    transport_rf_end[channel] = PLAYBACK_RF_END_NONE;
    ++finish_transport_calls;
}
bool gui_playback_is_channel_paused(gui_app_t *app,int channel) {
    assert(app==&route_app && channel>=0 && channel<2);
    return transport_paused[channel];
}
bool gui_rf_preview_worker_audio_is_current(const void *worker) { return worker != NULL; }
bool gui_rf_preview_worker_audio_is_ld(const void *worker) {
    return worker && gui_rf_profile_is_laserdisc(((const route_worker_t *)worker)->profile);
}
void gui_audio_push_ld(const void *source, const float *pcm, size_t frames,
                       uint64_t end_sample, bool (*current)(const void *source)) {
    (void)end_sample;
    assert(source && pcm && frames && current(source));
}
void gui_audio_push_rf_checked(const void *source, const float *pcm, size_t frames,
                               uint64_t end_sample, bool (*current)(const void *source)) {
    (void)end_sample;
    assert(source && pcm && frames && current(source));
}
void gui_audio_reset_hifi(const void *source) { assert(source); ++monitor_resets; }
void gui_rf_preview_worker_destroy(gui_rf_preview_worker_t *worker) {
    assert(worker); atomic_fetch_add(&destroys,1); free(worker);
}
bool gui_rf_preview_worker_set_budget(double cores) { assert(cores>=.5 && cores<=8); return true; }
void gui_rf_preview_worker_set_paused(gui_rf_preview_worker_t *worker,bool paused) {
    atomic_store(&((route_worker_t*)worker)->paused,paused);
}
void gui_rf_preview_worker_reset(gui_rf_preview_worker_t *worker) { assert(worker); atomic_fetch_add(&resets,1); }
bool gui_vhs_fm_bind_worker(vhs_fm_decoder_t *decoder,gui_rf_preview_worker_t *worker) {
    atomic_fetch_add(&binds,1); decoder->worker=worker; return true;
}
size_t bufmgr_fill_level(buffer_manager_t *manager,buffer_id_t id) { (void)manager; (void)id; return simulated_fill; }
bool gui_rf_preview_worker_submit(gui_rf_preview_worker_t *worker,const int16_t *samples,size_t count,const gui_rf_block_info_t *info) {
    assert(worker && count==2);
    assert((samples==route_a && info->sample_rate_hz==40000000 && info->start_sample==123456 && info->epoch==42) ||
           (samples==route_b && info->sample_rate_hz==20000000 && info->start_sample==654321 && info->epoch==43));
    if (seek_monitor_epoch_floor && info->discontinuity)
        assert(monitor_resets >= seek_monitor_epoch_floor);
    if (atomic_load(&((route_worker_t*)worker)->paused)) return false;
    atomic_fetch_add(&((route_worker_t*)worker)->submits,1);
    atomic_fetch_add(&route_calls,1); atomic_store(&copy_entered,true);
    while (atomic_load(&hold_copy)) thrd_sleep_ms(1);
    return true;
}
bool gui_rf_preview_worker_submit_with_audio_reset(gui_rf_preview_worker_t *worker,
                                                   const int16_t *samples, size_t count,
                                                   const gui_rf_block_info_t *info,
                                                   void (*reset_output)(const void *worker)) {
    if (reset_output) {
        assert(info->discontinuity);
        reset_output(worker);
    }
    return gui_rf_preview_worker_submit(worker,samples,count,info);
}
static const uint8_t route_iq[] = {95, 171, 201, 68};
static const gui_rf_block_info_t route_iq_info = {
    .sample_rate_hz = 1024000, .start_sample = UINT64_C(0x100000017),
    .epoch = 44, .discontinuity = true, .iq_center_hz = 1500000
};
static unsigned route_iq_calls;
bool gui_rf_preview_worker_submit_iq(gui_rf_preview_worker_t *worker, const uint8_t *iq,
                                     size_t count, const gui_rf_block_info_t *info,
                                     void (*reset_output)(const void *worker)) {
    assert(worker && count == 2 && memcmp(iq, route_iq, sizeof(route_iq)) == 0);
    assert(info->sample_rate_hz == route_iq_info.sample_rate_hz &&
           info->start_sample == route_iq_info.start_sample && info->epoch == route_iq_info.epoch &&
           info->iq_center_hz == route_iq_info.iq_center_hz && info->discontinuity);
    assert(reset_output == gui_audio_reset_hifi);
    reset_output(worker);
    assert(!atomic_load(&((route_worker_t *)worker)->paused));
    atomic_fetch_add(&((route_worker_t *)worker)->submits, 1);
    ++route_iq_calls;
    return true;
}
static int route_producer(void *unused) { (void)unused; gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); return 0; }
static int route_detach(void *unused) { (void)unused; video_detach_rf(detach_decoder); atomic_store(&detach_done,true); return 0; }
static video_panel_state_t *new_panel(void) {
    video_panel_state_t *panel=calloc(1,sizeof(*panel)); assert(panel);
    atomic_init(&panel->active_plugin,VIDEO_PLUGIN_CVBS); panel->vhs_fm.decoder.bound_channel=-1;
    return panel;
}
static void wait_flag(atomic_bool *flag) {
    uint64_t begin=get_time_ms(); while(!atomic_load(flag)) { if(get_time_ms()-begin>2000) exit(1); thrd_sleep_ms(1); }
}
static bool route_audio_enabled(const vhs_fm_decoder_t *decoder) {
    assert(decoder->worker);
    return atomic_load(&((route_worker_t*)decoder->worker)->audio_enabled);
}
static void test_secam_vhs_routes(bool mesecam,gui_rf_profile_t profile,
                                  vhs_fm_format_t format,const char *name) {
    _Static_assert(GUI_RF_PROFILE_PAL_VHS==0 && GUI_RF_PROFILE_NTSC_VHS==1 &&
        GUI_RF_PROFILE_NTSC_BETAMAX==2 && GUI_RF_PROFILE_NTSC_BETAMAX_HIFI==3 &&
        GUI_RF_PROFILE_NTSC_SUPERBETA==4 && GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI==5 &&
        GUI_RF_PROFILE_SECAM_VHS==6 && GUI_RF_PROFILE_MESECAM_VHS==7 &&
        GUI_RF_PROFILE_NTSC_ED_BETA==8 && GUI_RF_PROFILE_PAL_SVHS==9 &&
        GUI_RF_PROFILE_NTSC_SVHS==10 && GUI_RF_PROFILE_PAL_LASERDISC==11 &&
        GUI_RF_PROFILE_NTSC_LASERDISC==12 && GUI_RF_PROFILE_NTSC_VHS_HIFI==13 &&
        GUI_RF_PROFILE_PAL_VHS_HIFI==14 && GUI_RF_PROFILE_COUNT==15,
        "append RF profiles without renumbering existing IDs");
    video_panel_state_t left={0},right={0};
    assert(gui_vhs_fm_init(&left.vhs_fm.decoder));
    assert(gui_vhs_fm_init(&right.vhs_fm.decoder));
    vhs_fm_decoder_t *a=&left.vhs_fm.decoder,*b=&right.vhs_fm.decoder;
    assert(!a->overlay.secam_mesecam && !b->overlay.secam_mesecam);
    atomic_init(&left.active_plugin,VIDEO_PLUGIN_VHS_FM);
    atomic_init(&right.active_plugin,VIDEO_PLUGIN_VHS_FM);
    route_app.panel_config_a=(channel_panel_config_t){.left_view=PANEL_VIEW_CVBS,
        .right_view=PANEL_VIEW_CVBS,.left_state=&left,.right_state=&right,.split=true};
    route_app.panel_config_b=(channel_panel_config_t){.left_view=PANEL_VIEW_WAVEFORM,
        .right_view=PANEL_VIEW_WAVEFORM};
    route_app.is_capturing=true;route_app.settings.audio_monitor_playback=true;
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    const int system = mesecam ? 3 : 2;
    const int other_system = mesecam ? 2 : 3;
    gui_vhs_fm_set_format(a,system);gui_vhs_fm_set_format(b,system);
    /* A remembered Beta video preset cannot affect either SECAM VHS recording. */
    gui_vhs_fm_set_beta_preset(a,GUI_BETA_PRESET_ED_BETA);
    assert(gui_vhs_fm_get_profile(a)==profile);
    assert(gui_vhs_fm_get_format(a)==format);
    assert(!strcmp(gui_vhs_fm_get_format_name(a),name));
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+1 && a->worker==b->worker);
    assert(((route_worker_t*)a->worker)->format==GUI_RF_PAL && a->bound_system==0);
    assert(!monitor_source && !route_audio_enabled(a));
    gui_rf_preview_worker_t *secam_worker=a->worker;
    gui_vhs_fm_set_format(b,0);gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+2 && a->worker!=b->worker);
    assert(a->worker==secam_worker && ((route_worker_t*)b->worker)->format==GUI_RF_PAL);
    assert(((route_worker_t*)b->worker)->profile==GUI_RF_PROFILE_PAL_VHS);
    gui_vhs_fm_set_format(b,system);gui_video_update_rf_routes(&route_app);
    assert(a->worker==secam_worker && b->worker==secam_worker);
    assert(atomic_load(&destroys)==destroyed+1);
    int calls=atomic_load(&route_calls),reset_count=atomic_load(&resets);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==calls+1); /* One submit for shared SECAM views. */
    gui_vhs_fm_reset(a);assert(atomic_load(&resets)==reset_count+1);
    gui_rf_block_info_t gap[2]={route_info[0],route_info[1]};gap[0].discontinuity=true;
    gui_video_submit_rf(&route_app,NULL,NULL,0,0,gap,true);
    assert(atomic_load(&resets)==reset_count+2);
    /* Betamax continues to force NTSC even when the prior VHS pick was SECAM. */
    b->overlay.selected_tape=1;gui_vhs_fm_set_format(b,system);
    assert(b->overlay.selected_system==1 && gui_vhs_fm_get_format(b)==VHS_FM_FORMAT_NTSC);
    assert(gui_vhs_fm_get_profile(b)==GUI_RF_PROFILE_NTSC_BETAMAX);
    gui_video_update_rf_routes(&route_app);
    assert(a->worker==secam_worker && ((route_worker_t*)b->worker)->format==GUI_RF_NTSC);
    b->overlay.selected_tape=0;gui_vhs_fm_set_format(b,-1);
    assert(gui_vhs_fm_get_profile(b)==GUI_RF_PROFILE_PAL_VHS);
    /* Native SECAM and MESECAM share timing, never their worker identity. */
    gui_vhs_fm_set_format(b,other_system);
    gui_video_update_rf_routes(&route_app);
    assert(a->worker==secam_worker && b->worker!=secam_worker);
    assert(((route_worker_t*)b->worker)->format==GUI_RF_PAL);
    assert(((route_worker_t*)b->worker)->profile==
        (mesecam?GUI_RF_PROFILE_SECAM_VHS:GUI_RF_PROFILE_MESECAM_VHS));
    assert(a->overlay.selected_system==system && b->overlay.selected_system==other_system);
    calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==calls+2);
    gui_vhs_fm_set_format(b,system);gui_video_update_rf_routes(&route_app);
    assert(b->worker==secam_worker);
    video_detach_rf(a);assert(b->worker==secam_worker);
    video_detach_rf(b);assert(!monitor_source);
    assert(atomic_load(&creates)-created==atomic_load(&destroys)-destroyed);
    for(int index=0;index<GUI_RF_PROFILE_COUNT;index++) assert(!atomic_load(&s_rf_routes[0][index]));
}
// Use the production panel cache transitions with lightweight panel factories:
// the route fixtures below own their video states and need no render resources.
static void *cache_waveform_create(void) { void *state=calloc(1,1); assert(state); return state; }
static void cache_video_destroy(void *state) { video_detach_rf(&((video_panel_state_t*)state)->vhs_fm.decoder); }
const panel_vtable_t *panel_get_vtable(panel_view_type_t type) {
    static const panel_vtable_t waveform={.create=cache_waveform_create,.destroy=free};
    static const panel_vtable_t video={.destroy=cache_video_destroy};
    return type==PANEL_VIEW_WAVEFORM ? &waveform : type==PANEL_VIEW_CVBS ? &video : NULL;
}
static void test_secam_recording_selector(void) {
    vhs_fm_decoder_t d;
    assert(gui_vhs_fm_init(&d));
    assert(!d.overlay.secam_mesecam);
    gui_vhs_fm_set_format(&d,2);
    assert(gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_SECAM_VHS);
    assert(gui_vhs_fm_get_format(&d)==VHS_FM_FORMAT_SECAM);
    gui_vhs_fm_set_mesecam(&d,true);
    assert(d.overlay.selected_system==3 && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_MESECAM_VHS);
    assert(gui_vhs_fm_get_format(&d)==VHS_FM_FORMAT_MESECAM);
    assert(!strcmp(gui_vhs_fm_get_format_name(&d),"MESECAM"));
    gui_vhs_fm_set_format(&d,0);
    assert(d.overlay.secam_mesecam && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_PAL_VHS);
    assert(gui_vhs_fm_get_format(&d)==VHS_FM_FORMAT_PAL);
    gui_vhs_fm_set_format(&d,1);
    assert(d.overlay.secam_mesecam && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_NTSC_VHS);
    assert(gui_vhs_fm_get_format(&d)==VHS_FM_FORMAT_NTSC);
    d.overlay.selected_tape=1;
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_NTSC_BETAMAX,
        GUI_RF_PROFILE_NTSC_BETAMAX_HIFI,GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI,GUI_RF_PROFILE_NTSC_ED_BETA};
    for(int preset=0;preset<GUI_BETA_PRESET_COUNT;preset++) {
        gui_vhs_fm_set_beta_preset(&d,preset);
        gui_vhs_fm_set_format(&d,2);
        assert(d.overlay.selected_system==1 && d.overlay.secam_mesecam);
        assert(gui_vhs_fm_get_format(&d)==VHS_FM_FORMAT_NTSC);
        assert(gui_vhs_fm_get_profile(&d)==profiles[preset]);
    }
    d.overlay.selected_tape=0;gui_vhs_fm_set_format(&d,3);
    assert(d.overlay.beta_preset==GUI_BETA_PRESET_ED_BETA &&
        gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_MESECAM_VHS);
    gui_vhs_fm_set_mesecam(&d,false);
    assert(d.overlay.selected_system==2 && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_SECAM_VHS);
    assert(gui_vhs_fm_get_format(&d)==VHS_FM_FORMAT_SECAM);
    assert(!strcmp(gui_vhs_fm_get_format_name(&d),"SECAM"));
    gui_vhs_fm_set_format(&d,4); /* Invalid system selection falls back to PAL. */
    assert(d.overlay.selected_system==0 && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_PAL_VHS);
}
static void test_vhs_preset_selector(void) {
    _Static_assert(GUI_VHS_PRESET_VHS==0 && GUI_VHS_PRESET_SVHS==1 &&
                   GUI_VHS_PRESET_HIFI==2 && GUI_VHS_PRESET_COUNT==3,
                   "VHS presets append without changing existing IDs");
    vhs_fm_decoder_t d;
    assert(gui_vhs_fm_init(&d));
    assert(d.overlay.vhs_preset==GUI_VHS_PRESET_VHS);
    assert(!strcmp(gui_vhs_fm_get_vhs_preset_name(&d),"VHS"));
    gui_vhs_fm_set_vhs_preset(&d,GUI_VHS_PRESET_SVHS);
    assert(gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_PAL_SVHS);
    assert(d.format==VHS_FM_FORMAT_PAL && !strcmp(gui_vhs_fm_get_vhs_preset_name(&d),"S-VHS"));
    gui_vhs_fm_cycle_system(&d);
    assert(gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_NTSC_SVHS);
    assert(d.format==VHS_FM_FORMAT_NTSC);
    gui_vhs_fm_cycle_system(&d);
    assert(d.overlay.vhs_preset==GUI_VHS_PRESET_SVHS);
    assert(gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_PAL_SVHS);
    gui_vhs_fm_cycle_system(&d);
    d.overlay.selected_tape=1;
    gui_vhs_fm_set_beta_preset(&d,GUI_BETA_PRESET_ED_BETA);
    assert(d.overlay.vhs_preset==GUI_VHS_PRESET_SVHS && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_NTSC_ED_BETA);
    gui_vhs_fm_set_vhs_preset(&d,GUI_VHS_PRESET_VHS);
    assert(d.overlay.beta_preset==GUI_BETA_PRESET_ED_BETA && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_NTSC_ED_BETA);
    gui_vhs_fm_set_vhs_preset(&d,GUI_VHS_PRESET_SVHS);
    d.overlay.selected_tape=0;
    assert(gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_NTSC_SVHS);
    for(int mesecam=0;mesecam<2;mesecam++) {
        gui_vhs_fm_set_mesecam(&d,mesecam!=0);
        gui_vhs_fm_set_format(&d,mesecam ? 3 : 2);
        assert(d.overlay.vhs_preset==GUI_VHS_PRESET_VHS);
        assert(gui_vhs_fm_get_profile(&d)==(mesecam?GUI_RF_PROFILE_MESECAM_VHS:GUI_RF_PROFILE_SECAM_VHS));
        gui_vhs_fm_set_vhs_preset(&d,GUI_VHS_PRESET_SVHS);
        assert(d.overlay.selected_system==0 && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_PAL_SVHS);
        assert(d.format==VHS_FM_FORMAT_PAL && d.overlay.secam_mesecam==(mesecam!=0));
    }
    const int invalid[]={-1,GUI_VHS_PRESET_COUNT,999};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        gui_vhs_fm_set_vhs_preset(&d,invalid[i]);
        assert(d.overlay.vhs_preset==GUI_VHS_PRESET_VHS && gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_PAL_VHS);
    }
    gui_vhs_fm_set_vhs_preset(NULL,GUI_VHS_PRESET_SVHS);
    assert(!strcmp(gui_vhs_fm_get_vhs_preset_name(NULL),"VHS"));
    gui_vhs_fm_set_format(&d, 2);
    gui_vhs_fm_set_vhs_preset(&d, GUI_VHS_PRESET_HIFI);
    assert(d.overlay.selected_system == 2 && gui_vhs_fm_get_profile(&d) == GUI_RF_PROFILE_PAL_VHS_HIFI);
    gui_vhs_fm_set_vhs_preset(&d, GUI_VHS_PRESET_VHS);
    assert(gui_vhs_fm_get_profile(&d) == GUI_RF_PROFILE_SECAM_VHS);
}
static void test_svhs_routes(void) {
    video_panel_state_t left={0},right={0};
    assert(gui_vhs_fm_init(&left.vhs_fm.decoder) && gui_vhs_fm_init(&right.vhs_fm.decoder));
    atomic_init(&left.active_plugin,VIDEO_PLUGIN_VHS_FM);
    atomic_init(&right.active_plugin,VIDEO_PLUGIN_VHS_FM);
    vhs_fm_decoder_t *a=&left.vhs_fm.decoder,*b=&right.vhs_fm.decoder;
    route_app.panel_config_a=(channel_panel_config_t){.left_view=PANEL_VIEW_CVBS,
        .right_view=PANEL_VIEW_CVBS,.left_state=&left,.right_state=&right,.split=true};
    route_app.panel_config_b=(channel_panel_config_t){.left_view=PANEL_VIEW_WAVEFORM,.right_view=PANEL_VIEW_WAVEFORM};
    route_app.is_capturing=true;route_app.settings.audio_monitor_playback=true;
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    gui_vhs_fm_set_vhs_preset(a,GUI_VHS_PRESET_SVHS);
    gui_vhs_fm_set_vhs_preset(b,GUI_VHS_PRESET_SVHS);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+1 && a->worker==b->worker);
    gui_rf_preview_worker_t *pal_svhs=a->worker;
    assert(((route_worker_t*)pal_svhs)->profile==GUI_RF_PROFILE_PAL_SVHS);
    assert(((route_worker_t*)pal_svhs)->format==GUI_RF_PAL && !monitor_source && !route_audio_enabled(a));
    gui_vhs_fm_set_format(b,1);gui_video_update_rf_routes(&route_app);
    assert(a->worker==pal_svhs && a->worker!=b->worker);
    assert(((route_worker_t*)b->worker)->profile==GUI_RF_PROFILE_NTSC_SVHS && b->bound_system==GUI_RF_NTSC);
    gui_vhs_fm_set_vhs_preset(b,GUI_VHS_PRESET_VHS);gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)b->worker)->profile==GUI_RF_PROFILE_NTSC_VHS && a->worker==pal_svhs);
    gui_vhs_fm_set_format(b,0);gui_vhs_fm_set_vhs_preset(b,GUI_VHS_PRESET_SVHS);
    gui_video_update_rf_routes(&route_app);
    assert(a->worker==pal_svhs && b->worker==pal_svhs);
    b->overlay.selected_tape=1;gui_vhs_fm_set_beta_preset(b,GUI_BETA_PRESET_ED_BETA);
    gui_video_update_rf_routes(&route_app);
    assert(b->worker!=pal_svhs && ((route_worker_t*)b->worker)->profile==GUI_RF_PROFILE_NTSC_ED_BETA);
    b->overlay.selected_tape=0;gui_video_update_rf_routes(&route_app);
    assert(b->overlay.vhs_preset==GUI_VHS_PRESET_SVHS && b->worker==pal_svhs);
    int calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==calls+1);
    gui_vhs_fm_set_vhs_preset(b, GUI_VHS_PRESET_HIFI);
    gui_video_update_rf_routes(&route_app);
    assert(a->worker == pal_svhs && b->worker != pal_svhs);
    assert(b->format == VHS_FM_FORMAT_PAL && !strcmp(gui_vhs_fm_get_vhs_preset_name(b), "Hi-Fi"));
    assert(((route_worker_t *)b->worker)->profile == GUI_RF_PROFILE_PAL_VHS_HIFI);
    assert(monitor_source == b->worker && route_audio_enabled(b));
    gui_vhs_fm_set_format(b, 1);
    gui_video_update_rf_routes(&route_app);
    assert(a->worker == pal_svhs && b->format == VHS_FM_FORMAT_NTSC);
    assert(((route_worker_t *)b->worker)->profile == GUI_RF_PROFILE_NTSC_VHS_HIFI);
    assert(monitor_source == b->worker && route_audio_enabled(b));
    transport_running = true;
    transport_paused[0] = true;
    gui_video_update_rf_routes(&route_app);
    assert(!route_audio_enabled(b));
    transport_running = transport_paused[0] = false;
    gui_vhs_fm_set_format(b, 0);
    gui_vhs_fm_set_vhs_preset(b, GUI_VHS_PRESET_SVHS);
    gui_video_update_rf_routes(&route_app);
    assert(b->worker == pal_svhs && b->format == VHS_FM_FORMAT_PAL);
    video_detach_rf(a);video_detach_rf(b);
    assert(!monitor_source && atomic_load(&creates)-created==atomic_load(&destroys)-destroyed);
    for(int index=0;index<GUI_RF_PROFILE_COUNT;index++) assert(!atomic_load(&s_rf_routes[0][index]));
}
static void test_rf_audio_playback_priority(void) {
    video_panel_state_t panel_a = {0}, panel_b = {0};
    vhs_fm_decoder_t *a = &panel_a.vhs_fm.decoder, *b = &panel_b.vhs_fm.decoder;
    assert(gui_vhs_fm_init(a) && gui_vhs_fm_init(b));
    atomic_init(&panel_a.active_plugin, VIDEO_PLUGIN_VHS_FM);
    atomic_init(&panel_b.active_plugin, VIDEO_PLUGIN_VHS_FM);
    gui_vhs_fm_set_vhs_preset(a, GUI_VHS_PRESET_HIFI);
    gui_vhs_fm_set_vhs_preset(b, GUI_VHS_PRESET_HIFI);
    gui_vhs_fm_set_format(a, 1);
    route_app.panel_config_a = (channel_panel_config_t){.left_view=PANEL_VIEW_CVBS, .left_state=&panel_a};
    route_app.panel_config_b = (channel_panel_config_t){.left_view=PANEL_VIEW_CVBS, .left_state=&panel_b};
    route_app.is_capturing = true;
    route_app.settings.audio_monitor_playback = true;
    transport_running = true;

    gui_video_update_rf_routes(&route_app);
    gui_rf_preview_worker_t *worker_a = a->worker, *worker_b = b->worker;
    assert(worker_a && worker_b && worker_a != worker_b);

    // An unpaused A panel with no file must not mask a playing B file.
    atomic_store(&route_app.rf_source_rate_hz[0], 0);
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_b && route_audio_enabled(b) && !route_audio_enabled(a));
    atomic_store(&route_app.rf_source_rate_hz[1], 0);
    gui_video_update_rf_routes(&route_app);
    assert(!monitor_source && !route_audio_enabled(a) && !route_audio_enabled(b));
    atomic_store(&route_app.rf_source_rate_hz[0], 12000000);
    atomic_store(&route_app.rf_source_rate_hz[1], 8000000);
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_b && route_audio_enabled(b) && !route_audio_enabled(a));
    atomic_store(&route_app.rf_source_rate_hz[0], 40000000);
    atomic_store(&route_app.rf_source_rate_hz[1], 40000000);

    const struct { bool pause_a, pause_b; const void *source; } cases[] = {
        {false, false, worker_a},
        {true, false, worker_b},
        {true, true, worker_a},
        {false, true, worker_a},
        {false, false, worker_a}
    };
    for (size_t i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
        transport_paused[0] = cases[i].pause_a;
        transport_paused[1] = cases[i].pause_b;
        gui_video_update_rf_routes(&route_app);
        assert(a->worker == worker_a && b->worker == worker_b);
        assert(monitor_source == cases[i].source);
        assert(route_audio_enabled(a) == (monitor_source == worker_a && !transport_paused[0]));
        assert(route_audio_enabled(b) == (monitor_source == worker_b && !transport_paused[1]));
    }
    // Natural LD EOF must keep A selected through RF, decoder, and monitor
    // drain. Manual pause in the cases above still hands off immediately.
    a->overlay.selected_tape = b->overlay.selected_tape = GUI_RF_FAMILY_LASERDISC;
    gui_vhs_fm_set_format(a, 1);
    gui_vhs_fm_set_format(b, 1);
    gui_video_update_rf_routes(&route_app);
    worker_a = a->worker;
    worker_b = b->worker;
    assert(worker_a && worker_b && monitor_source == worker_a);
    unsigned worker_calls = finish_worker_calls, monitor_calls = finish_monitor_calls;
    transport_paused[0] = true;
    transport_rf_end[0] = PLAYBACK_RF_END_PENDING;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && finish_worker_calls == worker_calls);
    transport_rf_end[0] = PLAYBACK_RF_END_READY;
    finish_worker_ready = false;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && finish_worker_calls == worker_calls + 1 &&
           finish_monitor_calls == monitor_calls);
    finish_worker_ready = true;
    finish_monitor_ready = false;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && finish_monitor_calls == monitor_calls + 1 &&
           route_audio_enabled(a) && monitor_ld_sync);
    finish_monitor_ready = true;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_b && !route_audio_enabled(a) && route_audio_enabled(b));
    transport_paused[1] = true;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && !route_audio_enabled(a));
    monitor_calls = finish_monitor_calls;
    gui_video_update_rf_routes(&route_app);
    assert(!route_audio_enabled(a) && finish_monitor_calls == monitor_calls); // EOF is consumed.
    transport_paused[0] = transport_paused[1] = false;
    transport_rf_end[0] = PLAYBACK_RF_END_NONE;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && transport_ld_audio_channel == 0);

    // A looping source stays logically playing, yet must wait for both drains.
    unsigned transport_calls = finish_transport_calls;
    transport_rf_end[0] = PLAYBACK_RF_END_PENDING;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && finish_transport_calls == transport_calls);
    transport_rf_end[0] = PLAYBACK_RF_END_READY;
    finish_monitor_ready = false;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && finish_transport_calls == transport_calls);
    finish_monitor_ready = true;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == worker_a && route_audio_enabled(a) &&
           finish_transport_calls == transport_calls + 1);
    worker_calls = finish_worker_calls;
    monitor_output_unavailable = true;
    gui_video_update_rf_routes(&route_app);
    assert(transport_ld_audio_channel == -1 && finish_worker_calls == worker_calls &&
           finish_transport_calls == transport_calls + 1);
    monitor_output_unavailable = false;

    // A normally has priority, but resuming it must not cut B's audible
    // tail. Check both a paused natural end and a still-playing loop.
    for (int natural_end = 0; natural_end < 2; ++natural_end) {
        transport_paused[0] = true;
        gui_video_update_rf_routes(&route_app);
        assert(monitor_source == worker_b && transport_ld_audio_channel == 1);
        transport_paused[1] = natural_end != 0;
        transport_rf_end[1] = PLAYBACK_RF_END_PENDING;
        transport_paused[0] = false;
        transport_calls = finish_transport_calls;
        monitor_calls = finish_monitor_calls;
        gui_video_update_rf_routes(&route_app);
        assert(monitor_source == worker_b && route_audio_enabled(b) && !route_audio_enabled(a));
        assert(finish_transport_calls == transport_calls && finish_monitor_calls == monitor_calls);
        transport_rf_end[1] = PLAYBACK_RF_END_READY;
        finish_worker_ready = false;
        gui_video_update_rf_routes(&route_app);
        assert(monitor_source == worker_b && finish_monitor_calls == monitor_calls);
        finish_worker_ready = true;
        finish_monitor_ready = false;
        gui_video_update_rf_routes(&route_app);
        assert(monitor_source == worker_b && finish_transport_calls == transport_calls &&
               finish_monitor_calls == monitor_calls + 1);
        finish_monitor_ready = true;
        gui_video_update_rf_routes(&route_app);
        assert(monitor_source == worker_a && route_audio_enabled(a) && !route_audio_enabled(b));
        assert(finish_transport_calls == transport_calls + 1 && transport_rf_end[1] == PLAYBACK_RF_END_NONE);
        transport_paused[1] = false;
    }

    // Both visible LD workers still finish picture RF when audio is muted.
    // An outstanding B video tail must not stop the completed A ACK.
    // The GUI must also retain A's last pixels if rewind beats the draw.
    uint8_t rgba[2][4] = {0};
    a->rgba = rgba[0];
    b->rgba = rgba[1];
    end_frame_panel[0] = a;
    end_frame_panel[1] = b;
    end_frame_generation[0] = 42;
    end_frame_generation[1] = 84;
    route_app.settings.audio_monitor_playback = false;
    transport_rf_end[0] = transport_rf_end[1] = PLAYBACK_RF_END_READY;
    finish_video_pending = worker_b;
    unsigned video_calls = finish_video_calls;
    transport_calls = finish_transport_calls;
    worker_calls = finish_worker_calls;
    gui_video_update_rf_routes(&route_app);
    assert(transport_ld_video_channels == 3 && transport_ld_audio_channel == -1);
    assert(finish_video_calls == video_calls + 2 && finish_worker_calls == worker_calls);
    assert(finish_transport_calls == transport_calls + 1 &&
           transport_rf_end[0] == PLAYBACK_RF_END_NONE &&
           transport_rf_end[1] == PLAYBACK_RF_END_READY);
    gui_vhs_fm_swap_buffers(a);
    assert(a->generation == 42 && rgba[0][0] == 42 && end_frame_generation[0] == 43);
    assert(!b->ld_tail_latched);
    finish_video_pending = NULL;
    gui_video_update_rf_routes(&route_app);
    assert(finish_video_calls == video_calls + 3 && finish_worker_calls == worker_calls &&
           finish_transport_calls == transport_calls + 2);
    gui_vhs_fm_swap_buffers(b);
    assert(b->generation == 84 && rgba[1][0] == 84 && end_frame_generation[1] == 85);
    end_frame_panel[0] = end_frame_panel[1] = NULL;
    route_app.settings.audio_monitor_playback = true;
    transport_running = false;
    video_detach_rf(a);
    video_detach_rf(b);
    assert(!monitor_source);
}
static void test_laserdisc_routes(void) {
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_ANALOG, false) == GUI_RF_LD_AUDIO_DIGITAL);
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_DIGITAL, false) == GUI_RF_LD_AUDIO_DTS);
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_DIGITAL, true) == GUI_RF_LD_AUDIO_DTS);
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_DTS, false) == GUI_RF_LD_AUDIO_ANALOG);
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_DTS, true) == GUI_RF_LD_AUDIO_AC3);
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_AC3, true) == GUI_RF_LD_AUDIO_ANALOG);
    ld_dts_available = false;
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_DIGITAL, false) == GUI_RF_LD_AUDIO_ANALOG);
    assert(vhs_next_ld_audio(GUI_RF_LD_AUDIO_DIGITAL, true) == GUI_RF_LD_AUDIO_AC3);
    ld_dts_available = true;

    _Static_assert(GUI_RF_FAMILY_VHS==0 && GUI_RF_FAMILY_BETAMAX==1 &&
        GUI_RF_FAMILY_LASERDISC==2 && GUI_RF_FAMILY_COUNT==3,
        "append LaserDisc to the existing Format choices");
    video_panel_state_t left={0},right={0};
    vhs_fm_decoder_t *a=&left.vhs_fm.decoder,*b=&right.vhs_fm.decoder;
    assert(gui_vhs_fm_init(a) && gui_vhs_fm_init(b));
    assert(a->overlay.selected_tape==GUI_RF_FAMILY_VHS);
    atomic_init(&left.active_plugin,VIDEO_PLUGIN_VHS_FM);
    atomic_init(&right.active_plugin,VIDEO_PLUGIN_VHS_FM);
    route_app.panel_config_a=(channel_panel_config_t){.left_view=PANEL_VIEW_CVBS,
        .right_view=PANEL_VIEW_CVBS,.left_state=&left,.right_state=&right,.split=true};
    route_app.panel_config_b=(channel_panel_config_t){.left_view=PANEL_VIEW_WAVEFORM};
    route_app.is_capturing=true;route_app.settings.audio_monitor_playback=true;
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);

    gui_vhs_fm_set_vhs_preset(b,GUI_VHS_PRESET_SVHS);
    a->overlay.selected_tape=GUI_RF_FAMILY_LASERDISC;
    b->overlay.selected_tape=GUI_RF_FAMILY_LASERDISC;
    gui_vhs_fm_set_format(a,3); /* A tape-only system falls back to PAL. */
    gui_vhs_fm_set_format(b,1);
    assert(a->overlay.selected_system==0 && a->format==VHS_FM_FORMAT_PAL);
    assert(b->overlay.selected_system==1 && b->format==VHS_FM_FORMAT_NTSC);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+2 && a->worker && b->worker && a->worker!=b->worker);
    assert(((route_worker_t*)a->worker)->profile==GUI_RF_PROFILE_PAL_LASERDISC && a->bound_system==GUI_RF_PAL);
    assert(((route_worker_t*)b->worker)->profile==GUI_RF_PROFILE_NTSC_LASERDISC && b->bound_system==GUI_RF_NTSC);
    assert(monitor_source==a->worker && route_audio_enabled(a) && !route_audio_enabled(b));
    assert(monitor_rf_mode == GUI_AUDIO_RF_HIFI && !((route_worker_t*)a->worker)->ld_sync);
    gui_rf_preview_worker_t *pal_worker=a->worker;
    unsigned previous_resets = monitor_resets;
    a->overlay.ld_audio=GUI_RF_LD_AUDIO_DIGITAL;
    gui_video_update_rf_routes(&route_app);
    assert(a->worker==pal_worker && monitor_source==pal_worker);
    assert(((route_worker_t*)pal_worker)->ld_audio==GUI_RF_LD_AUDIO_DIGITAL);
    assert(monitor_resets == previous_resets + 1);
    a->overlay.ld_audio = GUI_RF_LD_AUDIO_DTS;
    gui_video_update_rf_routes(&route_app);
    assert(a->worker == pal_worker && monitor_source == pal_worker);
    assert(((route_worker_t *)pal_worker)->ld_audio == GUI_RF_LD_AUDIO_DTS && !monitor_burst_audio);
    a->overlay.ld_audio = GUI_RF_LD_AUDIO_DIGITAL;
    gui_video_update_rf_routes(&route_app);
    route_app.settings.audio_monitor_playback=false;
    gui_video_update_rf_routes(&route_app);
    assert(!route_audio_enabled(a));
    route_app.settings.audio_monitor_playback=true;
    transport_running=true;
    transport_paused[0]=true;
    gui_video_update_rf_routes(&route_app);
    assert(!route_audio_enabled(a));
    transport_paused[0]=false;
    gui_video_update_rf_routes(&route_app);
    assert(route_audio_enabled(a));
    assert(monitor_ld_sync && ((route_worker_t*)a->worker)->ld_sync);
    assert(!((route_worker_t*)b->worker)->ld_sync);
    transport_speed = PLAYBACK_SPEED_2X;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_rf_mode == GUI_AUDIO_RF_LD && !((route_worker_t*)a->worker)->ld_sync);
    transport_speed = PLAYBACK_SPEED_0_5X;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_rf_mode == GUI_AUDIO_RF_LD && !((route_worker_t*)a->worker)->ld_sync);
    transport_speed = PLAYBACK_SPEED_1X;
    transport_running=false;
    int calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==calls+2);
    gui_rf_block_info_t seek_info[2] = {route_info[0], route_info[1]};
    seek_info[0].discontinuity = true;
    seek_monitor_epoch_floor = monitor_resets + 1;
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,seek_info,true);
    assert(atomic_load(&route_calls)==calls+4);
    seek_monitor_epoch_floor = 0;

    gui_vhs_fm_set_format(b,0);gui_video_update_rf_routes(&route_app);
    assert(a->worker && a->worker==b->worker && atomic_load(&destroys)==destroyed+1);
    assert(b->overlay.ld_audio==GUI_RF_LD_AUDIO_DIGITAL);
    b->overlay.selected_tape=GUI_RF_FAMILY_VHS;
    gui_vhs_fm_set_format(b,0);gui_video_update_rf_routes(&route_app);
    assert(b->overlay.vhs_preset==GUI_VHS_PRESET_SVHS);
    assert(a->worker!=b->worker && ((route_worker_t*)b->worker)->profile==GUI_RF_PROFILE_PAL_SVHS);

    gui_vhs_fm_set_format(a,1);
    gui_video_update_rf_routes(&route_app);
    gui_rf_preview_worker_t *ntsc_worker = a->worker;
    a->overlay.ld_audio = GUI_RF_LD_AUDIO_DTS;
    gui_video_update_rf_routes(&route_app);
    assert(a->worker == ntsc_worker && monitor_source == ntsc_worker);
    assert(((route_worker_t *)ntsc_worker)->ld_audio == GUI_RF_LD_AUDIO_DTS && !monitor_burst_audio);
    a->overlay.ld_audio = GUI_RF_LD_AUDIO_AC3;
    gui_video_update_rf_routes(&route_app);
    assert(a->worker == ntsc_worker && monitor_source == ntsc_worker);
    assert(((route_worker_t *)ntsc_worker)->ld_audio == GUI_RF_LD_AUDIO_AC3);
    assert(monitor_burst_audio);
    gui_vhs_fm_set_format(a,0);
    gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t *)a->worker)->ld_audio == GUI_RF_LD_AUDIO_ANALOG);
    assert(!monitor_burst_audio);
    video_detach_rf(a);video_detach_rf(b);
    assert(atomic_load(&creates)-created==atomic_load(&destroys)-destroyed);
}
static void test_cached_mesecam_panel(void) {
    video_panel_state_t panel={0};
    vhs_fm_decoder_t *d=&panel.vhs_fm.decoder;
    assert(gui_vhs_fm_init(d));
    atomic_init(&panel.active_plugin,VIDEO_PLUGIN_VHS_FM);
    gui_vhs_fm_set_format(d,2);gui_vhs_fm_set_mesecam(d,true);
    channel_panel_config_t *config=&route_app.panel_config_a;
    *config=(channel_panel_config_t){.left_view=PANEL_VIEW_CVBS,.left_state=&panel,
        .left_cached_view=PANEL_VIEW_COUNT,.right_cached_view=PANEL_VIEW_COUNT};
    route_app.panel_config_b=(channel_panel_config_t){.left_view=PANEL_VIEW_WAVEFORM,
        .right_view=PANEL_VIEW_WAVEFORM};
    route_app.is_capturing=true;
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_MESECAM_VHS);
    panel_config_set_left_view(config,PANEL_VIEW_WAVEFORM);
    assert(config->left_cached_state==&panel);
    gui_video_update_rf_routes(&route_app);
    assert(!d->worker && d->overlay.selected_system==3 && d->overlay.secam_mesecam);
    panel_config_set_left_view(config,PANEL_VIEW_CVBS);
    assert(config->left_state==&panel);
    gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_MESECAM_VHS);
    assert(d->overlay.selected_system==3 && d->overlay.secam_mesecam);
    gui_vhs_fm_set_mesecam(d,false);gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_SECAM_VHS);
    panel_config_cleanup(config);
    assert(atomic_load(&creates)==created+3 && atomic_load(&destroys)==destroyed+3);
}
static void test_cached_hifi_panel_routes(void) {
    video_panel_state_t a={0},b={0};
    video_panel_state_t *panels[]={&a,&b};
    channel_panel_config_t *configs[]={&route_app.panel_config_a,&route_app.panel_config_b};
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    for(size_t i=0;i<2;i++) {
        atomic_init(&panels[i]->active_plugin,VIDEO_PLUGIN_VHS_FM);
        vhs_fm_decoder_t *d=&panels[i]->vhs_fm.decoder;
        d->bound_channel=-1;
        d->overlay.selected_tape=1;d->overlay.selected_system=1;
        gui_vhs_fm_set_beta_preset(d,GUI_BETA_PRESET_BETAMAX_HIFI);
        *configs[i]=(channel_panel_config_t){0};
        configs[i]->left_view=PANEL_VIEW_CVBS;configs[i]->left_state=panels[i];
        configs[i]->left_cached_view=configs[i]->right_cached_view=PANEL_VIEW_COUNT;
    }
    gui_vhs_fm_set_beta_preset(&a.vhs_fm.decoder,GUI_BETA_PRESET_SUPERBETA);
    route_app.is_capturing=true;route_app.settings.audio_monitor_playback=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+2 && monitor_source==a.vhs_fm.decoder.worker);
    assert(route_audio_enabled(&a.vhs_fm.decoder) && !route_audio_enabled(&b.vhs_fm.decoder));
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&((route_worker_t*)a.vhs_fm.decoder.worker)->submits)==1);
    assert(atomic_load(&((route_worker_t*)b.vhs_fm.decoder.worker)->submits)==1);

    // Switching to waveform parks the exact panel, but its RF/audio route must
    // detach. Channel B becomes audible and is the sole remaining submit target.
    panel_config_set_left_view(configs[0],PANEL_VIEW_WAVEFORM);
    assert(configs[0]->left_cached_state==&a && configs[0]->left_cached_view==PANEL_VIEW_CVBS);
    gui_video_update_rf_routes(&route_app);
    assert(!a.vhs_fm.decoder.worker && atomic_load(&destroys)==destroyed+1);
    assert(monitor_source==b.vhs_fm.decoder.worker && route_audio_enabled(&b.vhs_fm.decoder));
    for(int profile=0;profile<GUI_RF_PROFILE_COUNT;profile++) assert(!atomic_load(&s_rf_routes[0][profile]));
    int calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&route_calls)==calls+1);
    assert(atomic_load(&((route_worker_t*)b.vhs_fm.decoder.worker)->submits)==2);

    // Restoring the parked state retains the video preset, creates the matching
    // profile worker and restores A's source priority while disabling B audio.
    panel_config_set_left_view(configs[0],PANEL_VIEW_CVBS);
    assert(configs[0]->left_state==&a && configs[0]->left_cached_view==PANEL_VIEW_WAVEFORM);
    vhs_fm_decoder_t *d=&a.vhs_fm.decoder;
    assert(d->overlay.selected_tape==1 && d->overlay.selected_system==1);
    assert(d->overlay.beta_preset==GUI_BETA_PRESET_SUPERBETA);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+3 && ((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    assert(monitor_source==d->worker && route_audio_enabled(d) && !route_audio_enabled(&b.vhs_fm.decoder));
    calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&route_calls)==calls+2);
    assert(atomic_load(&((route_worker_t*)d->worker)->submits)==1);
    assert(atomic_load(&((route_worker_t*)b.vhs_fm.decoder.worker)->submits)==3);
    panel_config_cleanup(configs[0]);panel_config_cleanup(configs[1]);
    assert(!monitor_source && atomic_load(&destroys)==destroyed+3);
    calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&route_calls)==calls);
}
static void test_beta_preset_routes(void) {
    _Static_assert(GUI_BETA_PRESET_BETAMAX==0 && GUI_BETA_PRESET_BETAMAX_HIFI==1 &&
        GUI_BETA_PRESET_SUPERBETA==2 && GUI_BETA_PRESET_ED_BETA==3 && GUI_BETA_PRESET_COUNT==4,
        "four selectable video presets, independent of Audio Mon");
    const struct { gui_beta_preset_t preset; gui_rf_profile_t profile; const char *name; bool hifi; } cases[]={
        {GUI_BETA_PRESET_BETAMAX,GUI_RF_PROFILE_NTSC_BETAMAX,"Betamax",false},
        {GUI_BETA_PRESET_BETAMAX_HIFI,GUI_RF_PROFILE_NTSC_BETAMAX_HIFI,"Betamax Hi-Fi",true},
        {GUI_BETA_PRESET_SUPERBETA,GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI,"SuperBeta",true},
        {GUI_BETA_PRESET_ED_BETA,GUI_RF_PROFILE_NTSC_ED_BETA,"ED Beta",true}
    };
    video_panel_state_t panel={0};
    vhs_fm_decoder_t *d=&panel.vhs_fm.decoder;
    assert(gui_vhs_fm_init(d));
    assert(d->overlay.beta_preset==GUI_BETA_PRESET_BETAMAX);
    atomic_store(&panel.active_plugin,VIDEO_PLUGIN_VHS_FM);
    d->overlay.selected_tape=1;
    route_app.panel_config_a=(channel_panel_config_t){.left_state=&panel,.left_view=PANEL_VIEW_CVBS};
    route_app.panel_config_b=(channel_panel_config_t){.left_view=PANEL_VIEW_WAVEFORM,
        .right_view=PANEL_VIEW_WAVEFORM};
    route_app.settings.audio_monitor_playback=false;
    route_app.is_capturing=true;
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        gui_vhs_fm_set_beta_preset(d,cases[i].preset);
        gui_vhs_fm_set_format(d,2); /* Every Beta preset forces NTSC. */
        assert(d->overlay.selected_system==1 && gui_vhs_fm_get_format(d)==VHS_FM_FORMAT_NTSC);
        assert(!strcmp(gui_vhs_fm_get_beta_preset_name(d),cases[i].name));
        assert(!route_app.settings.audio_monitor_playback); /* Selection never opens Audio Mon. */
        gui_video_update_rf_routes(&route_app);
        gui_rf_preview_worker_t *worker=d->worker;
        assert(worker && ((route_worker_t*)worker)->profile==cases[i].profile);
        assert(cases[i].profile!=GUI_RF_PROFILE_NTSC_SUPERBETA); /* CLI-only video-only profile. */
        int create_count=atomic_load(&creates),destroy_count=atomic_load(&destroys);
        int reset_count=atomic_load(&resets),bind_count=atomic_load(&binds);
        for(int toggle=0;toggle<3;toggle++) {
            bool audible=toggle==1;
            route_app.settings.audio_monitor_playback=audible;
            gui_video_update_rf_routes(&route_app);
            assert(d->worker==worker && d->bound_profile==(int)cases[i].profile);
            assert(gui_vhs_fm_get_profile(d)==cases[i].profile && d->overlay.beta_preset==cases[i].preset);
            assert(monitor_source==(cases[i].hifi?worker:NULL));
            assert(route_audio_enabled(d)==(cases[i].hifi && audible));
            assert(atomic_load(&creates)==create_count && atomic_load(&destroys)==destroy_count);
            assert(atomic_load(&resets)==reset_count && atomic_load(&binds)==bind_count);
        }
    }
    /* VHS ignores a remembered preset. Editing it while VHS is active must not
     * rebind video or turn monitoring on; returning to Beta applies that pick. */
    d->overlay.selected_tape=0;gui_vhs_fm_set_format(d,1);
    gui_video_update_rf_routes(&route_app);
    assert(d->overlay.beta_preset==GUI_BETA_PRESET_ED_BETA);
    gui_rf_preview_worker_t *vhs=d->worker;
    assert(((route_worker_t*)vhs)->profile==GUI_RF_PROFILE_NTSC_VHS);
    int bind_count=atomic_load(&binds),reset_count=atomic_load(&resets);
    gui_vhs_fm_set_beta_preset(d,GUI_BETA_PRESET_BETAMAX_HIFI);
    gui_video_update_rf_routes(&route_app);
    assert(d->worker==vhs && gui_vhs_fm_get_profile(d)==GUI_RF_PROFILE_NTSC_VHS);
    assert(atomic_load(&binds)==bind_count && atomic_load(&resets)==reset_count);
    assert(!monitor_source && !route_audio_enabled(d) && !route_app.settings.audio_monitor_playback);
    d->overlay.selected_tape=1;gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    assert(monitor_source==d->worker && !route_audio_enabled(d));
    const int invalid[]={-1,GUI_BETA_PRESET_COUNT,12345};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        gui_vhs_fm_set_beta_preset(d,invalid[i]);
        assert(d->overlay.beta_preset==GUI_BETA_PRESET_BETAMAX);
        assert(gui_vhs_fm_get_profile(d)==GUI_RF_PROFILE_NTSC_BETAMAX);
        assert(!strcmp(gui_vhs_fm_get_beta_preset_name(d),"Betamax"));
    }
    gui_video_update_rf_routes(&route_app);
    assert(!monitor_source && !route_audio_enabled(d) && !route_app.settings.audio_monitor_playback);
    atomic_store(&panel.active_plugin,VIDEO_PLUGIN_CVBS);
    gui_video_update_rf_routes(&route_app);
    assert(!d->worker && !monitor_source);
    assert(atomic_load(&creates)-created==atomic_load(&destroys)-destroyed);
}
static void test_beta_monitor_priority(void) {
    video_panel_state_t a_left={0},a_right={0},b_left={0},b_right={0};
    video_panel_state_t *panels[]={&a_left,&a_right,&b_left,&b_right};
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    const gui_beta_preset_t initial[]={GUI_BETA_PRESET_BETAMAX,GUI_BETA_PRESET_SUPERBETA,
        GUI_BETA_PRESET_BETAMAX_HIFI,GUI_BETA_PRESET_ED_BETA};
    for(size_t i=0;i<4;i++) {
        atomic_store(&panels[i]->active_plugin,VIDEO_PLUGIN_VHS_FM);
        panels[i]->vhs_fm.decoder.bound_channel=-1;
        panels[i]->vhs_fm.decoder.overlay.selected_tape=1;
        gui_vhs_fm_set_beta_preset(&panels[i]->vhs_fm.decoder,initial[i]);
    }
    route_app.panel_config_a.left_state=&a_left;
    route_app.panel_config_a.right_state=&a_right;
    route_app.panel_config_a.left_view=route_app.panel_config_a.right_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.split=true;
    route_app.panel_config_b.left_state=&b_left;
    route_app.panel_config_b.right_state=&b_right;
    route_app.panel_config_b.left_view=route_app.panel_config_b.right_view=PANEL_VIEW_WAVEFORM;
    route_app.panel_config_b.split=true;
    route_app.settings.audio_monitor_playback=true;
    route_app.is_capturing=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+2 && a_left.vhs_fm.decoder.worker!=a_right.vhs_fm.decoder.worker);
    assert(!route_audio_enabled(&a_left.vhs_fm.decoder));
    /* SuperBeta is always a monitor-capable video preset, without another switch. */
    assert(monitor_source==a_right.vhs_fm.decoder.worker && route_audio_enabled(&a_right.vhs_fm.decoder));
    assert(((route_worker_t*)monitor_source)->profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    gui_vhs_fm_set_beta_preset(&a_left.vhs_fm.decoder,GUI_BETA_PRESET_SUPERBETA);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+2 && a_left.vhs_fm.decoder.worker==a_right.vhs_fm.decoder.worker);
    route_app.panel_config_b.left_view=route_app.panel_config_b.right_view=PANEL_VIEW_CVBS;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+4 && monitor_source==a_right.vhs_fm.decoder.worker);
    assert(!route_audio_enabled(&b_left.vhs_fm.decoder) && !route_audio_enabled(&b_right.vhs_fm.decoder));
    // A-left wins across all Hi-Fi preset kinds; the previous SuperBeta source is disabled.
    gui_vhs_fm_set_beta_preset(&a_left.vhs_fm.decoder,GUI_BETA_PRESET_BETAMAX_HIFI);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+5 && monitor_source==a_left.vhs_fm.decoder.worker);
    assert(route_audio_enabled(&a_left.vhs_fm.decoder) && !route_audio_enabled(&a_right.vhs_fm.decoder));
    gui_rf_preview_worker_t *saved[4];
    const gui_beta_preset_t presets[]={GUI_BETA_PRESET_BETAMAX_HIFI,GUI_BETA_PRESET_SUPERBETA,
        GUI_BETA_PRESET_BETAMAX_HIFI,GUI_BETA_PRESET_ED_BETA};
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_NTSC_BETAMAX_HIFI,GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI,
        GUI_RF_PROFILE_NTSC_BETAMAX_HIFI,GUI_RF_PROFILE_NTSC_ED_BETA};
    for(size_t i=0;i<4;i++) {
        saved[i]=panels[i]->vhs_fm.decoder.worker;
        assert(((route_worker_t*)saved[i])->profile==profiles[i]);
    }
    int reset_count=atomic_load(&resets),bind_count=atomic_load(&binds);
    int destroy_count=atomic_load(&destroys);
    // Muting keeps every video profile, worker and reference intact.
    route_app.settings.audio_monitor_playback=false;gui_video_update_rf_routes(&route_app);
    for(size_t i=0;i<4;i++) assert(panels[i]->vhs_fm.decoder.worker==saved[i] && !route_audio_enabled(&panels[i]->vhs_fm.decoder));
    assert(atomic_load(&creates)==created+5 && monitor_source==saved[0]);
    assert(atomic_load(&destroys)==destroy_count && atomic_load(&resets)==reset_count && atomic_load(&binds)==bind_count);
    route_app.settings.audio_monitor_playback=true;route_app.is_capturing=false;
    gui_video_update_rf_routes(&route_app);
    for(size_t i=0;i<4;i++) assert(panels[i]->vhs_fm.decoder.worker==saved[i] && !route_audio_enabled(&panels[i]->vhs_fm.decoder));
    route_app.is_capturing=true;gui_video_update_rf_routes(&route_app);
    assert(route_audio_enabled(&a_left.vhs_fm.decoder) && !route_audio_enabled(&a_right.vhs_fm.decoder));
    assert(atomic_load(&destroys)==destroy_count && atomic_load(&resets)==reset_count && atomic_load(&binds)==bind_count);
    // A paused Beta route also yields to the first playing B route.
    transport_running=transport_paused[0]=true;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source==saved[2] && route_audio_enabled(&b_left.vhs_fm.decoder));
    assert(!route_audio_enabled(&a_left.vhs_fm.decoder) && !route_audio_enabled(&a_right.vhs_fm.decoder));
    transport_running=transport_paused[0]=false;
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source==saved[0] && route_audio_enabled(&a_left.vhs_fm.decoder));
    /* Real pressure gating pauses the existing workers before any RF copy.
     * Changing Audio Mon while paused must neither rebuild nor resume video. */
    route_app.is_recording=false;
    route_app.buffers.initialized[BUF_CAPTURE_RF]=true;
    route_app.buffers.buffers[BUF_CAPTURE_RF].buffer_size=100;
    atomic_store(&route_app.buffers.stats[BUF_CAPTURE_RF].bytes_read,0);
    atomic_store(&route_app.buffers.stats[BUF_CAPTURE_RF].bytes_written,95);
    int calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&s_rf_pressure_paused) && atomic_load(&route_calls)==calls);
    for(int audible=0;audible<2;audible++) {
        route_app.settings.audio_monitor_playback=audible!=0;gui_video_update_rf_routes(&route_app);
        for(size_t i=0;i<4;i++) {
            assert(panels[i]->vhs_fm.decoder.worker==saved[i]);
            assert(panels[i]->vhs_fm.decoder.overlay.beta_preset==presets[i]);
            assert(gui_vhs_fm_get_profile(&panels[i]->vhs_fm.decoder)==profiles[i]);
            assert(atomic_load(&((route_worker_t*)saved[i])->paused));
        }
        assert(atomic_load(&creates)==created+5 && atomic_load(&destroys)==destroy_count);
        assert(atomic_load(&resets)==reset_count && atomic_load(&binds)==bind_count);
    }
    gui_rf_block_info_t continuous[2]={route_info[0],route_info[1]};continuous[1].discontinuity=false;
    route_app.is_capturing=false;
    atomic_store(&route_app.buffers.stats[BUF_CAPTURE_RF].bytes_read,95);
    gui_video_submit_rf(&route_app,NULL,NULL,0,0,continuous,true);
    assert(!atomic_load(&s_rf_pressure_paused));
    route_app.is_capturing=true;gui_video_update_rf_routes(&route_app);
    // Releasing each source transfers ownership in visible A-left/A-right/B-left/B-right order.
    for(size_t i=0;i<4;i++) {
        atomic_store(&panels[i]->active_plugin,VIDEO_PLUGIN_CVBS);
        gui_video_update_rf_routes(&route_app);
        assert(monitor_source==(i+1<4 ? saved[i+1] : NULL));
        if(i+1<4) assert(route_audio_enabled(&panels[i+1]->vhs_fm.decoder));
        for(size_t j=i+2;j<4;j++) assert(!route_audio_enabled(&panels[j]->vhs_fm.decoder));
    }
    assert(atomic_load(&creates)==created+5 && atomic_load(&destroys)==destroyed+5);
}
static void test_rtlsdr_routes(void) {
    const gui_rf_profile_t profile = GUI_RF_PROFILE_NTSC_VHS_HIFI;
    route_worker_t a = {.profile = profile}, b = {.profile = profile};
    assert(!atomic_load(&s_rf_routes[0][profile]) && !atomic_load(&s_rf_routes[1][profile]));
    atomic_store(&s_rf_routes[0][profile], (gui_rf_preview_worker_t *)&a);
    atomic_store(&s_rf_routes[1][profile], (gui_rf_preview_worker_t *)&b);

    route_app.is_capturing = true;
    atomic_store(&route_app.rf_source_is_rtlsdr, true);
    int ordinary_calls = atomic_load(&route_calls);
    unsigned audio_resets = monitor_resets;
    gui_video_submit_rf(&route_app, route_a, route_b, 2, 2, route_info, true);
    assert(atomic_load(&route_calls) == ordinary_calls);
    assert(!atomic_load(&a.submits) && !atomic_load(&b.submits));

    gui_video_submit_rtlsdr(&route_app, route_iq, 2, &route_iq_info);
    assert(route_iq_calls == 2 && monitor_resets == audio_resets + 2);
    assert(atomic_load(&a.submits) == 1 && atomic_load(&b.submits) == 1);
    assert(atomic_load(&route_calls) == ordinary_calls);

    int worker_resets = atomic_load(&resets);
    gui_video_reset_rtlsdr(&route_app);
    assert(atomic_load(&resets) == worker_resets + 2 && monitor_resets == audio_resets + 4);

    atomic_store(&route_app.rf_source_is_rtlsdr, false);
    gui_video_submit_rf(&route_app, route_a, route_b, 2, 2, route_info, true);
    assert(atomic_load(&route_calls) == ordinary_calls + 2);
    assert(atomic_load(&a.submits) == 2 && atomic_load(&b.submits) == 2);
    atomic_store(&s_rf_routes[0][profile], NULL);
    atomic_store(&s_rf_routes[1][profile], NULL);

    video_panel_state_t panel_a = {0}, panel_b = {0};
    vhs_fm_decoder_t *ntsc = &panel_a.vhs_fm.decoder;
    vhs_fm_decoder_t *pal = &panel_b.vhs_fm.decoder;
    assert(gui_vhs_fm_init(ntsc) && gui_vhs_fm_init(pal));
    atomic_init(&panel_a.active_plugin, VIDEO_PLUGIN_VHS_FM);
    atomic_init(&panel_b.active_plugin, VIDEO_PLUGIN_VHS_FM);
    gui_vhs_fm_set_vhs_preset(ntsc, GUI_VHS_PRESET_HIFI);
    gui_vhs_fm_set_vhs_preset(pal, GUI_VHS_PRESET_HIFI);
    gui_vhs_fm_set_format(ntsc, 1);
    route_app.panel_config_a = (channel_panel_config_t){.left_view=PANEL_VIEW_CVBS, .left_state=&panel_a};
    route_app.panel_config_b = (channel_panel_config_t){.left_view=PANEL_VIEW_CVBS, .left_state=&panel_b};
    route_app.settings.audio_monitor_playback = true;
    atomic_store(&route_app.rf_source_is_rtlsdr, true);
    atomic_store(&route_app.sample_rate, 1024000);
    atomic_store(&route_app.rf_source_rtlsdr_tuning, 1710000);
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == pal->worker && route_audio_enabled(pal) && !route_audio_enabled(ntsc));

    atomic_store(&route_app.rf_source_rtlsdr_tuning, UINT64_C(1) << 32);
    gui_video_update_rf_routes(&route_app);
    assert(!monitor_source && !route_audio_enabled(ntsc) && !route_audio_enabled(pal));

    atomic_store(&route_app.rf_source_rtlsdr_tuning, 1500000);
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source == ntsc->worker && route_audio_enabled(ntsc) && !route_audio_enabled(pal));
    video_detach_rf(ntsc);
    video_detach_rf(pal);
    route_app.panel_config_a = (channel_panel_config_t){0};
    route_app.panel_config_b = (channel_panel_config_t){0};
    atomic_store(&route_app.rf_source_is_rtlsdr, false);
}

int main(void) {
    atomic_store(&route_app.rf_source_rate_hz[0], 40000000);
    atomic_store(&route_app.rf_source_rate_hz[1], 40000000);
    video_panel_state_t *left=new_panel(), *right=new_panel(), *b=new_panel();
    route_app.is_capturing=true; route_app.settings.rf_preview_budget_percent=100;
    route_app.panel_config_a.left_view=route_app.panel_config_a.right_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.left_state=left; route_app.panel_config_a.right_state=right; route_app.panel_config_a.split=true;
    gui_video_update_rf_routes(&route_app); assert(atomic_load(&creates)==0); // No inactive RF allocation.
    atomic_store(&left->active_plugin,VIDEO_PLUGIN_VHS_FM); atomic_store(&right->active_plugin,VIDEO_PLUGIN_VHS_FM);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==1 && left->vhs_fm.decoder.worker==right->vhs_fm.decoder.worker);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==1);
    right->vhs_fm.decoder.overlay.selected_system=1; gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==2 && atomic_load(&destroys)==0);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==3);
    atomic_store(&left->active_plugin,VIDEO_PLUGIN_CVBS); gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&destroys)==1);
    left->vhs_fm.decoder.overlay.selected_system=1; atomic_store(&left->active_plugin,VIDEO_PLUGIN_VHS_FM);
    gui_video_update_rf_routes(&route_app); assert(atomic_load(&creates)==2);
    // Hiding a still-existing split view releases its reference, not the other view.
    route_app.panel_config_a.split=false; gui_video_update_rf_routes(&route_app);
    assert(!right->vhs_fm.decoder.worker && atomic_load(&destroys)==1);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==4);
    // Severe capture pressure gates actual decode before a copy.
    route_app.buffers.initialized[BUF_CAPTURE_RF]=true; route_app.buffers.buffers[BUF_CAPTURE_RF].buffer_size=100;
    simulated_fill=95; atomic_store(&route_app.buffers.stats[BUF_CAPTURE_RF].bytes_written,95); gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==4 && atomic_load(&s_rf_pressure_paused));
    route_app.is_capturing=false; simulated_fill=0; atomic_store(&route_app.buffers.stats[BUF_CAPTURE_RF].bytes_written,0); gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==5 && !atomic_load(&s_rf_pressure_paused)); route_app.is_capturing=true;
    // Wrapped ring head/tail can look full while monotonic byte totals are empty.
    route_app.is_recording=true; route_app.settings.capture_a=true;
    route_app.buffers.initialized[BUF_RECORD_A]=true; route_app.buffers.buffers[BUF_RECORD_A].buffer_size=1024;
    atomic_store(&route_app.buffers.buffers[BUF_RECORD_A].head,0);
    atomic_store(&route_app.buffers.buffers[BUF_RECORD_A].tail,1024);
    atomic_store(&route_app.buffers.stats[BUF_RECORD_A].bytes_read,1024);
    atomic_store(&route_app.buffers.stats[BUF_RECORD_A].bytes_written,1024);
    assert(video_buffer_percent(&route_app,BUF_RECORD_A)==0);
    // Old record drops stay in the snapshot during stop/start, causing no new pause.
    atomic_store(&route_app.buffers.stats[BUF_RECORD_A].write_drops,3);
    route_app.is_capturing=false; gui_video_submit_rf(&route_app,NULL,NULL,0,0,route_info,true);
    route_app.is_capturing=true; route_app.is_recording=false;
    gui_video_submit_rf(&route_app,NULL,NULL,0,0,route_info,true);
    route_app.is_recording=true; gui_video_submit_rf(&route_app,NULL,NULL,0,0,route_info,true);
    assert(!atomic_load(&s_rf_pressure_paused));
    // Independent B source preserves its own metadata and receives one copy.
    route_app.panel_config_b.left_view=PANEL_VIEW_CVBS; route_app.panel_config_b.left_state=b;
    atomic_store(&b->active_plugin,VIDEO_PLUGIN_VHS_FM); gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==3);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true); assert(atomic_load(&route_calls)==7);
    video_detach_rf(&b->vhs_fm.decoder); assert(atomic_load(&destroys)==2);
    detach_decoder=&left->vhs_fm.decoder;
    atomic_store(&copy_entered,false); atomic_store(&hold_copy,true);
    thrd_t producer,detacher; assert(thrd_create_with_priority(&producer,route_producer,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    wait_flag(&copy_entered); assert(thrd_create_with_priority(&detacher,route_detach,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    uint64_t begin=get_time_ms(); while(atomic_load(&s_rf_routes[0][1])) { if(get_time_ms()-begin>2000) exit(1); thrd_sleep_ms(1); }
    assert(!atomic_load(&detach_done) && atomic_load(&destroys)==2);
    atomic_store(&hold_copy,false); thrd_join(producer,NULL); thrd_join(detacher,NULL);
    assert(atomic_load(&destroys)==3 && atomic_load(&route_calls)==8);
    free(left); free(right); free(b);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==8);
    // Same-channel Beta views share only identical full profiles.
    video_panel_state_t beta_left={0}, beta_right={0};
    atomic_store(&beta_left.active_plugin,VIDEO_PLUGIN_VHS_FM);
    atomic_store(&beta_right.active_plugin,VIDEO_PLUGIN_VHS_FM);
    beta_left.vhs_fm.decoder.overlay.selected_tape=1;
    beta_right.vhs_fm.decoder.overlay.selected_tape=1;
    route_app.panel_config_a.left_state=&beta_left;
    route_app.panel_config_a.right_state=&beta_right;
    route_app.panel_config_a.left_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.right_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.split=true;
    route_app.panel_config_b.left_view=PANEL_VIEW_WAVEFORM;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==4 && beta_left.vhs_fm.decoder.worker==beta_right.vhs_fm.decoder.worker);
    assert(!monitor_source);
    gui_vhs_fm_set_beta_preset(&beta_right.vhs_fm.decoder,GUI_BETA_PRESET_BETAMAX_HIFI);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==5 && beta_left.vhs_fm.decoder.worker!=beta_right.vhs_fm.decoder.worker);
    assert(monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(((route_worker_t*)monitor_source)->profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    assert(!atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    route_app.settings.audio_monitor_playback=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    route_app.settings.audio_monitor_playback=false;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==5 && monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(!atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    route_app.settings.audio_monitor_playback=true;
    video_panel_state_t beta_b={0};
    atomic_store(&beta_b.active_plugin,VIDEO_PLUGIN_VHS_FM);
    beta_b.vhs_fm.decoder.overlay.selected_tape=1;
    gui_vhs_fm_set_beta_preset(&beta_b.vhs_fm.decoder,GUI_BETA_PRESET_BETAMAX_HIFI);
    route_app.panel_config_b.left_view=PANEL_VIEW_CVBS;
    route_app.panel_config_b.left_state=&beta_b;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==6 && monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    assert(!atomic_load(&((route_worker_t*)beta_b.vhs_fm.decoder.worker)->audio_enabled));
    atomic_store(&beta_right.active_plugin,VIDEO_PLUGIN_CVBS);
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source==beta_b.vhs_fm.decoder.worker);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    atomic_store(&beta_right.active_plugin,VIDEO_PLUGIN_VHS_FM);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==7 && monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    assert(!atomic_load(&((route_worker_t*)beta_b.vhs_fm.decoder.worker)->audio_enabled));
    video_detach_rf(&beta_left.vhs_fm.decoder);
    video_detach_rf(&beta_right.vhs_fm.decoder);
    video_detach_rf(&beta_b.vhs_fm.decoder);
    assert(atomic_load(&destroys)==7 && !monitor_source);
    test_beta_monitor_priority();
    test_beta_preset_routes();
    test_cached_hifi_panel_routes();
    test_secam_recording_selector();
    test_vhs_preset_selector();
    test_svhs_routes();
    test_rf_audio_playback_priority();
    test_laserdisc_routes();
    test_secam_vhs_routes(false,GUI_RF_PROFILE_SECAM_VHS,VHS_FM_FORMAT_SECAM,"SECAM");
    test_secam_vhs_routes(true,GUI_RF_PROFILE_MESECAM_VHS,VHS_FM_FORMAT_MESECAM,"MESECAM");
    test_cached_mesecam_panel();
    test_rf_overlay_scale();
    test_rf_overlay_short_panes();
    test_rtlsdr_routes();
    puts("RF routes: VHS/S-VHS, four Beta presets, PAL/NTSC LaserDisc, independent Audio Mon, SECAM/MESECAM isolation, cached panels, source priority, mute, pressure and concurrent last-release passed");
}
