#include "../common/ddd_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

typedef struct {
    uint8_t registers[256];
    uint8_t requests[16];
    uint16_t values[16];
    size_t request_count;
    int fail_at;
} mock_usb_t;

static void mock_usb_init(mock_usb_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->registers[DDD_REGISTER_IDENTITY] = DDD_IDENTITY_VALUE;
    mock->registers[DDD_REGISTER_MAP_VERSION] =
        DDD_SUPPORTED_REGISTER_MAP;
    mock->registers[DDD_REGISTER_IMAGE_ROLE] =
        DDD_APPLICATION_IMAGE_ROLE;
    mock->registers[DDD_REGISTER_BUILD_FLAGS] = DDD_BUILD_COMMIT_FLAG;
    memcpy(&mock->registers[DDD_REGISTER_COMMIT], "deadbeef", 8);
    mock->fail_at = -1;
}

static int mock_transfer(void *context,
                         uint8_t request_type,
                         uint8_t request,
                         uint16_t value,
                         uint16_t index,
                         uint8_t *data,
                         uint16_t length)
{
    mock_usb_t *mock = (mock_usb_t *)context;
    size_t call = mock->request_count;
    (void)index;
    if (call < sizeof(mock->requests)) {
        mock->requests[call] = request;
        mock->values[call] = value;
    }
    ++mock->request_count;
    if ((int)call == mock->fail_at) return -1;
    if (request_type == DDD_USB_REQUEST_VENDOR_IN &&
        request == DDD_REQUEST_REGISTER_READ) {
        memcpy(data, &mock->registers[value & 0xffu], length);
        return length;
    }
    if (request_type == DDD_USB_REQUEST_VENDOR_OUT &&
        request == DDD_REQUEST_REGISTER_WRITE) {
        mock->registers[(value >> 8) & 0xffu] = value & 0xffu;
        return 0;
    }
    if (request_type == DDD_USB_REQUEST_VENDOR_OUT &&
        request == DDD_REQUEST_COLLECTION) {
        return 0;
    }
    return -1;
}

static bool test_profiles_and_rates(void)
{
    ddd_profile_index_state_t indices;
    CHECK(ddd_classify_device(DDD_LEGACY_VENDOR_ID,
                              DDD_LEGACY_PRODUCT_ID, 0) ==
          DDD_DEVICE_LEGACY);
    CHECK(ddd_classify_device(DDD_CURRENT_VENDOR_ID,
                              DDD_CURRENT_PRODUCT_ID, 0x0100) ==
          DDD_DEVICE_PROTOCOL_V1);
    CHECK(ddd_classify_device(DDD_CURRENT_VENDOR_ID,
                              DDD_CURRENT_PRODUCT_ID, 0x0200) ==
          DDD_DEVICE_UNSUPPORTED);
    CHECK(ddd_classify_device(0xffff, 0xffff, 0) == DDD_DEVICE_NOT_DDD);
    CHECK(ddd_profile_supports_decimation(DDD_DEVICE_LEGACY, 1));
    CHECK(!ddd_profile_supports_decimation(DDD_DEVICE_LEGACY, 2));
    CHECK(ddd_profile_supports_decimation(DDD_DEVICE_PROTOCOL_V1, 1));
    CHECK(ddd_profile_supports_decimation(DDD_DEVICE_PROTOCOL_V1, 2));
    CHECK(!ddd_profile_requires_usb_path(DDD_DEVICE_LEGACY));
    CHECK(ddd_profile_requires_usb_path(DDD_DEVICE_PROTOCOL_V1));
    CHECK(!ddd_profile_requires_usb_path(DDD_DEVICE_UNSUPPORTED));
    CHECK(ddd_sample_rate_hz(1) == 40000000u);
    CHECK(ddd_sample_rate_hz(2) == 20000000u);
    CHECK(ddd_sample_rate_hz(3) == 0);
    CHECK(ddd_v1_link_speed_allowed(false, false));
    CHECK(!ddd_v1_link_speed_allowed(true, false));
    CHECK(ddd_v1_link_speed_allowed(true, true));

    ddd_profile_index_state_init(&indices);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_PROTOCOL_V1) == 0);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_LEGACY) == 0);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_UNSUPPORTED) == 0);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_PROTOCOL_V1) == 1);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_LEGACY) == 1);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_NOT_DDD) == -1);
    return true;
}

