/*
* simple_capture Android stub
* Copyright (C) 2025 — MISRC GUI android-support
*
* Android has no V4L2 (Linux), MediaFoundation (Windows), or AVFoundation
* (macOS). CXADC / V4L2 device capture is out of scope for the basic Android
* release, which ships simulated-device only. This stub provides the sc_*
* symbols declared in simple_capture.h so common/device_enum.c and
* misrc_gui/input/gui_cxadc.c link cleanly; it enumerates zero devices and
* refuses to start a capture.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*/

#include "simple_capture.h"
#include <stddef.h>

char* sc_get_impl_name(void) {
    /* Non-const char* to match the header declaration (other backends do too). */
    return (char *)"android-stub";
}

char* sc_get_impl_name_short(void) {
    return (char *)"android";
}

size_t sc_get_devices(sc_capture_dev_t **dev_list) {
    if (dev_list) *dev_list = NULL;
    return 0;
}

size_t sc_get_formats(char* device_id, sc_formatlist_t **fmt_list) {
    (void)device_id;
    if (fmt_list) *fmt_list = NULL;
    return 0;
}

int sc_start_capture(const char* device_id, uint32_t width, uint32_t height,
                     sc_codec_t codec, uint32_t fps_num, uint32_t fps_den,
                     sc_frame_callback_t cb, void* cb_ctx,
                     sc_handle_t** out_handle) {
    (void)device_id; (void)width; (void)height; (void)codec;
    (void)fps_num; (void)fps_den; (void)cb; (void)cb_ctx;
    if (out_handle) *out_handle = NULL;
    return -1;  /* no capture devices on Android in this build */
}

void sc_stop_capture(sc_handle_t *handle) {
    (void)handle;
}
