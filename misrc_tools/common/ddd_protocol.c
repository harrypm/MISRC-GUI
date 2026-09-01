/* MISRC Common - Domesday Duplicator firmware protocol helpers. */

#include "ddd_protocol.h"

#include <stdio.h>
#include <string.h>

static bool ddd_control_ops_valid(const ddd_control_ops_t *ops)
{
    return ops != NULL && ops->transfer != NULL;
}

static bool ddd_control_out(const ddd_control_ops_t *ops,
                            uint8_t request,
                            uint16_t value)
{
    return ops->transfer(ops->context,
                         DDD_USB_REQUEST_VENDOR_OUT,
                         request,
                         value,
                         0,
                         NULL,
                         0) == 0;
}

static bool ddd_control_in(const ddd_control_ops_t *ops,
                           uint8_t request,
                           uint16_t value,
                           uint8_t *data,
                           uint16_t length)
{
    return ops->transfer(ops->context,
                         DDD_USB_REQUEST_VENDOR_IN,
                         request,
                         value,
                         0,
                         data,
                         length) == (int)length;
}

static bool ddd_write_register(const ddd_control_ops_t *ops,
                               uint8_t address,
                               uint8_t value)
{
    return ddd_control_out(ops, DDD_REQUEST_REGISTER_WRITE,
                           ddd_make_register_write(address, value));
}

static bool ddd_read_register(const ddd_control_ops_t *ops,
                              uint8_t address,
                              uint8_t *value)
{
    return ddd_control_in(ops, DDD_REQUEST_REGISTER_READ, address, value, 1);
}

static uint16_t ddd_read_little_endian_word(const uint8_t *block,
                                             size_t offset)
{
    return (uint16_t)(block[offset] |
                      ((uint16_t)block[offset + 1u] << 8));
}

bool ddd_is_known_device_id(uint16_t vendor_id, uint16_t product_id)
{
    return (vendor_id == DDD_LEGACY_VENDOR_ID &&
            product_id == DDD_LEGACY_PRODUCT_ID) ||
           (vendor_id == DDD_CURRENT_VENDOR_ID &&
            product_id == DDD_CURRENT_PRODUCT_ID);
}

ddd_device_profile_t ddd_classify_device(uint16_t vendor_id,
                                         uint16_t product_id,
                                         uint16_t bcd_device)
{
    if (vendor_id == DDD_LEGACY_VENDOR_ID &&
        product_id == DDD_LEGACY_PRODUCT_ID) {
        return DDD_DEVICE_LEGACY;
    }
    if (vendor_id == DDD_CURRENT_VENDOR_ID &&
        product_id == DDD_CURRENT_PRODUCT_ID) {
        return (uint8_t)(bcd_device >> 8) == DDD_SUPPORTED_PROTOCOL_VERSION
            ? DDD_DEVICE_PROTOCOL_V1
            : DDD_DEVICE_UNSUPPORTED;
    }
    return DDD_DEVICE_NOT_DDD;
}

bool ddd_profile_can_capture(ddd_device_profile_t profile)
{
    return profile == DDD_DEVICE_LEGACY ||
           profile == DDD_DEVICE_PROTOCOL_V1;
}

bool ddd_profile_requires_usb_path(ddd_device_profile_t profile)
{
    return profile == DDD_DEVICE_PROTOCOL_V1;
}

bool ddd_clockgen_candidate_is_preferred(
    bool selection_present,
    ddd_device_profile_t selected_profile,
    ddd_device_profile_t candidate_profile,
    bool candidate_capture_supported)
{
    if (!candidate_capture_supported ||
        !ddd_profile_can_capture(candidate_profile)) {
        return false;
    }
    if (!selection_present) return true;
    return selected_profile == DDD_DEVICE_PROTOCOL_V1 &&
           candidate_profile == DDD_DEVICE_LEGACY;
}

bool ddd_reconnect_path_matches(
    ddd_device_profile_t target_profile,
    const char *target_usb_path,
    ddd_device_profile_t candidate_profile,
    const char *candidate_usb_path)
{
    return ddd_profile_requires_usb_path(target_profile) &&
           target_profile == candidate_profile &&
           target_usb_path != NULL && target_usb_path[0] != '\0' &&
           candidate_usb_path != NULL && candidate_usb_path[0] != '\0' &&
           strcmp(target_usb_path, candidate_usb_path) == 0;
}

