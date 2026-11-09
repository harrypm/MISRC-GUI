#include "../misrc_gui/signal/gui_ld_ac3_frames.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    PACKET_BYTES = 1536,
    SLOT_BYTES = 1688,
    PACKET_COUNT = 20,
    PAYLOAD_BYTES = 2110,
    C2_COLUMNS = 66,
    C2_DATA = 32,
    C2_LENGTH = 36,
    C1_DATA = 33,
    C1_LENGTH = 37,
    CHANNEL_FRAMES = 72,
    CHANNEL_BYTES = 3 + C1_LENGTH,
    BLOCK_COUNT = (PACKET_COUNT * SLOT_BYTES + PAYLOAD_BYTES - 1) / PAYLOAD_BYTES,
    RF_RATE = 20000000,
    SYMBOL_RATE = 288000,
    SLOT_TICKS = 640000
};

/* An independent polynomial-division encoder builds valid C1 and C2 parity
 * even when the inner AC-3 slot has been replaced or its header corrupted. */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    unsigned result = 0;
    unsigned factor = a;
    for (; b; b >>= 1) {
        if (b & 1) result ^= factor;
        factor <<= 1;
        if (factor & 256) factor ^= 0x187;
    }
    return (uint8_t)result;
}

static void make_generator(uint8_t generator[5]) {
    uint8_t root = 1;
    for (unsigned i = 0; i < 120; ++i) root = gf_mul(root, 2);
    generator[0] = 1;
    for (unsigned degree = 1; degree <= 4; ++degree) {
        for (unsigned i = degree; i > 0; --i)
            generator[i] ^= gf_mul(generator[i - 1], root);
        root = gf_mul(root, 2);
    }
}

static void append_parity(uint8_t *word, unsigned data, const uint8_t generator[5]) {
    uint8_t dividend[C1_LENGTH] = {0};
    memcpy(dividend, word, data);
    for (unsigned i = 0; i < data; ++i) {
        const uint8_t leading = dividend[i];
        for (unsigned j = 1; j <= 4; ++j)
            dividend[i + j] ^= gf_mul(leading, generator[j]);
    }
    memcpy(word + data, dividend + data, 4);
}

static void make_packet(uint8_t packet[PACKET_BYTES], unsigned index) {
    memset(packet, (int)(0x80 + index), PACKET_BYTES);
    packet[0] = 0x0b;
    packet[1] = 0x77;
    packet[4] = 0x1c;
    packet[5] = 0x40;
}

typedef struct {
    gui_ld_ac3_frames_t *decoder;
    uint64_t origin, symbol_count, first_packet_end;
    uint32_t missing_slots, rf_clock;
    unsigned next_packet, total_packets;
    bool failed;
} test_stream_t;

static void receive_packet(void *context, const uint8_t *packet, size_t size, uint64_t end_sample) {
    test_stream_t *stream = context;
    unsigned index = stream->next_packet;
    while (index < stream->total_packets && (stream->missing_slots & (1u << (index % PACKET_COUNT)))) ++index;
    if (index == stream->total_packets || size != PACKET_BYTES) {
        stream->failed = true;
        return;
    }
    uint8_t expected[PACKET_BYTES];
    make_packet(expected, index % PACKET_COUNT);
    const uint64_t elapsed = (uint64_t)index * SLOT_TICKS * stream->rf_clock / RF_RATE;
    const uint64_t expected_end = stream->first_packet_end + elapsed;
    const uint64_t error = expected_end >= end_sample ? expected_end - end_sample : end_sample - expected_end;
    /* Clock phase is observed at each RF block boundary. With skew, the
     * estimate inside a block can differ by up to one 40 ms phase step. */
    const unsigned tolerance = stream->rf_clock == RF_RATE ? 0 : 100;
    if (memcmp(packet, expected, sizeof(expected)) || error > tolerance) {
        fprintf(stderr, "AC-3 packet %u expected timestamp %" PRIu64 ", got %" PRIu64 "\n",
                index, expected_end, end_sample);
        stream->failed = true;
    }
    stream->next_packet = index + 1;
}

static void push_symbol(test_stream_t *stream, uint8_t symbol) {
    uint64_t end = stream->origin + ++stream->symbol_count * stream->rf_clock / SYMBOL_RATE;
    gui_ld_ac3_frames_push(stream->decoder, symbol, end, receive_packet, stream);
}

