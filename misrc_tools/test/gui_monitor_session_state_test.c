/* Offline, deterministic transport-state tests. The logical session does not
 * disappear on HTTP/preview request failures; only user stop tears it down. */
#include "../misrc_gui/net/gui_monitor_session_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

int main(void) {
    gui_monitor_session_state_t s={0};
    assert(!s.established && s.status_errors==0 && !s.preview_failed);

    /* Initially unreachable: don't claim a successful connection. */
    assert(gui_monitor_session_status_failed(&s));
    assert(!s.established && s.status_errors==1);
    assert(!gui_monitor_session_status_failed(&s));
    assert(s.status_errors==2 && s.consecutive_status_errors==2);
    assert(!gui_monitor_session_status_ok(&s)); /* First connection, not recovery */
    assert(s.established && s.consecutive_status_errors==0);

    /* Wi-Fi outage: keep the established logical session. */
    assert(gui_monitor_session_status_failed(&s));
    assert(s.established && s.consecutive_status_errors==1);
    assert(!gui_monitor_session_status_failed(&s));
    assert(s.established && s.status_errors==4);
    assert(gui_monitor_session_status_ok(&s));
    assert(s.established && s.consecutive_status_errors==0);
    assert(!gui_monitor_session_status_ok(&s)); /* Clean follow-up tick. */

    /* A preview timeout is a separate client-side error. */
    assert(gui_monitor_session_preview_failed(&s));
    assert(s.preview_failed && s.preview_errors==1);
    assert(!gui_monitor_session_preview_failed(&s));
    assert(s.preview_errors==2 && s.established);
    assert(gui_monitor_session_preview_ok(&s));
    assert(!s.preview_failed && s.preview_errors==2);
    assert(!gui_monitor_session_preview_ok(&s));

    /* The worker creates a new session only on explicit Connect/Stop. */
    gui_monitor_session_state_t fresh={0};
    assert(!fresh.established && fresh.status_errors==0 && fresh.preview_errors==0);
    assert(gui_monitor_session_status_failed(&fresh));
    assert(!fresh.established);

    /* Avoid counter wraparound during extremely long running sessions. */
    fresh.status_errors=UINT32_MAX;
    gui_monitor_session_status_failed(&fresh);
    assert(fresh.status_errors==UINT32_MAX);
    fresh.preview_errors=UINT32_MAX;
    gui_monitor_session_preview_failed(&fresh);
    assert(fresh.preview_errors==UINT32_MAX);
    puts("Monitor session-state regression tests passed");
    return 0;
}
