#include "gui_monitor_video_server.h"
#include "gui_monitor_video_options.h"
#include "gui_monitor_video_codec.h"
#include "gui_monitor_video_mux.h"
#include "gui_monitor_video_framebox.h"
#include "gui_monitor_video_i420.h"
#include "gui_monitor_video_wire.h"
#include "../signal/gui_cvbs.h"
#include "../../common/threading.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef LIBSOXR_ENABLED
#define LIBSOXR_ENABLED 0
#endif
#if LIBSOXR_ENABLED
#include <soxr.h>
#endif
#include <time.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <winsock2.h>
#define MVD_SEND_SOCKET(s,b,n) send((SOCKET)(s),(const char *)(b),(int)(n),0)
#else
#include <sys/socket.h>
#define MVD_SEND_SOCKET(s,b,n) send((int)(s),(const void *)(b),(n),MSG_NOSIGNAL)
#endif
#ifndef MSG_NOSIGNAL
/* Windows never invokes the POSIX send macro; MSG_NOSIGNAL unavailable on macOS? defined there. */
#define MSG_NOSIGNAL 0
#endif
#define MVD_INPUT_SAMPLES 65536u
/* Local GUI CVBS path reduces 40 MSPS to a fixed 4fsc-ish rate. Mirror
 * that DSP preprocessing on the headless server, on the decode worker ONLY. */
#define MVD_RESAMPLE_CAP (MVD_INPUT_SAMPLES + 16384u)
#define MVD_INPUT_SLOTS 64u
#define MVD_PIXEL_CAP ((size_t)MVD_MAX_WIDTH*MVD_MAX_HEIGHT*4u)
#define MVD_DEFAULT_BITRATE 750000u
#define MVD_DEFAULT_FPS_X100 2500u

typedef struct {
    gui_rf_block_info_t info;
    size_t n;
    int16_t samples[MVD_INPUT_SAMPLES];
} mvd_input_slot;
typedef struct {
    gui_monitor_video_framebox_t *box;
    cvbs_decoder_t *cvbs;
    gui_rf_preview_worker_t *rf;
    mvd_input_slot *slots;
    thrd_t worker;
    atomic_uint_fast64_t read_idx,write_idx;
    atomic_bool viewing,gap;
    atomic_uint mode;
    atomic_uint controls;
    /* A private, non-blocking snapshot: decoder state itself is NOT safe
     * to read from an HTTP thread while the CVBS worker is processing. */
    atomic_flag osd_busy;
    char cvbs_osd[640];
    bool worker_started;
#if LIBSOXR_ENABLED
    soxr_t cvbs_resampler;
    uint32_t cvbs_input_rate,cvbs_output_rate;
    float *cvbs_float;
    int16_t *cvbs_samples;
#endif
} mvd_channel;
static mvd_channel s_channels[2];
static atomic_bool s_active;
static atomic_uint s_capture_calls,s_stream_calls;
/* Monotonic, lock-free diagnostic counters; never used in the RF hot path for I/O. */
static atomic_uint_fast64_t s_input_blocks[2],s_queue_drops[2],s_encoded_frames[2];
static atomic_uint_fast64_t s_encoder_attempts[2],s_rc_skips[2],s_encoded_bytes[2];
static atomic_uint s_codec_choice[2];
static atomic_bool s_initialized;
static void mvd_wait(void) { thrd_sleep_ms(1); }


