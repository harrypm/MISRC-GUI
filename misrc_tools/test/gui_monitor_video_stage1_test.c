#include "../misrc_gui/net/gui_monitor_video_wire.h"
#include "../misrc_gui/net/gui_monitor_video_options.h"
#include "../misrc_gui/net/gui_monitor_video_i420.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_headers(void) {
    const mvd_frame_header f = {MVD_CODEC_H264_ANNEXB, MVD_FLAG_KEYFRAME,
         0x01234567u, 77u, UINT64_C(1234567890123), 720, 576, 2500, 128};
    uint8_t encoded[MVD_HEADER_BYTES];
    mvd_frame_header d;
    assert(mvd_header_encode(encoded, &f) == 0);
    assert(memcmp(encoded, "MVD1", 4) == 0);
    assert(encoded[4] == 1 && encoded[5] == MVD_HEADER_BYTES);
    assert(mvd_header_decode(&d, encoded) == 0);
    assert(d.stream_id == f.stream_id && d.sequence == f.sequence &&
           d.pts_us == f.pts_us && d.payload_bytes == f.payload_bytes &&
           d.width == f.width && d.height == f.height && d.fps_x100 == 2500);
    encoded[0] = 0;
    assert(mvd_header_decode(&d, encoded) == -1);
    encoded[0] = 'M';
    encoded[4] = 2;
    assert(mvd_header_decode(&d, encoded) == -1);
    encoded[4] = 1;
    encoded[32] = 0x7f; /* adversarial > max payload */
    assert(mvd_header_decode(&d, encoded) == -1);
    encoded[32] = 0;
    encoded[31] = 1; /* reserved field */
    assert(mvd_header_decode(&d, encoded) == -1);
}

static void test_fragmented_reader(void) {
    const mvd_frame_header header = {MVD_CODEC_H264_ANNEXB, MVD_FLAG_KEYFRAME,
                                    1, 17, 999, 640, 480, 2500, 7};
    uint8_t message[MVD_HEADER_BYTES + 7];
    const uint8_t h264[7] = {0,0,0,1,0x65,0xac,0x7f};
    uint8_t payload[7];
    mvd_frame_reader r;
    assert(mvd_header_encode(message, &header) == 0);
    memcpy(message + MVD_HEADER_BYTES, h264, sizeof(h264));
    mvd_reader_init(&r, payload, sizeof(payload));
    for (size_t i = 0; i < sizeof(message); ++i) {
        size_t used = 100;
        int rc = mvd_reader_feed(&r, message+i, 1, &used);
        assert(used == 1);
        assert(rc == (i == sizeof(message)-1 ? 1 : 0));
    }
    assert(r.frame.sequence == 17 && r.frame.pts_us == 999);
    assert(memcmp(payload, h264, sizeof(payload)) == 0);
    mvd_reader_reset(&r);
    size_t consumed=0;
    assert(mvd_reader_feed(&r,message,sizeof(message),&consumed)==1);
    assert(consumed == sizeof(message));
    mvd_reader_init(&r, payload, 6);
    assert(mvd_reader_feed(&r,message,sizeof(message),&consumed)==-2);
    assert(consumed == MVD_HEADER_BYTES);
    mvd_reader_init(&r, payload, sizeof(payload));
    uint8_t bad[MVD_HEADER_BYTES];
    memcpy(bad,message,sizeof(bad)); bad[4]=0;
    assert(mvd_reader_feed(&r,bad,sizeof(bad),&consumed)==-1);
}

static void test_multi_frame(void) {
    mvd_frame_header h = {MVD_CODEC_H264_ANNEXB,0,1,1,20,320,240,2500,4};
    uint8_t packet[MVD_HEADER_BYTES + 4], payload[4];
    mvd_frame_reader r;
    size_t used;
    assert(mvd_header_encode(packet,&h)==0);
    memcpy(packet+MVD_HEADER_BYTES,"abcd",4);
    mvd_reader_init(&r,payload,sizeof(payload));
    assert(mvd_reader_feed(&r,packet,sizeof(packet),&used)==1 && used == sizeof(packet));
    assert(memcmp(payload,"abcd",4)==0);
    mvd_reader_reset(&r);
    h.sequence++;
    assert(mvd_header_encode(packet,&h)==0);
    assert(mvd_reader_feed(&r,packet,sizeof(packet),&used)==1);
    assert(r.frame.sequence==2);
}