void ddd_profile_index_state_init(ddd_profile_index_state_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

int ddd_profile_index_take(ddd_profile_index_state_t *state,
                           ddd_device_profile_t profile)
{
    if (!state) return -1;
    switch (profile) {
        case DDD_DEVICE_LEGACY: return state->legacy_count++;
        case DDD_DEVICE_PROTOCOL_V1: return state->protocol_v1_count++;
        case DDD_DEVICE_UNSUPPORTED: return state->unsupported_count++;
        case DDD_DEVICE_NOT_DDD: return -1;
    }
    return -1;
}

bool ddd_v1_link_speed_allowed(bool speed_known, bool at_least_superspeed)
{
    return !speed_known || at_least_superspeed;
}

bool ddd_decimation_is_supported(uint8_t factor)
{
    return factor == DDD_DECIMATION_FULL_RATE ||
           factor == DDD_DECIMATION_HALF_RATE;
}

bool ddd_profile_supports_decimation(ddd_device_profile_t profile,
                                     uint8_t factor)
{
    if (profile == DDD_DEVICE_LEGACY) {
        return factor == DDD_DECIMATION_FULL_RATE;
    }
    return profile == DDD_DEVICE_PROTOCOL_V1 &&
           ddd_decimation_is_supported(factor);
}

uint16_t ddd_make_register_write(uint8_t address, uint8_t value)
{
    return (uint16_t)(((uint16_t)address << 8) | value);
}

uint32_t ddd_sample_rate_hz(uint8_t factor)
{
    return ddd_decimation_is_supported(factor)
        ? DDD_CONVERTER_SAMPLE_RATE_HZ / factor
        : 0;
}

uint32_t ddd_sample_rate_khz(uint8_t factor)
{
    return ddd_sample_rate_hz(factor) / UINT32_C(1000);
}

uint32_t ddd_v1_effective_output_rate_khz(
    bool resample_enabled,
    uint32_t resample_rate_khz,
    uint8_t stored_decimation_factor)
{
    uint32_t hardware_rate_khz = ddd_sample_rate_khz(
        stored_decimation_factor);
    if (hardware_rate_khz == 0) return 0;
    if (!resample_enabled || resample_rate_khz == 0 ||
        resample_rate_khz >= hardware_rate_khz) {
        return hardware_rate_khz;
    }
    return resample_rate_khz;
}

bool ddd_v1_plan_output_rate_khz(uint32_t output_rate_khz,
                                 ddd_v1_rate_plan_t *plan)
{
    uint8_t factor;
    uint32_t hardware_rate_khz;

    if (!plan || output_rate_khz == 0 ||
        output_rate_khz > ddd_sample_rate_khz(
            DDD_DECIMATION_FULL_RATE)) {
        return false;
    }

    factor = output_rate_khz <= ddd_sample_rate_khz(
        DDD_DECIMATION_HALF_RATE)
        ? DDD_DECIMATION_HALF_RATE
        : DDD_DECIMATION_FULL_RATE;
    hardware_rate_khz = ddd_sample_rate_khz(factor);
    plan->decimation_factor = factor;
    plan->hardware_rate_khz = hardware_rate_khz;
    plan->output_rate_khz = output_rate_khz;
    plan->software_resample = output_rate_khz < hardware_rate_khz;
    return true;
}

bool ddd_identity_is_supported(const uint8_t *identity, size_t length)
{
    return identity != NULL && length >= DDD_IDENTITY_LENGTH &&
           identity[DDD_REGISTER_IDENTITY] == DDD_IDENTITY_VALUE &&
           identity[DDD_REGISTER_MAP_VERSION] ==
               DDD_SUPPORTED_REGISTER_MAP &&
           identity[DDD_REGISTER_IMAGE_ROLE] ==
               DDD_APPLICATION_IMAGE_ROLE;
}

bool ddd_format_gateware_commit(const uint8_t *identity,
                                size_t identity_length,
                                char *destination,
                                size_t destination_size)
{
    size_t commit_length = 0;
    bool dirty;

    if (!destination || destination_size == 0) return false;
    destination[0] = '\0';
    if (!identity || identity_length < DDD_IDENTITY_LENGTH ||
        (identity[DDD_REGISTER_BUILD_FLAGS] & DDD_BUILD_COMMIT_FLAG) == 0) {
        return false;
    }
    dirty = (identity[DDD_REGISTER_BUILD_FLAGS] & DDD_BUILD_DIRTY_FLAG) != 0;
    while (commit_length < DDD_COMMIT_LENGTH) {
        uint8_t value = identity[DDD_REGISTER_COMMIT + commit_length];
        bool is_hex = (value >= (uint8_t)'0' && value <= (uint8_t)'9') ||
                      (value >= (uint8_t)'a' && value <= (uint8_t)'f') ||
                      (value >= (uint8_t)'A' && value <= (uint8_t)'F');
        if (!is_hex) break;
        ++commit_length;
    }
    if (commit_length == 0 ||
        commit_length + (dirty ? 6u : 0u) + 1u > destination_size) {
        return false;
    }
    memcpy(destination, &identity[DDD_REGISTER_COMMIT], commit_length);
    if (dirty) {
        memcpy(destination + commit_length, "-dirty", 6u);
        commit_length += 6u;
    }
    destination[commit_length] = '\0';
    return true;
}

bool ddd_format_usb_topology_path(uint8_t bus_number,
                                  const uint8_t *ports,
                                  int port_count,
                                  char *destination,
                                  size_t destination_size)
{
    int written;
    size_t used;

    if (!destination || destination_size == 0 || !ports || port_count <= 0) {
        return false;
    }
    written = snprintf(destination, destination_size, "usb:%u-", bus_number);
    if (written < 0 || (size_t)written >= destination_size) return false;
    used = (size_t)written;
    for (int i = 0; i < port_count; ++i) {
        written = snprintf(destination + used, destination_size - used,
                           i == 0 ? "%u" : ".%u", ports[i]);
        if (written < 0 || (size_t)written >= destination_size - used) {
            destination[0] = '\0';
            return false;
        }
        used += (size_t)written;
    }
    return true;
}

void ddd_stream_selector_init(ddd_stream_selector_t *selector,
                              ddd_device_profile_t profile)
{
    if (!selector) return;
    memset(selector, 0, sizeof(*selector));
    selector->profile = profile;
    selector->selected.interface_number = DDD_STREAM_INTERFACE_NUMBER;
    selector->selected.alternate_setting = DDD_STREAM_ALTERNATE_SETTING;
    selector->selected.endpoint_address = DDD_STREAM_ENDPOINT_ADDRESS;
}

void ddd_stream_selector_consider(
    ddd_stream_selector_t *selector,
    const ddd_stream_endpoint_candidate_t *candidate)
{
    if (!selector || !candidate || !candidate->is_bulk || !candidate->is_in) {
        return;
    }
    if (selector->profile == DDD_DEVICE_PROTOCOL_V1) {
        bool exact = candidate->interface_number == DDD_STREAM_INTERFACE_NUMBER &&
                     candidate->alternate_setting == DDD_STREAM_ALTERNATE_SETTING &&
                     candidate->endpoint_address == DDD_STREAM_ENDPOINT_ADDRESS &&
                     candidate->max_packet_size == DDD_STREAM_MAX_PACKET_SIZE;
        if (!exact) return;
        ++selector->protocol_v1_exact_matches;
        if (selector->protocol_v1_exact_matches == 1) {
            selector->selected.interface_number = candidate->interface_number;
            selector->selected.alternate_setting = candidate->alternate_setting;
            selector->selected.endpoint_address = candidate->endpoint_address;
            selector->selected.max_packet_size = candidate->max_packet_size;
            selector->selected.found = true;
        } else {
            selector->selected.found = false;
        }
        return;
    }
    if (selector->profile == DDD_DEVICE_LEGACY &&
        (!selector->selected.found ||
         candidate->max_packet_size > selector->selected.max_packet_size)) {
        selector->selected.interface_number = candidate->interface_number;
        selector->selected.alternate_setting = candidate->alternate_setting;
        selector->selected.endpoint_address = candidate->endpoint_address;
        selector->selected.max_packet_size = candidate->max_packet_size;
        selector->selected.found = true;
    }
}

bool ddd_stream_selector_get(const ddd_stream_selector_t *selector,
                             ddd_stream_path_t *selected)
{
    if (!selector || !selected) return false;
    *selected = selector->selected;
    if (selector->profile == DDD_DEVICE_PROTOCOL_V1 &&
        selector->protocol_v1_exact_matches != 1) {
        selected->found = false;
    }
    return selected->found;
}

void ddd_collection_state_init(ddd_collection_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->profile = DDD_DEVICE_NOT_DDD;
}

void ddd_fifo_telemetry_init(ddd_fifo_telemetry_t *telemetry)
{
    if (telemetry) memset(telemetry, 0, sizeof(*telemetry));
}

bool ddd_fifo_telemetry_parse(const uint8_t *block,
                              size_t block_length,
                              ddd_fifo_telemetry_t *telemetry)
{
    uint8_t status;
    uint8_t format;
    uint16_t depth;
    uint16_t packet;

    if (!telemetry) return false;
    ddd_fifo_telemetry_init(telemetry);
    if (!block || block_length < DDD_FIFO_TELEMETRY_LENGTH ||
        block[0] != DDD_FIFO_TELEMETRY_ID) {
        return false;
    }

    status = block[DDD_FIFO_OFFSET_STATUS];
    format = status & DDD_FIFO_TELEMETRY_FORMAT_MASK;
    if (format != DDD_FIFO_TELEMETRY_FORMAT) return false;

    depth = ddd_read_little_endian_word(block, DDD_FIFO_OFFSET_DEPTH);
    packet = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_PACKET_WORDS);
    if (packet == 0 || depth <= packet) return false;

    telemetry->present = true;
    telemetry->format = format;
    telemetry->overflow_seen =
        (status & DDD_FIFO_FLAG_OVERFLOW_SEEN) != 0;
    telemetry->saturated = (status & DDD_FIFO_FLAG_SATURATED) != 0;
    telemetry->latch_count = block[DDD_FIFO_OFFSET_LATCH_COUNT];
    telemetry->used_now = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_USED_NOW);
    telemetry->peak = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_PEAK);
    telemetry->peak_since_open = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_PEAK_LIFETIME);
    telemetry->overflow_events = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_OVERFLOWS);
    telemetry->dropped_words = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_DROPPED);
    telemetry->packets_read = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_PACKETS);
    telemetry->near_full_units = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_NEAR_FULL);
    telemetry->depth_words = depth;
    telemetry->packet_words = packet;
    telemetry->near_full_words = ddd_read_little_endian_word(
        block, DDD_FIFO_OFFSET_NEAR_FULL_WORDS);
    return true;
}

