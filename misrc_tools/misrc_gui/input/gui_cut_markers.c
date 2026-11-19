/*
 * MISRC GUI - playback cut markers (IN / OUT) for FLAC-Chop
 * See gui_cut_markers.h.
 */

#include "gui_cut_markers.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CUT_PATH_MAX 1024

typedef struct {
    gui_cut_marks_t marks;
    char path[CUT_PATH_MAX];   // file the marks belong to
    bool preset;               // --markers: adopt the first file shown
} cut_channel_t;

static cut_channel_t s_cut[2];

static cut_channel_t *cut_channel(int ch) {
    return &s_cut[ch == 1 ? 1 : 0];
}

gui_cut_marks_t gui_cut_marks_get(int ch, const char *path) {
    cut_channel_t *c = cut_channel(ch);
    gui_cut_marks_t none = {0};
    if (!path || !path[0]) return none;
    if (c->preset) {
        snprintf(c->path, sizeof(c->path), "%s", path);
        c->preset = false;
    }
    if (strcmp(c->path, path) != 0) return none;
    return c->marks;
}

gui_cut_set_result_t gui_cut_marks_set(int ch, const char *path, bool out_mark, uint64_t pos) {
    cut_channel_t *c = cut_channel(ch);
    if (!path || !path[0]) return GUI_CUT_SET_OK;
    if (c->preset || strcmp(c->path, path) != 0) {
        memset(&c->marks, 0, sizeof(c->marks));
        snprintf(c->path, sizeof(c->path), "%s", path);
        c->preset = false;
    }
    gui_cut_set_result_t r = GUI_CUT_SET_OK;
    if (out_mark) {
        if (c->marks.has_in && pos <= c->marks.in) {
            c->marks.has_in = false;
            r = GUI_CUT_SET_DROPPED_OTHER;
        }
        c->marks.has_out = true;
        c->marks.out = pos;
    } else {
        if (c->marks.has_out && pos >= c->marks.out) {
            c->marks.has_out = false;
            r = GUI_CUT_SET_DROPPED_OTHER;
        }
        c->marks.has_in = true;
        c->marks.in = pos;
    }
    return r;
}

void gui_cut_marks_clear(int ch) {
    cut_channel_t *c = cut_channel(ch);
    memset(&c->marks, 0, sizeof(c->marks));
    c->preset = false;
}

void gui_cut_marks_preset(int ch, uint64_t in, uint64_t out) {
    cut_channel_t *c = cut_channel(ch);
    memset(c, 0, sizeof(*c));
    c->marks.has_in = in != UINT64_MAX;
    c->marks.in = c->marks.has_in ? in : 0;
    c->marks.has_out = out != UINT64_MAX;
    c->marks.out = c->marks.has_out ? out : 0;
    if (c->marks.has_in && c->marks.has_out && c->marks.out <= c->marks.in) c->marks.has_out = false;
    c->preset = true;
}

// Append src quoted for the host shell; false when it does not fit.
static bool cut_append_quoted(char *buf, size_t size, size_t *len, const char *src) {
#ifdef _WIN32
    // Windows paths cannot contain '"'.
    int n = snprintf(buf + *len, size - *len, "\"%s\"", src);
    if (n < 0 || (size_t)n >= size - *len) return false;
    *len += (size_t)n;
    return true;
#else
    // POSIX: '...' with each ' written as '\''.
    if (*len + 1 >= size) return false;
    buf[(*len)++] = '\'';
    for (const char *p = src; *p; p++) {
        const char *piece = *p == '\'' ? "'\\''" : NULL;
        size_t pn = piece ? 4 : 1;
        if (*len + pn + 1 >= size) return false;
        if (piece) {
            memcpy(buf + *len, piece, pn);
        } else {
            buf[*len] = *p;
        }
        *len += pn;
    }
    if (*len + 2 > size) return false;
    buf[(*len)++] = '\'';
    buf[*len] = '\0';
    return true;
#endif
}

bool gui_cut_marks_flac_chop_command(const gui_cut_marks_t *m, const char *path,
                                     char *buf, size_t size) {
    if (!buf || size == 0) return false;
    buf[0] = '\0';
    if (!path || !path[0]) return false;
    size_t len = 0;
    int n = snprintf(buf, size, "flac-chop --gui ");
    if (n < 0 || (size_t)n >= size) return false;
    len = (size_t)n;
    if (!cut_append_quoted(buf, size, &len, path)) return false;
    if (m && m->has_in) {
        n = snprintf(buf + len, size - len, " --in %" PRIu64, m->in);
        if (n < 0 || (size_t)n >= size - len) return false;
        len += (size_t)n;
    }
    if (m && m->has_out) {
        n = snprintf(buf + len, size - len, " --out %" PRIu64, m->out);
        if (n < 0 || (size_t)n >= size - len) return false;
        len += (size_t)n;
    }
    n = snprintf(buf + len, size - len, " --units samples");
    return n >= 0 && (size_t)n < size - len;
}
