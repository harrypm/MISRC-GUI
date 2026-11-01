#include "gui_monitor_video_codec.h"
#include "gui_monitor_video_i420.h"
#include "gui_monitor_video_wire.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifndef MISRC_HAVE_OPENH264
#define MISRC_HAVE_OPENH264 0
#endif
#if MISRC_HAVE_OPENH264
#include <wels/codec_api.h>
struct gui_mvd_encoder { ISVCEncoder *encoder; unsigned w, h; };
struct gui_mvd_decoder { ISVCDecoder *decoder; };
static uint8_t limit8(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}
int gui_mvd_codec_available(void) { return 1; }
gui_mvd_encoder_t *gui_mvd_encoder_create(unsigned w,unsigned h,
                                         unsigned fps_x100,unsigned bitrate_bps) {
    if (!mvd_i420_buffer_size(w,h) || !fps_x100 || fps_x100 > 6000 ||
        bitrate_bps < 64000 || bitrate_bps > 20000000) return NULL;
    gui_mvd_encoder_t *ctx=calloc(1,sizeof(*ctx));
    if (!ctx) return NULL;
    if (WelsCreateSVCEncoder(&ctx->encoder)!=0 || !ctx->encoder) {
        free(ctx); return NULL;
    }
    SEncParamBase p;
    memset(&p,0,sizeof(p));
    p.iUsageType=CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth=(int)w;
    p.iPicHeight=(int)h;
    p.iTargetBitrate=(int)bitrate_bps;
    /* Timestamp rate control accounts for variable PAL/VHS decoder cadence.
     * Keep frame skipping enabled to preserve the configured bitrate budget. */
    p.iRCMode=RC_TIMESTAMP_MODE;
    p.fMaxFrameRate=(float)fps_x100 / 100.0f;
    if ((*ctx->encoder)->Initialize(ctx->encoder,&p)!=0) {
        WelsDestroySVCEncoder(ctx->encoder);free(ctx);return NULL;
    }
    ctx->w=w;ctx->h=h;
    return ctx;
}
void gui_mvd_encoder_destroy(gui_mvd_encoder_t *ctx) {
    if (!ctx) return;
    if (ctx->encoder) {
        (*ctx->encoder)->Uninitialize(ctx->encoder);
        WelsDestroySVCEncoder(ctx->encoder);
    }
    free(ctx);
}
void gui_mvd_encoder_request_keyframe(gui_mvd_encoder_t *ctx) {
    if (ctx && ctx->encoder)
        (void)(*ctx->encoder)->ForceIntraFrame(ctx->encoder,true);
}
int gui_mvd_encoder_frame(gui_mvd_encoder_t *ctx,const uint8_t *input,
                          size_t input_bytes,uint64_t pts_us,
                          uint8_t *out,size_t cap,size_t *out_len,int *key) {
    if (out_len) *out_len=0;
    if (key) *key=0;
    if (!ctx || !input || !out || !out_len || !key ||
        input_bytes < mvd_i420_buffer_size(ctx->w,ctx->h)) return -1;
    SSourcePicture pic;
    memset(&pic,0,sizeof(pic));
    pic.iColorFormat=videoFormatI420;
    pic.iPicWidth=(int)ctx->w;pic.iPicHeight=(int)ctx->h;
    pic.iStride[0]=(int)ctx->w;pic.iStride[1]=(int)ctx->w/2;pic.iStride[2]=(int)ctx->w/2;
    pic.pData[0]=(unsigned char *)input;
    pic.pData[1]=(unsigned char *)(input+(size_t)ctx->w*ctx->h);
    pic.pData[2]=(unsigned char *)(input+(size_t)ctx->w*ctx->h*5u/4u);
    pic.uiTimeStamp=(long long)(pts_us/1000u);
    SFrameBSInfo bs;
    memset(&bs,0,sizeof(bs));
    if ((*ctx->encoder)->EncodeFrame(ctx->encoder,&pic,&bs)!=0 ||
        bs.eFrameType==videoFrameTypeInvalid) return -1;
    if (bs.eFrameType==videoFrameTypeSkip) return 0;
    if (bs.iLayerNum < 1 || bs.iLayerNum > MAX_LAYER_NUM_OF_FRAME) return -1;
    size_t size=0;
    for(int l=0;l<bs.iLayerNum;l++) {
        const SLayerBSInfo *layer=&bs.sLayerInfo[l];
        if (!layer->pBsBuf || !layer->pNalLengthInByte ||
            layer->iNalCount<1 || layer->iNalCount>MAX_NAL_UNITS_IN_LAYER) return -1;
        size_t layer_size=0;
        for(int n=0;n<layer->iNalCount;n++) {
            if (layer->pNalLengthInByte[n]<=0 ||
                layer_size>SIZE_MAX-(size_t)layer->pNalLengthInByte[n]) return -1;
            layer_size+=(size_t)layer->pNalLengthInByte[n];
        }
        if (size>cap || layer_size>cap-size) return -1;
        memcpy(out+size,layer->pBsBuf,layer_size);
        size+=layer_size;
    }
    *out_len=size;
    *key=(bs.eFrameType==videoFrameTypeIDR);
    return size>0?1:-1;
}
gui_mvd_decoder_t *gui_mvd_decoder_create(void) {
    gui_mvd_decoder_t *ctx=calloc(1,sizeof(*ctx));
    if (!ctx) return NULL;
    if (WelsCreateDecoder(&ctx->decoder)!=0 || !ctx->decoder) {free(ctx);return NULL;}
    SDecodingParam p;
    memset(&p,0,sizeof(p));
    p.sVideoProperty.size=sizeof(p.sVideoProperty);
    p.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
    if ((*ctx->decoder)->Initialize(ctx->decoder,&p)!=0) {
        WelsDestroyDecoder(ctx->decoder);free(ctx);return NULL;
    }
    return ctx;
}
void gui_mvd_decoder_destroy(gui_mvd_decoder_t *ctx) {
    if (!ctx) return;
    if (ctx->decoder) {
        (*ctx->decoder)->Uninitialize(ctx->decoder);
        WelsDestroyDecoder(ctx->decoder);
    }
    free(ctx);
}
int gui_mvd_decoder_frame(gui_mvd_decoder_t *ctx,const uint8_t *packet,
                          size_t len,uint8_t *rgba,size_t cap,
                          unsigned *width,unsigned *height) {
    if (!ctx || !ctx->decoder || !packet || !rgba || !width || !height ||
        len < 4 || len > MVD_MAX_PAYLOAD || len > INT_MAX) return -1;
    unsigned char *planes[3]={0};
    SBufferInfo info;
    memset(&info,0,sizeof(info));
    DECODING_STATE rc=(*ctx->decoder)->DecodeFrameNoDelay(ctx->decoder,
                                                     packet,(int)len,planes,&info);
    if (rc != dsErrorFree) return -1;
    if (!info.iBufferStatus) return 0;
    int w=info.UsrData.sSystemBuffer.iWidth;
    int h=info.UsrData.sSystemBuffer.iHeight;
    int ys=info.UsrData.sSystemBuffer.iStride[0];
    int us=info.UsrData.sSystemBuffer.iStride[1];
    if (w<2 || h<2 || w>(int)MVD_MAX_WIDTH || h>(int)MVD_MAX_HEIGHT ||
        ys<w || us<w/2 || !planes[0] || !planes[1] || !planes[2] ||
        (size_t)w*(size_t)h>cap/4u) return -1;
    for(int y=0;y<h;y++){
        for(int x=0;x<w;x++){
            int Y=(int)planes[0][(size_t)y*ys+x]-16;
            int U=(int)planes[1][(size_t)(y/2)*us+x/2]-128;
            int V=(int)planes[2][(size_t)(y/2)*us+x/2]-128;
            if(Y<0)Y=0;
            int c=298*Y;
            uint8_t *d=rgba+((size_t)y*w+x)*4u;
            d[0]=limit8((c+409*V+128)>>8);
            d[1]=limit8((c-100*U-208*V+128)>>8);
            d[2]=limit8((c+516*U+128)>>8);
            d[3]=255;
        }
    }
    *width=(unsigned)w;*height=(unsigned)h;
    return 1;
}
#else
struct gui_mvd_encoder { int unused; };
struct gui_mvd_decoder { int unused; };
int gui_mvd_codec_available(void) { return 0; }
gui_mvd_encoder_t *gui_mvd_encoder_create(unsigned w,unsigned h,unsigned fps,unsigned bitrate) {
    (void)w;(void)h;(void)fps;(void)bitrate;return NULL;
}
void gui_mvd_encoder_destroy(gui_mvd_encoder_t *ctx) {(void)ctx;}
void gui_mvd_encoder_request_keyframe(gui_mvd_encoder_t *ctx) {(void)ctx;}
int gui_mvd_encoder_frame(gui_mvd_encoder_t *ctx,const uint8_t *y,size_t n,uint64_t t,
                          uint8_t *out,size_t cap,size_t *len,int *key) {
    (void)ctx;(void)y;(void)n;(void)t;(void)out;(void)cap;
    if (len) *len = 0;
    if (key) *key = 0;
    return -1;
}
gui_mvd_decoder_t *gui_mvd_decoder_create(void) {return NULL;}
void gui_mvd_decoder_destroy(gui_mvd_decoder_t *ctx) {(void)ctx;}
int gui_mvd_decoder_frame(gui_mvd_decoder_t *ctx,const uint8_t *packet,
                          size_t n,uint8_t *rgba,size_t cap,unsigned *w,unsigned *h) {
    (void)ctx;(void)packet;(void)n;(void)rgba;(void)cap;(void)w;(void)h;return -1;
}
#endif