static bool test_v1_rate_plans(void)
{
    ddd_v1_rate_plan_t plan;
    static const uint32_t software_rates_khz[] = {
        5000u, 10000u, 14300u, 17900u
    };

    CHECK(ddd_v1_plan_output_rate_khz(40000u, &plan));
    CHECK(plan.decimation_factor == DDD_DECIMATION_FULL_RATE);
    CHECK(plan.hardware_rate_khz == 40000u);
    CHECK(plan.output_rate_khz == 40000u);
    CHECK(!plan.software_resample);

    CHECK(ddd_v1_plan_output_rate_khz(20000u, &plan));
    CHECK(plan.decimation_factor == DDD_DECIMATION_HALF_RATE);
    CHECK(plan.hardware_rate_khz == 20000u);
    CHECK(plan.output_rate_khz == 20000u);
    CHECK(!plan.software_resample);

    for (size_t i = 0;
         i < sizeof(software_rates_khz) / sizeof(software_rates_khz[0]);
         ++i) {
        CHECK(ddd_v1_plan_output_rate_khz(
            software_rates_khz[i], &plan));
        CHECK(plan.decimation_factor == DDD_DECIMATION_HALF_RATE);
        CHECK(plan.hardware_rate_khz == 20000u);
        CHECK(plan.output_rate_khz == software_rates_khz[i]);
        CHECK(plan.software_resample);
    }

    /* Preserve hand-edited intermediate rates without ever upsampling. */
    CHECK(ddd_v1_plan_output_rate_khz(30000u, &plan));
    CHECK(plan.decimation_factor == DDD_DECIMATION_FULL_RATE);
    CHECK(plan.hardware_rate_khz == 40000u);
    CHECK(plan.output_rate_khz == 30000u);
    CHECK(plan.software_resample);

    CHECK(!ddd_v1_plan_output_rate_khz(0, &plan));
    CHECK(!ddd_v1_plan_output_rate_khz(40001u, &plan));
    CHECK(!ddd_v1_plan_output_rate_khz(20000u, NULL));

    CHECK(ddd_v1_effective_output_rate_khz(
              false, 5000u, DDD_DECIMATION_FULL_RATE) == 40000u);
    CHECK(ddd_v1_effective_output_rate_khz(
              false, 5000u, DDD_DECIMATION_HALF_RATE) == 20000u);
    CHECK(ddd_v1_effective_output_rate_khz(
              true, 10000u, DDD_DECIMATION_FULL_RATE) == 10000u);
    CHECK(ddd_v1_effective_output_rate_khz(
              true, 30000u, DDD_DECIMATION_HALF_RATE) == 20000u);
    CHECK(ddd_v1_effective_output_rate_khz(
              true, 20000u, DDD_DECIMATION_HALF_RATE) == 20000u);
    CHECK(ddd_v1_effective_output_rate_khz(
              true, 10000u, 3) == 0);
    return true;
}

static int select_clockgen_candidate(
    const ddd_device_profile_t *profiles,
    const bool *capture_supported,
    size_t count)
{
    bool selection_present = false;
    ddd_device_profile_t selected_profile = DDD_DEVICE_NOT_DDD;
    int selected_index = -1;
    for (size_t i = 0; i < count; ++i) {
        if (ddd_clockgen_candidate_is_preferred(
                selection_present, selected_profile, profiles[i],
                capture_supported[i])) {
            selection_present = true;
            selected_profile = profiles[i];
            selected_index = (int)i;
        }
    }
    return selected_index;
}

