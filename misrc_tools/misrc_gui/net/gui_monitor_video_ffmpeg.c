/* Optional libavcodec backend: modern codecs use the same I420 frames as
 * OpenH264. All codec/device contexts belong to one video worker; neither
 * capture nor GUI render threads execute libavcodec calls. */
#include "gui_monitor_video_ffmpeg.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdatomic.h>
#ifndef MISRC_HAVE_FFMPEG
#define MISRC_HAVE_FFMPEG 0
#endif
#if MISRC_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <errno.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
struct mvd_ff_encoder {
    AVCodecContext *ctx;
    AVFrame *frame,*gpu_frame;
    AVBufferRef *gpu_device,*gpu_frames;
    AVPacket *packet;
    struct SwsContext *sws;
    unsigned w,h;
    int first;
};
struct mvd_ff_decoder {
    AVCodecContext *ctx;
    AVFrame *frame,*software;
    AVPacket *packet;
    struct SwsContext *sws;
    AVBufferRef *device;
    enum AVPixelFormat hw_format;
    unsigned wire_codec;
};
/* A failed GPU decoder must not be retried forever on every reconnect. */
static atomic_uint s_decoder_hw_failures;
static enum AVCodecID codec_id(unsigned wire) {
    switch(wire) {
        case MVD_CODEC_HEVC_ANNEXB: return AV_CODEC_ID_HEVC;
        case MVD_CODEC_VP9: return AV_CODEC_ID_VP9;
        case MVD_CODEC_AV1: return AV_CODEC_ID_AV1;
        default: return AV_CODEC_ID_NONE;
    }
}
/* CUDA/NVENC, VideoToolbox, AMF accept host-side YUV input. VAAPI and QSV
 * use NV12 staging frames uploaded to a real hardware frame pool. A codec
 * is selected only AFTER its device and encoder context have opened. */
