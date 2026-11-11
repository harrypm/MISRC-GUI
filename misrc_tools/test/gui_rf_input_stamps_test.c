/* Exercise production playback sideband ownership through wrap, overflow,
 * seek epochs, final-block padding and concurrent producer/consumer copies. */
#include "../misrc_gui/processing/gui_extract.c"
#include "../misrc_gui/processing/gui_rf_stream_marks.c"
#include <assert.h>

/* Run the actual extraction start/loop/stop with controlled FIFO readiness.
 * Only its surrounding hardware/record/display services are replaced. */
atomic_int do_exit = 0;
static atomic_bool stop_test_continuous;
static atomic_bool stop_test_done;
static atomic_uint stop_test_reads;
static atomic_size_t client_discard_test_tail;
_Alignas(16) static uint32_t stop_test_raw[BUFFER_READ_SIZE];
static atomic_bool rtl_alignment_test_active;
static atomic_size_t rtl_alignment_available, rtl_alignment_consumed;
static atomic_size_t rtl_alignment_scalar, rtl_alignment_bulk;
static atomic_bool record_test_active;
static atomic_bool record_test_input_ready;
static atomic_bool record_test_input_taken;
static atomic_bool record_test_preview_done;
static atomic_uint_fast64_t record_test_bytes[2];
static int16_t record_test_output[2][BUFFER_READ_SIZE];
static int16_t record_test_display[2][BUFFER_READ_SIZE];
static size_t record_test_samples;
static atomic_bool record_test_hold_write;
static atomic_bool record_test_write_entered;
static atomic_bool record_test_write_release;
static atomic_bool record_test_finish_done;
static size_t record_test_preview_counts[2];
static atomic_bool independent_tail_test_active, independent_tail_test_taken;
static const int16_t independent_tail_test_samples[13] = {123};

