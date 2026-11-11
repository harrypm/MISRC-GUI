/*
 * MISRC GUI - Cypress FX3 Device Support
 *
 * Provides capture support using Cypress FX3 USB 3.0 devices.
 * Uses libcyusb for USB communication on Linux, or libusb on other platforms.
 */

#ifdef ENABLE_FX3

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

// Include the appropriate USB library BEFORE raylib/gui headers to avoid
// Windows header conflicts (raylib redefines CloseWindow, Rectangle, etc.)
#ifdef __linux__
#include <cyusb.h>
#else
// On Windows, avoid windows.h conflicts with raylib
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#endif
#include "../../common/libusb_compat.h"
#if defined(_WIN32)
#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#undef NOUSER
#endif
#endif

#include "gui_fx3.h"
#include "../output/gui_record.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

//-----------------------------------------------------------------------------
// FX3 USB Constants
//-----------------------------------------------------------------------------

// Cypress FX3 USB VID/PID for MISRC firmware
#define FX3_VID              0x04B4      // Cypress VID
#define FX3_PID_BOOTLOADER   0x00F3      // FX3 bootloader mode
#define FX3_PID_FX3USBADC    0x00F1      // fx3usbadc runtime firmware
#define FX3_PID_MISRC        0x1234      // MISRC custom firmware (adjust as needed)
// fx3usbadc endpoint for data streaming
#define FX3USBADC_EP_BULK_IN 0x81

// USB transfer parameters
#define FX3_TRANSFER_SIZE    (FX3_BUFFER_SIZE * 4)  // 4 bytes per sample (32-bit packed)
#define FX3_TRANSFER_TIMEOUT 1000                    // 1 second timeout
#define FX3_CTRL_TIMEOUT     1000                    // Control transfer timeout (ms) — must be >=1s for fx3usbadc 0x91 start

// fx3usbadc start command (0x91):
// wValue = gain control word [0:HighGain(1):GainCode(7)], default 0x94.
// wIndex = [bit15 sample16][bit14 external_clock][bits7:0 clkConfig].
// Use discrete external-clock clkConfig settings and keep auto-fallback
// with no user-side mode control.
#define FX3USBADC_CMD_START                  0x91
#define FX3USBADC_DEFAULT_GAIN_CONTROL_WORD  ((uint16_t)0x94)
#define FX3USBADC_START_WINDEX_BASE          ((uint16_t)0xC000)  // 16-bit sample + external clock
#define FX3USBADC_START_WINDEX_FROM_CLKCFG(c) ((uint16_t)(FX3USBADC_START_WINDEX_BASE | ((uint16_t)(c) & 0x00FF)))
#define FX3USBADC_DEFAULT_CLK_CONFIG         ((uint8_t)16)       // 100 MHz
// DdD-compatible single-channel 12-bit pack (channel A in bits 11:0, AUX/B zero).
#define FX3_DDD_PACK_12BIT(sample10_u) ((uint32_t)(4095u - ((((uint32_t)(sample10_u)) & 0x03FFu) << 2)))

//-----------------------------------------------------------------------------
// Platform-specific USB abstraction
//-----------------------------------------------------------------------------

#ifdef __linux__
// Linux uses cyusb library
typedef cyusb_handle *fx3_handle_t;
#define fx3_usb_init()           cyusb_open()
#define fx3_usb_exit()           cyusb_close()
#define fx3_usb_claim(h, i)      cyusb_claim_interface(h, i)
#define fx3_usb_release(h, i)    cyusb_release_interface(h, i)
#define fx3_usb_bulk_transfer    cyusb_bulk_transfer
#else
// Other platforms use libusb directly
typedef libusb_device_handle *fx3_handle_t;
static libusb_context *s_fx3_usb_ctx = NULL;

static int fx3_usb_init(void) {
    return libusb_init(&s_fx3_usb_ctx);
}

static void fx3_usb_exit(void) {
    if (s_fx3_usb_ctx) {
        libusb_exit(s_fx3_usb_ctx);
        s_fx3_usb_ctx = NULL;
    }
}

#define fx3_usb_claim(h, i)      libusb_claim_interface(h, i)
#define fx3_usb_release(h, i)    libusb_release_interface(h, i)
#define fx3_usb_bulk_transfer    libusb_bulk_transfer
#endif

//-----------------------------------------------------------------------------
// FX3 Device State
//-----------------------------------------------------------------------------

static fx3_handle_t s_fx3_handle = NULL;
static int s_fx3_interface = 0;
static atomic_bool s_fx3_transfer_ready = false;  // Set when capture thread is ready for data
typedef enum {
    FX3_PROTOCOL_UNKNOWN = 0,
    FX3_PROTOCOL_SIGROK,
    FX3_PROTOCOL_FX3USBADC,
} fx3_protocol_t;
static fx3_protocol_t s_fx3_protocol = FX3_PROTOCOL_UNKNOWN;
static uint8_t s_fx3_bulk_ep = FX3_EP_BULK_IN;

typedef struct {
    uint8_t clk_config;
    uint16_t start_windex;
    uint32_t nominal_sample_rate_hz; // 0 means firmware mapping is undefined (e.g. clkConfig 5/12)
} fx3usbadc_mode_t;

static const fx3usbadc_mode_t s_fx3usbadc_modes[] = {
    {  1, FX3USBADC_START_WINDEX_FROM_CLKCFG(1),   25000000u },
    {  2, FX3USBADC_START_WINDEX_FROM_CLKCFG(2),   33333333u },
    {  3, FX3USBADC_START_WINDEX_FROM_CLKCFG(3),   37500000u },
    {  4, FX3USBADC_START_WINDEX_FROM_CLKCFG(4),   40000000u },
    {  5, FX3USBADC_START_WINDEX_FROM_CLKCFG(5),          0u },
    {  6, FX3USBADC_START_WINDEX_FROM_CLKCFG(6),   50000000u },
    {  7, FX3USBADC_START_WINDEX_FROM_CLKCFG(7),   58333333u },
    {  8, FX3USBADC_START_WINDEX_FROM_CLKCFG(8),   60000000u },
    {  9, FX3USBADC_START_WINDEX_FROM_CLKCFG(9),   62500000u },
    { 10, FX3USBADC_START_WINDEX_FROM_CLKCFG(10),  66666667u },
    { 11, FX3USBADC_START_WINDEX_FROM_CLKCFG(11),  75000000u },
    { 12, FX3USBADC_START_WINDEX_FROM_CLKCFG(12),         0u },
    { 13, FX3USBADC_START_WINDEX_FROM_CLKCFG(13),  80000000u },
    { 14, FX3USBADC_START_WINDEX_FROM_CLKCFG(14),  83333333u },
    { 15, FX3USBADC_START_WINDEX_FROM_CLKCFG(15),  93750000u },
    { 16, FX3USBADC_START_WINDEX_FROM_CLKCFG(16), 100000000u },
    { 17, FX3USBADC_START_WINDEX_FROM_CLKCFG(17), 125000000u },
    { 18, FX3USBADC_START_WINDEX_FROM_CLKCFG(18), 150000000u },
    { 19, FX3USBADC_START_WINDEX_FROM_CLKCFG(19), 156250000u },
    { 20, FX3USBADC_START_WINDEX_FROM_CLKCFG(20), 158333333u },
};
static size_t s_fx3usbadc_active_mode_index = 0;

