/*
 * MISRC Common - Device Enumeration Implementation
 */

#include "device_enum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hsdaoh.h>
#include "../misrc_capture/simple_capture/simple_capture.h"

#ifdef ENABLE_FX3
#ifdef __linux__
#include <cyusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

// FX3 USB VID/PID
// #define FX3_VID              0x04B4
// #define FX3_PID_BOOTLOADER   0x00F3
// #define FX3_PID_MISRC        0x00F1
// #endif


// FX3 USB VID/PID
#define FX3_VID              0x04B4
#define FX3_PID_BOOTLOADER   0x00F3
#define FX3_PID_MISRC        0x1234
#endif


/*-----------------------------------------------------------------------------
 * Device List Management
 *-----------------------------------------------------------------------------*/

void misrc_device_list_init(misrc_device_list_t *list)
{
    list->devices = NULL;
    list->count = 0;
    list->capacity = 0;
}

void misrc_device_list_free(misrc_device_list_t *list)
{
    if (list->devices) {
        free(list->devices);
        list->devices = NULL;
    }
    list->count = 0;
    list->capacity = 0;
}

static bool device_list_grow(misrc_device_list_t *list)
{
    size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
    misrc_device_info_t *new_devices = realloc(list->devices,
                                                new_capacity * sizeof(misrc_device_info_t));
    if (!new_devices) {
        return false;
    }
    list->devices = new_devices;
    list->capacity = new_capacity;
    return true;
}

static misrc_device_info_t *device_list_add(misrc_device_list_t *list)
{
    if (list->count >= list->capacity) {
        if (!device_list_grow(list)) {
            return NULL;
        }
    }
    misrc_device_info_t *dev = &list->devices[list->count];
    memset(dev, 0, sizeof(*dev));
    list->count++;
    return dev;
}

/*-----------------------------------------------------------------------------
 * Device Enumeration
 *-----------------------------------------------------------------------------*/

int misrc_device_enumerate(misrc_device_list_t *list, bool include_hsdaoh, bool include_simple_capture)
{
    /* Clear existing list */
    misrc_device_list_free(list);
    misrc_device_list_init(list);

    /* Enumerate hsdaoh devices */
    if (include_hsdaoh) {
        uint32_t hs_count = hsdaoh_get_device_count();
        for (uint32_t i = 0; i < hs_count; i++) {
            misrc_device_info_t *dev = device_list_add(list);
            if (!dev) {
                return -1;
            }
            dev->type = MISRC_DEVICE_TYPE_HSDAOH;
            dev->index = (int)i;
            const char *name = hsdaoh_get_device_name(i);
            snprintf(dev->name, sizeof(dev->name), "%s", name ? name : "Unknown");
            dev->device_id[0] = '\0';
            dev->supports_1080p60 = true;  /* hsdaoh devices always support this */
        }
    }

    /* Enumerate simple_capture devices */
    if (include_simple_capture) {
        sc_capture_dev_t *sc_devs;
        size_t sc_count = sc_get_devices(&sc_devs);

        for (size_t i = 0; i < sc_count; i++) {
            /* Check if device supports 1920x1080 YUYV at >=40fps */
            bool supports_1080p60 = false;
            sc_formatlist_t *sc_fmt;
            size_t f_count = sc_get_formats(sc_devs[i].device_id, &sc_fmt);

            for (size_t j = 0; j < f_count && !supports_1080p60; j++) {
                if (!SC_CODEC_EQUAL(sc_fmt[j].codec, SC_CODEC_YUYV)) {
                    continue;
                }
                for (size_t k = 0; k < sc_fmt[j].n_sizes && !supports_1080p60; k++) {
                    if (sc_fmt[j].sizes[k].w != 1920 || sc_fmt[j].sizes[k].h != 1080) {
                        continue;
                    }
                    for (size_t l = 0; l < sc_fmt[j].sizes[k].n_fps; l++) {
                        sc_fps_t *fps = &sc_fmt[j].sizes[k].fps[l];
                        float fps_val;
                        if (fps->den != 0) {
                            fps_val = (float)fps->num / (float)fps->den;
                        } else {
                            fps_val = (float)fps->max_num / (float)fps->max_den;
                        }
                        if (fps_val >= 40.0f) {
                            supports_1080p60 = true;
                            break;
                        }
                    }
                }
            }

            /* Only add devices that support the required format */
            if (supports_1080p60) {
                misrc_device_info_t *dev = device_list_add(list);
                if (!dev) {
                    return -1;
                }
                dev->type = MISRC_DEVICE_TYPE_SIMPLE_CAPTURE;
                dev->index = -1;  /* Not used for simple_capture */
                snprintf(dev->name, sizeof(dev->name), "%s", sc_devs[i].name);
                snprintf(dev->device_id, sizeof(dev->device_id), "%s", sc_devs[i].device_id);
                dev->supports_1080p60 = true;
            }
        }
    }

    return (int)list->count;
}

