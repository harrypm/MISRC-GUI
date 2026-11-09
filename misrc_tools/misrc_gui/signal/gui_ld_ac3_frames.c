/* MISRC GUI LaserDisc AC-3 RF frame recovery. GPL-3.0-or-later.
 * Frame layout and Reed-Solomon parameters follow museLD's Ac3InputFraming
 * and Ac3BlockHandler (Copyright 2025-2026 Staffan Ulfberg, GPL-3.0-or-later).
 * https://github.com/staffanu/museld/tree/master/player/src/ac3
 */
#include "gui_ld_ac3_frames.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
    LD_AC3_RF_RATE = 20000000,
    LD_AC3_SYMBOL_RATE = 288000,
    LD_AC3_SYNC_SYMBOLS = 12,
    LD_AC3_CHANNEL_BYTES = 37,
    LD_AC3_FRAME_SYMBOLS = LD_AC3_SYNC_SYMBOLS + 4 * LD_AC3_CHANNEL_BYTES,
    LD_AC3_CHANNEL_FRAMES = 72,
    LD_AC3_C1_DATA = 33,
    LD_AC3_C2_WORDS = 2 * LD_AC3_C1_DATA,
    LD_AC3_C2_DATA = 32,
    LD_AC3_C2_LENGTH = LD_AC3_CHANNEL_FRAMES / 2,
    LD_AC3_BLOCK_BYTES = LD_AC3_C2_WORDS * LD_AC3_C2_DATA,
    LD_AC3_BLOCK_PAYLOAD_BYTES = LD_AC3_BLOCK_BYTES - 2,
    LD_AC3_BLOCK_TICKS = LD_AC3_RF_RATE / 25,
    LD_AC3_PACKET_BYTES = 1536,
    /* Five 32 ms AC-3 slots span the payload of four 40 ms RF blocks. */
    LD_AC3_PACKET_SLOT_BYTES = LD_AC3_BLOCK_PAYLOAD_BYTES * 4 / 5,
    LD_AC3_PACKET_TICKS = LD_AC3_PACKET_BYTES * (LD_AC3_RF_RATE / 1000) / 48,
    LD_AC3_RS_PARITY = 4,
    LD_AC3_RS_FIRST_ROOT = 120,
    LD_AC3_HEADER_TOLERANCE = 8,
    LD_AC3_MAX_FRAME_DISTANCE = 3,
    LD_AC3_MAX_SYMBOL_GAP = 512
};

typedef struct {
    uint64_t symbol_count, last_symbol_sample, last_header_symbol;
    uint64_t block_start_sample, packet_end_sample;
    uint64_t payload_offset, last_packet_payload_end;
    uint64_t last_packet_block_start, last_packet_block_offset;
    uint32_t sync_window;
    unsigned frame_number, data_symbols, packet_bytes;
    bool have_header, block_done, have_packet_clock;
    uint8_t block[LD_AC3_CHANNEL_FRAMES][LD_AC3_CHANNEL_BYTES];
    bool present[LD_AC3_CHANNEL_FRAMES];
    uint8_t packet[LD_AC3_PACKET_BYTES];
} ld_ac3_stream_t;

struct gui_ld_ac3_frames {
    uint8_t gf_log[256], gf_exp[510];
    ld_ac3_stream_t stream;
};

gui_ld_ac3_frames_t *gui_ld_ac3_frames_create(void) {
    gui_ld_ac3_frames_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    unsigned element = 1;
    for (unsigned i = 0; i < 255; ++i) {
        d->gf_exp[i] = (uint8_t)element;
        d->gf_exp[i + 255] = (uint8_t)element;
        d->gf_log[element] = (uint8_t)i;
        element <<= 1;
        if (element & 256u) element ^= 0x187u;
    }
    return d;
}

void gui_ld_ac3_frames_destroy(gui_ld_ac3_frames_t *d) {
    free(d);
}

