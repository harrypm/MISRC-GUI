/* Exercise the production recording filename and overwrite preflight without hardware. */
#ifdef _WIN32
#include <direct.h>
#endif
#include "../misrc_gui/output/gui_record.c"
#include <FLAC/stream_encoder.h>

static char last_status[256];
void gui_app_set_status(gui_app_t *app, const char *message) {
    (void)app;
    snprintf(last_status, sizeof(last_status), "%s", message);
}
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
    CHECK(gui_record_check_output_paths(app, &existing, guarded.text, sizeof(guarded.text)));
    CHECK(existing == 3);  // Existing WAV must be included, not just RF.
    CHECK(guarded.text[sizeof(guarded.text) - 1] == 0);
    for (int i = 0; i < 16; i++) CHECK(guarded.before[i] == 'Q' && guarded.after[i] == 'Q');
    app->settings.capture_a = app->settings.capture_b = false;
    CHECK(gui_record_check_output_paths(app, &existing, NULL, 0));
    CHECK(existing == 1);
    app->settings.capture_a = app->settings.capture_b = true;
    strcpy(app->settings.output_filename_b, app->settings.output_filename_a);
    CHECK(!gui_record_check_output_paths(app, &existing, NULL, 0));
    app->settings.output_filename_b[0] = 0;
    CHECK(!gui_record_check_output_paths(app, &existing, NULL, 0));
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
    check_flac_finalize(directory);
    remove_path(directory, true);
    free(app);
    printf("record paths: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