static void test_convert(void *input, size_t count, size_t *clip, uint8_t *aux,
                         void *output_a, void *output_b, uint16_t *peak) {
    (void)input; (void)clip; (void)aux; (void)peak;
    if (atomic_load(&record_test_active)) {
        for (size_t i = 0; i < count; ++i) ((int16_t *)output_a)[i] = 111;
        if (output_b) for (size_t i = 0; i < count; ++i) ((int16_t *)output_b)[i] = 222;
    } else {
        memset(output_a, 0, count * sizeof(int16_t));
        if (output_b) memset(output_b, 0, count * sizeof(int16_t));
    }
    /* Input remains available throughout: this yields CPU, not FIFO data. */
    thrd_sleep_ms(1);
}
static void stop_test_convert(void *input, size_t count, size_t *clip, uint8_t *aux,
                              void *output_a, void *output_b, uint16_t *peak) {
    if (atomic_load(&rtl_alignment_test_active)) {
        assert((uintptr_t)input % 16 == 0 && count % 8 == 0);
        atomic_fetch_add(&rtl_alignment_bulk, count);
    }
    test_convert(input, count, clip, aux, output_a, output_b, peak);
}
conv_function_t get_conv_function(bool single, bool pad, bool dword, bool peak,
                                  void *out_a, void *out_b) {
    (void)single; (void)pad; (void)dword; (void)peak; (void)out_a; (void)out_b;
    return stop_test_convert;
}
conv_16to8_t get_16to8_function(void) { return NULL; }
conv_16to32_t get_16to8to32_function(void) { return NULL; }
void extract_AB_C(uint32_t *input, size_t count, size_t *clip, uint8_t *aux,
                  int16_t *output_a, int16_t *output_b, uint16_t *peak) {
    if (atomic_load(&rtl_alignment_test_active)) atomic_fetch_add(&rtl_alignment_scalar, count);
    test_convert(input, count, clip, aux, output_a, output_b, peak);
}
int rb_event_init(rb_event_t *event) { (void)event; return -1; }
void rb_event_destroy(rb_event_t *event) { (void)event; }
int bufmgr_ensure_init(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; return 0; }
void *bufmgr_read_begin(buffer_manager_t *mgr, buffer_id_t id, size_t bytes, int timeout_ms) {
    (void)mgr;
    if (atomic_load(&rtl_alignment_test_active)) {
        size_t consumed = atomic_load(&rtl_alignment_consumed);
        size_t available = atomic_load(&rtl_alignment_available) - consumed;
        assert(id == BUF_CAPTURE_RF && bytes <= available * sizeof(uint32_t));
        return stop_test_raw + consumed;
    }
    if (atomic_load(&independent_tail_test_active)) {
        assert(id == BUF_CAPTURE_RF_B && bytes == sizeof(independent_tail_test_samples));
        assert(!atomic_exchange(&independent_tail_test_taken, true));
        return (void *)independent_tail_test_samples;
    }
    size_t sample_count = atomic_load(&record_test_active) ? record_test_samples : BUFFER_READ_SIZE;
    assert(id == BUF_CAPTURE_RF && bytes == sample_count * sizeof(stop_test_raw[0]));
    atomic_fetch_add(&stop_test_reads, 1);
    if (atomic_load(&record_test_active)) {
        if (atomic_load(&record_test_input_ready) && !atomic_exchange(&record_test_input_taken, true))
            return stop_test_raw;
        thrd_sleep_ms(timeout_ms);
        return NULL;
    }
    if (atomic_load(&stop_test_continuous)) return stop_test_raw;
    thrd_sleep_ms(timeout_ms);
    return NULL;
}
void bufmgr_read_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes) {
    (void)mgr;
    if (id == BUF_CAPTURE_RF && atomic_load(&rtl_alignment_test_active)) {
        atomic_fetch_add(&rtl_alignment_consumed, bytes / sizeof(uint32_t));
        return;
    }
    size_t stale = atomic_load(&client_discard_test_tail);
    if (id == BUF_CAPTURE_RF && stale) {
        assert(bytes == stale);
        atomic_store(&client_discard_test_tail, 0);
    }
}
void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id, size_t bytes,
                          const backpressure_policy_t *policy) {
    (void)mgr; (void)bytes; (void)policy;
    if (atomic_load(&record_test_active)) {
        if (id == BUF_DISPLAY && record_test_samples < BUFFER_READ_SIZE) return record_test_display;
        if (id == BUF_RECORD_A) {
            if (atomic_load(&record_test_hold_write)) {
                atomic_store(&record_test_write_entered, true);
                while (!atomic_load(&record_test_write_release)) thrd_sleep_ms(1);
            }
            return record_test_output[0];
        }
        if (id == BUF_RECORD_B) return record_test_output[1];
    }
    return NULL;
}
void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes) {
    (void)mgr;
    if (id == BUF_RECORD_A) atomic_fetch_add(&record_test_bytes[0], bytes);
    if (id == BUF_RECORD_B) atomic_fetch_add(&record_test_bytes[1], bytes);
}
size_t bufmgr_fill_level(buffer_manager_t *mgr, buffer_id_t id) {
    (void)mgr;
    if (id == BUF_CAPTURE_RF && atomic_load(&rtl_alignment_test_active))
        return (atomic_load(&rtl_alignment_available) - atomic_load(&rtl_alignment_consumed)) * sizeof(uint32_t);
    if (id == BUF_CAPTURE_RF && atomic_load(&client_discard_test_tail))
        return atomic_load(&client_discard_test_tail);
    if (id == BUF_CAPTURE_RF && atomic_load(&record_test_active) &&
        atomic_load(&record_test_input_ready) && !atomic_load(&record_test_input_taken))
        return record_test_samples * sizeof(stop_test_raw[0]);
    return 0;
}
bool bufmgr_wait_data(buffer_manager_t *mgr, buffer_id_t id, int timeout_ms) {
    (void)mgr; (void)id;
    thrd_sleep_ms(timeout_ms);
    return false;
}
void bufmgr_log_periodic(buffer_manager_t *mgr) { (void)mgr; }
void gui_video_submit_rf(gui_app_t *app, const int16_t *a, const int16_t *b,
                          size_t count_a, size_t count_b, const gui_rf_block_info_t info[2], bool supported) {
    (void)app; (void)a; (void)b; (void)count_a; (void)count_b; (void)info; (void)supported;
    if (atomic_load(&independent_tail_test_active)) {
        assert(!a && count_a == 0 && b && b[0] == 123 && count_b == 13 && supported);
        assert(info[1].start_sample == 1000 && info[1].sample_rate_hz == 40000000);
        assert(gui_extract_playback_stamps_forwarded(1) == 0);
    }
    if (atomic_load(&record_test_active)) {
        if (s_tagged_input) assert(gui_extract_playback_stamps_forwarded(-1) == 0);
        record_test_preview_counts[0] = count_a;
        record_test_preview_counts[1] = count_b;
        atomic_store(&record_test_preview_done, true);
    }
}
bool gui_record_check_disk_space_guard(gui_app_t *app, uint32_t frame, char *msg, size_t capacity) {
    (void)app; (void)frame; (void)msg; (void)capacity; return false;
}
bool gui_record_push_converted(gui_app_t *app, int channel, const int16_t *samples,
                               size_t bytes, uint32_t frame, char *msg, size_t capacity) {
    (void)frame; (void)msg; (void)capacity;
    buffer_id_t id = channel ? BUF_RECORD_B : BUF_RECORD_A;
    void *out = bufmgr_write_begin(&app->buffers, id, bytes, NULL);
    if (!out) return false;
    memcpy(out, samples, bytes);
    bufmgr_write_end(&app->buffers, id, bytes);
    return true;
}
void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t count) {
    (void)app; (void)level; (void)message; (void)error_class; (void)count;
}