#if LIBSOXR_ENABLED
/* Runs only on the per-channel decode thread. No allocation or DSP on capture. */
static void mvd_resampler_clear(mvd_channel *ch) {
    if(ch->cvbs_resampler)soxr_delete(ch->cvbs_resampler);
    ch->cvbs_resampler=NULL;
    ch->cvbs_input_rate=ch->cvbs_output_rate=0;
}
static bool mvd_cvbs_resampled(mvd_channel *ch,const int16_t *input,
                               size_t count,uint32_t input_rate) {
    if(input_rate<1000000u || input_rate>100000000u) return false;
    uint32_t target=ch->cvbs->state.format==CVBS_FORMAT_NTSC ? 14300000u:17900000u;
    if(input_rate==target)return false;
    if(!ch->cvbs_resampler || ch->cvbs_input_rate!=input_rate ||
       ch->cvbs_output_rate!=target) {
        mvd_resampler_clear(ch);
        soxr_io_spec_t io=soxr_io_spec(SOXR_INT16_I,SOXR_FLOAT32_I);
        soxr_quality_spec_t quality=soxr_quality_spec(SOXR_HQ,0);
        quality.passband_end=0.95;
        quality.stopband_begin=1.0;
        quality.phase_response=50.0;
        soxr_runtime_spec_t runtime=soxr_runtime_spec(1);
        soxr_error_t error=NULL;
        ch->cvbs_resampler=soxr_create(input_rate,target,1,&error,&io,&quality,&runtime);
        if(error || !ch->cvbs_resampler) {
            mvd_resampler_clear(ch);
            return false;
        }
        ch->cvbs_input_rate=input_rate;
        ch->cvbs_output_rate=target;
    }
    if(!ch->cvbs_float)ch->cvbs_float=malloc(MVD_RESAMPLE_CAP*sizeof(float));
    if(!ch->cvbs_samples)ch->cvbs_samples=malloc(MVD_RESAMPLE_CAP*sizeof(int16_t));
    if(!ch->cvbs_float || !ch->cvbs_samples)return false;
    size_t consumed=0,produced=0;
    while(consumed<count) {
        size_t in_done=0,out_done=0;
        soxr_error_t err=soxr_process(ch->cvbs_resampler,
            input+consumed,count-consumed,&in_done,
            ch->cvbs_float+produced,MVD_RESAMPLE_CAP-produced,&out_done);
        if(err || (!in_done && !out_done) || produced+out_done>MVD_RESAMPLE_CAP) {
            mvd_resampler_clear(ch);
            return false;
        }
        consumed+=in_done;
        produced+=out_done;
    }
    /* Match Local GUI's deterministic FLOAT32->INT16 conversion: INT16
     * soxr output dither is nondeterministic and can disturb sync locking. */
    for(size_t i=0;i<produced;i++) {
        long v=lrintf(ch->cvbs_float[i]*32768.0f);
        if(v>32767)v=32767;
        if(v< -32768)v= -32768;
        ch->cvbs_samples[i]=(int16_t)v;
    }
    ch->cvbs->rate_trusted=true;
    if(produced)gui_cvbs_process_buffer(ch->cvbs,ch->cvbs_samples,produced,target);
    return true;
}
#endif

