/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GUI_CXADC_MACOS_H
#define GUI_CXADC_MACOS_H
#ifdef __APPLE__
#include "cxadc_client.h"
#include <stdbool.h>
struct gui_app;
int gui_cxadc_macos_detect(void);
bool gui_cxadc_macos_clockgen_available(void);
uint32_t gui_cxadc_macos_rate(void);
void gui_cxadc_macos_profile(bool clockgen);
int gui_cxadc_macos_get_card(int card,const char *name,int *value);
int gui_cxadc_macos_set_card(int card,const char *name,int value);
int gui_cxadc_macos_prepare(int count, bool clockgen);
int gui_cxadc_macos_start_cards(void);
int gui_cxadc_macos_audio_open(uint32_t *rate);
int gui_cxadc_macos_audio_start(void);
int gui_cxadc_macos_audio_read(void *bytes, size_t capacity_frames, size_t *frames);
void gui_cxadc_macos_audio_close(void);
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
bool gui_cxadc_macos_clockgen_active(void);
bool gui_cxadc_macos_failed(void);
void gui_cxadc_macos_mark_failed(void);
bool gui_cxadc_macos_ui_handle(struct gui_app *app);
void gui_cxadc_macos_ui_draw(struct gui_app *app);
void gui_cxadc_macos_log(struct gui_app *app);
#endif
#endif
