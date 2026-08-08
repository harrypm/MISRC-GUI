/*
 * MISRC GUI - DomesdayDuplicator (DdD) Device Support
 *
 * Provides capture support using the DomesdayDuplicator LaserDisc RF sampler.
 * Uses libusb for USB communication on all platforms (the DdD native app uses
 * libusb, not cyusb).
 *
 * Data flow:
 *   USB bulk IN (EP 0x81) -> 16-bit words (10-bit sample + 6-bit seq)
 *   -> polarity-compensated 12-bit pack -> 32-bit packed ringbuffer
 *   -> existing MISRC extract + FLAC record pipeline
 *
 * The 32-bit packed format written to BUF_CAPTURE_RF matches the hsdaoh/FX3
 * layout: bits 0-11 = channel A (12-bit), bits 12-19 = AUX (0), bits 20-31
 * = channel B (0, DdD is single-channel). The polarity-compensated pack
 * (4095 - (sample10 << 2)) ensures MISRC's extract-pad path produces 16-bit
 * signed output that bit-matches the native DdD .raw format for ld-decode
 * compatibility.
 */

#ifdef ENABLE_DDD

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

// Include libusb BEFORE raylib/gui headers to avoid Windows header conflicts
// (raylib redefines CloseWindow, Rectangle, etc. — same guard as gui_fx3.c)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include "../../common/libusb_compat.h"
#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#undef NOUSER
#else
#include "../../common/libusb_compat.h"
#endif

#include "gui_ddd.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

//-----------------------------------------------------------------------------
// DdD USB Constants
//-----------------------------------------------------------------------------

// USB control transfer timeout for vendor commands
#define DDD_CTRL_TIMEOUT     1000

//-----------------------------------------------------------------------------
// DdD Device State
//-----------------------------------------------------------------------------

static libusb_context *s_ddd_ctx = NULL;
static libusb_device_handle *s_ddd_handle = NULL;
static int s_ddd_interface = 0;
static uint8_t s_ddd_bulk_ep = DDD_EP_BULK_IN;
static atomic_bool s_ddd_transfer_ready = false;

typedef struct ddd_stream_path {
    int interface_number;
    int alternate_setting;
    uint8_t endpoint_address;
    uint16_t max_packet_size;
    bool found;
} ddd_stream_path_t;

// Sequence-number validation state (capture thread only). The DdD sequence
// number is constant for 65536 samples then advances by 1 (mod 64). We only
// report a dropout when the sequence skips a value or jumps backward — a
// normal +1 advance after 65536 samples is expected, not an error.
static uint32_t s_ddd_last_seq = 0;
static bool s_ddd_seq_synced = false;

//-----------------------------------------------------------------------------
// DdD USB Context Management
//-----------------------------------------------------------------------------

static int gui_ddd_usb_init(void) {
    if (s_ddd_ctx) return 0;
#if LIBUSB_API_VERSION >= 0x0100010A
    return libusb_init_context(&s_ddd_ctx, NULL, 0);
#else
    return libusb_init(&s_ddd_ctx);
#endif
}

static void gui_ddd_usb_exit(void) {
    if (s_ddd_ctx) {
        libusb_exit(s_ddd_ctx);
        s_ddd_ctx = NULL;
    }
}