static size_t fx3usbadc_mode_count(void)
{
    return sizeof(s_fx3usbadc_modes) / sizeof(s_fx3usbadc_modes[0]);
}
static size_t fx3usbadc_default_mode_index(void)
{
    const size_t mode_count = fx3usbadc_mode_count();
    for (size_t i = 0; i < mode_count; i++) {
        if (s_fx3usbadc_modes[i].clk_config == FX3USBADC_DEFAULT_CLK_CONFIG) {
            return i;
        }
    }
    return 0;
}

static const fx3usbadc_mode_t *fx3usbadc_active_mode(void)
{
    size_t mode_count = fx3usbadc_mode_count();
    if (mode_count == 0) return NULL;
    if (s_fx3usbadc_active_mode_index >= mode_count) {
        s_fx3usbadc_active_mode_index = fx3usbadc_default_mode_index();
    }
    return &s_fx3usbadc_modes[s_fx3usbadc_active_mode_index];
}

static void fx3usbadc_reset_mode(void)
{
    s_fx3usbadc_active_mode_index = fx3usbadc_default_mode_index();
}

static fx3_protocol_t fx3_protocol_from_pid(uint16_t pid)
{
    if (pid == FX3_PID_FX3USBADC) {
        return FX3_PROTOCOL_FX3USBADC;
    }
    return FX3_PROTOCOL_SIGROK;
}

static const char *fx3_protocol_name(fx3_protocol_t protocol)
{
    switch (protocol) {
        case FX3_PROTOCOL_FX3USBADC:
            return "fx3usbadc";
        case FX3_PROTOCOL_SIGROK:
            return "sigrok-compatible";
        default:
            return "unknown";
    }
}

static uint32_t fx3_protocol_sample_rate_hz(fx3_protocol_t protocol)
{
    if (protocol == FX3_PROTOCOL_FX3USBADC) {
        const fx3usbadc_mode_t *mode = fx3usbadc_active_mode();
        if (mode && mode->nominal_sample_rate_hz > 0) {
            return mode->nominal_sample_rate_hz;
        }
        return FX3_SAMPLE_RATE;
    }
    return FX3_SAMPLE_RATE;
}

static uint32_t fx3_active_sample_rate_hz(void)
{
    return fx3_protocol_sample_rate_hz(s_fx3_protocol);
}

static uint32_t fx3usbadc_pack_signed10_from_sample16(int16_t sample16)
{
    // fx3usbadc 16-bit mode packs signed 10-bit samples left-shifted by 6.
    // Convert to 10-bit unsigned, then apply the exact same polarity-
    // compensated 12-bit mapping used by DdD.
    int32_t sample10 = ((int32_t)sample16) >> 6;
    if (sample10 < -512) sample10 = -512;
    if (sample10 > 511) sample10 = 511;
    uint32_t u10 = (uint32_t)(sample10 + 512);
    return FX3_DDD_PACK_12BIT(u10);
}

//-----------------------------------------------------------------------------
// FX3 Vendor Commands (from sigrok cypress-fx3 driver)
//-----------------------------------------------------------------------------

// Structure for start acquisition command (must be packed)
#pragma pack(push, 1)
struct fx3_cmd_start_acquisition_sigrok {
    uint16_t sampling_factor;  // PIB_CLOCK / sample_rate
};
#pragma pack(pop)

// Send vendor command to get firmware version
static int fx3_cmd_get_fw_version(uint8_t *major, uint8_t *minor) {
    if (!s_fx3_handle || s_fx3_protocol != FX3_PROTOCOL_SIGROK) return -1;

    uint8_t version[2] = {0, 0};
    int ret = libusb_control_transfer(s_fx3_handle,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
        FX3_CMD_GET_FW_VERSION,
        0x0000, 0x0000,
        version, sizeof(version),
        FX3_CTRL_TIMEOUT);

    if (ret < 0) {
        fprintf(stderr, "[FX3] Failed to get firmware version: %s\n",
                libusb_error_name(ret));
        return -1;
    }

    if (major) *major = version[0];
    if (minor) *minor = version[1];
    fprintf(stderr, "[FX3] Firmware version: %d.%d\n", version[0], version[1]);
    return 0;
}

// Send vendor command to start acquisition
static int fx3_cmd_start_acquisition_sigrok(uint32_t sample_rate) {
    if (!s_fx3_handle) return -1;

    struct fx3_cmd_start_acquisition_sigrok cmd;
    cmd.sampling_factor = (uint16_t)(FX3_PIB_CLOCK / sample_rate);

    fprintf(stderr, "[FX3] Starting acquisition: sample_rate=%u, sampling_factor=%u\n",
            sample_rate, cmd.sampling_factor);

    int ret = libusb_control_transfer(s_fx3_handle,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
        FX3_CMD_START,
        0x0000, 0x0000,
        (unsigned char *)&cmd, sizeof(cmd),
        FX3_CTRL_TIMEOUT);

    if (ret < 0) {
        fprintf(stderr, "[FX3] Failed to send start command: %s\n",
                libusb_error_name(ret));
        return -1;
    }

    fprintf(stderr, "[FX3] Start command sent successfully\n");
    return 0;
}

// Send fx3usbadc start command (0x91).
static int fx3_cmd_start_acquisition_fx3usbadc(void)
{
    if (!s_fx3_handle) return -1;

    const size_t mode_count = fx3usbadc_mode_count();
    if (mode_count == 0) return -1;
    if (s_fx3usbadc_active_mode_index >= mode_count) {
        s_fx3usbadc_active_mode_index = fx3usbadc_default_mode_index();
    }

    int last_error = 0;
    for (size_t i = 0; i < mode_count; i++) {
        const size_t mode_index = (s_fx3usbadc_active_mode_index + i) % mode_count;
        const fx3usbadc_mode_t *mode = &s_fx3usbadc_modes[mode_index];

        int ret = libusb_control_transfer(s_fx3_handle,
            LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
            FX3USBADC_CMD_START,
            FX3USBADC_DEFAULT_GAIN_CONTROL_WORD,
            mode->start_windex,
            NULL, 0,
            FX3_CTRL_TIMEOUT);

        if (ret >= 0) {
            s_fx3usbadc_active_mode_index = mode_index;
            if (mode->nominal_sample_rate_hz > 0) {
                fprintf(stderr,
                        "[FX3] fx3usbadc start command sent (clk=%u, wValue=0x%04X, wIndex=0x%04X, sample_rate=%u)%c",
                        (unsigned int)mode->clk_config,
                        FX3USBADC_DEFAULT_GAIN_CONTROL_WORD,
                        mode->start_windex,
                        mode->nominal_sample_rate_hz,
                        10);
            } else {
                fprintf(stderr,
                        "[FX3] fx3usbadc start command sent (clk=%u, wValue=0x%04X, wIndex=0x%04X, sample_rate=unknown)%c",
                        (unsigned int)mode->clk_config,
                        FX3USBADC_DEFAULT_GAIN_CONTROL_WORD,
                        mode->start_windex,
                        10);
            }
            return 0;
        }

        last_error = ret;
        fprintf(stderr,
                "[FX3] fx3usbadc start attempt failed (clk=%u, wIndex=0x%04X): %s%c",
                (unsigned int)mode->clk_config,
                mode->start_windex,
                libusb_error_name(ret),
                10);
    }

    fprintf(stderr,
            "[FX3] fx3usbadc start command failed for all auto modes (clk=1..20): %s%c",
            libusb_error_name(last_error),
            10);
    return -1;
}

