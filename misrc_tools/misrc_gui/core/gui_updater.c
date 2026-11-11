/* gui_updater: release update checking + confirmed self-update.
 * See gui_updater.h for the design. Everything here is plain libc + shell-out
 * to curl/unzip/tar/hdiutil, matching the updater pattern the app
 * has always used. Pure-logic parts (parse/build/compare) are unit-tested by
 * test/gui_updater_test.c, which #includes this file. */
#include "gui_updater.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h> /* stat/S_ISDIR/S_ISREG: the portable forms; the _stat
                           variants are MSVC-only and do NOT compile on MinGW
                           (run 39789335139 broke both Windows jobs on them). */
#if defined(_WIN32)
#include <direct.h>   /* _mkdir */
#include <windows.h>  /* GetModuleFileNameA */
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif
#include <dirent.h>
#include <sys/types.h>

#if defined(_WIN32)
#define GUI_UPDATER_CURL_EXE "curl.exe"
#define GUI_UPDATER_MKDIR(path) _mkdir(path)
#else
#define GUI_UPDATER_POPEN popen
#define GUI_UPDATER_PCLOSE pclose
#define GUI_UPDATER_CURL_EXE "curl"
#define GUI_UPDATER_MKDIR(path) mkdir(path, 0755)
#endif

/* ------------------------------------------------------------------ */
/* Shared helpers (moved from gui_ui.c, now non-static + tested)       */
/* ------------------------------------------------------------------ */