static int mvd_decode_thread(void *argument) {
    mvd_channel *ch=(mvd_channel *)argument;
    unsigned last_mode=~0u;
    uint32_t last_controls=UINT32_MAX;
    unsigned osd_tick=0;
    bool have_previous=false;
    gui_rf_block_info_t previous={0};
    while (atomic_load_explicit(&s_active,memory_order_acquire)) {
        if (!atomic_load_explicit(&ch->viewing,memory_order_acquire) ||
            atomic_load_explicit(&ch->mode,memory_order_acquire)>=2) {
            atomic_store_explicit(&ch->read_idx,
                atomic_load_explicit(&ch->write_idx,memory_order_acquire),
                memory_order_release);
            have_previous=false;
            mvd_wait();
            continue;
        }
        unsigned mode=atomic_load_explicit(&ch->mode,memory_order_acquire);
        uint32_t controls=atomic_load_explicit(&ch->controls,memory_order_acquire);
        if(mode!=last_mode || controls!=last_controls) {
            gui_cvbs_reset(ch->cvbs);
#if LIBSOXR_ENABLED
            mvd_resampler_clear(ch);
#endif
            ch->cvbs->decoder_mode=mode==1?CVBS_DECODER_MODE_TAPE:CVBS_DECODER_MODE_CVBS;
            /* Apply only on the owning decoder worker. No HTTP or capture
             * thread can mutate these unguarded Harry decoder fields. */
            unsigned system=mvd_ctl_get(controls,mode==1?MVD_CTL_TAPE_SYSTEM:MVD_CTL_SYSTEM,3u);
            gui_cvbs_set_format(ch->cvbs,(int)system);
            ch->cvbs->frame_view_mode=mvd_ctl_get(controls,MVD_CTL_FRAME,1u)?CVBS_FRAME_VIEW_FULL:CVBS_FRAME_VIEW_ACTIVE;
            ch->cvbs->level_mode=mvd_ctl_get(controls,MVD_CTL_LEVELS,1u)?CVBS_LEVEL_MODE_FIXED:CVBS_LEVEL_MODE_AUTO;
            ch->cvbs->fixed_levels_valid=false;
            ch->cvbs->sync_mode=mvd_ctl_get(controls,MVD_CTL_SYNC,1u)?CVBS_SYNC_MODE_FIXED:CVBS_SYNC_MODE_AUTO;
            ch->cvbs->fixed_sync_locked=false;
            ch->cvbs->colour_mode=mvd_ctl_get(controls,MVD_CTL_COLOUR,1u)?CVBS_COLOUR_AUTO:CVBS_COLOUR_OFF;
            ch->cvbs->prev_burst_valid=0;
            ch->cvbs->tape_format=(cvbs_tape_format_t)mvd_ctl_get(controls,MVD_CTL_TAPE_FAMILY,7u);
            ch->cvbs->tape_mode=(cvbs_tape_mode_t)mvd_ctl_get(controls,MVD_CTL_TAPE_SPEED,3u);
            last_mode=mode;
            last_controls=controls;
            have_previous=false;
        }
        uint64_t r=atomic_load_explicit(&ch->read_idx,memory_order_relaxed);
        if(r==atomic_load_explicit(&ch->write_idx,memory_order_acquire)) {
            mvd_wait(); continue;
        }
        mvd_input_slot *item=&ch->slots[r%MVD_INPUT_SLOTS];
        if(atomic_exchange_explicit(&ch->gap,false,memory_order_acq_rel) ||
           (have_previous &&
            (previous.epoch!=item->info.epoch ||
             previous.sample_rate_hz!=item->info.sample_rate_hz ||
             previous.start_sample!=item->info.start_sample)) ||
           item->info.discontinuity) {
            gui_cvbs_reset(ch->cvbs);
#if LIBSOXR_ENABLED
            mvd_resampler_clear(ch);
#endif
            have_previous=false;
        }
        if(item->n) {
            bool converted=false;
#if LIBSOXR_ENABLED
            /* Tape Decode intentionally retains its existing native-rate DSP
             * path. CVBS matches the Local GUI soxr resampling strategy. */
            if(mode==0)
                converted=mvd_cvbs_resampled(ch,item->samples,item->n,item->info.sample_rate_hz);
#endif
            if(!converted) {
                ch->cvbs->rate_trusted=false;
                gui_cvbs_process_buffer(ch->cvbs,item->samples,item->n,item->info.sample_rate_hz);
            }
            previous=item->info;
            previous.start_sample+=item->n;
            have_previous=true;
            if((++osd_tick & 127u)==0 &&
               !atomic_flag_test_and_set_explicit(&ch->osd_busy,memory_order_acquire)) {
                cvbs_decoder_t *d=ch->cvbs;
                snprintf(ch->cvbs_osd,sizeof(ch->cvbs_osd),
                    "%s | %s | source %.2f MHz\n"
                    "frames:%d sync_err:%d jumps:%d hsync/field:%d\n"
                    "VBI accepted:%d candidates:%d rejected:%d (cad:%d/par:%d)\n"
                    "rate %.1f MHz | VBI rej: %.120s",
                    gui_cvbs_get_format_name(d),d->last_weave?"weave":"bob",
                    item->info.sample_rate_hz/1000000.0,
                    d->state.frames_decoded,d->sync_errors,d->stream_jumps,
                    d->debug.hsyncs_last_field,
                    d->vsync.fields_accepted,d->vsync.vsync_candidates,
                    d->vsync.fields_rejected,d->vsync.reject_cadence,
                    d->vsync.reject_parity,
                    d->sample_rate_hz/1000000.0,
                    d->vsync.last_reject_reason[0]?d->vsync.last_reject_reason:"none");
                atomic_flag_clear_explicit(&ch->osd_busy,memory_order_release);
            }
        }
        atomic_store_explicit(&ch->read_idx,r+1,memory_order_release);
    }
#if LIBSOXR_ENABLED
    mvd_resampler_clear(ch);
#endif
    return 0;
}

