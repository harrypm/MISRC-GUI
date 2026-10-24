/* CRC16-CCITT/FALSE, byte-equivalent to hsdaoh_crc.h, slicing-by-8 fast path. */
#ifndef MISRC_FAST_CRC16_H
#define MISRC_FAST_CRC16_H
#include <stddef.h>
#include <stdint.h>
/* Uses initial 0xffff, no reflection, polynomial 0x1021, no final xor. */
uint16_t misrc_crc16_ccitt_fast(const uint8_t *bytes, size_t length);
#endif