int ddd_fifo_backpressure_percent(const ddd_fifo_telemetry_t *telemetry)
{
    int headroom;
    int excursion;
    int percent;

    if (!telemetry || !telemetry->present) return 0;
    if (telemetry->overflow_events > 0) return 100;
    if (telemetry->peak <= telemetry->packet_words) return 0;

    headroom = (int)telemetry->depth_words - (int)telemetry->packet_words;
    if (headroom <= 0) return 0;
    excursion = (int)telemetry->peak - (int)telemetry->packet_words;
    percent = excursion * 100 / headroom;
    if (percent < 0) return 0;
    return percent > 100 ? 100 : percent;
}

int ddd_fifo_peak_percent(const ddd_fifo_telemetry_t *telemetry)
{
    int percent;
    if (!telemetry || !telemetry->present || telemetry->depth_words == 0) {
        return 0;
    }
    percent = (int)telemetry->peak * 100 / (int)telemetry->depth_words;
    if (percent < 0) return 0;
    return percent > 100 ? 100 : percent;
}

int ddd_fifo_used_percent(const ddd_fifo_telemetry_t *telemetry)
{
    int percent;
    if (!telemetry || !telemetry->present || telemetry->depth_words == 0) {
        return 0;
    }
    percent = (int)telemetry->used_now * 100 /
        (int)telemetry->depth_words;
    if (percent < 0) return 0;
    return percent > 100 ? 100 : percent;
}