static int stop_test_call_stop(void *unused) {
    (void)unused;
    gui_extract_stop();
    atomic_store(&stop_test_done, true);
    return 0;
}
static void stop_test_timeout(const char *condition) {
    fprintf(stderr, "Extraction stop regression timed out: %s\n", condition);
    /* Exit the entire test process, rather than joining a potentially stuck
     * regression thread. The test can never turn this failure into a hang. */
    exit(1);
}

static void rtl_short_read_realign_test(void) {
    gui_app_t *app = calloc(1, sizeof(*app));
    assert(app);
    app->is_capturing = true;
    app->capture_has_channel_b = true;
    atomic_store(&app->rf_source_is_rtlsdr, true);
    atomic_store(&rtl_alignment_test_active, true);
    atomic_store(&rtl_alignment_available, 1);
    assert(gui_extract_start(app) == 0);

    uint64_t until = get_time_ms() + 2000;
    while (atomic_load(&rtl_alignment_consumed) != 1) {
        if (get_time_ms() >= until) stop_test_timeout("extracting first short RTL-SDR completion");
        thrd_sleep_ms(1);
    }
    atomic_store(&rtl_alignment_available, 16);
    while (atomic_load(&rtl_alignment_consumed) != 16) {
        if (get_time_ms() >= until) stop_test_timeout("realigning RTL-SDR after a short completion");
        thrd_sleep_ms(1);
    }
    gui_extract_stop();
    assert(atomic_load(&rtl_alignment_scalar) == 8);
    assert(atomic_load(&rtl_alignment_bulk) == 8);
    atomic_store(&rtl_alignment_test_active, false);
    free(app);
    puts("RTL-SDR: scalar short read and alignment prefix preserve all samples and resume SIMD");
}

static void client_discard_test(void) {
    gui_app_t *app = calloc(1, sizeof(*app));
    assert(app);
    app->is_capturing = true;
    ringbuffer_t *rf = &app->buffers.buffers[BUF_CAPTURE_RF];
    atomic_store(&rf->head, 65536 * sizeof(uint32_t));
    atomic_store(&rf->tail, 65538 * sizeof(uint32_t));
    atomic_store(&client_discard_test_tail, 2 * sizeof(uint32_t));
    atomic_store(&stop_test_continuous, false);
    assert(gui_extract_start(app) == 0);
    uint64_t until = get_time_ms() + 2000;
    while (!gui_extract_discard_client_rf(app)) {
        if (get_time_ms() >= until) stop_test_timeout("discarding short Client RF tail");
        thrd_sleep_ms(1);
    }
    assert(!atomic_load(&client_discard_test_tail));
    assert(!atomic_load(&rf->head) && !atomic_load(&rf->tail));
    gui_extract_stop();
    free(app);
    puts("Client RF reconnect: short old tail discarded and SIMD alignment restored before acknowledgement");
}

