/* Monitor logical session health: keep user-established sessions until the user
 * explicitly disconnects, while reporting request and preview dropouts.
 * This state is owned exclusively by the M-Client network worker. */
#ifndef GUI_MONITOR_SESSION_STATE_H
#define GUI_MONITOR_SESSION_STATE_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

typedef struct gui_monitor_session_state {
    bool established;
    bool preview_failed;
    unsigned consecutive_status_errors;
    uint32_t status_errors;
    uint32_t preview_errors;
} gui_monitor_session_state_t;

static inline bool gui_monitor_session_status_ok(gui_monitor_session_state_t *s) {
    bool recovered=s->established && s->consecutive_status_errors!=0;
    s->established=true;
    s->consecutive_status_errors=0;
    return recovered;
}
static inline bool gui_monitor_session_status_failed(gui_monitor_session_state_t *s) {
    if(s->consecutive_status_errors<UINT_MAX)++s->consecutive_status_errors;
    if(s->status_errors<UINT32_MAX)++s->status_errors;
    return s->consecutive_status_errors==1;
}
static inline bool gui_monitor_session_preview_ok(gui_monitor_session_state_t *s) {
    bool recovered=s->preview_failed;
    s->preview_failed=false;
    return recovered;
}
static inline bool gui_monitor_session_preview_failed(gui_monitor_session_state_t *s) {
    bool first=!s->preview_failed;
    s->preview_failed=true;
    if(s->preview_errors<UINT32_MAX)++s->preview_errors;
    return first;
}
#endif