// Send the active protocol's start-acquisition command.
static int fx3_cmd_start_acquisition(uint32_t sample_rate)
{
    if (s_fx3_protocol == FX3_PROTOCOL_FX3USBADC) {
        return fx3_cmd_start_acquisition_fx3usbadc();
    }
    return fx3_cmd_start_acquisition_sigrok(sample_rate);
}

//-----------------------------------------------------------------------------
// FX3 Firmware Upload (for bootloader mode devices)
//-----------------------------------------------------------------------------

// Helper macros for FX3 firmware address handling
#define GET_LSW(v)  ((uint16_t)((v) & 0xFFFF))
#define GET_MSW(v)  ((uint16_t)((v) >> 16))

// Write data to FX3 RAM via bootloader
static int fx3_ram_write(uint8_t *buf, uint32_t ram_addr, int len) {
    if (!s_fx3_handle) return -1;

    int index = 0;
    while (len > 0) {
        int size = (len < FX3_FW_CHUNK_SIZE) ? len : FX3_FW_CHUNK_SIZE;

        int ret = libusb_control_transfer(s_fx3_handle,
            LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
            FX3_CMD_FW_UPLOAD,
            GET_LSW(ram_addr), GET_MSW(ram_addr),
            &buf[index], size,
            1000);  // 1 second timeout for firmware upload

        if (ret != size) {
            fprintf(stderr, "[FX3] Firmware write failed at 0x%08X: %s\n",
                    ram_addr, libusb_error_name(ret));
            return -1;
        }

        ram_addr += size;
        index += size;
        len -= size;
    }

    return 0;
}

// Upload firmware to FX3 device in bootloader mode
// Firmware format: Cypress FX3 .img format
// - 4 bytes: "CY" magic + 2 byte header
// - Sections: [4-byte length][4-byte address][data...]
// - Final section has length=0, address=entry point
static int fx3_upload_firmware(const char *firmware_path) {
    if (!s_fx3_handle) return -1;

    FILE *fp = fopen(firmware_path, "rb");
    if (!fp) {
        fprintf(stderr, "[FX3] Failed to open firmware file: %s\n", firmware_path);
        return -1;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (filesize < 8 || filesize > 256 * 1024) {
        fprintf(stderr, "[FX3] Invalid firmware file size: %ld\n", filesize);
        fclose(fp);
        return -1;
    }

    // Read entire firmware file
    uint8_t *firmware = (uint8_t *)malloc(filesize);
    if (!firmware) {
        fprintf(stderr, "[FX3] Failed to allocate memory for firmware\n");
        fclose(fp);
        return -1;
    }

    if (fread(firmware, 1, filesize, fp) != (size_t)filesize) {
        fprintf(stderr, "[FX3] Failed to read firmware file\n");
        free(firmware);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // Check magic header "CY"
    if (firmware[0] != 'C' || firmware[1] != 'Y') {
        fprintf(stderr, "[FX3] Invalid firmware magic (expected 'CY', got '%c%c')\n",
                firmware[0], firmware[1]);
        free(firmware);
        return -1;
    }

    fprintf(stderr, "[FX3] Uploading firmware '%s' (%ld bytes)...\n", firmware_path, filesize);

    // Parse and upload firmware sections
    // Format after header: [length:4][address:4][data:length*4]...
    uint32_t checksum = 0;
    int index = 4;  // Skip header

    while (index < filesize) {
        uint32_t *data_p = (uint32_t *)(firmware + index);
        uint32_t length = data_p[0];    // Length in 32-bit words
        uint32_t address = data_p[1];   // RAM address

        if (length != 0) {
            // Calculate checksum
            for (uint32_t i = 0; i < length; i++) {
                checksum += data_p[2 + i];
            }

            // Write section to RAM
            int ret = fx3_ram_write(firmware + index + 8, address, length * 4);
            if (ret != 0) {
                fprintf(stderr, "[FX3] Failed to write firmware section at 0x%08X\n", address);
                free(firmware);
                return -1;
            }

            fprintf(stderr, "[FX3]   Section: addr=0x%08X, len=%u bytes\n", address, length * 4);
        } else {
            // Final section - verify checksum and jump to entry point
            if (checksum != data_p[2]) {
                fprintf(stderr, "[FX3] Firmware checksum mismatch (expected 0x%08X, got 0x%08X)\n",
                        data_p[2], checksum);
                free(firmware);
                return -1;
            }

            fprintf(stderr, "[FX3] Checksum OK, jumping to entry point 0x%08X\n", address);

            // Send zero-length transfer to jump to entry point
            int ret = libusb_control_transfer(s_fx3_handle,
                LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
                FX3_CMD_FW_UPLOAD,
                GET_LSW(address), GET_MSW(address),
                NULL, 0,
                1000);

            // This transfer may return an error since the device reboots
            if (ret < 0) {
                fprintf(stderr, "[FX3] Device rebooting (expected: %s)\n", libusb_error_name(ret));
            }
            break;
        }

        index += (8 + length * 4);
    }

    free(firmware);
    fprintf(stderr, "[FX3] Firmware upload complete, device will re-enumerate\n");
    return 0;
}

//-----------------------------------------------------------------------------
// FX3 Device Enumeration
//-----------------------------------------------------------------------------

int gui_fx3_enumerate(fx3_device_info_t *devices, int max_devices) {
    int count = 0;

#ifdef __linux__
    // Linux: Use cyusb enumeration
    int num_devices = cyusb_open();
    if (num_devices < 0) {
        fprintf(stderr, "[FX3] Failed to initialize cyusb: %d\n", num_devices);
        return 0;
    }

    for (int i = 0; i < num_devices && count < max_devices; i++) {
        cyusb_handle *h = cyusb_gethandle(i);
        if (!h) continue;

        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(libusb_get_device(h), &desc) == 0) {
            // Check for FX3 VID and known PIDs
            if (desc.idVendor == FX3_VID &&
                (desc.idProduct == FX3_PID_MISRC ||
                 desc.idProduct == FX3_PID_FX3USBADC ||
                 desc.idProduct == FX3_PID_BOOTLOADER)) {

                devices[count].bus = libusb_get_bus_number(libusb_get_device(h));
                devices[count].address = libusb_get_device_address(libusb_get_device(h));

                // Try to get serial number
                devices[count].serial[0] = '\0';
                if (desc.iSerialNumber) {
                    unsigned char serial[64];
                    if (libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
                                                           serial, sizeof(serial)) > 0) {
                        snprintf(devices[count].serial, sizeof(devices[count].serial),
                                 "%s", serial);
                    }
                }
                const char *fx3_name = "Cypress FX3";
                if (desc.idProduct == FX3_PID_FX3USBADC) {
                    fx3_name = "FX3 USB ADC";
                } else if (desc.idProduct == FX3_PID_BOOTLOADER) {
                    fx3_name = "Cypress FX3 Bootloader";
                }

                snprintf(devices[count].name, sizeof(devices[count].name),
                         "%s (Bus %d, Addr %d)",
                         fx3_name, devices[count].bus, devices[count].address);

                count++;
            }
        }
    }

    cyusb_close();
#else
    // Other platforms: Use libusb enumeration
    if (fx3_usb_init() != 0) {
        fprintf(stderr, "[FX3] Failed to initialize libusb\n");
        return 0;
    }

    libusb_device **devlist;
    ssize_t num_devices = libusb_get_device_list(s_fx3_usb_ctx, &devlist);

    for (ssize_t i = 0; i < num_devices && count < max_devices; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devlist[i], &desc) == 0) {
            // Check for FX3 VID and known PIDs
            if (desc.idVendor == FX3_VID &&
                (desc.idProduct == FX3_PID_MISRC ||
                 desc.idProduct == FX3_PID_FX3USBADC ||
                 desc.idProduct == FX3_PID_BOOTLOADER)) {

                devices[count].bus = libusb_get_bus_number(devlist[i]);
                devices[count].address = libusb_get_device_address(devlist[i]);
                devices[count].serial[0] = '\0';
                const char *fx3_name = "Cypress FX3";
                if (desc.idProduct == FX3_PID_FX3USBADC) {
                    fx3_name = "FX3 USB ADC";
                } else if (desc.idProduct == FX3_PID_BOOTLOADER) {
                    fx3_name = "Cypress FX3 Bootloader";
                }

                snprintf(devices[count].name, sizeof(devices[count].name),
                         "%s (Bus %d, Addr %d)",
                         fx3_name, devices[count].bus, devices[count].address);

                count++;
            }
        }
    }

    libusb_free_device_list(devlist, 1);
    fx3_usb_exit();