void gui_updater_trim_ascii_whitespace_inplace(char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

bool gui_updater_ci_starts_with(const char *value, const char *prefix)
{
    if (!value || !prefix) return false;
    while (*prefix) {
        if (!*value) return false;
        if (tolower((unsigned char)*value) != tolower((unsigned char)*prefix)) {
            return false;
        }
        value++;
        prefix++;
    }
    return true;
}

bool gui_updater_extract_release_tag_from_url(const char *url, char *tag_out, size_t tag_out_len)
{
    if (!url || !tag_out || tag_out_len == 0) return false;
    tag_out[0] = '\0';
    const char *marker = "/releases/tag/";
    const char *start = strstr(url, marker);
    if (!start) return false;
    start += strlen(marker);
    if (*start == '\0') return false;

    size_t i = 0;
    while (start[i] &&
           !isspace((unsigned char)start[i]) &&
           start[i] != '?' &&
           start[i] != '#' &&
           start[i] != '/') {
        if (i + 1 >= tag_out_len) break;
        tag_out[i] = start[i];
        i++;
    }
    tag_out[i] = '\0';
    return (i > 0);
}

bool gui_updater_parse_semver_triplet(const char *version, int *major_out, int *minor_out, int *patch_out)
{
    if (!version || !major_out || !minor_out || !patch_out) return false;
    const char *p = version;
    if (*p == 'v' || *p == 'V') p++;
    if (!isdigit((unsigned char)*p)) return false;

    char *end = NULL;
    long major = strtol(p, &end, 10);
    if (!end || end == p || *end != '.') return false;
    p = end + 1;
    if (!isdigit((unsigned char)*p)) return false;

    long minor = strtol(p, &end, 10);
    if (!end || end == p || *end != '.') return false;
    p = end + 1;
    if (!isdigit((unsigned char)*p)) return false;

    long patch = strtol(p, &end, 10);
    if (!end || end == p) return false;

    *major_out = (int)major;
    *minor_out = (int)minor;
    *patch_out = (int)patch;
    return true;
}

bool gui_updater_version_is_comparable(const char *version)
{
    int maj = 0, min = 0, pat = 0;
    return gui_updater_parse_semver_triplet(version, &maj, &min, &pat);
}

int gui_updater_compare_versions(const char *current_version, const char *latest_version)
{
    int cmaj = 0, cmin = 0, cpat = 0;
    int lmaj = 0, lmin = 0, lpat = 0;
    bool current_ok = gui_updater_parse_semver_triplet(current_version, &cmaj, &cmin, &cpat);
    bool latest_ok = gui_updater_parse_semver_triplet(latest_version, &lmaj, &lmin, &lpat);
    if (current_ok && latest_ok) {
        if (cmaj != lmaj) return (cmaj < lmaj) ? -1 : 1;
        if (cmin != lmin) return (cmin < lmin) ? -1 : 1;
        if (cpat != lpat) return (cpat < lpat) ? -1 : 1;
        return 0;
    }
    /* A dev/unversioned current build ("dev-2026-10-21-abcdef",
     * "unknown_version") is NOT comparable: return 0 so no caller ever treats
     * it as older than a release tag. The old lexical fallback ('d' < 'v')
     * made every such build permanently report "update available" against
     * the current release even when the dev build was newer. Callers that
     * need to know use gui_updater_version_is_comparable(). */
    if (!current_ok) return 0;
    /* Weird latest (non-semver tag): keep a lexical text comparison for the
     * release-vs-release edge only. */
    int text_cmp = strcmp(current_version ? current_version : "",
                          latest_version ? latest_version : "");
    if (text_cmp < 0) return -1;
    if (text_cmp > 0) return 1;
    return 0;
}

bool gui_updater_is_release_tag_safe(const char *tag)
{
    if (!tag || !tag[0]) return false;
    const unsigned char *p = (const unsigned char *)tag;
    while (*p) {
        if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_')) {
            return false;
        }
        p++;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* External command helper                                             */
/* ------------------------------------------------------------------ */

static int gui_updater_run_command(const char *cmd, char *output, size_t output_size)
{
    if (output && output_size > 0) output[0] = '\0';
    if (!cmd) return -1;
#if defined(_WIN32)
    /* GUI applications must not attach helper tools to a transient console. */
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE read_pipe = NULL, write_pipe = NULL, null_io = INVALID_HANDLE_VALUE;
    HANDLE child_error = NULL;
    PROCESS_INFORMATION process = {0};
    STARTUPINFOA startup = {0};
    char *command_line = NULL;
    int result = -1;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) goto cleanup;
    null_io = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (null_io == INVALID_HANDLE_VALUE) goto cleanup;
    HANDLE parent_error = GetStdHandle(STD_ERROR_HANDLE);
    if (parent_error && parent_error != INVALID_HANDLE_VALUE &&
        !DuplicateHandle(GetCurrentProcess(), parent_error, GetCurrentProcess(),
                         &child_error, 0, TRUE, DUPLICATE_SAME_ACCESS)) child_error = NULL;
    command_line = malloc(strlen(cmd) + 1);
    if (!command_line) goto cleanup;
    strcpy(command_line, cmd);
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_io;
    startup.hStdOutput = write_pipe;
    startup.hStdError = child_error ? child_error : null_io;
    if (!CreateProcessA(NULL, command_line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) goto cleanup;
    CloseHandle(write_pipe);
    write_pipe = NULL;
    char buffer[512];
    DWORD count;
    size_t used = 0;
    BOOL read_ok;
    while ((read_ok = ReadFile(read_pipe, buffer, sizeof(buffer), &count, NULL)) && count) {
        if (output && output_size > 0 && used + 1 < output_size) {
            size_t take = count;
            if (take > output_size - used - 1) take = output_size - used - 1;
            memcpy(output + used, buffer, take);
            used += take;
            output[used] = '\0';
        }
    }
    bool pipe_ok = read_ok || GetLastError() == ERROR_BROKEN_PIPE;
    CloseHandle(read_pipe);
    read_pipe = NULL;
    DWORD exit_code;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exit_code) && pipe_ok) result = (int)exit_code;
cleanup:
    free(command_line);
    if (read_pipe) CloseHandle(read_pipe);
    if (write_pipe) CloseHandle(write_pipe);
    if (null_io != INVALID_HANDLE_VALUE) CloseHandle(null_io);
    if (child_error) CloseHandle(child_error);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    return result;
#else
    FILE *fp = GUI_UPDATER_POPEN(cmd, "r");
    if (!fp) return -1;
    if (output && output_size > 0) {
        size_t used = 0;
        while (used + 1 < output_size) {
            size_t got = fread(output + used, 1, output_size - 1 - used, fp);
            if (got == 0) break;
            used += got;
        }
        output[used] = '\0';
    } else {
        char sink[512];
        while (fread(sink, 1, sizeof(sink), fp) > 0) {
            /* drain */
        }
    }
    return GUI_UPDATER_PCLOSE(fp);
#endif
}

/* ------------------------------------------------------------------ */
/* Manifest: flat-JSON parse (documented format, see header)           */
/* ------------------------------------------------------------------ */

/* Find "key": "value" in flat JSON text. Returns false if the key is absent,
 * malformed, or the value does not fit. No escape handling (format forbids
 * escapes); the value must be a single-line string. */
static bool gui_updater_manifest_find_string(const char *text, const char *key,
                                             char *out, size_t out_len)
{
    if (!text || !key || !out || out_len == 0) return false;
    out[0] = '\0';

    char quoted_key[96];
    if (snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key) >= (int)sizeof(quoted_key)) {
        return false;
    }
    const char *pos = strstr(text, quoted_key);
    if (!pos) return false;
    pos += strlen(quoted_key);

    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != '"') return false;
    pos++;

    size_t i = 0;
    while (pos[i] && pos[i] != '"') {
        if (pos[i] == '\n' || pos[i] == '\r') return false; /* single-line only */
        if (i + 1 >= out_len) return false;
        out[i] = pos[i];
        i++;
    }
    if (pos[i] != '"') return false; /* unterminated */
    out[i] = '\0';
    return (i > 0);
}

