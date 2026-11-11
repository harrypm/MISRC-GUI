/* Headless reconnect integration, GPL-3.0-or-later.
 * Actual FLAC producer -> production mapped FIFO/events -> actual extraction
 * and sideband stamps -> production RF route/pressure -> worker/DSP -> actual
 * UI wrapper RGBA swap. Actual display start/stop shares the same instance.
 * Drawing, unrelated panel processing and inactive recording are replaced.
 * Long idle below is real wall time; this is not a window or hardware test.
 */
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/input/gui_playback.c"
static void reconnect_submit_observed(gui_app_t *app,const int16_t *a,const int16_t *b,
    size_t count_a,size_t count_b,const gui_rf_block_info_t info[2],bool supported);
#define gui_video_submit_rf reconnect_submit_observed
#include "../misrc_gui/processing/gui_extract.c"
#undef gui_video_submit_rf
#include "../misrc_gui/processing/gui_display_thread.c"
#include <FLAC/stream_encoder.h>
#include <assert.h>

#define RECONNECT_RATE 20000000u
#define RECONNECT_PREFIX_SAMPLES RECONNECT_RATE
#define RECONNECT_TIMEOUT_MS 5000u
#define RECONNECT_RING_BYTES ((size_t)16*1024*1024)
#define RECONNECT_FRAME_BYTES ((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4)
#define RECONNECT_FNV_OFFSET UINT64_C(14695981039346656037)
#define RECONNECT_FNV_PRIME UINT64_C(1099511628211)

atomic_int do_exit=0;
static atomic_uint_fast64_t reconnect_blocks,reconnect_samples,reconnect_epoch;
static atomic_uint_fast64_t reconnect_panel_calls;
static atomic_bool reconnect_first_checked;
static int16_t reconnect_expected[2];
static uint64_t reconnect_expected_block_hash=RECONNECT_FNV_OFFSET;
static uint64_t reconnect_hash(const uint8_t *bytes,size_t count);

double GetTime(void) { return (double)gui_playback_clock_us()/1000000.0; }
void gui_app_set_status(gui_app_t *app,const char *message) {
    snprintf(app->status_message,sizeof(app->status_message),"%s",message);
}
void UnloadTexture(Texture2D texture) { (void)texture; }
void gui_waveform_set_trigger_source_buffers(const int16_t *a,const int16_t *b,size_t count) {
    (void)a; (void)b; assert(count==DISPLAY_FRAME_SAMPLES);
}
void panel_process_all(gui_app_t *app,const int16_t *a,const int16_t *b,size_t count,uint32_t rate) {
    (void)app; assert(a && b && count==DISPLAY_FRAME_SAMPLES && rate==RECONNECT_RATE);
    atomic_fetch_add(&reconnect_panel_calls,1);
}
bool gui_record_check_disk_space_guard(gui_app_t *app,uint32_t frame,char *msg,size_t capacity) {
    (void)app; (void)frame; (void)msg; (void)capacity; return false;
}
bool gui_record_push_converted(gui_app_t *app, int channel, const int16_t *samples,
                               size_t bytes, uint32_t frame, char *msg, size_t capacity) {
    (void)app; (void)channel; (void)samples; (void)bytes; (void)frame; (void)msg; (void)capacity;
    assert(false); return false; /* Recording is disabled in this harness. */
}
void gui_record_log_capture_event(gui_app_t *app,const char *level,const char *message,
    gui_error_class_t error_class,uint32_t count) {
    (void)app; (void)level; (void)message; (void)error_class; (void)count; assert(false);
}