#endif

    return count;
}

//-----------------------------------------------------------------------------
// FX3 Device Open/Close
//-----------------------------------------------------------------------------
#ifdef __linux__
typedef struct {
    cyusb_handle *handle;
    uint16_t pid;
    int fx3_index;
} fx3_linux_runtime_candidate_t;

static int fx3_open_linux_runtime_candidate(const fx3_linux_runtime_candidate_t *candidate)
{
    if (!candidate || !candidate->handle) return -1;

    s_fx3_handle = candidate->handle;
    s_fx3_protocol = fx3_protocol_from_pid(candidate->pid);
    s_fx3_bulk_ep = (s_fx3_protocol == FX3_PROTOCOL_FX3USBADC)
                    ? FX3USBADC_EP_BULK_IN
                    : FX3_EP_BULK_IN;

    int r = fx3_usb_claim(s_fx3_handle, s_fx3_interface);
    if (r < 0) {
        fprintf(stderr, "[FX3] Failed to claim interface on FX3 index %d (PID=0x%04X): %d\n",
                candidate->fx3_index, candidate->pid, r);
        s_fx3_handle = NULL;
        s_fx3_protocol = FX3_PROTOCOL_UNKNOWN;
        s_fx3_bulk_ep = FX3_EP_BULK_IN;
        return -1;
    }

    fprintf(stderr, "[FX3] Opened FX3 index %d (PID=0x%04X, protocol=%s, EP=0x%02X)%c",
            candidate->fx3_index, candidate->pid, fx3_protocol_name(s_fx3_protocol), s_fx3_bulk_ep, 10);
    return 0;
}
#endif

int gui_fx3_open(gui_app_t *app, int device_index) {
    (void)app;
    fx3usbadc_reset_mode();

#ifdef __linux__
    int num_devices = cyusb_open();
    if (num_devices <= 0) {
        fprintf(stderr, "[FX3] No FX3 devices found\n");
        return -1;
    }
    fx3_linux_runtime_candidate_t *runtime_candidates =
        (fx3_linux_runtime_candidate_t *)calloc((size_t)num_devices, sizeof(*runtime_candidates));
    if (!runtime_candidates) {
        fprintf(stderr, "[FX3] Out of memory while enumerating FX3 devices\n");
        cyusb_close();
        return -1;
    }

    // Collect all FX3 devices and runtime-openable candidates.
    // Find the device by index (counting only FX3 devices)
    int fx3_count = 0;
    int runtime_count = 0;
    int selected_runtime_candidate = -1;
    bool selected_found = false;
    bool selected_is_bootloader = false;
    for (int i = 0; i < num_devices; i++) {
        cyusb_handle *h = cyusb_gethandle(i);
        if (!h) continue;

        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(libusb_get_device(h), &desc) == 0) {
            if (desc.idVendor == FX3_VID &&
                (desc.idProduct == FX3_PID_MISRC ||
                 desc.idProduct == FX3_PID_FX3USBADC ||
                 desc.idProduct == FX3_PID_BOOTLOADER)) {

                bool is_bootloader = (desc.idProduct == FX3_PID_BOOTLOADER);
                if (fx3_count == device_index) {
                    selected_found = true;
                    selected_is_bootloader = is_bootloader;
                }

                if (!is_bootloader && runtime_count < num_devices) {
                    runtime_candidates[runtime_count].handle = h;
                    runtime_candidates[runtime_count].pid = desc.idProduct;
                    runtime_candidates[runtime_count].fx3_index = fx3_count;
                    if (fx3_count == device_index) {
                        selected_runtime_candidate = runtime_count;
                    }
                    runtime_count++;
                }
                fx3_count++;
            }
        }
    }

    if (!selected_found) {
        fprintf(stderr, "[FX3] Device index %d not found (found %d FX3 devices)\n",
                device_index, fx3_count);
        free(runtime_candidates);
        cyusb_close();
        return -1;
    }

    bool selected_runtime_failed = false;
    if (selected_runtime_candidate >= 0) {
        if (fx3_open_linux_runtime_candidate(&runtime_candidates[selected_runtime_candidate]) == 0) {
            free(runtime_candidates);
            return 0;
        }
        selected_runtime_failed = true;
    } else if (selected_is_bootloader) {
        fprintf(stderr, "[FX3] Selected FX3 index %d is bootloader mode (PID=0x%04X); searching for runtime FX3 device...%c",
                device_index, FX3_PID_BOOTLOADER, 10);
    }

    if (selected_runtime_failed) {
        fprintf(stderr, "[FX3] Selected FX3 index %d could not be opened; trying other runtime FX3 devices...%c",
                device_index, 10);
    }

    for (int i = 0; i < runtime_count; i++) {
        if (i == selected_runtime_candidate) continue;
        if (fx3_open_linux_runtime_candidate(&runtime_candidates[i]) == 0) {
            fprintf(stderr, "[FX3] Using fallback runtime FX3 index %d for capture%c",
                    runtime_candidates[i].fx3_index, 10);
            free(runtime_candidates);
            return 0;
        }
    }

    if (runtime_count == 0) {
        fprintf(stderr, "[FX3] No runtime FX3 device found. Flash firmware and reconnect as PID=0x%04X.%c",
                FX3_PID_FX3USBADC, 10);
    } else {
        fprintf(stderr, "[FX3] Found %d runtime FX3 device(s), but none could be opened.%c",
                runtime_count, 10);
    }

    free(runtime_candidates);
    s_fx3_handle = NULL;
    s_fx3_protocol = FX3_PROTOCOL_UNKNOWN;
    s_fx3_bulk_ep = FX3_EP_BULK_IN;
    cyusb_close();
    return -1;
