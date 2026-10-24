/* MISRC Remote Monitor v4: local-equivalent FFT and native scope preview. */
#ifndef GUI_REMOTE_MONITOR_H
#define GUI_REMOTE_MONITOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_RM_POINTS 256
#define GUI_RM_FFT_SIZE 4096
#define GUI_RM_FFT_BINS (GUI_RM_FFT_SIZE / 2 + 1)
/* MRM1 version 3: 4096-point, 2049-bin native FFT scaling, 16-bit
 * normalized magnitudes (0=-120 dB, 65535=0 dB), all little endian. */
#define GUI_RM_VERSION 3
#define GUI_RM_CHANNEL_BYTES (8 + 4 * GUI_RM_POINTS + 2 * GUI_RM_FFT_BINS)
#define GUI_RM_PACKET_BYTES (16 + 2 * GUI_RM_CHANNEL_BYTES)

typedef struct gui_rm_channel {
    uint8_t peak_le[2];      /* absolute peak, signed 12-bit ADC units */
    uint8_t rms_le[2];       /* RMS amplitude, signed 12-bit units */
    uint8_t peak_pos_le[2];  /* Local meter: maximum positive, first 1000 samples */
    uint8_t peak_neg_le[2];  /* Local meter: magnitude of negative, first 1000 samples */
    uint8_t scope_min_le[GUI_RM_POINTS][2]; /* signed int16 ADC minimum, LE */
    uint8_t scope_max_le[GUI_RM_POINTS][2]; /* signed int16 ADC maximum, LE */
    uint8_t spectrum_le[GUI_RM_FFT_BINS][2]; /* -120..0 dB, same as Local FFT */
} gui_rm_channel_t;

typedef struct gui_rm_packet {
    uint8_t magic[4];       /* 'MRM1' */
    uint8_t version;        /* GUI_RM_VERSION */
    uint8_t channels;       /* 1 or 2 */
    uint8_t reserved[2];
    uint8_t sample_rate_le[4];
    uint8_t sequence_le[4];
    gui_rm_channel_t ch[2];
} gui_rm_packet_t;

/* Native waveform renderer input, sent separately from the compact FFT/meter
 * packet. A high-resolution prefix preserves close-in zoom; a decimated view
 * of the full 65536-sample capture block preserves the original time axis.
 * This is bounded preview data, never the entire streaming RF signal. */
#define GUI_RM_SCOPE_SAMPLES 8192
#define GUI_RM_SCOPE_VERSION 2
#define GUI_RM_SCOPE_PACKET_BYTES (20 + 2 * 2 * GUI_RM_SCOPE_SAMPLES * 2)
typedef struct gui_rm_scope_channel {
    uint8_t native_le[GUI_RM_SCOPE_SAMPLES][2];
    uint8_t wide_le[GUI_RM_SCOPE_SAMPLES][2]; /* representative real source sample; no box-average cancellation */
} gui_rm_scope_channel_t;
typedef struct gui_rm_scope_packet {
    uint8_t magic[4];       /* 'MRS1' */
    uint8_t version;
    uint8_t channels;
    uint8_t reserved[2];
    uint8_t sample_rate_le[4];
    uint8_t sequence_le[4];
    uint8_t source_count_le[4];
    gui_rm_scope_channel_t ch[2];
} gui_rm_scope_packet_t;
bool gui_rm_scope_server_snapshot(gui_rm_scope_packet_t *out);
bool gui_rm_scope_client_accept(const void *data, size_t bytes);
bool gui_rm_scope_client_snapshot(gui_rm_scope_packet_t *out);
void gui_rm_scope_client_clear(void);

/* Called by extraction producer. At most ten updates/second, non-blocking
 * publication; skips a preview rather than waiting on an HTTP client. */
void gui_rm_submit(const int16_t *a, const int16_t *b, size_t count, uint32_t sample_rate);
bool gui_rm_server_snapshot(gui_rm_packet_t *out);
/* A client accepts ONLY exact, version-matched complete packets. */
bool gui_rm_client_accept(const void *data, size_t bytes);
bool gui_rm_client_snapshot(gui_rm_packet_t *out);
void gui_rm_client_clear(void);
uint16_t gui_rm_get_u16(const uint8_t *p);
uint32_t gui_rm_get_u32(const uint8_t *p);
int16_t gui_rm_get_s16(const uint8_t *p);
/* Vertical auto-range used by the preview renderer; minimum suppresses
 * runaway amplification of near-zero noise/silence. */
float gui_rm_waveform_range(uint16_t peak);
#endif