void ddd_fifo_telemetry_totals_init(
    ddd_fifo_telemetry_totals_t *totals)
{
    if (!totals) return;
    memset(totals, 0, sizeof(*totals));
    totals->interval_coverage_complete = true;
}

bool ddd_fifo_telemetry_totals_add(
    ddd_fifo_telemetry_totals_t *totals,
    const ddd_fifo_telemetry_t *telemetry)
{
    uint8_t latch_delta;
    int backpressure;

    if (!totals || !telemetry || !telemetry->present) return false;
    if (totals->latch_seen) {
        latch_delta = (uint8_t)(telemetry->latch_count -
                                totals->last_latch_count);
        if (latch_delta == 0) return false;
        if (latch_delta != 1) totals->interval_coverage_complete = false;
    }

    totals->latch_seen = true;
    totals->last_latch_count = telemetry->latch_count;
    totals->saturated = totals->saturated || telemetry->saturated;
    totals->overflow_events += telemetry->overflow_events;
    totals->dropped_words += telemetry->dropped_words;
    totals->near_full_units += telemetry->near_full_units;
    if (telemetry->peak > totals->peak_words) {
        totals->peak_words = telemetry->peak;
    }
    backpressure = ddd_fifo_backpressure_percent(telemetry);
    if (backpressure > totals->peak_backpressure_percent) {
        totals->peak_backpressure_percent = backpressure;
    }
    return true;
}

