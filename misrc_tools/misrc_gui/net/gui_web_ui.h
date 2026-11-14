/* Browser Monitor & Remote: a self-contained web page served by the
 * unified Server on its existing TCP port. The page uses the same
 * /monitor/... endpoints as the native Monitor & Remote client.
 * No additional listener, no TLS/authentication (trusted LAN only). */
#ifndef GUI_WEB_UI_H
#define GUI_WEB_UI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Embedded page, generated from misrc_gui/web/index.html by
 * scripts/embed_web_ui.py into gui_web_ui_assets.c. */
extern const char gui_web_ui_index_html[];
extern const size_t gui_web_ui_index_html_len;

/* True when the raw request (request line + headers) asks for the web UI:
 * GET /web, /web/, /web/index.html, or GET / from a browser (Accept text/html). */
bool gui_web_ui_is_request(const char *request);

/* Sends the page and returns. Caller closes the socket. When the
 * MISRC_WEB_ROOT environment variable names a directory containing
 * index.html, that file is served instead (development only). */
void gui_web_ui_serve(uintptr_t socket_handle, bool monitor_available);

/* Browser CSRF guard for every control endpoint on the shared port.
 * Native MISRC clients send neither header and are unaffected. Returns true
 * when a browser marks the request cross-site, or when an Origin header is
 * present and does not match the Host header. */
bool gui_web_request_cross_site(const char *request);

/* Case-insensitive header lookup in a raw request. Copies the trimmed value. */
bool gui_web_header(const char *request, const char *name, char *out, size_t cap);

#endif