int gui_monitor_video_server_start(void) {
    if (!gui_mvd_codec_available()) return -1;
    if (atomic_load(&s_active)) return 0;
    /* Resources are never published until all channels are configured. */
    if(!atomic_load(&s_initialized)) {
        for(unsigned i=0;i<2;i++){
            mvd_channel *ch=&s_channels[i];
            ch->slots=calloc(MVD_INPUT_SLOTS,sizeof(*ch->slots));
            ch->box=gui_monitor_video_framebox_create(MVD_MAX_WIDTH,MVD_MAX_HEIGHT);
            ch->cvbs=calloc(1,sizeof(*ch->cvbs));
            ch->rf=gui_rf_preview_worker_create(GUI_RF_PAL);
            if(!ch->slots || !ch->box || !ch->cvbs || !ch->rf ||
               !gui_cvbs_init(ch->cvbs)) {
                fprintf(stderr,"[M-VIDEO] Decoder/queue allocation failed (channel %u)\n",i);
                /* Keep partial allocations until process shutdown; never publish state. */
                return -1;
            }
            gui_cvbs_set_monitor_frame_sink(ch->cvbs,gui_monitor_video_framebox_cvbs_sink,ch->box);
            gui_rf_preview_worker_set_monitor_frame_sink(ch->rf,gui_monitor_video_framebox_rf_sink,ch->box);
            gui_rf_preview_worker_set_audio_enabled(ch->rf,false);
            gui_rf_preview_worker_set_paused(ch->rf,true);
            atomic_store(&ch->viewing,false);
            atomic_store(&ch->mode,0);
            atomic_store(&ch->controls,mvd_ctl_default(0));
            atomic_store(&ch->read_idx,0);
            atomic_store(&ch->write_idx,0);
            atomic_store(&ch->gap,false);
            atomic_flag_clear(&ch->osd_busy);
            ch->cvbs_osd[0]=0;
        }
        atomic_store(&s_initialized,true);
    }
    atomic_store(&s_active,true);
    for(unsigned i=0;i<2;i++){
        mvd_channel *ch=&s_channels[i];
        if(thrd_create(&ch->worker,mvd_decode_thread,ch)!=thrd_success){
            atomic_store(&s_active,false);
            for(unsigned j=0;j<i;j++) thrd_join(s_channels[j].worker,NULL);
            return -1;
        }
        ch->worker_started=true;
    }
    return 0;
}
void gui_monitor_video_server_stop(void) {
    if(!atomic_exchange(&s_active,false))return;
    for(unsigned c=0;c<2;c++){
        atomic_store(&s_channels[c].viewing,false);
        gui_rf_preview_worker_set_paused(s_channels[c].rf,true);
    }
    for(unsigned c=0;c<2;c++){
        if(s_channels[c].worker_started){thrd_join(s_channels[c].worker,NULL);s_channels[c].worker_started=false;}
    }
    /* Existing HTTP workers and capture producer must finish before their
     * slots/boxes can be freed. Here all resources intentionally stay
     * allocated so mode switches cannot race with detached socket workers. */
}
void gui_monitor_video_server_capture(const int16_t *a,const int16_t *b,
    size_t ac,size_t bc,const gui_rf_block_info_t info[2],bool input_supported) {
    if(!atomic_load_explicit(&s_active,memory_order_acquire) || !info)return;
    atomic_fetch_add(&s_capture_calls,1);
    const int16_t *src[2]={a,b};const size_t counts[2]={ac,bc};
    for(unsigned c=0;c<2;c++){
        mvd_channel *ch=&s_channels[c];
        if(!atomic_load_explicit(&ch->viewing,memory_order_acquire) ||
           !src[c] || !counts[c])continue;
        unsigned mode=atomic_load_explicit(&ch->mode,memory_order_relaxed);
        if(mode>=2){
            if(input_supported)
                (void)gui_rf_preview_worker_submit(ch->rf,src[c],counts[c],&info[c]);
            continue;
        }
        if(counts[c]>MVD_INPUT_SAMPLES){atomic_fetch_add(&s_queue_drops[c],1);atomic_store(&ch->gap,true);continue;}
        uint64_t w=atomic_load_explicit(&ch->write_idx,memory_order_relaxed);
        uint64_t r=atomic_load_explicit(&ch->read_idx,memory_order_acquire);
        if(w-r>=MVD_INPUT_SLOTS){atomic_fetch_add(&s_queue_drops[c],1);atomic_store(&ch->gap,true);continue;}
        mvd_input_slot *slot=&ch->slots[w%MVD_INPUT_SLOTS];
        memcpy(slot->samples,src[c],counts[c]*sizeof(int16_t));
        slot->n=counts[c];slot->info=info[c];
        atomic_store_explicit(&ch->write_idx,w+1,memory_order_release);
        atomic_fetch_add_explicit(&s_input_blocks[c],1,memory_order_relaxed);
    }
    atomic_fetch_sub(&s_capture_calls,1);
}
static int mvd_send_all(uintptr_t fd,const uint8_t *data,size_t size) {
    while(size){
        size_t n=size>65536u?65536u:size;
        int sent=(int)MVD_SEND_SOCKET(fd,data,n);
        if(sent<=0)return -1;
        data+=(size_t)sent;size-=(size_t)sent;
    }
    return 0;
}