static ddd_protocol_result_t ddd_restore_safe_defaults(
    const ddd_control_ops_t *ops)
{
    uint8_t test_mode = UINT8_MAX;
    uint8_t decimation = UINT8_MAX;
    bool transfer_failed = false;
    bool mismatch = false;

    if (!ddd_write_register(ops, DDD_REGISTER_TEST_MODE, 0)) {
        transfer_failed = true;
    }
    if (!ddd_write_register(ops, DDD_REGISTER_DECIMATION,
                            DDD_DECIMATION_FULL_RATE)) {
        transfer_failed = true;
    }
    if (!ddd_read_register(ops, DDD_REGISTER_TEST_MODE, &test_mode)) {
        transfer_failed = true;
    } else if (test_mode != 0) {
        mismatch = true;
    }
    if (!ddd_read_register(ops, DDD_REGISTER_DECIMATION, &decimation)) {
        transfer_failed = true;
    } else if (decimation != DDD_DECIMATION_FULL_RATE) {
        mismatch = true;
    }
    if (transfer_failed) return DDD_PROTOCOL_CONTROL_FAILURE;
    return mismatch ? DDD_PROTOCOL_READBACK_MISMATCH : DDD_PROTOCOL_OK;
}

ddd_protocol_result_t ddd_collection_rollback_v1(
    const ddd_control_ops_t *ops,
    ddd_collection_state_t *state)
{
    ddd_protocol_result_t result = DDD_PROTOCOL_OK;
    bool stop_ok = true;

    if (!state || !ddd_control_ops_valid(ops)) {
        return DDD_PROTOCOL_INVALID_ARGUMENT;
    }
    if (state->profile != DDD_DEVICE_PROTOCOL_V1) {
        return DDD_PROTOCOL_UNSUPPORTED_PROFILE;
    }
    state->rollback_attempted = true;
    if (state->collection_start_attempted || state->collection_active) {
        stop_ok = ddd_control_out(ops, DDD_REQUEST_COLLECTION, 0);
        if (!stop_ok) result = DDD_PROTOCOL_CONTROL_FAILURE;
    }
    ddd_protocol_result_t restore = ddd_restore_safe_defaults(ops);
    if (result == DDD_PROTOCOL_OK && restore != DDD_PROTOCOL_OK) result = restore;
    if (stop_ok) {
        state->collection_start_attempted = false;
        state->collection_active = false;
    }
    state->configured = false;
    state->rollback_succeeded = result == DDD_PROTOCOL_OK;
    return result;
}

