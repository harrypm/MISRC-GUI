/* gui_updater unit tests: manifest parse (flat JSON), {tag} asset URL
 * building, semver compare + dev-tag text fallback, release tag extraction +
 * safety, archive path naming, and the no-network failure paths of the
 * confirmed self-update pipeline (the download step uses a reserved-invalid
 * host so curl fails without any real network dependency).
 *
 * Includes the production gui_updater.c directly (established repo test
 * pattern) so every moved helper stays covered after the gui_ui.c split. */
#include "../misrc_gui/core/gui_updater.c"
#include <assert.h>

static const char *k_valid_manifest =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"releases_page\": \"https://github.com/harrypm/MISRC-GUI/releases/\",\n"
    "  \"tag_lookup_url\": \"https://github.com/harrypm/MISRC-GUI/releases/latest\",\n"
    "  \"download_base_url\": \"https://github.com/harrypm/MISRC-GUI/releases/download\",\n"
    "  \"asset_android\": \"Android_MISRC_GUI_{tag}_arm64.apk\",\n"
    "  \"asset_macos\": \"macOS_MISRC_GUI_{tag}_universal.dmg\",\n"
    "  \"asset_windows_x86\": \"Windows_MISRC_GUI_{tag}_x86.zip\",\n"
    "  \"asset_windows_arm64\": \"Windows_MISRC_GUI_{tag}_arm64.zip\",\n"
    "  \"asset_linux_x86\": \"Linux_MISRC_GUI_{tag}_x86.zip\",\n"
    "  \"asset_linux_arm64\": \"Linux_MISRC_GUI_{tag}_arm64.zip\"\n"
    "}\n";

/* Mutate one same-length region of a copy of the valid manifest. */
static bool manifest_with_mutation(char *buf, size_t buf_len,
                                    const char *needle, const char *replacement)
{
    if (strlen(k_valid_manifest) + 1 > buf_len) return false;
    snprintf(buf, buf_len, "%s", k_valid_manifest);
    char *pos = strstr(buf, needle);
    if (!pos || strlen(needle) != strlen(replacement)) return false;
    memcpy(pos, replacement, strlen(replacement));
    return true;
}

static void test_manifest_parse(void)
{
    gui_updater_manifest_t m;

    assert(gui_updater_parse_manifest_text(k_valid_manifest, &m));
    assert(m.parsed);
    assert(!strcmp(m.releases_page, "https://github.com/harrypm/MISRC-GUI/releases/"));
    assert(!strcmp(m.tag_lookup_url, "https://github.com/harrypm/MISRC-GUI/releases/latest"));
    assert(!strcmp(m.download_base_url, "https://github.com/harrypm/MISRC-GUI/releases/download"));
    assert(!strcmp(m.asset_android, "Android_MISRC_GUI_{tag}_arm64.apk"));
    assert(!strcmp(m.asset_macos, "macOS_MISRC_GUI_{tag}_universal.dmg"));
    assert(!strcmp(m.asset_windows_x86, "Windows_MISRC_GUI_{tag}_x86.zip"));
    assert(!strcmp(m.asset_windows_arm64, "Windows_MISRC_GUI_{tag}_arm64.zip"));
    assert(!strcmp(m.asset_linux_x86, "Linux_MISRC_GUI_{tag}_x86.zip"));
    assert(!strcmp(m.asset_linux_arm64, "Linux_MISRC_GUI_{tag}_arm64.zip"));

    /* Missing key: no asset patterns at all. */
    assert(!gui_updater_parse_manifest_text(
        "{ \"schema\": 1, \"releases_page\": \"https://x.example/\" }", &m));
    /* Not an object / garbage. */
    assert(!gui_updater_parse_manifest_text("", &m));
    assert(!gui_updater_parse_manifest_text("not json at all", &m));
    /* Wrong schema version. */
    {
        char buf[GUI_UPDATER_MANIFEST_TEXT_MAX];
        assert(manifest_with_mutation(buf, sizeof(buf), "\"schema\": 1", "\"schema\": 2"));
        assert(!gui_updater_parse_manifest_text(buf, &m));
    }
    /* Non-https releases page rejected. */
    {
        char buf[GUI_UPDATER_MANIFEST_TEXT_MAX];
        assert(manifest_with_mutation(buf, sizeof(buf),
            "\"releases_page\": \"https://", "\"releases_page\": \"httpx://"));
        assert(!gui_updater_parse_manifest_text(buf, &m));
    }
    /* Asset pattern without the {tag} placeholder rejected. */
    {
        char buf[GUI_UPDATER_MANIFEST_TEXT_MAX];
        assert(manifest_with_mutation(buf, sizeof(buf),
            "Android_MISRC_GUI_{tag}_arm64.apk", "Android_MISRC_GUI_{ver}_arm64.apk"));
        assert(!gui_updater_parse_manifest_text(buf, &m));
    }
    /* Multiline value rejected (single-line strings only): a newline inside
     * the value (replacing the final 'k', same length) spans lines. */
    {
        char buf[GUI_UPDATER_MANIFEST_TEXT_MAX];
        assert(manifest_with_mutation(buf, sizeof(buf),
            "Android_MISRC_GUI_{tag}_arm64.apk", "Android_MISRC_GUI_{tag}_arm64.ap\n"));
        assert(!gui_updater_parse_manifest_text(buf, &m));
    }
    /* Unterminated value rejected. */
    {
        char buf[GUI_UPDATER_MANIFEST_TEXT_MAX];
        assert(manifest_with_mutation(buf, sizeof(buf),
            "Android_MISRC_GUI_{tag}_arm64.apk\",", "Android_MISRC_GUI_{tag}_arm64.apk,,"));
        assert(!gui_updater_parse_manifest_text(buf, &m));
    }
}