#else
    if (fx3_usb_init() != 0) {
        fprintf(stderr, "[FX3] Failed to initialize libusb\n");
        return -1;
    }

    // Default firmware path - look in current directory and common locations
    const char *firmware_paths[] = {
        "cypress-fx3.fw",
        "./cypress-fx3.fw",
        "../cypress-fx3.fw",
        "C:/git/MISRC/cypress-fx3.fw",
        NULL
    };

retry_after_firmware:;
    // Find device by index (same approach as enumeration)
    libusb_device **devlist;
    ssize_t num_devices = libusb_get_device_list(s_fx3_usb_ctx, &devlist);
    if (num_devices < 0) {
        fprintf(stderr, "[FX3] Failed to get device list\n");
        fx3_usb_exit();
        return -1;
    }

    int fx3_count = 0;
    for (ssize_t i = 0; i < num_devices; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devlist[i], &desc) == 0) {
            if (desc.idVendor == FX3_VID &&
                (desc.idProduct == FX3_PID_MISRC ||
                 desc.idProduct == FX3_PID_FX3USBADC ||
                 desc.idProduct == FX3_PID_BOOTLOADER)) {
                if (fx3_count == device_index) {
                    // Found the device, open it
                    int r = libusb_open(devlist[i], &s_fx3_handle);
                    if (r < 0) {
                        fprintf(stderr, "[FX3] Failed to open device: %s\n", libusb_error_name(r));
                        libusb_free_device_list(devlist, 1);
                        fx3_usb_exit();
                        return -1;
                    }
                    s_fx3_protocol = fx3_protocol_from_pid(desc.idProduct);
                    s_fx3_bulk_ep = (s_fx3_protocol == FX3_PROTOCOL_FX3USBADC)
                                    ? FX3USBADC_EP_BULK_IN
                                    : FX3_EP_BULK_IN;
                    fprintf(stderr, "[FX3] Selected protocol=%s for PID=0x%04X (default EP=0x%02X)%c",
                            fx3_protocol_name(s_fx3_protocol), desc.idProduct, s_fx3_bulk_ep, 10);

                    if (s_fx3_protocol == FX3_PROTOCOL_SIGROK) {
                        // Check if firmware is loaded by reading USB manufacturer/product strings.
                        // Sigrok firmware sets manufacturer="sigrok", product="cypress-fx3".
                        // If these don't match, we need to upload firmware (even with PID 0x1234).
                        bool needs_firmware = false;
                        char manufacturer[64] = {0};
                        char product[64] = {0};

                    if (desc.iManufacturer) {
                        libusb_get_string_descriptor_ascii(s_fx3_handle, desc.iManufacturer,
                            (unsigned char *)manufacturer, sizeof(manufacturer));
                    }
                    if (desc.iProduct) {
                        libusb_get_string_descriptor_ascii(s_fx3_handle, desc.iProduct,
                            (unsigned char *)product, sizeof(product));
                    }

                    fprintf(stderr, "[FX3] USB strings: manufacturer='%s', product='%s'\n",
                            manufacturer, product);

                    // Check if firmware is already loaded (sigrok sets these strings)
                    if (strcmp(manufacturer, "sigrok") != 0 || strcmp(product, "cypress-fx3") != 0) {
                        needs_firmware = true;
                        fprintf(stderr, "[FX3] Firmware not loaded (strings don't match sigrok/cypress-fx3)\n");
                    } else {
                        fprintf(stderr, "[FX3] Firmware already loaded\n");
                    }

                    // Upload firmware if needed (bootloader mode OR missing firmware strings)
                    if (desc.idProduct == FX3_PID_BOOTLOADER || needs_firmware) {
                        fprintf(stderr, "[FX3] Device needs firmware upload (PID=0x%04X, needs_fw=%d)\n",
                                desc.idProduct, needs_firmware);

                        // Detach kernel driver if necessary (as sigrok does)
                        if (libusb_kernel_driver_active(s_fx3_handle, 0) == 1) {
                            fprintf(stderr, "[FX3] Detaching kernel driver for bootloader\n");
                            libusb_detach_kernel_driver(s_fx3_handle, 0);
                        }

                        // Set configuration BEFORE firmware upload (as sigrok does)
                        r = libusb_set_configuration(s_fx3_handle, 1);
                        if (r < 0) {
                            fprintf(stderr, "[FX3] Warning: Failed to set configuration for bootloader: %s\n",
                                    libusb_error_name(r));
                            // Continue anyway - bootloader might not need it
                        } else {
                            fprintf(stderr, "[FX3] Set configuration 1 for bootloader\n");
                        }

                        // Find firmware file
                        const char *fw_path = NULL;
                        for (int p = 0; firmware_paths[p] != NULL; p++) {
                            FILE *test = fopen(firmware_paths[p], "rb");
                            if (test) {
                                fclose(test);
                                fw_path = firmware_paths[p];
                                break;
                            }
                        }

                        if (!fw_path) {
                            fprintf(stderr, "[FX3] ERROR: Firmware file 'cypress-fx3.fw' not found!\n");
                            fprintf(stderr, "[FX3] Please place the firmware file in one of these locations:\n");
                            for (int p = 0; firmware_paths[p] != NULL; p++) {
                                fprintf(stderr, "[FX3]   - %s\n", firmware_paths[p]);
                            }
                            libusb_close(s_fx3_handle);
                            s_fx3_handle = NULL;
                            libusb_free_device_list(devlist, 1);
                            fx3_usb_exit();
                            return -1;
                        }

                        // Upload firmware
                        r = fx3_upload_firmware(fw_path);
                        libusb_close(s_fx3_handle);
                        s_fx3_handle = NULL;
                        libusb_free_device_list(devlist, 1);

                        if (r != 0) {
                            fprintf(stderr, "[FX3] Firmware upload failed\n");
                            fx3_usb_exit();
                            return -1;
                        }

                        // Wait for device to re-enumerate with new firmware
                        // Sigrok waits 1 second before polling, then up to 3 seconds total
                        fprintf(stderr, "[FX3] Waiting for device to re-enumerate (1s initial delay)...\n");
                        thrd_sleep_ms(1000);  // 1 second initial delay as sigrok does

                        for (int wait = 0; wait < 20; wait++) {  // Poll for up to 2 more seconds
                            thrd_sleep_ms(100);  // 100ms

                            // Check if device appeared with firmware loaded
                            libusb_device **newlist;
                            ssize_t new_count = libusb_get_device_list(s_fx3_usb_ctx, &newlist);
                            for (ssize_t j = 0; j < new_count; j++) {
                                struct libusb_device_descriptor new_desc;
                                if (libusb_get_device_descriptor(newlist[j], &new_desc) == 0) {
                                    if (new_desc.idVendor == FX3_VID && new_desc.idProduct == FX3_PID_MISRC) {
                                        // Try to open and check USB strings to verify firmware is loaded
                                        libusb_device_handle *test_handle;
                                        if (libusb_open(newlist[j], &test_handle) == 0) {
                                            char test_manuf[64] = {0};
                                            char test_prod[64] = {0};
                                            if (new_desc.iManufacturer) {
                                                libusb_get_string_descriptor_ascii(test_handle,
                                                    new_desc.iManufacturer, (unsigned char *)test_manuf, sizeof(test_manuf));
                                            }
                                            if (new_desc.iProduct) {
                                                libusb_get_string_descriptor_ascii(test_handle,
                                                    new_desc.iProduct, (unsigned char *)test_prod, sizeof(test_prod));
                                            }
                                            libusb_close(test_handle);

                                            // Check if firmware strings match
                                            if (strcmp(test_manuf, "sigrok") == 0 && strcmp(test_prod, "cypress-fx3") == 0) {
                                                fprintf(stderr, "[FX3] Device re-enumerated with firmware (PID=0x%04X, '%s'/'%s')\n",
                                                        new_desc.idProduct, test_manuf, test_prod);
                                                libusb_free_device_list(newlist, 1);
                                                goto retry_after_firmware;
                                            }
                                        }
                                    }
                                }
                            }
                            libusb_free_device_list(newlist, 1);
                        }

                        fprintf(stderr, "[FX3] Timeout waiting for device to re-enumerate\n");
                        fx3_usb_exit();
                        return -1;
                    }

                    } else {
                        fprintf(stderr, "[FX3] Using fx3usbadc protocol; skipping sigrok firmware checks%c", 10);
                    }

                    // Device has firmware loaded, proceed normally
                    fprintf(stderr, "[FX3] Opened device index %d (VID=%04X PID=%04X)\n",
                            device_index, desc.idVendor, desc.idProduct);

                    // Detach kernel driver if necessary
                    if (libusb_kernel_driver_active(s_fx3_handle, s_fx3_interface) == 1) {
                        fprintf(stderr, "[FX3] Detaching kernel driver from interface %d\n", s_fx3_interface);
                        libusb_detach_kernel_driver(s_fx3_handle, s_fx3_interface);
                    }

                    // Set configuration BEFORE claiming interface (as sigrok does)
                    r = libusb_set_configuration(s_fx3_handle, 1);
                    if (r < 0 && r != LIBUSB_ERROR_BUSY) {
                        fprintf(stderr, "[FX3] Warning: Failed to set configuration 1: %s\n", libusb_error_name(r));
                        // Continue anyway - might already be configured
                    } else {
                        fprintf(stderr, "[FX3] Set configuration 1\n");
                    }

                    // Claim interface
                    r = fx3_usb_claim(s_fx3_handle, s_fx3_interface);
                    if (r < 0) {
                        fprintf(stderr, "[FX3] Failed to claim interface %d: %s\n",
                                s_fx3_interface, libusb_error_name(r));
                        libusb_close(s_fx3_handle);
                        s_fx3_handle = NULL;
                        libusb_free_device_list(devlist, 1);
                        fx3_usb_exit();
                        return -1;
                    }
                    fprintf(stderr, "[FX3] Claimed interface %d\n", s_fx3_interface);

                    // Enumerate endpoints and find an alternate setting with bulk endpoints
                    struct libusb_config_descriptor *config;
                    int best_alt = -1;
                    int bulk_ep_found = 0;

                    if (libusb_get_active_config_descriptor(devlist[i], &config) == 0) {
                        fprintf(stderr, "[FX3] Configuration %d: %d interface(s)\n",
                                config->bConfigurationValue, config->bNumInterfaces);

                        // Look at interface 0
                        if (config->bNumInterfaces > 0) {
                            const struct libusb_interface *interface = &config->interface[0];
                            fprintf(stderr, "[FX3] Interface 0 has %d alternate setting(s)\n",
                                    interface->num_altsetting);

                            for (int alt = 0; alt < interface->num_altsetting; alt++) {
                                const struct libusb_interface_descriptor *iface_desc = &interface->altsetting[alt];
                                fprintf(stderr, "[FX3]   Alt %d: %d endpoint(s)\n",
                                        iface_desc->bAlternateSetting, iface_desc->bNumEndpoints);

                                for (int ep = 0; ep < iface_desc->bNumEndpoints; ep++) {
                                    const struct libusb_endpoint_descriptor *ep_desc = &iface_desc->endpoint[ep];
                                    const char *dir = (ep_desc->bEndpointAddress & 0x80) ? "IN" : "OUT";
                                    const char *type = "Unknown";
                                    switch (ep_desc->bmAttributes & 0x03) {
                                        case 0: type = "Control"; break;
                                        case 1: type = "Isochronous"; break;
                                        case 2: type = "Bulk"; break;
                                        case 3: type = "Interrupt"; break;
                                    }
                                    fprintf(stderr, "[FX3]     EP 0x%02X: %s %s, MaxPacket=%d\n",
                                            ep_desc->bEndpointAddress, type, dir, ep_desc->wMaxPacketSize);

                                    // Check for bulk IN endpoint
                                    if ((ep_desc->bmAttributes & 0x03) == 2 &&  // Bulk
                                        (ep_desc->bEndpointAddress & 0x80)) {   // IN
                                        if (best_alt < 0 || iface_desc->bAlternateSetting > 0) {
                                            best_alt = iface_desc->bAlternateSetting;
                                            bulk_ep_found = ep_desc->bEndpointAddress;
                                        }
                                    }
                                }
                            }
                        }
                        libusb_free_config_descriptor(config);
                    }

                    // If we found an alternate setting with bulk endpoints, select it
                    if (best_alt > 0) {
                        fprintf(stderr, "[FX3] Selecting alternate setting %d (has bulk EP 0x%02X)\n",
                                best_alt, bulk_ep_found);
                        r = libusb_set_interface_alt_setting(s_fx3_handle, s_fx3_interface, best_alt);
                        if (r < 0) {
                            fprintf(stderr, "[FX3] Warning: Failed to set alt setting %d: %s\n",
                                    best_alt, libusb_error_name(r));
                        } else {
                            fprintf(stderr, "[FX3] Set alternate setting %d successfully\n", best_alt);
                        }
                        s_fx3_bulk_ep = (uint8_t)bulk_ep_found;
                    } else if (bulk_ep_found) {
                        fprintf(stderr, "[FX3] Using default alt setting 0 with bulk EP 0x%02X\n", bulk_ep_found);
                        s_fx3_bulk_ep = (uint8_t)bulk_ep_found;
                    } else {
                        fprintf(stderr, "[FX3] Warning: No bulk IN endpoint found!\n");
                        fprintf(stderr, "[FX3] Device may need power cycle. Please:\n");
                        fprintf(stderr, "[FX3]   1. Unplug the FX3 device\n");
                        fprintf(stderr, "[FX3]   2. Wait 5 seconds\n");
                        fprintf(stderr, "[FX3]   3. Plug it back in\n");
                        fprintf(stderr, "[FX3]   4. Try again - firmware will be uploaded automatically\n");
                    }

                    libusb_free_device_list(devlist, 1);
                    return 0;
                }
                fx3_count++;
            }
        }
    }

    fprintf(stderr, "[FX3] Device index %d not found (found %d FX3 devices)\n",
            device_index, fx3_count);
    libusb_free_device_list(devlist, 1);
    fx3_usb_exit();
    return -1;
