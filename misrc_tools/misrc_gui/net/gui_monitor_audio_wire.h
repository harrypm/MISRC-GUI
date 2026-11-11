/* Framed Opus-over-HTTP audio packets: 10 ms mono at 48 kHz, 16-bit PCM
 * at the decoder. Each packet has a 12-byte header + an Opus payload.
 * No Ogg container or raw capture samples are transferred.
 *
 *   0..3   ASCII "MAO1"
 *   4..7   packet sequence, unsigned big-endian
 *   8..9   payload size, unsigned big-endian (1..1275)
 *  10..11  reserved, must be zero
 */
#ifndef GUI_MONITOR_AUDIO_WIRE_H
#define GUI_MONITOR_AUDIO_WIRE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define MA_WIRE_HEADER 12u
#define MA_WIRE_MAX_PAYLOAD 1275u
#define MA_OPUS_RATE 48000u
#define MA_OPUS_FRAME 480u

static inline bool ma_wire_pack(uint8_t out[MA_WIRE_HEADER],
                                uint32_t sequence, size_t length)
{
    if (!out || length == 0 || length > MA_WIRE_MAX_PAYLOAD) return false;
    out[0]='M'; out[1]='A'; out[2]='O'; out[3]='1';
    out[4]=(uint8_t)(sequence>>24); out[5]=(uint8_t)(sequence>>16);
    out[6]=(uint8_t)(sequence>>8); out[7]=(uint8_t)sequence;
    out[8]=(uint8_t)(length>>8); out[9]=(uint8_t)length;
    out[10]=out[11]=0;
    return true;
}
static inline bool ma_wire_unpack(const uint8_t in[MA_WIRE_HEADER],
                                  uint32_t *sequence, size_t *length)
{
    if (!in || !sequence || !length || in[0]!='M' || in[1]!='A' ||
        in[2]!='O' || in[3]!='1' || in[10] || in[11]) return false;
    size_t n = ((size_t)in[8]<<8) | (size_t)in[9];
    if (!n || n>MA_WIRE_MAX_PAYLOAD) return false;
    *sequence=((uint32_t)in[4]<<24)|((uint32_t)in[5]<<16)|
              ((uint32_t)in[6]<<8)|(uint32_t)in[7];
    *length=n;
    return true;
}
#endif