void gui_ld_ac3_frames_reset(gui_ld_ac3_frames_t *d) {
    if (d) memset(&d->stream, 0, sizeof(d->stream));
}

static inline uint8_t ld_ac3_gf_mul(const gui_ld_ac3_frames_t *d, uint8_t a, uint8_t b) {
    return a && b ? d->gf_exp[d->gf_log[a] + d->gf_log[b]] : 0;
}

static inline uint8_t ld_ac3_gf_div(const gui_ld_ac3_frames_t *d, uint8_t a, uint8_t b) {
    return a ? d->gf_exp[d->gf_log[a] + 255u - d->gf_log[b]] : 0;
}

static void ld_ac3_rs_syndromes(const gui_ld_ac3_frames_t *d, const uint8_t *word,
                                unsigned length, uint8_t syndrome[LD_AC3_RS_PARITY]) {
    for (unsigned row = 0; row < LD_AC3_RS_PARITY; ++row) {
        uint8_t value = 0;
        uint8_t root = d->gf_exp[LD_AC3_RS_FIRST_ROOT + row];
        for (unsigned i = 0; i < length; ++i)
            value = ld_ac3_gf_mul(d, value, root) ^ word[i];
        syndrome[row] = value;
    }
}

static bool ld_ac3_rs_repair(const gui_ld_ac3_frames_t *d, uint8_t *word, unsigned length,
                              const uint8_t syndrome[LD_AC3_RS_PARITY],
                              const unsigned positions[LD_AC3_RS_PARITY], unsigned count) {
    uint8_t equations[LD_AC3_RS_PARITY][LD_AC3_RS_PARITY + 1];
    for (unsigned row = 0; row < LD_AC3_RS_PARITY; ++row) {
        for (unsigned col = 0; col < count; ++col) {
            unsigned power = (LD_AC3_RS_FIRST_ROOT + row) * (length - 1 - positions[col]);
            equations[row][col] = d->gf_exp[power % 255u];
        }
        equations[row][count] = syndrome[row];
    }

    /* Distinct locations give a nonsingular Vandermonde matrix. Include the
     * surplus syndrome rows so impossible corrections leave the word intact. */
    for (unsigned col = 0; col < count; ++col) {
        unsigned pivot = col;
        while (pivot < count && equations[pivot][col] == 0) ++pivot;
        if (pivot == count) return false;
        for (unsigned j = col; j <= count; ++j) {
            uint8_t value = equations[col][j];
            equations[col][j] = equations[pivot][j];
            equations[pivot][j] = value;
        }
        uint8_t inverse = ld_ac3_gf_div(d, 1, equations[col][col]);
        for (unsigned j = col; j <= count; ++j)
            equations[col][j] = ld_ac3_gf_mul(d, equations[col][j], inverse);

        for (unsigned row = 0; row < LD_AC3_RS_PARITY; ++row) {
            if (row == col) continue;
            uint8_t factor = equations[row][col];
            for (unsigned j = col; j <= count; ++j)
                equations[row][j] ^= ld_ac3_gf_mul(d, factor, equations[col][j]);
        }
    }
    for (unsigned row = count; row < LD_AC3_RS_PARITY; ++row)
        if (equations[row][count]) return false;
    for (unsigned i = 0; i < count; ++i)
        word[positions[i]] ^= equations[i][count];
    return true;
}

