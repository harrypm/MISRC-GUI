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
typedef struct { gui_rf_format_t format; gui_rf_profile_t profile; atomic_bool paused, audio_enabled; atomic_int submits; } route_worker_t;
static const void *monitor_source;
static atomic_int route_calls, creates, destroys, resets, binds;
static atomic_bool hold_copy, copy_entered, detach_done;
static vhs_fm_decoder_t *detach_decoder;
static gui_app_t route_app;
static size_t simulated_fill;
static int16_t route_a[2]={123,-456}, route_b[2]={789,-123};
static gui_rf_block_info_t route_info[2]={{40000000,123456,42,false},{20000000,654321,43,true}};
gui_rf_preview_worker_t *gui_rf_preview_worker_create(gui_rf_format_t format) {
    route_worker_t *w=calloc(1,sizeof(*w)); assert(w); w->format=format;
    atomic_init(&w->audio_enabled,true);
    atomic_init(&w->submits,0);
    atomic_fetch_add(&creates,1); return (gui_rf_preview_worker_t*)w;
}
bool gui_rf_profile_has_hifi(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI || profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI ||
        profile==GUI_RF_PROFILE_NTSC_ED_BETA;
}
gui_rf_format_t gui_rf_profile_format(gui_rf_profile_t p) {
    return p==GUI_RF_PROFILE_PAL_VHS || p==GUI_RF_PROFILE_SECAM_VHS ||
        p==GUI_RF_PROFILE_MESECAM_VHS || p==GUI_RF_PROFILE_PAL_SVHS ? GUI_RF_PAL : GUI_RF_NTSC;
}
gui_rf_preview_worker_t *gui_rf_preview_worker_create_profile(gui_rf_profile_t profile) {
    gui_rf_preview_worker_t *worker=gui_rf_preview_worker_create(gui_rf_profile_format(profile));
    ((route_worker_t*)worker)->profile=profile;
    return worker;
}
void gui_rf_preview_worker_set_audio_callback(gui_rf_preview_worker_t *w, gui_rf_preview_audio_callback_t cb, void *ctx) {
    assert(w && cb && ctx==w);
}
void gui_rf_preview_worker_set_audio_enabled(gui_rf_preview_worker_t *w,bool enabled) {
    assert(w); atomic_store(&((route_worker_t*)w)->audio_enabled,enabled);
}
void gui_audio_set_hifi_source(const void *source) { monitor_source=source; }
void gui_audio_push_hifi(const void *source,const float *pcm,size_t frames) { assert(source && pcm && frames); }
void gui_audio_reset_hifi(const void *source) { assert(source); }
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
    if (atomic_load(&((route_worker_t*)worker)->paused)) return false;
    atomic_fetch_add(&((route_worker_t*)worker)->submits,1);
    atomic_fetch_add(&route_calls,1); atomic_store(&copy_entered,true);
    while (atomic_load(&hold_copy)) thrd_sleep_ms(1);
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
        GUI_RF_PROFILE_NTSC_SVHS==10 && GUI_RF_PROFILE_COUNT==11,
        "append tape profiles without renumbering existing IDs");
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
    _Static_assert(GUI_VHS_PRESET_VHS==0 && GUI_VHS_PRESET_SVHS==1 && GUI_VHS_PRESET_COUNT==2,
                   "VHS and S-VHS are independent manual recording presets");
    vhs_fm_decoder_t d;
    assert(gui_vhs_fm_init(&d));
    assert(d.overlay.vhs_preset==GUI_VHS_PRESET_VHS);
    assert(!strcmp(gui_vhs_fm_get_vhs_preset_name(&d),"VHS"));
    gui_vhs_fm_set_vhs_preset(&d,GUI_VHS_PRESET_SVHS);
    assert(gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_PAL_SVHS);
    assert(d.format==VHS_FM_FORMAT_PAL && !strcmp(gui_vhs_fm_get_vhs_preset_name(&d),"S-VHS"));
    gui_vhs_fm_set_format(&d,1);
    assert(gui_vhs_fm_get_profile(&d)==GUI_RF_PROFILE_NTSC_SVHS);
    assert(d.format==VHS_FM_FORMAT_NTSC);
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
    video_detach_rf(a);video_detach_rf(b);
    assert(!monitor_source && atomic_load(&creates)-created==atomic_load(&destroys)-destroyed);
    for(int index=0;index<GUI_RF_PROFILE_COUNT;index++) assert(!atomic_load(&s_rf_routes[0][index]));
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
int main(void) {
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
    test_secam_vhs_routes(false,GUI_RF_PROFILE_SECAM_VHS,VHS_FM_FORMAT_SECAM,"SECAM");
    test_secam_vhs_routes(true,GUI_RF_PROFILE_MESECAM_VHS,VHS_FM_FORMAT_MESECAM,"MESECAM");
    test_cached_mesecam_panel();
    puts("RF routes: VHS/S-VHS and four Beta video presets, independent Audio Mon, SECAM/MESECAM isolation, cached panels, source priority, mute, pressure and concurrent last-release passed");
}