bool gui_monitor_video_server_available(void) {
    return atomic_load_explicit(&s_active,memory_order_acquire);
}

void gui_monitor_video_server_stats(char *output,size_t capacity) {
    if(!output || !capacity)return;
    const mvd_channel *a=&s_channels[0], *b=&s_channels[1];
    unsigned enabled=gui_monitor_video_server_available()?1u:0u;
    snprintf(output,capacity,
        "{\"openh264\":%u,\"active\":%u,\"ch\":["
        "{\"viewing\":%u,\"mode\":%u,\"input_blocks\":%llu,\"dropped_blocks\":%llu,\"decoded_frames\":%llu,\"sent_frames\":%llu,\"encoder_attempts\":%llu,\"rc_skips\":%llu,\"encoded_bytes\":%llu,\"queue\":%llu},"
        "{\"viewing\":%u,\"mode\":%u,\"input_blocks\":%llu,\"dropped_blocks\":%llu,\"decoded_frames\":%llu,\"sent_frames\":%llu,\"encoder_attempts\":%llu,\"rc_skips\":%llu,\"encoded_bytes\":%llu,\"queue\":%llu}]}",
        gui_mvd_codec_available()?1u:0u,enabled,
        (unsigned)atomic_load(&a->viewing),(unsigned)atomic_load(&a->mode),
        (unsigned long long)atomic_load(&s_input_blocks[0]),
        (unsigned long long)atomic_load(&s_queue_drops[0]),
        (unsigned long long)gui_monitor_video_framebox_published(a->box),
        (unsigned long long)atomic_load(&s_encoded_frames[0]),
        (unsigned long long)atomic_load(&s_encoder_attempts[0]),
        (unsigned long long)atomic_load(&s_rc_skips[0]),
        (unsigned long long)atomic_load(&s_encoded_bytes[0]),
        (unsigned long long)(atomic_load(&a->write_idx)-atomic_load(&a->read_idx)),
        (unsigned)atomic_load(&b->viewing),(unsigned)atomic_load(&b->mode),
        (unsigned long long)atomic_load(&s_input_blocks[1]),
        (unsigned long long)atomic_load(&s_queue_drops[1]),
        (unsigned long long)gui_monitor_video_framebox_published(b->box),
        (unsigned long long)atomic_load(&s_encoded_frames[1]),
        (unsigned long long)atomic_load(&s_encoder_attempts[1]),
        (unsigned long long)atomic_load(&s_rc_skips[1]),
        (unsigned long long)atomic_load(&s_encoded_bytes[1]),
        (unsigned long long)(atomic_load(&b->write_idx)-atomic_load(&b->read_idx)));
}

/* Compact human-readable OSD wire view of the server's REAL decoder
 * statistics. This endpoint runs on an HTTP worker, never the capture thread.
 * The client merely draws these lines; no duplicate DSP logic lives there. */