static void test_helpers(void)
{
    char s[32];

    snprintf(s, sizeof(s), "stale output");
    assert(gui_updater_run_command(NULL, s, sizeof(s)) == -1);
    assert(s[0] == '\0');
#if defined(_WIN32)
    assert(gui_updater_run_command("misrc-updater-nonexistent-tool.exe", s, sizeof(s)) == -1);
    assert(s[0] == '\0');
    assert(gui_updater_run_command("cmd.exe /d /c \"exit /b 7\"", NULL, 0) == 7);
    HANDLE saved_error = GetStdHandle(STD_ERROR_HANDLE);
    assert(SetStdHandle(STD_ERROR_HANDLE, (HANDLE)(UINT_PTR)1));
    int no_error_handle = gui_updater_run_command("cmd.exe /d /c \"exit /b 7\"", NULL, 0);
    assert(SetStdHandle(STD_ERROR_HANDLE, saved_error));
    assert(no_error_handle == 7);
    /* A short capture buffer must still drain output beyond the pipe capacity. */
    assert(gui_updater_run_command(
        "cmd.exe /d /c \"for /L %i in (1,1,1000) do @echo payload\"", s, 5) == 0);
    assert(!strcmp(s, "payl"));
#endif

    snprintf(s, sizeof(s), "  v1.2.6 \t\r\n");
    gui_updater_trim_ascii_whitespace_inplace(s);
    assert(!strcmp(s, "v1.2.6"));

    assert(gui_updater_ci_starts_with("Location: https://x", "location:"));
    assert(!gui_updater_ci_starts_with("location: https://x", "Xocation:"));
    assert(!gui_updater_ci_starts_with(NULL, "x"));
    assert(!gui_updater_ci_starts_with("x", NULL));

    char tag[GUI_UPDATER_TAG_MAX];
    assert(gui_updater_extract_release_tag_from_url(
        "https://github.com/harrypm/MISRC-GUI/releases/tag/v1.2.6", tag, sizeof(tag)));
    assert(!strcmp(tag, "v1.2.6"));
    assert(gui_updater_extract_release_tag_from_url(
        "https://github.com/harrypm/MISRC-GUI/releases/tag/v1.2.7?query=1#frag", tag, sizeof(tag)));
    assert(!strcmp(tag, "v1.2.7"));
    assert(gui_updater_extract_release_tag_from_url(
        "https://github.com/harrypm/MISRC-GUI/releases/tag/", tag, sizeof(tag)) == false);
    assert(!gui_updater_extract_release_tag_from_url(
        "https://github.com/harrypm/MISRC-GUI/releases/latest", tag, sizeof(tag)));
}