static bool gui_updater_manifest_find_int(const char *text, const char *key, long *out)
{
    if (!text || !key || !out) return false;
    char quoted_key[96];
    if (snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key) >= (int)sizeof(quoted_key)) {
        return false;
    }
    const char *pos = strstr(text, quoted_key);
    if (!pos) return false;
    pos += strlen(quoted_key);
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (!isdigit((unsigned char)*pos)) return false;
    char *end = NULL;
    long value = strtol(pos, &end, 10);
    if (!end || end == pos) return false;
    *out = value;
    return true;
}

static bool gui_updater_url_scheme_ok(const char *url)
{
    if (!url || !url[0]) return false;
    if (gui_updater_ci_starts_with(url, "https://")) return true;
    /* http:// is allowed ONLY for loopback hosts (127.0.0.1 / localhost /
     * [::1]): the automated update-CLI end-to-end test serves the manifest,
     * the tag redirect and the asset from a local server. Any other http
     * source is rejected. */
    if (gui_updater_ci_starts_with(url, "http://")) {
        const char *host = url + 7;
        size_t consumed = 0;
        while (host[consumed] && host[consumed] != ':' && host[consumed] != '/' &&
               consumed < 64) {
            consumed++;
        }
        char hostbuf[65];
        memcpy(hostbuf, host, consumed);
        hostbuf[consumed] = '\0';
        return strcmp(hostbuf, "127.0.0.1") == 0 ||
               strcmp(hostbuf, "localhost") == 0 ||
               strcmp(hostbuf, "[::1]") == 0;
    }
    return false;
}

static bool gui_updater_asset_pattern_valid(const char *pattern)
{
    if (!pattern || !pattern[0]) return false;
    if (!strstr(pattern, "{tag}")) return false;
    if (strlen(pattern) >= GUI_UPDATER_ASSET_PATTERN_MAX) return false;
    for (const char *p = pattern; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c) || c == '"' || c == '\'' || c == '\\') return false;
    }
    return true;
}

bool gui_updater_parse_manifest_text(const char *text, gui_updater_manifest_t *out)
{
    if (!text || !out) return false;
    memset(out, 0, sizeof(*out));

    long schema = 0;
    if (!gui_updater_manifest_find_int(text, "schema", &schema) || schema != 1) {
        return false;
    }
    if (!gui_updater_manifest_find_string(text, "releases_page", out->releases_page, sizeof(out->releases_page)) ||
        !gui_updater_manifest_find_string(text, "tag_lookup_url", out->tag_lookup_url, sizeof(out->tag_lookup_url)) ||
        !gui_updater_manifest_find_string(text, "download_base_url", out->download_base_url, sizeof(out->download_base_url)) ||
        !gui_updater_manifest_find_string(text, "asset_android", out->asset_android, sizeof(out->asset_android)) ||
        !gui_updater_manifest_find_string(text, "asset_macos", out->asset_macos, sizeof(out->asset_macos)) ||
        !gui_updater_manifest_find_string(text, "asset_windows_x86", out->asset_windows_x86, sizeof(out->asset_windows_x86)) ||
        !gui_updater_manifest_find_string(text, "asset_windows_arm64", out->asset_windows_arm64, sizeof(out->asset_windows_arm64)) ||
        !gui_updater_manifest_find_string(text, "asset_linux_x86", out->asset_linux_x86, sizeof(out->asset_linux_x86)) ||
        !gui_updater_manifest_find_string(text, "asset_linux_arm64", out->asset_linux_arm64, sizeof(out->asset_linux_arm64))) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    if (!gui_updater_url_scheme_ok(out->releases_page) ||
        !gui_updater_url_scheme_ok(out->tag_lookup_url) ||
        !gui_updater_url_scheme_ok(out->download_base_url)) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    const char *patterns[6] = {
        out->asset_android, out->asset_macos,
        out->asset_windows_x86, out->asset_windows_arm64,
        out->asset_linux_x86, out->asset_linux_arm64,
    };
    for (int i = 0; i < 6; i++) {
        if (!gui_updater_asset_pattern_valid(patterns[i])) {
            memset(out, 0, sizeof(*out));
            return false;
        }
    }

    out->parsed = true;
    return true;
}