ddd_protocol_result_t ddd_collection_start_v1(
    const ddd_control_ops_t *ops,
    bool test_mode,
    uint8_t decimation_factor,
    ddd_collection_state_t *state)
{
    uint8_t identity[DDD_IDENTITY_LENGTH] = {0};
    uint8_t readback = 0;

    if (!state || !ddd_control_ops_valid(ops)) {
        return DDD_PROTOCOL_INVALID_ARGUMENT;
    }
    ddd_collection_state_init(state);
    state->profile = DDD_DEVICE_PROTOCOL_V1;
    state->test_mode = test_mode;
    state->decimation_factor = decimation_factor;
    state->sample_rate_hz = ddd_sample_rate_hz(decimation_factor);
    if (!ddd_decimation_is_supported(decimation_factor)) {
        return DDD_PROTOCOL_UNSUPPORTED_DECIMATION;
    }
    if (!ddd_control_in(ops, DDD_REQUEST_REGISTER_READ,
                        DDD_REGISTER_IDENTITY, identity,
                        DDD_IDENTITY_LENGTH)) {
        return DDD_PROTOCOL_CONTROL_FAILURE;
    }
    if (!ddd_identity_is_supported(identity, sizeof(identity))) {
        return DDD_PROTOCOL_IDENTITY_MISMATCH;
    }
    memcpy(state->identity, identity, sizeof(state->identity));
    if (!ddd_write_register(ops, DDD_REGISTER_TEST_MODE,
                            test_mode ? 1 : 0) ||
        !ddd_write_register(ops, DDD_REGISTER_DECIMATION,
                            decimation_factor)) {
        (void)ddd_collection_rollback_v1(ops, state);
        return DDD_PROTOCOL_CONTROL_FAILURE;
    }
    if (!ddd_read_register(ops, DDD_REGISTER_TEST_MODE, &readback)) {
        (void)ddd_collection_rollback_v1(ops, state);
        return DDD_PROTOCOL_CONTROL_FAILURE;
    }
    if (readback != (test_mode ? 1 : 0)) {
        (void)ddd_collection_rollback_v1(ops, state);
        return DDD_PROTOCOL_READBACK_MISMATCH;
    }
    if (!ddd_read_register(ops, DDD_REGISTER_DECIMATION, &readback)) {
        (void)ddd_collection_rollback_v1(ops, state);
        return DDD_PROTOCOL_CONTROL_FAILURE;
    }
    if (readback != decimation_factor) {
        (void)ddd_collection_rollback_v1(ops, state);
        return DDD_PROTOCOL_READBACK_MISMATCH;
    }
    state->configured = true;
    state->collection_start_attempted = true;
    if (!ddd_control_out(ops, DDD_REQUEST_COLLECTION, 1)) {
        (void)ddd_collection_rollback_v1(ops, state);
        return DDD_PROTOCOL_CONTROL_FAILURE;
    }
    state->collection_active = true;
    return DDD_PROTOCOL_OK;
}

ddd_protocol_result_t ddd_collection_stop_v1(
    const ddd_control_ops_t *ops,
    ddd_collection_state_t *state)
{
    if (!state || !ddd_control_ops_valid(ops)) {
        return DDD_PROTOCOL_INVALID_ARGUMENT;
    }
    if (state->profile != DDD_DEVICE_PROTOCOL_V1) {
        return DDD_PROTOCOL_UNSUPPORTED_PROFILE;
    }
    if (!state->collection_start_attempted && !state->collection_active) {
        return DDD_PROTOCOL_OK;
    }
    if (!ddd_control_out(ops, DDD_REQUEST_COLLECTION, 0)) {
        return DDD_PROTOCOL_CONTROL_FAILURE;
    }
    state->collection_start_attempted = false;
    state->collection_active = false;
    return DDD_PROTOCOL_OK;
}

void ddd_sequence_validator_init(ddd_sequence_validator_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->phase = DDD_SEQUENCE_SYNCHRONIZING;
}

static uint8_t ddd_next_sequence_marker(uint8_t marker)
{
    ++marker;
    return marker == DDD_SEQUENCE_MARKER_COUNT ? 0 : marker;
}

static ddd_validation_result_t ddd_sequence_fail(
    ddd_sequence_validator_t *state, uint8_t expected, uint8_t actual)
{
    state->phase = DDD_SEQUENCE_FAILED;
    state->error_sample_index = state->samples_seen;
    state->expected_marker = expected;
    state->actual_marker = actual;
    return DDD_VALIDATION_MISMATCH;
}