static void test_versions_and_tags(void)
{
    int maj = 0, min = 0, pat = 0;
    assert(gui_updater_parse_semver_triplet("v1.2.6", &maj, &min, &pat));
    assert(maj == 1 && min == 2 && pat == 6);
    assert(gui_updater_parse_semver_triplet("1.10.0", &maj, &min, &pat));
    assert(maj == 1 && min == 10 && pat == 0);
    assert(!gui_updater_parse_semver_triplet("v1.2", &maj, &min, &pat));
    assert(!gui_updater_parse_semver_triplet("dev-2026-10-21-abcdef", &maj, &min, &pat));
    assert(!gui_updater_parse_semver_triplet("", &maj, &min, &pat));

    assert(gui_updater_compare_versions("v1.2.5", "v1.2.6") < 0);
    assert(gui_updater_compare_versions("v1.2.6", "v1.2.6") == 0);
    assert(gui_updater_compare_versions("v1.3.0", "v1.2.9") > 0);
    assert(gui_updater_compare_versions("v2.0.0", "v10.0.0") < 0);
    /* Dev/unversioned builds are NOT comparable: never older, never newer.
     * (The old lexical fallback ('d' < 'v') made every dev build permanently
     * report "update available" against the current release.) */
    assert(gui_updater_compare_versions("dev-2026-10-21-abcdef", "v1.2.6") == 0);
    assert(gui_updater_compare_versions("dev-2026-10-21-abcdef-dirty", "v1.2.6") == 0);
    assert(gui_updater_compare_versions("unknown_version", "v1.2.6") == 0);
    assert(gui_updater_compare_versions("dev-2026-10-21-abcdef", "v99.99.99") == 0);
    /* A comparable current against a non-semver latest keeps the text edge. */
    assert(gui_updater_compare_versions("v1.2.6", "weird-tag") < 0);
    assert(gui_updater_compare_versions("v1.2.6", "aaaa") > 0);

    assert(gui_updater_version_is_comparable("v1.2.6"));
    assert(gui_updater_version_is_comparable("1.10.0"));
    assert(!gui_updater_version_is_comparable("dev-2026-10-21-abcdef"));
    assert(!gui_updater_version_is_comparable("dev-2026-10-21-abcdef-dirty"));
    assert(!gui_updater_version_is_comparable("unknown_version"));
    assert(!gui_updater_version_is_comparable(""));
    assert(!gui_updater_version_is_comparable(NULL));

    assert(gui_updater_is_release_tag_safe("v1.2.6"));
    assert(gui_updater_is_release_tag_safe("v1.2.6-rc1"));
    assert(gui_updater_is_release_tag_safe("v1.2.6_beta.2"));
    assert(!gui_updater_is_release_tag_safe(""));
    assert(!gui_updater_is_release_tag_safe("v1 2"));
    assert(!gui_updater_is_release_tag_safe("../etc/passwd"));
    assert(!gui_updater_is_release_tag_safe("v$1"));
    assert(!gui_updater_is_release_tag_safe(NULL));
}

static void test_asset_url_building(void)
{
    gui_updater_manifest_t m;
    assert(gui_updater_parse_manifest_text(k_valid_manifest, &m));

    const char *pattern = gui_updater_platform_asset_pattern(&m);
    assert(pattern && strstr(pattern, "_MISRC_GUI_{tag}_"));
    assert(!gui_updater_platform_asset_pattern(NULL));

    char name[192];
    assert(gui_updater_build_asset_filename("Linux_MISRC_GUI_{tag}_x86.zip", "v1.2.6",
                                            name, sizeof(name)));
    assert(!strcmp(name, "Linux_MISRC_GUI_v1.2.6_x86.zip"));
    assert(!gui_updater_build_asset_filename("Linux_MISRC_GUI_{tag}_x86.zip", "../evil",
                                              name, sizeof(name)));
    assert(!gui_updater_build_asset_filename("no_placeholder.zip", "v1.2.6",
                                              name, sizeof(name)));
    assert(!gui_updater_build_asset_filename("Linux_MISRC_GUI_{tag}_x86.zip", "v1.2.6",
                                              name, 8)); /* too small */

    char url[GUI_UPDATER_URL_MAX];
    char asset[192];
    assert(gui_updater_build_asset_url(&m, "v1.2.6", url, sizeof(url), asset, sizeof(asset)));
    assert(gui_updater_ci_starts_with(
        url, "https://github.com/harrypm/MISRC-GUI/releases/download/v1.2.6/"));
    assert(strstr(url, "_MISRC_GUI_v1.2.6_"));
    assert(!strcmp(strstr(url, "v1.2.6/") + 7, asset));
    assert(strstr(asset, "_MISRC_GUI_v1.2.6_"));
    assert(!gui_updater_build_asset_url(&m, "", url, sizeof(url), asset, sizeof(asset)));
    assert(!gui_updater_build_asset_url(&m, "bad tag!", url, sizeof(url), asset, sizeof(asset)));
}

