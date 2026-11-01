#include "../misrc_gui/net/gui_monitor_video_codec.h"
#include "../misrc_gui/net/gui_monitor_video_i420.h"
#include "../misrc_gui/net/gui_monitor_video_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    if (!gui_mvd_codec_available()) {
        assert(gui_mvd_encoder_create(320,240,2500,500000) == NULL);
        assert(gui_mvd_decoder_create() == NULL);
        puts("OpenH264 disabled: stubs PASS");
        return 0;
    }
    const unsigned w=320,h=240;
    const size_t size=mvd_i420_buffer_size(w,h);
    uint8_t *input=malloc(size);
    uint8_t *encoded=malloc(MVD_MAX_PAYLOAD);
    uint8_t *rgba=malloc((size_t)MVD_MAX_WIDTH*MVD_MAX_HEIGHT*4);
    assert(input&&encoded&&rgba);
    memset(input,95,(size_t)w*h);
    memset(input+(size_t)w*h,128,(size_t)w*h/2);
    gui_mvd_encoder_t *enc=gui_mvd_encoder_create(w,h,2500,600000);
    gui_mvd_decoder_t *dec=gui_mvd_decoder_create();
    assert(enc&&dec);
    int decoded=0,keys=0;
    for(int k=0;k<16;k++){
        size_t bytes=0;
        int key=0;
        int rc=gui_mvd_encoder_frame(enc,input,size,(uint64_t)k*40000,
                                      encoded,MVD_MAX_PAYLOAD,&bytes,&key);
        assert(rc>=0);
        if(rc==0)continue;
        assert(bytes>4 && bytes<MVD_MAX_PAYLOAD);
        if(key)keys++;
        unsigned ow=0,oh=0;
        int d=gui_mvd_decoder_frame(dec,encoded,bytes,rgba,
                (size_t)MVD_MAX_WIDTH*MVD_MAX_HEIGHT*4,&ow,&oh);
        assert(d>=0);
        if(d==1){ assert(ow==w && oh==h); assert(rgba[3]==255); decoded++; }
    }
    assert(keys>=1 && decoded>=1);
    gui_mvd_encoder_destroy(enc);gui_mvd_decoder_destroy(dec);
    free(input);free(encoded);free(rgba);
    puts("OpenH264 encode/decode roundtrip PASS");
    return 0;
}