bool gui_monitor_video_server_osd(unsigned channel,unsigned detail,
                                  char *output,size_t capacity) {
    if(!output || !capacity || channel>1 || detail>2)return false;
    output[0]=0;
    if(!detail || !atomic_load(&s_active))return true;
    mvd_channel *ch=&s_channels[channel];
    if(!atomic_load(&ch->viewing))return true;
    unsigned mode=atomic_load(&ch->mode);
    if(mode<2) {
        if(!atomic_flag_test_and_set_explicit(&ch->osd_busy,memory_order_acquire)) {
            snprintf(output,capacity,"%s",ch->cvbs_osd);
            atomic_flag_clear_explicit(&ch->osd_busy,memory_order_release);
        }
        if(detail==1) {
            char *line=strchr(output,'\n');
            if(line)*line=0;
        }
        return true;
    }
    gui_rf_preview_stats_t stats={0};
    gui_rf_preview_worker_get_stats(ch->rf,&stats);
    const gui_rf_decode_status_t *st=&stats.decode;
    uint32_t controls=atomic_load(&ch->controls);
    gui_rf_profile_t profile=(gui_rf_profile_t)mvd_ctl_rf_profile(controls);
    snprintf(output,capacity,
        "%s | %.3f -> %.0f MHz | H:%s V:%s Color:%s\n"
        "Decode %.1f fps | DSP %.1f ms | Age %.1f ms",
        gui_rf_profile_name(profile),
        stats.source_rate_hz/1000000.0,
        gui_rf_profile_video_rate_hz(profile)/1000000.0,
        st->h_locked?"LOCK":"SCAN",st->v_locked?"LOCK":"SCAN",
        st->color_locked?"LOCK":"OFF",
        stats.fps,stats.decode_ms,stats.latency_ms);
    if(detail<2)return true;
    size_t used=strlen(output);
    if(used<capacity)snprintf(output+used,capacity-used,
        "\nV candidates %llu | Fields %llu | Reject %llu (cad %llu/par %llu/map %llu)"
        "\nQueue drops %llu | Pressure skips %llu | Color loss %llu | CPU %.0f%% core"
        "\nLast field rejection: %.90s"
        "\n%s",
        (unsigned long long)stats.total_vsync_candidates,
        (unsigned long long)stats.total_fields_accepted,
        (unsigned long long)stats.total_fields_rejected,
        (unsigned long long)stats.total_reject_cadence,
        (unsigned long long)stats.total_reject_parity,
        (unsigned long long)stats.total_reject_line_map,
        (unsigned long long)stats.blocks_dropped,
        (unsigned long long)stats.pause_drops,
        (unsigned long long)stats.total_color_losses,stats.cpu_percent,
        st->last_reject_reason[0]?st->last_reject_reason:"none",
        stats.paused?"Preview paused: capture/recording pressure":
        stats.budget_waiting?"Preview yielding to shared processing budget":st->reason);
    return true;
}

/* Reproduce the current Harry Local CVBS Active/Full render selection on
 * the SERVER, before RGB->I420 conversion. These timing constants are a
 * Monitor-only compatibility adapter: future upstream decoder geometry
 * changes must be re-integrated here, never in the Monitor Client. */
static void mvd_cvbs_active_crop(uint32_t controls,unsigned width,unsigned height,
                                unsigned *left,unsigned *top,
                                unsigned *crop_width,unsigned *crop_height) {
    *left=0;*top=0;*crop_width=width;*crop_height=height;
    if(mvd_ctl_get(controls,MVD_CTL_FRAME,1u) || width<32 || height<32)return;
    bool ntsc=mvd_ctl_get(controls,MVD_CTL_SYSTEM,3u)==1;
    const double line_us=ntsc?63.55555555555556:64.0;
    const double ax0_us=ntsc?9.45:10.5;
    const double ax1_us=ntsc?62.55555555555556:62.5;
    /* The Local 4fsc resampler emits 14.3/17.9 MSPS on this path. */
    const double rate=ntsc?14300000.0:17900000.0;
    const double edge_delay_us=8000000.0/rate;
    int x0=(int)((ax0_us-edge_delay_us)*width/line_us+0.5);
    int x1=(int)((ax1_us-edge_delay_us)*width/line_us+0.5);
    unsigned y0=ntsc?40u:44u;
    unsigned rh=ntsc?484u:576u;
    if(x0<0)x0=0;
    if(x1>(int)width)x1=(int)width;
    if(y0>=height || x0>=x1 || y0+rh>height)return;
    *left=(unsigned)x0;*top=y0;
    *crop_width=(unsigned)(x1-x0);
    *crop_height=rh;
}