static void push_block(test_stream_t *stream, const uint8_t *encoded, unsigned channels) {
    for (unsigned byte = 0; byte < channels * CHANNEL_BYTES; ++byte)
        for (int shift = 6; shift >= 0; shift -= 2)
            push_symbol(stream, (encoded[byte] >> shift) & 3u);
}

static void encode_block(uint8_t *out, const uint8_t *payload, const uint8_t generator[5]) {
    uint8_t block[2 + PAYLOAD_BYTES] = {0x10, 0x00};
    memcpy(block + 2, payload, PAYLOAD_BYTES);
    uint8_t c2[C2_COLUMNS][C2_LENGTH];
    for (unsigned column = 0; column < C2_COLUMNS; ++column) {
        memcpy(c2[column], block + column * C2_DATA, C2_DATA);
        append_parity(c2[column], C2_DATA, generator);
    }
    for (unsigned row = 0; row < C2_LENGTH; ++row) {
        uint8_t c1[2][C1_LENGTH];
        for (unsigned lane = 0; lane < 2; ++lane) {
            for (unsigned column = 0; column < C1_DATA; ++column)
                c1[lane][column] = c2[2 * column + lane][row];
            append_parity(c1[lane], C1_DATA, generator);
        }
        for (unsigned frame = 0; frame < 2; ++frame) {
            *out++ = 0x17;
            *out++ = (uint8_t)(2 * row + frame);
            *out++ = 0;
            for (unsigned byte = 0; byte < C1_LENGTH; ++byte) {
                unsigned index = frame * C1_LENGTH + byte;
                *out++ = c1[index % 2][index / 2];
            }
        }
    }
}

static void encode_fixture(uint8_t encoded[BLOCK_COUNT][CHANNEL_FRAMES * CHANNEL_BYTES],
                           uint32_t missing, uint32_t corrupt) {
    uint8_t payload[BLOCK_COUNT * PAYLOAD_BYTES] = {0};
    for (unsigned index = 0; index < PACKET_COUNT; ++index) {
        if (missing & (1u << index)) continue;
        uint8_t *packet = payload + index * SLOT_BYTES;
        make_packet(packet, index);
        if (corrupt & (1u << index)) packet[4] = 0x1d;
    }
    uint8_t generator[5] = {0};
    make_generator(generator);
    for (unsigned block = 0; block < BLOCK_COUNT; ++block)
        encode_block(encoded[block], payload + block * PAYLOAD_BYTES, generator);
}

static bool check_stream(gui_ld_ac3_frames_t *decoder, uint32_t missing, uint32_t corrupt,
                         uint64_t origin, uint32_t rf_clock, unsigned repeats,
                         unsigned final_channels, bool recoverable_tail) {
    uint8_t encoded[BLOCK_COUNT][CHANNEL_FRAMES * CHANNEL_BYTES];
    encode_fixture(encoded, missing, corrupt);
    test_stream_t stream = {
        .decoder = decoder,
        .origin = origin,
        .first_packet_end = origin + (uint64_t)PACKET_BYTES * (RF_RATE / 25) / PAYLOAD_BYTES,
        .missing_slots = missing | corrupt,
        .rf_clock = rf_clock,
        .total_packets = repeats * PACKET_COUNT
    };
    for (unsigned repeat = 0; repeat < repeats; ++repeat) {
        for (unsigned block = 0; block < BLOCK_COUNT; ++block) {
            const bool final = repeat == repeats - 1 && block == BLOCK_COUNT - 1;
            push_block(&stream, encoded[block], final ? final_channels : CHANNEL_FRAMES);
        }
    }
    /* The final RF block completes the last two packets in this fixture. */
    const unsigned before_finish = stream.total_packets - (final_channels < CHANNEL_FRAMES ? 2 : 0);
    if (stream.next_packet != before_finish) {
        fprintf(stderr, "AC-3 expected %u packets before EOF with %u final RF frames, got %u\n",
                before_finish, final_channels, stream.next_packet);
        return false;
    }
    gui_ld_ac3_frames_finish(decoder, receive_packet, &stream);
    gui_ld_ac3_frames_finish(decoder, receive_packet, &stream);
    const unsigned expected = recoverable_tail ? stream.total_packets : before_finish;
    if (stream.next_packet != expected)
        fprintf(stderr, "AC-3 expected %u packets after EOF with %u final RF frames, got %u\n",
                expected, final_channels, stream.next_packet);
    return !stream.failed && stream.next_packet == expected;
}

