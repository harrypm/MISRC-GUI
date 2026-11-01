#include "../misrc_gui/net/gui_monitor_video_framebox.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

static void basic_test(void) {
    gui_monitor_video_framebox_t *box = gui_monitor_video_framebox_create(8, 8);
    assert(box);
    uint8_t src[8 * 8 * 4], dst[8 * 8 * 4];
    memset(src, 0x5a, sizeof(src));
    uint64_t generation = 0;
    gui_monitor_video_frame_info_t info;
    assert(!gui_monitor_video_framebox_take(box, dst, sizeof(dst), &generation, &info));
    assert(!gui_monitor_video_framebox_publish(box, src, 2, 32, 8, 8, 4, 1));
    assert(!gui_monitor_video_framebox_publish(box, src, sizeof(src), 3, 8, 8, 4, 1));
    assert(!gui_monitor_video_framebox_publish(box, src, sizeof(src), 32, 9, 8, 4, 1));
    assert(gui_monitor_video_framebox_publish(box, src, sizeof(src), 32, 8, 8, 4, 123));
    assert(!gui_monitor_video_framebox_take(box, dst, 15, &generation, &info));
    assert(generation == 0);
    assert(gui_monitor_video_framebox_take(box, dst, sizeof(dst), &generation, &info));
    assert(generation == 1 && info.generation == 1 && info.timestamp_us == 123);
    assert(info.width == 8 && info.height == 8 && info.channels == 4);
    assert(!memcmp(src, dst, sizeof(src)));
    assert(!gui_monitor_video_framebox_take(box, dst, sizeof(dst), &generation, &info));
    uint8_t rgb[5*4*3];
    memset(rgb, 0xa3, sizeof(rgb));
    gui_monitor_video_framebox_cvbs_sink(box, rgb, 5, 4, 124);
    assert(gui_monitor_video_framebox_take(box, dst, sizeof(dst), &generation, &info));
    assert(generation == 2 && info.channels == 3 && info.width == 5);
    assert(!memcmp(rgb,dst,sizeof(rgb)));
    uint8_t rgba[2*2*4];
    memset(rgba, 0x4e, sizeof(rgba));
    gui_monitor_video_framebox_rf_sink(box, rgba, 2, 2, 125);
    assert(gui_monitor_video_framebox_take(box,dst,sizeof(dst),&generation,&info));
    assert(info.channels == 4 && generation == 3);
    assert(gui_monitor_video_framebox_published(box) == 3);
    gui_monitor_video_framebox_destroy(box);
    assert(gui_monitor_video_framebox_create(0, 2) == NULL);
}

typedef struct {
    gui_monitor_video_framebox_t *box;
    atomic_bool run;
} test_ctx;
static int producer(void *p) {
    test_ctx *ctx = p;
    uint8_t frame[320 * 240 * 3];
    for (unsigned i = 0; i < 12000; i++) {
        memset(frame, (int)(i & 255), sizeof(frame));
        (void)gui_monitor_video_framebox_publish(ctx->box, frame, sizeof(frame),
                                                  320 * 3, 320, 240, 3, i);
    }
    atomic_store(&ctx->run, false);
    return 0;
}
static void concurrency_test(void) {
    test_ctx ctx = {0};
    ctx.box = gui_monitor_video_framebox_create(320, 240);
    assert(ctx.box);
    atomic_init(&ctx.run, true);
    thrd_t worker;
    assert(thrd_create(&worker, producer, &ctx) == thrd_success);
    uint8_t frame[320 * 240 * 3];
    uint64_t generation = 0;
    gui_monitor_video_frame_info_t info;
    while (atomic_load(&ctx.run)) {
        if (!gui_monitor_video_framebox_take(ctx.box, frame, sizeof(frame),
                                              &generation, &info)) continue;
        uint8_t value = frame[0];
        for (size_t i = 1; i < sizeof(frame); i++) assert(frame[i] == value);
    }
    thrd_join(worker, NULL);
    assert(gui_monitor_video_framebox_published(ctx.box) > 0);
    gui_monitor_video_framebox_destroy(ctx.box);
}
int main(void) {
    basic_test();
    concurrency_test();
    puts("monitor video framebox tests: PASS");
    return 0;
}