static void test_archive_path(void)
{
    char out[GUI_UPDATER_PATH_MAX];

    assert(gui_updater_build_archive_path("/opt/MISRC/MISRC.AppImage", "v1.2.5",
                                           out, sizeof(out)));
    assert(!strcmp(out, "/opt/MISRC/MISRC.AppImage" GUI_UPDATER_ARCHIVE_SUFFIX "v1.2.5"));
    /* Unsafe version characters are sanitized into the archive suffix. */
    assert(gui_updater_build_archive_path("/opt/MISRC/MISRC.AppImage", "dev/../x y",
                                           out, sizeof(out)));
    assert(!strcmp(out, "/opt/MISRC/MISRC.AppImage" GUI_UPDATER_ARCHIVE_SUFFIX "dev-..-x-y"));
    assert(!gui_updater_build_archive_path("", "v1.2.5", out, sizeof(out)));
    assert(!gui_updater_build_archive_path(NULL, "v1.2.5", out, sizeof(out)));
    assert(!gui_updater_build_archive_path("/opt/MISRC/MISRC.AppImage", NULL,
                                           out, sizeof(out)));
}

#if !defined(_WIN32) && !defined(__ANDROID__)
static void test_updates_dir_and_install_failure(void)
{
    /* Isolated HOME so the updater's real user dirs are never touched. */
    char home_tpl[] = "/tmp/misrc_gui_updater_test_XXXXXX";
    char *home = mkdtemp(home_tpl);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    char dir[GUI_UPDATER_PATH_MAX];
    assert(gui_updater_get_updates_dir(dir, sizeof(dir)));
    assert(gui_updater_ci_starts_with(dir, home));
    assert(strstr(dir, "misrc/updates"));
    /* Created on demand, idempotent. */
    assert(gui_updater_get_updates_dir(dir, sizeof(dir)));

    /* Self-update download failure path: reserved-invalid host (RFC 6761
     * .invalid TLD never resolves) so this is deterministic, offline-safe
     * CI behavior. The install must fail WITHOUT touching any old binary
     * and with a human-readable status. */
    char status[GUI_UPDATER_STATUS_MAX] = {0};
    char archive[GUI_UPDATER_PATH_MAX] = {0};
    assert(!gui_updater_install_asset("https://update-host.invalid/misrc/x.zip",
                                      "x.zip", "v0.0.1-test",
                                      status, sizeof(status),
                                      archive, sizeof(archive)));
    assert(status[0]);
    assert(!archive[0]);

    /* Nothing was archived or replaced anywhere: no archive path was
     * reported and the install failed before any payload existed. */

    /* Best-effort cleanup of the isolated tree (Apple uses a different
     * subpath than Linux, so walk up from the created dir). Some curl
     * versions leave a 0-byte output on early failure; remove it first. */
    char check[GUI_UPDATER_PATH_MAX + 64];
    snprintf(check, sizeof(check), "%s/x.zip", dir);
    (void)unlink(check);
    char walk[GUI_UPDATER_PATH_MAX];
    snprintf(walk, sizeof(walk), "%s", dir);
    for (int i = 0; i < 4; i++) {
        (void)rmdir(walk);
        char *slash = strrchr(walk, '/');
        if (!slash || slash == walk) break;
        *slash = '\0';
    }
    (void)rmdir(home);
}
#else
static void test_updates_dir_and_install_failure(void)
{
    /* Windows: the download-failure path only (real LOCALAPPDATA dir; the
     * same one the app itself uses, created empty). Android never runs this
     * test (meson test block is non-Android). */
    char status[GUI_UPDATER_STATUS_MAX] = {0};
    char archive[GUI_UPDATER_PATH_MAX] = {0};
# if !defined(__ANDROID__)
    assert(!gui_updater_install_asset("https://update-host.invalid/misrc/x.zip",
                                      "x.zip", "v0.0.1-test",
                                      status, sizeof(status),
                                      archive, sizeof(archive)));
    assert(status[0]);
    assert(!archive[0]);
# endif
}
#endif

int main(void)
{
    test_manifest_parse();
    test_helpers();
    test_versions_and_tags();
    test_asset_url_building();
    test_archive_path();
    test_updates_dir_and_install_failure();
    puts("gui_updater: manifest parse, URL building, semver, tag safety, archive naming, "
         "updates dir + offline install failure passed");
    return 0;
}