#endif
}

void gui_fx3_close(gui_app_t *app) {
    (void)app;

    if (s_fx3_handle) {
        fx3_usb_release(s_fx3_handle, s_fx3_interface);

#ifdef __linux__
        cyusb_close();
#else
        libusb_close(s_fx3_handle);
        fx3_usb_exit();
#endif
        s_fx3_handle = NULL;
    }
    s_fx3_protocol = FX3_PROTOCOL_UNKNOWN;
    s_fx3_bulk_ep = FX3_EP_BULK_IN;
    fx3usbadc_reset_mode();
}

//-----------------------------------------------------------------------------
// FX3 Capture Thread
//-----------------------------------------------------------------------------

static int fx3_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    fx3_protocol_t active_protocol = s_fx3_protocol;
    uint32_t active_sample_rate_hz = fx3_active_sample_rate_hz();

    // Allocate transfer buffer
    uint8_t *transfer_buf = (uint8_t *)malloc(FX3_TRANSFER_SIZE);
    if (!transfer_buf) {
        fprintf(stderr, "[FX3] Failed to allocate transfer buffer\n");
        return -1;
    }
    fprintf(stderr, "[FX3] Capture thread started (%s) at %u MSPS%c",
            fx3_protocol_name(active_protocol), active_sample_rate_hz / 1000000, 10);

    // Clear any stale data from endpoint. NOTE: do NOT issue a bulk read here
    // before the 0x91 start command is sent. fx3usbadc sends no data until 0x91,
    // and a pre-start read times out and can leave the endpoint in a stalled
    // state that breaks the subsequent streaming. The 0x91 command is sent by
    // gui_fx3_start() AFTER this thread signals s_fx3_transfer_ready.
    fprintf(stderr, "[FX3] Clearing endpoint 0x%02X...%c", s_fx3_bulk_ep, 10);
    libusb_clear_halt(s_fx3_handle, s_fx3_bulk_ep);

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, active_sample_rate_hz);

    uint64_t batch_count = 0;
    uint64_t timeout_count = 0;
    int actual_length = 0;

    // Signal that we're about to start the first transfer
    fprintf(stderr, "[FX3] Capture thread signaling ready for transfers\n");
    fprintf(stderr, "[FX3] Transfer params: EP=0x%02X, size=%d, timeout=%dms\n",
            s_fx3_bulk_ep, FX3_TRANSFER_SIZE, FX3_TRANSFER_TIMEOUT);
    atomic_store(&s_fx3_transfer_ready, true);

    // Try configured endpoint first, then other bulk IN endpoints.
    // NOTE: the multi-endpoint fallback is for the sigrok cypress-fx3 firmware
    // (PID 0x1234), whose bulk IN endpoint can vary. fx3usbadc (PID 0x00F1)
    // has exactly one bulk IN endpoint (0x81) and no others; switching to
    // 0x82/0x83 produces LIBUSB_ERROR_IO spam. So for fx3usbadc only try EP 0x81.
    uint8_t endpoints_to_try[4];
    int endpoint_count = 0;
    endpoints_to_try[endpoint_count++] = s_fx3_bulk_ep;
    if (active_protocol != FX3_PROTOCOL_FX3USBADC) {
        if (s_fx3_bulk_ep != 0x81) endpoints_to_try[endpoint_count++] = 0x81;
        if (s_fx3_bulk_ep != 0x82) endpoints_to_try[endpoint_count++] = 0x82;
        if (s_fx3_bulk_ep != 0x83) endpoints_to_try[endpoint_count++] = 0x83;
    }
    int current_ep_idx = 0;
    uint8_t current_ep = endpoints_to_try[0];

    while (atomic_load(&app->fx3_running)) {
        // Perform bulk transfer from FX3
        actual_length = 0;
        int r = fx3_usb_bulk_transfer(s_fx3_handle, current_ep,
                                       transfer_buf, FX3_TRANSFER_SIZE,
                                       &actual_length, FX3_TRANSFER_TIMEOUT);
        if (!atomic_load(&app->fx3_running)) break;

        if (r < 0) {
            if (r == LIBUSB_ERROR_TIMEOUT) {
                if (actual_length > 0) gui_record_mark_source_loss();
                timeout_count++;
                if (timeout_count <= 3) {
                    fprintf(stderr, "[FX3] Bulk transfer timeout #%llu on EP 0x%02X (no data received)\n",
                            (unsigned long long)timeout_count, current_ep);
                }
                // After 3 timeouts on current endpoint, try next one
                if (timeout_count == 3 && current_ep_idx < (endpoint_count - 1)) {
                    if (batch_count != 0 && actual_length == 0) gui_record_mark_source_loss();
                    current_ep_idx++;
                    current_ep = endpoints_to_try[current_ep_idx];
                    fprintf(stderr, "[FX3] Switching to endpoint 0x%02X\n", current_ep);
                    libusb_clear_halt(s_fx3_handle, current_ep);
                    timeout_count = 0;
                }
                continue;
            }
            gui_record_mark_source_loss();
            // Fatal errors: the device/handle is gone. Break out of the loop
            // instead of spinning and spamming hundreds of thousands of errors
            // (verified: LIBUSB_ERROR_NO_DEVICE spammed 432684x when the FX3
            // dropped mid-stream because bulk_transfer returns instantly).
            if (r == LIBUSB_ERROR_NO_DEVICE ||
                r == LIBUSB_ERROR_NOT_FOUND ||
                r == LIBUSB_ERROR_NO_MEM ||
                r == LIBUSB_ERROR_ACCESS) {
                fprintf(stderr, "[FX3] Fatal USB error on EP 0x%02X: %s (%d) - stopping capture\n",
                        current_ep, libusb_error_name(r), r);
                gui_app_count_system_errors(app, 1);
                atomic_store(&app->stream_synced, false);
                break;
            }
            // Transient errors (PIPE, OVERFLOW, BABBLE, INTERRUPTED): log once,
            // count, and continue. Rate-limit to avoid log flooding.
            static uint64_t transient_err_count = 0;
            transient_err_count++;
            if (transient_err_count <= 5 || (transient_err_count % 1000) == 0) {
                fprintf(stderr, "[FX3] Bulk transfer error #%llu on EP 0x%02X: %s (%d)\n",
                        (unsigned long long)transient_err_count,
                        current_ep, libusb_error_name(r), r);
            }
            gui_app_count_system_errors(app, 1);
            libusb_clear_halt(s_fx3_handle, current_ep);
            continue;
        }

        if (actual_length == 0) {
            fprintf(stderr, "[FX3] Got 0-length transfer\n");
            continue;
        }

        // First successful transfer
        if (batch_count == 0) {
            fprintf(stderr, "[FX3] First data received: %d bytes\n", actual_length);
        }

        size_t wire_sample_bytes = active_protocol == FX3_PROTOCOL_FX3USBADC
            ? sizeof(int16_t) : sizeof(uint32_t);
        if ((size_t)actual_length % wire_sample_bytes != 0) {
            gui_record_mark_source_loss();
            continue;
        }

        size_t num_samples = 0;
        size_t write_bytes = 0;
        if (active_protocol == FX3_PROTOCOL_FX3USBADC) {
            num_samples = (size_t)actual_length / sizeof(int16_t);
            write_bytes = num_samples * sizeof(uint32_t);
        } else {
            num_samples = (size_t)actual_length / sizeof(uint32_t);
            write_bytes = (size_t)actual_length;
        }
        uint8_t *buf_out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                               write_bytes, NULL);
        if (buf_out) {
            if (active_protocol == FX3_PROTOCOL_FX3USBADC) {
                const int16_t *samples_in = (const int16_t *)transfer_buf;
                uint32_t *packed_out = (uint32_t *)buf_out;
                for (size_t i = 0; i < num_samples; i++) {
                    packed_out[i] = fx3usbadc_pack_signed10_from_sample16(samples_in[i]);
                }
            } else {
                // Legacy FX3 data is already in the correct 32-bit packed format:
                // Bits 0-11: Channel A (12-bit), Bits 12-19: AUX (8-bit),
                // Bits 20-31: Channel B (12-bit).
                memcpy(buf_out, transfer_buf, write_bytes);
            }
            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, write_bytes);
            bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);
        } else {
            // Buffer full - drop data
            atomic_fetch_add(&app->rb_drop_count, 1);
            if (atomic_load(&app->rb_drop_count) <= 5) {
                fprintf(stderr, "[FX3] Warning: BUF_CAPTURE_RF full, data dropped\n");
            }
        }

        // Update statistics
        atomic_fetch_add(&app->total_samples, num_samples);
        if (active_protocol == FX3_PROTOCOL_FX3USBADC) {
            atomic_fetch_add(&app->samples_a, num_samples);
        }
        atomic_store(&app->last_callback_time_ms, get_time_ms());

        batch_count++;
    }

    fprintf(stderr, "[FX3] Capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(transfer_buf);
    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_fx3_start(gui_app_t *app) {
    fprintf(stderr, "[FX3] Starting FX3 capture\n");

    if (s_fx3_protocol == FX3_PROTOCOL_SIGROK) {
        // Check firmware version first (optional but helpful for diagnostics)
        uint8_t fw_major = 0, fw_minor = 0;
        if (fx3_cmd_get_fw_version(&fw_major, &fw_minor) == 0) {
            fprintf(stderr, "[FX3] Connected to FX3 firmware v%d.%d%c", fw_major, fw_minor, 10);
        } else {
            fprintf(stderr, "[FX3] Warning: Could not read firmware version (may be OK)%c", 10);
        }
    } else {
        fprintf(stderr, "[FX3] Using protocol %s%c", fx3_protocol_name(s_fx3_protocol), 10);
    }

    // Start-acquisition ordering differs by protocol:
    //  - sigrok cypress-fx3 (PID 0x1234): USB transfers must be pending BEFORE
    //    CMD_START, so 0x91 is sent after the capture thread is reading.
    //  - fx3usbadc (PID 0x00F1): the reference app (BulkTransfer.cpp) sends 0x91
    //    and waits for it to complete BEFORE submitting bulk reads. Sending 0x91
    //    while a bulk read is already pending on EP 0x81 causes that read to
    //    time out (verified: 3x timeout then endpoint-switch spam). So for
    //    fx3usbadc we send 0x91 before launching the capture thread.
    const bool fx3_start_before_thread = (s_fx3_protocol == FX3_PROTOCOL_FX3USBADC);

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
    atomic_store(&app->sample_rate, fx3_active_sample_rate_hz());
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // Reset display buffers
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Ensure capture buffer is initialized
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[FX3] Failed to initialize capture ringbuffer\n");
        gui_app_set_status(app, "Failed to initialize capture buffer");
        return -1;
    }

    // Start thread
    atomic_store(&app->fx3_running, true);
    app->is_capturing = true;

    // Start extraction thread - reads from BUF_CAPTURE_RF, writes to BUF_DISPLAY
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[FX3] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        atomic_store(&app->fx3_running, false);
        app->is_capturing = false;
        return -1;
    }

    // Start display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[FX3] Failed to start display thread (non-fatal)\n");
            // Non-fatal - display will use legacy path
        }
    }


    // For fx3usbadc: send 0x91 NOW, before the capture thread reads EP 0x81.
    // The extraction + display threads are already running and will drain the
    // ringbuffer once the capture thread starts filling it.
    if (fx3_start_before_thread) {
        if (fx3_cmd_start_acquisition(fx3_active_sample_rate_hz()) != 0) {
            fprintf(stderr, "[FX3] Failed to start acquisition\n");
            gui_app_set_status(app, "FX3ADC: Failed to start acquisition");
            gui_extract_stop();
            if (app->display_thread) {
                gui_display_thread_stop(app->display_thread);
            }
            atomic_store(&app->fx3_running, false);
            app->is_capturing = false;
            return -1;
        }
        // Start command may auto-fallback to a different firmware mode.
        atomic_store(&app->sample_rate, fx3_active_sample_rate_hz());
    }

    atomic_store(&s_fx3_transfer_ready, false);
    // Start FX3 capture thread
    thrd_t thread;
    if (thrd_create_with_priority(&thread,
                                  fx3_capture_thread,
                                  app,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[FX3] Failed to create capture thread\n");
        gui_extract_stop();
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        atomic_store(&app->fx3_running, false);
        app->is_capturing = false;
        return -1;
    }
    app->fx3_thread = (void *)(uintptr_t)thread;

    if (!fx3_start_before_thread) {
        // sigrok ordering: wait for the capture thread to be reading, then send 0x91.
        fprintf(stderr, "[FX3] Waiting for capture thread to be ready...\n");
        for (int i = 0; i < 100; i++) {  // Max 1 second wait
            if (atomic_load(&s_fx3_transfer_ready)) {
                fprintf(stderr, "[FX3] Capture thread ready after %d ms\n", i * 10);
                break;
            }
            thrd_sleep_ms(10);
        }
        if (!atomic_load(&s_fx3_transfer_ready)) {
            fprintf(stderr, "[FX3] Warning: Capture thread did not signal ready\n");
        }
        thrd_sleep_ms(100);
        fprintf(stderr, "[FX3] Waited additional 100ms for bulk transfer to be pending\n");
        if (fx3_cmd_start_acquisition(fx3_active_sample_rate_hz()) != 0) {
            fprintf(stderr, "[FX3] Failed to start acquisition\n");
            gui_app_set_status(app, "FX3ADC: Failed to start acquisition");
            atomic_store(&app->fx3_running, false);
            thrd_join(thread, NULL);
            app->fx3_thread = NULL;
            gui_extract_stop();
            if (app->display_thread) {
                gui_display_thread_stop(app->display_thread);
            }
            app->is_capturing = false;
            return -1;
        }
    }

    gui_app_set_status(app, "FX3ADC capture running");
    return 0;
}

void gui_fx3_stop(gui_app_t *app) {
    if (!atomic_load(&app->fx3_running)) return;

    fprintf(stderr, "[FX3] Stopping FX3 capture\n");

    // Reset transfer ready flag
    atomic_store(&s_fx3_transfer_ready, false);

    // Set is_capturing to false BEFORE stopping extraction thread
    app->is_capturing = false;

    atomic_store(&app->fx3_running, false);

    // Stop capture thread first
    if (app->fx3_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->fx3_thread;
        thrd_join(thread, NULL);
        app->fx3_thread = NULL;
    }

    // Stop display thread
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop extraction thread
    gui_extract_stop();

    // Close USB device
    gui_fx3_close(app);

    atomic_store(&app->stream_synced, false);

    gui_app_set_status(app, "FX3ADC capture stopped");
}

bool gui_fx3_is_running(gui_app_t *app) {
    return atomic_load(&app->fx3_running);
}

#endif // ENABLE_FX3
