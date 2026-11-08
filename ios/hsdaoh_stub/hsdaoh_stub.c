/*
 * hsdaoh iOS stub — permanent no-device implementation
 * Copyright (C) 2026 — MISRC GUI ios-support
 *
 * iOS has no libusb backend and no public USB host API for the MS213x
 * sticks, so real hsdaoh capture is impossible on this platform. The
 * vendored hsdaoh public headers (third_party/hsdaoh/include) are
 * libusb-free, which lets this stub provide the same ABI surface so
 * misrc_gui/input/gui_capture.c compiles and links completely unchanged:
 * it enumerates zero devices and every open/start returns an error, so
 * the GUI device dropdown simply never offers an MS2130 on iOS.
 *
 * Compiled by ios/build-deps-ios.sh against the vendored headers with the
 * iOS clang and archived as libhsdaoh.a + hsdaoh.pc in each iOS prefix.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <hsdaoh.h>
#include <hsdaoh_raw.h>
#include <stddef.h>

uint32_t hsdaoh_get_device_count(void)
{
    return 0;  /* no USB capture devices exist on iOS */
}

const char *hsdaoh_get_device_name(uint32_t index)
{
    (void)index;
    return NULL;
}

int hsdaoh_get_device_usb_strings(uint32_t index, char *manufact,
                                  char *product, char *serial)
{
    (void)index; (void)manufact; (void)product; (void)serial;
    return -1;
}

int hsdaoh_get_index_by_serial(const char *serial)
{
    (void)serial;
    return -2;  /* no devices were found at all (header contract) */
}

int hsdaoh_open(hsdaoh_dev_t **dev, uint32_t index)
{
    (void)index;
    if (dev) *dev = NULL;
    return -1;  /* USB capture unsupported on iOS */
}

int hsdaoh_close(hsdaoh_dev_t *dev)
{
    (void)dev;
    return -1;
}

int hsdaoh_get_usb_strings(hsdaoh_dev_t *dev, char *manufact,
                           char *product, char *serial)
{
    (void)dev; (void)manufact; (void)product; (void)serial;
    return -1;
}

void hsdaoh_set_msg_callback(hsdaoh_dev_t *dev, hsdaoh_message_cb_t cb, void *ctx)
{
    (void)dev; (void)cb; (void)ctx;  /* no device -> no messages */
}

int hsdaoh_start_stream(hsdaoh_dev_t *dev, hsdaoh_read_cb_t cb,
                        void *ctx, unsigned int buf_num)
{
    (void)dev; (void)cb; (void)ctx; (void)buf_num;
    return -1;
}

int hsdaoh_stop_stream(hsdaoh_dev_t *dev)
{
    (void)dev;
    return -1;
}

int hsdaoh_write_edid_cmd_data(hsdaoh_dev_t *dev, uint8_t *data, uint8_t len)
{
    (void)dev; (void)data; (void)len;
    return -1;
}

/* hsdaoh_raw.h API */

int hsdaoh_check_idle_cnt(uint16_t *idle_cnt, uint16_t *buf, size_t length)
{
    (void)idle_cnt; (void)buf; (void)length;
    return -1;
}

void hsdaoh_raw_callback(hsdaoh_dev_t *dev, bool raw_cb)
{
    (void)dev; (void)raw_cb;
}
