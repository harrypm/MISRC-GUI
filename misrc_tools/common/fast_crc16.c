/*
 * Optimize the high-throughput capture frame parser's CRC16 validation.
 * The normal hsdaoh crc16_ccitt() visits every byte with a dependent table
 * lookup. This slicing-by-eight implementation has exactly the same result
 * for all input lengths, including short, empty and unaligned buffers.
 * It does not change tolerated CRC errors or any decoded sample data.
 */
#include "fast_crc16.h"
#include <hsdaoh_crc.h>
#include <stdatomic.h>

static uint16_t s_crc_slice[8][256];
/*
 * Portable C11 one-time initialization (Windows MinGW-UCRT may not ship
 * <threads.h>).  States: 0 = uninitialized, 1 = initializing, 2 = ready.
 * Readers wait for the release store to make all table entries visible.
 */
static atomic_int s_crc_init_state = ATOMIC_VAR_INIT(0);

static void misrc_crc_init_slices(void) {
    for (unsigned i = 0; i < 256; ++i) {
        uint16_t crc = ccitt_hash[i];
        s_crc_slice[0][i] = crc;
        for (unsigned slice = 1; slice < 8; ++slice) {
            crc = (uint16_t)((crc << 8) ^ ccitt_hash[crc >> 8]);
            s_crc_slice[slice][i] = crc;
        }
    }
}

static void misrc_crc_ensure_slices(void) {
    if (atomic_load_explicit(&s_crc_init_state, memory_order_acquire) == 2)
        return;

    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s_crc_init_state, &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        misrc_crc_init_slices();
        atomic_store_explicit(&s_crc_init_state, 2, memory_order_release);
    } else {
        /* Initialization takes 8*256 small operations; this is a one-off wait. */
        while (atomic_load_explicit(&s_crc_init_state, memory_order_acquire) != 2) {
        }
    }
}

uint16_t misrc_crc16_ccitt_fast(const uint8_t *bytes, size_t length) {
    if (!bytes && length) return 0;
    misrc_crc_ensure_slices();
    uint16_t crc = 0xffff;
    const uint8_t *p = bytes;
    while (length >= 8) {
        crc = s_crc_slice[7][((crc >> 8) ^ p[0]) & 255] ^
              s_crc_slice[6][((crc & 255) ^ p[1]) & 255] ^
              s_crc_slice[5][p[2]] ^
              s_crc_slice[4][p[3]] ^
              s_crc_slice[3][p[4]] ^
              s_crc_slice[2][p[5]] ^
              s_crc_slice[1][p[6]] ^
              s_crc_slice[0][p[7]];
        p += 8;
        length -= 8;
    }
    while (length--) {
        crc = (uint16_t)((crc << 8) ^ ccitt_hash[((crc >> 8) ^ *p++) & 255]);
    }
    return crc;
}
