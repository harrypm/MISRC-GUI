/*
 * MISRC GUI - RTL-SDR (RTL2832) Device Support
 *
 * Provides capture support for RTL-SDR USB receivers via librtlsdr.
 *
 * The RTL-SDR delivers unsigned 8-bit interleaved I/Q. The backend packs
 * each I,Q pair into the existing hsdaoh 32-bit capture format (I -> channel A,
 * Q -> channel B, 8-bit values shifted into the 12-bit field) and feeds
 * BUF_CAPTURE_RF, so the existing extraction/display/record pipeline runs
 * unchanged. This is the raw-data path; demodulation is a separate panel.
 *
 * Requires librtlsdr (rtl-sdr) + libusb-1.0 (already linked for FX3/DdD).
 */

#ifndef GUI_RTLSDR_H
#define GUI_RTLSDR_H

#ifdef ENABLE_RTLSDR

#include <stdbool.h>
#include <stdint.h>

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// RTL-SDR Device Info (for enumeration)
//-----------------------------------------------------------------------------

typedef struct {
    int  index;                 // rtl-sdr device index
    char name[128];             // product name (rtl-sdr name)
    char serial[64];            // serial string (if available)
    bool blog_v4;               // exact USB manufacturer/product match
} rtlsdr_device_info_t;

//-----------------------------------------------------------------------------
// RTL-SDR Device API
//-----------------------------------------------------------------------------

// Enumerate RTL-SDR devices.
// Returns number of devices found, fills devices[] up to max_devices.
int gui_rtlsdr_enumerate(rtlsdr_device_info_t *devices, int max_devices);

// Open the RTL-SDR device by index and apply settings (freq/gain/rate/agc/offset).
// Returns 0 on success, -1 on open error, or GUI_RTLSDR_VGA_FAILED.
#define GUI_RTLSDR_VGA_FAILED (-2)
int gui_rtlsdr_open(gui_app_t *app, int device_index);

// Close the RTL-SDR device.
void gui_rtlsdr_close(gui_app_t *app);

// Start RTL-SDR capture (launches extraction + display + capture threads).
// Returns 0 on success, -1 on error.
int gui_rtlsdr_start(gui_app_t *app);

// Stop RTL-SDR capture.
void gui_rtlsdr_stop(gui_app_t *app);

// True while the RTL-SDR capture thread is running.
bool gui_rtlsdr_is_running(gui_app_t *app);

// Read the successfully programmed rate and settled tuning generation/center for RF recording.
bool gui_rtlsdr_get_rf_source(const gui_app_t *app, uint32_t *rate, uint64_t *tuning);

// Live-retune the open RTL-SDR to hz (also updates settings.rtlsdr_freq_hz).
// Safe to call whether or not capture is running; returns 0 on success.
int gui_rtlsdr_set_frequency(gui_app_t *app, uint64_t hz);

#endif // ENABLE_RTLSDR

#endif // GUI_RTLSDR_H