static const char * const hw_av1[]={"av1_nvenc","av1_amf","av1_videotoolbox","av1_vaapi","av1_qsv",NULL};
static const char * const hw_vp9[]={"vp9_vaapi","vp9_qsv","vp9_mf",NULL};
static const char * const hw_hevc[]={"hevc_nvenc","hevc_amf","hevc_videotoolbox","hevc_vaapi","hevc_qsv",NULL};
static const char * const sw_av1[]={"libsvtav1","libaom-av1","librav1e",NULL};
static const char * const sw_vp9[]={"libvpx-vp9",NULL};
static const char * const sw_hevc[]={"libx265",NULL};
static const char * const *names(unsigned wire,int hardware) {
    switch(wire) {
        case MVD_CODEC_AV1: return hardware?hw_av1:sw_av1;
        case MVD_CODEC_VP9: return hardware?hw_vp9:sw_vp9;
        case MVD_CODEC_HEVC_ANNEXB: return hardware?hw_hevc:sw_hevc;
        default: return NULL;
    }
}
static int usable_pixel_format(const AVCodec *codec,enum AVPixelFormat format) {
    const enum AVPixelFormat *formats=NULL;
#if LIBAVCODEC_VERSION_MAJOR >= 62
    /* FFmpeg 9 removed AVCodec.pix_fmts. Query the supported configuration
     * through the stable libavcodec 7.1+ API instead. */
    if(avcodec_get_supported_config(NULL,codec,AV_CODEC_CONFIG_PIX_FORMAT,0,
                                    (const void **)&formats,NULL)<0)return 0;
#else
    formats=codec->pix_fmts;
#endif
    if(!formats)return format==AV_PIX_FMT_YUV420P;
    for(const enum AVPixelFormat *p=formats;*p!=AV_PIX_FMT_NONE;p++)
        if(*p==format)return 1;
    return 0;
}
static enum AVPixelFormat input_format(const AVCodec *codec) {
    if(usable_pixel_format(codec,AV_PIX_FMT_YUV420P))return AV_PIX_FMT_YUV420P;
    if(usable_pixel_format(codec,AV_PIX_FMT_NV12))return AV_PIX_FMT_NV12;
    if(usable_pixel_format(codec,AV_PIX_FMT_VAAPI))return AV_PIX_FMT_VAAPI;
    if(usable_pixel_format(codec,AV_PIX_FMT_QSV))return AV_PIX_FMT_QSV;
    return AV_PIX_FMT_NONE;
}
uint32_t mvd_ff_encode_mask(int hardware) {
    uint32_t out=0;
    for(unsigned wire=MVD_CODEC_HEVC_ANNEXB;wire<=MVD_CODEC_AV1;wire++){
        const char * const *p=names(wire,hardware);
        for(;p && *p;p++){
            const AVCodec *codec=avcodec_find_encoder_by_name(*p);
            if(codec && input_format(codec)!=AV_PIX_FMT_NONE) {
                out |= MVD_CODEC_BIT(wire);break;
            }
        }
    }
    return out;
}
uint32_t mvd_ff_decode_mask(void) {
    uint32_t out=0;
    for(unsigned wire=MVD_CODEC_HEVC_ANNEXB;wire<=MVD_CODEC_AV1;wire++)
        if(avcodec_find_decoder(codec_id(wire)))out|=MVD_CODEC_BIT(wire);
    return out;
}
static mvd_ff_encoder *create_one(const AVCodec *codec,unsigned w,unsigned h,
                                  unsigned fps_x100,unsigned bitrate) {
    if(!codec || input_format(codec)==AV_PIX_FMT_NONE)return NULL;
    mvd_ff_encoder *s=calloc(1,sizeof(*s));
    if(!s)return NULL;
    s->ctx=avcodec_alloc_context3(codec);
    s->frame=av_frame_alloc();
    s->packet=av_packet_alloc();
    if(!s->ctx || !s->frame || !s->packet)goto fail;
    s->ctx->width=(int)w;s->ctx->height=(int)h;
    s->ctx->time_base=(AVRational){1,1000000};
    s->ctx->framerate=(AVRational){(int)fps_x100,100};
    enum AVPixelFormat encoder_format=input_format(codec);
    s->ctx->pix_fmt=encoder_format;
    s->ctx->bit_rate=(int64_t)bitrate;
    if(encoder_format==AV_PIX_FMT_VAAPI || encoder_format==AV_PIX_FMT_QSV) {
        enum AVHWDeviceType kind=encoder_format==AV_PIX_FMT_VAAPI
                    ? AV_HWDEVICE_TYPE_VAAPI : AV_HWDEVICE_TYPE_QSV;
        if(av_hwdevice_ctx_create(&s->gpu_device,kind,NULL,NULL,0)<0)goto fail;
        s->gpu_frames=av_hwframe_ctx_alloc(s->gpu_device);
        if(!s->gpu_frames)goto fail;
        AVHWFramesContext *frames=(AVHWFramesContext *)s->gpu_frames->data;
        frames->format=encoder_format;
        frames->sw_format=AV_PIX_FMT_NV12;
        frames->width=(int)w;frames->height=(int)h;
        frames->initial_pool_size=8;
        if(av_hwframe_ctx_init(s->gpu_frames)<0)goto fail;
        s->ctx->hw_frames_ctx=av_buffer_ref(s->gpu_frames);
        if(!s->ctx->hw_frames_ctx)goto fail;
        s->gpu_frame=av_frame_alloc();
        if(!s->gpu_frame)goto fail;
    }
    s->ctx->gop_size=(int)(fps_x100/100u)*2;
    if(s->ctx->gop_size<1)s->ctx->gop_size=25;
    s->ctx->max_b_frames=0; /* Low latency, one access unit per packet. */
    s->ctx->thread_count=2; /* Do not overload the capture/DSP server. */
    if(codec->id==AV_CODEC_ID_VP9){
        av_opt_set(s->ctx->priv_data,"deadline","realtime",0);
        av_opt_set(s->ctx->priv_data,"cpu-used","8",0);
        av_opt_set(s->ctx->priv_data,"row-mt","1",0);
        av_opt_set(s->ctx->priv_data,"lag-in-frames","0",0);
        av_opt_set(s->ctx->priv_data,"auto-alt-ref","0",0);
    } else if(codec->id==AV_CODEC_ID_AV1){
        av_opt_set(s->ctx->priv_data,"preset","12",0);
        av_opt_set(s->ctx->priv_data,"cpu-used","8",0);
        av_opt_set(s->ctx->priv_data,"usage","realtime",0);
        av_opt_set(s->ctx->priv_data,"lag-in-frames","0",0);
    } else if(codec->id==AV_CODEC_ID_HEVC){
        av_opt_set(s->ctx->priv_data,"preset","ultrafast",0);
        av_opt_set(s->ctx->priv_data,"tune","zerolatency",0);
        av_opt_set(s->ctx->priv_data,"x265-params",
                   "repeat-headers=1:aud=1:rc-lookahead=0:bframes=0:frame-threads=1:pools=none",0);
    }
    if(avcodec_open2(s->ctx,codec,NULL)<0)goto fail;
    s->frame->width=(int)w;s->frame->height=(int)h;
    s->frame->format=s->gpu_frames?AV_PIX_FMT_NV12:s->ctx->pix_fmt;
    if(av_frame_get_buffer(s->frame,32)<0)goto fail;
    s->w=w;s->h=h;s->first=1;
    return s;
fail:
    mvd_ff_encoder_free(s);
    return NULL;
}
mvd_ff_encoder *mvd_ff_encoder_create(unsigned wire,unsigned w,unsigned h,
                                    unsigned fps_x100,unsigned bitrate,int hardware) {
    if(codec_id(wire)==AV_CODEC_ID_NONE || w<2 || h<2 ||
       w>MVD_MAX_WIDTH || h>MVD_MAX_HEIGHT || (w&1u) || (h&1u) ||
       fps_x100<100 || fps_x100>6000 ||
       bitrate<64000 || bitrate>20000000)return NULL;
    const char * const *p=names(wire,hardware);
    for(;p && *p;p++){
        const AVCodec *codec=avcodec_find_encoder_by_name(*p);
        if(codec) {
            mvd_ff_encoder *s=create_one(codec,w,h,fps_x100,bitrate);
            if(s)return s;
        }
    }
    return NULL;
}
void mvd_ff_encoder_free(mvd_ff_encoder *s) {
    if(!s)return;
    sws_freeContext(s->sws);
    av_frame_free(&s->gpu_frame);
    av_buffer_unref(&s->gpu_frames);
    av_buffer_unref(&s->gpu_device);
    av_packet_free(&s->packet);
    av_frame_free(&s->frame);
    avcodec_free_context(&s->ctx);
    free(s);
}
int mvd_ff_encoder_frame(mvd_ff_encoder *s,const uint8_t *i420,size_t bytes,
                         uint64_t pts_us,uint8_t *out,size_t cap,size_t *len,int *key) {
    if(len)*len=0;
    if(key)*key=0;
    if(!s || !i420 || !out || !len || !key ||
       bytes<(size_t)s->w*s->h*3u/2u)return -1;
    if(av_frame_make_writable(s->frame)<0)return -1;
    const uint8_t *src[4]={i420,i420+(size_t)s->w*s->h,
                           i420+(size_t)s->w*s->h*5u/4u,NULL};
    int stride[4]={(int)s->w,(int)s->w/2,(int)s->w/2,0};
    s->sws=sws_getCachedContext(s->sws,(int)s->w,(int)s->h,AV_PIX_FMT_YUV420P,
                  (int)s->w,(int)s->h,(enum AVPixelFormat)s->frame->format,SWS_FAST_BILINEAR,
                  NULL,NULL,NULL);
    if(!s->sws || sws_scale(s->sws,src,stride,0,(int)s->h,
                           s->frame->data,s->frame->linesize)!=(int)s->h)return -1;
    s->frame->pts=(int64_t)pts_us;
    s->frame->pict_type=s->first?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
    s->first=0;
    AVFrame *to_send=s->frame;
    if(s->gpu_frames){
        av_frame_unref(s->gpu_frame);
        if(av_hwframe_get_buffer(s->gpu_frames,s->gpu_frame,0)<0 ||
           av_hwframe_transfer_data(s->gpu_frame,s->frame,0)<0)return -1;
        s->gpu_frame->pts=s->frame->pts;
        s->gpu_frame->pict_type=s->frame->pict_type;
        to_send=s->gpu_frame;
    }
    if(avcodec_send_frame(s->ctx,to_send)<0)return -1;
    int rc=avcodec_receive_packet(s->ctx,s->packet);
    if(rc==AVERROR(EAGAIN) || rc==AVERROR_EOF)return 0;
    if(rc<0)return -1;
    if(s->packet->size<=0 || (size_t)s->packet->size>cap){
        av_packet_unref(s->packet);return -1;
    }
    memcpy(out,s->packet->data,(size_t)s->packet->size);
    *len=(size_t)s->packet->size;
    *key=(s->packet->flags & AV_PKT_FLAG_KEY)!=0;
    av_packet_unref(s->packet);
    return 1;
}
static enum AVPixelFormat choose_decoder_format(AVCodecContext *ctx,
                                                const enum AVPixelFormat *candidates) {
    mvd_ff_decoder *d=(mvd_ff_decoder *)ctx->opaque;
    if(d && d->device)for(const enum AVPixelFormat *p=candidates;
                           *p!=AV_PIX_FMT_NONE;p++)
        if(*p==d->hw_format)return *p;
    for(const enum AVPixelFormat *p=candidates;*p!=AV_PIX_FMT_NONE;p++)
        if(*p==AV_PIX_FMT_YUV420P || *p==AV_PIX_FMT_NV12 ||
           *p==AV_PIX_FMT_YUV420P10LE || *p==AV_PIX_FMT_P010LE)return *p;
    return candidates[0];
}
mvd_ff_decoder *mvd_ff_decoder_create(unsigned wire) {
    enum AVCodecID id=codec_id(wire);
    if(id==AV_CODEC_ID_NONE)return NULL;
    const AVCodec *codec=avcodec_find_decoder(id);
    if(!codec)return NULL;
    mvd_ff_decoder *d=calloc(1,sizeof(*d));
    if(!d)return NULL;
    d->wire_codec=wire;
    d->ctx=avcodec_alloc_context3(codec);
    d->frame=av_frame_alloc();
    d->software=av_frame_alloc();
    d->packet=av_packet_alloc();
    if(!d->ctx || !d->frame || !d->software || !d->packet)goto fail;
    d->hw_format=AV_PIX_FMT_NONE;
    /* Use real hardware decoding when a device can actually be opened;
     * otherwise FFmpeg's software decoder is always retained. */
    for(int i=0;!(atomic_load(&s_decoder_hw_failures)&MVD_CODEC_BIT(wire));i++){
        const AVCodecHWConfig *hw=avcodec_get_hw_config(codec,i);
        if(!hw)break;
        if(!(hw->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))continue;
        AVBufferRef *device=NULL;
        if(av_hwdevice_ctx_create(&device,hw->device_type,NULL,NULL,0)==0){
            d->device=device;
            d->hw_format=hw->pix_fmt;
            d->ctx->hw_device_ctx=av_buffer_ref(device);
            d->ctx->opaque=d;
            d->ctx->get_format=choose_decoder_format;
            break;
        }
    }
    d->ctx->thread_count=2;
    if(avcodec_open2(d->ctx,codec,NULL)<0) {
        if(!d->device)goto fail;
        /* Driver reports a supported codec but refuses to initialize.
         * Try software on this same connection and suppress further GPU
         * retries for this codec until the next process start. */
        atomic_fetch_or(&s_decoder_hw_failures,MVD_CODEC_BIT(wire));
        avcodec_free_context(&d->ctx);
        av_buffer_unref(&d->device);
        d->hw_format=AV_PIX_FMT_NONE;
        d->ctx=avcodec_alloc_context3(codec);
        if(!d->ctx)goto fail;
        d->ctx->thread_count=2;
        if(avcodec_open2(d->ctx,codec,NULL)<0)goto fail;
    }
    return d;
fail:
    mvd_ff_decoder_free(d);return NULL;
}
void mvd_ff_decoder_free(mvd_ff_decoder *d) {
    if(!d)return;
    sws_freeContext(d->sws);
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    av_frame_free(&d->software);
    avcodec_free_context(&d->ctx);
    av_buffer_unref(&d->device);
    free(d);
}
int mvd_ff_decoder_frame(mvd_ff_decoder *d,const uint8_t *packet,size_t len,
                         uint8_t *rgba,size_t capacity,unsigned *w,unsigned *h) {
    if(!d || !packet || len<1 || len>INT_MAX ||
       !rgba || !w || !h)return -1;
    av_packet_unref(d->packet);
    if(av_new_packet(d->packet,(int)len)<0)return -1;
    memcpy(d->packet->data,packet,len);
    int rc=avcodec_send_packet(d->ctx,d->packet);
    av_packet_unref(d->packet);
    if(rc<0){
        if(d->device)atomic_fetch_or(&s_decoder_hw_failures,MVD_CODEC_BIT(d->wire_codec));
        return -1;
    }
    rc=avcodec_receive_frame(d->ctx,d->frame);
    if(rc==AVERROR(EAGAIN) || rc==AVERROR_EOF)return 0;
    if(rc<0){
        if(d->device)atomic_fetch_or(&s_decoder_hw_failures,MVD_CODEC_BIT(d->wire_codec));
        return -1;
    }
    AVFrame *frame=d->frame;
    if(d->device && d->frame->format==d->hw_format){
        av_frame_unref(d->software);
        if(av_hwframe_transfer_data(d->software,d->frame,0)<0){
            atomic_fetch_or(&s_decoder_hw_failures,MVD_CODEC_BIT(d->wire_codec));
            av_frame_unref(d->frame);return -1;
        }
        frame=d->software;
    }
    const int width=frame->width,height=frame->height;
    if(width<2 || height<2 || width>(int)MVD_MAX_WIDTH ||
       height>(int)MVD_MAX_HEIGHT ||
       (size_t)width*(size_t)height>capacity/4u){
        av_frame_unref(d->frame);av_frame_unref(d->software);return -1;
    }
    d->sws=sws_getCachedContext(d->sws,width,height,
         (enum AVPixelFormat)frame->format,width,height,AV_PIX_FMT_RGBA,
         SWS_FAST_BILINEAR,NULL,NULL,NULL);
    uint8_t *out[4]={rgba,NULL,NULL,NULL};
    int out_stride[4]={width*4,0,0,0};
    rc=d->sws?sws_scale(d->sws,(const uint8_t *const *)frame->data,
                       frame->linesize,0,height,out,out_stride):-1;
    av_frame_unref(d->frame);av_frame_unref(d->software);
    if(rc!=height)return -1;
    *w=(unsigned)width;*h=(unsigned)height;
    return 1;
}
#else
struct mvd_ff_encoder { int unused; };
struct mvd_ff_decoder { int unused; };
uint32_t mvd_ff_encode_mask(int hw) {(void)hw;return 0;}
uint32_t mvd_ff_decode_mask(void) {return 0;}
mvd_ff_encoder *mvd_ff_encoder_create(unsigned c,unsigned w,unsigned h,unsigned f,unsigned b,int hw){
    (void)c;(void)w;(void)h;(void)f;(void)b;(void)hw;return NULL;
}
void mvd_ff_encoder_free(mvd_ff_encoder *s) {(void)s;}
int mvd_ff_encoder_frame(mvd_ff_encoder *s,const uint8_t *i,size_t n,uint64_t t,
    uint8_t *o,size_t cap,size_t *len,int *key){
    (void)s;(void)i;(void)n;(void)t;(void)o;(void)cap;
    if(len)*len=0;if(key)*key=0;return -1;
}
mvd_ff_decoder *mvd_ff_decoder_create(unsigned c) {(void)c;return NULL;}
void mvd_ff_decoder_free(mvd_ff_decoder *d) {(void)d;}
int mvd_ff_decoder_frame(mvd_ff_decoder *d,const uint8_t *p,size_t n,
   uint8_t *o,size_t cap,unsigned *w,unsigned *h){
    (void)d;(void)p;(void)n;(void)o;(void)cap;(void)w;(void)h;return -1;
}
#endif
