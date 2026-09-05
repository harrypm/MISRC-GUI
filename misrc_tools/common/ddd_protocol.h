/*
 * MISRC Common - Domesday Duplicator firmware protocol helpers.
 *
 * This module is dependency-free so protocol-v1 can be unit-tested without
 * libusb or GUI state. Legacy capture remains implemented by gui_ddd.c.
 */

#ifndef MISRC_DDD_PROTOCOL_H
#define MISRC_DDD_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DDD_LEGACY_VENDOR_ID             UINT16_C(0x1D50)
#define DDD_LEGACY_PRODUCT_ID            UINT16_C(0x603B)
#define DDD_CURRENT_VENDOR_ID            UINT16_C(0x1209)
#define DDD_CURRENT_PRODUCT_ID           UINT16_C(0x2347)
#define DDD_SUPPORTED_PROTOCOL_VERSION   UINT8_C(1)

#define DDD_REQUEST_COLLECTION           UINT8_C(0xB5)
#define DDD_REQUEST_REGISTER_READ        UINT8_C(0xB7)
#define DDD_REQUEST_REGISTER_WRITE       UINT8_C(0xB8)
#define DDD_USB_REQUEST_VENDOR_OUT       UINT8_C(0x40)
#define DDD_USB_REQUEST_VENDOR_IN        UINT8_C(0xC0)

#define DDD_REGISTER_IDENTITY            UINT8_C(0x00)
#define DDD_IDENTITY_LENGTH              12u
#define DDD_IDENTITY_VALUE               UINT8_C(0x44)
#define DDD_REGISTER_MAP_VERSION         UINT8_C(0x01)
#define DDD_SUPPORTED_REGISTER_MAP       UINT8_C(2)
#define DDD_REGISTER_BUILD_FLAGS         UINT8_C(0x02)
#define DDD_REGISTER_COMMIT              UINT8_C(0x03)
#define DDD_COMMIT_LENGTH                8u
#define DDD_BUILD_DIRTY_FLAG             UINT8_C(0x01)
#define DDD_BUILD_COMMIT_FLAG            UINT8_C(0x02)
#define DDD_REGISTER_IMAGE_ROLE          UINT8_C(0x0B)
#define DDD_APPLICATION_IMAGE_ROLE       UINT8_C(1)
#define DDD_REGISTER_TEST_MODE           UINT8_C(0x10)
#define DDD_REGISTER_DECIMATION          UINT8_C(0x12)

/* Self-described FIFO telemetry block. Reading address 0x40 latches a
 * coherent snapshot and clears the interval counters in the gateware. */
#define DDD_REGISTER_FIFO_TELEMETRY       UINT8_C(0x40)
#define DDD_FIFO_TELEMETRY_ID             UINT8_C(0xBD)
#define DDD_FIFO_TELEMETRY_LENGTH         23u
#define DDD_FIFO_TELEMETRY_FORMAT         UINT8_C(1)
#define DDD_FIFO_TELEMETRY_FORMAT_MASK    UINT8_C(0x0F)
#define DDD_FIFO_FLAG_OVERFLOW_SEEN       UINT8_C(0x10)
#define DDD_FIFO_FLAG_SATURATED           UINT8_C(0x20)
#define DDD_FIFO_NEAR_FULL_PRESCALE       256u

#define DDD_FIFO_OFFSET_STATUS            1u
#define DDD_FIFO_OFFSET_LATCH_COUNT       2u
#define DDD_FIFO_OFFSET_USED_NOW          3u
#define DDD_FIFO_OFFSET_PEAK              5u
#define DDD_FIFO_OFFSET_PEAK_LIFETIME     7u
#define DDD_FIFO_OFFSET_OVERFLOWS         9u
#define DDD_FIFO_OFFSET_DROPPED           11u
#define DDD_FIFO_OFFSET_PACKETS           13u
#define DDD_FIFO_OFFSET_NEAR_FULL         15u
#define DDD_FIFO_OFFSET_DEPTH             17u
#define DDD_FIFO_OFFSET_PACKET_WORDS      19u
#define DDD_FIFO_OFFSET_NEAR_FULL_WORDS   21u

#define DDD_DECIMATION_FULL_RATE         UINT8_C(1)
#define DDD_DECIMATION_HALF_RATE         UINT8_C(2)
#define DDD_CONVERTER_SAMPLE_RATE_HZ     UINT32_C(40000000)

#define DDD_STREAM_INTERFACE_NUMBER      0
#define DDD_STREAM_ALTERNATE_SETTING     0
#define DDD_STREAM_ENDPOINT_ADDRESS      UINT8_C(0x81)
#define DDD_STREAM_MAX_PACKET_SIZE       UINT16_C(1024)

#define DDD_SEQUENCE_MARKER_COUNT        UINT8_C(63)
#define DDD_SEQUENCE_SAMPLES_PER_MARKER  UINT32_C(65536)
#define DDD_TEST_RAMP_NEW_WRAP           UINT16_C(1021)
#define DDD_TEST_RAMP_LEGACY_WRAP        UINT16_C(1024)
#define DDD_STABLE_ID_MAX                128u