static bool test_clockgen_profile_selection(void)
{
    static const ddd_device_profile_t legacy_then_v1[] = {
        DDD_DEVICE_LEGACY, DDD_DEVICE_PROTOCOL_V1
    };
    static const ddd_device_profile_t v1_then_legacy[] = {
        DDD_DEVICE_PROTOCOL_V1, DDD_DEVICE_LEGACY
    };
    static const ddd_device_profile_t only_v1[] = {
        DDD_DEVICE_PROTOCOL_V1
    };
    static const ddd_device_profile_t unavailable_then_legacy[] = {
        DDD_DEVICE_PROTOCOL_V1, DDD_DEVICE_LEGACY
    };
    static const ddd_device_profile_t unsupported[] = {
        DDD_DEVICE_UNSUPPORTED
    };
    static const bool both_supported[] = {true, true};
    static const bool one_supported[] = {true};
    static const bool second_supported[] = {false, true};
    static const bool none_supported[] = {false};

    CHECK(select_clockgen_candidate(legacy_then_v1, both_supported, 2) == 0);
    CHECK(select_clockgen_candidate(v1_then_legacy, both_supported, 2) == 1);
    CHECK(select_clockgen_candidate(only_v1, one_supported, 1) == 0);
    CHECK(select_clockgen_candidate(
              unavailable_then_legacy, second_supported, 2) == 1);
    CHECK(select_clockgen_candidate(unsupported, none_supported, 1) == -1);
    return true;
}

static int select_reconnect_candidate(
    ddd_device_profile_t target_profile,
    const char *target_usb_path,
    const ddd_device_profile_t *candidate_profiles,
    const char *const *candidate_usb_paths,
    size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (ddd_reconnect_path_matches(
                target_profile, target_usb_path,
                candidate_profiles[i], candidate_usb_paths[i])) {
            return (int)i;
        }
    }
    return -1;
}

static bool test_reconnect_path_selection(void)
{
    static const ddd_device_profile_t two_v1[] = {
        DDD_DEVICE_PROTOCOL_V1, DDD_DEVICE_PROTOCOL_V1
    };
    static const char *const path_b_then_a[] = {
        "usb:1-4", "usb:1-3"
    };
    static const char *const path_a_then_b[] = {
        "usb:1-3", "usb:1-4"
    };
    static const char *const other_paths[] = {
        "usb:1-4", "usb:1-5"
    };

    CHECK(select_reconnect_candidate(
              DDD_DEVICE_PROTOCOL_V1, "usb:1-4", two_v1,
              path_b_then_a, 2) == 0);
    CHECK(select_reconnect_candidate(
              DDD_DEVICE_PROTOCOL_V1, "usb:1-4", two_v1,
              path_a_then_b, 2) == 1);
    CHECK(select_reconnect_candidate(
              DDD_DEVICE_PROTOCOL_V1, "usb:1-3", two_v1,
              other_paths, 2) == -1);
    CHECK(!ddd_reconnect_path_matches(
              DDD_DEVICE_PROTOCOL_V1, "usb:1-3",
              DDD_DEVICE_LEGACY, "usb:1-3"));
    CHECK(!ddd_reconnect_path_matches(
              DDD_DEVICE_PROTOCOL_V1, "",
              DDD_DEVICE_PROTOCOL_V1, "usb:1-3"));
    CHECK(!ddd_reconnect_path_matches(
              DDD_DEVICE_PROTOCOL_V1, NULL,
              DDD_DEVICE_PROTOCOL_V1, "usb:1-3"));
    CHECK(!ddd_reconnect_path_matches(
              DDD_DEVICE_LEGACY, "usb:1-3",
              DDD_DEVICE_LEGACY, "usb:1-3"));
    return true;
}