bool gui_monitor_video_server_stream(uintptr_t fd,unsigned channel,
    unsigned mode,unsigned bitrate_bps,unsigned fps_x100,uint32_t controls,
    uint32_t client_codecs) {
    if(!atomic_load(&s_active) || channel>1 || !(client_codecs & MVD_CODEC_ALL) ||
       !mvd_ctl_valid(controls,mode) ||
       bitrate_bps<64000u || bitrate_bps>4000000u ||
       fps_x100<500u || fps_x100>3000u)return false;
    mvd_channel *ch=&s_channels[channel];
#if defined(SO_NOSIGPIPE)
    /* macOS: do not terminate the entire capture process if a monitor
     * viewer disconnects during a send(). MSG_NOSIGNAL covers Linux. */
    { int no_sigpipe=1;
      (void)setsockopt((int)fd,SOL_SOCKET,SO_NOSIGPIPE,
                      (const void *)&no_sigpipe,sizeof(no_sigpipe)); }
#endif
    if(atomic_exchange(&ch->viewing,true))return false;
    atomic_fetch_add(&s_stream_calls,1);
    atomic_store(&ch->controls,controls);
    atomic_store(&ch->mode,mode);
    atomic_store(&ch->gap,true);
    if(mode>=2) {
        gui_rf_preview_worker_set_profile(ch->rf,(gui_rf_profile_t)mvd_ctl_rf_profile(controls));
        gui_rf_preview_worker_set_frame_mode(ch->rf,mvd_ctl_get(controls,MVD_CTL_FRAME,1u)!=0);
        gui_rf_preview_worker_set_budget(mvd_ctl_budget(controls)/100.0);
    }
    gui_rf_preview_worker_set_paused(ch->rf,mode<2);
    const char response[]="HTTP/1.0 200 OK\r\nContent-Type: application/x-misrc-video\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    uint8_t *source=malloc(MVD_PIXEL_CAP);
    uint8_t *i420=malloc(mvd_i420_buffer_size(640,480));
    uint8_t *encoded=malloc(MVD_MAX_PAYLOAD);
    mvd_codec_encoder *encoder=NULL;
    unsigned enc_w=0,enc_h=0,sequence=0;
    uint64_t previous_gen=gui_monitor_video_framebox_published(ch->box);
    uint64_t last_submitted_us=0;
    time_t last_heartbeat=time(NULL),last_frame_log=last_heartbeat;
    if(source && i420 && encoded &&
       mvd_send_all(fd,(const uint8_t *)response,sizeof(response)-1)==0){
        while(atomic_load(&s_active)){
            gui_monitor_video_frame_info_t info;
            if(!gui_monitor_video_framebox_take(ch->box,source,MVD_PIXEL_CAP,
                                               &previous_gen,&info)) {
                time_t now=time(NULL);
                if(now>last_frame_log && now-last_frame_log>=10){
                    fprintf(stderr,"[M-VIDEO] CH%u mode %u: awaiting decoded frames; input blocks=%llu queue drops=%llu decoded=%llu\n",
                        channel+1,mode,(unsigned long long)atomic_load(&s_input_blocks[channel]),
                        (unsigned long long)atomic_load(&s_queue_drops[channel]),
                        (unsigned long long)gui_monitor_video_framebox_published(ch->box));
                    last_frame_log=now;
                }
                if(now>last_heartbeat && now-last_heartbeat>=1){
                    mvd_frame_header keep={0};
                    keep.codec=(uint8_t)(encoder?mvd_video_encoder_codec(encoder):MVD_CODEC_H264_ANNEXB);
                    keep.flags=MVD_FLAG_HEARTBEAT;
                    keep.stream_id=channel+1;
                    keep.sequence=sequence++;
                    keep.width=320;keep.height=240;
                    keep.fps_x100=(uint16_t)fps_x100;
                    keep.payload_bytes=4;
                    uint8_t head[MVD_HEADER_BYTES],dummy[4]={0};
                    if(mvd_header_encode(head,&keep)!=0 ||
                       mvd_send_all(fd,head,sizeof(head))!=0 ||
                       mvd_send_all(fd,dummy,sizeof(dummy))!=0)break;
                    last_heartbeat=now;
                }
                thrd_sleep_ms(2);continue;
            }
            /* Limit actual ENCODER INPUT to the requested FPS. A skipped
             * compressed frame must not cause an immediate retry of the
             * next source frame (which previously led to >25 FPS bursts). */
            if(last_submitted_us &&
               (info.timestamp_us<=last_submitted_us ||
                info.timestamp_us-last_submitted_us<100000000ULL/fps_x100))
                continue;
            last_submitted_us=info.timestamp_us;
            unsigned crop_x=0,crop_y=0,crop_w=info.width,crop_h=info.height;
            if(mode<2 && mode==0)
                mvd_cvbs_active_crop(controls,info.width,info.height,
                                     &crop_x,&crop_y,&crop_w,&crop_h);
            unsigned w=crop_w>640?640:crop_w;
            unsigned h=crop_h>480?480:crop_h;
            w &= ~1u;h &= ~1u;
            if(w<2 || h<2)continue;
            if(!encoder || enc_w!=w || enc_h!=h){
                mvd_video_encoder_free(encoder);
                encoder=mvd_video_encoder_best(client_codecs & MVD_CODEC_ALL,
                                                 w,h,fps_x100,bitrate_bps);
                enc_w=w;enc_h=h;
                if(!encoder)break;
                atomic_store(&s_codec_choice[channel],mvd_video_encoder_codec(encoder));
            }
            size_t src_size=(size_t)info.width*info.height*info.channels;
            size_t start=((size_t)crop_y*info.width+crop_x)*info.channels;
            size_t dst_size=mvd_i420_buffer_size(w,h);
            if(start>=src_size ||
               mvd_rgb_to_i420(source+start,src_size-start,crop_w,crop_h,
                    (size_t)info.width*info.channels,(mvd_rgb_format)info.channels,
                    i420,mvd_i420_buffer_size(640,480),w,h)!=0)break;
            size_t bytes=0;int key=0;
            atomic_fetch_add_explicit(&s_encoder_attempts[channel],1,memory_order_relaxed);
            int rc=mvd_video_encoder_frame(encoder,i420,dst_size,info.timestamp_us,
                                        encoded,MVD_MAX_PAYLOAD,&bytes,&key);
            if(rc<0)break;
            if(rc==0){
                atomic_fetch_add_explicit(&s_rc_skips[channel],1,memory_order_relaxed);
                continue;
            }
            mvd_frame_header hdr={0};
            hdr.codec=(uint8_t)mvd_video_encoder_codec(encoder);
            hdr.flags=key?(MVD_FLAG_KEYFRAME|MVD_FLAG_CODEC_CONFIG):0;
            hdr.stream_id=channel+1;
            hdr.sequence=sequence++;
            hdr.pts_us=info.timestamp_us;
            hdr.width=(uint16_t)w;hdr.height=(uint16_t)h;
            hdr.fps_x100=(uint16_t)fps_x100;
            hdr.payload_bytes=(uint32_t)bytes;
            uint8_t head[MVD_HEADER_BYTES];
            if(mvd_header_encode(head,&hdr)!=0 ||
               mvd_send_all(fd,head,sizeof(head))!=0 ||
               mvd_send_all(fd,encoded,bytes)!=0)break;
            atomic_fetch_add_explicit(&s_encoded_frames[channel],1,memory_order_relaxed);
            atomic_fetch_add_explicit(&s_encoded_bytes[channel],bytes,memory_order_relaxed);
            last_heartbeat=time(NULL);
            last_frame_log=last_heartbeat;
        }
    }
    mvd_video_encoder_free(encoder);
    atomic_store(&s_codec_choice[channel],0);
    free(source);free(i420);free(encoded);
    gui_rf_preview_worker_set_paused(ch->rf,true);
    atomic_store(&ch->viewing,false);
    atomic_fetch_sub(&s_stream_calls,1);
    return true;
}
