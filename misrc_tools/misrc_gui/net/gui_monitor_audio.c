/* Opus Audio Mon exclusively for MISRC-M/2 Monitor & Remote.
 * Server capture tap -> bounded 4ch/24-bit ring -> per-subscriber mono mix +
 * libsoxr 48k resample + 10ms Opus encoding. Client receives a separate HTTP
 * stream and feeds a bounded 48k mono jitter queue to a raylib callback.
 * No raw /baseband transfer, no changes to legacy Client or recording. */
#include "gui_monitor_audio.h"
#include "gui_monitor_audio_wire.h"
#include "gui_monitor_transport.h"
#include "gui_net.h"
#include "../../common/threading.h"
#include <opus.h>
#include <soxr.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
#include <ws2tcpip.h>
typedef SOCKET ma_sock_t;
#define MA_BAD INVALID_SOCKET
#define MA_CLOSE closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
typedef int ma_sock_t;
#define MA_BAD (-1)
#define MA_CLOSE close
#endif

#define MA_RAW_FRAME_BYTES 12u
#define MA_RING_FRAMES 32768u
#define MA_READ_FRAMES 1024u
#define MA_PCM_QUEUE 9600u
#define MA_PCM_MAX_LAG 4800u
#define MA_PCM_PREROLL 1920u

static struct {
    atomic_flag guard;
    atomic_uint subscribers;
    uint8_t frames[MA_RING_FRAMES * MA_RAW_FRAME_BYTES];
    uint64_t written;
    uint32_t rate;
    unsigned mode;
    uint32_t epoch;
} g_ma_capture = {.guard=ATOMIC_FLAG_INIT};

static bool ma_trylock(atomic_flag *flag) {
    return !atomic_flag_test_and_set_explicit(flag, memory_order_acquire);
}
static void ma_unlock(atomic_flag *flag) {
    atomic_flag_clear_explicit(flag, memory_order_release);
}
static void ma_lock(atomic_flag *flag) {
    while (!ma_trylock(flag)) thrd_sleep_ms(1);
}

void gui_monitor_audio_server_reset(void)
{
    ma_lock(&g_ma_capture.guard);
    g_ma_capture.written=0;
    g_ma_capture.rate=0;
    g_ma_capture.mode=0;
    ++g_ma_capture.epoch;
    ma_unlock(&g_ma_capture.guard);
}

/* This tap is the same low-latency tap used by the original /baseband fanout,
 * but it never blocks the hardware callback and does nothing with no listeners.
 * 24-bit interleaved input: four 3-byte little-endian channels per frame. */
void gui_monitor_audio_server_tap(gui_app_t *app, const void *data, size_t bytes)
{
    if (!app || !data || !bytes || !app->is_capturing ||
        !atomic_load(&g_ma_capture.subscribers)) return;
    size_t frames=bytes / MA_RAW_FRAME_BYTES;
    if (!frames || !ma_trylock(&g_ma_capture.guard)) return;
    device_type_t type=DEVICE_TYPE_HSDAOH;
    if (app->selected_device>=0 && app->selected_device<app->device_count)
        type=app->devices[app->selected_device].type;
    unsigned mode=(type==DEVICE_TYPE_CXADC) ? 1u :
                  (type==DEVICE_TYPE_MISRC_CLOCKGEN ? 2u : 0u);
    uint32_t rate=78125u;
    if (mode) {
        rate=atomic_load(&app->audio_sample_rate);
        if (rate<8000u || rate>384000u) rate=46875u;
    }
    if (rate!=g_ma_capture.rate || mode!=g_ma_capture.mode) {
        g_ma_capture.rate=rate;
        g_ma_capture.mode=mode;
        g_ma_capture.written=0;
        ++g_ma_capture.epoch;
    }
    const uint8_t *input=(const uint8_t *)data;
    if (frames>MA_RING_FRAMES) {
        input+=(frames-MA_RING_FRAMES)*MA_RAW_FRAME_BYTES;
        frames=MA_RING_FRAMES;
    }
    size_t offset=(size_t)(g_ma_capture.written % MA_RING_FRAMES);
    size_t first=frames < MA_RING_FRAMES-offset ? frames : MA_RING_FRAMES-offset;
    memcpy(g_ma_capture.frames + offset*MA_RAW_FRAME_BYTES,
           input, first*MA_RAW_FRAME_BYTES);
    if (first<frames)
        memcpy(g_ma_capture.frames, input+first*MA_RAW_FRAME_BYTES,
               (frames-first)*MA_RAW_FRAME_BYTES);
    g_ma_capture.written+=frames;
    ma_unlock(&g_ma_capture.guard);
}