typedef enum ddd_device_profile {
    DDD_DEVICE_NOT_DDD = 0,
    DDD_DEVICE_LEGACY,
    DDD_DEVICE_PROTOCOL_V1,
    DDD_DEVICE_UNSUPPORTED
} ddd_device_profile_t;

bool ddd_is_known_device_id(uint16_t vendor_id, uint16_t product_id);
ddd_device_profile_t ddd_classify_device(uint16_t vendor_id,
                                         uint16_t product_id,
                                         uint16_t bcd_device);
bool ddd_profile_can_capture(ddd_device_profile_t profile);
bool ddd_profile_requires_usb_path(ddd_device_profile_t profile);
bool ddd_clockgen_candidate_is_preferred(
    bool selection_present,
    ddd_device_profile_t selected_profile,
    ddd_device_profile_t candidate_profile,
    bool candidate_capture_supported);
bool ddd_reconnect_path_matches(
    ddd_device_profile_t target_profile,
    const char *target_usb_path,
    ddd_device_profile_t candidate_profile,
    const char *candidate_usb_path);

typedef struct ddd_profile_index_state {
    int legacy_count;
    int protocol_v1_count;
    int unsupported_count;
} ddd_profile_index_state_t;

void ddd_profile_index_state_init(ddd_profile_index_state_t *state);
int ddd_profile_index_take(ddd_profile_index_state_t *state,
                           ddd_device_profile_t profile);

bool ddd_v1_link_speed_allowed(bool speed_known, bool at_least_superspeed);
bool ddd_decimation_is_supported(uint8_t factor);
bool ddd_profile_supports_decimation(ddd_device_profile_t profile,
                                     uint8_t factor);
uint16_t ddd_make_register_write(uint8_t address, uint8_t value);
uint32_t ddd_sample_rate_hz(uint8_t factor);
uint32_t ddd_sample_rate_khz(uint8_t factor);

typedef struct ddd_v1_rate_plan {
    uint8_t decimation_factor;
    uint32_t hardware_rate_khz;
    uint32_t output_rate_khz;
    bool software_resample;
} ddd_v1_rate_plan_t;

/* Resolve the output rate represented by the pre-unified settings. With the
 * resampler disabled, the stored hardware decimation remains authoritative. */
uint32_t ddd_v1_effective_output_rate_khz(
    bool resample_enabled,
    uint32_t resample_rate_khz,
    uint8_t stored_decimation_factor);

/* Protocol-v1 routes 20 MSPS directly through the FPGA half-rate path. Lower
 * output rates use that 20 MSPS hardware stream as the software source. */
bool ddd_v1_plan_output_rate_khz(uint32_t output_rate_khz,
                                 ddd_v1_rate_plan_t *plan);

bool ddd_identity_is_supported(const uint8_t *identity, size_t length);
bool ddd_format_gateware_commit(const uint8_t *identity,
                                size_t identity_length,
                                char *destination,
                                size_t destination_size);
bool ddd_format_usb_topology_path(uint8_t bus_number,
                                  const uint8_t *ports,
                                  int port_count,
                                  char *destination,
                                  size_t destination_size);

typedef struct ddd_stream_endpoint_candidate {
    int interface_number;
    int alternate_setting;
    uint8_t endpoint_address;
    uint16_t max_packet_size;
    bool is_bulk;
    bool is_in;
} ddd_stream_endpoint_candidate_t;

typedef struct ddd_stream_path {
    int interface_number;
    int alternate_setting;
    uint8_t endpoint_address;
    uint16_t max_packet_size;
    bool found;
} ddd_stream_path_t;

typedef struct ddd_stream_selector {
    ddd_device_profile_t profile;
    ddd_stream_path_t selected;
    size_t protocol_v1_exact_matches;
} ddd_stream_selector_t;

void ddd_stream_selector_init(ddd_stream_selector_t *selector,
                              ddd_device_profile_t profile);
void ddd_stream_selector_consider(
    ddd_stream_selector_t *selector,
    const ddd_stream_endpoint_candidate_t *candidate);
bool ddd_stream_selector_get(const ddd_stream_selector_t *selector,
                             ddd_stream_path_t *selected);

typedef int (*ddd_control_transfer_fn)(void *context,
                                       uint8_t request_type,
                                       uint8_t request,
                                       uint16_t value,
                                       uint16_t index,
                                       uint8_t *data,
                                       uint16_t length);

typedef struct ddd_control_ops {
    ddd_control_transfer_fn transfer;
    void *context;
} ddd_control_ops_t;

