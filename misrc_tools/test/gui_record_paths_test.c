/* Exercise the production recording filename and overwrite preflight without hardware. */
#ifdef _WIN32
#include <direct.h>
#endif
#include <stdatomic.h>
static void record_fixture_after_load(const void *ptr);
#undef atomic_load
#define atomic_load(object) __extension__ ({ \
    __auto_type fixture_value = atomic_load_explicit((object), memory_order_seq_cst); \
    record_fixture_after_load(object); \
    fixture_value; \
})
#define soxr_create record_fixture_soxr_create
#define mls_proxy_write record_fixture_write
#define mls_proxy_close record_fixture_close
#include "../misrc_gui/output/gui_record.c"
#undef atomic_load
#define atomic_load(object) atomic_load_explicit((object), memory_order_seq_cst)
#undef soxr_create
#undef mls_proxy_write
#undef mls_proxy_close
#undef fwrite
#undef fclose
#undef fflush
#undef ferror
#undef clearerr
#undef file_open_utf8
#if defined(_WIN32)
static BOOL WINAPI record_fixture_duplicate_stage(HANDLE process, HANDLE source, HANDLE target,
    LPHANDLE duplicate, DWORD access, BOOL inherit, DWORD options);
static BOOL WINAPI record_fixture_set_stage_info(HANDLE stage, FILE_INFO_BY_HANDLE_CLASS info_class,
    LPVOID data, DWORD bytes);
#define DuplicateHandle record_fixture_duplicate_stage
#define SetFileInformationByHandle record_fixture_set_stage_info
#else
#include <sys/stat.h>
static int record_fixture_stat_at(int fd, const char *name, struct stat *info, int flags);
#define fstatat record_fixture_stat_at
#endif
#include "../misrc_gui/net/monitor_local_save_files.c"
#if defined(_WIN32)
#undef DuplicateHandle
#undef SetFileInformationByHandle
#else
#undef fstatat
#endif
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <assert.h>

static FILE *record_fixture_gated_output;
static atomic_bool record_fixture_hold_write;
static atomic_bool record_fixture_write_entered;
static atomic_bool record_fixture_release_write;
static atomic_bool record_fixture_fail_close;
static atomic_size_t record_fixture_short_count;
static const void *record_fixture_spill_snapshot;
static atomic_bool record_fixture_hold_snapshot;
static atomic_bool record_fixture_snapshot_entered;
static atomic_bool record_fixture_release_snapshot;

static void record_fixture_after_load(const void *ptr) {
    if (ptr == record_fixture_spill_snapshot &&
        atomic_exchange(&record_fixture_hold_snapshot, false)) {
        atomic_store(&record_fixture_snapshot_entered, true);
        while (!atomic_load(&record_fixture_release_snapshot)) thrd_sleep_ms(1);
    }
}

size_t record_fixture_write(const void *data, size_t size, size_t count, FILE *file) {
    if (file == record_fixture_gated_output && atomic_exchange(&record_fixture_hold_write, false)) {
        atomic_store(&record_fixture_write_entered, true);
        while (!atomic_load(&record_fixture_release_write)) thrd_sleep_ms(1);
    }
    if (file == record_fixture_gated_output) {
        size_t short_count = atomic_exchange(&record_fixture_short_count, 0);
        if (short_count && short_count < count) count = short_count;
    }
    return fwrite(data, size, count, file);
}

int record_fixture_close(FILE *file) {
    int result = fclose(file);
    return atomic_exchange(&record_fixture_fail_close, false) ? EOF : result;
}

static char last_status[256];
void gui_app_set_status(gui_app_t *app, const char *message) {
    (void)app;
    snprintf(last_status, sizeof(last_status), "%s", message);
}
double GetTime(void) { return (double)get_time_us() / 1000000.0; }
bool gui_cxadc_is_running(void) { return false; }
bool gui_cxadc_direct_record_available(int channel, uint8_t bits, size_t *bytes) {
    (void)channel; (void)bits; (void)bytes; return false;
}

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #value, last_status); failures++; \
} } while (0)

static void remove_path(const char *path, bool directory) {
#ifdef _WIN32
    wchar_t wide[GUI_RECORD_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, GUI_RECORD_PATH_MAX)) return;
    if (directory) _wrmdir(wide); else _wremove(wide);
#else
    if (directory) rmdir(path); else remove(path);
#endif
}