typedef struct {
    uint64_t cursor;
    uint32_t epoch;
} ma_reader_t;

/* Reader never holds the capture lock while doing DSP or network I/O. */
static size_t ma_read_capture(ma_reader_t *r, uint8_t *output,
                              uint32_t *rate, unsigned *mode, bool *discontinuity)
{
    ma_lock(&g_ma_capture.guard);
    uint64_t written=g_ma_capture.written;
    *rate=g_ma_capture.rate;
    *mode=g_ma_capture.mode;
    *discontinuity=false;
    if (r->epoch!=g_ma_capture.epoch || r->cursor>written) {
        r->epoch=g_ma_capture.epoch;
        r->cursor=written>0 && *rate ? written-((*rate/30u)<written ? *rate/30u : written) : written;
        *discontinuity=true;
    }
    /* Never accumulate latency after an overrun / slow receiver: jump to the
     * newest ~30 ms. This may skip captured audio, never delay capture. */
    if (written-r->cursor>MA_RING_FRAMES ||
        (*rate && written-r->cursor > *rate/5u)) {
        uint64_t back=*rate/30u;
        r->cursor=written>back ? written-back : 0;
        *discontinuity=true;
    }
    size_t frames=(size_t)(written-r->cursor);
    if (frames>MA_READ_FRAMES) frames=MA_READ_FRAMES;
    if (!*rate) frames=0;
    if (frames) {
        size_t offset=(size_t)(r->cursor%MA_RING_FRAMES);
        size_t first=frames<MA_RING_FRAMES-offset ? frames : MA_RING_FRAMES-offset;
        memcpy(output,g_ma_capture.frames+offset*MA_RAW_FRAME_BYTES,
               first*MA_RAW_FRAME_BYTES);
        if(first<frames)
            memcpy(output+first*MA_RAW_FRAME_BYTES,g_ma_capture.frames,
                   (frames-first)*MA_RAW_FRAME_BYTES);
        r->cursor+=frames;
    }
    ma_unlock(&g_ma_capture.guard);
    return frames;
}
static bool ma_socket_peer_closed(uintptr_t fd)
{
    ma_sock_t sock=(ma_sock_t)fd;
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock,&rset);
    struct timeval timeout={0,0};
    int ready=select((int)sock+1,&rset,NULL,NULL,&timeout);
    if(ready<=0 || !FD_ISSET(sock,&rset))return false;
    char probe;
    int n=recv(sock,&probe,1,MSG_PEEK);
    return n==0; /* A new request is never expected on this connection. */
}
static int32_t ma_read_s24(const uint8_t *frame, unsigned ch)
{
    const uint8_t *p=frame+ch*3u;
    uint32_t v=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16);
    if(v & 0x800000u) v|=0xff000000u;
    return (int32_t)v;
}
static int16_t ma_mono_sample(const uint8_t *frame, unsigned pair, unsigned mode)
{
    int32_t pcm;
    if (pair==1 && mode==1) {
        /* CXADC: CH3 is headswitch, heard dual-mono in the local Audio Mon. */
        pcm=ma_read_s24(frame,2);
    } else {
        /* MISRC clockgen CH3/4 can contain invalid data after Windows
         * resampling: follow the existing local monitor CH1/2 fallback. */
        unsigned first=(pair==1 && mode!=2) ? 2u : 0u;
        int32_t a=ma_read_s24(frame,first), b=ma_read_s24(frame,first+1u);
        pcm=a/2 + b/2;
    }
    return (int16_t)(pcm>>8);
}
bool gui_monitor_audio_server_stream(uintptr_t fd, gui_app_t *app, unsigned pair,
                                     gui_monitor_audio_send_fn send_packet)
{
    if(!app || !send_packet || pair>1 || !app->is_capturing ||
       !gui_monitor_server_active()) return false;
    int error=0;
    OpusEncoder *encoder=opus_encoder_create(MA_OPUS_RATE,1,OPUS_APPLICATION_AUDIO,&error);
    if(!encoder || error!=OPUS_OK) return false;
    (void)opus_encoder_ctl(encoder,OPUS_SET_BITRATE(80000));
    (void)opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(5));
    (void)opus_encoder_ctl(encoder,OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    atomic_fetch_add(&g_ma_capture.subscribers,1);
    const char *header="HTTP/1.0 200 OK\r\nContent-Type: application/x-misrc-opus\r\n"
                       "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    bool ok=send_packet(fd,header,strlen(header))==0;
    ma_reader_t reader={0};
    soxr_t resampler=NULL;
    uint32_t current_rate=0;
    uint32_t sequence=0;
    uint8_t raw[MA_READ_FRAMES*MA_RAW_FRAME_BYTES];
    int16_t mixed[MA_READ_FRAMES];
    int16_t converted[MA_READ_FRAMES*2];
    int16_t pending[MA_OPUS_FRAME];
    size_t have=0;
    uint8_t payload[MA_WIRE_MAX_PAYLOAD];
    while(ok && gui_monitor_server_active() && app->is_capturing) {
        uint32_t rate=0;unsigned mode=0;bool discontinuity=false;
        size_t frames=ma_read_capture(&reader,raw,&rate,&mode,&discontinuity);
        if(discontinuity || rate!=current_rate) {
            if(sequence)++sequence; /* tell decoder to reset after a gap */
            have=0;
            (void)opus_encoder_ctl(encoder,OPUS_RESET_STATE);
            if(resampler){soxr_delete(resampler);resampler=NULL;}
            current_rate=rate;
        }
        if(!resampler && rate) {
            soxr_error_t soxr_error=NULL;
            soxr_io_spec_t io=soxr_io_spec(SOXR_INT16_I,SOXR_INT16_I);
            soxr_quality_spec_t quality=soxr_quality_spec(SOXR_HQ,0);
            resampler=soxr_create((double)rate,(double)MA_OPUS_RATE,1,
                                  &soxr_error,&io,&quality,NULL);
            if(soxr_error || !resampler){ok=false;break;}
        }
        if(!frames || !resampler) {
            if(ma_socket_peer_closed(fd))break;
            thrd_sleep_ms(5);
            continue;
        }
        for(size_t i=0;i<frames;i++)
            mixed[i]=ma_mono_sample(raw+i*MA_RAW_FRAME_BYTES,pair,mode);
        size_t in_at=0;
        while(in_at<frames && ok) {
            size_t used=0,produced=0;
            soxr_error_t e=soxr_process(resampler,mixed+in_at,frames-in_at,&used,
                                         converted,MA_READ_FRAMES*2,&produced);
            if(e || (!used && !produced)){ok=false;break;}
            in_at+=used;
            for(size_t j=0;j<produced && ok;j++) {
                pending[have++]=converted[j];
                if(have==MA_OPUS_FRAME) {
                    int len=opus_encode(encoder,pending,MA_OPUS_FRAME,payload,sizeof(payload));
                    if(len<=0 || len>(int)MA_WIRE_MAX_PAYLOAD){ok=false;break;}
                    uint8_t h[MA_WIRE_HEADER];
                    if(!ma_wire_pack(h,sequence++,(size_t)len) ||
                       send_packet(fd,(const char*)h,sizeof(h))!=0 ||
                       send_packet(fd,(const char*)payload,(size_t)len)!=0)
                        ok=false;
                    have=0;
                }
            }
        }
    }
    if(resampler)soxr_delete(resampler);
    opus_encoder_destroy(encoder);
    atomic_fetch_sub(&g_ma_capture.subscribers,1);
    return true; /* HTTP response was handled, including normal disconnect. */
}

