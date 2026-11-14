/* Browser Monitor & Remote page server. See gui_web_ui.h. */
#include "gui_web_ui.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <winsock2.h>
typedef SOCKET web_sock_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
typedef int web_sock_t;
#endif

#define WEB_DEV_MAX_BYTES (4u * 1024u * 1024u)

/* Portable ASCII case-insensitive compare (no strcasecmp on all targets). */
static int web_ncaseeq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}
static int web_caseeq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    return la == lb && web_ncaseeq(a, b, la);
}

static int web_send(web_sock_t s, const char *data, size_t n) {
    while (n) {
#ifdef _WIN32
        int k = send(s, data, n > 65536 ? 65536 : (int)n, 0);
#else
        ssize_t k = send(s, data, n, 0);
#endif
        if (k <= 0) return -1;
        data += (size_t)k;
        n -= (size_t)k;
    }
    return 0;
}

bool gui_web_header(const char *request, const char *name, char *out, size_t cap) {
    if (!request || !name || !out || !cap) return false;
    size_t nlen = strlen(name);
    const char *line = strstr(request, "\r\n");
    while (line) {
        line += 2;
        if (line[0] == '\r' || line[0] == '\0') break; /* end of headers */
        const char *end = strstr(line, "\r\n");
        if (!end) end = line + strlen(line);
        if ((size_t)(end - line) > nlen && line[nlen] == ':' &&
            web_ncaseeq(line, name, nlen)) {
            const char *v = line + nlen + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(end - v);
            while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;
            if (vlen >= cap) return false;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        line = *end ? end : NULL;
    }
    return false;
}

static bool web_path_is(const char *request, const char *path) {
    size_t n = strlen(path);
    if (strncmp(request, "GET ", 4) != 0) return false;
    const char *p = request + 4;
    if (strncmp(p, path, n) != 0) return false;
    return p[n] == ' ' || p[n] == '?';
}

bool gui_web_ui_is_request(const char *request) {
    if (!request) return false;
    if (web_path_is(request, "/web") || web_path_is(request, "/web/") ||
        web_path_is(request, "/web/index.html")) return true;
    if (web_path_is(request, "/")) {
        /* Native clients fetch "/" for the version string; only browsers
         * advertise HTML. Keep the legacy text response for everyone else. */
        char accept[512];
        return gui_web_header(request, "Accept", accept, sizeof(accept)) &&
               strstr(accept, "text/html") != NULL;
    }
    return false;
}

bool gui_web_request_cross_site(const char *request) {
    char v[256];
    if (gui_web_header(request, "Sec-Fetch-Site", v, sizeof(v))) {
        /* "none" = typed into the address bar / bookmark. */
        if (!web_caseeq(v, "same-origin") && !web_caseeq(v, "none"))
            return true;
    }
    if (gui_web_header(request, "Origin", v, sizeof(v))) {
        char host[256];
        if (!gui_web_header(request, "Host", host, sizeof(host))) return true;
        const char *o = strstr(v, "://");
        o = o ? o + 3 : v;
        if (!web_caseeq(o, host)) return true;
    }
    return false;
}

static char *web_read_dev_file(size_t *len) {
    const char *root = getenv("MISRC_WEB_ROOT");
    if (!root || !*root) return NULL;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/index.html", root);
    if (n <= 0 || (size_t)n >= sizeof(path)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && (unsigned long)size <= WEB_DEV_MAX_BYTES && fseek(f, 0, SEEK_SET) == 0) {
            buf = (char *)malloc((size_t)size);
            if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) *len = (size_t)size;
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

void gui_web_ui_serve(uintptr_t socket_handle, bool monitor_available) {
    web_sock_t fd = (web_sock_t)socket_handle;
    if (!monitor_available) {
        static const char msg[] =
            "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
            "Connection: close\r\n\r\nMISRC web UI is available only in Server mode.\n";
        (void)web_send(fd, msg, sizeof(msg) - 1);
        return;
    }
    size_t len = gui_web_ui_index_html_len;
    char *dev = web_read_dev_file(&len);
    const char *body = dev ? dev : gui_web_ui_index_html;
    if (!dev) len = gui_web_ui_index_html_len;
    char hdr[768];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; "
        "media-src 'self' blob:; worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'none'\r\n"
        "Connection: close\r\n\r\n", len);
    if (hn > 0 && (size_t)hn < sizeof(hdr) && web_send(fd, hdr, (size_t)hn) == 0)
        (void)web_send(fd, body, len);
    free(dev);
}