static void reconnect_submit_observed(gui_app_t *app,const int16_t *a,const int16_t *b,
    size_t count_a,size_t count_b,const gui_rf_block_info_t info[2],bool supported) {
    if(count_a) {
        assert(a && !b && count_a<=BUFFER_READ_SIZE && count_b==0 && supported);
        assert(info[0].sample_rate_hz==RECONNECT_RATE && info[1].sample_rate_hz==0);
        if(!atomic_exchange(&reconnect_first_checked,true)) {
            assert(info[0].start_sample==0 && count_a==BUFFER_READ_SIZE);
            /* FLAC16 -> production 12-bit pack -> actual SIMD extraction. */
            assert(a[0]==reconnect_expected[0] && a[37]==reconnect_expected[1]);
            assert(reconnect_hash((const uint8_t *)a,count_a*sizeof(*a))==reconnect_expected_block_hash);
        }
        atomic_fetch_add(&reconnect_blocks,1);
        atomic_fetch_add(&reconnect_samples,count_a);
        atomic_store(&reconnect_epoch,info[0].epoch);
    }
    /* Observation never substitutes a route, pressure decision or worker. */
    gui_video_submit_rf(app,a,b,count_a,count_b,info,supported);
}
static uint64_t reconnect_hash(const uint8_t *bytes,size_t count) {
    uint64_t hash=RECONNECT_FNV_OFFSET;
    for(size_t n=0;n<count;++n) { hash^=bytes[n]; hash*=RECONNECT_FNV_PRIME; }
    return hash;
}
static bool reconnect_save_ppm(const char *path,const vhs_fm_decoder_t *decoder) {
    FILE *file=fopen(path,"wb");
    if(!file) return false;
    bool ok=fprintf(file,"P6\n%d %d\n255\n",decoder->width,decoder->height)>0;
    uint8_t row[GUI_RF_FRAME_MAX_WIDTH*3];
    for(int y=0;ok && y<decoder->height;++y) {
        for(int x=0;x<decoder->width;++x)
            memcpy(row+x*3,decoder->rgba+((size_t)y*decoder->width+x)*4,3);
        ok=fwrite(row,3,(size_t)decoder->width,file)==(size_t)decoder->width;
    }
    return fclose(file)==0 && ok;
}
static void reconnect_make_flac(const char *raw,const char *normal,const char *unknown) {
    FILE *input=fopen(raw,"rb"); assert(input);
    FLAC__StreamEncoder *encoder=FLAC__stream_encoder_new(); assert(encoder);
    assert(FLAC__stream_encoder_set_channels(encoder,1));
    assert(FLAC__stream_encoder_set_bits_per_sample(encoder,16));
    assert(FLAC__stream_encoder_set_sample_rate(encoder,20000));
    assert(FLAC__stream_encoder_set_compression_level(encoder,0));
    assert(FLAC__stream_encoder_set_blocksize(encoder,4096));
    assert(FLAC__stream_encoder_set_total_samples_estimate(encoder,RECONNECT_PREFIX_SAMPLES));
    assert(FLAC__stream_encoder_init_file(encoder,normal,NULL,NULL)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    uint8_t bytes[4096*2]; FLAC__int32 samples[4096];
    for(unsigned offset=0;offset<RECONNECT_PREFIX_SAMPLES;offset+=4096u) {
        unsigned count=RECONNECT_PREFIX_SAMPLES-offset; if(count>4096u) count=4096u;
        assert(fread(bytes,2,count,input)==count);
        for(unsigned n=0;n<count;++n) {
            samples[n]=(int16_t)((uint16_t)bytes[n*2]|((uint16_t)bytes[n*2+1]<<8));
            if(offset==0 && (n==0 || n==37)) reconnect_expected[n==37]=(int16_t)(samples[n]>>4);
            if(offset+n<BUFFER_READ_SIZE) {
                uint16_t quantized=(uint16_t)(int16_t)(samples[n]>>4);
                reconnect_expected_block_hash^=(uint8_t)quantized;
                reconnect_expected_block_hash*=RECONNECT_FNV_PRIME;
                reconnect_expected_block_hash^=(uint8_t)(quantized>>8);
                reconnect_expected_block_hash*=RECONNECT_FNV_PRIME;
            }
        }
        assert(FLAC__stream_encoder_process_interleaved(encoder,samples,count));
    }
    assert(fclose(input)==0);
    assert(FLAC__stream_encoder_finish(encoder)); FLAC__stream_encoder_delete(encoder);
    input=fopen(normal,"rb"); FILE *output=fopen(unknown,"wb"); assert(input && output);
    size_t count;
    while((count=fread(bytes,1,sizeof(bytes),input))>0) assert(fwrite(bytes,1,count,output)==count);
    assert(!ferror(input)); fclose(input); fclose(output);
    output=fopen(unknown,"r+b"); assert(output);
    assert(fseek(output,21,SEEK_SET)==0); int preserved=fgetc(output); assert(preserved>=0);
    assert(fseek(output,21,SEEK_SET)==0);
    uint8_t zero_total[5]={(uint8_t)(preserved&0xf0),0,0,0,0};
    assert(fwrite(zero_total,1,sizeof(zero_total),output)==sizeof(zero_total)); fclose(output);
    printf("Fixture: real RF prefix=%u samples / 40000000 bytes; mono16 FLAC header20k physical20M; extraction samples[0]=%d samples[37]=%d; first65536 quantized RF FNV1a64=%016llx\n",
           RECONNECT_PREFIX_SAMPLES,reconnect_expected[0],reconnect_expected[1],
           (unsigned long long)reconnect_expected_block_hash);
}
static void reconnect_layer_report(gui_app_t *app,display_thread_t *display,vhs_fm_decoder_t *decoder,
    unsigned session,const char *phase,uint64_t elapsed,bool unknown,bool passed,const char *ppm) {
    gui_rf_preview_stats_t stats; gui_rf_preview_worker_get_stats(decoder->worker,&stats);
    uint64_t rgba_hash=decoder->width>0 && decoder->height>0
        ? reconnect_hash(decoder->rgba,(size_t)decoder->width*decoder->height*4):0;
    printf("{\"session\":%u,\"phase\":\"%s\",\"passed\":%s,\"unknown_header\":%s,\"elapsed_ms\":%.3f,"
           "\"raw_written\":%llu,\"raw_read\":%llu,\"rf_submitted_blocks\":%llu,\"rf_source_samples\":%llu,"
           "\"stream_epoch\":%llu,\"observed_epoch\":%llu,\"display_frames\":%llu,\"display_ready\":%s,"
           "\"display_thread_running\":%s,\"extraction_running\":%s,\"panel_calls\":%llu,\"generation\":%llu,"
           "\"worker_frames\":%llu,\"worker_fields\":%llu,\"worker_resets\":%llu,\"queue_drops\":%llu,"
           "\"pressure_skips\":%llu,\"paused\":%s,\"budget_waiting\":%s,\"budget_wait_ms\":%.3f,"
           "\"width\":%d,\"height\":%d,\"texture_dirty\":%s,\"h_locked\":%s,\"v_locked\":%s,"
           "\"color_locked\":%s,\"total_samples\":%llu,\"indexing\":%s,\"rgba_fnv1a64\":\"%016llx\",\"ppm_saved\":%s}\n",
           session,phase,passed?"true":"false",unknown?"true":"false",elapsed/1000.0,
           (unsigned long long)atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].bytes_written),
           (unsigned long long)atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].bytes_read),
           (unsigned long long)atomic_load(&reconnect_blocks),(unsigned long long)atomic_load(&reconnect_samples),
           (unsigned long long)atomic_load(&app->rf_stream_epoch),(unsigned long long)atomic_load(&reconnect_epoch),
           (unsigned long long)atomic_load(&display->frames_processed),atomic_load(&display->samples.ready)?"true":"false",
           atomic_load(&display->running)?"true":"false",gui_extract_is_running()?"true":"false",
           (unsigned long long)atomic_load(&reconnect_panel_calls),(unsigned long long)decoder->generation,
           (unsigned long long)stats.total_frames,(unsigned long long)stats.total_fields,(unsigned long long)stats.resets,
           (unsigned long long)stats.blocks_dropped,(unsigned long long)stats.pause_drops,
           stats.paused?"true":"false",stats.budget_waiting?"true":"false",stats.budget_wait_ms,
           decoder->width,decoder->height,decoder->texture_dirty?"true":"false",
           stats.decode.h_locked?"true":"false",stats.decode.v_locked?"true":"false",stats.decode.color_locked?"true":"false",
           (unsigned long long)gui_playback_get_total_samples(app),gui_playback_is_indexing(app,0)?"true":"false",
           (unsigned long long)rgba_hash,ppm && reconnect_save_ppm(ppm,decoder)?"true":"false");
    fflush(stdout);
}
static bool reconnect_session(gui_app_t *app,display_thread_t *display,vhs_fm_decoder_t *decoder,
    const char *fixture,const char *directory,unsigned session,bool unknown) {
    gui_rf_preview_worker_t *same_worker=decoder->worker;
    assert(gui_rf_preview_worker_wait_idle(same_worker,RECONNECT_TIMEOUT_MS));
    gui_vhs_fm_swap_buffers(decoder); decoder->texture_dirty=false;
    uint64_t generation_before=decoder->generation;
    uint64_t display_before=atomic_load(&display->samples.frame);
    uint64_t epoch_before=atomic_load(&app->rf_stream_epoch);
    atomic_store(&reconnect_blocks,0); atomic_store(&reconnect_samples,0);
    atomic_store(&reconnect_epoch,0); atomic_store(&reconnect_panel_calls,0);
    atomic_store(&reconnect_first_checked,false);
    uint64_t begin=gui_playback_clock_us();
    assert(gui_playback_start_async(app,fixture,NULL)==0);
    int result=0;
    while(!result && gui_playback_clock_us()-begin<RECONNECT_TIMEOUT_MS*1000u) {
        result=gui_playback_poll_start(app); thrd_sleep_ms(1);
    }
    if(result!=1) {
        reconnect_layer_report(app,display,decoder,session,"commit_failed",gui_playback_clock_us()-begin,unknown,false,NULL);
        gui_playback_finish_start(app); return false;
    }
    assert(gui_playback_is_running(app) && app->is_capturing && gui_extract_is_running());
    assert(decoder->worker==same_worker && app->rf_source_rate_hz[0]==RECONNECT_RATE);
    assert(atomic_load(&app->rf_stream_epoch)>epoch_before);
    bool passed=false;
    while(gui_playback_clock_us()-begin<RECONNECT_TIMEOUT_MS*1000u) {
        assert(gui_playback_poll_start(app)==0);
        gui_vhs_fm_swap_buffers(decoder);
        if(decoder->generation>generation_before && decoder->texture_dirty &&
           decoder->width==640 && decoder->height==480 && decoder->stats.decode.v_locked &&
           atomic_load(&reconnect_blocks)>0 && atomic_load(&reconnect_epoch)>epoch_before &&
           atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].bytes_read)>0 &&
           atomic_load(&display->samples.frame)>display_before && atomic_load(&display->samples.ready) &&
           !gui_playback_is_indexing(app,0) && gui_playback_get_total_samples(app)==RECONNECT_PREFIX_SAMPLES) {
            passed=true; break;
        }
        thrd_sleep_ms(1);
    }
    char ppm[1024]; snprintf(ppm,sizeof(ppm),"%s/reconnect-session-%u.ppm",directory,session);
    reconnect_layer_report(app,display,decoder,session,passed?"fresh_rgba":"timeout",gui_playback_clock_us()-begin,
                           unknown,passed,passed?ppm:NULL);
    gui_playback_stop(app); gui_playback_finish_start(app);
    assert(!gui_playback_is_running(app) && !app->is_capturing && !gui_extract_is_running());
    assert(!atomic_load(&display->running) && display->thread==NULL);
    assert(!gui_playback_is_indexing(app,0));
    assert(gui_rf_preview_worker_wait_idle(same_worker,RECONNECT_TIMEOUT_MS));
    assert(decoder->worker==same_worker); /* Pool survives disconnection. */
    return passed;
}
int main(int argc,char **argv) {
    if(argc!=3) { fprintf(stderr,"Usage: %s real20M.s16le output-directory\n",argv[0]); return 2; }
    char normal[1024],unknown[1024];
    snprintf(normal,sizeof(normal),"%s/reconnect-real-known.flac",argv[2]);
    snprintf(unknown,sizeof(unknown),"%s/reconnect-real-unknown.flac",argv[2]);
    reconnect_make_flac(argv[1],normal,unknown);
    gui_app_t *app=calloc(1,sizeof(*app)); assert(app);
    display_thread_t display; assert(gui_display_thread_init(&display)==0); app->display_thread=&display;
    buffer_config_t configs[BUF_COUNT];
    for(int n=0;n<BUF_COUNT;++n) configs[n]=(buffer_config_t){.name="reconnect",.size=1024*1024,.lazy_init=true};
    configs[BUF_CAPTURE_RF]=(buffer_config_t){.name="reconnect_rf",.size=RECONNECT_RING_BYTES,.lazy_init=false};
    configs[BUF_DISPLAY]=(buffer_config_t){.name="reconnect_display",.size=RECONNECT_RING_BYTES,.lazy_init=false};
    assert(bufmgr_init_custom(&app->buffers,configs)==0);
    app->settings.rf_preview_budget_percent=100;
    app->settings.capture_a=true; app->settings.capture_b=false;
    video_panel_state_t *panel=calloc(1,sizeof(*panel)); assert(panel);
    assert(gui_vhs_fm_init(&panel->vhs_fm.decoder));
    gui_vhs_fm_set_format(&panel->vhs_fm.decoder,1);
    atomic_store(&panel->active_plugin,VIDEO_PLUGIN_VHS_FM);
    app->panel_config_a.left_view=PANEL_VIEW_CVBS; app->panel_config_a.left_state=panel;
    gui_video_update_rf_routes(app);
    vhs_fm_decoder_t *decoder=&panel->vhs_fm.decoder; assert(decoder->worker && decoder->rgba);
    bool passed=reconnect_session(app,&display,decoder,normal,argv[2],1,false);
    passed=reconnect_session(app,&display,decoder,normal,argv[2],2,false) && passed;
    uint64_t idle_begin=gui_playback_clock_us();
    for(unsigned n=0;n<30;++n) {
        assert(!app->is_capturing && !gui_extract_is_running() && !atomic_load(&display.running));
        assert(decoder->worker && !atomic_load(&do_exit));
        thrd_sleep_ms(1000);
        if(n==9 || n==19) { printf("Idle: actual worker retained for %u seconds\n",n+1); fflush(stdout); }
    }
    printf("Idle: actual wall=%.3fms; same app/display/worker/FIFO retained, no time hook\n",
           (gui_playback_clock_us()-idle_begin)/1000.0);
    passed=reconnect_session(app,&display,decoder,unknown,argv[2],3,true) && passed;
    passed=reconnect_session(app,&display,decoder,normal,argv[2],4,false) && passed;
    video_detach_rf(decoder); assert(!decoder->worker && !decoder->rgba);
    gui_extract_cleanup(); gui_display_thread_cleanup(&display); bufmgr_cleanup(&app->buffers);
    assert(!gui_playback_is_starting(app) && !gui_playback_is_indexing(app,0));
    free(panel); free(app);
    printf("Reconnect actual-chain regression %s (headless; no hardware/window claims)\n",passed?"passed":"FAILED");
    return passed?0:1;
}