ddd_validation_result_t ddd_sequence_validator_feed(
    ddd_sequence_validator_t *state,
    const uint16_t *sample_words,
    size_t sample_count)
{
    if (!state || (sample_count != 0 && !sample_words)) {
        return DDD_VALIDATION_INVALID_ARGUMENT;
    }
    if (state->phase == DDD_SEQUENCE_FAILED) return DDD_VALIDATION_MISMATCH;
    for (size_t i = 0; i < sample_count; ++i) {
        uint8_t actual = (uint8_t)((sample_words[i] >> 10) & 0x3Fu);
        if (actual >= DDD_SEQUENCE_MARKER_COUNT) {
            return ddd_sequence_fail(state, 0, actual);
        }
        if (!state->marker_seen) {
            state->marker_seen = true;
            state->marker = actual;
            state->samples_in_marker = 1;
            ++state->samples_seen;
            continue;
        }
        if (state->phase == DDD_SEQUENCE_SYNCHRONIZING) {
            if (actual == state->marker) {
                if (state->samples_in_marker == DDD_SEQUENCE_SAMPLES_PER_MARKER) {
                    return ddd_sequence_fail(
                        state, ddd_next_sequence_marker(state->marker), actual);
                }
                ++state->samples_in_marker;
            } else {
                uint8_t expected = ddd_next_sequence_marker(state->marker);
                if (actual != expected) {
                    return ddd_sequence_fail(state, expected, actual);
                }
                state->phase = DDD_SEQUENCE_RUNNING;
                state->marker = actual;
                state->samples_in_marker = 1;
            }
            ++state->samples_seen;
            continue;
        }
        if (actual == state->marker) {
            if (state->samples_in_marker == DDD_SEQUENCE_SAMPLES_PER_MARKER) {
                return ddd_sequence_fail(
                    state, ddd_next_sequence_marker(state->marker), actual);
            }
            ++state->samples_in_marker;
        } else {
            if (state->samples_in_marker != DDD_SEQUENCE_SAMPLES_PER_MARKER) {
                return ddd_sequence_fail(state, state->marker, actual);
            }
            uint8_t expected = ddd_next_sequence_marker(state->marker);
            if (actual != expected) {
                return ddd_sequence_fail(state, expected, actual);
            }
            state->marker = actual;
            state->samples_in_marker = 1;
        }
        ++state->samples_seen;
    }
    return DDD_VALIDATION_OK;
}

void ddd_test_ramp_validator_init(ddd_test_ramp_validator_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

static ddd_validation_result_t ddd_test_ramp_fail(
    ddd_test_ramp_validator_t *state, uint16_t expected, uint16_t actual)
{
    state->failed = true;
    state->error_sample_index = state->samples_seen;
    state->expected_value = expected;
    state->actual_value = actual;
    return DDD_VALIDATION_MISMATCH;
}

ddd_validation_result_t ddd_test_ramp_validator_feed(
    ddd_test_ramp_validator_t *state,
    const uint16_t *sample_words,
    size_t sample_count)
{
    if (!state || (sample_count != 0 && !sample_words)) {
        return DDD_VALIDATION_INVALID_ARGUMENT;
    }
    if (state->failed) return DDD_VALIDATION_MISMATCH;
    for (size_t i = 0; i < sample_count; ++i) {
        uint16_t actual = sample_words[i] & UINT16_C(0x03FF);
        if (!state->armed) {
            state->armed = true;
            state->expected_next = (uint16_t)(actual + 1u);
        } else if (!state->wrap_detected && actual == 0 &&
                   (state->expected_next == DDD_TEST_RAMP_NEW_WRAP ||
                    state->expected_next == DDD_TEST_RAMP_LEGACY_WRAP)) {
            state->wrap_detected = true;
            state->wrap_value = state->expected_next;
            state->expected_next = 1;
        } else {
            if (actual != state->expected_next) {
                return ddd_test_ramp_fail(state, state->expected_next, actual);
            }
            ++state->expected_next;
            if (state->wrap_detected &&
                state->expected_next == state->wrap_value) {
                state->expected_next = 0;
            }
        }
        ++state->samples_seen;
    }
    return DDD_VALIDATION_OK;
}