/* -- Monitor-only client: independent socket, decoder and bounded jitter. -- */
static struct {
    atomic_flag guard;
    int16_t pcm[MA_PCM_QUEUE];
    size_t head,count;
    bool primed;
} g_ma_pcm={.guard=ATOMIC_FLAG_INIT};

static void ma_pcm_clear(void)
{
    ma_lock(&g_ma_pcm.guard);
    g_ma_pcm.head=g_ma_pcm.count=0;
    g_ma_pcm.primed=false;
    ma_unlock(&g_ma_pcm.guard);
}
static void ma_pcm_push(const int16_t *pcm,size_t count)
{
    if(!pcm || !count) return;
    if(!ma_trylock(&g_ma_pcm.guard))return; /* never stall decoder or render */
    if(count>MA_PCM_QUEUE){pcm+=count-MA_PCM_QUEUE;count=MA_PCM_QUEUE;}
    if(g_ma_pcm.count+count>MA_PCM_MAX_LAG) {
        /* Bound latency to roughly 40 ms after a stalled audio device. */
        size_t target=MA_PCM_PREROLL;
        if(g_ma_pcm.count>target) {
            g_ma_pcm.head=(g_ma_pcm.head+g_ma_pcm.count-target)%MA_PCM_QUEUE;
            g_ma_pcm.count=target;
        }
    }
    if(count>MA_PCM_QUEUE-g_ma_pcm.count) {
        size_t drop=count-(MA_PCM_QUEUE-g_ma_pcm.count);
        g_ma_pcm.head=(g_ma_pcm.head+drop)%MA_PCM_QUEUE;
        g_ma_pcm.count-=drop;
    }
    size_t tail=(g_ma_pcm.head+g_ma_pcm.count)%MA_PCM_QUEUE;
    size_t first=count<MA_PCM_QUEUE-tail ? count : MA_PCM_QUEUE-tail;
    memcpy(g_ma_pcm.pcm+tail,pcm,first*sizeof(*pcm));
    if(first<count)memcpy(g_ma_pcm.pcm,pcm+first,(count-first)*sizeof(*pcm));
    g_ma_pcm.count+=count;
    ma_unlock(&g_ma_pcm.guard);
}
static void ma_playback_callback(void *out,unsigned int frames)
{
    int16_t *pcm=(int16_t*)out;
    if(!out)return;
    memset(out,0,(size_t)frames*sizeof(*pcm));
    if(!ma_trylock(&g_ma_pcm.guard))return;
    if(!g_ma_pcm.primed && g_ma_pcm.count>=MA_PCM_PREROLL)g_ma_pcm.primed=true;
    if(g_ma_pcm.primed) {
        size_t n=frames<g_ma_pcm.count ? (size_t)frames : g_ma_pcm.count;
        size_t first=n<MA_PCM_QUEUE-g_ma_pcm.head ? n : MA_PCM_QUEUE-g_ma_pcm.head;
        memcpy(pcm,g_ma_pcm.pcm+g_ma_pcm.head,first*sizeof(*pcm));
        if(first<n)memcpy(pcm+first,g_ma_pcm.pcm,(n-first)*sizeof(*pcm));
        g_ma_pcm.head=(g_ma_pcm.head+n)%MA_PCM_QUEUE;
        g_ma_pcm.count-=n;
        if(n<(size_t)frames)g_ma_pcm.primed=false;
    }
    ma_unlock(&g_ma_pcm.guard);
}
static struct {
    gui_app_t *app;
    char host[128];
    uint16_t port;
    thrd_t thread;
    atomic_bool running,stop,requested;
    atomic_uint pair;
    AudioStream output;
    bool output_active; /* GUI-thread only */
} g_ma_client;

