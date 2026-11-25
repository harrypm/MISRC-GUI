#include "gui_source_meta.h"

// GUI_SOURCE_META_FILE_ONLY: the file rule alone (unit test, no raylib).
#ifndef GUI_SOURCE_META_FILE_ONLY
#include "../core/gui_app.h"
#endif
#include "../../common/file_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool meta_ext_is(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/'), *bslash = strrchr(path, '\\');
    if (!dot || (slash && dot < slash) || (bslash && dot < bslash)) return false;
    size_t n = strlen(ext);
    if (strlen(dot) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)dot[i]) != tolower((unsigned char)ext[i])) return false;
    return true;
}

// Read up to cap bytes of a text file (NUL-terminated); NULL if absent.
static char *meta_read_text(const char *path, size_t cap) {
    FILE *f = file_open_utf8(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static bool meta_file_contains(const char *path, const char *a, const char *b) {
    char *text = meta_read_text(path, 256u * 1024u);
    if (!text) return false;
    bool hit = strstr(text, a) && (!b || strstr(text, b));
    free(text);
    return hit;
}

// End of the last "_YYYY.MM.DD_HH.MM.SS" stamp in name (MISRC-GUI naming),
// or NULL.
static const char *meta_timestamp_end(const char *name) {
    static const char pat[] = "_dddd.dd.dd_dd.dd.dd";
    const size_t plen = sizeof(pat) - 1;
    const char *found = NULL;
    for (const char *p = name; strlen(p) >= plen; p++) {
        size_t i = 0;
        for (; i < plen; i++) {
            const char c = p[i];
            if (pat[i] == 'd' ? !isdigit((unsigned char)c) : c != pat[i]) break;
        }
        if (i == plen) found = p + plen;
    }
    return found;
}

bool gui_source_file_is_ddd(const char *path) {
    if (!path || !path[0]) return false;
    if (meta_ext_is(path, ".lds") || meta_ext_is(path, ".ldf")) return true;

    const size_t len = strlen(path);
    if (len + 64 >= FILE_PATH_MAX) return false;
    char probe[FILE_PATH_MAX];

    // DdD application sidecar: <capture>.json with its captureInfo block.
    memcpy(probe, path, len + 1);
    char *dot = strrchr(probe, '.');
    char *sep = strrchr(probe, '/');
    char *bsep = strrchr(probe, '\\');
    if (bsep > sep) sep = bsep;
    if (dot && (!sep || dot > sep)) {
        strcpy(dot, ".json");
        if (meta_file_contains(probe, "\"captureInfo\"", "\"transferCount\"")) return true;
    }

    // MISRC-GUI capture log of the same take.
    const char *name = sep ? path + (sep - probe) + 1 : path;
    const char *stamp_end = meta_timestamp_end(name);
    if (stamp_end) {
        const size_t prefix = (size_t)(stamp_end - path);
        for (int seq = 0; seq <= 3; seq++) {
            int n = seq ? snprintf(probe, sizeof(probe), "%.*s_misrc_capture-%d.log", (int)prefix, path, seq)
                        : snprintf(probe, sizeof(probe), "%.*s_misrc_capture.log", (int)prefix, path);
            if (n < 0 || (size_t)n >= sizeof(probe)) break;
            if (meta_file_contains(probe, "capture_device_type: ddd", NULL)) return true;
        }
    }
    return false;
}

#ifndef GUI_SOURCE_META_FILE_ONLY
bool gui_source_is_ddd(gui_app_t *app, int channel) {
    if (!app || channel < 0 || channel > 1) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    const device_type_t type = app->devices[app->selected_device].type;
#ifdef ENABLE_DDD
    if (type == DEVICE_TYPE_DDD) return true;
#endif
    if (type != DEVICE_TYPE_PLAYBACK) return false;
    static char s_path[2][FILE_PATH_MAX];
    static bool s_ddd[2], s_valid[2];
    const char *path = channel ? app->settings.playback_file_b : app->settings.playback_file_a;
    if (!s_valid[channel] || strcmp(path, s_path[channel]) != 0) {
        snprintf(s_path[channel], sizeof(s_path[channel]), "%s", path);
        s_ddd[channel] = gui_source_file_is_ddd(path);
        s_valid[channel] = true;
    }
    return s_ddd[channel];
}
#endif
