/*
 * MISRC GUI - DomesdayDuplicator (DdD) Device Support
 *
 * Provides capture support using the DomesdayDuplicator LaserDisc RF sampler.
 * The DdD is a Cypress FX3 + FPGA USB 3.0 device that produces 10-bit unsigned
 * RF samples at 40 MSPS with a 6-bit rolling sequence number for dropout
 * detection.
 *
 * Features:
 * - Direct USB 3.0 bulk transfers via libusb (all platforms)
 * - 40 MSPS single-channel ADC capture
 * - Polarity-compensated 12-bit pack into MISRC's 32-bit packed ringbuffer
 *   format so the existing extract-pad path bit-matches the native DdD
 *   16-bit signed .raw output (ld-decode compatible)
 * - 6-bit sequence-number validation for non-fatal dropout reporting
 *
 * USB identification:
 * - VID 0x1D50 / PID 0x603B (Domesday Duplicator)
 *
 * No firmware upload is required: the FX3 firmware and FPGA bitstream are
 * stored in onboard SPI flash and the device enumerates ready. Data flows
 * automatically once bulk transfers are submitted (GPIF/DMA started on
 * USB SET_CONF); the host only sends vendor command 0xB6 for test-mode
 * configuration before capture.
 */

#ifndef GUI_DDD_H
#define GUI_DDD_H

#ifdef ENABLE_DDD

#include <stdbool.h>
#include <stdint.h>

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// DdD Device Configuration
//-----------------------------------------------------------------------------

#define DDD_SAMPLE_RATE      40000000    // 40 MSPS - matches hsdaoh/FX3/CXADC
#define DDD_BUFFER_SIZE      (1024 * 1024) // 1 MiB USB transfer buffer
#define DDD_TRANSFER_TIMEOUT 1000        // 1 second bulk transfer timeout

// DdD USB identification (Domesday Duplicator)
#define DDD_VID              0x1D50
#define DDD_PID              0x603B

// DdD USB endpoints (from FX3 firmware: CY_FX_EP_CONSUMER = 0x81)
#define DDD_EP_BULK_IN       0x81        // Bulk IN endpoint for ADC data

// DdD vendor commands (from FX3 firmware domDupUSBSetupCB)
#define DDD_CMD_CONFIG       0xB6        // Configuration flags (bit 0 = test mode)
#define DDD_CMD_CONFIG_TEST  0x0001      // wValue bit 0: enable test mode

// DdD sample format: 16-bit word = 10-bit sample + 6-bit sequence number
#define DDD_SAMPLE_MASK      0x3FF       // bits 0-9: 10-bit unsigned ADC sample
#define DDD_SEQ_SHIFT        10          // bits 10-15: 6-bit sequence number
#define DDD_SEQ_MAX          63          // sequence number wraps at 63

// Polarity-compensated 12-bit pack so MISRC extract-pad bit-matches native
// DdD Signed16Bit: (2047 - sample12) << 4 == (sample10 - 0x200) << 6
#define DDD_PACK_12BIT(sample10) ((uint32_t)(4095 - ((uint32_t)(sample10) << 2)))

//-----------------------------------------------------------------------------
// DdD Device Info (for enumeration)
//-----------------------------------------------------------------------------

typedef struct {
    int bus;                              // USB bus number
    int address;                          // USB device address
    char serial[64];                      // Serial number (if available)
    char name[128];                       // Device name
} ddd_device_info_t;

//-----------------------------------------------------------------------------
// DdD Device API
//-----------------------------------------------------------------------------

// Enumerate DdD devices
// Returns number of devices found, fills devices array up to max_devices
int gui_ddd_enumerate(ddd_device_info_t *devices, int max_devices);

// Start DdD capture
// Returns 0 on success, -1 on error
int gui_ddd_start(gui_app_t *app);

// Stop DdD capture
void gui_ddd_stop(gui_app_t *app);

// Check if DdD capture is running
bool gui_ddd_is_running(gui_app_t *app);

// Open DdD device by index
// Returns 0 on success, -1 on error
int gui_ddd_open(gui_app_t *app, int device_index);

// Close DdD device
void gui_ddd_close(gui_app_t *app);

#endif // ENABLE_DDD

#endif // GUI_DDD_H
