#ifndef GUI_MONITOR_SETTINGS_H
#define GUI_MONITOR_SETTINGS_H
#include "../core/gui_app.h"
#include <stdbool.h>
#include <stddef.h>
/* Strict allowlist: recording configuration only; excludes network, local-save,
 * display settings, file system paths and device handles. M-modes only. */
#define MON_SETTINGS_WIRE_CAP 16384
#define MON_SETTINGS_VALUE_CAP 512
#define MON_SETTINGS_KEY_CAP 64
size_t gui_monitor_settings_count(void);
const char *gui_monitor_settings_key(size_t index);
bool gui_monitor_settings_get(const gui_settings_t *settings,size_t index,char *out,size_t cap);
bool gui_monitor_settings_set(gui_settings_t *settings,const char *key,const char *value);
void gui_monitor_settings_copy(gui_settings_t *dst,const gui_settings_t *src);
#endif