static bool ld_ac3_rs_decode(const gui_ld_ac3_frames_t *d, uint8_t *word, unsigned length,
                              uint64_t erasures, bool conservative) {
    uint8_t syndrome[LD_AC3_RS_PARITY];
    ld_ac3_rs_syndromes(d, word, length, syndrome);
    if (!erasures && !(syndrome[0] | syndrome[1] | syndrome[2] | syndrome[3])) return true;

    unsigned positions[LD_AC3_RS_PARITY];
    unsigned known = 0;
    unsigned available = LD_AC3_RS_PARITY;
    uint8_t filtered[LD_AC3_RS_PARITY];
    memcpy(filtered, syndrome, sizeof(filtered));
    for (unsigned i = 0; i < length && erasures; ++i, erasures >>= 1) {
        if (!(erasures & 1u)) continue;
        if (known == LD_AC3_RS_PARITY) return false;
        positions[known++] = i;
        uint8_t x = d->gf_exp[length - 1 - i];
        --available;
        for (unsigned row = 0; row < available; ++row)
            filtered[row] = filtered[row + 1] ^ ld_ac3_gf_mul(d, filtered[row], x);
    }
    if (known && ld_ac3_rs_repair(d, word, length, syndrome, positions, known)) return true;
    if (available < 2) return false;

    /* Removing erasure locators from the syndromes leaves a geometric
     * sequence for one unknown error, or a second-order recurrence for two. */
    if (filtered[0] && filtered[1]) {
        uint8_t x = ld_ac3_gf_div(d, filtered[1], filtered[0]);
        unsigned power = d->gf_log[x];
        if (power < length) {
            unsigned pos = length - 1 - power;
            bool duplicate = false;
            for (unsigned i = 0; i < known; ++i) duplicate |= positions[i] == pos;
            if (!duplicate) {
                positions[known] = pos;
                if (ld_ac3_rs_repair(d, word, length, syndrome, positions, known + 1)) return true;
            }
        }
    }
    if (available != LD_AC3_RS_PARITY || conservative) return false;

    uint8_t det = ld_ac3_gf_mul(d, syndrome[1], syndrome[1]) ^
                  ld_ac3_gf_mul(d, syndrome[0], syndrome[2]);
    if (!det) return false;
    uint8_t a = ld_ac3_gf_div(d, ld_ac3_gf_mul(d, syndrome[1], syndrome[2]) ^
                                  ld_ac3_gf_mul(d, syndrome[0], syndrome[3]), det);
    uint8_t b = ld_ac3_gf_div(d, ld_ac3_gf_mul(d, syndrome[2], syndrome[2]) ^
                                  ld_ac3_gf_mul(d, syndrome[1], syndrome[3]), det);
    unsigned roots = 0;
    for (unsigned pos = 0; pos < length; ++pos) {
        uint8_t x = d->gf_exp[length - 1 - pos];
        if ((ld_ac3_gf_mul(d, x, x ^ a) ^ b) == 0) {
            positions[roots++] = pos;
            if (roots == 2) return ld_ac3_rs_repair(d, word, length, syndrome, positions, roots);
        }
    }
    return false;
}

static bool ld_ac3_correct_block(const gui_ld_ac3_frames_t *d, uint8_t payload[LD_AC3_BLOCK_BYTES]) {
    const ld_ac3_stream_t *s = &d->stream;
    uint8_t columns[LD_AC3_C2_WORDS][LD_AC3_C2_LENGTH];
    uint64_t erasures[2] = {0, 0};
    for (unsigned row = 0; row < LD_AC3_C2_LENGTH; ++row) {
        for (unsigned lane = 0; lane < 2; ++lane) {
            uint8_t word[LD_AC3_CHANNEL_BYTES];
            uint64_t missing = 0;
            for (unsigned i = 0; i < LD_AC3_CHANNEL_BYTES; ++i) {
                unsigned index = 2 * i + lane;
                unsigned frame = 2 * row + index / LD_AC3_CHANNEL_BYTES;
                word[i] = s->present[frame] ? s->block[frame][index % LD_AC3_CHANNEL_BYTES] : 0;
                if (!s->present[frame]) missing |= UINT64_C(1) << i;
            }
            if (!ld_ac3_rs_decode(d, word, LD_AC3_CHANNEL_BYTES, missing, true))
                erasures[lane] |= UINT64_C(1) << row;
            for (unsigned i = 0; i < LD_AC3_C1_DATA; ++i)
                columns[2 * i + lane][row] = word[i];
        }
    }
    for (unsigned col = 0; col < LD_AC3_C2_WORDS; ++col) {
        if (!ld_ac3_rs_decode(d, columns[col], LD_AC3_C2_LENGTH, erasures[col & 1u], false))
            return false;
        memcpy(payload + col * LD_AC3_C2_DATA, columns[col], LD_AC3_C2_DATA);
    }
    return payload[0] == 0x10 && payload[1] == 0x00;
}