static bool test_topology_and_endpoint(void)
{
    uint8_t ports[] = {3, 2, 7};
    char path[32];
    ddd_stream_selector_t selector;
    ddd_stream_path_t selected;
    ddd_stream_endpoint_candidate_t wrong = {
        .interface_number = 0, .alternate_setting = 0,
        .endpoint_address = 0x81, .max_packet_size = 512,
        .is_bulk = true, .is_in = true
    };
    ddd_stream_endpoint_candidate_t exact = {
        .interface_number = 0, .alternate_setting = 0,
        .endpoint_address = 0x81, .max_packet_size = 1024,
        .is_bulk = true, .is_in = true
    };
    CHECK(ddd_format_usb_topology_path(1, ports, 3, path, sizeof(path)));
    CHECK(strcmp(path, "usb:1-3.2.7") == 0);
    ddd_stream_selector_init(&selector, DDD_DEVICE_PROTOCOL_V1);
    ddd_stream_selector_consider(&selector, &wrong);
    CHECK(!ddd_stream_selector_get(&selector, &selected));
    ddd_stream_selector_consider(&selector, &exact);
    CHECK(ddd_stream_selector_get(&selector, &selected));
    ddd_stream_selector_consider(&selector, &exact);
    CHECK(!ddd_stream_selector_get(&selector, &selected));
    return true;
}

static bool test_lifecycle(void)
{
    mock_usb_t mock;
    ddd_collection_state_t state;
    ddd_control_ops_t ops = {.transfer = mock_transfer, .context = &mock};

    mock_usb_init(&mock);
    CHECK(ddd_collection_start_v1(&ops, true, 2, &state) ==
          DDD_PROTOCOL_OK);
    CHECK(state.collection_active);
    CHECK(state.sample_rate_hz == 20000000u);
    CHECK(mock.request_count == 6);
    CHECK(mock.requests[0] == DDD_REQUEST_REGISTER_READ);
    CHECK(mock.requests[1] == DDD_REQUEST_REGISTER_WRITE);
    CHECK(mock.values[1] == ddd_make_register_write(
        DDD_REGISTER_TEST_MODE, 1));
    CHECK(mock.requests[2] == DDD_REQUEST_REGISTER_WRITE);
    CHECK(mock.values[2] == ddd_make_register_write(
        DDD_REGISTER_DECIMATION, 2));
    CHECK(mock.requests[5] == DDD_REQUEST_COLLECTION);
    CHECK(mock.values[5] == 1);
    CHECK(ddd_collection_stop_v1(&ops, &state) == DDD_PROTOCOL_OK);
    CHECK(!state.collection_active);

    mock_usb_init(&mock);
    mock.fail_at = 2;
    CHECK(ddd_collection_start_v1(&ops, true, 2, &state) ==
          DDD_PROTOCOL_CONTROL_FAILURE);
    CHECK(state.rollback_attempted);
    CHECK(state.rollback_succeeded);
    CHECK(mock.registers[DDD_REGISTER_TEST_MODE] == 0);
    CHECK(mock.registers[DDD_REGISTER_DECIMATION] == 1);
    return true;
}

static void put_word(uint8_t *block, size_t offset, uint16_t value)
{
    block[offset] = (uint8_t)(value & 0xffu);
    block[offset + 1u] = (uint8_t)(value >> 8);
}

static void make_fifo_block(uint8_t *block,
                            uint16_t peak,
                            uint16_t overflows,
                            uint16_t dropped)
{
    memset(block, 0, DDD_FIFO_TELEMETRY_LENGTH);
    block[0] = DDD_FIFO_TELEMETRY_ID;
    block[DDD_FIFO_OFFSET_STATUS] = DDD_FIFO_TELEMETRY_FORMAT;
    block[DDD_FIFO_OFFSET_LATCH_COUNT] = 7;
    put_word(block, DDD_FIFO_OFFSET_USED_NOW, 4000);
    put_word(block, DDD_FIFO_OFFSET_PEAK, peak);
    put_word(block, DDD_FIFO_OFFSET_PEAK_LIFETIME, 12288);
    put_word(block, DDD_FIFO_OFFSET_OVERFLOWS, overflows);
    put_word(block, DDD_FIFO_OFFSET_DROPPED, dropped);
    put_word(block, DDD_FIFO_OFFSET_PACKETS, 1221);
    put_word(block, DDD_FIFO_OFFSET_NEAR_FULL, 12);
    put_word(block, DDD_FIFO_OFFSET_DEPTH, 16384);
    put_word(block, DDD_FIFO_OFFSET_PACKET_WORDS, 8192);
    put_word(block, DDD_FIFO_OFFSET_NEAR_FULL_WORDS, 12288);
}

