/* Exercise production settings persistence without a display or hardware. */
#include "../misrc_gui/core/gui_settings.c"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

uint32_t get_num_cores(void) { return 8; }

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "settings paths: line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

static int remove_fixture(const char *path, bool directory) {
#ifdef _WIN32
    wchar_t wide[FILE_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, FILE_PATH_MAX)) return -1;
    return directory ? _wrmdir(wide) : _wremove(wide);
#else
    return directory ? rmdir(path) : unlink(path);
#endif
}

static bool write_config(const char *path, const char *content) {
    FILE *f = file_open_utf8(path, "wb");
    if (!f) return false;
    bool ok = fputs(content, f) >= 0;
    return fclose(f) == 0 && ok;
}

int main(void) {
    const char unicode[] = "\xe3\x80\x8c\xe3\x83\xbb \xe3\x80\x8d\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x93\xbc";
    char root[FILE_PATH_MAX], config[FILE_PATH_MAX], nested[FILE_PATH_MAX];
#ifdef _WIN32
    wchar_t temp_wide[FILE_PATH_MAX];
    char temp[FILE_PATH_MAX];
    CHECK(GetTempPathW(FILE_PATH_MAX, temp_wide) > 0);
    CHECK(gui_settings_from_wide(temp, sizeof(temp), temp_wide));
    CHECK(gui_settings_format(root, sizeof(root), "%s/misrc-settings-%ld-%s", temp, (long)_getpid(), unicode));
#else
    const char *temp = getenv("TMPDIR");
    if (!temp || !temp[0]) temp = "/tmp";
    CHECK(gui_settings_format(root, sizeof(root), "%s/misrc-settings-%ld-%s", temp, (long)getpid(), unicode));
#endif
    CHECK(file_mkdir_utf8(root, 0700) == 0);
    CHECK(gui_settings_format(nested, sizeof(nested), "%s/%s", root, unicode));
    CHECK(gui_settings_format(config, sizeof(config), "%s/settings.json", nested));
    gui_settings_t before, after;
    gui_settings_init_defaults(&before);
    before.auto_names_enabled = false;
    CHECK(gui_settings_format(before.output_path, sizeof(before.output_path), "%s/quote\"slash\\line\nfeed\r", unicode));
    CHECK(gui_settings_format(before.playback_file_a, sizeof(before.playback_file_a), "%s/file.flac", unicode));
    // A key-looking substring in a quoted value must not shadow a real key.
    CHECK(gui_settings_copy(before.ingest_notes, sizeof(before.ingest_notes), "\"playback_file_b\": \"wrong\""));
    memset(before.playback_file_b, 'x', 700);
    before.playback_file_b[700] = '\0';
    gui_settings_set_override_path(config);
    gui_settings_save(&before);
    CHECK(file_path_info_utf8(config, NULL, NULL));
    gui_settings_load(&after);
    CHECK(!strcmp(before.output_path, after.output_path));
    CHECK(!strcmp(before.playback_file_a, after.playback_file_a));
    CHECK(!strcmp(before.playback_file_b, after.playback_file_b));
    CHECK(!strcmp(before.ingest_notes, after.ingest_notes));

    CHECK(write_config(config, "{\n\"auto_names_enabled\": false,\n\"output_path\": \"C:\\tapes\\new\\\",\n\"playback_file_a\": \"C:\\video\\test.flac\"\n}"));
    gui_settings_load(&after);
    CHECK(!strcmp(after.output_path, "C:\\tapes\\new\\"));
    CHECK(!strcmp(after.playback_file_a, "C:\\video\\test.flac"));

    CHECK(write_config(config, "{\n\"settings_format_version\": 2,\n\"output_path\": \"\\u4e2d\\u6587\\ud83d\\udcfc\",\n\"playback_file_a\": \"C:\\\\tapes\\\\new.flac\"\n}"));
    gui_settings_load(&after);
    CHECK(!strcmp(after.output_path, "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x93\xbc"));
    CHECK(!strcmp(after.playback_file_a, "C:\\tapes\\new.flac"));
    CHECK(find_value("{\"settings_format_version\": 2,\"output_path\": \"\\uD800\"}", "output_path") == NULL);
    CHECK(find_value("{\"settings_format_version\": 2,\"output_path\": \"\\u0000bad\"}", "output_path") == NULL);
    CHECK(find_value("{\"settings_format_version\": 2,\"output_path\": \"\\u", "output_path") == NULL);
    CHECK(find_value("{\"settings_format_version\": 2,\"output_path\": \"\\u12", "output_path") == NULL);
    CHECK(find_value("{\"settings_format_version\": 2,\"output_path\": \"\\ud800\\u", "output_path") == NULL);

    // A leading BOM must preserve both legacy paths and current JSON escapes,
    // including the values that automatic saving would otherwise overwrite.
    const char *bom_configs[] = {
        "\xef\xbb\xbf{\"device_index\": 7,\"auto_names_enabled\": false,"
        "\"output_path\": \"C:\\tapes\\new\\\",\"playback_file_a\": \"C:\\video\\test.flac\"}",
        "\xef\xbb\xbf{\"settings_format_version\": 2,\"device_index\": 7,\"auto_names_enabled\": false,"
        "\"output_path\": \"\\u4e2d\\u6587\",\"playback_file_a\": \"C:\\\\video\\\\test.flac\"}"
    };
    const char *bom_output_paths[] = {"C:\\tapes\\new\\", "\xe4\xb8\xad\xe6\x96\x87"};
    for (size_t i = 0; i < sizeof(bom_configs)/sizeof(bom_configs[0]); i++) {
        CHECK(write_config(config, bom_configs[i]));
        for (int roundtrip = 0; roundtrip < 2; roundtrip++) {
            gui_settings_load(&after);
            CHECK(after.device_index == 7);
            CHECK(!after.auto_names_enabled);
            CHECK(!strcmp(after.output_path, bom_output_paths[i]));
            CHECK(!strcmp(after.playback_file_a, "C:\\video\\test.flac"));
            if (!roundtrip) gui_settings_save(&after);
        }
    }

    gui_settings_init_defaults(&after);
    FILE *f = file_open_utf8(config, "wb");
    CHECK(f != NULL);
    fputs("{\"settings_format_version\": 2,\"output_path\": \"", f);
    for (size_t i = 0; i < FILE_PATH_MAX; i++) fputc('x', f);
    fputs("\"}", f);
    CHECK(fclose(f) == 0);
    gui_settings_load(&after);
    CHECK(after.output_path[0] == '\0');
    char oversized_name[301], name_json[512];
    memset(oversized_name, 'x', sizeof(oversized_name) - 1);
    oversized_name[sizeof(oversized_name) - 1] = '\0';
    CHECK(gui_settings_format(name_json, sizeof(name_json),
        "{\"auto_names_enabled\": false,\"output_filename_a\": \"%s\"}", oversized_name));
    CHECK(write_config(config, name_json));
    gui_settings_load(&after);
    CHECK(after.output_filename_a[0] == '\0');
    CHECK(gui_settings_format(name_json, sizeof(name_json),
        "{\"output_path\": \".\",\"output_base_name\": \"%s\"}", oversized_name));
    CHECK(write_config(config, name_json));
    gui_settings_load(&after);
    CHECK(after.output_path[0] == '\0');
    char small[] = "unchanged";
    CHECK(!gui_settings_copy(small, sizeof(small), "too-long-to-copy"));
    CHECK(!strcmp(small, "unchanged"));

#ifdef _WIN32
    // Legacy ACP directory values migrate once; UTF-8 values above stay intact.
    wchar_t wide[] = L"\x4e2d\x6587";
    char legacy[64], expected[64], json[256];
    BOOL used_default = FALSE;
    if (WideCharToMultiByte(CP_ACP, 0, wide, -1, legacy, sizeof(legacy), NULL, &used_default) && !used_default) {
        CHECK(gui_settings_from_wide(expected, sizeof(expected), wide));
        CHECK(gui_settings_format(json, sizeof(json), "{\"output_path\": \"%s\"}", legacy));
        CHECK(write_config(config, json));
        gui_settings_load(&after);
        CHECK(!strcmp(after.output_path, expected));
    }
    char tiny[] = "same";
    CHECK(!gui_settings_from_wide(tiny, sizeof(tiny), L"\x4e2d\x6587"));
    CHECK(!strcmp(tiny, "same"));
#else
    char literal[FILE_PATH_MAX], prefix[FILE_PATH_MAX];
    CHECK(gui_settings_format(prefix, sizeof(prefix), "%s/literal", root));
    CHECK(write_config(prefix, "existing regular file"));
    CHECK(gui_settings_format(literal, sizeof(literal), "%s/literal\\name.json", root));
    gui_settings_set_override_path(literal);
    gui_settings_save(&before);
    gui_settings_load(&after);
    CHECK(!strcmp(before.output_path, after.output_path));
    CHECK(remove_fixture(literal, false) == 0);
    CHECK(remove_fixture(prefix, false) == 0);

    const char picked[] = "/tmp/\xe4\xb8\xad\xe6\x96\x87/quote\"literal\\line\nfeed\r\n";
    f = tmpfile();
    CHECK(f != NULL);
    CHECK(fwrite(picked, 1, sizeof(picked) - 1, f) == sizeof(picked) - 1);
    fputc('\n', f); // Picker transport terminator, not part of the filename.
    rewind(f);
    char actual[FILE_PATH_MAX];
    CHECK(gui_settings_read_picker(f, actual, sizeof(actual)));
    CHECK(!strcmp(actual, picked));
    fclose(f);
    f = tmpfile();
    CHECK(f != NULL);
    for (int i = 0; i < 40; i++) fputc('x', f);
    fputc('\n', f);
    rewind(f);
    CHECK(!gui_settings_read_picker(f, actual, 16));
    fclose(f);
#endif

    gui_settings_init_defaults(&after);
    memset(after.output_base_name, 'x', sizeof(after.output_base_name) - 1);
    after.output_base_name[sizeof(after.output_base_name) - 1] = '\0';
    gui_settings_refresh_auto_names(&after);
    CHECK(!after.output_filename_a[0] && !after.output_filename_b[0]);
    CHECK(!after.audio_2ch_12_filename[0] && !after.audio_2ch_34_filename[0]);
    CHECK(remove_fixture(config, false) == 0);
    CHECK(remove_fixture(nested, true) == 0);
    // Exercise the real default configuration location, not only --config.
#ifdef _WIN32
    wchar_t root_wide[FILE_PATH_MAX];
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, root_wide, FILE_PATH_MAX));
    CHECK(SetEnvironmentVariableW(L"APPDATA", root_wide));
    CHECK(gui_settings_format(nested, sizeof(nested), "%s/MISRC", root));
    CHECK(gui_settings_format(config, sizeof(config), "%s/misrc_gui_settings.json", nested));
#else
    CHECK(setenv("XDG_CONFIG_HOME", root, 1) == 0);
    CHECK(gui_settings_format(config, sizeof(config), "%s/misrc_gui_settings.json", root));
#endif
    gui_settings_set_override_path(NULL);
    CHECK(!strcmp(get_settings_file_path(), config));
    gui_settings_save(&before);
    gui_settings_load(&after);
    CHECK(!strcmp(before.playback_file_a, after.playback_file_a));
    CHECK(remove_fixture(config, false) == 0);
#ifdef _WIN32
    CHECK(remove_fixture(nested, true) == 0);
#endif
    CHECK(remove_fixture(root, true) == 0);
    puts("settings paths: Unicode, escapes, legacy, limits and native separators passed");
    return 0;
}