static ddd_stream_path_t gui_ddd_find_stream_path(libusb_device *device) {
    ddd_stream_path_t best = {
        .interface_number = 0,
        .alternate_setting = 0,
        .endpoint_address = DDD_EP_BULK_IN,
        .max_packet_size = 0,
        .found = false,
    };

    struct libusb_config_descriptor *config = NULL;
    int r = libusb_get_active_config_descriptor(device, &config);
    if (r != 0 || !config) {
        r = libusb_get_config_descriptor(device, 0, &config);
        if (r != 0 || !config) {
            return best;
        }
    }

    for (int if_i = 0; if_i < config->bNumInterfaces; if_i++) {
        const struct libusb_interface *iface = &config->interface[if_i];
        for (int alt = 0; alt < iface->num_altsetting; alt++) {
            const struct libusb_interface_descriptor *id = &iface->altsetting[alt];
            for (int ep = 0; ep < id->bNumEndpoints; ep++) {
                const struct libusb_endpoint_descriptor *ed = &id->endpoint[ep];
                bool is_bulk = (ed->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK;
                bool is_in = (ed->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
                if (!is_bulk || !is_in) continue;

                if (!best.found || ed->wMaxPacketSize > best.max_packet_size) {
                    best.interface_number = id->bInterfaceNumber;
                    best.alternate_setting = id->bAlternateSetting;
                    best.endpoint_address = ed->bEndpointAddress;
                    best.max_packet_size = ed->wMaxPacketSize;
                    best.found = true;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    return best;
}

//-----------------------------------------------------------------------------
// DdD Vendor Commands
//-----------------------------------------------------------------------------

// Send configuration command (0xB6) to set test mode on/off.
// Bit 0 of wValue = test mode. Data flows automatically once bulk transfers
// are submitted; this command only configures the FPGA test-mode GPIO.
static int gui_ddd_send_config_command(bool test_mode) {
    if (!s_ddd_handle) return -1;

    uint16_t wValue = test_mode ? DDD_CMD_CONFIG_TEST : 0x0000;
    int ret = libusb_control_transfer(s_ddd_handle,
        0x40,  // LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT
        DDD_CMD_CONFIG,
        wValue, 0x0000,
        NULL, 0,
        DDD_CTRL_TIMEOUT);

    if (ret < 0) {
        fprintf(stderr, "[DdD] Failed to send config command: %s\n",
                libusb_error_name(ret));
        return -1;
    }

    fprintf(stderr, "[DdD] Config command sent (test_mode=%d)\n", test_mode);
    return 0;
}

//-----------------------------------------------------------------------------
// DdD Device Enumeration
//-----------------------------------------------------------------------------

int gui_ddd_enumerate(ddd_device_info_t *devices, int max_devices) {
    int count = 0;

    if (gui_ddd_usb_init() != 0) {
        fprintf(stderr, "[DdD] Failed to initialize libusb for enumeration\n");
        return 0;
    }

    libusb_device **devlist;
    ssize_t num_devices = libusb_get_device_list(s_ddd_ctx, &devlist);
    if (num_devices < 0) {
        fprintf(stderr, "[DdD] Failed to get device list: %s\n",
                libusb_error_name((int)num_devices));
        gui_ddd_usb_exit();
        return 0;
    }

    for (ssize_t i = 0; i < num_devices && count < max_devices; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devlist[i], &desc) != 0) continue;

        if (desc.idVendor == DDD_VID && desc.idProduct == DDD_PID) {
            devices[count].bus = libusb_get_bus_number(devlist[i]);
            devices[count].address = libusb_get_device_address(devlist[i]);
            devices[count].serial[0] = '\0';

            // Try to get serial number string
            libusb_device_handle *tmp_handle = NULL;
            if (libusb_open(devlist[i], &tmp_handle) == 0 && tmp_handle) {
                if (desc.iSerialNumber) {
                    unsigned char serial[64];
                    if (libusb_get_string_descriptor_ascii(tmp_handle,
                            desc.iSerialNumber, serial, sizeof(serial)) > 0) {
                        snprintf(devices[count].serial,
                                 sizeof(devices[count].serial), "%s", serial);
                    }
                }
                libusb_close(tmp_handle);
            }

            snprintf(devices[count].name, sizeof(devices[count].name),
                     "Domesday Duplicator");
            count++;
        }
    }

    libusb_free_device_list(devlist, 1);
    gui_ddd_usb_exit();
    return count;
}

//-----------------------------------------------------------------------------
// DdD Device Open/Close
//-----------------------------------------------------------------------------

int gui_ddd_open(gui_app_t *app, int device_index) {

    if (gui_ddd_usb_init() != 0) {
        fprintf(stderr, "[DdD] Failed to initialize libusb\n");
        return -1;
    }

    libusb_device **devlist;
    ssize_t num_devices = libusb_get_device_list(s_ddd_ctx, &devlist);
    if (num_devices < 0) {
        fprintf(stderr, "[DdD] Failed to get device list: %s\n",
                libusb_error_name((int)num_devices));
        gui_ddd_usb_exit();
        return -1;
    }

    // Find the DdD device by index (counting only DdD VID/PID matches)
    int ddd_count = 0;
    for (ssize_t i = 0; i < num_devices; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devlist[i], &desc) != 0) continue;

        if (desc.idVendor != DDD_VID || desc.idProduct != DDD_PID) continue;

        if (ddd_count != device_index) {
            ddd_count++;
            continue;
        }

        // Found the target device — open it
        int r = libusb_open(devlist[i], &s_ddd_handle);
        if (r < 0) {
            fprintf(stderr, "[DdD] Failed to open device: %s\n",
                    libusb_error_name(r));
            if (r == LIBUSB_ERROR_ACCESS) {
#if defined(_WIN32)
                fprintf(stderr, "[DdD] Access denied. Install/bind a WinUSB/libusbK "
                                "driver for the DdD device interface.\n");
                gui_app_set_status(app, "DdD access denied (install WinUSB/libusbK driver)");
#elif defined(__APPLE__)
                fprintf(stderr, "[DdD] Access denied. On macOS, run with appropriate "
                                "permissions and allow USB device access.\n");
                gui_app_set_status(app, "DdD access denied (check macOS USB permissions)");
#else
                fprintf(stderr, "[DdD] Access denied. Check udev/device permissions.\n");
                gui_app_set_status(app, "DdD access denied (check USB permissions)");
#endif
            } else {
                gui_app_set_status(app, "Failed to open DdD device");
            }
            libusb_free_device_list(devlist, 1);
            gui_ddd_usb_exit();
            return -1;
        }

        fprintf(stderr, "[DdD] Opened device index %d (VID=%04X PID=%04X)\n",
                device_index, desc.idVendor, desc.idProduct);

        // Verify USB speed is at least high-speed (DdD requires USB 3.0,
        // but we accept high-speed as a minimum to avoid hard failure on
        // USB 2.0 ports — the native app also warns on non-SuperSpeed).
        enum libusb_speed speed = libusb_get_device_speed(devlist[i]);
        if (speed < LIBUSB_SPEED_HIGH) {
            fprintf(stderr, "[DdD] WARNING: Device connected at less than "
                    "high-speed; capture may fail. Connect to a USB 3.0 port.\n");
        } else if (speed < LIBUSB_SPEED_SUPER) {
            fprintf(stderr, "[DdD] WARNING: Device connected at high-speed, "
                    "not SuperSpeed. DdD requires USB 3.0 for full 40 MSPS.\n");
        }
        // Determine the streaming interface/endpoint from descriptors
        ddd_stream_path_t stream_path = gui_ddd_find_stream_path(devlist[i]);
        s_ddd_interface = stream_path.interface_number;
        s_ddd_bulk_ep = stream_path.endpoint_address;
        if (stream_path.found) {
            fprintf(stderr, "[DdD] Using interface %d alt %d, bulk IN endpoint 0x%02X "
                            "(max packet %u)\n",
                    stream_path.interface_number,
                    stream_path.alternate_setting,
                    stream_path.endpoint_address,
                    stream_path.max_packet_size);
        } else {
            fprintf(stderr, "[DdD] WARNING: Could not auto-discover stream path; "
                            "using defaults interface %d, endpoint 0x%02X\n",
                    s_ddd_interface, s_ddd_bulk_ep);
        }

#if LIBUSB_API_VERSION >= 0x01000106
        // Let libusb auto-detach where supported (Linux). Non-fatal elsewhere.
        r = libusb_set_auto_detach_kernel_driver(s_ddd_handle, 1);
        if (r < 0 && r != LIBUSB_ERROR_NOT_SUPPORTED) {
            fprintf(stderr, "[DdD] Warning: auto-detach request failed: %s\n",
                    libusb_error_name(r));
        }
#endif

        // Detach kernel driver if active
        r = libusb_kernel_driver_active(s_ddd_handle, s_ddd_interface);
        if (r == 1) {
            fprintf(stderr, "[DdD] Detaching kernel driver from interface %d\n",
                    s_ddd_interface);
            int detach_r = libusb_detach_kernel_driver(s_ddd_handle, s_ddd_interface);
            if (detach_r < 0 && detach_r != LIBUSB_ERROR_NOT_SUPPORTED) {
                fprintf(stderr, "[DdD] Warning: detach kernel driver failed: %s\n",
                        libusb_error_name(detach_r));
            }
        } else if (r < 0 && r != LIBUSB_ERROR_NOT_SUPPORTED) {
            fprintf(stderr, "[DdD] Warning: kernel-driver query failed on interface %d: %s\n",
                    s_ddd_interface, libusb_error_name(r));
        }

        // Set configuration
        r = libusb_set_configuration(s_ddd_handle, 1);
        if (r < 0 && r != LIBUSB_ERROR_BUSY && r != LIBUSB_ERROR_NOT_SUPPORTED) {
            fprintf(stderr, "[DdD] Warning: Failed to set configuration 1: %s\n",
                    libusb_error_name(r));
        } else {
            fprintf(stderr, "[DdD] Set configuration 1\n");
        }

        // Claim interface
        r = libusb_claim_interface(s_ddd_handle, s_ddd_interface);
        if (r < 0) {
            fprintf(stderr, "[DdD] Failed to claim interface %d: %s\n",
                    s_ddd_interface, libusb_error_name(r));
            libusb_close(s_ddd_handle);
            s_ddd_handle = NULL;
            libusb_free_device_list(devlist, 1);
            gui_ddd_usb_exit();
            return -1;
        }
        fprintf(stderr, "[DdD] Claimed interface %d\n", s_ddd_interface);
        if (stream_path.found && stream_path.alternate_setting != 0) {
            r = libusb_set_interface_alt_setting(s_ddd_handle,
                                                 s_ddd_interface,
                                                 stream_path.alternate_setting);
            if (r < 0) {
                fprintf(stderr, "[DdD] Failed to set interface %d alt %d: %s\n",
                        s_ddd_interface,
                        stream_path.alternate_setting,
                        libusb_error_name(r));
                libusb_release_interface(s_ddd_handle, s_ddd_interface);
                libusb_close(s_ddd_handle);
                s_ddd_handle = NULL;
                libusb_free_device_list(devlist, 1);
                gui_ddd_usb_exit();
                return -1;
            }
            fprintf(stderr, "[DdD] Activated alternate setting %d\n",
                    stream_path.alternate_setting);
        }

        libusb_free_device_list(devlist, 1);
        return 0;
    }

    fprintf(stderr, "[DdD] Device index %d not found (found %d DdD devices)\n",
            device_index, ddd_count);
    libusb_free_device_list(devlist, 1);
    gui_ddd_usb_exit();
    return -1;
}

void gui_ddd_close(gui_app_t *app) {
    (void)app;

    if (s_ddd_handle) {
        libusb_release_interface(s_ddd_handle, s_ddd_interface);
        libusb_close(s_ddd_handle);
        s_ddd_handle = NULL;
    }
    s_ddd_interface = 0;
    s_ddd_bulk_ep = DDD_EP_BULK_IN;
    gui_ddd_usb_exit();
}

//-----------------------------------------------------------------------------
// DdD Capture Thread
//-----------------------------------------------------------------------------

static int ddd_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    // Allocate USB transfer buffer (raw 16-bit DdD words)
    uint8_t *transfer_buf = (uint8_t *)malloc(DDD_BUFFER_SIZE);
    if (!transfer_buf) {
        fprintf(stderr, "[DdD] Failed to allocate transfer buffer\n");
        return -1;
    }

    fprintf(stderr, "[DdD] Capture thread started at %d MSPS (EP 0x%02X)\n",
            DDD_SAMPLE_RATE / 1000000, s_ddd_bulk_ep);

    // Clear any stale data from the endpoint
    libusb_clear_halt(s_ddd_handle, s_ddd_bulk_ep);

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, DDD_SAMPLE_RATE);

    // Reset sequence-number validation state
    s_ddd_seq_synced = false;
    s_ddd_last_seq = 0;

    uint64_t batch_count = 0;
    uint64_t timeout_count = 0;
    int actual_length = 0;

    // Signal that we're ready for transfers
    atomic_store(&s_ddd_transfer_ready, true);

    while (atomic_load(&app->ddd_running)) {
        int r = libusb_bulk_transfer(s_ddd_handle, s_ddd_bulk_ep,
                                      transfer_buf, DDD_BUFFER_SIZE,
                                      &actual_length, DDD_TRANSFER_TIMEOUT);

        if (r < 0) {
            if (r == LIBUSB_ERROR_TIMEOUT) {
                timeout_count++;
                if (timeout_count <= 3) {
                    fprintf(stderr, "[DdD] Bulk transfer timeout #%llu (no data)\n",
                            (unsigned long long)timeout_count);
                }
                continue;
            }
            fprintf(stderr, "[DdD] Bulk transfer error: %s (%d)\n",
                    libusb_error_name(r), r);
            gui_app_count_system_errors(app, 1);
            continue;
        }

        if (actual_length == 0) {
            continue;
        }

        if (batch_count == 0) {
            fprintf(stderr, "[DdD] First data received: %d bytes\n", actual_length);
        }

        // DdD data is 16-bit words. Each word -> one 32-bit packed sample.
        // So output_bytes = (actual_length / 2) * 4 = actual_length * 2.
        size_t num_words = (size_t)actual_length / 2;
        size_t output_bytes = num_words * sizeof(uint32_t);

        uint8_t *buf_out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                               output_bytes, NULL);
        if (buf_out) {
            uint32_t *packed_out = (uint32_t *)buf_out;
            const uint16_t *words_in = (const uint16_t *)transfer_buf;

            for (size_t i = 0; i < num_words; i++) {
                uint16_t word = words_in[i];
                uint32_t sample10 = (uint32_t)(word & DDD_SAMPLE_MASK);
                uint32_t seq = (uint32_t)(word >> DDD_SEQ_SHIFT);

                // Sequence-number validation (non-fatal dropout detection).
                // The DdD sequence number is constant for 65536 samples then
                // advances by 1 (mod 64). Only flag an error when the sequence
                // skips a value or jumps backward — a normal +1 advance is the
                // expected transition, not a dropout. This matches the native
                // DdD app's intent (detect dropped USB data) without spamming
                // errors on every per-sample check.
                if (!s_ddd_seq_synced) {
                    s_ddd_last_seq = seq;
                    s_ddd_seq_synced = true;
                } else if (seq != s_ddd_last_seq) {
                    uint32_t expected_next = (s_ddd_last_seq + 1) & DDD_SEQ_MAX;
                    if (seq != expected_next) {
                        // Sequence skipped a value or jumped backward — USB
                        // data was dropped. Report as missed frame + error
                        // (tolerated, not fatal, per MISRC AGENTS.MD philosophy).
                        atomic_fetch_add(&app->missed_frame_count, 1);
                        atomic_fetch_add(&app->error_count, 1);
                    }
                    // Resync to the received sequence number either way
                    s_ddd_last_seq = seq;
                }

                // Polarity-compensated 12-bit pack into 32-bit packed format.
                // Channel A only; AUX=0, channel B=0 (DdD is single-channel).
                packed_out[i] = DDD_PACK_12BIT(sample10);
            }

            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, output_bytes);
            bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);
        } else {
            // Buffer full — drop data
            atomic_fetch_add(&app->rb_drop_count, 1);
            if (atomic_load(&app->rb_drop_count) <= 5) {
                fprintf(stderr, "[DdD] Warning: BUF_CAPTURE_RF full, data dropped\n");
            }
        }

        // Update statistics (one DdD word = one sample)
        atomic_fetch_add(&app->total_samples, num_words);
        atomic_fetch_add(&app->samples_a, num_words);
        atomic_store(&app->last_callback_time_ms, get_time_ms());

        batch_count++;
    }

    fprintf(stderr, "[DdD] Capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(transfer_buf);
    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_ddd_start(gui_app_t *app) {
    fprintf(stderr, "[DdD] Starting DdD capture\n");

    // Send configuration command (test mode off) before starting transfers.
    // The DdD data flows automatically once bulk transfers are submitted;
    // this command configures the FPGA test-mode GPIO.
    if (gui_ddd_send_config_command(false) != 0) {
        fprintf(stderr, "[DdD] Warning: Could not send config command (continuing)\n");
    }

    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    // Reset statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, DDD_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    atomic_store(&s_ddd_transfer_ready, false);

    // Reset display buffers
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Ensure capture buffer is initialized
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[DdD] Failed to initialize capture ringbuffer\n");
        gui_app_set_status(app, "Failed to initialize capture buffer");
        return -1;
    }

    // Start extraction thread - reads from BUF_CAPTURE_RF, writes to BUF_DISPLAY
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[DdD] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        return -1;
    }

    // Start display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[DdD] Failed to start display thread (non-fatal)\n");
        }
    }

    // Set capture state
    atomic_store(&app->ddd_running, true);
    app->is_capturing = true;

    // Start DdD capture thread
    thrd_t thread;
    if (thrd_create_with_priority(&thread,
                                  ddd_capture_thread,
                                  app,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[DdD] Failed to create capture thread\n");
        gui_app_set_status(app, "Failed to create capture thread");
        gui_extract_stop();
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        atomic_store(&app->ddd_running, false);
        app->is_capturing = false;
        return -1;
    }
    app->ddd_thread = (void *)(uintptr_t)thread;

    // Wait for capture thread to signal ready
    bool transfer_ready = false;
    for (int i = 0; i < 100; i++) {
        if (atomic_load(&s_ddd_transfer_ready)) {
            fprintf(stderr, "[DdD] Capture thread ready after %d ms\n", i * 10);
            transfer_ready = true;
            break;
        }
        thrd_sleep_ms(10);
    }
    if (!transfer_ready) {
        fprintf(stderr, "[DdD] Warning: capture thread did not signal readiness "
                        "within 1000 ms (continuing)\n");
    }

    gui_app_set_status(app, "DdD capture running");
    return 0;
}

void gui_ddd_stop(gui_app_t *app) {
    if (!atomic_load(&app->ddd_running)) return;

    fprintf(stderr, "[DdD] Stopping DdD capture\n");

    atomic_store(&s_ddd_transfer_ready, false);
    app->is_capturing = false;
    atomic_store(&app->ddd_running, false);

    // Stop capture thread
    if (app->ddd_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->ddd_thread;
        thrd_join(thread, NULL);
        app->ddd_thread = NULL;
    }

    // Stop display thread
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop extraction thread
    gui_extract_stop();

    // Close USB device
    gui_ddd_close(app);

    atomic_store(&app->stream_synced, false);

    gui_app_set_status(app, "DdD capture stopped");
}

bool gui_ddd_is_running(gui_app_t *app) {
    return atomic_load(&app->ddd_running);
}

#endif // ENABLE_DDD
