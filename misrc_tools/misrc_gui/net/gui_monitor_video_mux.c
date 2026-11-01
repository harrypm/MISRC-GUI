#include "gui_monitor_video_mux.h"
#include "gui_monitor_video_codec.h"
#include "gui_monitor_video_ffmpeg.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
struct mvd_codec_encoder {
    unsigned codec;
    int hardware;
    gui_mvd_encoder_t *avc;
    mvd_ff_encoder *ff;
};
struct mvd_codec_decoder {
    gui_mvd_decoder_t *avc;
    mvd_ff_decoder *ff;
};
uint32_t mvd_video_decoder_mask(void) {
    return (gui_mvd_codec_available()?MVD_CODEC_AVC_BIT:0u)|
           mvd_ff_decode_mask();
}
uint32_t mvd_video_encoder_mask(int hw) {
    return mvd_ff_encode_mask(hw)|
           ((!hw && gui_mvd_codec_available())?MVD_CODEC_AVC_BIT:0u);
}
static mvd_codec_encoder *try_create(unsigned codec,int hardware,
                   unsigned w,unsigned h,unsigned fps,unsigned bitrate) {
    mvd_codec_encoder *e=calloc(1,sizeof(*e));
    if(!e)return NULL;
    e->codec=codec;e->hardware=hardware;
    if(codec==MVD_CODEC_H264_ANNEXB && !hardware)
        e->avc=gui_mvd_encoder_create(w,h,fps,bitrate);
    else
        e->ff=mvd_ff_encoder_create(codec,w,h,fps,bitrate,hardware);
    if(!e->avc && !e->ff) {free(e);return NULL;}
    if(e->avc)gui_mvd_encoder_request_keyframe(e->avc);
    return e;
}
mvd_codec_encoder *mvd_video_encoder_best(uint32_t client_mask,
                       unsigned w,unsigned h,unsigned fps,unsigned bitrate) {
    uint32_t hw=mvd_video_encoder_mask(1)&client_mask;
    uint32_t sw=mvd_video_encoder_mask(0)&client_mask;
    /* Each selected backend is actually OPENED on the server. If FFmpeg
     * advertises an NVENC/AMF/VideoToolbox codec but the GPU/driver cannot
     * initialize, discard that candidate and try the next one instead of
     * sending an undecodable stream or burning CPU in a retry loop. */
    for(unsigned attempt=0;attempt<8;attempt++){
        unsigned codec=mvd_codec_select(client_mask,hw,sw);
        if(!codec)break;
        int hardware=(hw & MVD_CODEC_BIT(codec))!=0;
        mvd_codec_encoder *e=try_create(codec,hardware,w,h,fps,bitrate);
        if(e){
            fprintf(stderr,"[M-VIDEO] selected %s (%s encoder)\n",
                    mvd_codec_name(codec),hardware?"hardware":"software");
            return e;
        }
        if(hardware)hw&=~MVD_CODEC_BIT(codec);
        else sw&=~MVD_CODEC_BIT(codec);
    }
    return NULL;
}
unsigned mvd_video_encoder_codec(const mvd_codec_encoder *e) {
    return e?e->codec:0;
}
int mvd_video_encoder_hardware(const mvd_codec_encoder *e) {
    return e?e->hardware:0;
}
int mvd_video_encoder_frame(mvd_codec_encoder *e,const uint8_t *in,size_t bytes,
                      uint64_t pts,uint8_t *out,size_t cap,size_t *len,int *key) {
    if(!e)return -1;
    if(e->avc)return gui_mvd_encoder_frame(e->avc,in,bytes,pts,out,cap,len,key);
    return mvd_ff_encoder_frame(e->ff,in,bytes,pts,out,cap,len,key);
}
void mvd_video_encoder_free(mvd_codec_encoder *e) {
    if(!e)return;
    gui_mvd_encoder_destroy(e->avc);
    mvd_ff_encoder_free(e->ff);
    free(e);
}
mvd_codec_decoder *mvd_video_decoder_new(unsigned codec) {
    if(!mvd_codec_valid(codec))return NULL;
    mvd_codec_decoder *d=calloc(1,sizeof(*d));
    if(!d)return NULL;
    if(codec==MVD_CODEC_H264_ANNEXB)
        d->avc=gui_mvd_decoder_create();
    else d->ff=mvd_ff_decoder_create(codec);
    if(!d->avc && !d->ff){free(d);return NULL;}
    return d;
}
int mvd_video_decoder_frame(mvd_codec_decoder *d,const uint8_t *p,size_t bytes,
                       uint8_t *rgba,size_t cap,unsigned *w,unsigned *h){
    if(!d)return -1;
    if(d->avc)return gui_mvd_decoder_frame(d->avc,p,bytes,rgba,cap,w,h);
    return mvd_ff_decoder_frame(d->ff,p,bytes,rgba,cap,w,h);
}
void mvd_video_decoder_free(mvd_codec_decoder *d) {
    if(!d)return;
    gui_mvd_decoder_destroy(d->avc);
    mvd_ff_decoder_free(d->ff);
    free(d);
}
