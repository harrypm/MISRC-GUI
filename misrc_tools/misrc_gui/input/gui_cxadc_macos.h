/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GUI_CXADC_MACOS_H
#define GUI_CXADC_MACOS_H
#ifdef __APPLE__
#include "cxadc_client.h"
#include <stdbool.h>
struct gui_app;
int gui_cxadc_macos_detect(void);
const char *gui_cxadc_macos_name(int index);
uint64_t gui_cxadc_macos_registry_id(int index);
void gui_cxadc_macos_select(int index);
int gui_cxadc_macos_get(const char *parameter, int *value);
int gui_cxadc_macos_set(const char *parameter, int value);
int gui_cxadc_macos_open(int card);
int gui_cxadc_macos_read(int card, unsigned char *bytes, size_t length);
void gui_cxadc_macos_close(int card);
const char *gui_cxadc_macos_error(void);
bool gui_cxadc_macos_active(void);
bool gui_cxadc_macos_failed(void);
void gui_cxadc_macos_mark_failed(void);
bool gui_cxadc_macos_ui_handle(struct gui_app *app);
void gui_cxadc_macos_ui_draw(struct gui_app *app);
void gui_cxadc_macos_log(struct gui_app *app);
#endif
#endif
