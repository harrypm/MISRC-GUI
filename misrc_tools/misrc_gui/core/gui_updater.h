/* gui_updater: release update checking + confirmed self-update.
 *
 * Design (2026-10-22, replaces the compiled-in asset-URL updater that 404'd
 * repeatedly, e.g. v1.2.4):
 *
 * 1. The manual "Download Latest" button is gone; a "Releases Page" button
 *    opens the releases PAGE (GUI_UPDATER_RELEASES_PAGE_URL). A page URL can
 *    never 404 the way a per-platform asset URL can.
 * 2. The "auto patch": every installed app fetches a flat-JSON update
 *    manifest from a FIXED raw URL on main (GUI_UPDATER_MANIFEST_URL).
 *    Asset filename patterns, the releases page, the tag lookup and the
 *    download base all live in that manifest. Fixing any of them for every
 *    installed binary = editing update_manifest.json and pushing to main -
 *    no new release required.
 * 3. Self-update (desktop): on explicit two-click user confirmation the app
 *    downloads its platform asset, archives the old binary by RENAMING it to
 *    "<path>" GUI_UPDATER_ARCHIVE_SUFFIX "<old-version>" (never deleted),
 *    and moves the new payload into place. Android uses a guided browser
 *    download + the system package installer (see gui_ui.c).
 *
 * Manifest format (update_manifest.json at the repo root): FLAT JSON only -
 * one level of "key": "value" string pairs (plus the integer "schema"). No
 * escapes, no nesting; values must be single-line. The parser enforces this.
 */
#ifndef GUI_UPDATER_H
#define GUI_UPDATER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUI_UPDATER_TAG_MAX 64
#define GUI_UPDATER_URL_MAX 512
#define GUI_UPDATER_PATH_MAX 1024
#define GUI_UPDATER_ERROR_MAX 200
#define GUI_UPDATER_STATUS_MAX 512
#define GUI_UPDATER_MANIFEST_TEXT_MAX (16 * 1024)
#define GUI_UPDATER_ASSET_PATTERN_MAX 128

/* The "auto patch" source: a fixed raw URL on main. */
#define GUI_UPDATER_MANIFEST_URL \
    "https://raw.githubusercontent.com/harrypm/MISRC-GUI/main/update_manifest.json"

/* Compiled-in fallbacks, used ONLY when the manifest is unreachable. The
 * releases page is the safe fallback by design (never asset-specific).
 * There are deliberately NO compiled-in asset filename patterns: when the
 * manifest is unavailable, self-update is disabled instead of guessing
 * asset names that can rot inside old binaries (the v1.2.4 failure class). */
#define GUI_UPDATER_RELEASES_PAGE_URL "https://github.com/harrypm/MISRC-GUI/releases/"
#define GUI_UPDATER_TAG_LOOKUP_URL "https://github.com/harrypm/MISRC-GUI/releases/latest"
#define GUI_UPDATER_DOWNLOAD_BASE_URL "https://github.com/harrypm/MISRC-GUI/releases/download"

/* Old binaries/apps are archived under this suffix, never deleted. */
#define GUI_UPDATER_ARCHIVE_SUFFIX ".archived-"

typedef struct {
    bool parsed;
    char releases_page[GUI_UPDATER_URL_MAX];
    char tag_lookup_url[GUI_UPDATER_URL_MAX];
    char download_base_url[GUI_UPDATER_URL_MAX];
    char asset_android[GUI_UPDATER_ASSET_PATTERN_MAX];
    char asset_macos[GUI_UPDATER_ASSET_PATTERN_MAX];
    char asset_windows_x86[GUI_UPDATER_ASSET_PATTERN_MAX];
    char asset_windows_arm64[GUI_UPDATER_ASSET_PATTERN_MAX];
    char asset_linux_x86[GUI_UPDATER_ASSET_PATTERN_MAX];
    char asset_linux_arm64[GUI_UPDATER_ASSET_PATTERN_MAX];
} gui_updater_manifest_t;