static void ma_set_timeout(ma_sock_t sock,unsigned ms)
{
#ifdef _WIN32
    DWORD d=ms;
    (void)setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char*)&d,sizeof(d));
    (void)setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(const char*)&d,sizeof(d));
#else
    struct timeval t={(long)(ms/1000u),(long)((ms%1000u)*1000u)};
    (void)setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));
    (void)setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
#endif
}
static int ma_read_full(ma_sock_t sock, void *buffer,size_t length)
{
    uint8_t *p=(uint8_t*)buffer;
    while(length) {
        int n=recv(sock,(char*)p,(int)(length>4096?4096:length),0);
        if(n<=0)return -1;
        p+=(size_t)n;length-=(size_t)n;
    }
    return 0;
}
static ma_sock_t ma_connect_audio(const char *host,uint16_t port,unsigned pair)
{
    char service[12];snprintf(service,sizeof(service),"%u",(unsigned)port);
    struct addrinfo hints={0},*addrs=NULL;
    hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,service,&hints,&addrs)!=0)return MA_BAD;
    ma_sock_t sock=MA_BAD;
    for(struct addrinfo *p=addrs;p;p=p->ai_next){
        sock=socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(sock==MA_BAD)continue;
        ma_set_timeout(sock,350);
        if(connect(sock,p->ai_addr,(int)p->ai_addrlen)==0)break;
        MA_CLOSE(sock);sock=MA_BAD;
    }
    freeaddrinfo(addrs);
    if(sock==MA_BAD)return MA_BAD;
    char request[256];
    int n=snprintf(request,sizeof(request),
        "GET /monitor/audio?pair=%u HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        pair,host);
    if(n<=0 || n>=(int)sizeof(request)){MA_CLOSE(sock);return MA_BAD;}
    size_t sent=0;
    while(sent<(size_t)n){
        int k=send(sock,request+sent,n-(int)sent,0);
        if(k<=0){MA_CLOSE(sock);return MA_BAD;}
        sent+=(size_t)k;
    }
    char header[512]={0};
    size_t count=0;
    while(count<sizeof(header)-1) {
        if(ma_read_full(sock,header+count,1)!=0)break;
        count++;
        if(count>=4 && !memcmp(header+count-4,"\r\n\r\n",4))break;
    }
    if(count<15 || count==sizeof(header)-1 ||
       (strncmp(header,"HTTP/1.0 200 ",13)!=0 &&
        strncmp(header,"HTTP/1.1 200 ",13)!=0)){
        MA_CLOSE(sock);return MA_BAD;
    }
    return sock;
}
static bool ma_client_should_run(unsigned pair)
{
    return !atomic_load(&g_ma_client.stop) &&
           atomic_load(&g_ma_client.requested) &&
           atomic_load(&g_ma_client.pair)==pair &&
           gui_monitor_client_connected() && gui_monitor_client_peer_state()>0;
}
static int ma_client_thread(void *unused)
{
    (void)unused;
    while(!atomic_load(&g_ma_client.stop)) {
        unsigned pair=atomic_load(&g_ma_client.pair);
        if(!ma_client_should_run(pair)){thrd_sleep_ms(80);continue;}
        ma_sock_t sock=ma_connect_audio(g_ma_client.host,g_ma_client.port,pair);
        if(sock==MA_BAD){thrd_sleep_ms(350);continue;}
        int error=0;
        OpusDecoder *decoder=opus_decoder_create(MA_OPUS_RATE,1,&error);
        if(!decoder || error!=OPUS_OK){
            if(decoder)opus_decoder_destroy(decoder);
            MA_CLOSE(sock);thrd_sleep_ms(350);continue;
        }
        ma_pcm_clear();
        bool have_sequence=false;
        uint32_t last_sequence=0;
        while(ma_client_should_run(pair)) {
            uint8_t header[MA_WIRE_HEADER];
            uint32_t seq=0;size_t bytes=0;
            if(ma_read_full(sock,header,sizeof(header))!=0 ||
               !ma_wire_unpack(header,&seq,&bytes))break;
            uint8_t payload[MA_WIRE_MAX_PAYLOAD];
            if(ma_read_full(sock,payload,bytes)!=0)break;
            int16_t pcm[MA_OPUS_FRAME];
            if(have_sequence && seq!=last_sequence+1u) {
                /* A discontinuity should never play old queued sound. */
                ma_pcm_clear();
                (void)opus_decoder_ctl(decoder,OPUS_RESET_STATE);
            }
            int frames=opus_decode(decoder,payload,(opus_int32)bytes,
                                   pcm,MA_OPUS_FRAME,0);
            if(frames!=(int)MA_OPUS_FRAME)break;
            ma_pcm_push(pcm,(size_t)frames);
            have_sequence=true;last_sequence=seq;
        }
        MA_CLOSE(sock);
        opus_decoder_destroy(decoder);
        ma_pcm_clear();
        thrd_sleep_ms(100);
    }
    return 0;
}
int gui_monitor_audio_client_start(gui_app_t *app,const char *host,uint16_t port)
{
    gui_monitor_audio_client_stop();
    if(!app || !host || !*host || !port)return -1;
    g_ma_client.app=app;
    snprintf(g_ma_client.host,sizeof(g_ma_client.host),"%s",host);
    g_ma_client.port=port;
    atomic_store(&g_ma_client.stop,false);
    atomic_store(&g_ma_client.requested,false);
    atomic_store(&g_ma_client.pair,0);
    ma_pcm_clear();
    if(thrd_create(&g_ma_client.thread,ma_client_thread,NULL)!=thrd_success)
        return -1;
    atomic_store(&g_ma_client.running,true);
    return 0;
}
void gui_monitor_audio_client_stop(void)
{
    if(!atomic_load(&g_ma_client.running))return;
    atomic_store(&g_ma_client.stop,true);
    atomic_store(&g_ma_client.requested,false);
    thrd_join(g_ma_client.thread,NULL);
    atomic_store(&g_ma_client.running,false);
    g_ma_client.app=NULL;
    ma_pcm_clear();
}
/* GUI thread only. The audio callback consumes PCM independently of GUI FPS. */
void gui_monitor_audio_client_update(gui_app_t *app)
{
    bool enable=app && app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT &&
                atomic_load(&g_ma_client.running) &&
                app->settings.audio_monitor_playback &&
                gui_monitor_client_connected() && gui_monitor_client_peer_state()>0;
    unsigned pair=app && app->settings.audio_monitor_ch34 ? 1u : 0u;
    if(pair!=atomic_load(&g_ma_client.pair)){
        ma_pcm_clear();
        atomic_store(&g_ma_client.pair,pair);
    }
    atomic_store(&g_ma_client.requested,enable);
    if(!enable){
        if(g_ma_client.output_active){
            StopAudioStream(g_ma_client.output);
            UnloadAudioStream(g_ma_client.output);
            g_ma_client.output_active=false;
        }
        return;
    }
    if(!g_ma_client.output_active){
        if(!IsAudioDeviceReady())InitAudioDevice();
        if(!IsAudioDeviceReady())return;
        SetAudioStreamBufferSizeDefault(480);
        g_ma_client.output=LoadAudioStream(MA_OPUS_RATE,16,1);
        if(!IsAudioStreamValid(g_ma_client.output))return;
        SetAudioStreamCallback(g_ma_client.output,ma_playback_callback);
        PlayAudioStream(g_ma_client.output);
        g_ma_client.output_active=true;
    }
}