typedef struct ddd_fifo_telemetry {
    bool present;
    uint8_t format;
    bool overflow_seen;
    bool saturated;
    uint8_t latch_count;
    uint16_t used_now;
    uint16_t peak;
    uint16_t peak_since_open;
    uint16_t overflow_events;
    uint16_t dropped_words;
    uint16_t packets_read;
    uint16_t near_full_units;
    uint16_t depth_words;
    uint16_t packet_words;
    uint16_t near_full_words;
} ddd_fifo_telemetry_t;

typedef struct ddd_fifo_telemetry_totals {
    bool latch_seen;
    uint8_t last_latch_count;
    bool interval_coverage_complete;
    bool saturated;
    uint64_t overflow_events;
    uint64_t dropped_words;
    uint64_t near_full_units;
    uint16_t peak_words;
    int peak_backpressure_percent;
} ddd_fifo_telemetry_totals_t;

typedef enum ddd_protocol_result {
    DDD_PROTOCOL_OK = 0,
    DDD_PROTOCOL_INVALID_ARGUMENT,
    DDD_PROTOCOL_UNSUPPORTED_PROFILE,
    DDD_PROTOCOL_UNSUPPORTED_DECIMATION,
    DDD_PROTOCOL_CONTROL_FAILURE,
    DDD_PROTOCOL_IDENTITY_MISMATCH,
    DDD_PROTOCOL_READBACK_MISMATCH
} ddd_protocol_result_t;

typedef struct ddd_collection_state {
    ddd_device_profile_t profile;
    uint8_t decimation_factor;
    uint32_t sample_rate_hz;
    bool test_mode;
    bool configured;
    bool collection_start_attempted;
    bool collection_active;
    bool rollback_attempted;
    bool rollback_succeeded;
    uint8_t identity[DDD_IDENTITY_LENGTH];
} ddd_collection_state_t;

void ddd_collection_state_init(ddd_collection_state_t *state);

void ddd_fifo_telemetry_init(ddd_fifo_telemetry_t *telemetry);
bool ddd_fifo_telemetry_parse(const uint8_t *block,
                              size_t block_length,
                              ddd_fifo_telemetry_t *telemetry);
int ddd_fifo_backpressure_percent(const ddd_fifo_telemetry_t *telemetry);
int ddd_fifo_peak_percent(const ddd_fifo_telemetry_t *telemetry);
int ddd_fifo_used_percent(const ddd_fifo_telemetry_t *telemetry);
void ddd_fifo_telemetry_totals_init(
    ddd_fifo_telemetry_totals_t *totals);
bool ddd_fifo_telemetry_totals_add(
    ddd_fifo_telemetry_totals_t *totals,
    const ddd_fifo_telemetry_t *telemetry);

/* Protocol-v1 order: B7(identity), B8(test), B8(decimation), B7(test),
 * B7(decimation), B5(start). Every partial start is rolled back to B5(stop),
 * test=0 and decimation=1 with verified readback. */
ddd_protocol_result_t ddd_collection_start_v1(
    const ddd_control_ops_t *ops,
    bool test_mode,
    uint8_t decimation_factor,
    ddd_collection_state_t *state);
ddd_protocol_result_t ddd_collection_stop_v1(
    const ddd_control_ops_t *ops,
    ddd_collection_state_t *state);
ddd_protocol_result_t ddd_collection_rollback_v1(
    const ddd_control_ops_t *ops,
    ddd_collection_state_t *state);

typedef enum ddd_validation_result {
    DDD_VALIDATION_OK = 0,
    DDD_VALIDATION_MISMATCH,
    DDD_VALIDATION_INVALID_ARGUMENT
} ddd_validation_result_t;

typedef enum ddd_sequence_phase {
    DDD_SEQUENCE_SYNCHRONIZING = 0,
    DDD_SEQUENCE_RUNNING,
    DDD_SEQUENCE_FAILED
} ddd_sequence_phase_t;

typedef struct ddd_sequence_validator {
    ddd_sequence_phase_t phase;
    bool marker_seen;
    uint8_t marker;
    uint32_t samples_in_marker;
    uint64_t samples_seen;
    uint64_t error_sample_index;
    uint8_t expected_marker;
    uint8_t actual_marker;
} ddd_sequence_validator_t;

void ddd_sequence_validator_init(ddd_sequence_validator_t *state);
ddd_validation_result_t ddd_sequence_validator_feed(
    ddd_sequence_validator_t *state,
    const uint16_t *sample_words,
    size_t sample_count);

typedef struct ddd_test_ramp_validator {
    bool armed;
    bool failed;
    bool wrap_detected;
    uint16_t expected_next;
    uint16_t wrap_value;
    uint64_t samples_seen;
    uint64_t error_sample_index;
    uint16_t expected_value;
    uint16_t actual_value;
} ddd_test_ramp_validator_t;

void ddd_test_ramp_validator_init(ddd_test_ramp_validator_t *state);
ddd_validation_result_t ddd_test_ramp_validator_feed(
    ddd_test_ramp_validator_t *state,
    const uint16_t *sample_words,
    size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif /* MISRC_DDD_PROTOCOL_H */
