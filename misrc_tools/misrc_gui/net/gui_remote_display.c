#include "gui_remote_display.h"
#ifndef GUI_RD_PROTOCOL_TEST
#include "../visualization/gui_oscilloscope.h"
#include "../visualization/gui_fft.h"
#else
/* Linked to real functions in production; lightweight deterministic stubs
 * allow on-wire/Zstd tests without the whole Raylib application. */
size_t gui_waveform_server_display_points(const int16_t *,const int16_t *,const int16_t *,
    size_t,uint32_t,const gui_rd_config_t *,float *,int32_t *);
void gui_fft_server_bins(const int16_t *,size_t,uint32_t,float *,size_t);
#endif
#include "../../common/threading.h"
#include <zstd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

_Static_assert(sizeof(float)==4, "Float32 required");
_Static_assert(sizeof(gui_rd_frame_t)==16+2*(16+4*GUI_RD_WIDTH+4*GUI_RD_BINS), "Unexpected wire layout");
/* Single mailbox of original ADC samples; never exposed over HTTP. */
static int16_t raw_a[GUI_RD_MAX_SOURCE], raw_b[GUI_RD_MAX_SOURCE];
static size_t raw_count;
static uint32_t raw_rate,raw_sequence;
static uint64_t raw_time_ms;
static atomic_flag raw_lock=ATOMIC_FLAG_INIT;
static gui_rd_frame_t client_frame;
static atomic_flag client_lock=ATOMIC_FLAG_INIT;
static atomic_bool client_valid=ATOMIC_VAR_INIT(false);
/* Atomic UI config, written only by main thread, read from network worker. */
static atomic_int req[2][6];