/* Parse a manifest text (flat JSON, see file header). Returns false and
 * leaves *out zeroed on any malformed/missing field. Pure logic, no IO. */
bool gui_updater_parse_manifest_text(const char *text, gui_updater_manifest_t *out);

/* Fetch + parse the compiled-in manifest URL (what the GUI updater uses). */
bool gui_updater_fetch_manifest(gui_updater_manifest_t *out,
                                char *error_out, size_t error_out_len);

/* Load + parse a manifest from an explicit source (the CLI's --update-manifest
 * override for diagnostics/automated tests): an https URL, an http URL on a
 * loopback host (127.0.0.1/localhost/[::1] ONLY - the automated end-to-end
 * test serves the manifest/tag/assets from a local server; everything else
 * must be https), or a local file path. Same acceptance rules as the live
 * fetch. */
bool gui_updater_load_manifest_from(const char *source,
                                    gui_updater_manifest_t *out,
                                    char *error_out, size_t error_out_len);

/* --- helpers shared by the check + install paths (moved from gui_ui.c) --- */
void gui_updater_trim_ascii_whitespace_inplace(char *s);
bool gui_updater_ci_starts_with(const char *value, const char *prefix);
bool gui_updater_extract_release_tag_from_url(const char *url,
                                              char *tag_out, size_t tag_out_len);
bool gui_updater_parse_semver_triplet(const char *version,
                                      int *major_out, int *minor_out, int *patch_out);

/* True when the version string is a comparable release, (v)X.Y.Z. Dev builds
 * (dev-YYYY-MM-DD-<sha>) and unversioned builds ("unknown_version") are NOT
 * comparable: they are never treated as older or newer than a release tag. */
bool gui_updater_version_is_comparable(const char *version);

/* -1 current older than latest, 0 equal/not comparable, +1 current newer.
 * A NON-comparable current (dev/unknown build) always returns 0: the old
 * lexical fallback made every dev build report "update available" against
 * the current release ('d' < 'v'), even when the dev build was newer. */
int gui_updater_compare_versions(const char *current_version, const char *latest_version);
bool gui_updater_is_release_tag_safe(const char *tag);
bool gui_updater_fetch_latest_release_tag(const char *tag_lookup_url,
                                          char *tag_out, size_t tag_out_len,
                                          char *error_out, size_t error_out_len);

/* --- asset URL building (manifest-driven, never compiled-in) --- */
const char *gui_updater_platform_asset_pattern(const gui_updater_manifest_t *manifest);
bool gui_updater_build_asset_filename(const char *pattern, const char *tag,
                                      char *out, size_t out_len);
bool gui_updater_build_asset_url(const gui_updater_manifest_t *manifest, const char *tag,
                                 char *url_out, size_t url_out_len,
                                 char *asset_name_out, size_t asset_name_len);

/* --- self-update install pipeline (desktop platforms; runs on a worker
 * thread, never the UI thread) --- */

/* Current executable/AppImage/app-bundle path (not supported on Android). */
bool gui_updater_get_executable_path(char *out, size_t out_len);

/* Per-platform user updates directory (created on demand). */
bool gui_updater_get_updates_dir(char *out, size_t out_len);

/* "<target>" GUI_UPDATER_ARCHIVE_SUFFIX "<sanitized current version>". */
bool gui_updater_build_archive_path(const char *target_path, const char *current_version,
                                    char *out, size_t out_len);

/* Full confirmed-install pipeline: download the asset into the updates dir,
 * extract the payload, archive the old binary by rename (never delete),
 * move the new payload into place, restore the old binary if the final
 * move fails. status_out carries a human-readable result for the UI (also
 * on failure, including where the downloaded file was left); archive_path_out
 * receives the old binary's archive location on success. */
bool gui_updater_install_asset(const char *asset_url, const char *asset_name,
                               const char *current_version,
                               char *status_out, size_t status_len,
                               char *archive_path_out, size_t archive_path_len);

#ifdef __cplusplus
}
#endif

#endif /* GUI_UPDATER_H */
