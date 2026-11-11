#ifndef MISRC_MONITOR_LOCAL_SAVE_PROTOCOL_H
#define MISRC_MONITOR_LOCAL_SAVE_PROTOCOL_H
/* Platform-independent, versioned client-owned recording file transport.
 * Frame headers are 24-byte fixed-endian binary. Nothing in this layer opens
 * files or assumes a platform-specific separator; those belong to the receiver.
 */
#include <stddef.h>
#include <stdint.h>
#define MLS_HEADER_SIZE 24u
#define MLS_MAX_PAYLOAD (1024u * 1024u)
#define MLS_MAX_FILES 16u
#define MLS_VERSION 3u
#define MLS_OPEN 1u
#define MLS_WRITE 2u
#define MLS_SEEK 3u
#define MLS_CLOSE 4u
#define MLS_ACK 5u
#define MLS_ABORT 6u
#define MLS_COMMIT 7u
#define MLS_WRITE_STREAM 8u /* continuous transfer: ACK follows MLS_SYNC */
#define MLS_SYNC 9u         /* cumulative ACK, after receiver flush */

typedef struct {
    uint8_t type;
    uint8_t file_id;
    uint32_t length;
    uint64_t offset;
    uint32_t crc;
} mls_frame_t;

/* 0 success, negative means malformed/unrecognized/oversize. */
int mls_encode_header(uint8_t out[MLS_HEADER_SIZE], const mls_frame_t *f);
int mls_decode_header(mls_frame_t *out, const uint8_t *in, size_t len);
uint32_t mls_crc32(const void *data, size_t len);
/* OPEN must be a filename only, NEVER a client path. */
int mls_safe_filename(const char *name, size_t len);
#endif
