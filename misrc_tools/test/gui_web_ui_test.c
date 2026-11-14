/* Unit test: browser web UI routing and cross-site request guard. */
#include "../misrc_gui/net/gui_web_ui.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void) {
    /* Routing: explicit /web paths always serve the page. */
    CHECK(gui_web_ui_is_request("GET /web HTTP/1.1\r\nHost: a\r\n\r\n"));
    CHECK(gui_web_ui_is_request("GET /web/ HTTP/1.1\r\nHost: a\r\n\r\n"));
    CHECK(gui_web_ui_is_request("GET /web/index.html?x=1 HTTP/1.1\r\n\r\n"));
    CHECK(!gui_web_ui_is_request("GET /website HTTP/1.1\r\n\r\n"));
    CHECK(!gui_web_ui_is_request("POST /web HTTP/1.1\r\n\r\n"));
    CHECK(!gui_web_ui_is_request("GET /monitor/status HTTP/1.0\r\n\r\n"));
    /* "/" is HTML only for browsers; native clients keep the version text. */
    CHECK(gui_web_ui_is_request("GET / HTTP/1.1\r\nHost: a\r\naccept: text/html,application/xhtml+xml\r\n\r\n"));
    CHECK(!gui_web_ui_is_request("GET / HTTP/1.0\r\n\r\n"));
    CHECK(!gui_web_ui_is_request("GET / HTTP/1.0\r\nAccept: */*\r\n\r\n"));

    /* Header parsing stops at the blank line and trims whitespace. */
    char v[64];
    CHECK(gui_web_header("GET / HTTP/1.1\r\nX-Test:   hello  \r\n\r\n", "x-test", v, sizeof(v)) && !strcmp(v, "hello"));
    CHECK(!gui_web_header("GET / HTTP/1.1\r\n\r\nX-Test: body\r\n", "X-Test", v, sizeof(v)));
    CHECK(!gui_web_header("GET / HTTP/1.1\r\nX-Testing: no\r\n\r\n", "X-Test", v, sizeof(v)));

    /* CSRF guard. Native clients send neither header. */
    CHECK(!gui_web_request_cross_site("GET /monitor/record?on=1 HTTP/1.0\r\nConnection: close\r\n\r\n"));
    CHECK(!gui_web_request_cross_site("GET /monitor/status HTTP/1.1\r\nHost: 10.0.0.5:8080\r\nSec-Fetch-Site: same-origin\r\n\r\n"));
    CHECK(!gui_web_request_cross_site("GET /web HTTP/1.1\r\nHost: 10.0.0.5:8080\r\nSec-Fetch-Site: none\r\n\r\n"));
    CHECK(gui_web_request_cross_site("GET /monitor/record?on=1 HTTP/1.1\r\nHost: 10.0.0.5:8080\r\nSec-Fetch-Site: cross-site\r\n\r\n"));
    CHECK(gui_web_request_cross_site("GET /monitor/record?on=1 HTTP/1.1\r\nHost: 10.0.0.5:8080\r\nSec-Fetch-Site: same-site\r\n\r\n"));
    CHECK(!gui_web_request_cross_site("GET /x HTTP/1.1\r\nHost: 10.0.0.5:8080\r\nOrigin: http://10.0.0.5:8080\r\n\r\n"));
    CHECK(gui_web_request_cross_site("GET /x HTTP/1.1\r\nHost: 10.0.0.5:8080\r\nOrigin: http://evil.example\r\n\r\n"));
    CHECK(gui_web_request_cross_site("GET /x HTTP/1.1\r\nOrigin: http://10.0.0.5:8080\r\n\r\n"));

    /* Embedded asset is present and NUL-terminated. */
    CHECK(gui_web_ui_index_html_len > 1000);
    CHECK(gui_web_ui_index_html[gui_web_ui_index_html_len] == '\0');
    CHECK(strstr(gui_web_ui_index_html, "/monitor/status") != NULL);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("gui_web_ui_test: all checks passed\n");
    return 0;
}