static void ld_ac3_clear_packet(ld_ac3_stream_t *s) {
    s->packet_bytes = 0;
    s->have_packet_clock = false;
}

static void ld_ac3_output_block(gui_ld_ac3_frames_t *d, gui_ld_ac3_frame_callback_t emit,
                                void *context) {
    ld_ac3_stream_t *s = &d->stream;
    uint8_t payload[LD_AC3_BLOCK_BYTES];
    s->block_done = true;
    const uint64_t block_payload_offset = s->payload_offset;
    s->payload_offset += LD_AC3_BLOCK_PAYLOAD_BYTES;
    if (!ld_ac3_correct_block(d, payload)) {
        ld_ac3_clear_packet(s);
        return;
    }

    for (unsigned i = 2; i < sizeof(payload); ++i) {
        uint8_t byte = payload[i];
        unsigned pos = s->packet_bytes;
        /* 0B77 sync, fscod=0 / frmsizecod=28, and no half-rate or E-AC-3
         * bsid. The audio decoder checks the frame's two CRCs before use. */
        if ((pos == 0 && byte != 0x0b) || (pos == 1 && byte != 0x77) ||
            (pos == 4 && byte != 0x1c) || (pos == 5 && (byte >> 3) > 8)) {
            s->packet_bytes = 0;
            if (pos == 0 || byte != 0x0b) continue;
            pos = 0;
        }
        s->packet[pos] = byte;
        s->packet_bytes = pos + 1;
        if (s->packet_bytes == LD_AC3_PACKET_BYTES) {
            const uint64_t payload_end = block_payload_offset + i - 1u;
            if (s->have_packet_clock) {
                /* Count the elapsed RF payload slots even if inner sync or
                 * header validation discarded an entire AC-3 frame. */
                const uint64_t distance = payload_end - s->last_packet_payload_end;
                const uint64_t slots = (distance + LD_AC3_PACKET_SLOT_BYTES / 2u) /
                                       LD_AC3_PACKET_SLOT_BYTES;
                const uint64_t nominal_blocks = (block_payload_offset - s->last_packet_block_offset) /
                                                LD_AC3_BLOCK_PAYLOAD_BYTES;
                const uint64_t nominal_ticks = nominal_blocks * LD_AC3_BLOCK_TICKS;
                const uint64_t rf_ticks = s->block_start_sample >= s->last_packet_block_start
                    ? s->block_start_sample - s->last_packet_block_start : 0;
                /* Track RF clock phase at the block boundary. The slot count
                 * preserves PCM cadence and missing slots within each block. */
                const uint64_t advance = slots * LD_AC3_PACKET_TICKS + rf_ticks;
                s->packet_end_sample += advance > nominal_ticks ? advance - nominal_ticks : 1;
            } else {
                /* Approximate the phase of the first packet from its place
                 * in this block. Interleaving delays delivery of the PCM. */
                s->packet_end_sample = s->block_start_sample +
                    (uint64_t)(i - 1) * LD_AC3_BLOCK_TICKS / LD_AC3_BLOCK_PAYLOAD_BYTES;
                s->have_packet_clock = true;
            }
            s->last_packet_payload_end = payload_end;
            s->last_packet_block_start = s->block_start_sample;
            s->last_packet_block_offset = block_payload_offset;
            emit(context, s->packet, sizeof(s->packet), s->packet_end_sample);
            s->packet_bytes = 0;
        }
    }
}