#ifdef ENABLE_FX3
int misrc_device_enumerate_fx3(misrc_device_list_t *list, bool include_hsdaoh,
                                bool include_simple_capture, bool include_fx3)
{
    /* First enumerate non-FX3 devices */
    int result = misrc_device_enumerate(list, include_hsdaoh, include_simple_capture);
    if (result < 0) {
        return result;
    }

    if (!include_fx3) {
        return (int)list->count;
    }

    /* Enumerate FX3 devices */
#ifdef __linux__
    int num_devices = cyusb_open();
    if (num_devices < 0) {
        /* cyusb init failed, but we still have other devices */
        return (int)list->count;
    }

    for (int i = 0; i < num_devices; i++) {
        cyusb_handle *h = cyusb_gethandle(i);
        if (!h) continue;

        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(libusb_get_device(h), &desc) == 0) {
            if (desc.idVendor == FX3_VID &&
                (desc.idProduct == FX3_PID_MISRC || desc.idProduct == FX3_PID_BOOTLOADER)) {

                misrc_device_info_t *dev = device_list_add(list);
                if (!dev) {
                    cyusb_close();
                    return -1;
                }

                dev->type = MISRC_DEVICE_TYPE_FX3;
                dev->index = i;

                int bus = libusb_get_bus_number(libusb_get_device(h));
                int addr = libusb_get_device_address(libusb_get_device(h));
                snprintf(dev->name, sizeof(dev->name),
                         "Cypress FX3 (Bus %d, Addr %d)", bus, addr);

                /* Try to get serial number */
                dev->device_id[0] = '\0';
                if (desc.iSerialNumber) {
                    unsigned char serial[64];
                    if (libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
                                                           serial, sizeof(serial)) > 0) {
                        snprintf(dev->device_id, sizeof(dev->device_id), "%s", serial);
                    }
                }

                dev->supports_1080p60 = false;  /* N/A for FX3 */
            }
        }
    }

    cyusb_close();
#else
    /* Other platforms: use libusb */
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        return (int)list->count;
    }

    libusb_device **devlist;
    ssize_t num_devices = libusb_get_device_list(ctx, &devlist);

    int fx3_index = 0;
    for (ssize_t i = 0; i < num_devices; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devlist[i], &desc) == 0) {
            if (desc.idVendor == FX3_VID &&
                (desc.idProduct == FX3_PID_MISRC || desc.idProduct == FX3_PID_BOOTLOADER)) {

                misrc_device_info_t *dev = device_list_add(list);
                if (!dev) {
                    libusb_free_device_list(devlist, 1);
                    libusb_exit(ctx);
                    return -1;
                }

                dev->type = MISRC_DEVICE_TYPE_FX3;
                dev->index = fx3_index++;

                int bus = libusb_get_bus_number(devlist[i]);
                int addr = libusb_get_device_address(devlist[i]);
                snprintf(dev->name, sizeof(dev->name),
                         "Cypress FX3 (Bus %d, Addr %d)", bus, addr);

                dev->device_id[0] = '\0';
                dev->supports_1080p60 = false;  /* N/A for FX3 */
            }
        }
    }

    libusb_free_device_list(devlist, 1);
    libusb_exit(ctx);
#endif

    return (int)list->count;
}
#endif /* ENABLE_FX3 */

const char *device_get_simple_capture_name(void)
{
    return sc_get_impl_name();
}

const char *device_get_simple_capture_short_name(void)
{
    return sc_get_impl_name_short();
}

/*-----------------------------------------------------------------------------
 * Device List Output
 *-----------------------------------------------------------------------------*/

void misrc_device_list_print(const misrc_device_list_t *list)
{
    /* Group by type for CLI output */
    fprintf(stderr, "Devices that can be used using libusb / libuvc / libhsdaoh:\n");
    for (size_t i = 0; i < list->count; i++) {
        if (list->devices[i].type == MISRC_DEVICE_TYPE_HSDAOH) {
            fprintf(stderr, " %d: %s\n", list->devices[i].index, list->devices[i].name);
        }
    }

    fprintf(stderr, "\nDevices that can be used using %s:\n", sc_get_impl_name());
    for (size_t i = 0; i < list->count; i++) {
        if (list->devices[i].type == MISRC_DEVICE_TYPE_SIMPLE_CAPTURE) {
            fprintf(stderr, " %s://%s: %s\n",
                    sc_get_impl_name_short(),
                    list->devices[i].device_id,
                    list->devices[i].name);
        }
    }

    fprintf(stderr, "\nDevice names can change when devices are connected/disconnected!\n"
                    "Using %s requires that the device does not modify the video data.\n\n",
            sc_get_impl_name_short());
}