static bool test_fifo_telemetry(void)
{
    uint8_t block[DDD_FIFO_TELEMETRY_LENGTH];
    ddd_fifo_telemetry_t telemetry;

    make_fifo_block(block, 12288, 0, 0);
    block[DDD_FIFO_OFFSET_STATUS] |=
        DDD_FIFO_FLAG_OVERFLOW_SEEN | DDD_FIFO_FLAG_SATURATED;
    CHECK(ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(telemetry.present);
    CHECK(telemetry.format == DDD_FIFO_TELEMETRY_FORMAT);
    CHECK(telemetry.overflow_seen);
    CHECK(telemetry.saturated);
    CHECK(telemetry.latch_count == 7);
    CHECK(telemetry.used_now == 4000);
    CHECK(telemetry.peak == 12288);
    CHECK(telemetry.peak_since_open == 12288);
    CHECK(telemetry.packets_read == 1221);
    CHECK(telemetry.near_full_units == 12);
    CHECK(telemetry.depth_words == 16384);
    CHECK(telemetry.packet_words == 8192);
    CHECK(telemetry.near_full_words == 12288);
    CHECK(ddd_fifo_backpressure_percent(&telemetry) == 50);
    CHECK(ddd_fifo_peak_percent(&telemetry) == 75);
    CHECK(ddd_fifo_used_percent(&telemetry) == 24);

    make_fifo_block(block, 8192, 0, 0);
    CHECK(ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(ddd_fifo_backpressure_percent(&telemetry) == 0);

    make_fifo_block(block, 1000, 1, 4200);
    CHECK(ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(ddd_fifo_backpressure_percent(&telemetry) == 100);
    CHECK(telemetry.dropped_words == 4200);

    memset(block, 0, sizeof(block));
    CHECK(!ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(!telemetry.present);
    memset(block, 0xff, sizeof(block));
    CHECK(!ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    make_fifo_block(block, 9000, 0, 0);
    CHECK(!ddd_fifo_telemetry_parse(
        block, DDD_FIFO_TELEMETRY_LENGTH - 1u, &telemetry));
    block[DDD_FIFO_OFFSET_STATUS] = DDD_FIFO_TELEMETRY_FORMAT + 1u;
    CHECK(!ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    make_fifo_block(block, 9000, 0, 0);
    put_word(block, DDD_FIFO_OFFSET_PACKET_WORDS, 16385);
    CHECK(!ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    make_fifo_block(block, 9000, 0, 0);
    put_word(block, DDD_FIFO_OFFSET_PACKET_WORDS, 0);
    CHECK(!ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    make_fifo_block(block, 9000, 0, 0);
    put_word(block, DDD_FIFO_OFFSET_PACKET_WORDS, 16384);
    CHECK(!ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));

    make_fifo_block(block, UINT16_MAX, 0, 0);
    put_word(block, DDD_FIFO_OFFSET_USED_NOW, UINT16_MAX);
    CHECK(ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(ddd_fifo_peak_percent(&telemetry) == 100);
    CHECK(ddd_fifo_used_percent(&telemetry) == 100);
    CHECK(!ddd_fifo_telemetry_parse(block, sizeof(block), NULL));
    CHECK(!ddd_fifo_telemetry_parse(NULL, sizeof(block), &telemetry));
    return true;
}

static bool test_fifo_telemetry_totals(void)
{
    uint8_t block[DDD_FIFO_TELEMETRY_LENGTH];
    ddd_fifo_telemetry_t telemetry;
    ddd_fifo_telemetry_totals_t totals;

    ddd_fifo_telemetry_totals_init(&totals);
    CHECK(totals.interval_coverage_complete);
    make_fifo_block(block, 9000, 1, 100);
    block[DDD_FIFO_OFFSET_LATCH_COUNT] = 255;
    CHECK(ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(ddd_fifo_telemetry_totals_add(&totals, &telemetry));
    CHECK(totals.overflow_events == 1);
    CHECK(totals.dropped_words == 100);
    CHECK(totals.near_full_units == 12);
    CHECK(totals.peak_words == 9000);
    CHECK(totals.peak_backpressure_percent == 100);

    CHECK(!ddd_fifo_telemetry_totals_add(&totals, &telemetry));
    CHECK(totals.overflow_events == 1);

    make_fifo_block(block, 12288, 0, 0);
    block[DDD_FIFO_OFFSET_LATCH_COUNT] = 0;
    CHECK(ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(ddd_fifo_telemetry_totals_add(&totals, &telemetry));
    CHECK(totals.interval_coverage_complete);
    CHECK(totals.peak_words == 12288);

    block[DDD_FIFO_OFFSET_LATCH_COUNT] = 2;
    CHECK(ddd_fifo_telemetry_parse(block, sizeof(block), &telemetry));
    CHECK(ddd_fifo_telemetry_totals_add(&totals, &telemetry));
    CHECK(!totals.interval_coverage_complete);
    CHECK(!ddd_fifo_telemetry_totals_add(NULL, &telemetry));
    CHECK(!ddd_fifo_telemetry_totals_add(&totals, NULL));
    ddd_fifo_telemetry_totals_init(NULL);
    return true;
}

static bool test_validators(void)
{
    ddd_sequence_validator_t sequence;
    ddd_test_ramp_validator_t ramp;
    uint16_t *words = calloc(DDD_SEQUENCE_SAMPLES_PER_MARKER,
                             sizeof(*words));
    CHECK(words != NULL);
    ddd_sequence_validator_init(&sequence);
    words[0] = (uint16_t)(10u << 10);
    words[1] = (uint16_t)(11u << 10);
    CHECK(ddd_sequence_validator_feed(&sequence, words, 2) ==
          DDD_VALIDATION_OK);
    CHECK(sequence.phase == DDD_SEQUENCE_RUNNING);
    for (size_t i = 0; i < DDD_SEQUENCE_SAMPLES_PER_MARKER; ++i) {
        words[i] = (uint16_t)(11u << 10);
    }
    /* One sample for marker 11 was already consumed above. */
    CHECK(ddd_sequence_validator_feed(
              &sequence, words, DDD_SEQUENCE_SAMPLES_PER_MARKER - 1) ==
          DDD_VALIDATION_OK);
    words[0] = (uint16_t)(12u << 10);
    CHECK(ddd_sequence_validator_feed(&sequence, words, 1) ==
          DDD_VALIDATION_OK);
    words[0] = (uint16_t)(14u << 10);
    CHECK(ddd_sequence_validator_feed(&sequence, words, 1) ==
          DDD_VALIDATION_MISMATCH);

    ddd_test_ramp_validator_init(&ramp);
    for (size_t i = 0; i < DDD_TEST_RAMP_NEW_WRAP; ++i) words[i] = (uint16_t)i;
    words[DDD_TEST_RAMP_NEW_WRAP] = 0;
    CHECK(ddd_test_ramp_validator_feed(
              &ramp, words, DDD_TEST_RAMP_NEW_WRAP + 1u) == DDD_VALIDATION_OK);
    words[0] = 2;
    CHECK(ddd_test_ramp_validator_feed(&ramp, words, 1) ==
          DDD_VALIDATION_MISMATCH);

    ddd_test_ramp_validator_init(&ramp);
    for (size_t i = 0; i < DDD_TEST_RAMP_LEGACY_WRAP; ++i) {
        words[i] = (uint16_t)i;
    }
    words[DDD_TEST_RAMP_LEGACY_WRAP] = 0;
    CHECK(ddd_test_ramp_validator_feed(
              &ramp, words, DDD_TEST_RAMP_LEGACY_WRAP + 1u) ==
          DDD_VALIDATION_OK);
    free(words);
    return true;
}

int main(void)
{
    if (!test_profiles_and_rates() ||
        !test_v1_rate_plans() ||
        !test_clockgen_profile_selection() ||
        !test_reconnect_path_selection() ||
        !test_topology_and_endpoint() ||
        !test_lifecycle() ||
        !test_fifo_telemetry() ||
        !test_fifo_telemetry_totals() ||
        !test_validators()) {
        return 1;
    }
    puts("DDD protocol tests passed");
    return 0;
}