void gui_ld_ac3_frames_finish(gui_ld_ac3_frames_t *d, gui_ld_ac3_frame_callback_t emit,
                              void *context) {
    if (!d || !emit) return;
    ld_ac3_stream_t *s = &d->stream;
    /* A truncated channel frame is already absent from present[]. Use the
     * usual erasure repair, even though no following block header arrived. */
    if (s->have_header && !s->block_done) ld_ac3_output_block(d, emit, context);
    ld_ac3_clear_packet(s);
}

void gui_ld_ac3_frames_push(gui_ld_ac3_frames_t *d, uint8_t symbol, uint64_t end_sample,
                            gui_ld_ac3_frame_callback_t emit, void *context) {
    if (!d || !emit) return;
    ld_ac3_stream_t *s = &d->stream;
    if (symbol > 3 || (s->symbol_count && (end_sample <= s->last_symbol_sample ||
                    end_sample - s->last_symbol_sample > LD_AC3_MAX_SYMBOL_GAP))) {
        gui_ld_ac3_frames_reset(d);
        if (symbol > 3) return;
    }

    s->last_symbol_sample = end_sample;
    ++s->symbol_count;
    s->sync_window = ((s->sync_window << 2) | symbol) & 0xffffffu;
    if (s->data_symbols) {
        unsigned pos = 4 * LD_AC3_CHANNEL_BYTES - s->data_symbols;
        uint8_t *byte = &s->block[s->frame_number][pos / 4];
        *byte = pos % 4 ? (uint8_t)((*byte << 2) | symbol) : symbol;
        if (--s->data_symbols == 0) {
            s->present[s->frame_number] = true;
            if (s->frame_number == LD_AC3_CHANNEL_FRAMES - 1 && !s->block_done)
                ld_ac3_output_block(d, emit, context);
        }
        return;
    }

    uint64_t distance = s->symbol_count - s->last_header_symbol;
    if (s->have_header && distance > LD_AC3_MAX_FRAME_DISTANCE * LD_AC3_FRAME_SYMBOLS + LD_AC3_HEADER_TOLERANCE) {
        /* Noise may follow a repairable block instead of the next header. */
        if (!s->block_done) ld_ac3_output_block(d, emit, context);
        s->have_header = false;
        ld_ac3_clear_packet(s);
    }
    /* Sync consists of 0,1,1,3, the sequence byte, then four zeros. */
    if (s->symbol_count < LD_AC3_SYNC_SYMBOLS || (s->sync_window & 0xff00ffu) != 0x170000u)
        return;
    unsigned frame = (s->sync_window >> 8) & 0xffu;
    if (frame >= LD_AC3_CHANNEL_FRAMES) return;

    if (s->have_header) {
        unsigned advance = (frame + LD_AC3_CHANNEL_FRAMES - s->frame_number) % LD_AC3_CHANNEL_FRAMES;
        if (!advance || advance > LD_AC3_MAX_FRAME_DISTANCE) return;
        uint64_t expected = advance * LD_AC3_FRAME_SYMBOLS;
        if (distance + LD_AC3_HEADER_TOLERANCE < expected || distance > expected + LD_AC3_HEADER_TOLERANCE)
            return;
        if (frame < s->frame_number && !s->block_done)
            ld_ac3_output_block(d, emit, context);
    } else if (frame != 0) {
        return;
    }

    if (!s->have_header || frame < s->frame_number) {
        memset(s->present, 0, sizeof(s->present));
        uint64_t block_offset = (uint64_t)(frame * LD_AC3_FRAME_SYMBOLS + LD_AC3_SYNC_SYMBOLS) *
                                LD_AC3_RF_RATE / LD_AC3_SYMBOL_RATE;
        s->block_start_sample = end_sample > block_offset ? end_sample - block_offset : 0;
        s->block_done = false;
    }
    s->have_header = true;
    s->last_header_symbol = s->symbol_count;
    s->frame_number = frame;
    s->data_symbols = 4 * LD_AC3_CHANNEL_BYTES;
}