static void stop_test_case(bool continuous) {
    gui_app_t *app = calloc(1, sizeof(*app));
    assert(app);
    app->is_capturing = true; /* Exactly the startup-failure deadlock condition. */
    app->capture_has_channel_b = true;
    atomic_init(&app->rf_preview_input_supported, true);
    atomic_init(&app->total_samples, 0);
    atomic_init(&app->samples_a, 0);
    atomic_init(&app->samples_b, 0);
    atomic_init(&app->clip_count_a_pos, 0); atomic_init(&app->clip_count_a_neg, 0);
    atomic_init(&app->clip_count_b_pos, 0); atomic_init(&app->clip_count_b_neg, 0);
    atomic_init(&app->peak_a_pos, 0); atomic_init(&app->peak_a_neg, 0);
    atomic_init(&app->peak_b_pos, 0); atomic_init(&app->peak_b_neg, 0);
    atomic_store(&stop_test_continuous, continuous);
    atomic_store(&stop_test_done, false);
    atomic_store(&stop_test_reads, 0);
    assert(gui_extract_start_tagged(app) == 0);
    assert(s_buf_a && s_buf_b && s_buf_aux);

    uint64_t begin = get_time_ms();
    unsigned minimum_reads = continuous ? 3u : 1u;
    while (atomic_load(&stop_test_reads) < minimum_reads) {
        if (get_time_ms() - begin > 2000u) stop_test_timeout("reader did not start/restart");
        thrd_sleep_ms(1);
    }
    thrd_t stopper;
    assert(thrd_create_with_priority(&stopper, stop_test_call_stop, NULL, THRD_PRIORITY_NORMAL) == thrd_success);
    begin = get_time_ms();
    while (!atomic_load(&stop_test_done)) {
        if (get_time_ms() - begin > 2000u)
            stop_test_timeout(continuous ? "input continuously available" : "empty FIFO, is_capturing=true");
        thrd_sleep_ms(1);
    }
    thrd_join(stopper, NULL);
    assert(app->is_capturing && !gui_extract_is_running());
    /* A stop/start round trip must clear the prior independent stop request. */
    free(app);
}

static int record_test_finish(void *unused) {
    (void)unused;
    gui_extract_wait_recording_idle();
    atomic_store(&record_test_finish_done, true);
    return 0;
}

/* A preview metadata failure must not silence raw recording or alter its A/B
 * swap. Known per-block channel presence also wins over a newer capability. */
