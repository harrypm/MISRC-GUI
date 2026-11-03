#include "../misrc_gui/net/gui_monitor_video_ffmpeg.h"
#include "../misrc_gui/net/gui_monitor_video_mux.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void exercise(unsigned codec) {
    if(!(mvd_ff_encode_mask(0)&MVD_CODEC_BIT(codec)) ||
       !(mvd_ff_decode_mask()&MVD_CODEC_BIT(codec)))return;
    const unsigned w=160,h=120;
    mvd_ff_encoder *encoder=mvd_ff_encoder_create(codec,w,h,2500,350000,0);
    if(!encoder){fprintf(stderr,"[ffmpeg-test] %s encoder not openable\n",
                          mvd_codec_name(codec));return;}
    mvd_ff_decoder *decoder=mvd_ff_decoder_create(codec);
    assert(decoder!=NULL);
    size_t size=(size_t)w*h*3u/2u;
    uint8_t *raw=malloc(size),*encoded=malloc(MVD_MAX_PAYLOAD);
    uint8_t *rgba=malloc((size_t)w*h*4u);
    assert(raw&&encoded&&rgba);
    memset(raw,90,(size_t)w*h);
    memset(raw+(size_t)w*h,128,(size_t)w*h/2u);
    unsigned encoded_frames=0,decoded_frames=0;
    for(unsigned i=0;i<20;i++){
        size_t len=0;int key=0;
        int rc=mvd_ff_encoder_frame(encoder,raw,size,(uint64_t)i*40000,
                                   encoded,MVD_MAX_PAYLOAD,&len,&key);
        assert(rc>=0);
        if(rc==0)continue;
        assert(len>0 && len<=MVD_MAX_PAYLOAD);
        encoded_frames++;
        unsigned dw=0,dh=0;
        rc=mvd_ff_decoder_frame(decoder,encoded,len,rgba,(size_t)w*h*4u,&dw,&dh);
        assert(rc>=0);
        if(rc==1){
            assert(dw==w && dh==h);
            decoded_frames++;
        }
    }
    fprintf(stderr,"[ffmpeg-test] %s frames: encoded=%u decoded=%u\n",
           mvd_codec_name(codec),encoded_frames,decoded_frames);
    /* Some AV1 encoders queue more than one second of frames. VP9 / HEVC
     * low-latency codecs must emit and decode actual frames. */
    if(codec!=MVD_CODEC_AV1)assert(encoded_frames && decoded_frames);
    free(raw);free(encoded);free(rgba);
    mvd_ff_decoder_free(decoder);
    mvd_ff_encoder_free(encoder);
}
static void test_mux_negotiation(void) {
    /* Link and exercise the same mux as the M-Server/M-Client. With the
     * OpenH264 test stub disabled, this proves AV1/VP9/HEVC are genuinely
     * selectable and the decoder can follow the chosen wire ID. */
    uint32_t dec=mvd_video_decoder_mask();
    assert((dec & MVD_CODEC_AVC_BIT)==0); /* This test intentionally omits OpenH264. */
    mvd_codec_encoder *encoder=mvd_video_encoder_best(dec,160,120,2500,350000);
    assert(encoder!=NULL);
    unsigned codec=mvd_video_encoder_codec(encoder);
    assert(codec==MVD_CODEC_AV1 || codec==MVD_CODEC_VP9 ||
           codec==MVD_CODEC_HEVC_ANNEXB);
    mvd_codec_decoder *decoder=mvd_video_decoder_new(codec);
    assert(decoder!=NULL);
    const unsigned w=160,h=120;
    const size_t raw_size=(size_t)w*h*3u/2u;
    uint8_t *raw=malloc(raw_size);
    uint8_t *encoded=malloc(MVD_MAX_PAYLOAD);
    uint8_t *rgba=malloc((size_t)w*h*4u);
    assert(raw && encoded && rgba);
    memset(raw,100,raw_size);
    unsigned decoded=0;
    for(unsigned i=0;i<24;i++){
        int key=0;size_t bytes=0;
        int rc=mvd_video_encoder_frame(encoder,raw,raw_size,(uint64_t)i*40000,
                                       encoded,MVD_MAX_PAYLOAD,&bytes,&key);
        assert(rc>=0);
        if(rc==0)continue;
        unsigned rw=0,rh=0;
        rc=mvd_video_decoder_frame(decoder,encoded,bytes,rgba,
                                   (size_t)w*h*4u,&rw,&rh);
        assert(rc>=0);
        if(rc==1){assert(rw==w && rh==h);decoded++;}
    }
    fprintf(stderr,"[mux-test] selected %s, decoded %u frames\\n",
            mvd_codec_name(codec),decoded);
    assert(decoded>0);
    free(raw);free(encoded);free(rgba);
    mvd_video_decoder_free(decoder);
    mvd_video_encoder_free(encoder);
}
int main(void) {
    printf("FFmpeg decode mask=%u software encode mask=%u hardware mask=%u\n",
           (unsigned)mvd_ff_decode_mask(),(unsigned)mvd_ff_encode_mask(0),
           (unsigned)mvd_ff_encode_mask(1));
    exercise(MVD_CODEC_VP9);
    exercise(MVD_CODEC_HEVC_ANNEXB);
    exercise(MVD_CODEC_AV1);
    test_mux_negotiation();
    puts("monitor FFmpeg backend smoke test: PASS");
    return 0;
}
