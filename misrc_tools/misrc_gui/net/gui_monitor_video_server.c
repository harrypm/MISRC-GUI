#include "gui_monitor_video_server.h"
#include "gui_monitor_video_codec.h"
#include "gui_monitor_video_framebox.h"
#include "gui_monitor_video_i420.h"
#include "gui_monitor_video_wire.h"
#include "../signal/gui_cvbs.h"
#include "../../common/threading.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    bool worker_started;
} mvd_channel;
static mvd_channel s_channels[2];
static atomic_bool s_active;
static atomic_uint s_capture_calls,s_stream_calls;
/* Monotonic, lock-free diagnostic counters; never used in the RF hot path for I/O. */
static atomic_uint_fast64_t s_input_blocks[2],s_queue_drops[2],s_encoded_frames[2];
static atomic_bool s_initialized;
static void mvd_wait(void) { thrd_sleep_ms(1); }

static int mvd_decode_thread(void *argument) {
    mvd_channel *ch=(mvd_channel *)argument;
    unsigned last_mode=~0u;
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
        if(mode!=last_mode) {
            gui_cvbs_reset(ch->cvbs);
            ch->cvbs->decoder_mode=mode==1?CVBS_DECODER_MODE_TAPE:CVBS_DECODER_MODE_CVBS;
            last_mode=mode;
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
            have_previous=false;
        }
        if(item->n) {
            gui_cvbs_process_buffer(ch->cvbs,item->samples,item->n,item->info.sample_rate_hz);
            previous=item->info;
            previous.start_sample+=item->n;
            have_previous=true;
        }
        atomic_store_explicit(&ch->read_idx,r+1,memory_order_release);
    }
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
            atomic_store(&ch->read_idx,0);
            atomic_store(&ch->write_idx,0);
            atomic_store(&ch->gap,false);
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
        "{\"viewing\":%u,\"mode\":%u,\"input_blocks\":%llu,\"dropped_blocks\":%llu,\"decoded_frames\":%llu,\"sent_frames\":%llu,\"queue\":%llu},"
        "{\"viewing\":%u,\"mode\":%u,\"input_blocks\":%llu,\"dropped_blocks\":%llu,\"decoded_frames\":%llu,\"sent_frames\":%llu,\"queue\":%llu}]}",
        gui_mvd_codec_available()?1u:0u,enabled,
        (unsigned)atomic_load(&a->viewing),(unsigned)atomic_load(&a->mode),
        (unsigned long long)atomic_load(&s_input_blocks[0]),
        (unsigned long long)atomic_load(&s_queue_drops[0]),
        (unsigned long long)gui_monitor_video_framebox_published(a->box),
        (unsigned long long)atomic_load(&s_encoded_frames[0]),
        (unsigned long long)(atomic_load(&a->write_idx)-atomic_load(&a->read_idx)),
        (unsigned)atomic_load(&b->viewing),(unsigned)atomic_load(&b->mode),
        (unsigned long long)atomic_load(&s_input_blocks[1]),
        (unsigned long long)atomic_load(&s_queue_drops[1]),
        (unsigned long long)gui_monitor_video_framebox_published(b->box),
        (unsigned long long)atomic_load(&s_encoded_frames[1]),
        (unsigned long long)(atomic_load(&b->write_idx)-atomic_load(&b->read_idx)));
}

bool gui_monitor_video_server_stream(uintptr_t fd,unsigned channel,
    unsigned mode,unsigned bitrate_bps,unsigned fps_x100) {
    if(!atomic_load(&s_active) || channel>1 || mode>3 ||
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
    atomic_store(&ch->mode,mode);
    atomic_store(&ch->gap,true);
    if(mode>=2)gui_rf_preview_worker_set_profile(ch->rf,mode==3?GUI_RF_PROFILE_NTSC_VHS:GUI_RF_PROFILE_PAL_VHS);
    gui_rf_preview_worker_set_paused(ch->rf,mode<2);
    const char response[]="HTTP/1.0 200 OK\r\nContent-Type: application/x-misrc-h264\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    uint8_t *source=malloc(MVD_PIXEL_CAP);
    uint8_t *i420=malloc(mvd_i420_buffer_size(640,480));
    uint8_t *encoded=malloc(MVD_MAX_PAYLOAD);
    gui_mvd_encoder_t *encoder=NULL;
    unsigned enc_w=0,enc_h=0,sequence=0;
    uint64_t previous_gen=gui_monitor_video_framebox_published(ch->box);
    uint64_t last_sent_us=0;
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
                    keep.codec=MVD_CODEC_H264_ANNEXB;
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
            if(last_sent_us && info.timestamp_us>last_sent_us &&
               info.timestamp_us-last_sent_us < 100000000ULL/fps_x100)continue;
            unsigned w=info.width>640?640:info.width;
            unsigned h=info.height>480?480:info.height;
            w &= ~1u;h &= ~1u;
            if(w<2 || h<2)continue;
            if(!encoder || enc_w!=w || enc_h!=h){
                gui_mvd_encoder_destroy(encoder);
                encoder=gui_mvd_encoder_create(w,h,fps_x100,bitrate_bps);
                enc_w=w;enc_h=h;
                if(!encoder)break;
                gui_mvd_encoder_request_keyframe(encoder);
            }
            size_t src_size=(size_t)info.width*info.height*info.channels;
            size_t dst_size=mvd_i420_buffer_size(w,h);
            if(mvd_rgb_to_i420(source,src_size,info.width,info.height,
                    (size_t)info.width*info.channels,(mvd_rgb_format)info.channels,
                    i420,mvd_i420_buffer_size(640,480),w,h)!=0)break;
            size_t bytes=0;int key=0;
            int rc=gui_mvd_encoder_frame(encoder,i420,dst_size,info.timestamp_us,
                                        encoded,MVD_MAX_PAYLOAD,&bytes,&key);
            if(rc<0)break;
            if(rc==0)continue;
            mvd_frame_header hdr={0};
            hdr.codec=MVD_CODEC_H264_ANNEXB;
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
            last_sent_us=info.timestamp_us;
            atomic_fetch_add_explicit(&s_encoded_frames[channel],1,memory_order_relaxed);
            last_heartbeat=time(NULL);
            last_frame_log=last_heartbeat;
        }
    }
    gui_mvd_encoder_destroy(encoder);
    free(source);free(i420);free(encoded);
    gui_rf_preview_worker_set_paused(ch->rf,true);
    atomic_store(&ch->viewing,false);
    atomic_fetch_sub(&s_stream_calls,1);
    return true;
}