static void check_flac_finalize(const char *directory) {
    char path[GUI_RECORD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/finalize_\xe4\xb8\xad\xe6\x96\x87_\xf0\x9f\x93\xbc.flac", directory);
    FILE *f = file_open_utf8(path, "wb");
    FLAC__StreamEncoder *encoder = FLAC__stream_encoder_new();
    FLAC__StreamMetadata *padding = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
    CHECK(f && encoder && padding);
    if (!f || !encoder || !padding) {
        if (f) fclose(f);
        if (encoder) FLAC__stream_encoder_delete(encoder);
        if (padding) FLAC__metadata_object_delete(padding);
        return;
    }
    padding->length = 1024;
    CHECK(FLAC__stream_encoder_set_channels(encoder, 1));
    CHECK(FLAC__stream_encoder_set_bits_per_sample(encoder, 16));
    CHECK(FLAC__stream_encoder_set_sample_rate(encoder, 20000));
    CHECK(FLAC__stream_encoder_set_metadata(encoder, &padding, 1));
    FLAC__StreamEncoderInitStatus init = FLAC__stream_encoder_init_FILE(encoder, f, NULL, NULL);
    CHECK(init == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    if (init == FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__int32 samples[8000] = {0};
        CHECK(FLAC__stream_encoder_process_interleaved(encoder, samples, 8000));
        CHECK(FLAC__stream_encoder_finish(encoder));  // Owns and closes f.
        gui_record_finalize_flac_metadata(NULL, path, "RF test", 8000, 20000);
        FLAC__StreamMetadata info;
        CHECK(FLAC__metadata_get_streaminfo(path, &info));
        CHECK(info.data.stream_info.total_samples == 8);
        CHECK(info.data.stream_info.sample_rate == 20000);
        FLAC__StreamMetadata *tags = NULL;
        CHECK(FLAC__metadata_get_tags(path, &tags));
        if (tags) {
            int total = FLAC__metadata_object_vorbiscomment_find_entry_from(tags, 0, "RF_TOTAL_SAMPLES");
            int rate = FLAC__metadata_object_vorbiscomment_find_entry_from(tags, 0, "RF_SAMPLE_RATE");
            CHECK(total >= 0 && rate >= 0);
            if (total >= 0) CHECK(strcmp((char *)tags->data.vorbis_comment.comments[total].entry,
                                         "RF_TOTAL_SAMPLES=8000") == 0);
            if (rate >= 0) CHECK(strcmp((char *)tags->data.vorbis_comment.comments[rate].entry,
                                        "RF_SAMPLE_RATE=20000000") == 0);
            FLAC__metadata_object_delete(tags);
        }
    } else {
        fclose(f);
    }
    FLAC__stream_encoder_delete(encoder);
    FLAC__metadata_object_delete(padding);
    remove_path(path, false);
}

typedef struct {
    const int16_t *expected;
    size_t count;
    size_t decoded;
} legacy_flac_check_t;

static FLAC__StreamDecoderWriteStatus check_legacy_flac_samples(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const samples[], void *user_data) {
    (void)decoder;
    legacy_flac_check_t *check = user_data;
    size_t count = frame->header.blocksize;
    if (count > check->count - check->decoded) {
        CHECK(false);
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    if (check->expected) {
        for (size_t i = 0; i < count; ++i)
            assert(samples[0][i] == check->expected[check->decoded + i]);
    }
    check->decoded += count;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void check_legacy_flac_error(const FLAC__StreamDecoder *decoder,
                                    FLAC__StreamDecoderErrorStatus status, void *user_data) {
    (void)decoder;
    (void)status;
    (void)user_data;
    CHECK(false);
}

static void check_legacy_writer(gui_app_t *app, const char *directory, bool flac,
                                uint8_t bits, bool resample, size_t ring_count, size_t spill_count) {
    char output_path[GUI_RECORD_PATH_MAX], spill_path[GUI_RECORD_PATH_MAX];
    snprintf(output_path, sizeof(output_path), "%s/legacy-tail.%s", directory, flac ? "flac" : "raw");
    snprintf(spill_path, sizeof(spill_path), "%s/legacy-spill.tmp", directory);

    const size_t count = ring_count + spill_count;
    const size_t output_count = resample ? count / 2 : count;
    int16_t *input = malloc(count * sizeof(*input));
    assert(input);
    for (size_t i = 0; i < count; ++i)
        input[i] = (int16_t)(((int)(i % 97) - 48) * 16);

    buffer_config_t configs[BUF_COUNT];
    for (int id = 0; id < BUF_COUNT; ++id)
        configs[id] = (buffer_config_t){.name = "legacy writer test", .size = 4096, .lazy_init = true};
    configs[BUF_RECORD_A].size = GUI_RECORD_WRITER_BLOCK_BYTES * 2;
    configs[BUF_RECORD_A].lazy_init = false;
    assert(bufmgr_init_custom(&app->buffers, configs) == 0);
    size_t ring_bytes = ring_count * sizeof(*input);
    void *ring = bufmgr_write_begin(&app->buffers, BUF_RECORD_A, ring_bytes, NULL);
    assert(ring);
    memcpy(ring, input, ring_bytes);
    bufmgr_write_end(&app->buffers, BUF_RECORD_A, ring_bytes);

    // Use an in-fixture spill file on all platforms, then exercise the real
    // append, read and recycle paths. The spill must follow the older ring.
    gui_record_spill_channel_t *spill = &s_record_spill[0];
    spill->fp = file_open_utf8(spill_path, "w+b");
    assert(spill->fp);
    atomic_store(&spill->opened, true);
    assert(gui_record_spill_enqueue(NULL, 0, input + ring_count,
                                    spill_count * sizeof(*input), 0, NULL, 0));

    FILE *file = file_open_utf8(output_path, "wb");
    assert(file);
    writer_ctx_t writer = {
        .bufmgr = &app->buffers, .buf_id = BUF_RECORD_A, .file = file, .app = app,
        .rf_bits = bits, .raw_bytes_per_sample = bits == 8 ? 1 : 2,
        .enable_resample = resample, .input_sample_rate_khz = 40000,
        .resample_rate_khz = 20000, .resample_quality = 3,
    };
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&s_record_writer_draining, true);
    atomic_store(&s_record_write_error[0], false);
    if (flac) {
        flac_writer_config_t config = flac_writer_default_config();
        config.sample_rate = resample ? 20000 : 40000;
        config.bits_per_sample = bits;
        config.num_threads = 1;
        writer.writer = flac_writer_create_stream(file, &config);
        assert(writer.writer);
        writer.flac_bits_per_sample = bits;
        flac_writer_thread(&writer);
        CHECK(flac_writer_get_samples_written(writer.writer) == output_count);
        CHECK(flac_writer_finish(writer.writer) == FLAC_WRITER_OK);
    } else {
        raw_writer_thread(&writer);
    }
    CHECK(fclose(file) == 0);
    CHECK(!atomic_load(&s_record_write_error[0]));
    CHECK(bufmgr_fill_level(&app->buffers, BUF_RECORD_A) == 0);
    CHECK(gui_record_spill_backlog_bytes(0) == 0);
    size_t written_bytes = flac ? count * sizeof(*input) : output_count * writer.raw_bytes_per_sample;
    CHECK(atomic_load(&app->recording_bytes) == written_bytes);
    CHECK(atomic_load(&app->recording_raw_a) == written_bytes);
    gui_record_spill_close_channel(0);
    bufmgr_cleanup(&app->buffers);

    file = file_open_read_utf8(output_path);
    assert(file);
    if (flac) {
        FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
        assert(decoder);
        legacy_flac_check_t check = {resample ? NULL : input, output_count, 0};
        assert(FLAC__stream_decoder_init_FILE(decoder, file, check_legacy_flac_samples,
                     NULL, check_legacy_flac_error, &check) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
        CHECK(FLAC__stream_decoder_process_until_end_of_stream(decoder));
        CHECK(FLAC__stream_decoder_finish(decoder));
        FLAC__stream_decoder_delete(decoder);
        CHECK(check.decoded == output_count);
    } else {
        uint64_t size = 0;
        CHECK(file_path_info_utf8(output_path, &size, NULL));
        CHECK(size == written_bytes);
        if (!resample) {
            if (bits == 8) {
                for (size_t i = 0; i < count; ++i)
                    assert(fgetc(file) == (uint8_t)(input[i] / 16));
            } else {
                int16_t samples[256];
                for (size_t i = 0; i < count;) {
                    size_t got = fread(samples, sizeof(*samples), 256, file);
                    assert(got && got <= count - i);
                    assert(memcmp(samples, input + i, got * sizeof(*samples)) == 0);
                    i += got;
                }
            }
            CHECK(fgetc(file) == EOF && !ferror(file));
        }
        CHECK(fclose(file) == 0);
    }
    free(input);
    remove_path(output_path, false);
    remove_path(spill_path, false);
}

static void check_overwrite_targets(gui_app_t *app, const char *directory) {
    char other_directory[GUI_RECORD_PATH_MAX];
    char path_a[GUI_RECORD_PATH_MAX], other_a[GUI_RECORD_PATH_MAX], path_b[GUI_RECORD_PATH_MAX];
    snprintf(other_directory, sizeof(other_directory), "%s/other", directory);
    snprintf(path_a, sizeof(path_a), "%s/confirmed-a.s16", directory);
    snprintf(other_a, sizeof(other_a), "%s/other/confirmed-a.s16", directory);
    snprintf(path_b, sizeof(path_b), "%s/confirmed-b.s16", directory);
    assert(file_mkdir_utf8(other_directory, 0700) == 0);
    app->settings.capture_a = app->settings.capture_b = true;
    app->settings.enable_audio_2ch_12 = false;
    strcpy(app->settings.output_filename_a, "confirmed-a.s16");
    strcpy(app->settings.output_filename_b, "confirmed-b.s16");
    const char *existing_paths[] = {path_a, other_a, path_b};
    for (size_t i = 0; i < 2; i++) {
        FILE *f = file_open_utf8(existing_paths[i], "wb");
        assert(f && fwrite("sentinel", 1, 8, f) == 8 && fclose(f) == 0);
    }

    gui_record_output_targets_t confirmed, current;
    CHECK(gui_record_check_output_paths(app, NULL, NULL, 0, &confirmed));
    CHECK(gui_record_match_output_targets(app, &confirmed, &current));
    snprintf(app->settings.output_path, sizeof(app->settings.output_path), "%s", other_directory);
    CHECK(!gui_record_match_output_targets(app, &confirmed, &current));
    snprintf(app->settings.output_path, sizeof(app->settings.output_path), "%s", directory);
    strcpy(app->settings.output_filename_b, "changed-b.s16");
    CHECK(!gui_record_match_output_targets(app, &confirmed, &current));
    strcpy(app->settings.output_filename_b, "confirmed-b.s16");
    app->settings.capture_b = false;
    CHECK(!gui_record_match_output_targets(app, &confirmed, &current));
    app->settings.capture_b = true;
    FILE *f = file_open_utf8(path_b, "wb");
    assert(f && fwrite("sentinel", 1, 8, f) == 8 && fclose(f) == 0);
    CHECK(!gui_record_match_output_targets(app, &confirmed, &current));

    for (size_t i = 0; i < 3; i++) {
        char contents[8];
        f = file_open_read_utf8(existing_paths[i]);
        assert(f && fread(contents, 1, 8, f) == 8 && fclose(f) == 0);
        CHECK(memcmp(contents, "sentinel", 8) == 0);
        remove_path(existing_paths[i], false);
    }
    remove_path(other_directory, true);
    puts("Overwrite confirmation rejects changed directory, channel, filename and newly existing file");
}

/* The oldest block occupies the entire ring while its first output write is
 * held. Force a spill, allow the writer to free the ring, and then publish a
 * newer block while the older spill is still pending. */
static void check_active_spill(gui_app_t *app, const char *directory, bool flac,
                               const int16_t *input, size_t count, int channel,
                               bool late_publication) {
    const size_t first = GUI_RECORD_WRITER_BLOCK_SAMPLES;
    const size_t middle = 65536;
    if (late_publication) assert(count >= 2 * first);
    else assert(count > first + middle && count < first * 2);
    buffer_id_t id = channel ? BUF_RECORD_B : BUF_RECORD_A;
    buffer_config_t configs[BUF_COUNT];
    for (int i = 0; i < BUF_COUNT; ++i)
        configs[i] = (buffer_config_t){.name = "active converted spill", .size = 4096, .lazy_init = true};
    configs[id].size = GUI_RECORD_WRITER_BLOCK_BYTES;
    configs[id].lazy_init = false;
    assert(bufmgr_init_custom(&app->buffers, configs) == 0);

    char path[GUI_RECORD_PATH_MAX], spill_path[GUI_RECORD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/legacy-active-spill-%c.%s", directory,
             channel ? 'B' : 'A', flac ? "flac" : "s16");
    snprintf(spill_path, sizeof(spill_path), "%s/legacy-active-spill.tmp", directory);
    s_record_spill[channel].fp = file_open_utf8(spill_path, "w+b");
    assert(s_record_spill[channel].fp);
    atomic_store(&s_record_spill[channel].opened, true);
    FILE *file = file_open_utf8(path, "wb");
    assert(file);
    writer_ctx_t writer = {
        .bufmgr = &app->buffers, .buf_id = id, .file = file, .app = app, .channel = channel,
        .rf_bits = 16, .raw_bytes_per_sample = 2, .flac_bits_per_sample = 12,
    };
    if (flac) {
        flac_writer_config_t config = flac_writer_default_config();
        config.bits_per_sample = 12;
        config.num_threads = 1;
        config.sink_write = record_fixture_write;
        writer.writer = flac_writer_create_stream(file, &config);
        assert(writer.writer);
    }
    atomic_store(&s_record_write_error[channel], false);
    atomic_store(&s_record_writer_draining, false);
    atomic_store(&record_fixture_hold_write, !late_publication);
    atomic_store(&record_fixture_write_entered, false);
    atomic_store(&record_fixture_release_write, false);
    record_fixture_gated_output = file;
    if (late_publication) {
        record_fixture_spill_snapshot = &s_record_spill[channel].read_offset;
        atomic_store(&record_fixture_hold_snapshot, true);
        atomic_store(&record_fixture_snapshot_entered, false);
        atomic_store(&record_fixture_release_snapshot, false);
    } else {
        assert(gui_record_push_converted(app, channel, input, first * sizeof(*input), 0, NULL, 0));
        assert(bufmgr_fill_level(&app->buffers, id) == GUI_RECORD_WRITER_BLOCK_BYTES);
    }
    thrd_t thread;
    assert(thrd_create_with_priority(&thread, flac ? flac_writer_thread : raw_writer_thread,
                                     &writer, THRD_PRIORITY_NORMAL) == thrd_success);
    uint64_t deadline = get_time_ms() + 5000;
    if (late_publication) {
        while (!atomic_load(&record_fixture_snapshot_entered)) {
            assert(get_time_ms() < deadline);
            thrd_sleep_ms(1);
        }
        // Writer has sampled zero backlog. Publish an entire older ring and
        // a full newer spill block before it decides whether to read spill.
        assert(gui_record_push_converted(app, channel, input, first * sizeof(*input), 0, NULL, 0));
        assert(gui_record_push_converted(app, channel, input + first,
                                         (count - first) * sizeof(*input), 1, NULL, 0));
        CHECK(gui_record_queued_bytes(&app->buffers) == count * sizeof(*input));
        atomic_store(&record_fixture_release_snapshot, true);
    } else {
        while (!atomic_load(&record_fixture_write_entered)) {
            assert(get_time_ms() < deadline);
            thrd_sleep_ms(1);
        }
        assert(gui_record_push_converted(app, channel, input + first, middle * sizeof(*input), 1, NULL, 0));
        assert(gui_record_spill_backlog_bytes(channel) == middle * sizeof(*input));
        atomic_store(&record_fixture_release_write, true);
        while (bufmgr_fill_level(&app->buffers, id) != 0) {
            assert(get_time_ms() < deadline);
            thrd_sleep_ms(1);
        }
        size_t newest = count - first - middle;
        assert(gui_record_push_converted(app, channel, input + first + middle,
                                         newest * sizeof(*input), 2, NULL, 0));
        assert(bufmgr_fill_level(&app->buffers, id) == 0);
        assert(gui_record_spill_backlog_bytes(channel) == (middle + newest) * sizeof(*input));
    }
    atomic_store(&s_record_writer_draining, true);
    thrd_join(thread, NULL);
    record_fixture_spill_snapshot = NULL;
    record_fixture_gated_output = NULL;
    CHECK(gui_record_spill_backlog_bytes(channel) == 0);
    CHECK(atomic_load(&app->buffers.stats[id].write_drops) == 0);
    CHECK(!atomic_load(&s_record_write_error[channel]));
    if (flac) {
        CHECK(flac_writer_get_samples_written(writer.writer) == count);
        CHECK(flac_writer_finish(writer.writer) == FLAC_WRITER_OK);
    }
    gui_record_close_output(&file, channel);
    CHECK(!file && !atomic_load(&s_record_write_error[channel]));
    gui_record_spill_close_channel(channel);
    bufmgr_cleanup(&app->buffers);

    file = file_open_read_utf8(path);
    assert(file);
    if (flac) {
        FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
        assert(decoder);
        legacy_flac_check_t check = {input, count, 0};
        assert(FLAC__stream_decoder_init_FILE(decoder, file, check_legacy_flac_samples,
                     NULL, check_legacy_flac_error, &check) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
        CHECK(FLAC__stream_decoder_process_until_end_of_stream(decoder));
        CHECK(FLAC__stream_decoder_finish(decoder));
        FLAC__stream_decoder_delete(decoder);
        CHECK(check.decoded == count);
    } else {
        int16_t readback[256];
        for (size_t i = 0; i < count;) {
            size_t got = fread(readback, sizeof(*readback), 256, file);
            assert(got && got <= count - i);
            assert(memcmp(readback, input + i, got * sizeof(*readback)) == 0);
            i += got;
        }
        CHECK(fgetc(file) == EOF && !ferror(file));
        CHECK(fclose(file) == 0);
    }
    printf("Active %s spill: %zu samples in exact order (%s)\n",
           flac ? "FLAC12" : "RAW16", count,
           late_publication ? "published after zero snapshot" : "older ring and two spill pushes");
    remove_path(path, false);
    remove_path(spill_path, false);
}

static void check_raw_short_write_retry(const char *directory) {
    char path[GUI_RECORD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/raw-short-write.s16", directory);
    FILE *file = file_open_utf8(path, "wb");
    assert(file);
    int16_t samples[] = {-160, 432, -250, 509};
    uint8_t converted[sizeof(samples)];
    writer_ctx_t writer = {.file = file, .rf_bits = 16, .raw_bytes_per_sample = 2};
    atomic_store(&s_record_writer_draining, true);
    atomic_store(&s_record_write_error[0], false);
    record_fixture_gated_output = file;
    atomic_store(&record_fixture_short_count, 3);
    assert(gui_record_write_raw_samples(&writer, samples, 4, converted));
    record_fixture_gated_output = NULL;
    gui_record_close_output(&file, 0);
    CHECK(!atomic_load(&s_record_write_error[0]));
    file = file_open_read_utf8(path);
    assert(file);
    int16_t readback[4];
    CHECK(fread(readback, sizeof(*readback), 4, file) == 4);
    CHECK(memcmp(readback, samples, sizeof(samples)) == 0);
    CHECK(fgetc(file) == EOF && !ferror(file));
    CHECK(fclose(file) == 0);
    remove_path(path, false);
}

static void check_failed_output_close(const char *directory) {
    char path[GUI_RECORD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/failed-close.s16", directory);
    FILE *file = file_open_utf8(path, "wb");
    assert(file);
    assert(fwrite("tail", 1, 4, file) == 4);
    atomic_store(&s_record_write_error[1], false);
    atomic_store(&record_fixture_fail_close, true);
    gui_record_close_output(&file, 1);
    CHECK(file == NULL && atomic_load(&s_record_write_error[1]));
    atomic_store(&s_record_write_error[1], false);
    remove_path(path, false);
}

static void check_unread_spill_is_failure(gui_app_t *app, const char *directory) {
    buffer_config_t configs[BUF_COUNT];
    for (int i = 0; i < BUF_COUNT; ++i)
        configs[i] = (buffer_config_t){.name = "unread spill", .size = 4096, .lazy_init = true};
    configs[BUF_RECORD_A].size = configs[BUF_RECORD_B].size = GUI_RECORD_WRITER_BLOCK_BYTES;
    configs[BUF_RECORD_A].lazy_init = configs[BUF_RECORD_B].lazy_init = false;
    assert(bufmgr_init_custom(&app->buffers, configs) == 0);
    atomic_store(&s_record_writer_draining, true);

    char path[GUI_RECORD_PATH_MAX], spill_path[GUI_RECORD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/unread-spill.s16", directory);
    snprintf(spill_path, sizeof(spill_path), "%s/unread-spill.tmp", directory);
    for (int channel = 0; channel < 2; ++channel) {
        FILE *file = file_open_utf8(path, "wb");
        assert(file);
        s_record_spill[channel].fp = file_open_utf8(spill_path, "w+b");
        assert(s_record_spill[channel].fp);
        // The spill cursor records a committed block, but the file is truncated.
        atomic_store(&s_record_spill[channel].opened, true);
        atomic_store(&s_record_spill[channel].write_offset, 34);
        atomic_store(&s_record_write_error[channel], false);
        if (channel == 0) {
            writer_ctx_t writer = {
                .file = file, .bufmgr = &app->buffers, .buf_id = BUF_RECORD_A,
                .app = app, .channel = channel, .rf_bits = 16, .raw_bytes_per_sample = 2,
            };
            raw_writer_thread(&writer);
        } else {
            gui_record_direct_ctx_t writer = {
                .file = file, .bufmgr = &app->buffers, .buf_id = BUF_RECORD_B,
                .channel = channel, .writer_draining = &s_record_writer_draining,
                .cb = {.spill_read = gui_record_direct_spill_read_cb,
                       .spill_backlog = gui_record_direct_spill_backlog_cb},
            };
            gui_record_direct_writer_thread(&writer);
        }
        CHECK(gui_record_spill_backlog_bytes(channel) == 34);
        gui_record_check_writer_drained(app, channel);
        CHECK(atomic_load(&s_record_write_error[channel]));
        CHECK(fclose(file) == 0);
        gui_record_spill_close_channel(channel);
        atomic_store(&s_record_write_error[channel], false);
    }
    bufmgr_cleanup(&app->buffers);
    remove_path(path, false);
    remove_path(spill_path, false);
}

static void check_exclusive_output_preserves_existing(const char *directory) {
    char path[GUI_RECORD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/new-output.s16", directory);
    FILE *file = file_open_utf8(path, "wbx");
    assert(file);
    CHECK(fwrite("sentinel", 1, 8, file) == 8);
    CHECK(fclose(file) == 0);
    errno = 0;
    FILE *collision = file_open_utf8(path, "wbx");
    CHECK(collision == NULL && errno == EEXIST);
    if (collision) fclose(collision);
    file = file_open_read_utf8(path);
    assert(file);
    char data[8];
    CHECK(fread(data, 1, sizeof(data), file) == sizeof(data));
    CHECK(memcmp(data, "sentinel", sizeof(data)) == 0);
    CHECK(fgetc(file) == EOF && !ferror(file));
    CHECK(fclose(file) == 0);
    remove_path(path, false);
}


#if LIBSOXR_ENABLED
static bool record_fixture_fail_soxr;
extern soxr_t soxr_create(double, double, unsigned, soxr_error_t *,
    soxr_io_spec_t const *, soxr_quality_spec_t const *, soxr_runtime_spec_t const *);

soxr_t record_fixture_soxr_create(double input_rate, double output_rate, unsigned channels,
    soxr_error_t *error, soxr_io_spec_t const *io, soxr_quality_spec_t const *quality,
    soxr_runtime_spec_t const *runtime) {
    if (!record_fixture_fail_soxr)
        return soxr_create(input_rate, output_rate, channels, error, io, quality, runtime);
    *error = "controlled soxr_create failure";
    return NULL;
}

static bool record_fixture_count_samples(writer_ctx_t *ctx, const int16_t *input,
                                         size_t count, void *scratch) {
    (void)ctx;
    assert(input);
    *(size_t *)scratch += count;
    return true;
}

static void check_resampler_failure(void) {
    const int16_t input[12] = {0};
    int16_t output[12];
    writer_ctx_t writer = {.enable_resample = true, .input_sample_rate_khz = 40000,
                           .resample_rate_khz = 20000};
    size_t emitted = 0;
    record_fixture_fail_soxr = true;
    CHECK(!gui_record_process_samples(&writer, input, 12, output, record_fixture_count_samples, &emitted));
    CHECK(emitted == 0 && writer.soxr == NULL);
    writer.resample_rate_khz = 40000;
    CHECK(gui_record_process_samples(&writer, input, 12, output, record_fixture_count_samples, &emitted));
    CHECK(emitted == 12 && writer.soxr == NULL);
    record_fixture_fail_soxr = false;
}
#endif

static void check_failed_spill_enqueue(gui_app_t *app, const char *directory) {
    buffer_config_t configs[BUF_COUNT];
    for (int i = 0; i < BUF_COUNT; ++i)
        configs[i] = (buffer_config_t){.name = "failed spill enqueue", .size = 4096, .lazy_init = true};
    configs[BUF_RECORD_A].size = configs[BUF_RECORD_B].size = GUI_RECORD_WRITER_BLOCK_BYTES;
    configs[BUF_RECORD_A].lazy_init = configs[BUF_RECORD_B].lazy_init = false;
    assert(bufmgr_init_custom(&app->buffers, configs) == 0);
    atomic_store(&s_record_writer_draining, true);

    int16_t samples[17] = {0};
    char path[GUI_RECORD_PATH_MAX], spill_path[GUI_RECORD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/failed-spill.raw", directory);
    snprintf(spill_path, sizeof(spill_path), "%s/failed-spill.tmp", directory);
    for (int channel = 0; channel < 2; ++channel) {
        buffer_id_t id = channel ? BUF_RECORD_B : BUF_RECORD_A;
        FILE *file = file_open_utf8(path, "wb");
        assert(file);
        s_record_spill[channel].fp = file_open_utf8(spill_path, "w+b");
        assert(s_record_spill[channel].fp);
        atomic_store(&s_record_spill[channel].opened, true);
        atomic_store(&s_record_producer_lost_data, false);
        assert(gui_record_spill_enqueue(app, channel, samples, sizeof(samples), 1, NULL, 0));

        record_fixture_gated_output = s_record_spill[channel].fp;
        atomic_store(&record_fixture_short_count, 1);
        if (channel == 0) {
            CHECK(!gui_record_push_converted(app, channel, samples, sizeof(samples), 2, NULL, 0));
            writer_ctx_t writer = {.file = file, .bufmgr = &app->buffers, .buf_id = id, .app = app,
                                   .channel = channel, .rf_bits = 16, .raw_bytes_per_sample = 2};
            raw_writer_thread(&writer);
        } else {
            gui_record_direct_ctx_t writer = {
                .file = file, .bufmgr = &app->buffers, .buf_id = id,
                .channel = channel, .writer_draining = &s_record_writer_draining,
                .cb = {.user = app, .spill_enqueue = gui_record_direct_spill_enqueue_cb,
                       .spill_read = gui_record_direct_spill_read_cb,
                       .spill_backlog = gui_record_direct_spill_backlog_cb},
            };
            gui_record_direct_tap_enable(&writer);
            CHECK(gui_record_direct_push(&writer, (const uint8_t *)samples, sizeof(samples), 2) ==
                  GUI_RECORD_DIRECT_PUSH_DROPPED);
            gui_record_direct_tap_disable(&writer);
            gui_record_direct_writer_thread(&writer);
        }
        record_fixture_gated_output = NULL;
        gui_record_check_writer_drained(app, channel);
        CHECK(atomic_load(&s_record_producer_lost_data));
        CHECK(!atomic_load(&app->buffers.stats[id].write_drops));
        CHECK(!gui_record_spill_backlog_bytes(channel) && ftell(file) == sizeof(samples));
        CHECK(fclose(file) == 0);
        gui_record_spill_close_channel(channel);
    }
    atomic_store(&s_record_producer_lost_data, false);
    bufmgr_cleanup(&app->buffers);
    remove_path(path, false);
    remove_path(spill_path, false);
}

static void check_capture_log_collision(gui_app_t *app, const char *directory) {
    const char *date = "2026.11.09_00.00.00";
    char old_path[GUI_RECORD_PATH_MAX];
    snprintf(app->settings.output_path, sizeof(app->settings.output_path), "%s", directory);
    snprintf(old_path, sizeof(old_path), "%s/fixture_%s_misrc_capture.log", directory, date);
    FILE *old = file_open_utf8(old_path, "wb");
    assert(old);
    CHECK(fwrite("first", 1, 5, old) == 5);
    CHECK(fclose(old) == 0);

    gui_record_output_targets_t targets = {0};
    snprintf(targets.paths[GUI_RECORD_RF_OUTPUT_COUNT], GUI_RECORD_PATH_MAX,
             "%s/fixture_%s_misrc_capture-1.log", directory, date);
    for (unsigned i = 2; i <= 3; ++i) {
        FILE *log = gui_record_create_session_log(app, &targets, "fixture", date);
        assert(log);
        char suffix[32];
        snprintf(suffix, sizeof(suffix), "_misrc_capture-%u.log", i);
        CHECK(strstr(s_capture_log_path, suffix) != NULL);
        CHECK(fclose(log) == 0);
    }
    CHECK(!file_path_info_utf8(targets.paths[GUI_RECORD_RF_OUTPUT_COUNT], NULL, NULL));
    old = file_open_read_utf8(old_path);
    assert(old);
    char saved[5];
    CHECK(fread(saved, 1, sizeof(saved), old) == sizeof(saved) && memcmp(saved, "first", 5) == 0);
    CHECK(fclose(old) == 0);
    remove_path(old_path, false);
    for (unsigned i = 2; i <= 3; ++i) {
        snprintf(old_path, sizeof(old_path), "%s/fixture_%s_misrc_capture-%u.log", directory, date, i);
        remove_path(old_path, false);
    }
}

static int record_fixture_move_path(const char *from, const char *to) {
#if defined(_WIN32)
    wchar_t source[MLS_PATH_MAX], destination[MLS_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, from, -1, source, MLS_PATH_MAX) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, to, -1, destination, MLS_PATH_MAX))
        return -1;
    return MoveFileExW(source, destination, 0) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

#if defined(_WIN32)
static bool record_fixture_fail_stage_duplicate;
static const char *record_fixture_first_final;
static const char *record_fixture_attempted_path;
static const char *record_fixture_collision_path;
static bool record_fixture_first_final_locked;
static unsigned record_fixture_stage_renames;

static BOOL WINAPI record_fixture_duplicate_stage(HANDLE process, HANDLE source, HANDLE target,
    LPHANDLE duplicate, DWORD access, BOOL inherit, DWORD options)
{
    if (record_fixture_fail_stage_duplicate) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return DuplicateHandle(process, source, target, duplicate, access, inherit, options);
}

static BOOL WINAPI record_fixture_set_stage_info(HANDLE stage, FILE_INFO_BY_HANDLE_CLASS info_class,
    LPVOID data, DWORD bytes)
{
    if (info_class == FileRenameInfo && record_fixture_first_final &&
        ++record_fixture_stage_renames == 2) {
        record_fixture_first_final_locked = file_path_info_utf8(record_fixture_first_final, NULL, NULL) &&
            record_fixture_move_path(record_fixture_first_final, record_fixture_attempted_path) != 0;
        if (record_fixture_collision_path) {
            FILE *other = file_open_utf8(record_fixture_collision_path, "wbx");
            assert(other && fwrite("existing", 1, 8, other) == 8 && fclose(other) == 0);
        }
    }
    return SetFileInformationByHandle(stage, info_class, data, bytes);
}
#else
static const char *record_fixture_group_destination;
static bool record_fixture_old_flat_race_attempted;
static int record_fixture_payload_swap_fd = -1;

static void record_fixture_replace_payload(int work_fd) {
    assert(renameat(work_fd, "payload", work_fd, "saved-payload") == 0);
    assert(mkdirat(work_fd, "payload", 0700) == 0);
    int foreign = openat(work_fd, "payload/group-a.bin", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(foreign >= 0 && write(foreign, "foreign", 7) == 7 && close(foreign) == 0);
}

static int record_fixture_stat_at(int fd, const char *name, struct stat *info, int flags)
{
    int result = fstatat(fd, name, info, flags);
    if (result == 0 && fd == record_fixture_payload_swap_fd && !strcmp(name, "payload")) {
        record_fixture_payload_swap_fd = -1;
        record_fixture_replace_payload(fd);
    }
    if (result == 0 && record_fixture_group_destination && !strcmp(name, "group-b.bin")) {
        // At B's identity check the old implementation exposed A's flat
        // final name. Recreate that interleaving against the new receiver.
        assert(!file_path_info_utf8(record_fixture_group_destination, NULL, NULL));
        FILE *foreign = file_open_utf8(record_fixture_group_destination, "wbx");
        assert(foreign && fwrite("foreign", 1, 7, foreign) == 7 && fclose(foreign) == 0);
        record_fixture_old_flat_race_attempted = true;
        record_fixture_group_destination = NULL;
    }
    return result;
}
#endif

static void record_fixture_stage(mls_receiver_t *receiver, unsigned id,
                                 const char *name, const char *payload) {
    uint32_t length = (uint32_t)strlen(name);
    mls_frame_t frame = {.type = MLS_OPEN, .file_id = id, .length = length,
                         .crc = mls_crc32(name, length)};
    assert(mls_receiver_apply(receiver, &frame, name) == 0);
    length = (uint32_t)strlen(payload);
    frame = (mls_frame_t){.type = MLS_WRITE, .file_id = id, .length = length,
                          .crc = mls_crc32(payload, length)};
    assert(mls_receiver_apply(receiver, &frame, payload) == 0);
    frame = (mls_frame_t){.type = MLS_CLOSE, .file_id = id};
    assert(mls_receiver_apply(receiver, &frame, NULL) == 0);
}

static void check_receiver_no_replace(const char *directory) {
    mls_receiver_t *receiver = calloc(1, sizeof(*receiver));
    assert(receiver && mls_receiver_init(receiver, directory, false) == 0);
    const char name[] = "receiver.bin";
    const char payload[] = "new bytes";
    mls_frame_t frame = {.type = MLS_OPEN, .length = sizeof(name) - 1,
                         .crc = mls_crc32(name, sizeof(name) - 1)};
    CHECK(mls_receiver_apply(receiver, &frame, name) == 0);
    frame = (mls_frame_t){.type = MLS_WRITE, .length = sizeof(payload) - 1,
                         .crc = mls_crc32(payload, sizeof(payload) - 1)};
    CHECK(mls_receiver_apply(receiver, &frame, payload) == 0);
    frame = (mls_frame_t){.type = MLS_CLOSE};
    CHECK(mls_receiver_apply(receiver, &frame, NULL) == 0);

    mls_file_t *staged = &receiver->files[0];
    FILE *final = file_open_utf8(staged->final_path, "wb");
    assert(final);
    CHECK(fwrite("existing", 1, 8, final) == 8 && fclose(final) == 0);
    // Exercise the primitive after preflight, as if another process won the race.
#if defined(_WIN32)
    CHECK(mls_publish_stage(receiver, staged) != 0);
    CHECK(file_path_info_utf8(staged->incomplete_path, NULL, NULL));
#else
    CHECK(mls_move_noreplace(receiver->payload_fd, staged->name,
                             receiver->directory_fd, staged->name, false) != 0);
    CHECK(faccessat(receiver->payload_fd, staged->name, F_OK, 0) == 0);
#endif
    final = file_open_read_utf8(staged->final_path);
    assert(final);
    char saved[8];
    CHECK(fread(saved, 1, sizeof(saved), final) == sizeof(saved) && memcmp(saved, "existing", 8) == 0);
    CHECK(fclose(final) == 0);

    remove_path(staged->final_path, false);
    frame = (mls_frame_t){.type = MLS_COMMIT};
    CHECK(mls_receiver_apply(receiver, &frame, NULL) == 0);
    mls_receiver_finish_commit(receiver);
#if defined(_WIN32)
    CHECK(!file_path_info_utf8(staged->incomplete_path, NULL, NULL));
#else
    CHECK(!file_path_info_utf8(receiver->recovery_path, NULL, NULL));
#endif
    final = file_open_read_utf8(staged->final_path);
    assert(final);
    char bytes[sizeof(payload) - 1];
    CHECK(fread(bytes, 1, sizeof(bytes), final) == sizeof(bytes));
    CHECK(memcmp(bytes, payload, sizeof(bytes)) == 0 && fclose(final) == 0);
    remove_path(staged->final_path, false);
    free(receiver);
}

#if !defined(_WIN32)
static void check_receiver_replaced_payload(const char *directory, bool after_lookup) {
    mls_receiver_t *receiver = calloc(1, sizeof(*receiver));
    assert(receiver && mls_receiver_init(receiver, directory, false) == 0);
    record_fixture_stage(receiver, 0, "group-a.bin", "original A");
    record_fixture_stage(receiver, 1, "group-b.bin", "original B");
    int work_fd = receiver->work_fd;
    if (after_lookup) record_fixture_payload_swap_fd = work_fd;
    else record_fixture_replace_payload(work_fd);

    const mls_frame_t frame = {.type = MLS_COMMIT};
    CHECK(mls_receiver_apply(receiver, &frame, NULL) != 0);
    CHECK(record_fixture_payload_swap_fd == -1);
    CHECK(receiver->had_error && !receiver->published_path[0]);
    struct stat info;
    CHECK(fstatat(receiver->directory_fd, receiver->group_name, &info, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);

    const char *paths[] = {"payload/group-a.bin", "saved-payload/group-a.bin", "saved-payload/group-b.bin"};
    const char *contents[] = {"foreign", "original A", "original B"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int fd = openat(work_fd, paths[i], O_RDONLY);
        char bytes[16];
        size_t length = strlen(contents[i]);
        assert(fd >= 0);
        CHECK(read(fd, bytes, sizeof(bytes)) == (ssize_t)length && memcmp(bytes, contents[i], length) == 0);
        CHECK(close(fd) == 0);
        CHECK(unlinkat(work_fd, paths[i], 0) == 0);
    }
    CHECK(unlinkat(work_fd, "payload", AT_REMOVEDIR) == 0);
    CHECK(unlinkat(work_fd, "saved-payload", AT_REMOVEDIR) == 0);
    mls_receiver_abort(receiver);
    remove_path(receiver->recovery_path, true);
    free(receiver);
}

static void check_receiver_replaced_stage(const char *directory) {
    mls_receiver_t *receiver = calloc(1, sizeof(*receiver));
    assert(receiver && mls_receiver_init(receiver, directory, false) == 0);
    record_fixture_stage(receiver, 0, "replaced.bin", "original");

    char payload[MLS_PATH_MAX], stage[MLS_PATH_MAX], saved[MLS_PATH_MAX];
    assert(mls_join_path(payload, receiver->recovery_path, "payload"));
    assert(mls_join_path(stage, payload, receiver->files[0].name));
    assert(mls_join_path(saved, directory, "replaced.saved"));
    assert(record_fixture_move_path(stage, saved) == 0);
    FILE *impostor = file_open_utf8(stage, "wbx");
    assert(impostor && fwrite("impostor", 1, 8, impostor) == 8 && fclose(impostor) == 0);
    mls_frame_t frame = {.type = MLS_COMMIT};
    CHECK(mls_receiver_apply(receiver, &frame, NULL) != 0);
    CHECK(!file_path_info_utf8(receiver->files[0].final_path, NULL, NULL));

    FILE *replacement = file_open_read_utf8(stage);
    FILE *original = file_open_read_utf8(saved);
    assert(replacement && original);
    char bytes[8];
    CHECK(fread(bytes, 1, sizeof(bytes), replacement) == sizeof(bytes) &&
          memcmp(bytes, "impostor", sizeof(bytes)) == 0);
    CHECK(fread(bytes, 1, sizeof(bytes), original) == sizeof(bytes) &&
          memcmp(bytes, "original", sizeof(bytes)) == 0);
    CHECK(fclose(replacement) == 0);
    CHECK(fclose(original) == 0);
    mls_receiver_abort(receiver);
    CHECK(file_path_info_utf8(stage, NULL, NULL));
    remove_path(stage, false);
    remove_path(payload, true);
    remove_path(receiver->recovery_path, true);
    remove_path(saved, false);
    free(receiver);
}

static void check_receiver_posix_group(const char *directory) {
    char selected[MLS_PATH_MAX], alias[MLS_PATH_MAX], other[MLS_PATH_MAX];
    assert(mls_join_path(selected, directory, "with\\backslash"));
    assert(mls_join_path(alias, directory, "selected"));
    assert(mls_join_path(other, directory, "other"));
    assert(file_mkdir_utf8(selected, 0700) == 0 && file_mkdir_utf8(other, 0700) == 0);
    char *absolute = realpath(selected, NULL);
    assert(absolute && symlink(absolute, alias) == 0);
    free(absolute);

    mls_receiver_t *receiver = calloc(1, sizeof(*receiver));
    assert(receiver && mls_receiver_init(receiver, alias, false) == 0);
    record_fixture_stage(receiver, 0, "group-a.bin", "original A");
    absolute = realpath(other, NULL);
    assert(absolute && unlink(alias) == 0 && symlink(absolute, alias) == 0);
    free(absolute);
    record_fixture_stage(receiver, 1, "group-b.bin", "original B");

    char flat[MLS_PATH_MAX];
    assert(mls_join_path(flat, selected, "group-a.bin"));
    record_fixture_group_destination = flat;
    record_fixture_old_flat_race_attempted = false;
    mls_frame_t frame = {.type = MLS_COMMIT};
    CHECK(mls_receiver_apply(receiver, &frame, NULL) == 0);
    CHECK(record_fixture_old_flat_race_attempted);
    record_fixture_group_destination = NULL;
    mls_receiver_finish_commit(receiver);

    char expected[MLS_PATH_MAX], actual[MLS_PATH_MAX], redirected[MLS_PATH_MAX];
    absolute = realpath(selected, NULL);
    assert(absolute && mls_join_path(expected, absolute, receiver->group_name));
    free(absolute);
    CHECK(strcmp(expected, receiver->published_path) == 0);
    assert(mls_join_path(redirected, other, receiver->group_name));
    CHECK(!file_path_info_utf8(redirected, NULL, NULL));
    for (unsigned id = 0; id < 2; ++id) {
        assert(mls_join_path(actual, expected, receiver->files[id].name));
        FILE *stream = file_open_read_utf8(actual);
        char bytes[10];
        assert(stream);
        CHECK(fread(bytes, 1, sizeof(bytes), stream) == sizeof(bytes) &&
              memcmp(bytes, id ? "original B" : "original A", sizeof(bytes)) == 0);
        CHECK(fclose(stream) == 0);
        remove_path(actual, false);
    }
    CHECK(file_path_info_utf8(flat, NULL, NULL));
    remove_path(flat, false);
    remove_path(expected, true);
    CHECK(unlink(alias) == 0);
    remove_path(selected, true);
    remove_path(other, true);
    free(receiver);
}

#else
static void check_receiver_stage_setup_retry(const char *directory) {
    mls_receiver_t *receiver = calloc(1, sizeof(*receiver));
    assert(receiver && mls_receiver_init(receiver, directory, false) == 0);
    const char name[] = "stage-retry.bin";
    mls_frame_t frame = {.type = MLS_OPEN, .length = sizeof(name) - 1,
                         .crc = mls_crc32(name, sizeof(name) - 1)};
    record_fixture_fail_stage_duplicate = true;
    CHECK(mls_receiver_apply(receiver, &frame, name) != 0);
    record_fixture_fail_stage_duplicate = false;
    CHECK(!file_path_info_utf8(receiver->files[0].incomplete_path, NULL, NULL));
    mls_receiver_abort(receiver);

    assert(mls_receiver_init(receiver, directory, false) == 0);
    CHECK(mls_receiver_apply(receiver, &frame, name) == 0);
    frame = (mls_frame_t){.type = MLS_CLOSE};
    CHECK(mls_receiver_apply(receiver, &frame, NULL) == 0);
    frame.type = MLS_COMMIT;
    CHECK(mls_receiver_apply(receiver, &frame, NULL) == 0);
    mls_receiver_finish_commit(receiver);
    CHECK(file_path_info_utf8(receiver->files[0].final_path, NULL, NULL));
    remove_path(receiver->files[0].final_path, false);
    free(receiver);
}

static void check_receiver_stage_pinning(const char *directory, bool collision) {
    mls_receiver_t *receiver = calloc(1, sizeof(*receiver));
    assert(receiver && mls_receiver_init(receiver, directory, false) == 0);
    const char *payloads[] = {"original A", "original B"};
    record_fixture_stage(receiver, 0, "group-a.bin", payloads[0]);
    record_fixture_stage(receiver, 1, "group-b.bin", payloads[1]);
    mls_file_t *first = &receiver->files[0];
    mls_file_t *second = &receiver->files[1];
    char attempted[GUI_RECORD_PATH_MAX];
    snprintf(attempted, sizeof(attempted), "%s/group-moved.bin", directory);
    CHECK(record_fixture_move_path(first->incomplete_path, attempted) != 0);
    record_fixture_first_final = first->final_path;
    record_fixture_attempted_path = attempted;
    record_fixture_collision_path = collision ? second->final_path : NULL;
    record_fixture_first_final_locked = false;
    record_fixture_stage_renames = 0;
    mls_frame_t frame = {.type = MLS_COMMIT};
    CHECK((mls_receiver_apply(receiver, &frame, NULL) != 0) == collision);
    CHECK(record_fixture_stage_renames == (collision ? 3 : 2));
    CHECK(record_fixture_first_final_locked);
    record_fixture_first_final = NULL;
    record_fixture_attempted_path = NULL;
    record_fixture_collision_path = NULL;

    if (collision) {
        mls_receiver_abort(receiver);
        CHECK(!file_path_info_utf8(first->final_path, NULL, NULL));
        CHECK(file_path_info_utf8(second->incomplete_path, NULL, NULL));
        FILE *other = file_open_read_utf8(second->final_path);
        char bytes[8];
        assert(other);
        CHECK(fread(bytes, 1, sizeof(bytes), other) == sizeof(bytes) &&
              memcmp(bytes, "existing", sizeof(bytes)) == 0);
        CHECK(fclose(other) == 0);
    } else {
        // The caller has not acknowledged COMMIT yet.
        CHECK(record_fixture_move_path(first->final_path, attempted) != 0);
        mls_receiver_finish_commit(receiver);
        CHECK(record_fixture_move_path(first->final_path, attempted) == 0);
    }

    FILE *original = file_open_read_utf8(collision ? first->incomplete_path : attempted);
    assert(original);
    char bytes[10];
    CHECK(fread(bytes, 1, sizeof(bytes), original) == sizeof(bytes) &&
          memcmp(bytes, payloads[0], sizeof(bytes)) == 0);
    CHECK(fclose(original) == 0);
    remove_path(collision ? first->incomplete_path : attempted, false);
    if (collision) remove_path(second->incomplete_path, false);
    remove_path(second->final_path, false);
    free(receiver);
}
#endif

bool gui_net_client_wav_ready(const gui_app_t *app) { (void)app; return true; }

static void check_physical_audio_source_loss(gui_app_t *app) {
    gui_record_output_targets_t targets = {0};
    app->settings.net_mode = GUI_NET_MODE_LOCAL;
    atomic_store(&s_record_producer_lost_data, false);
    gui_record_start_source_tracking(app, &targets, false, false);
    gui_record_mark_audio_source_loss();
    gui_record_check_source_tracking(app);
    CHECK(!atomic_load(&s_record_producer_lost_data));

    snprintf(targets.paths[GUI_RECORD_RF_OUTPUT_COUNT], GUI_RECORD_PATH_MAX, "audio.wav");
    gui_record_start_source_tracking(app, &targets, false, false);
    gui_record_mark_audio_source_loss();
    gui_record_check_source_tracking(app);
    CHECK(atomic_load(&s_record_producer_lost_data));
    atomic_store(&s_record_producer_lost_data, false);
    gui_record_start_source_tracking(app, &targets, false, false);
    gui_record_check_source_tracking(app);
    CHECK(!atomic_load(&s_record_producer_lost_data));
}

int main(void) {
    gui_app_t *app = calloc(1, sizeof(*app));
    if (!app) return 1;
    app->settings.auto_names_enabled = true;
    app->settings.capture_a = app->settings.capture_b = true;
    app->settings.use_flac = true;
    app->settings.rf_bits_a = app->settings.rf_bits_b = 16;
    strcpy(app->settings.output_base_name, "\xe4\xb8\xad\xe6\x96\x87-\xe3\x80\x8c\xe3\x83\xbb\xe3\x80\x8d");
    strcpy(app->settings.rf_channel_tags[0], "\xe6\x97\xa5\xe6\x9c\xac");
    CHECK(gui_record_apply_auto_names(app));
    CHECK(strstr(app->settings.output_filename_a, app->settings.output_base_name) != NULL);
    CHECK(strstr(app->settings.output_filename_a, app->settings.rf_channel_tags[0]) != NULL);
    CHECK(strstr(app->settings.output_filename_a, "_16-bit.flac") != NULL);
    CHECK(strcmp(app->settings.audio_2ch_12_filename, app->settings.audio_2ch_34_filename) != 0);
    app->settings.append_timestamp_on_capture_start = true;
    CHECK(gui_record_apply_auto_names(app));
    CHECK(strlen(app->settings.output_filename_a) > strlen(app->settings.output_base_name) + 25);
    memset(app->settings.output_base_name, 'x', MAX_FILENAME_LEN - 1);
    app->settings.output_base_name[MAX_FILENAME_LEN - 1] = 0;
    CHECK(!gui_record_apply_auto_names(app));
    CHECK(app->settings.output_filename_a[0] == 0);
    app->settings.append_timestamp_on_capture_start = false;
    memset(app->settings.output_base_name, 'x', 230);
    app->settings.output_base_name[230] = 0;
    app->settings.rf_channel_tags[0][0] = 0;
    strcpy(app->settings.audio_output_tags[1], "unused-tag-is-long");
    CHECK(gui_record_apply_auto_names(app));  // Disabled WAV names must not block RF.
    CHECK(app->settings.audio_2ch_12_filename[0] == 0);
    app->settings.enable_audio_2ch_12 = true;
    CHECK(!gui_record_apply_auto_names(app));

    char *tmp = file_getenv_utf8("TMPDIR");
    char directory[FILE_PATH_MAX];
    snprintf(directory, sizeof(directory), "%s/misrc-record-path-%lu-\xe4\xb8\xad\xe6\x96\x87-\xf0\x9f\x93\xbc",
             tmp && tmp[0] ? tmp : ".", (unsigned long)time(NULL));
    free(tmp);
    CHECK(file_mkdir_utf8(directory, 0700) == 0);
    snprintf(app->settings.output_path, sizeof(app->settings.output_path), "%s", directory);
    app->settings.auto_names_enabled = false;
    app->settings.capture_a = app->settings.capture_b = true;
    app->settings.enable_audio_2ch_12 = true;
    char *names[] = {app->settings.output_filename_a, app->settings.output_filename_b,
                     app->settings.audio_2ch_12_filename};
    const char *ext[] = {"a.flac", "b.raw", "audio.wav"};
    char paths[3][GUI_RECORD_PATH_MAX];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 70; j++) memcpy(names[i] + j * 3, "\xe4\xb8\xad", 3);
        snprintf(names[i] + 210, MAX_FILENAME_LEN - 210, "_\xe4\xb8\xad\xe6\x96\x87_%s", ext[i]);
        snprintf(paths[i], sizeof(paths[i]), "%s/%s", directory, names[i]);
        FILE *f = file_open_utf8(paths[i], "wb");
        CHECK(f != NULL);
        if (f) { CHECK(fwrite("sentinel", 1, 8, f) == 8); fclose(f); }
    }
    unsigned existing = 0;
    struct { char before[16]; char text[64]; char after[16]; } guarded;
    memset(&guarded, 'Q', sizeof(guarded));
    CHECK(gui_record_check_output_paths(app, &existing, guarded.text, sizeof(guarded.text), NULL));
    CHECK(existing == 3);  // Existing WAV must be included, not just RF.
    CHECK(guarded.text[sizeof(guarded.text) - 1] == 0);
    for (int i = 0; i < 16; i++) CHECK(guarded.before[i] == 'Q' && guarded.after[i] == 'Q');
    app->settings.capture_a = app->settings.capture_b = false;
    CHECK(gui_record_check_output_paths(app, &existing, NULL, 0, NULL));
    CHECK(existing == 1);
    app->settings.capture_a = app->settings.capture_b = true;
    strcpy(app->settings.output_filename_b, app->settings.output_filename_a);
    CHECK(!gui_record_check_output_paths(app, &existing, NULL, 0, NULL));
    app->settings.output_filename_b[0] = 0;
    CHECK(!gui_record_check_output_paths(app, &existing, NULL, 0, NULL));
    CHECK(!gui_record_valid_filename("../same.wav"));
#ifdef _WIN32
    CHECK(gui_record_same_filename("\xc3\x84.wav", "\xc3\xa4.wav"));
    CHECK(!gui_record_valid_filename("same.wav."));
    CHECK(!gui_record_valid_filename("same.wav "));
    CHECK(!gui_record_valid_filename("back\\slash.wav"));
#else
    CHECK(!gui_record_same_filename("A.wav", "a.wav"));
    CHECK(gui_record_valid_filename("back\\slash\nquote\".wav"));
#endif
    for (int i = 0; i < 3; i++) {
        FILE *f = file_open_read_utf8(paths[i]);
        char contents[8] = {0};
        CHECK(f != NULL);
        if (f) { CHECK(fread(contents, 1, sizeof(contents), f) == 8); fclose(f); }
        CHECK(memcmp(contents, "sentinel", 8) == 0);
        remove_path(paths[i], false);
    }
    check_overwrite_targets(app, directory);
    check_flac_finalize(directory);
    check_legacy_writer(app, directory, false, 8, false, 17, 23);
    check_legacy_writer(app, directory, false, 16, false, GUI_RECORD_WRITER_BLOCK_SAMPLES + 31, 37);
    check_legacy_writer(app, directory, true, 12, false, 17, 23);
    check_legacy_writer(app, directory, true, 12, false, GUI_RECORD_WRITER_BLOCK_SAMPLES + 31, 37);
#if LIBSOXR_ENABLED
    check_legacy_writer(app, directory, false, 16, true, 17, 23);
    check_legacy_writer(app, directory, false, 16, true, GUI_RECORD_WRITER_BLOCK_SAMPLES + 31, 37);
    check_legacy_writer(app, directory, true, 12, true, 17, 23);
    check_legacy_writer(app, directory, true, 12, true, GUI_RECORD_WRITER_BLOCK_SAMPLES + 31, 37);
#endif
    puts("Legacy recording: sub-block and full-block tails, ring before spill, RAW and decoded FLAC checked");
    const size_t active_count = GUI_RECORD_WRITER_BLOCK_SAMPLES + 130935;
    int16_t *active_input = malloc(active_count * sizeof(*active_input));
    assert(active_input);
    for (size_t i = 0; i < active_count; ++i) active_input[i] = (int16_t)((int)(i % 4093) - 2046);
    check_active_spill(app, directory, false, active_input, active_count, 0, false);
    check_active_spill(app, directory, true, active_input, active_count, 1, false);
    free(active_input);
    const size_t race_count = 2 * GUI_RECORD_WRITER_BLOCK_SAMPLES + 31;
    int16_t *race_input = malloc(race_count * sizeof(*race_input));
    assert(race_input);
    for (size_t i = 0; i < race_count; ++i)
        race_input[i] = (int16_t)((int)(i / GUI_RECORD_WRITER_BLOCK_SAMPLES) * 1024 + (int)(i % 1024) - 1024);
    check_active_spill(app, directory, false, race_input, race_count, 0, true);
    check_active_spill(app, directory, true, race_input, race_count, 1, true);
    free(race_input);
    check_raw_short_write_retry(directory);
    check_failed_output_close(directory);
    check_unread_spill_is_failure(app, directory);
    check_exclusive_output_preserves_existing(directory);
    check_failed_spill_enqueue(app, directory);
#if LIBSOXR_ENABLED
    check_resampler_failure();
#endif
    check_capture_log_collision(app, directory);
    check_physical_audio_source_loss(app);
    check_receiver_no_replace(directory);
#if defined(_WIN32)
    check_receiver_stage_setup_retry(directory);
    check_receiver_stage_pinning(directory, false);
    check_receiver_stage_pinning(directory, true);
#else
    check_receiver_replaced_stage(directory);
    check_receiver_replaced_payload(directory, false);
    check_receiver_replaced_payload(directory, true);
    check_receiver_posix_group(directory);
#endif
    remove_path(directory, true);
    free(app);
    printf("record paths: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