static bool check_noisy_tail(gui_ld_ac3_frames_t *decoder, bool resume) {
    uint8_t encoded[BLOCK_COUNT][CHANNEL_FRAMES * CHANNEL_BYTES];
    encode_fixture(encoded, 0, 0);
    const uint64_t packet_offset = (uint64_t)PACKET_BYTES * (RF_RATE / 25) / PAYLOAD_BYTES;
    test_stream_t stream = {
        .decoder = decoder,
        .first_packet_end = packet_offset,
        .rf_clock = RF_RATE,
        .total_packets = PACKET_COUNT
    };
    for (unsigned block = 0; block < BLOCK_COUNT - 1; ++block)
        push_block(&stream, encoded[block], CHANNEL_FRAMES);
    push_block(&stream, encoded[BLOCK_COUNT - 1], CHANNEL_FRAMES - 1);
    /* 148 symbols since the last header, then 340 more still within the
     * 3*160+8 header timeout. The next no-sync symbol expires it. */
    for (unsigned i = 0; i < 340; ++i) push_symbol(&stream, 0);
    if (stream.next_packet != PACKET_COUNT - 2) return false;
    push_symbol(&stream, 0);
    if (stream.next_packet != PACKET_COUNT) {
        fprintf(stderr, "AC-3 header timeout discarded the recoverable tail: got %u of %u packets\n",
                stream.next_packet, PACKET_COUNT);
        return false;
    }
    if (resume) {
        /* The next block's 12-symbol header establishes a new RF anchor. */
        const uint64_t block_start = (stream.symbol_count + 12) * RF_RATE / SYMBOL_RATE -
                                     UINT64_C(12) * RF_RATE / SYMBOL_RATE;
        stream.first_packet_end = block_start + packet_offset - (uint64_t)PACKET_COUNT * SLOT_TICKS;
        stream.total_packets += PACKET_COUNT;
        for (unsigned block = 0; block < BLOCK_COUNT; ++block)
            push_block(&stream, encoded[block], CHANNEL_FRAMES);
    }
    gui_ld_ac3_frames_finish(decoder, receive_packet, &stream);
    return !stream.failed && stream.next_packet == stream.total_packets;
}

int main(void) {
    gui_ld_ac3_frames_t *decoder = gui_ld_ac3_frames_create();
    if (!decoder) return 1;
    const bool clean = check_stream(decoder, 0, 0, 0, RF_RATE, 1, CHANNEL_FRAMES, false);
    gui_ld_ac3_frames_reset(decoder);
    /* Corrupt one header; later remove one slot, two adjacent slots and
     * a burst crossing several RF blocks. Use an RF clock past four hours. */
    const uint32_t missing = (1u << 5) | (3u << 8) | (0x3fu << 12);
    const uint64_t late_origin = UINT64_C(300000000000);
    const bool damaged = check_stream(decoder, missing, 1u << 2, late_origin, RF_RATE, 1, CHANNEL_FRAMES, false);
    gui_ld_ac3_frames_reset(decoder);
    /* A ±100 ppm source skew for >3 minutes must not accumulate timestamp drift. */
    const bool faster = check_stream(decoder, missing, 1u << 2, late_origin, RF_RATE + 2000, 300,
                                     CHANNEL_FRAMES, false);
    gui_ld_ac3_frames_reset(decoder);
    const bool slower = check_stream(decoder, 0, 0, late_origin, RF_RATE - 2000, 300, CHANNEL_FRAMES, false);
    gui_ld_ac3_frames_reset(decoder);
    const bool tail = check_stream(decoder, 0, 0, 0, RF_RATE, 1, CHANNEL_FRAMES - 1, true);
    gui_ld_ac3_frames_reset(decoder);
    const bool truncated = check_stream(decoder, 0, 0, 0, RF_RATE, 1, CHANNEL_FRAMES / 2, false);
    gui_ld_ac3_frames_reset(decoder);
    const bool noisy_tail = check_noisy_tail(decoder, false);
    gui_ld_ac3_frames_reset(decoder);
    const bool resynced_tail = check_noisy_tail(decoder, true);
    gui_ld_ac3_frames_destroy(decoder);
    if (!clean || !damaged || !faster || !slower || !tail || !truncated || !noisy_tail || !resynced_tail) return 1;
    puts("AC-3 packet timing follows the RF clock and recovers final blocks across EOF and sync loss");
    return 0;
}