void gui_rd_publish_request(int c,gui_rd_config_t v) {
    if(c<0||c>1)return;
    if(!isfinite(v.zoom)||v.zoom<1||v.zoom>128)v.zoom=32;
    if(v.width<1||v.width>GUI_RD_WIDTH)v.width=GUI_RD_WIDTH;
    atomic_store(&req[c][0],(int)(v.zoom*1000.f+0.5f));
    atomic_store(&req[c][1],(int)v.width);
    atomic_store(&req[c][2],v.trigger_enabled?1:0);
    atomic_store(&req[c][3],v.trigger_level);
    atomic_store(&req[c][4],v.trigger_mode);
    atomic_store(&req[c][5],v.trigger_source);
}
gui_rd_config_t gui_rd_get_request(int c) {
    gui_rd_config_t v={32.f,GUI_RD_WIDTH,0,0,0,c==1?1:0};
    if(c<0||c>1)return v;
    int z=atomic_load(&req[c][0]),w=atomic_load(&req[c][1]);
    if(z>=1000&&z<=128000)v.zoom=(float)z/1000.f;
    if(w>=1&&w<=GUI_RD_WIDTH)v.width=(uint32_t)w;
    v.trigger_enabled=atomic_load(&req[c][2])!=0;
    v.trigger_level=atomic_load(&req[c][3]);
    v.trigger_mode=atomic_load(&req[c][4]);
    v.trigger_source=atomic_load(&req[c][5]);
    return v;
}
void gui_rd_capture(const int16_t *a,const int16_t *b,size_t count,uint32_t rate,uint32_t seq) {
    if(!a||count<4096||count>GUI_RD_MAX_SOURCE||!rate)return;
    if(atomic_flag_test_and_set_explicit(&raw_lock,memory_order_acquire))return;
    memcpy(raw_a,a,count*2);
    if(b)memcpy(raw_b,b,count*2);
    raw_count=count;raw_rate=rate;raw_sequence=seq;
    raw_time_ms=get_time_ms();
    /* NULL second channel distinguished by top bit, not sent to renderer. */
    if(!b)raw_sequence|=0x80000000u;
    atomic_flag_clear_explicit(&raw_lock,memory_order_release);
}
size_t gui_rd_wire_capacity(void){return ZSTD_compressBound(sizeof(gui_rd_frame_t));}
bool gui_rd_server_build(const gui_rd_config_t cfg[2],void **wire,size_t *bytes,size_t *uncompressed){
    if(!cfg||!wire||!bytes||!uncompressed)return false;
    *wire=NULL;*bytes=0;*uncompressed=0;
    int16_t *samples=(int16_t*)malloc(GUI_RD_MAX_SOURCE*4);
    if(!samples)return false;
    size_t n;uint32_t rate,seq;uint64_t stamp;
    while(atomic_flag_test_and_set_explicit(&raw_lock,memory_order_acquire))thrd_sleep_ms(1);
    n=raw_count;rate=raw_rate;seq=raw_sequence;stamp=raw_time_ms;
    if(n && n<=GUI_RD_MAX_SOURCE){memcpy(samples,raw_a,n*2);memcpy(samples+GUI_RD_MAX_SOURCE,raw_b,n*2);}
    atomic_flag_clear_explicit(&raw_lock,memory_order_release);
    uint64_t now=get_time_ms();
    if(n<4096||n>GUI_RD_MAX_SOURCE||!rate||now<stamp||now-stamp>500){free(samples);return false;}
    gui_rd_frame_t *f=(gui_rd_frame_t*)calloc(1,sizeof(*f));
    if(!f){free(samples);return false;}
    memcpy(f->magic,"MRD5",4);f->version=GUI_RD_VERSION;
    f->channels=(seq&0x80000000u)?1:2;
    f->sample_rate=rate;f->sequence=seq&0x7fffffffu;
    for(int c=0;c<f->channels;c++){
        gui_rd_config_t settings=cfg[c];
        if(!isfinite(settings.zoom)||settings.zoom<1||settings.zoom>128)settings.zoom=32;
        if(settings.width<1||settings.width>GUI_RD_WIDTH)settings.width=GUI_RD_WIDTH;
        int16_t *src=samples+(size_t)c*GUI_RD_MAX_SOURCE;
        uint32_t pos=0,neg=0,abspeak=0;
        double sq=0.0;
        for(size_t i=0;i<n;i++){
            int32_t value=src[i], magnitude=value<0?-value:value;
            if((uint32_t)magnitude>abspeak)abspeak=(uint32_t)magnitude;
            sq+=(double)value*(double)value;
            if(i<1000){
                if(value>0&&(uint32_t)value>pos)pos=value;
                if(value<0&&(uint32_t)magnitude>neg)neg=magnitude;
            }
        }
        f->ch[c].peak_pos=(uint16_t)(pos>32767u?32767u:pos);
        f->ch[c].peak_neg=(uint16_t)(neg>32767u?32767u:neg);
        f->ch[c].peak=(uint16_t)(abspeak>32767u?32767u:abspeak);
        double rms=sqrt(sq/(double)n);
        f->ch[c].rms=(uint16_t)(rms>32767.0?32767.0:rms);
        f->ch[c].count=(uint32_t)gui_waveform_server_display_points(src,samples,
               samples+GUI_RD_MAX_SOURCE,n,rate,&settings,f->ch[c].waveform,
               &f->ch[c].trigger_pos);
        /* Original Local FFTW calculation, pre-EMA, same frame window. */
        gui_fft_server_bins(src, n>=GUI_RD_WIDTH?GUI_RD_WIDTH:n,rate,f->ch[c].fft,GUI_RD_BINS);
    }
    free(samples);
    size_t capacity=gui_rd_wire_capacity();
    void *buf=malloc(capacity);
    if(buf){
        size_t result=ZSTD_compress(buf,capacity,f,sizeof(*f),1);
        if(!ZSTD_isError(result)){
            *wire=buf;*bytes=result;*uncompressed=sizeof(*f);
        }else free(buf);
    }
    free(f);
    return *wire!=NULL;
}
bool gui_rd_client_accept(const void *wire,size_t bytes){
    if(!wire||bytes<4||bytes>gui_rd_wire_capacity())return false;
    if(ZSTD_getFrameContentSize(wire,bytes)!=sizeof(gui_rd_frame_t))return false;
    gui_rd_frame_t *f=malloc(sizeof(*f));if(!f)return false;
    size_t result=ZSTD_decompress(f,sizeof(*f),wire,bytes);
    bool ok=!ZSTD_isError(result)&&result==sizeof(*f)&&
            !memcmp(f->magic,"MRD5",4)&&f->version==GUI_RD_VERSION&&
            f->channels>=1&&f->channels<=2&&f->sample_rate>0;
    if(ok)for(int c=0;c<f->channels;c++){
        if(f->ch[c].count>GUI_RD_WIDTH||f->ch[c].trigger_pos>=GUI_RD_WIDTH ||
           f->ch[c].trigger_pos < -1 ||
           (f->ch[c].count == 0 && f->ch[c].trigger_pos != -1)){ok=false;break;}
        for(size_t i=0;i<f->ch[c].count;i++)
            if(!isfinite(f->ch[c].waveform[i])){ok=false;break;}
        for(size_t i=0;i<GUI_RD_BINS;i++)
            if(!isfinite(f->ch[c].fft[i])){ok=false;break;}
        if(!ok)break;
    }
    if(ok){
        while(atomic_flag_test_and_set_explicit(&client_lock,memory_order_acquire))thrd_sleep_ms(1);
        memcpy(&client_frame,f,sizeof(*f));
        atomic_store_explicit(&client_valid,true,memory_order_release);
        atomic_flag_clear_explicit(&client_lock,memory_order_release);
    }
    free(f);return ok;
}
bool gui_rd_client_snapshot(gui_rd_frame_t *out){
    if(!out||!atomic_load_explicit(&client_valid,memory_order_acquire))return false;
    while(atomic_flag_test_and_set_explicit(&client_lock,memory_order_acquire))thrd_sleep_ms(1);
    memcpy(out,&client_frame,sizeof(*out));
    atomic_flag_clear_explicit(&client_lock,memory_order_release);return true;
}
void gui_rd_client_clear(void){atomic_store(&client_valid,false);}