static void test_i420(void) {
    uint8_t rgb[4*4*3];
    uint8_t yuv[4*4*3/2];
    for (int i=0;i<16;i++) {
        rgb[3*i]=255; rgb[3*i+1]=0; rgb[3*i+2]=0;
    }
    assert(mvd_i420_buffer_size(4,4) == sizeof(yuv));
    assert(mvd_i420_buffer_size(3,4) == 0);
    assert(mvd_rgb_to_i420(rgb,sizeof(rgb),4,4,4*3,MVD_RGB24,
                           yuv,sizeof(yuv),4,4)==0);
    for (int i=0;i<16;i++) assert(yuv[i]>=81 && yuv[i]<=83);
    for (int i=16;i<20;i++) assert(yuv[i]>=89 && yuv[i]<=91);
    for (int i=20;i<24;i++) assert(yuv[i]>=239 && yuv[i]<=241);
    assert(mvd_rgb_to_i420(rgb,sizeof(rgb)-1,4,4,12,MVD_RGB24,
                           yuv,sizeof(yuv),4,4)==-1);
    assert(mvd_rgb_to_i420(rgb,sizeof(rgb),4,4,12,MVD_RGB24,
                           yuv,sizeof(yuv),2,2)==0);
    uint8_t rgba[4*4*4];
    for (int i=0;i<16;i++) {
        rgba[4*i]=255;rgba[4*i+1]=255;rgba[4*i+2]=255;rgba[4*i+3]=0;
    }
    assert(mvd_rgb_to_i420(rgba,sizeof(rgba),4,4,16,MVD_RGBA32,
                           yuv,sizeof(yuv),4,4)==0);
    for (int i=0;i<16;i++) assert(yuv[i] == 235);
    for (int i=16;i<24;i++) assert(yuv[i] == 128);
}
static void test_monitor_remote_controls(void) {
    uint32_t c=mvd_ctl_default(0);
    assert(mvd_ctl_valid(c,0));
    assert(!mvd_ctl_valid(c,2));
    assert(mvd_ctl_get(c,MVD_CTL_SYNC,1u)==1);
    assert(mvd_ctl_get(c,MVD_CTL_COLOUR,1u)==1);
    assert(mvd_ctl_get(c,MVD_CTL_OSD,3u)==1);
    assert(mvd_ctl_budget(c)==100);
    c=mvd_ctl_set(c,MVD_CTL_SYSTEM,3u,2);
    c=mvd_ctl_set(c,MVD_CTL_FRAME,1u,1);
    c=mvd_ctl_set(c,MVD_CTL_COLOUR,1u,0);
    assert(mvd_ctl_valid(c,0));
    assert(mvd_ctl_get(c,MVD_CTL_SYSTEM,3u)==2);
    assert(mvd_ctl_get(c,MVD_CTL_FRAME,1u)==1);
    assert(mvd_ctl_get(c,MVD_CTL_COLOUR,1u)==0);
    c=mvd_ctl_set(c,MVD_CTL_BUDGET,7u,6);
    assert(!mvd_ctl_valid(c,0));
    static const unsigned budget[]={50,75,100,150,200,300};
    for(unsigned i=0;i<6;i++){
        c=mvd_ctl_set(c,MVD_CTL_BUDGET,7u,i);
        assert(mvd_ctl_valid(c,0));
        assert(mvd_ctl_budget(c)==budget[i]);
    }
    c=mvd_ctl_default(2);
    assert(mvd_ctl_rf_profile(c)==0); /* PAL VHS */
    c=mvd_ctl_set(c,MVD_CTL_SYSTEM,3u,1);
    assert(mvd_ctl_rf_profile(c)==1); /* NTSC VHS */
    c=mvd_ctl_set(c,MVD_CTL_SYSTEM,3u,2);
    assert(mvd_ctl_rf_profile(c)==6); /* SECAM VHS */
    c=mvd_ctl_set(c,MVD_CTL_MESECAM,1u,1);
    assert(mvd_ctl_rf_profile(c)==7); /* MESECAM VHS */
    c=mvd_ctl_set(c,MVD_CTL_TAPE,1u,1);
    for(unsigned i=0;i<4;i++){
        static const unsigned expected[]={2,3,5,8};
        c=mvd_ctl_set(c,MVD_CTL_BETA,3u,i);
        assert(mvd_ctl_rf_profile(c)==expected[i]);
    }
    c=mvd_ctl_set(c,MVD_CTL_TAPE_FAMILY,7u,7);
    assert(!mvd_ctl_valid(c,2));
    assert(mvd_ctl_rf_profile(mvd_ctl_default(3))==1); /* older NTSC client */
}

int main(void) {
    test_headers(); test_fragmented_reader(); test_multi_frame(); test_i420();
    test_monitor_remote_controls();
    puts("monitor video stage 1 tests: PASS");
    return 0;
}
