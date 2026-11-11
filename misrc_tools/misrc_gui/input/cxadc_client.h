/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CXADC_CLIENT_H
#define CXADC_CLIENT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CXADC_CLIENT_ABI 1u
#define CXADC_CLIENT_CHUNK_BYTES 262144u
typedef struct cxadc_client cxadc_client;
typedef struct {
    uint64_t registry_id;
    uint32_t vendor, device, subsystem_vendor, subsystem_device, revision, driver_attached;
    char name[128];
} cxadc_client_device;
typedef struct { uint32_t vmux, level, sixdb, center_offset, agc_enable; } cxadc_client_settings;
typedef struct {
    uint32_t driver_api, driver_build, nominal_rate_hz, running, error, overruns;
    uint32_t stop_verified, quarantined, pci_command, command_restored;
    cxadc_client_settings settings;
    uint64_t delivered_bytes, interrupts, ring_wraps;
} cxadc_client_state;
/* All operations return 0 on success, otherwise a native IOReturn value.
 * Enumeration never opens or programs hardware. Registry IDs are session IDs,
 * not device serials. Open does not start DMA. A client exclusively owns its
 * stream after start. Settings may only change while stopped. Read requires a
 * CHUNK_BYTES buffer: success with *bytes=0 means not ready; data is native u8.
 * Stop verifies teardown; close also stops an owned stream. No exceptions cross
 * this C ABI. Call error() immediately on the calling thread after an error.
 * Join readers before close; operations are serialized, object lifetime is the
 * caller's responsibility. Nominal rate is configured, not measured.
 * Host executables need the existing DriverKit userclient-access entitlement.
 */
typedef struct {
    uint32_t abi_version, struct_bytes;
    int32_t (*enumerate)(cxadc_client_device *, size_t capacity, size_t *count);
    int32_t (*open)(uint64_t registry_id, cxadc_client **);
    void (*close)(cxadc_client *);
    int32_t (*status)(cxadc_client *, cxadc_client_state *);
    int32_t (*configure)(cxadc_client *, const cxadc_client_settings *);
    int32_t (*start)(cxadc_client *);
    int32_t (*read)(cxadc_client *, void *, size_t capacity, size_t *bytes);
    int32_t (*stop)(cxadc_client *);
    const char *(*error)(void);
} cxadc_client_api;
const cxadc_client_api *cxadc_client_get_api_v1(void);
/* Optional additive ABI. v1 remains available, unchanged, for existing GUIs.
 * Rate is a declaration, never a clock measurement or hardware clock write.
 * Set only while stopped, after independently verifying an external clock.
 * Timestamps precede DMA enable: they are not sample-exact start markers. */
typedef struct {
    uint64_t registry_id, started_monotonic_ns, stopped_monotonic_ns;
} cxadc_client_timing;
typedef struct {
    uint32_t abi_version, struct_bytes;
    const cxadc_client_api *v1;
    int32_t (*set_nominal_rate)(cxadc_client *, uint32_t rate_hz);
    int32_t (*timing)(cxadc_client *, cxadc_client_timing *);
    /* Concurrent preparation/start; records separate kernel timestamps.
     * Not a shared hardware trigger or sample-phase synchronization claim. */
    int32_t (*start_pair)(cxadc_client *, cxadc_client *);
} cxadc_client_api_v2;
const cxadc_client_api_v2 *cxadc_client_get_api_v2(void);
#ifdef __cplusplus
}
#endif
#endif