static void record_test_case(bool tagged, bool metadata, bool unsafe,
                              bool block_has_b, bool capability_b, bool swap, bool rtl) {
    gui_app_t *app = calloc(1, sizeof(*app));
    assert(app);
    app->is_capturing = app->is_recording = true;
    app->capture_has_channel_b = capability_b;
    app->capture_backend_upstream = true; /* Keep both conversion outputs. */
    app->capture_mode_runtime_misrc = swap;
    app->user_capture_mode_misrc = swap;
    app->settings.capture_a = app->settings.capture_b = true;
    atomic_init(&app->rf_preview_input_supported, true);
    atomic_init(&app->rf_source_is_rtlsdr, rtl);
    record_test_samples = rtl ? 13 : BUFFER_READ_SIZE;
    atomic_store(&record_test_active, true);
    atomic_store(&record_test_input_ready, false);
    atomic_store(&record_test_input_taken, false);
    atomic_store(&record_test_preview_done, false);
    for (int ch = 0; ch < 2; ++ch) {
        atomic_store(&record_test_bytes[ch], 0);
        record_test_preview_counts[ch] = 0;
    }
    memset(record_test_output, 0, sizeof(record_test_output));
    memset(record_test_display, 0xff, sizeof(record_test_display));
    gui_rf_block_info_t info[2] = {{20000000u, 0, 1, unsafe}, {20000000u, 0, 1, unsafe}};
    if (!block_has_b) info[1].sample_rate_hz = 0;
    size_t counts[2] = {BUFFER_READ_SIZE, block_has_b ? BUFFER_READ_SIZE : 0};
    assert(s_rf_marks == NULL);
    if (!tagged && metadata) {
        s_rf_marks = gui_rf_stream_marks_create();
        assert(s_rf_marks);
        gui_rf_stream_marks_observe(s_rf_marks, 0, sizeof(stop_test_raw), info, unsafe);
    }
    assert((tagged ? gui_extract_start_tagged(app) : gui_extract_start(app)) == 0);
    if (tagged && metadata) assert(gui_extract_stamp_playback(info, counts));
    gui_extract_set_recording(true, false, 16, 16, false, false);
    atomic_store(&record_test_input_ready, true);
    uint64_t until = get_time_ms() + 2000;
    if (atomic_load(&record_test_hold_write)) {
        while (!atomic_load(&record_test_write_entered)) {
            if (get_time_ms() >= until) stop_test_timeout("record producer did not enter write");
            thrd_sleep_ms(1);
        }
        // The caller must be able to disarm while the publisher is blocked.
        gui_extract_disable_recording();
        assert(!gui_extract_is_recording(NULL));
        thrd_t finisher;
        assert(thrd_create_with_priority(&finisher, record_test_finish, NULL,
                                         THRD_PRIORITY_NORMAL) == thrd_success);
        thrd_sleep_ms(20);
        assert(!atomic_load(&record_test_finish_done));
        atomic_store(&record_test_write_release, true);
        thrd_join(finisher, NULL);
        assert(atomic_load(&record_test_finish_done));
        assert(atomic_load(&record_test_bytes[0]) == record_test_samples * sizeof(int16_t));
        assert(atomic_load(&record_test_bytes[1]) == record_test_samples * sizeof(int16_t));
        atomic_store(&record_test_hold_write, false);
    }
    while (!atomic_load(&record_test_preview_done)) {
        if (get_time_ms() >= until) stop_test_timeout("record/preview isolation block");
        thrd_sleep_ms(1);
    }
    gui_extract_stop();
    bool expected_b = metadata ? block_has_b : capability_b;
    bool expected_swap = expected_b && swap && !tagged;
    size_t expected_bytes = record_test_samples * sizeof(int16_t);
    assert(atomic_load(&record_test_bytes[0]) == expected_bytes);
    assert(atomic_load(&record_test_bytes[1]) == (expected_b ? expected_bytes : 0));
    assert(atomic_load(&app->total_samples) == record_test_samples);
    assert(atomic_load(&app->samples_a) == record_test_samples);
    assert(atomic_load(&app->samples_b) == (expected_b ? record_test_samples : 0));
    for (size_t i = 0; i < BUFFER_READ_SIZE; ++i) {
        bool valid = i < record_test_samples;
        assert(record_test_output[0][i] == (valid ? (expected_swap ? 222 : 111) : 0));
        assert(record_test_output[1][i] == (valid && expected_b ? (expected_swap ? 111 : 222) : 0));
        if (rtl) {
            assert(record_test_display[0][i] == record_test_output[0][i]);
            assert(record_test_display[1][i] == record_test_output[1][i]);
        }
    }
    bool preview_safe = metadata && (tagged || !unsafe);
    assert(record_test_preview_counts[0] == (preview_safe ? (expected_swap ? counts[1] : counts[0]) : 0));
    assert(record_test_preview_counts[1] == (preview_safe ? (expected_swap ? counts[0] : counts[1]) : 0));
    gui_rf_stream_marks_destroy(s_rf_marks);
    s_rf_marks = NULL;
    atomic_store(&record_test_active, false);
    free(app);
}

static void independent_tail_test(void) {
    gui_app_t *app = calloc(1, sizeof(*app));
    assert(app);
    app->is_capturing = true;
    atomic_init(&app->playback_independent_mode, true);
    atomic_store(&independent_tail_test_active, true);
    atomic_store(&independent_tail_test_taken, false);
    assert(gui_extract_start_tagged(app) == 0);
    gui_rf_block_info_t info = {.sample_rate_hz = 40000000, .start_sample = 1000, .epoch = 7};
    assert(gui_extract_stamp_playback_channel(1, &info, 13));
    uint64_t target = gui_extract_playback_stamps_written(1);
    assert(target == 1);
    uint64_t until = get_time_ms() + 2000;
    while (gui_extract_playback_stamps_forwarded(1) < target) {
        if (get_time_ms() >= until) stop_test_timeout("forwarding mixed-rate short RF tail");
        thrd_sleep_ms(1);
    }
    assert(atomic_load(&app->samples_b) == 13);
    gui_extract_stop();
    atomic_store(&independent_tail_test_active, false);
    free(app);
}