bool gui_updater_fetch_manifest(gui_updater_manifest_t *out,
                                char *error_out, size_t error_out_len)
{
    return gui_updater_load_manifest_from(GUI_UPDATER_MANIFEST_URL, out,
                                          error_out, error_out_len);
}

bool gui_updater_load_manifest_from(const char *source,
                                    gui_updater_manifest_t *out,
                                    char *error_out, size_t error_out_len)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (error_out && error_out_len > 0) error_out[0] = '\0';
    if (!source || !source[0]) {
        if (error_out) snprintf(error_out, error_out_len, "manifest source is empty");
        return false;
    }

    char text[GUI_UPDATER_MANIFEST_TEXT_MAX];
    if (gui_updater_ci_starts_with(source, "http://") ||
        gui_updater_ci_starts_with(source, "https://")) {
        if (!gui_updater_url_scheme_ok(source)) {
            if (error_out) {
                snprintf(error_out, error_out_len,
                         "manifest URL scheme not allowed (https, or http on a loopback host only)");
            }
            return false;
        }
        char cmd[GUI_UPDATER_URL_MAX + 64];
        if (snprintf(cmd, sizeof(cmd), "%s -fsSL --max-time 15 \"%s\"",
                     GUI_UPDATER_CURL_EXE, source) >= (int)sizeof(cmd)) {
            if (error_out) snprintf(error_out, error_out_len, "manifest command too long");
            return false;
        }
        int rc = gui_updater_run_command(cmd, text, sizeof(text));
        if (rc != 0) {
            if (error_out) snprintf(error_out, error_out_len, "manifest fetch failed (curl rc=%d)", rc);
            return false;
        }
    } else {
        /* Local file path (diagnostics / offline automated tests). */
        FILE *fp = fopen(source, "rb");
        if (!fp) {
            if (error_out) snprintf(error_out, error_out_len, "cannot open manifest file");
            return false;
        }
        size_t got = fread(text, 1, sizeof(text) - 1, fp);
        text[got] = '\0';
        fclose(fp);
        if (got == 0) {
            if (error_out) snprintf(error_out, error_out_len, "manifest file is empty");
            return false;
        }
    }

    if (!gui_updater_parse_manifest_text(text, out)) {
        if (error_out) snprintf(error_out, error_out_len, "manifest malformed or fields missing");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Tag resolution (releases/latest redirect -> Location -> tag)       */
/* ------------------------------------------------------------------ */

bool gui_updater_fetch_latest_release_tag(const char *tag_lookup_url,
                                          char *tag_out, size_t tag_out_len,
                                          char *error_out, size_t error_out_len)
{
    if (!tag_out || tag_out_len == 0 || !error_out || error_out_len == 0) return false;
    tag_out[0] = '\0';
    error_out[0] = '\0';

    const char *url = (tag_lookup_url && tag_lookup_url[0]) ? tag_lookup_url
                                                            : GUI_UPDATER_TAG_LOOKUP_URL;

    char cmd[GUI_UPDATER_URL_MAX + 64];
    if (snprintf(cmd, sizeof(cmd), "%s -fsSI --max-time 10 \"%s\"",
                 GUI_UPDATER_CURL_EXE, url) >= (int)sizeof(cmd)) {
        snprintf(error_out, error_out_len, "tag lookup command too long");
        return false;
    }

    char headers[4096];
    int rc = gui_updater_run_command(cmd, headers, sizeof(headers));

    char line[512];
    char location[512] = {0};
    const char *cursor = headers;
    while (*cursor) {
        const char *eol = strchr(cursor, '\n');
        size_t len = eol ? (size_t)(eol - cursor) : strlen(cursor);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, cursor, len);
        line[len] = '\0';
        gui_updater_trim_ascii_whitespace_inplace(line);
        if (gui_updater_ci_starts_with(line, "location:")) {
            const char *value = line + 9;
            while (*value && isspace((unsigned char)*value)) value++;
            snprintf(location, sizeof(location), "%s", value);
        }
        if (!eol) break;
        cursor = eol + 1;
    }

    if (location[0] == '\0') {
        if (rc != 0) {
            snprintf(error_out, error_out_len, "tag lookup request failed (curl rc=%d)", rc);
        } else {
            snprintf(error_out, error_out_len, "latest release redirect header missing");
        }
        return false;
    }

    if (!gui_updater_extract_release_tag_from_url(location, tag_out, tag_out_len)) {
        snprintf(error_out, error_out_len, "unable to parse release tag");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Asset URL building (manifest-driven; no compiled-in patterns)       */
/* ------------------------------------------------------------------ */

const char *gui_updater_platform_asset_pattern(const gui_updater_manifest_t *manifest)
{
    if (!manifest || !manifest->parsed) return NULL;
#if defined(__ANDROID__)
    return manifest->asset_android;
#elif defined(__APPLE__)
    return manifest->asset_macos;
#elif defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
    return manifest->asset_windows_arm64;
#else
    return manifest->asset_windows_x86;
#endif
#elif defined(__linux__)
#if defined(__aarch64__) || defined(__arm64__)
    return manifest->asset_linux_arm64;
#else
    return manifest->asset_linux_x86;
#endif
#else
    return NULL;
#endif
}

bool gui_updater_build_asset_filename(const char *pattern, const char *tag,
                                      char *out, size_t out_len)
{
    if (!pattern || !tag || !out || out_len == 0) return false;
    out[0] = '\0';
    if (!gui_updater_is_release_tag_safe(tag)) return false;

    const char *placeholder = strstr(pattern, "{tag}");
    if (!placeholder) return false;

    size_t prefix_len = (size_t)(placeholder - pattern);
    size_t tag_len = strlen(tag);
    size_t suffix_len = strlen(placeholder + 5);
    if (prefix_len + tag_len + suffix_len + 1 > out_len) return false;

    memcpy(out, pattern, prefix_len);
    memcpy(out + prefix_len, tag, tag_len);
    memcpy(out + prefix_len + tag_len, placeholder + 5, suffix_len + 1);
    return true;
}

bool gui_updater_build_asset_url(const gui_updater_manifest_t *manifest, const char *tag,
                                 char *url_out, size_t url_out_len,
                                 char *asset_name_out, size_t asset_name_len)
{
    if (!manifest || !url_out || url_out_len == 0) return false;
    url_out[0] = '\0';
    if (asset_name_out && asset_name_len > 0) asset_name_out[0] = '\0';
    if (!tag || !tag[0]) return false;

    const char *pattern = gui_updater_platform_asset_pattern(manifest);
    if (!pattern) return false;

    char asset[192];
    if (!gui_updater_build_asset_filename(pattern, tag, asset, sizeof(asset))) {
        return false;
    }

    const char *base = (manifest->download_base_url[0]) ? manifest->download_base_url
                                                        : GUI_UPDATER_DOWNLOAD_BASE_URL;
    int written = snprintf(url_out, url_out_len, "%s/%s/%s", base, tag, asset);
    if (written <= 0 || (size_t)written >= url_out_len) {
        url_out[0] = '\0';
        return false;
    }
    if (asset_name_out && asset_name_len > 0) {
        snprintf(asset_name_out, asset_name_len, "%s", asset);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Self-update install pipeline (desktop; worker thread only)          */
/* ------------------------------------------------------------------ */

bool gui_updater_get_executable_path(char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
#if defined(__ANDROID__)
    (void)out_len;
    return false; /* guided browser download path instead (gui_ui.c) */
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_len);
    if (n == 0 || n >= (DWORD)out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)out_len;
    if (_NSGetExecutablePath(out, &size) != 0) {
        out[0] = '\0';
        return false;
    }
    char resolved[GUI_UPDATER_PATH_MAX];
    if (realpath(out, resolved)) {
        snprintf(out, out_len, "%s", resolved);
    }
    return out[0] != '\0';
#else /* Linux: AppImage first ($APPIMAGE is the .AppImage file; /proc/self/exe
       * inside a running AppImage points INTO the squashfs mount, which must
       * never be the replace target), bare binary otherwise. */
    const char *appimage = getenv("APPIMAGE");
    if (appimage && appimage[0] == '/' && access(appimage, F_OK) == 0) {
        if (strlen(appimage) >= out_len) return false;
        snprintf(out, out_len, "%s", appimage);
        return true;
    }
    ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
    if (n <= 0 || (size_t)n >= out_len - 1) {
        out[0] = '\0';
        return false;
    }
    out[n] = '\0';
    return true;
#endif
}

static bool gui_updater_path_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool gui_updater_path_is_file(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* mkdir -p semantics, one component at a time, using only libc. */
static bool gui_updater_ensure_dir(const char *path)
{
    if (!path || !path[0]) return false;
    if (gui_updater_path_is_dir(path)) return true;

    char tmp[GUI_UPDATER_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, path, len + 1);

    /* Strip trailing separators (keep a leading drive/root separator). */
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[--len] = '\0';
    }
    char *start = tmp + 1;
#if defined(_WIN32)
    /* Keep the separator in a drive root: C: alone is drive-relative. */
    if (len >= 3 && isalpha((unsigned char)tmp[0]) && tmp[1] == ':' &&
        (tmp[2] == '/' || tmp[2] == '\\')) {
        start = tmp + 3;
    }
#endif
    for (char *p = start; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            if (!gui_updater_path_is_dir(tmp)) {
                if (GUI_UPDATER_MKDIR(tmp) != 0 && errno != EEXIST) return false;
            }
            *p = sep;
        }
    }
    if (!gui_updater_path_is_dir(tmp)) {
        if (GUI_UPDATER_MKDIR(tmp) != 0 && errno != EEXIST) return false;
    }
    return gui_updater_path_is_dir(tmp);
}

bool gui_updater_get_updates_dir(char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
#if defined(__ANDROID__)
    return false; /* guided browser download path instead (gui_ui.c) */
#elif defined(_WIN32)
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) return false;
    if (snprintf(out, out_len, "%s\\misrc\\updates", base) >= (int)out_len) {
        out[0] = '\0';
        return false;
    }
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
#if defined(__APPLE__)
    if (snprintf(out, out_len, "%s/Library/Application Support/misrc/updates", home) >= (int)out_len) {
        out[0] = '\0';
        return false;
    }
#else
    if (snprintf(out, out_len, "%s/.local/share/misrc/updates", home) >= (int)out_len) {
        out[0] = '\0';
        return false;
    }
#endif
#endif
    if (!gui_updater_ensure_dir(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool gui_updater_build_archive_path(const char *target_path, const char *current_version,
                                    char *out, size_t out_len)
{
    if (!target_path || !target_path[0] || !current_version || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return false;
    }

    /* Sanitize the version into a filename-safe archive suffix component. */
    char safe_version[64];
    size_t n = 0;
    for (const char *p = current_version; *p && n + 1 < sizeof(safe_version); p++) {
        unsigned char c = (unsigned char)*p;
        safe_version[n++] = (isalnum(c) || c == '.' || c == '-' || c == '_') ? (char)c : '-';
    }
    safe_version[n] = '\0';

    int written = snprintf(out, out_len, "%s%s%s",
                           target_path, GUI_UPDATER_ARCHIVE_SUFFIX, safe_version);
    if (written <= 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* First regular file directly inside dir (release zips/DMG payloads hold
 * exactly one entry; the macOS payload dir is named explicitly). */
static bool gui_updater_first_file_in_dir(const char *dir, char *out, size_t out_len)
{
    if (!dir || !out || out_len == 0) return false;
    out[0] = '\0';
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char candidate[GUI_UPDATER_PATH_MAX];
        if (snprintf(candidate, sizeof(candidate), "%s%s%s",
                     dir,
                     (dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\') ? "" : "/",
                     ent->d_name) >= (int)sizeof(candidate)) {
            continue;
        }
        if (gui_updater_path_is_file(candidate)) {
            if (strlen(candidate) >= out_len) continue;
            snprintf(out, out_len, "%s", candidate);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

#if defined(__APPLE__)
/* macOS: the app bundle root ("<bundle>/Contents/MacOS/<exe>" -> "<bundle>"). */
static bool gui_updater_macos_app_root(const char *exe_path, char *out, size_t out_len)
{
    if (!exe_path || !out || out_len == 0) return false;
    const char *marker = "/Contents/MacOS/";
    const char *pos = strstr(exe_path, marker);
    if (pos) {
        size_t root_len = (size_t)(pos - exe_path);
        if (root_len == 0 || root_len >= out_len) {
            out[0] = '\0';
            return false;
        }
        memcpy(out, exe_path, root_len);
        out[root_len] = '\0';
        return true;
    }
    /* Bare binary (dev build): archive/replace the binary itself. */
    if (strlen(exe_path) >= out_len) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, out_len, "%s", exe_path);
    return true;
}
#endif /* __APPLE__ */

bool gui_updater_install_asset(const char *asset_url, const char *asset_name,
                               const char *current_version,
                               char *status_out, size_t status_len,
                               char *archive_path_out, size_t archive_path_len)
{
    if (status_out && status_len > 0) status_out[0] = '\0';
    if (archive_path_out && archive_path_len > 0) archive_path_out[0] = '\0';
#if defined(__ANDROID__)
    (void)asset_url; (void)asset_name; (void)current_version;
    if (status_out) snprintf(status_out, status_len, "self-update unsupported on Android");
    return false;
#else
    if (!asset_url || !asset_url[0] || !asset_name || !asset_name[0] ||
        !current_version || !current_version[0]) {
        if (status_out) snprintf(status_out, status_len, "invalid update asset");
        return false;
    }

    /* 1. Where the current binary/app lives (the replace target). */
    char exe_path[GUI_UPDATER_PATH_MAX];
    if (!gui_updater_get_executable_path(exe_path, sizeof(exe_path))) {
        if (status_out) snprintf(status_out, status_len, "unable to resolve the running app path");
        return false;
    }

    /* 2. Download into the per-platform updates dir. */
    char updates_dir[GUI_UPDATER_PATH_MAX];
    if (!gui_updater_get_updates_dir(updates_dir, sizeof(updates_dir))) {
        if (status_out) snprintf(status_out, status_len, "unable to create the updates directory");
        return false;
    }

    char download_path[GUI_UPDATER_PATH_MAX];
    if (snprintf(download_path, sizeof(download_path), "%s%s%s",
                 updates_dir,
                 (updates_dir[strlen(updates_dir) - 1] == '/' ||
                  updates_dir[strlen(updates_dir) - 1] == '\\') ? "" : "/",
                 asset_name) >= (int)sizeof(download_path)) {
        if (status_out) snprintf(status_out, status_len, "download path too long");
        return false;
    }

    char cmd[GUI_UPDATER_PATH_MAX + GUI_UPDATER_URL_MAX + 64];
    if (snprintf(cmd, sizeof(cmd), "%s -fsSL --max-time 900 -o \"%s\" \"%s\"",
                 GUI_UPDATER_CURL_EXE, download_path, asset_url) >= (int)sizeof(cmd)) {
        if (status_out) snprintf(status_out, status_len, "download command too long");
        return false;
    }
    int rc = gui_updater_run_command(cmd, NULL, 0);
    if (rc != 0 || !gui_updater_path_is_file(download_path)) {
        if (status_out) {
            snprintf(status_out, status_len,
                     "update download failed (curl rc=%d); use the Releases Page button",
                     rc);
        }
        return false;
    }

    /* 3. Extract the payload into <updates>/payload. */
    char payload_dir[GUI_UPDATER_PATH_MAX];
    char staged[GUI_UPDATER_PATH_MAX];
    staged[0] = '\0';
#if defined(__APPLE__)
    {
        char mount_dir[GUI_UPDATER_PATH_MAX];
        if (snprintf(mount_dir, sizeof(mount_dir), "%s/dmgmount", updates_dir) >= (int)sizeof(mount_dir) ||
            snprintf(payload_dir, sizeof(payload_dir), "%s/payload", updates_dir) >= (int)sizeof(payload_dir)) {
            if (status_out) snprintf(status_out, status_len, "payload path too long (download kept at %s)", download_path);
            return false;
        }
        if (!gui_updater_ensure_dir(mount_dir) || !gui_updater_ensure_dir(payload_dir)) {
            if (status_out) snprintf(status_out, status_len, "unable to create payload dirs (download kept at %s)", download_path);
            return false;
        }
        char mount_cmd[GUI_UPDATER_PATH_MAX * 2 + 64];
        if (snprintf(mount_cmd, sizeof(mount_cmd),
                     "hdiutil attach -nobrowse -readonly -mountpoint \"%s\" \"%s\"",
                     mount_dir, download_path) >= (int)sizeof(mount_cmd)) {
            if (status_out) snprintf(status_out, status_len, "mount command too long (download kept at %s)", download_path);
            return false;
        }
        rc = gui_updater_run_command(mount_cmd, NULL, 0);
        if (rc != 0) {
            if (status_out) snprintf(status_out, status_len, "unable to mount the downloaded DMG (kept at %s)", download_path);
            return false;
        }
        char app_src[GUI_UPDATER_PATH_MAX];
        char copy_cmd[GUI_UPDATER_PATH_MAX * 2 + 64];
        char detach_cmd[GUI_UPDATER_PATH_MAX + 64];
        bool detach_ok =
            snprintf(detach_cmd, sizeof(detach_cmd), "hdiutil detach \"%s\"", mount_dir) < (int)sizeof(detach_cmd);
        if (snprintf(app_src, sizeof(app_src), "%s/MISRC.app", mount_dir) >= (int)sizeof(app_src) ||
            snprintf(copy_cmd, sizeof(copy_cmd), "cp -R \"%s\" \"%s/MISRC.app\"",
                     app_src, payload_dir) >= (int)sizeof(copy_cmd)) {
            if (detach_ok) (void)gui_updater_run_command(detach_cmd, NULL, 0);
            if (status_out) snprintf(status_out, status_len, "copy command too long (download kept at %s)", download_path);
            return false;
        }
        int copy_rc = gui_updater_run_command(copy_cmd, NULL, 0);
        if (detach_ok) (void)gui_updater_run_command(detach_cmd, NULL, 0);
        if (copy_rc != 0) {
            if (status_out) snprintf(status_out, status_len, "unable to stage the new app (download kept at %s)", download_path);
            return false;
        }
        if (snprintf(staged, sizeof(staged), "%s/MISRC.app", payload_dir) >= (int)sizeof(staged)) {
            if (status_out) snprintf(status_out, status_len, "staged app path too long");
            return false;
        }
    }
#else
    {
        if (snprintf(payload_dir, sizeof(payload_dir), "%s/payload", updates_dir) >= (int)sizeof(payload_dir)) {
            if (status_out) snprintf(status_out, status_len, "payload path too long (download kept at %s)", download_path);
            return false;
        }
        if (!gui_updater_ensure_dir(payload_dir)) {
            if (status_out) snprintf(status_out, status_len, "unable to create the payload dir (download kept at %s)", download_path);
            return false;
        }
        char extract_cmd[GUI_UPDATER_PATH_MAX * 2 + 64];
#if defined(_WIN32)
        /* Use Windows' ZIP-capable bsdtar, not a GNU tar earlier in PATH. */
        char system_dir[GUI_UPDATER_PATH_MAX];
        UINT system_len = GetSystemDirectoryA(system_dir, sizeof(system_dir));
        if (!system_len || system_len >= sizeof(system_dir)) {
            if (status_out) snprintf(status_out, status_len, "unable to locate Windows tar");
            return false;
        }
        if (snprintf(extract_cmd, sizeof(extract_cmd), "\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"",
                     system_dir, download_path, payload_dir) >= (int)sizeof(extract_cmd))
#else
        /* Linux release zips hold a single AppImage; unzip is the standard tool. */
        if (snprintf(extract_cmd, sizeof(extract_cmd), "unzip -o -j \"%s\" -d \"%s\"",
                     download_path, payload_dir) >= (int)sizeof(extract_cmd))
#endif
        {
            if (status_out) snprintf(status_out, status_len, "extract command too long (download kept at %s)", download_path);
            return false;
        }
        rc = gui_updater_run_command(extract_cmd, NULL, 0);
        if (rc != 0) {
            if (status_out) snprintf(status_out, status_len,
                                     "unable to extract the download (kept at %s); install it manually",
                                     download_path);
            return false;
        }
        if (!gui_updater_first_file_in_dir(payload_dir, staged, sizeof(staged))) {
            if (status_out) snprintf(status_out, status_len,
                                     "extracted payload not found (download kept at %s)",
                                     download_path);
            return false;
        }
    }
#endif

    /* 4. Archive the old binary/app by RENAMING it - never delete. */
    char target[GUI_UPDATER_PATH_MAX];
#if defined(__APPLE__)
    if (!gui_updater_macos_app_root(exe_path, target, sizeof(target))) {
        if (status_out) snprintf(status_out, status_len, "unable to resolve the app bundle path");
        return false;
    }
#else
    if (strlen(exe_path) >= sizeof(target)) {
        if (status_out) snprintf(status_out, status_len, "app path too long");
        return false;
    }
    snprintf(target, sizeof(target), "%s", exe_path);
#endif

    char archive_path[GUI_UPDATER_PATH_MAX];
    if (!gui_updater_build_archive_path(target, current_version, archive_path, sizeof(archive_path))) {
        if (status_out) snprintf(status_out, status_len, "unable to build the archive path for the old version");
        return false;
    }
    if (rename(target, archive_path) != 0) {
        if (status_out) {
            snprintf(status_out, status_len,
                     "unable to archive the old version (rename failed; nothing was changed). "
                     "Download kept at %s",
                     download_path);
        }
        return false;
    }

    /* 5. Move the new payload into place; restore the archive on failure. */
    if (rename(staged, target) != 0) {
        if (rename(archive_path, target) == 0) {
            if (status_out) {
                snprintf(status_out, status_len,
                         "install failed at the final step; the old version was restored. "
                         "Download kept at %s",
                         download_path);
            }
        } else {
            if (status_out) {
                snprintf(status_out, status_len,
                         "install failed AND the old version could not be restored; "
                         "the old version is archived at %s. Download kept at %s",
                         archive_path, download_path);
            }
        }
        return false;
    }
#if !defined(_WIN32)
    if (gui_updater_path_is_file(target)) {
        (void)chmod(target, 0755);
    }
#endif

    if (archive_path_out && archive_path_len > 0) {
        snprintf(archive_path_out, archive_path_len, "%s", archive_path);
    }
    if (status_out) {
        snprintf(status_out, status_len,
                 "Update installed - restart MISRC to run it. "
                 "Old version archived at %s",
                 archive_path);
    }
    return true;
#endif /* !__ANDROID__ */
}
