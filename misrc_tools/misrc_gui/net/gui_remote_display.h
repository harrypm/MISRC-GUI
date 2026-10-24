/* Remote Monitor v5: float32 Local display data, zstd on-wire. */
#ifndef GUI_REMOTE_DISPLAY_H
#define GUI_REMOTE_DISPLAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GUI_RD_WIDTH 4096
#define GUI_RD_BINS 2049
#define GUI_RD_VERSION 5
#define GUI_RD_MAX_SOURCE 65536
/* IEEE754 binary32, little endian. Source Pi5 and Windows x86_64. */
typedef struct {
    float zoom;
    uint32_t width;
    int32_t trigger_enabled;
    int32_t trigger_level;
    int32_t trigger_mode;
    int32_t trigger_source;
} gui_rd_config_t;
typedef struct {
    uint32_t count;
    int32_t trigger_pos;
    uint16_t peak_pos, peak_neg, peak, rms;
    float waveform[GUI_RD_WIDTH];
    float fft[GUI_RD_BINS];
} gui_rd_channel_t;
typedef struct {
    char magic[4];
    uint8_t version, channels, reserved[2];
    uint32_t sample_rate, sequence;
    gui_rd_channel_t ch[2];
} gui_rd_frame_t;
/* Producer: only trylock and copy latest RF block; never wait for network. */
void gui_rd_capture(const int16_t *a,const int16_t *b,size_t count,uint32_t sample_rate,uint32_t sequence);
/* Server: process latest capture using the SAME Local SOXR/trigger and FFTW
 * functions; compress; caller frees *wire. Returns false if stale/empty. */
bool gui_rd_server_build(const gui_rd_config_t cfg[2],void **wire,size_t *wire_size,size_t *uncompressed);
/* Client: strict checked Zstd frame, fixed frame bytes, magic and version. */
bool gui_rd_client_accept(const void *wire,size_t wire_size);
bool gui_rd_client_snapshot(gui_rd_frame_t *out);
void gui_rd_client_clear(void);
/* Waveform panel requests are published on render thread and read atomically. */
void gui_rd_publish_request(int channel,gui_rd_config_t cfg);
gui_rd_config_t gui_rd_get_request(int channel);
size_t gui_rd_wire_capacity(void);
#endif