#define STAMP_TEST_BLOCKS 100000u
static void stamp_test_data(uint64_t n, gui_rf_block_info_t info[2], size_t counts[2]) {
    for (int ch=0; ch<2; ++ch) {
        info[ch] = (gui_rf_block_info_t){
            .sample_rate_hz = (uint32_t[]){20000000,40000000,80000000}[n%3],
            .start_sample = (n%1000)*65536u + ch,
            .epoch = n/1000 + ch*10000u,
            .discontinuity = n%1000 == 0
        };
        counts[ch] = n%1000 == 999 ? 1234u : 65536u;
    }
}
static void stamp_test_check(uint64_t n, const rf_input_stamp_t *stamp) {
    gui_rf_block_info_t info[2]; size_t counts[2];
    stamp_test_data(n,info,counts);
    for (int ch=0;ch<2;++ch) {
        assert(stamp->info[ch].sample_rate_hz == info[ch].sample_rate_hz);
        assert(stamp->info[ch].start_sample == info[ch].start_sample);
        assert(stamp->info[ch].epoch == info[ch].epoch);
        assert(stamp->info[ch].discontinuity == info[ch].discontinuity);
        assert(stamp->counts[ch] == counts[ch]);
    }
}
static int stamp_test_producer(void *unused) {
    (void)unused;
    for(uint64_t n=0;n<STAMP_TEST_BLOCKS;++n) {
        gui_rf_block_info_t info[2]; size_t counts[2]; stamp_test_data(n,info,counts);
        while(!gui_extract_stamp_playback(info,counts)) { }
    }
    return 0;
}
int main(void) {
    s_tagged_input=true;
    atomic_store(&s_stamp_write,0); atomic_store(&s_stamp_read,0);
    for(uint64_t n=0;n<RF_STAMP_CAPACITY;++n) {
        gui_rf_block_info_t info[2]; size_t counts[2]; stamp_test_data(n,info,counts);
        assert(gui_extract_stamp_playback(info,counts));
    }
    gui_rf_block_info_t extra[2]={{0}}; size_t extra_counts[2]={1,1};
    assert(!gui_extract_stamp_playback(extra,extra_counts));
    for(uint64_t n=0;n<RF_STAMP_CAPACITY;++n) {
        rf_input_stamp_t stamp; assert(read_rf_stamp(&stamp)); stamp_test_check(n,&stamp);
    }
    rf_input_stamp_t stamp; assert(!read_rf_stamp(&stamp));
    atomic_store(&s_stamp_write,0); atomic_store(&s_stamp_read,0);
    thrd_t producer;
    assert(thrd_create_with_priority(&producer,stamp_test_producer,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    for(uint64_t n=0;n<STAMP_TEST_BLOCKS;++n) {
        while(!read_rf_stamp(&stamp)) { }
        stamp_test_check(n,&stamp);
    }
    thrd_join(producer,NULL);
    assert(!read_rf_stamp(&stamp));
    s_tagged_input=false;
    assert(!gui_extract_stamp_playback(extra,extra_counts));
    puts("RF playback stamps: wrap, overflow, seek, padding and concurrent ownership passed");
    stop_test_case(false);
    stop_test_case(true);
    client_discard_test();
    record_test_case(false, true, false, true, true, false, false);
    record_test_case(false, false, false, true, true, false, false);
    record_test_case(false, false, false, true, true, true, false);
    record_test_case(false, false, false, false, false, true, false);
    record_test_case(false, true, true, true, false, true, false);
    record_test_case(false, true, true, false, true, true, false);
    record_test_case(true, true, false, true, true, true, false);
    record_test_case(true, false, false, true, true, false, false);
    record_test_case(true, false, false, true, true, true, false);
    record_test_case(true, true, true, false, true, true, false);
    // A short RTL-SDR USB read keeps exact counts in both recordings, while
    // the fixed oscilloscope frame still receives display-only padding.
    record_test_case(false, false, false, true, true, false, true);
    record_test_case(false, false, false, true, true, true, true);
    puts("RTL-SDR short read: exact A/B recording and stats with display-only padding passed");
    rtl_short_read_realign_test();
    atomic_store(&record_test_hold_write, true);
    record_test_case(false, false, false, true, true, false, true);
    puts("Recording stop waits for the already accepted A/B short block before drain passed");
    puts("Extraction recording/preview isolation: missing metadata, known unsafe mapping and A/B swap passed");
    independent_tail_test();
    gui_extract_cleanup();
    puts("Extraction actual-thread stop: empty and continuously available input, is_capturing=true passed");
    return 0;
}
