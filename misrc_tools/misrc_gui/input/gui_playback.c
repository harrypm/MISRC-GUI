/*
 * MISRC - hsdaoh-rp2350 GUI - Playback Mode Implementation
 *
 * Reads FLAC files and plays them back as if being captured live.
 * Uses libFLAC stream decoder for reading.
 */

#include "gui_playback.h"
#include "gui_playback_length_cache.h"
#include "../core/gui_app.h"
#include "../visualization/gui_fft.h"
#if LIBFLAC_ENABLED == 1
#if LIBFFTW_ENABLED
#include <fftw3.h>
#endif
#endif
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../../common/buffer_manager.h"
#include "../../common/file_utils.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#else
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if LIBFLAC_ENABLED == 1
#include "FLAC/stream_decoder.h"
#include "FLAC/metadata.h"
#endif

//-----------------------------------------------------------------------------
// Playback State
//-----------------------------------------------------------------------------

typedef struct {
    uint16_t min_block_size;
    uint16_t max_block_size;
    uint32_t min_frame_size;
    uint32_t max_frame_size;
    uint32_t channels;
} playback_streaminfo_aux_t;

// Decoder IO layer state for libFLAC init_stream callbacks (see the LIBFLAC
// section for the callbacks and the STREAMINFO patch rationale). Deliberately
// FLAC-type-free so it can be embedded in the shared playback context.
typedef struct playback_io_ctx {
    FILE *file;
    void *owner;                // playback_ctx_t* — decoder client_data
    const atomic_bool *cancel;  // Private loader/analysis cancellation; NULL while playing
    // Staged first-bytes serving with in-memory STREAMINFO total patch
    int stage;                  // 0=off (plain fread), 1=serving staged head, 2=seek file to head_len first
    uint64_t bytes_served;      // Sequential bytes served since decoder init (valid pre-seek)
    uint64_t patch_total;       // TRUE stream total written into the STREAMINFO field
    uint64_t header_total;      // Raw STREAMINFO total before the in-memory patch
    uint8_t head[26];           // Patched file bytes 0..25 (fLaC + block header + SI up to total field)
    size_t head_len;
} playback_io_t;

// Headerless raw PCM / PCM WAV / packed .lds reader state (one per channel;
// the FILE handle is ctx->file_a/file_b itself). Everything is derived once at
// open: the audio byte span (whole file for headerless formats, the RIFF data
// chunk for WAV), the exact sample count, and the resolved real rate. Reads
// convert bytes straight into the 12-bit display container; .lds keeps a
// group-aligned cursor with a leading intra-group skip after a mid-group seek
// and a small carry for the group mates a bounded read leaves behind.
typedef struct {
    playback_input_format_t format;
    FILE *file;                 // Audio file handle (aliases ctx->file_a/file_b)
    uint32_t bits;              // On-disk width (8/16/24/32; 10 for .lds)
    uint64_t data_offset;       // First audio byte (data chunk body for WAV, else 0)
    uint64_t data_bytes;        // Audio bytes present (clamped to EOF for WAV)
    uint64_t file_size;
    uint64_t total_samples;     // Exact (size-derived; lds drops a trailing partial group)
    uint64_t cursor_sample;     // Next sample index a read converts
    uint32_t skip;              // .lds only: leading group mates to drop after a mid-group seek (0..3)
    int16_t carry[3];           // .lds only: unconsumed mates of a partially read group
    uint32_t carry_len;
} playback_raw_reader_t;

typedef struct playback_ctx {
    // File handles and decoders
#if LIBFLAC_ENABLED == 1
    FLAC__StreamDecoder *decoder_a;
    FLAC__StreamDecoder *decoder_b;
    playback_io_t io_a;         // Per-channel IO layer (callbacks + patch state)
    playback_io_t io_b;
    // Drag-and-drop audio source (settings.playback_audio_file): a private
    // WAV reader or FLAC decoder feeding the 48 k stereo monitor queue,
    // position-synced to the RF playhead (see the audio-feed section).
    struct playback_audio_source *audio;
#endif
    FILE *file_a;
    FILE *file_b;
    bool decoder_owns_file_a;
    bool decoder_owns_file_b;

    // File info
    playback_file_info_t info_a;
    playback_file_info_t info_b;
    playback_streaminfo_aux_t stream_a;
    playback_streaminfo_aux_t stream_b;
    gui_playback_length_cache_key_t loaded_key[2];
    bool loaded_key_valid[2]; // Captured from and matched against the held producer FILE
    bool seek_recovery_failed[2]; // Producer-only; failed seek could not restore its old cursor

    // Sniffed input container per channel: native FLAC keeps the libFLAC
    // decoder path verbatim; Ogg FLAC remuxes to a session temp native FLAC;
    // the raw formats (u8/s8/u16/s16/lds/WAV) drive the sample readers with
    // no libFLAC decoder at all (see the input reader section).
    int input_format[2];        // playback_input_format_t
    bool input_is_raw[2];       // Raw/WAV/.lds reader channels (no FLAC decoder)
    playback_raw_reader_t raw[2]; // Reader state (the FILE* aliases file_a/file_b)
    // Actual decode-source path per channel (the Ogg-FLAC session remux);
    // equals the info filepath for every on-disk native format. FLAC-domain
    // work (loaded-file identity, paused-analysis decoders) uses this path.
    char decode_path[2][512];
    bool decode_path_temp[2];   // decode_path is a session temp file (unlink on release)
    // 16-bit source sample-convention state per channel (FLAC frames and raw
    // s16/WAV16 reads share one sticky detector; see the conversion section).
    int conv16_state[2];

    // Decode buffers (filled by FLAC decoder callbacks)
    int16_t *decode_buf_a;
    int16_t *decode_buf_b;
    size_t decode_buf_size;        // Allocated size
    size_t decode_available_a;     // Samples available in buffer
    size_t decode_available_b;
    size_t decode_pos_a;           // Current read position in buffer
    size_t decode_pos_b;

    // Playback state
    atomic_int state;              // playback_state_t
    atomic_int speed;              // playback_speed_t
    atomic_bool loop_enabled;
    // Seek-while-paused backfill: decode the window ENDING at the seek target
    // and push it through the pipeline so the panels show the real timeline at
    // the playhead (DAW-style non-linear scrub), then hold. The spectrogram
    // clears its rolling history on the position jump (gui_spectrogram.c).
    bool backfill_active_a;
    uint64_t backfill_target_a;
    bool backfill_active_b;
    uint64_t backfill_target_b;
    atomic_uint_fast64_t seek_seq_a;  // Incremented on every successful CH A seek
    atomic_uint_fast64_t seek_seq_b;  // Incremented on every successful CH B seek
    atomic_bool seek_requested_a;
    atomic_bool seek_requested_b;
    atomic_uint_fast64_t seek_target_a;
    atomic_uint_fast64_t seek_target_b;

    // Position tracking
    atomic_uint_fast64_t current_sample;
    atomic_uint_fast64_t current_sample_a;
    atomic_uint_fast64_t current_sample_b;
    atomic_uint_fast64_t channel_total_samples[2]; // UI publishes to producer
    uint64_t total_samples;        // Max of file_a and file_b totals
    double total_duration_seconds; // Duration derived from resolved real sample rate
    double realtime_sample_rate_hz;
    double channel_rate_hz[2];

    // Thread
    void *playback_thread;
    atomic_bool running;

    // App reference
    gui_app_t *app;
} playback_ctx_t;

static playback_ctx_t s_idle_playback = {0};
static playback_ctx_t *s_playback = &s_idle_playback;

// The render thread owns the job and active-context pointer. The loader only
// writes its private prepared context; done publishes its result to UI polling.
static struct {
    thrd_t thread;
    bool pending;
    atomic_bool done;
    atomic_bool cancel;
    int result;
    playback_ctx_t *prepared;
} s_playback_start;

// A separate reader finds missing lengths after playback starts. Only the UI
// changes active metadata; each done flag publishes that channel's result.
static struct {
    thrd_t thread;
    bool pending;
    playback_ctx_t *owner; // Stable until join; worker reads immutable rates/app and atomics
    bool requested[2];
    bool published[2];
    playback_file_info_t info[2];
    playback_streaminfo_aux_t aux[2];
    uint64_t samples[2];
    atomic_bool channel_done[2];
    atomic_uint_fast64_t bytes_scanned[2];
    atomic_uint_fast64_t total_bytes[2];
    atomic_bool queued[2];
    atomic_bool waiting[2];
    atomic_bool done;
    atomic_bool cancel;
} s_playback_index;

//-----------------------------------------------------------------------------
// Raw Format Encoding
//-----------------------------------------------------------------------------

// Encode int16_t samples to the raw 32-bit capture format
// This mirrors the decoding done in extract.c
// Raw format:
//   Bits 0-11:  Channel A (12-bit, stored as 2047 - sample)
//   Bits 12-19: AUX data (8 bits, we set to 0)
//   Bits 20-31: Channel B (12-bit, stored as 2047 - sample)
static inline uint32_t encode_raw_sample(int16_t sample_a, int16_t sample_b) {
    // Clamp samples to 12-bit signed range
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    // Encode: store as (2047 - sample) to match extract.c decoding
    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    uint32_t aux = 0;  // No AUX data during playback

    return ch_a | (aux << 12) | (ch_b << 20);
}

//-----------------------------------------------------------------------------
// Speed Multipliers
//-----------------------------------------------------------------------------

static const float speed_multipliers[] = {
    0.25f,   // PLAYBACK_SPEED_0_25X
    0.5f,    // PLAYBACK_SPEED_0_5X
    1.0f,    // PLAYBACK_SPEED_1X
    2.0f,    // PLAYBACK_SPEED_2X
    4.0f,    // PLAYBACK_SPEED_4X
    0.0f     // PLAYBACK_SPEED_MAX (no delay)
};

const char* gui_playback_speed_name(playback_speed_t speed) {
    switch (speed) {
        case PLAYBACK_SPEED_0_25X: return "0.25x";
        case PLAYBACK_SPEED_0_5X:  return "0.5x";
        case PLAYBACK_SPEED_1X:    return "1x";
        case PLAYBACK_SPEED_2X:    return "2x";
        case PLAYBACK_SPEED_4X:    return "4x";
        case PLAYBACK_SPEED_MAX:   return "Max";
        default: return "?";
    }
}

//-----------------------------------------------------------------------------
// Input Sample Conversion (16-bit source conventions)
//-----------------------------------------------------------------------------

// The display/extraction pipeline consumes one fixed 12-bit container per
// sample (encode_raw_sample's raw packing, the extraction (2047 - field) << 4
// into the 16-bit display container, VU meters, peaks and clipping all match
// the live capture preview on that grid). 16-bit sources occur in TWO
// real-world conventions (measured on real captures): the MISRC recorder
// stores the 12-bit ADC container << 4 (every sample sits on a 16-grid,
// +-32752 span), while generic tools (the 20 kHz NTSC tape FLAC) store sample
// values DIRECTLY - a +-2190 peak file whose samples are 93.8% non-multiples
// of 16. A blanket >> 4 crushed the second kind to 1/16 amplitude (the
// waveform-playback accuracy bug); the detector below decides the convention
// from the first non-silent block and is sticky for the channel's session.
#define PLAYBACK_CONV16_PENDING 0
#define PLAYBACK_CONV16_SHIFT4 1  // recorder 12-bit << 4 restore (or full-range display downscale)
#define PLAYBACK_CONV16_PASS   2  // direct-domain values: pass through unchanged
#define PLAYBACK_CONV16_DECIDE_FLOOR 16  // a block peaking below this is inconclusive (silence)
#define PLAYBACK_CONV16_DIRECT_MAX 4096 // direct-domain peak ceiling (13-bit domain)
// These constants and the unguarded ctx->conv16_state member are
// dependency-independent (FLAC frames and raw s16/WAV16 reads share the
// sticky detector) and gui_playback_release_context() resets the state
// OUTSIDE the LIBFLAC guard - keep these defines outside it. Found by the
// LIBFLAC_ENABLED=0 translation proof, 2026-10-19.

#if LIBFLAC_ENABLED == 1

#if defined(_WIN32) || defined(_WIN64)
typedef __int64 playback_file_off_t;
#define PLAYBACK_FSEEK _fseeki64
#define PLAYBACK_FTELL _ftelli64
#define PLAYBACK_PATH_SEPARATOR '\\'
#else
typedef off_t playback_file_off_t;
#define PLAYBACK_FSEEK fseeko
#define PLAYBACK_FTELL ftello
#define PLAYBACK_PATH_SEPARATOR '/'
#endif

#define PLAYBACK_FLAC_TOTAL_SAMPLES_FIELD_MOD (1ULL << 36)
#define PLAYBACK_FRAME_SCAN_CHUNK_BYTES (8u << 20)
#define PLAYBACK_FRAME_SCAN_YIELD_BYTES (64u << 20)
#define PLAYBACK_MAX_HEADER_LEN 17u
#define PLAYBACK_TAIL_METADATA_BYTES (1u << 20)
#define PLAYBACK_TAIL_MAX_BYTES (8u << 20)
#define PLAYBACK_TAIL_READ_BYTES 65536u
#define PLAYBACK_TAIL_BUDGET_US 250000u
#define PLAYBACK_COMPANION_READ_LIMIT (64u * 1024u)
#define PLAYBACK_AUDIO_RATE_MATCH_TOLERANCE 0.05

typedef enum {
    PLAYBACK_TOTAL_SOURCE_UNRESOLVED = 0,
    PLAYBACK_TOTAL_SOURCE_STREAMINFO,
    PLAYBACK_TOTAL_SOURCE_VORBIS_TAG,
    PLAYBACK_TOTAL_SOURCE_VORBIS_DURATION,
    PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_CORRECTED,
    PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_ESTIMATED,
    PLAYBACK_TOTAL_SOURCE_COMPANION,
    PLAYBACK_TOTAL_SOURCE_LENGTH_CACHE,
    PLAYBACK_TOTAL_SOURCE_TAIL_ENDPOINT
} playback_total_source_t;

typedef struct {
    bool has_rf_total_samples;
    bool has_rf_sample_rate;
    bool has_duration_seconds;
    uint64_t rf_total_samples;
    uint64_t rf_sample_rate;
    double duration_seconds;
} playback_vorbis_rf_tags_t;

static const uint8_t playback_crc8_table[256] = {
    0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
    0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
    0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
    0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
    0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
    0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
    0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
    0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
    0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
    0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
    0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
    0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
    0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
    0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
    0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
    0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3,
};

static const char *gui_playback_total_source_name(playback_total_source_t source) {
    switch (source) {
        case PLAYBACK_TOTAL_SOURCE_STREAMINFO: return "STREAMINFO";
        case PLAYBACK_TOTAL_SOURCE_VORBIS_TAG: return "VORBIS_RF_TOTAL_SAMPLES";
        case PLAYBACK_TOTAL_SOURCE_VORBIS_DURATION: return "VORBIS_DURATION_SECONDS";
        case PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_CORRECTED: return "STREAMINFO_WRAP_CORRECTED";
        case PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_ESTIMATED: return "STREAMINFO_WRAP_ESTIMATED";
        case PLAYBACK_TOTAL_SOURCE_COMPANION: return "COMPANION_LOG_OR_WAV";
        case PLAYBACK_TOTAL_SOURCE_LENGTH_CACHE: return "VERIFIED_LENGTH_CACHE";
        case PLAYBACK_TOTAL_SOURCE_TAIL_ENDPOINT: return "PROVISIONAL_TAIL_ENDPOINT";
        default: return "UNRESOLVED";
    }
}

static uint64_t gui_playback_round_positive_ld(long double v) {
    if (!(v > 0.0L) || !isfinite((double)v)) {
        return 0;
    }
    if (v >= (long double)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)(v + 0.5L);
}

static bool gui_playback_is_audio_sample_rate(uint32_t header_sample_rate) {
    if (header_sample_rate == 0) return false;
    static const uint32_t audio_rates[] = {
        22050u, 24000u, 32000u, 44100u, 48000u, 64000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u
    };
    for (size_t i = 0; i < sizeof(audio_rates) / sizeof(audio_rates[0]); i++) {
        double target = (double)audio_rates[i];
        if (fabs((double)header_sample_rate - target) <= (target * PLAYBACK_AUDIO_RATE_MATCH_TOLERANCE)) {
            return true;
        }
    }
    return false;
}

static bool gui_playback_extract_msps_hint(const char *path, double *out_msps) {
    if (!path || !out_msps) return false;
    const unsigned char *b = (const unsigned char *)path;
    size_t len = strlen(path);
    size_t i = 0;
    while (i < len) {
        if (!isdigit(b[i])) {
            i++;
            continue;
        }
        size_t start = i;
        bool seen_dot = false;
        while (i < len) {
            if (isdigit(b[i])) {
                i++;
            } else if (b[i] == '.' && !seen_dot) {
                seen_dot = true;
                i++;
            } else {
                break;
            }
        }

        if (i + 4 <= len &&
            tolower(b[i]) == 'm' &&
            tolower(b[i + 1]) == 's' &&
            tolower(b[i + 2]) == 'p' &&
            tolower(b[i + 3]) == 's') {
            size_t n = i - start;
            if (n > 0 && n < 64) {
                char temp[64];
                memcpy(temp, &path[start], n);
                temp[n] = '\0';
                char *endptr = NULL;
                errno = 0;
                double v = strtod(temp, &endptr);
                if (errno == 0 && endptr != temp && *endptr == '\0' && isfinite(v) && v > 0.0) {
                    *out_msps = v;
                    return true;
                }
            }
        }
    }
    return false;
}

static double gui_playback_resolve_real_rate_hz(const char *filepath,
                                                 uint32_t header_sample_rate,
                                                 const playback_vorbis_rf_tags_t *tags) {
    if (tags && tags->has_rf_sample_rate && tags->rf_sample_rate > 0) {
        return (double)tags->rf_sample_rate;
    }
    if (header_sample_rate == 0) {
        return 0.0;
    }
    if (gui_playback_is_audio_sample_rate(header_sample_rate)) {
        return (double)header_sample_rate;
    }
    double msps_hint = 0.0;
    if (gui_playback_extract_msps_hint(filepath, &msps_hint) && msps_hint > 0.0) {
        return msps_hint * 1000000.0;
    }
    return (double)header_sample_rate * 1000.0;
}

static bool gui_playback_eq_ascii_ci_char(char a, char b) {
    if (a == b) return true;
    return (char)tolower((unsigned char)a) == (char)tolower((unsigned char)b);
}

static bool gui_playback_eq_ascii_ci_bytes(const uint8_t *lhs, size_t lhs_len, const char *rhs) {
    if (!lhs || !rhs) return false;
    size_t rhs_len = strlen(rhs);
    if (lhs_len != rhs_len) return false;
    for (size_t i = 0; i < lhs_len; i++) {
        if (!gui_playback_eq_ascii_ci_char((char)lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

static bool gui_playback_parse_u64_bytes(const uint8_t *src, size_t len, uint64_t *out_value) {
    if (!src || !out_value) return false;
    size_t start = 0;
    size_t end = len;
    while (start < end && isspace((unsigned char)src[start])) start++;
    while (end > start && isspace((unsigned char)src[end - 1])) end--;
    if (start >= end) return false;
    uint64_t value = 0;
    for (size_t i = start; i < end; i++) {
        if (src[i] < '0' || src[i] > '9') return false;
        uint64_t digit = (uint64_t)(src[i] - '0');
        if (value > (UINT64_MAX - digit) / 10ULL) {
            return false;
        }
        value = value * 10ULL + digit;
    }
    *out_value = value;
    return true;
}

static bool gui_playback_parse_f64_bytes(const uint8_t *src, size_t len, double *out_value) {
    if (!src || !out_value || len == 0) return false;
    while (len > 0 && isspace((unsigned char)*src)) {
        src++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        len--;
    }
    if (len == 0 || len >= 128) {
        return false;
    }
    char temp[128];
    memcpy(temp, src, len);
    temp[len] = '\0';
    char *endptr = NULL;
    errno = 0;
    double value = strtod(temp, &endptr);
    if (errno != 0 || endptr == temp || *endptr != '\0' || !isfinite(value)) {
        return false;
    }
    *out_value = value;
    return true;
}

static bool gui_playback_read_vorbis_rf_tags(const char *filepath, playback_vorbis_rf_tags_t *out_tags) {
    if (!filepath || !out_tags) return false;
    memset(out_tags, 0, sizeof(*out_tags));

    FLAC__StreamMetadata *tags_block = NULL;
    if (!FLAC__metadata_get_tags(filepath, &tags_block)) {
        return false;
    }
    if (!tags_block || tags_block->type != FLAC__METADATA_TYPE_VORBIS_COMMENT) {
        if (tags_block) {
            FLAC__metadata_object_delete(tags_block);
        }
        return false;
    }

    FLAC__StreamMetadata_VorbisComment *vc = &tags_block->data.vorbis_comment;
    for (uint32_t i = 0; i < vc->num_comments; i++) {
        FLAC__StreamMetadata_VorbisComment_Entry *entry = &vc->comments[i];
        if (!entry->entry || entry->length == 0) continue;
        uint32_t split = 0;
        while (split < entry->length && entry->entry[split] != '=') split++;
        if (split == 0 || split >= entry->length) continue;

        const uint8_t *key = entry->entry;
        size_t key_len = split;
        const uint8_t *value = entry->entry + split + 1;
        size_t value_len = (size_t)entry->length - (size_t)split - 1u;

        if (!out_tags->has_rf_total_samples &&
            gui_playback_eq_ascii_ci_bytes(key, key_len, "RF_TOTAL_SAMPLES")) {
            uint64_t parsed = 0;
            if (gui_playback_parse_u64_bytes(value, value_len, &parsed) && parsed > 0) {
                out_tags->has_rf_total_samples = true;
                out_tags->rf_total_samples = parsed;
            }
        } else if (!out_tags->has_rf_sample_rate &&
                   gui_playback_eq_ascii_ci_bytes(key, key_len, "RF_SAMPLE_RATE")) {
            uint64_t parsed = 0;
            if (gui_playback_parse_u64_bytes(value, value_len, &parsed) && parsed > 0) {
                out_tags->has_rf_sample_rate = true;
                out_tags->rf_sample_rate = parsed;
            }
        } else if (!out_tags->has_duration_seconds &&
                   gui_playback_eq_ascii_ci_bytes(key, key_len, "DURATION_SECONDS")) {
            double parsed = 0.0;
            if (gui_playback_parse_f64_bytes(value, value_len, &parsed) && parsed > 0.0) {
                out_tags->has_duration_seconds = true;
                out_tags->duration_seconds = parsed;
            }
        }
    }

    FLAC__metadata_object_delete(tags_block);
    return out_tags->has_rf_total_samples || out_tags->has_rf_sample_rate || out_tags->has_duration_seconds;
}

static bool gui_playback_file_seek_abs(FILE *file, uint64_t offset) {
    if (!file) return false;
    if (offset > (uint64_t)INT64_MAX) return false;
    return PLAYBACK_FSEEK(file, (playback_file_off_t)offset, SEEK_SET) == 0;
}

static bool gui_playback_file_seek_rel(FILE *file, uint64_t offset) {
    if (!file) return false;
    if (offset > (uint64_t)INT64_MAX) return false;
    return PLAYBACK_FSEEK(file, (playback_file_off_t)offset, SEEK_CUR) == 0;
}

static bool gui_playback_file_tell(FILE *file, uint64_t *out_offset) {
    if (!file || !out_offset) return false;
    playback_file_off_t pos = PLAYBACK_FTELL(file);
    if (pos < 0) return false;
    *out_offset = (uint64_t)pos;
    return true;
}

 //-----------------------------------------------------------------------------
// Decoder IO Layer (libFLAC init_stream callbacks + STREAMINFO patch)
 //-----------------------------------------------------------------------------
 /* The recorder stores the RF sample rate in the FLAC header as a kHz-domain
 * value (20 MSPS -> 20000 Hz) and, on finalized files, scales STREAMINFO
 * total_samples to RF/1000 so the header duration matches real time. The
 * audio stream still contains one FLAC sample per RF sample, so finalized
 * files hold ~1000x more samples than their header claims. libFLAC
 * refuses seeks to any sample >= STREAMINFO total_samples and ignores
 * seektable points beyond it, which silently broke timeline scrubbing on
 * finalized captures (seek_absolute returned false; the old code ignored
 * the return value and desynced the position tracker).
 *
 * Feeding the decoder through our own read callback lets us serve a PATCHED
 * copy of the first 26 file bytes (the STREAMINFO total_samples field, file
 * offsets 18..25, low 36 bits) with the probe-resolved TRUE stream total —
 * the file on disk is never modified. Seeking, the seektable and EOF
 * handling then all operate on real stream positions. */

#define PLAYBACK_IO_HEAD_LEN 26u
#define PLAYBACK_IO_SI_TOTAL_OFF 18u
#define PLAYBACK_IO_SI_TOTAL_END 25u /* inclusive */
#define PLAYBACK_IO_SI_TOTAL_MASK 0xFFFFFFFFFULL
#define PLAYBACK_IO_SI_TOTAL_MOD (1ULL << 36)   /* STREAMINFO total_samples field width */
#define PLAYBACK_IO_STAGE_OFF 0
#define PLAYBACK_IO_STAGE_SERVING 1
#define PLAYBACK_IO_STAGE_SEEK_FILE 2

static void gui_playback_io_reset(playback_io_t *io, FILE *file, void *owner,
                                  uint64_t patch_total) {
    if (!io) return;
    memset(io, 0, sizeof(*io));
    io->file = file;
    io->owner = owner;
    io->patch_total = patch_total;
    io->stage = PLAYBACK_IO_STAGE_OFF;
    io->head_len = 0;

    if (!io->file || patch_total == 0) {
        return;
    }

    /* Stage + patch the STREAMINFO header region. The file is expected at
     * offset 0 (fresh open); rewind defensively before reading. */
    if (PLAYBACK_FSEEK(io->file, 0, SEEK_SET) != 0) {
        return;
    }
    io->head_len = fread(io->head, 1, PLAYBACK_IO_HEAD_LEN, io->file);
    if (PLAYBACK_FSEEK(io->file, 0, SEEK_SET) != 0) {
        io->head_len = 0;
        return;
    }
    if (io->head_len != PLAYBACK_IO_HEAD_LEN) {
        io->head_len = 0;
        return;
    }
    /* Sanity: "fLaC" magic and first metadata block must be STREAMINFO. */
    if (memcmp(io->head, "fLaC", 4) != 0 || (io->head[4] & 0x7Fu) != 0x00u) {
        io->head_len = 0;
        return;
    }

    /* The STREAMINFO total_samples field is only 36 bits (max ~68.7G
     * samples). Long captures exceed it (~28 min at 40 MSPS, ~57 min at
     * 20 MSPS, ~3h49m at 5 MSPS), so the TRUE stream total cannot always be
     * represented. When it cannot, write 0 ("unknown") instead: libFLAC
     * then applies NO seek clamp (seek_absolute rejects targets >= the
     * claimed total only when a total is claimed) and resolves seeks via
     * the seektable / binary search over the real stream. Verified against
     * a 130 GB / 225G-sample capture where patching the true count truncated
     * to total % 2^36 and rejected every seek past that point. */
    uint64_t field_total = (patch_total >= PLAYBACK_IO_SI_TOTAL_MOD) ? 0 : patch_total;

    uint64_t v = 0;
    for (size_t k = 0; k < 8; k++) {
        v = (v << 8) | (uint64_t)io->head[PLAYBACK_IO_SI_TOTAL_OFF + k];
    }
    io->header_total = v & PLAYBACK_IO_SI_TOTAL_MASK;
    v = (v & ~PLAYBACK_IO_SI_TOTAL_MASK) | (field_total & PLAYBACK_IO_SI_TOTAL_MASK);
    for (size_t k = 0; k < 8; k++) {
        io->head[PLAYBACK_IO_SI_TOTAL_OFF + k] =
            (uint8_t)(v >> (56u - 8u * (unsigned)k));
    }
    io->stage = PLAYBACK_IO_STAGE_SERVING;
}

static FLAC__StreamDecoderReadStatus gui_playback_io_read_cb(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    if (!io || !io->file || !buffer || !bytes || *bytes == 0 ||
        (io->cancel && atomic_load(io->cancel))) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    size_t want = *bytes;
    size_t n = 0;

    if (io->stage == PLAYBACK_IO_STAGE_SERVING && io->bytes_served < (uint64_t)io->head_len) {
        size_t staged_left = (size_t)((uint64_t)io->head_len - io->bytes_served);
        n = (want < staged_left) ? want : staged_left;
        if (n > 0) {
            memcpy(buffer, io->head + (size_t)io->bytes_served, n);
        }
        if (io->bytes_served + (uint64_t)n >= (uint64_t)io->head_len) {
            io->stage = PLAYBACK_IO_STAGE_SEEK_FILE;
        }
    } else {
        if (io->stage == PLAYBACK_IO_STAGE_SEEK_FILE) {
            /* Staged head fully served: resume real reads after the staged
             * region (the underlying file handle is still at offset 0). */
            if (PLAYBACK_FSEEK(io->file, (playback_file_off_t)io->head_len, SEEK_SET) != 0) {
                return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
            }
            io->stage = PLAYBACK_IO_STAGE_OFF;
        }
        n = fread(buffer, 1, want, io->file);
    }

    *bytes = n;
    if (n == 0) {
        return feof(io->file) ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
                              : FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    io->bytes_served += (uint64_t)n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus gui_playback_io_seek_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    if (!io || !io->file || (io->cancel && atomic_load(io->cancel)))
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    if (absolute_byte_offset > (FLAC__uint64)INT64_MAX) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    if (PLAYBACK_FSEEK(io->file, (playback_file_off_t)absolute_byte_offset, SEEK_SET) != 0) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    /* Any seek ends staged-head serving semantics (none occur before the
     * initial metadata pass completes, but be safe). */
    if (io->stage != PLAYBACK_IO_STAGE_OFF) {
        io->stage = PLAYBACK_IO_STAGE_OFF;
    }
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus gui_playback_io_tell_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    if (!io || !io->file || !absolute_byte_offset) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    if (io->stage != PLAYBACK_IO_STAGE_OFF) {
        /* While serving the staged head, the reported stream position is the
         * logical position, not the underlying handle position. */
        *absolute_byte_offset = io->bytes_served;
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }
    uint64_t off = 0;
    if (!gui_playback_file_tell(io->file, &off)) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    *absolute_byte_offset = off;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus gui_playback_io_length_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    if (!io || !io->file || !stream_length) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }
    uint64_t cur = 0;
    if (!gui_playback_file_tell(io->file, &cur)) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }
    uint64_t size = 0;
    if (PLAYBACK_FSEEK(io->file, 0, SEEK_END) != 0 ||
        !gui_playback_file_tell(io->file, &size) ||
        !gui_playback_file_seek_abs(io->file, cur)) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }
    *stream_length = size;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool gui_playback_io_eof_cb(const FLAC__StreamDecoder *decoder, void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    return (io && io->file) ? (feof(io->file) != 0) : true;
}

//-----------------------------------------------------------------------------
// DAW-style paused analysis window — state types (implementation at the end)
//-----------------------------------------------------------------------------
// A dedicated analysis decoder pool per channel renders windows straight from
// the FLAC file while paused (the rolling pipeline only carries one
// 65,536-sample display frame — see the full notes at the end of this file).
// The window decode is split across seek-aligned segment decoders so span
// changes render in a fraction of the sequential decode time.
#define PLAYBACK_ANALYSIS_DECODE_SEGMENTS 8

// One parallel decode segment: its own FILE handle + IO layer + decoder
// (libFLAC decoders are single-threaded), plus its own FFTW plan/scratch.
typedef struct playback_analysis_segment {
    // Persistent segment decoder (created once, reused across windows)
    FILE *file;
    playback_io_t io;
    FLAC__StreamDecoder *decoder;
    bool decoder_ready;
    // FFTW engine (worker side): plans created/re-created in the WORKER
    // thread sequentially (FFTW plan creation is not thread-safe; executing
    // distinct plans IS thread-safe). One true FFT per column over the
    // column's full contiguous samples — the render thread never FFTs.
    fftwf_plan fft_plan;
    int fft_plan_size;            // Padded FFT size of the current plan (0 = none)
    float *fft_in;                 // Column scratch: PLAYBACK_ANALYSIS_HOP_MAX floats
    fftwf_complex *fft_out;        // (HOP_MAX/2+1) complex
    float *frame_buf;              // Magnitude scratch: (HOP_MAX/2+1) floats (NEVER
                                   // a stack array: hop_bins reaches 131073 and a
                                   // 2049-float stack buffer caused a stack-smashing
                                   // segfault on the first flush at 3s/20MSPS)
    // Per-request decode state (worker/segment thread only)
    uint64_t seg_start;          // Absolute sample position of the segment
    uint64_t seg_span;           // Samples this segment decodes
    size_t slice_first;          // First global slice index it captures
    size_t slice_count;          // Slices this segment captures
    size_t hop;                   // Contiguous samples per column (shared)
    uint64_t decoded;           // Samples decoded since seg_start
    size_t slices_done;         // Columns completed so far (local, 0-based)
    size_t col_pos;              // Samples accumulated in the current column
    int64_t col_dc_sum;          // DC accumulator for the current column
    float *dst;                  // Target magnitude buffer (unpublished)
    uint32_t errors_logged;
} playback_analysis_segment_t;

typedef struct {
    atomic_bool inited;          // Decoders + worker ready
    atomic_bool failed;          // Could not be created (logged once)
    uint64_t total_samples;      // Worker snapshot of the latest requested total
    char session_path[FILE_PATH_MAX];         // Immutable session path, copied before thread start
    playback_ctx_t *owner;       // Held until analysis shutdown joins the worker

    // Request (render thread writes params, then bumps req_seq)
    uint64_t req_center;
    uint64_t req_span;           // Span in SAMPLES (computed from duration)
    uint64_t req_total;          // Render-thread length publication snapshot
    atomic_bool locked;         // Short request/result publication critical section
    atomic_uint_fast64_t req_seq;

    // Worker + results (worker fills the unpublished buffer, publishes the
    // index, then bumps done_seq; the render thread reads the published one)
    atomic_bool worker_stop;
    thrd_t worker;
    bool worker_valid;
    atomic_uint_fast64_t done_seq;
    atomic_int publish_idx;      // Latest completed magnitude buffer
    int reader_idx;             // Pinned by render until the next successful poll
    float *mag_buf[3];          // Published, render-pinned, and worker buffers
    // Per-request bookkeeping (worker only)
    uint64_t win_start;
    uint64_t win_span;
    size_t hop;                   // Contiguous samples per column
    // Parallel decode segments (persistent decoders, dispatched per request)
    playback_analysis_segment_t segs[PLAYBACK_ANALYSIS_DECODE_SEGMENTS];
    size_t seg_count;            // Segments successfully initialized
} playback_analysis_t;

static playback_analysis_t s_analysis[2] = {0};

static void gui_playback_analysis_shutdown(playback_analysis_t *a);
static void gui_playback_analysis_release(playback_analysis_t *a);
static void gui_playback_analysis_lock(playback_analysis_t *a) {
    while (atomic_exchange(&a->locked, true)) thrd_sleep_ms(1);
}
static void gui_playback_analysis_unlock(playback_analysis_t *a) {
    atomic_store(&a->locked, false);
}

static bool gui_playback_find_audio_offset_and_size(const char *filepath, uint64_t *out_audio_offset, uint64_t *out_file_size) {
    if (!filepath || !out_audio_offset || !out_file_size) return false;
    *out_audio_offset = 0;
    *out_file_size = 0;

    FILE *file = file_open_read_utf8(filepath);
    if (!file) return false;

    bool ok = false;
    do {
        if (PLAYBACK_FSEEK(file, 0, SEEK_END) != 0) break;
        if (!gui_playback_file_tell(file, out_file_size)) break;
        if (!gui_playback_file_seek_abs(file, 0)) break;

        uint8_t magic[4];
        if (fread(magic, 1, sizeof(magic), file) != sizeof(magic)) break;
        if (memcmp(magic, "fLaC", 4) != 0) break;

        while (true) {
            uint8_t header[4];
            if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
                goto done_audio_offset;
            }
            bool is_last = (header[0] & 0x80u) != 0;
            uint32_t length = ((uint32_t)header[1] << 16) |
                              ((uint32_t)header[2] << 8) |
                              (uint32_t)header[3];
            if (!gui_playback_file_seek_rel(file, length)) {
                goto done_audio_offset;
            }
            if (is_last) {
                if (!gui_playback_file_tell(file, out_audio_offset)) {
                    goto done_audio_offset;
                }
                ok = true;
                goto done_audio_offset;
            }
        }
    } while (0);

done_audio_offset:
    fclose(file);
    return ok;
}

static bool gui_playback_read_utf8_coded(const uint8_t *w, size_t len, uint64_t *out_value, size_t *out_consumed) {
    if (!w || len == 0 || !out_value || !out_consumed) return false;
    uint8_t first = w[0];
    uint8_t read_additional = 0;
    uint8_t mask_data = 0x7F;
    uint8_t mask_mark = 0x80;
    while (first & mask_mark) {
        read_additional++;
        mask_data >>= 1;
        mask_mark >>= 1;
    }
    if (read_additional > 0) {
        if (read_additional == 1) {
            return false;
        }
        read_additional--;
    }
    size_t total = 1u + (size_t)read_additional;
    if (total > len) return false;

    uint64_t value = ((uint64_t)(first & mask_data)) << (6u * (unsigned)read_additional);
    for (size_t k = 0; k < (size_t)read_additional; k++) {
        uint8_t byte = w[1 + k];
        if ((byte & 0xC0u) != 0x80u) return false;
        size_t shift = 6u * ((size_t)read_additional - 1u - k);
        value |= ((uint64_t)(byte & 0x3Fu)) << shift;
    }
    *out_value = value;
    *out_consumed = total;
    return true;
}

static bool gui_playback_parse_frame_header(const uint8_t *w, size_t len,
                                            uint16_t *out_block_size, size_t *out_header_len,
                                            uint64_t *out_number, bool *out_variable) {
    if (!w || len < 4 || !out_block_size || !out_header_len || !out_number || !out_variable) {
        return false;
    }

    uint16_t sync = (uint16_t)(((uint16_t)w[0] << 8) | (uint16_t)w[1]);
    if ((sync & 0xFFFEu) != 0xFFF8u) return false;
    if (sync & 0x0002u) return false;
    bool variable = (sync & 0x0001u) != 0;

    uint8_t bs_sr = w[2];
    uint16_t block_size = 0;
    bool read_8bit_bs = false;
    bool read_16bit_bs = false;
    uint8_t block_code = (uint8_t)(bs_sr >> 4);
    if (block_code == 0x0u) {
        return false;
    } else if (block_code == 0x1u) {
        block_size = 192;
    } else if (block_code >= 0x2u && block_code <= 0x5u) {
        block_size = (uint16_t)(576u * (1u << (block_code - 2u)));
    } else if (block_code == 0x6u) {
        read_8bit_bs = true;
    } else if (block_code == 0x7u) {
        read_16bit_bs = true;
    } else {
        block_size = (uint16_t)(256u * (1u << (block_code - 8u)));
    }

    bool read_8bit_sr = false;
    bool read_16bit_sr = false;
    bool read_16bit_sr_ten = false;
    uint8_t sample_rate_code = (uint8_t)(bs_sr & 0x0Fu);
    if (sample_rate_code == 0xCu) {
        read_8bit_sr = true;
    } else if (sample_rate_code == 0xDu) {
        read_16bit_sr = true;
    } else if (sample_rate_code == 0xEu) {
        read_16bit_sr_ten = true;
    } else if (sample_rate_code == 0xFu) {
        return false;
    }

    uint8_t chan_bps_res = w[3];
    if ((chan_bps_res >> 4) >= 0xBu) return false;
    uint8_t bps_code = (uint8_t)((chan_bps_res & 0x0Eu) >> 1);
    if (!(bps_code == 0 || bps_code == 1 || bps_code == 2 || bps_code == 4 || bps_code == 5 || bps_code == 6)) {
        return false;
    }
    if (chan_bps_res & 0x01u) return false;

    uint64_t number = 0;
    size_t utf8_len = 0;
    if (!gui_playback_read_utf8_coded(&w[4], len - 4u, &number, &utf8_len)) {
        return false;
    }
    size_t pos = 4u + utf8_len;

    if (read_8bit_bs) {
        if (pos + 1u > len) return false;
        block_size = (uint16_t)w[pos] + 1u;
        pos += 1u;
    }
    if (read_16bit_bs) {
        if (pos + 2u > len) return false;
        uint16_t bs = (uint16_t)(((uint16_t)w[pos] << 8) | (uint16_t)w[pos + 1u]);
        if (bs == 0xFFFFu) return false;
        block_size = (uint16_t)(bs + 1u);
        pos += 2u;
    }
    if (read_8bit_sr) {
        if (pos + 1u > len) return false;
        pos += 1u;
    }
    if (read_16bit_sr || read_16bit_sr_ten) {
        if (pos + 2u > len) return false;
        pos += 2u;
    }

    if (pos + 1u > len) return false;
    uint8_t crc = 0;
    for (size_t i = 0; i < pos; i++) {
        crc = playback_crc8_table[(uint8_t)(crc ^ w[i])];
    }
    if (crc != w[pos]) return false;

    *out_block_size = block_size;
    *out_header_len = pos + 1u;
    *out_number = number;
    *out_variable = variable;
    return true;
}

// Decide (or keep) the 16-bit conversion mode from one frame/block of samples.
// Thresholds, chosen from the measured files:
//   - every sample == 0 (mod 16): the recorder's << 4 grid -> >> 4 restore
//     (RF noise never lands on that grid by chance; a quiet recorder capture
//     is still all-mod-16, so this decision never depends on level)
//   - otherwise peak <= 4096: direct-domain storage (the tape file peaks at
//     +-2190) -> pass through unchanged
//   - otherwise: full-range generic 16-bit -> >> 4 display-container downscale
// A block whose peak stays under 16 (digital silence) leaves the decision
// pending: zero samples convert identically under every convention, so a
// silent lead can never mislead the detector.
static void gui_playback_decide_conv16(int *state, const int32_t *in, uint32_t samples) {
    if (!state || *state != PLAYBACK_CONV16_PENDING || !in || samples == 0) return;
    int32_t peak = 0;
    bool all_mod16 = true;
    for (uint32_t i = 0; i < samples; i++) {
        int32_t v = in[i];
        int32_t mag = v < 0 ? -v : v;
        if (mag > peak) peak = mag;
        if (v & 0xF) all_mod16 = false;
    }
    if (peak < PLAYBACK_CONV16_DECIDE_FLOOR) return;
    *state = (all_mod16 || peak > PLAYBACK_CONV16_DIRECT_MAX)
        ? PLAYBACK_CONV16_SHIFT4 : PLAYBACK_CONV16_PASS;
}

static int16_t gui_playback_apply_conv16(int state, int32_t v) {
    // PASS keeps direct-domain values (encode_raw_sample clamps into the raw
    // 12-bit packing domain downstream); SHIFT4 covers both the recorder
    // restore and the full-range display downscale in one operation.
    return state == PLAYBACK_CONV16_PASS ? (int16_t)v : (int16_t)(v >> 4);
}

// Map a signed sample of `bits` width (<= 12) into the 12-bit container by
// left-justification (8-bit << 4, 10-bit << 2); wider depths take the top 12
// bits (24-bit >> 12, 32-bit >> 20). Callers center unsigned formats first
// (WAV 8-bit at 128, cxadc u16 ten-bit at 512).
static int16_t gui_playback_scale_to_container(int32_t v, int bits) {
    if (bits >= 12) return (int16_t)(v >> (bits - 12));
    return (int16_t)(v << (12 - bits));
}

//-----------------------------------------------------------------------------
// Input Container Sniffing (content first, extension only for raw PCM)
//-----------------------------------------------------------------------------

// FLAC-Chop's model: the container is ALWAYS decided by the first bytes
// (fLaC -> native FLAC, OggS + 0x7F "FLAC" mapping -> Ogg FLAC, RIFF/WAVE ->
// PCM WAV), so renamed captures just work; the extension is only consulted
// for headerless raw PCM (which carries no magic at all) and the packed .lds.
// Known container extensions WITHOUT their magic still route to the matching
// prober so the failure names what the file actually is ("no Ogg capture
// pattern at offset 0"), never a generic "unsupported".
static bool gui_playback_ext_equals(const char *filename, const char *ext) {
    if (!filename || !ext) return false;
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename || !dot[1]) return false;
    return gui_playback_eq_ascii_ci_bytes((const uint8_t *)(dot + 1), strlen(dot + 1), ext);
}

// Sniff the container of `filepath`. Returns true with *out_fmt set (and the
// first four bytes copied to out_magic for precise error text); returns false
// with a specific, actionable message in `err` for files playback cannot read
// at all (non-FLAC Ogg codecs, ID3-prefixed FLAC, unknown types).
static bool gui_playback_sniff_format(const char *filepath, playback_input_format_t *out_fmt,
                                      uint8_t out_magic[4], char *err, size_t err_len) {
    if (!filepath || !out_fmt || !out_magic || !err || err_len == 0) return false;
    err[0] = '\0';
    *out_fmt = PLAYBACK_INPUT_FLAC;
    out_magic[0] = out_magic[1] = out_magic[2] = out_magic[3] = 0;

    const char *filename = filepath;
    const char *slash = strrchr(filepath, '/');
    const char *backslash = strrchr(filepath, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (slash) filename = slash + 1;

    FILE *file = file_open_read_utf8(filepath);
    if (!file) {
        snprintf(err, err_len, "open failed: %s", filepath);
        return false;
    }
    uint8_t head[64] = {0};
    size_t n = fread(head, 1, sizeof(head), file);
    fclose(file);
    memcpy(out_magic, head, 4);

    if (n >= 4 && memcmp(head, "fLaC", 4) == 0) {
        *out_fmt = PLAYBACK_INPUT_FLAC;
        return true;
    }
    if (n >= 4 && memcmp(head, "OggS", 4) == 0) {
        // Classify from the first page payload (never by extension): only an
        // Ogg FLAC mapping header is playable; name the codec otherwise so
        // the user gets a real error instead of a decode failure.
        const char *codec = "unknown";
        bool classified = false;
        if (n > 27u + head[26]) {
            const uint8_t *payload = head + 27u + head[26];
            size_t payload_len = n - (size_t)(27u + head[26]);
            if (payload_len >= 5 && payload[0] == 0x7F && memcmp(payload + 1, "FLAC", 4) == 0) {
                *out_fmt = PLAYBACK_INPUT_OGG_FLAC;
                return true;
            }
            if (payload_len >= 7 && payload[0] == 0x01 && memcmp(payload + 1, "vorbis", 6) == 0) {
                codec = "Vorbis";
            } else if (payload_len >= 8 && memcmp(payload, "OpusHead", 8) == 0) {
                codec = "Opus";
            } else if (payload_len >= 8 && memcmp(payload, "Speex   ", 8) == 0) {
                codec = "Speex";
            } else if (payload_len >= 7 && payload[0] == 0x80 && memcmp(payload + 1, "theora", 6) == 0) {
                codec = "Theora";
            }
            classified = payload_len > 0;
        }
        (void)classified;
        snprintf(err, err_len,
                 "this is an Ogg %s stream, not Ogg FLAC - playback only cuts FLAC RF/audio captures",
                 codec);
        return false;
    }
    if (n >= 3 && memcmp(head, "ID3", 3) == 0) {
        snprintf(err, err_len,
                 "this FLAC carries a leading ID3v2 tag, which playback cannot read - strip it first, "
                 "e.g. `ffmpeg -i in.flac -c copy -map_metadata -1 -write_id3v2 0 out.flac`");
        return false;
    }
    if (n >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
        *out_fmt = PLAYBACK_INPUT_WAV;
        return true;
    }

    // Headerless from here: only the raw PCM extensions and the packed .lds.
    if (gui_playback_ext_equals(filename, "u8") || gui_playback_ext_equals(filename, "r8") ||
        gui_playback_ext_equals(filename, "8u")) {
        *out_fmt = PLAYBACK_INPUT_U8;
        return true;
    }
    if (gui_playback_ext_equals(filename, "s8") || gui_playback_ext_equals(filename, "8s")) {
        *out_fmt = PLAYBACK_INPUT_S8;
        return true;
    }
    if (gui_playback_ext_equals(filename, "u16") || gui_playback_ext_equals(filename, "r16") ||
        gui_playback_ext_equals(filename, "16u")) {
        *out_fmt = PLAYBACK_INPUT_U16;
        return true;
    }
    if (gui_playback_ext_equals(filename, "s16") || gui_playback_ext_equals(filename, "16s")) {
        *out_fmt = PLAYBACK_INPUT_S16;
        return true;
    }
    if (gui_playback_ext_equals(filename, "lds")) {
        *out_fmt = PLAYBACK_INPUT_LDS;
        return true;
    }
    if (gui_playback_ext_equals(filename, "raw") || gui_playback_ext_equals(filename, "bin") ||
        gui_playback_ext_equals(filename, "pcm")) {
        // 8-bit vs 16-bit is ambiguous without a header; the cxadc 8-bit mode
        // is by far the most common, so default to u8 and warn (FLAC-Chop's
        // choice). Use .u8/.s8/.u16/.s16 to disambiguate.
        fprintf(stderr,
                "[PLAYBACK] headerless .raw/.bin/.pcm: assumed unsigned 8-bit mono; "
                "use .u8/.s8/.u16/.s16 extensions to disambiguate\n");
        *out_fmt = PLAYBACK_INPUT_U8;
        return true;
    }
    // Known container extensions without their magic still go to the matching
    // prober for a precise error naming the actual first bytes.
    if (gui_playback_ext_equals(filename, "flac")) {
        *out_fmt = PLAYBACK_INPUT_FLAC;
        return true;
    }
    if (gui_playback_ext_equals(filename, "wav")) {
        *out_fmt = PLAYBACK_INPUT_WAV;
        return true;
    }
    if (gui_playback_ext_equals(filename, "ldf") || gui_playback_ext_equals(filename, "oga") ||
        gui_playback_ext_equals(filename, "ogg")) {
        *out_fmt = PLAYBACK_INPUT_OGG_FLAC;
        return true;
    }
    snprintf(err, err_len,
             "unsupported file type (%s): expected FLAC (fLaC marker), Ogg FLAC (OggS), WAV (RIFF), "
             "or raw u8/s8/u16/s16 RF data (also .8u/.8s/.16u/.16s/.raw/.bin/.pcm/.lds)",
             filename);
    return false;
}

//-----------------------------------------------------------------------------
// Raw / WAV / .lds sample readers
//-----------------------------------------------------------------------------

#define PLAYBACK_LDS_GROUP_BYTES 5u     // one DdD packing group: 4 samples
#define PLAYBACK_LDS_GROUP_SAMPLES 4u
#define PLAYBACK_LDS_DEFAULT_MSPS 40.0   // every .lds is a 40 MSPS ld-decode RF capture
                                        // (ld-lds-converter's own default rate, used
                                        // silently); an <n>msps hint overrides

// Parse a PCM RIFF/WAVE header: walk the chunk list (skipping unknown chunks
// like JUNK/LIST/bext, honouring the odd-chunk pad byte) to find `fmt ` and
// `data`. Streaming writers store 0 or 0xFFFFFFFF as the data size; the data
// span is clamped to EOF in every case. Only PCM (0x0001) and
// WAVE_FORMAT_EXTENSIBLE (0xFFFE) are accepted, 8/16/24/32-bit mono.
static bool gui_playback_parse_wav(FILE *file, uint64_t file_size,
                                   uint32_t *out_rate, uint32_t *out_bits,
                                   uint16_t *out_channels,
                                   uint64_t *out_data_offset, uint64_t *out_data_bytes,
                                   char *err, size_t err_len) {
    uint8_t riff[12];
    if (!gui_playback_file_seek_abs(file, 0) || fread(riff, 1, 12, file) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        snprintf(err, err_len, "not a RIFF/WAVE file");
        return false;
    }
    bool have_fmt = false, have_data = false;
    uint32_t rate = 0, bits = 0;
    uint16_t wav_channels = 0;   // fmt-branch value published to the tail
    uint64_t data_offset = 0, data_bytes = 0;
    uint64_t pos = 12;
    while (!have_fmt || !have_data) {
        if (pos + 8 > file_size) break;
        uint8_t hdr[8];
        if (!gui_playback_file_seek_abs(file, pos) || fread(hdr, 1, 8, file) != 8) break;
        uint64_t len = (uint64_t)hdr[4] | ((uint64_t)hdr[5] << 8) |
                      ((uint64_t)hdr[6] << 16) | ((uint64_t)hdr[7] << 24);
        uint64_t body = pos + 8;
        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint64_t want = len < sizeof(fmt) ? len : sizeof(fmt);
            if (want < 16 || fread(fmt, 1, (size_t)want, file) != (size_t)want) break;
            uint16_t audio_format = (uint16_t)((uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8));
            uint16_t channels = (uint16_t)((uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8));
            rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                   ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits = (uint32_t)fmt[14] | ((uint32_t)fmt[15] << 8);
            if (audio_format != 0x0001 && audio_format != 0xFFFE) {
                snprintf(err, err_len,
                         "WAV fmt is not PCM (format tag 0x%04x) - only PCM WAV is supported",
                         audio_format);
                return false;
            }
            if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
                snprintf(err, err_len,
                         "WAV: unsupported PCM layout (%u-bit, %u ch) - expected 8/16/24/32-bit",
                         bits, channels);
                return false;
            }
            if (out_channels) {
                // Audio-slot caller (the drag-and-drop monitor source): the
                // clockgen PCM1802 capture is 3/4-channel interleaved; the
                // feed takes CH1/CH2 (mono duplicates to both ears).
                if (channels == 0 || channels > 8) {
                    snprintf(err, err_len,
                             "WAV has %u channels - expected 1-8", channels);
                    return false;
                }
            } else if (channels != 1) {
                snprintf(err, err_len,
                         "WAV has %u channels (expected mono - one playback file per RF channel)",
                         channels);
                return false;
            }
            wav_channels = channels;
            have_fmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            uint64_t avail = file_size > body ? file_size - body : 0;
            // Streaming writers store 0 or 0xFFFFFFFF as the data size; clamp
            // the span to EOF in every case.
            uint64_t span = (len == 0 || len == 0xFFFFFFFFu) ? avail : len;
            if (span > avail) span = avail;
            data_offset = body;
            data_bytes = span;
            have_data = true;
            break;
        }
        pos = body + len + (len & 1);  // chunk body + the odd-size pad byte
    }
    if (!have_fmt) {
        snprintf(err, err_len, "WAV: no fmt chunk (not a PCM WAV)");
        return false;
    }
    if (!have_data) {
        snprintf(err, err_len, "WAV: no data chunk");
        return false;
    }
    if (rate == 0 || bits == 0) {
        snprintf(err, err_len, "WAV: zero sample rate or bit depth in fmt chunk");
        return false;
    }
    *out_rate = rate;
    *out_bits = bits;
    if (out_channels) *out_channels = wav_channels;
    *out_data_offset = data_offset;
    *out_data_bytes = data_bytes;
    return true;
}

// Open + fully resolve a raw/WAV/.lds channel file: everything the reader and
// the file info need is derived once here (byte span, exact total, real
// rate). `r` receives the reader state with its FILE* assigned (the caller
// aliases it into ctx->file_a/file_b); `info` receives the resolved metadata
// including a specific error message on failure.
static bool gui_playback_raw_open(playback_raw_reader_t *r, playback_file_info_t *info,
                                  const char *filepath, playback_input_format_t fmt,
                                  char *err, size_t err_len) {
    memset(r, 0, sizeof(*r));
    r->format = fmt;
    if (err && err_len) err[0] = '\0';
    if (!info) {
        snprintf(err, err_len, "internal: no file info");
        return false;
    }

    FILE *file = file_open_read_utf8(filepath);
    if (!file) {
        snprintf(err, err_len, "open failed: %s", filepath);
        goto fail;
    }
    if (PLAYBACK_FSEEK(file, 0, SEEK_END) != 0 ||
        !gui_playback_file_tell(file, &r->file_size) ||
        !gui_playback_file_seek_abs(file, 0)) {
        snprintf(err, err_len, "stat failed: %s", filepath);
        fclose(file);
        goto fail;
    }

    double msps = 0.0;
    bool have_msps = gui_playback_extract_msps_hint(filepath, &msps) && msps > 0.0;

    if (fmt == PLAYBACK_INPUT_WAV) {
        uint32_t rate = 0, bits = 0;
        if (!gui_playback_parse_wav(file, r->file_size, &rate, &bits, NULL,
                                    &r->data_offset, &r->data_bytes, err, err_len)) {
            fclose(file);
            goto fail;
        }
        r->bits = bits;
        r->total_samples = r->data_bytes / (uint64_t)(bits / 8);  // mono: one byte frame per sample
        // WAV rate resolution (FLAC-Chop's rule): a standard audio rate is
        // real audio as-is; a header rate above 1 MHz is a real RF rate (some
        // cxadc/DdD wav writers record the true MHz rate); anything else
        // (e.g. 20000) is the RF /1000 convention.
        if (gui_playback_is_audio_sample_rate(rate) || rate > 1000000u) {
            info->real_sample_rate_hz = (double)rate;
        } else {
            info->real_sample_rate_hz = (double)rate * 1000.0;
            fprintf(stderr,
                    "[PLAYBACK] WAV header rate %u Hz is the RF /1000 convention (%.0f Hz real)\n",
                    rate, info->real_sample_rate_hz);
        }
        info->sample_rate = rate;
        info->bits_per_sample = (uint8_t)bits;
    } else if (fmt == PLAYBACK_INPUT_LDS) {
        // Packed 10-bit DdD RF: the packing is fully defined (4 samples per 5
        // bytes, centre 512), so the totals are exact from the file size. Like
        // raw PCM there is no header, but the format has exactly one rate:
        // every .lds is a 40 MSPS ld-decode capture - used silently, not a
        // guess; an explicit <n>msps hint still wins for renamed files.
        if (!have_msps) msps = PLAYBACK_LDS_DEFAULT_MSPS;
        r->bits = 10;
        r->data_offset = 0;
        r->data_bytes = r->file_size;
        r->total_samples = (r->file_size / PLAYBACK_LDS_GROUP_BYTES) * PLAYBACK_LDS_GROUP_SAMPLES;
        if (r->file_size % PLAYBACK_LDS_GROUP_BYTES) {
            fprintf(stderr,
                    "[PLAYBACK] packed .lds file size is not a multiple of 5: dropping %llu "
                    "trailing byte(s) that don't form a whole 4-sample group\n",
                    (unsigned long long)(r->file_size % PLAYBACK_LDS_GROUP_BYTES));
        }
        info->real_sample_rate_hz = msps * 1000000.0;
        info->sample_rate = (uint32_t)(msps * 1000.0);  // kHz-domain equivalent (40 MSPS -> 40000)
        info->bits_per_sample = 10;                     // the true source depth (packed 4-per-5)
    } else {
        // Headerless raw PCM: width/signedness come from the sniffed
        // extension, the count is exact from the file size, and the REAL rate
        // can only come from an <n>msps filename hint - without it the rate
        // is unknowable, which is a hard error (never guess).
        if (!have_msps) {
            snprintf(err, err_len,
                     "raw %s audio has no header, so the sample rate is unknowable: rename the file "
                     "with the rate (e.g. ..._8-bit_20msps.u8) - the <n>msps hint is required",
                     fmt == PLAYBACK_INPUT_U8 ? "u8" : fmt == PLAYBACK_INPUT_S8 ? "s8"
                     : fmt == PLAYBACK_INPUT_U16 ? "u16" : "s16");
            fclose(file);
            goto fail;
        }
        r->bits = (fmt == PLAYBACK_INPUT_U8 || fmt == PLAYBACK_INPUT_S8) ? 8 : 16;
        r->data_offset = 0;
        r->data_bytes = r->file_size;
        r->total_samples = r->file_size / (uint64_t)(r->bits / 8);
        info->real_sample_rate_hz = msps * 1000000.0;
        info->sample_rate = (uint32_t)(msps * 1000.0);  // kHz-domain equivalent (20 MSPS -> 20000)
        info->bits_per_sample = (uint8_t)r->bits;
    }

    if (r->total_samples == 0) {
        snprintf(err, err_len, "no audio data in %s", filepath);
        fclose(file);
        goto fail;
    }
    if (!gui_playback_file_seek_abs(file, r->data_offset)) {
        snprintf(err, err_len, "seek failed: %s", filepath);
        fclose(file);
        goto fail;
    }
    r->file = file;
    info->format = (int)fmt;
    info->total_samples = r->total_samples;
    info->header_total_samples = r->total_samples;  // no header lies in these formats
    info->duration_seconds = info->real_sample_rate_hz > 0.0
        ? (double)r->total_samples / info->real_sample_rate_hz : 0.0;
    info->valid = true;
    return true;

fail:
    snprintf(info->error, sizeof(info->error), "%s", err ? err : "open failed");
    info->valid = false;
    return false;
}

// Convert up to `want` samples at the reader cursor into the 12-bit display
// container; returns the number produced (0 = end of the audio span). Reads
// stay bounded (never more than the requested samples, plus at most three
// carried .lds group mates from a previous partial group).
static size_t gui_playback_raw_read(playback_ctx_t *ctx, int channel, int16_t *out, size_t want) {
    playback_raw_reader_t *r = &ctx->raw[channel];
    if (!r->file || !out || want == 0 || r->total_samples == 0 ||
        r->cursor_sample >= r->total_samples) {
        return 0;
    }
    uint64_t remaining = r->total_samples - r->cursor_sample;
    size_t chunk = want < remaining ? want : (size_t)remaining;
    size_t produced = 0;
    uint8_t buf[5120];  // 4096 s16 samples per pass; >= one max .lds group batch

    // .lds group mates a previous bounded read left behind come first (they
    // are exactly the samples at the cursor).
    while (r->carry_len > 0 && produced < chunk) {
        out[produced++] = r->carry[0];
        r->carry[0] = r->carry[1];
        r->carry[1] = r->carry[2];
        r->carry_len--;
    }

    switch (r->format) {
        case PLAYBACK_INPUT_U8:
        case PLAYBACK_INPUT_S8: {
            while (produced < chunk) {
                size_t n = chunk - produced;
                if (n > sizeof(buf)) n = sizeof(buf);
                size_t got = fread(buf, 1, n, r->file);
                if (got == 0) break;
                for (size_t i = 0; i < got; i++) {
                    // u8: cxadc unsigned, centre 128; s8: signed. Both are the
                    // 8-bit top of the 12-bit container -> << 4.
                    int32_t v = (r->format == PLAYBACK_INPUT_U8)
                        ? ((int32_t)buf[i] - 128) : (int32_t)(int8_t)buf[i];
                    out[produced++] = gui_playback_scale_to_container(v, 8);
                }
            }
            break;
        }
        case PLAYBACK_INPUT_U16:
        case PLAYBACK_INPUT_S16:
        case PLAYBACK_INPUT_WAV: {
            uint32_t width = (r->format == PLAYBACK_INPUT_WAV) ? (r->bits / 8u) : 2u;
            while (produced < chunk) {
                size_t samples = chunk - produced;
                if (samples > sizeof(buf) / width) samples = sizeof(buf) / width;
                size_t items = fread(buf, width, samples, r->file);
                if (items == 0) break;
                if (r->format == PLAYBACK_INPUT_U16) {
                    // cxadc tenbit: unsigned 10-bit word, centre 512 -> << 2
                    // into the 12-bit container (the exact (value-512)*64 s16
                    // grid >> 4). Values beyond the 10-bit grid clamp.
                    for (size_t i = 0; i < items; i++) {
                        uint32_t w = (uint32_t)buf[2 * i] | ((uint32_t)buf[2 * i + 1] << 8);
                        int32_t v = (int32_t)w - 512;
                        if (v > 511) v = 511;
                        if (v < -512) v = -512;
                        out[produced++] = (int16_t)(v << 2);
                    }
                } else if (r->format == PLAYBACK_INPUT_WAV && r->bits == 8) {
                    // WAV 8-bit PCM is unsigned, centred at 128.
                    for (size_t i = 0; i < items; i++) {
                        out[produced++] =
                            gui_playback_scale_to_container((int32_t)buf[i] - 128, 8);
                    }
                } else if (width == 2) {
                    // 16-bit signed sources (raw s16 / WAV16) share the FLAC
                    // write-callback convention detector: recorder 12-bit
                    // << 4 grid (>> 4 restore) vs direct-domain values (pass
                    // through) vs full-range (>> 4 display downscale).
                    int32_t wide[sizeof(buf) / 2];
                    for (size_t i = 0; i < items; i++) {
                        wide[i] = (int32_t)(int16_t)((uint16_t)buf[2 * i] | ((uint16_t)buf[2 * i + 1] << 8));
                    }
                    gui_playback_decide_conv16(&ctx->conv16_state[channel], wide, (uint32_t)items);
                    for (size_t i = 0; i < items; i++) {
                        out[produced++] =
                            gui_playback_apply_conv16(ctx->conv16_state[channel], wide[i]);
                    }
                } else {
                    // WAV 24/32-bit: take the top 12 bits of the signed value.
                    for (size_t i = 0; i < items; i++) {
                        const uint8_t *b = buf + i * width;
                        uint32_t u;
                        int32_t v;
                        if (r->bits == 24) {
                            u = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
                            // Sign-extend from 24 bits in the unsigned domain.
                            if (u & 0x800000u) u |= 0xFF000000u;
                        } else {
                            u = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                        }
                        // unsigned -> signed without impl-defined conversion:
                        // values below 2^31 map directly; the rest shift down
                        // and add INT32_MIN (both sides stay in range).
                        v = (u & 0x80000000u)
                            ? (int32_t)(u - 0x80000000u) + INT32_MIN
                            : (int32_t)u;
                        out[produced++] = (int16_t)(v >> (r->bits - 12));
                    }
                }
            }
            break;
        }
        case PLAYBACK_INPUT_LDS: {
            while (produced < chunk) {
                // One 5-byte group -> 4 ten-bit words, centre 512, scaled
                // (value - 512) * 4 into the 12-bit container (the exact
                // 16-bit (value-512)*64 grid >> 4 of the reference unpacker).
                uint8_t group[PLAYBACK_LDS_GROUP_BYTES];
                if (fread(group, 1, sizeof(group), r->file) != sizeof(group)) break;
                uint32_t w[PLAYBACK_LDS_GROUP_SAMPLES];
                w[0] = ((uint32_t)group[0] * 4u) + ((uint32_t)(group[1] & 0xC0) >> 6);
                w[1] = ((uint32_t)(group[1] & 0x3F) * 16u) + ((uint32_t)(group[2] & 0xF0) >> 4);
                w[2] = ((uint32_t)(group[2] & 0x0F) * 64u) + ((uint32_t)(group[3] & 0xFC) >> 2);
                w[3] = ((uint32_t)(group[3] & 0x03) * 256u) + (uint32_t)group[4];
                for (uint32_t i = 0; i < PLAYBACK_LDS_GROUP_SAMPLES; i++) {
                    if (r->skip > 0) {
                        // Mid-group seek target: the leading group mates
                        // before the cursor sample are dropped, never counted.
                        r->skip--;
                        continue;
                    }
                    int32_t v = (int32_t)w[i] - 512;
                    if (v > 511) v = 511;
                    if (v < -512) v = -512;
                    int16_t s = (int16_t)(v << 2);
                    if (produced < chunk) {
                        out[produced++] = s;
                    } else {
                        // Bounded read ended inside the group: keep the
                        // unconsumed mates for the next call.
                        r->carry[r->carry_len++] = s;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    r->cursor_sample += produced;
    return produced;
}

// Position a raw channel at an exact sample (whole-sample boundaries for the
// byte formats; group-aligned with a leading skip for packed .lds). The next
// read then starts exactly at `sample` - the raw analogue of the FLAC seek
// attempt's exact-target-slice property.
static bool gui_playback_raw_seek(playback_ctx_t *ctx, int channel, uint64_t sample) {
    playback_raw_reader_t *r = &ctx->raw[channel];
    if (!r->file || r->total_samples == 0) return false;
    if (sample >= r->total_samples) sample = r->total_samples - 1;
    uint64_t byte = r->data_offset;
    if (r->format == PLAYBACK_INPUT_LDS) {
        byte += (sample / PLAYBACK_LDS_GROUP_SAMPLES) * PLAYBACK_LDS_GROUP_BYTES;
        r->skip = (uint32_t)(sample % PLAYBACK_LDS_GROUP_SAMPLES);
    } else {
        byte += sample * (uint64_t)(r->bits / 8);
        r->skip = 0;
    }
    if (!gui_playback_file_seek_abs(r->file, byte)) return false;
    r->cursor_sample = sample;
    r->carry_len = 0;
    return true;
}

static bool gui_playback_raw_eof(const playback_ctx_t *ctx, int channel) {
    const playback_raw_reader_t *r = &ctx->raw[channel];
    return r->total_samples == 0 || r->cursor_sample >= r->total_samples;
}

//-----------------------------------------------------------------------------
// Ogg FLAC (.ldf/.oga/.ogg) demux + remux
//-----------------------------------------------------------------------------

// The vhs-decode / ld-decode `.ldf` RF capture format is Ogg-wrapped FLAC
// (ffmpeg ... -acodec flac -f ogg), not native FLAC. Playback bridges it
// with a full-file remux into a session temp NATIVE FLAC (the FLAC-Chop
// window-remux model, simplified: a whole-file remux starts at frame 0, so
// frame packets are copied verbatim - no renumbering, no CRC recompute):
// the existing native machinery (IO-layer seeks, length resolution,
// paused-analysis decoders) then works on the temp unchanged, while
// playback stays streaming (bounded-memory page/packet buffers) and the
// temp is unlinked on release. The EXACT length comes from the remux's own
// copied-frame sum (cross-checked against the last Ogg page's 64-bit
// granule position, which is correct even when a piped ffmpeg writer left
// STREAMINFO total_samples = 0 - the real-world .ldf case).

#define PLAYBACK_OGG_PAGE_HEADER_BYTES 27u
#define PLAYBACK_OGG_MAX_SEGMENTS 255u
#define PLAYBACK_OGG_MAX_PAGE_BYTES \
    (PLAYBACK_OGG_PAGE_HEADER_BYTES + PLAYBACK_OGG_MAX_SEGMENTS + PLAYBACK_OGG_MAX_SEGMENTS * 255u)
#define PLAYBACK_OGG_NO_GRANULE UINT64_MAX
#define PLAYBACK_OGG_HEADER_PAGE_LIMIT 4096u
#define PLAYBACK_OGG_MAX_COMMENT_BYTES (64u * 1024u)   // larger comments are parsed but not carried into the remux
#define PLAYBACK_OGG_SCAN_CHUNK_BYTES (1u << 20)
// One FLAC frame packet (an Ogg packet): a page payload holds at most 255 x
// 255 bytes, but a frame SPANS pages - the worst case is a 65535-sample
// 32-bit verbatim frame (~262 KB), so packet buffers/limits use this bound,
// not the page bound.
#define PLAYBACK_OGG_MAX_PACKET_BYTES (65535u * 4u + 4096u)

// Ogg page CRC-32 (poly 0x04C11DB7, init 0, MSB-first, no final xor). The
// table is generated on first use; concurrent first uses (an idle validate
// on the render thread racing the background loader) both compute identical
// values into the static table, and the ready flag is an atomic, so the
// result is published only after the full table is in place.
static uint32_t s_ogg_crc_table[256];
static atomic_int s_ogg_crc_table_ready;

static uint32_t gui_playback_ogg_crc(uint32_t crc, const uint8_t *data, size_t len) {
    if (!atomic_load(&s_ogg_crc_table_ready)) {
        uint32_t table[256];
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) {
                r = (r & 0x80000000u) ? ((r << 1) ^ 0x04C11DB7u) : (r << 1);
            }
            table[i] = r;
        }
        memcpy(s_ogg_crc_table, table, sizeof(table));
        atomic_store(&s_ogg_crc_table_ready, 1);
    }
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ s_ogg_crc_table[((crc >> 24) ^ data[i]) & 0xFFu];
    }
    return crc;
}

typedef enum {
    PLAYBACK_OGG_PAGE_OK = 0,
    PLAYBACK_OGG_PAGE_EOF,        // clean end of file exactly at a page boundary
    PLAYBACK_OGG_PAGE_TRUNCATED,  // file ends inside a page (interrupted capture)
    PLAYBACK_OGG_PAGE_BAD,        // not a valid page (no capture pattern, bad version, CRC mismatch)
} playback_ogg_page_status_t;

typedef struct {
    FILE *file;
    uint64_t file_size;
    uint64_t offset;              // file offset of the page most recently read
    uint64_t page_len;            // total bytes of that page (header + lacing + payload)
    uint8_t header_type;
    uint64_t granule;             // PLAYBACK_OGG_NO_GRANULE when no packet completes here
    uint32_t serial;
    uint32_t nseg;                // lacing value count
    uint8_t *buf;                 // heap page buffer (max page size)
    const uint8_t *segments;      // nseg lacing values (inside buf)
    const uint8_t *payload;       // payload bytes (inside buf)
    size_t payload_len;
} playback_ogg_page_t;

static size_t gui_playback_read_full(FILE *file, void *dst, size_t len) {
    uint8_t *p = (uint8_t *)dst;
    size_t got = 0;
    while (got < len) {
        size_t n = fread(p + got, 1, len - got, file);
        if (n == 0) break;
        got += n;
    }
    return got;
}

// Read + CRC-verify one Ogg page at the file's current position.
static playback_ogg_page_status_t gui_playback_ogg_read_page(playback_ogg_page_t *pg) {
    uint64_t at = 0;
    if (!gui_playback_file_tell(pg->file, &at)) return PLAYBACK_OGG_PAGE_BAD;
    if (at >= pg->file_size) return PLAYBACK_OGG_PAGE_EOF;

    uint8_t header[PLAYBACK_OGG_PAGE_HEADER_BYTES];
    if (gui_playback_read_full(pg->file, header, sizeof(header)) != sizeof(header)) {
        return PLAYBACK_OGG_PAGE_TRUNCATED;
    }
    if (memcmp(header, "OggS", 4) != 0 || header[4] != 0) return PLAYBACK_OGG_PAGE_BAD;

    // The lacing table follows the 27-byte header (it is NOT part of it): read
    // it into the page buffer, derive the payload length from its values.
    uint32_t nseg = header[26];
    if ((uint64_t)PLAYBACK_OGG_PAGE_HEADER_BYTES + nseg > PLAYBACK_OGG_MAX_PAGE_BYTES) {
        return PLAYBACK_OGG_PAGE_BAD;
    }
    memcpy(pg->buf, header, sizeof(header));
    if (nseg > 0 &&
        gui_playback_read_full(pg->file, pg->buf + PLAYBACK_OGG_PAGE_HEADER_BYTES, nseg) != nseg) {
        return PLAYBACK_OGG_PAGE_TRUNCATED;
    }
    uint64_t payload_len = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        payload_len += pg->buf[PLAYBACK_OGG_PAGE_HEADER_BYTES + i];
    }
    uint64_t total = (uint64_t)PLAYBACK_OGG_PAGE_HEADER_BYTES + nseg + payload_len;
    if (total > PLAYBACK_OGG_MAX_PAGE_BYTES) return PLAYBACK_OGG_PAGE_BAD;
    if (at + total > pg->file_size) return PLAYBACK_OGG_PAGE_TRUNCATED;
    if (payload_len > 0 &&
        gui_playback_read_full(pg->file, pg->buf + PLAYBACK_OGG_PAGE_HEADER_BYTES + nseg,
                               (size_t)payload_len) != (size_t)payload_len) {
        return PLAYBACK_OGG_PAGE_TRUNCATED;
    }

    // Verify the page CRC (computed over the page with the CRC field zeroed).
    uint32_t stored = (uint32_t)header[22] | ((uint32_t)header[23] << 8) |
                      ((uint32_t)header[24] << 16) | ((uint32_t)header[25] << 24);
    memset(pg->buf + 22, 0, 4);
    if (gui_playback_ogg_crc(0, pg->buf, (size_t)total) != stored) {
        return PLAYBACK_OGG_PAGE_BAD;
    }

    pg->offset = at;
    pg->page_len = total;
    pg->header_type = header[5];
    pg->granule = 0;
    for (int k = 7; k >= 0; k--) {
        pg->granule = (pg->granule << 8) | (uint64_t)header[6 + k];
    }
    pg->serial = (uint32_t)header[14] | ((uint32_t)header[15] << 8) |
                 ((uint32_t)header[16] << 16) | ((uint32_t)header[17] << 24);
    pg->nseg = nseg;
    pg->segments = pg->buf + PLAYBACK_OGG_PAGE_HEADER_BYTES;
    pg->payload = pg->segments + nseg;
    pg->payload_len = (size_t)payload_len;
    return PLAYBACK_OGG_PAGE_OK;
}

// STREAMINFO payload offsets (the packed 64-bit rate/ch/bps/total word sits
// at payload bytes 10..17; the 36-bit total is its low bits).
#define PLAYBACK_OGG_SI_PACKED_OFF 10u
#define PLAYBACK_OGG_SI_TOTAL_MASK 0xFFFFFFFFFULL

// Parsed Ogg FLAC headers: the FLAC logical stream's serial, its metadata
// blocks (STREAMINFO + the first VORBIS_COMMENT), and where audio starts.
typedef struct {
    uint32_t serial;
    uint64_t audio_offset;         // file offset of the first audio page
    bool have_si;
    uint8_t streaminfo[34];
    bool has_vorbis_comment;
    uint8_t *vorbis_comment;        // heap copy (<= PLAYBACK_OGG_MAX_COMMENT_BYTES)
    size_t vorbis_comment_len;
    playback_vorbis_rf_tags_t tags; // RF tags parsed from that comment block
    bool has_tags;
} playback_ogg_header_t;

static void gui_playback_ogg_header_cleanup(playback_ogg_header_t *h) {
    if (!h) return;
    free(h->vorbis_comment);
    memset(h, 0, sizeof(*h));
}

// Parse the RF tag triple from a VORBIS_COMMENT block payload (little-endian
// lengths: vendor, count, entries). The same key matching as
// gui_playback_read_vorbis_rf_tags, from the raw bytes the Ogg header carries.
static bool gui_playback_parse_vorbis_rf_tags_payload(const uint8_t *p, size_t len,
                                                       playback_vorbis_rf_tags_t *out) {
    memset(out, 0, sizeof(*out));
    if (!p || !out || len < 8) return false;
    uint32_t vendor_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    size_t pos = 4;
    if ((uint64_t)vendor_len > len || pos + (uint64_t)vendor_len + 4 > len) return false;
    pos += vendor_len;
    uint32_t count = (uint32_t)p[pos] | ((uint32_t)p[pos + 1] << 8) |
                     ((uint32_t)p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
    pos += 4;
    for (uint32_t i = 0; i < count && pos + 4 <= len; i++) {
        uint32_t clen = (uint32_t)p[pos] | ((uint32_t)p[pos + 1] << 8) |
                        ((uint32_t)p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
        pos += 4;
        if (clen > len - pos) return false;  // malformed: keep what was parsed
        const uint8_t *entry = p + pos;
        uint32_t split = 0;
        while (split < clen && entry[split] != '=') split++;
        if (split > 0 && split < clen) {
            const uint8_t *key = entry;
            size_t key_len = split;
            const uint8_t *value = entry + split + 1;
            size_t value_len = (size_t)clen - (size_t)split - 1u;
            if (!out->has_rf_total_samples &&
                gui_playback_eq_ascii_ci_bytes(key, key_len, "RF_TOTAL_SAMPLES")) {
                uint64_t parsed = 0;
                if (gui_playback_parse_u64_bytes(value, value_len, &parsed) && parsed > 0) {
                    out->has_rf_total_samples = true;
                    out->rf_total_samples = parsed;
                }
            } else if (!out->has_rf_sample_rate &&
                       gui_playback_eq_ascii_ci_bytes(key, key_len, "RF_SAMPLE_RATE")) {
                uint64_t parsed = 0;
                if (gui_playback_parse_u64_bytes(value, value_len, &parsed) && parsed > 0) {
                    out->has_rf_sample_rate = true;
                    out->rf_sample_rate = parsed;
                }
            } else if (!out->has_duration_seconds &&
                       gui_playback_eq_ascii_ci_bytes(key, key_len, "DURATION_SECONDS")) {
                double parsed = 0.0;
                if (gui_playback_parse_f64_bytes(value, value_len, &parsed) && parsed > 0.0) {
                    out->has_duration_seconds = true;
                    out->duration_seconds = parsed;
                }
            }
        }
        pos += clen;
    }
    return out->has_rf_total_samples || out->has_rf_sample_rate || out->has_duration_seconds;
}

// Collect one metadata block from the mapping header / header packets.
// Only STREAMINFO and VORBIS_COMMENT are kept: a SEEKTABLE's byte offsets
// reference the native FLAC stream domain and would actively mis-seek the
// remux; padding is useless. Returns false only on structural errors.
static bool gui_playback_ogg_collect_block(playback_ogg_header_t *h, uint8_t type,
                                            const uint8_t *payload, size_t len,
                                            char *err, size_t err_len) {
    if (type == 0) {
        if (len != 34) {
            snprintf(err, err_len, "Ogg FLAC: STREAMINFO block is not 34 bytes");
            return false;
        }
        memcpy(h->streaminfo, payload, 34);
        h->have_si = true;
        return true;
    }
    if (type == 4) {
        if (h->has_vorbis_comment) return true;  // the first comment block wins
        if (len > PLAYBACK_OGG_MAX_COMMENT_BYTES) {
            fprintf(stderr,
                    "[PLAYBACK] Ogg FLAC: VORBIS_COMMENT larger than %u bytes; not carried into the remux\n",
                    PLAYBACK_OGG_MAX_COMMENT_BYTES);
            h->has_tags = gui_playback_parse_vorbis_rf_tags_payload(payload, len, &h->tags);
            return true;
        }
        free(h->vorbis_comment);
        h->vorbis_comment = (uint8_t *)malloc(len);
        if (!h->vorbis_comment) {
            snprintf(err, err_len, "out of memory reading Ogg FLAC metadata");
            return false;
        }
        memcpy(h->vorbis_comment, payload, len);
        h->vorbis_comment_len = len;
        h->has_vorbis_comment = true;
        h->has_tags = gui_playback_parse_vorbis_rf_tags_payload(payload, len, &h->tags);
        return true;
    }
    return true;
}

// Parse the Ogg FLAC mapping header: pages from offset 0 through the last
// metadata-block flag. Mirrors the FLAC-Chop demuxer's layout rules (BOS
// first page, 0x7F "FLAC" mapping packet, one metadata block per further
// header packet, foreign logical streams skipped by serial).
static bool gui_playback_ogg_read_header(const char *filepath, playback_ogg_header_t *h,
                                         char *err, size_t err_len) {
    memset(h, 0, sizeof(*h));
    if (err && err_len) err[0] = '\0';
    FILE *file = file_open_read_utf8(filepath);
    if (!file) {
        snprintf(err, err_len, "open failed: %s", filepath);
        return false;
    }
    uint64_t file_size = 0;
    playback_ogg_page_t pg = {.file = file};
    uint8_t *carry = NULL;
    bool ok = false;

    do {
        if (PLAYBACK_FSEEK(file, 0, SEEK_END) != 0 ||
            !gui_playback_file_tell(file, &file_size) ||
            !gui_playback_file_seek_abs(file, 0)) {
            snprintf(err, err_len, "stat failed: %s", filepath);
            break;
        }
        pg.file_size = file_size;
        pg.buf = (uint8_t *)malloc(PLAYBACK_OGG_MAX_PAGE_BYTES);
        carry = (uint8_t *)malloc(PLAYBACK_OGG_MAX_PACKET_BYTES);  // header packets, but sized for any packet
        if (!pg.buf || !carry) {
            snprintf(err, err_len, "out of memory reading the Ogg FLAC header");
            break;
        }

        bool have_serial = false;
        bool done = false;
        uint32_t packets_seen = 0;
        size_t carry_len = 0;
        for (uint32_t page_count = 0;
             page_count < PLAYBACK_OGG_HEADER_PAGE_LIMIT && !done;
             page_count++) {
            playback_ogg_page_status_t st = gui_playback_ogg_read_page(&pg);
            if (st == PLAYBACK_OGG_PAGE_EOF) {
                snprintf(err, err_len, "Ogg FLAC header: unexpected end of file");
                goto finish;
            }
            if (st == PLAYBACK_OGG_PAGE_TRUNCATED) {
                snprintf(err, err_len, "Ogg FLAC header: file ends inside an Ogg page");
                goto finish;
            }
            if (st == PLAYBACK_OGG_PAGE_BAD) {
                snprintf(err, err_len,
                         "Ogg FLAC header: damaged page at offset %llu (no capture pattern / bad version / CRC mismatch)",
                         (unsigned long long)pg.offset);
                goto finish;
            }
            if (!have_serial) {
                if (!(pg.header_type & 0x02)) {
                    snprintf(err, err_len,
                             "not an Ogg FLAC stream: the first Ogg page is not a BOS page");
                    goto finish;
                }
                h->serial = pg.serial;
                have_serial = true;
            } else if (pg.serial != h->serial) {
                continue;  // a multiplexed foreign logical stream: skip its pages
            }

            // Split the page payload into packets using the lacing values: a
            // value < 255 completes a packet. A page that does not continue an
            // unfinished packet means the stream was damaged - drop the partial.
            if (!(pg.header_type & 0x01) && carry_len != 0) carry_len = 0;
            size_t pos = 0;
            for (uint32_t s = 0; s < pg.nseg; s++) {
                size_t seg = pg.segments[s];
                if (carry_len + seg > PLAYBACK_OGG_MAX_PACKET_BYTES) {
                    snprintf(err, err_len, "Ogg FLAC: header packet too large");
                    goto finish;
                }
                memcpy(carry + carry_len, pg.payload + pos, seg);
                carry_len += seg;
                pos += seg;
                if (pg.segments[s] == 255) continue;  // packet continues on the next page

                if (done) {
                    snprintf(err, err_len,
                             "Ogg FLAC audio data shares a page with the header packets (unsupported layout)");
                    goto finish;
                }
                if (packets_seen == 0) {
                    // Mapping header: 0x7F "FLAC" major minor nheaders(BE16)
                    // "fLaC", then the first metadata block (STREAMINFO).
                    if (carry_len < 13 + 4 + 34 || carry[0] != 0x7F ||
                        memcmp(carry + 1, "FLAC", 4) != 0 || memcmp(carry + 9, "fLaC", 4) != 0) {
                        snprintf(err, err_len, "not an Ogg FLAC stream (bad mapping header)");
                        goto finish;
                    }
                    size_t p = 13;
                    bool first_block = true;
                    while (p + 4 <= carry_len) {
                        uint8_t type = carry[p] & 0x7F;
                        bool last = (carry[p] & 0x80) != 0;
                        size_t len = ((size_t)carry[p + 1] << 16) | ((size_t)carry[p + 2] << 8) |
                                     (size_t)carry[p + 3];
                        if (p + 4 + len > carry_len) {
                            snprintf(err, err_len,
                                     "Ogg FLAC mapping header: metadata block overruns the packet");
                            goto finish;
                        }
                        if (first_block && type != 0) {
                            snprintf(err, err_len,
                                     "Ogg FLAC: first metadata block is not a STREAMINFO");
                            goto finish;
                        }
                        first_block = false;
                        if (!gui_playback_ogg_collect_block(h, type, carry + p + 4, len, err, err_len)) {
                            goto finish;
                        }
                        p += 4 + len;
                        if (last) done = true;
                        if (p >= carry_len || last) break;
                    }
                } else {
                    // Subsequent header packets: one metadata block each.
                    if (carry_len < 4) {
                        snprintf(err, err_len, "Ogg FLAC: truncated metadata packet");
                        goto finish;
                    }
                    uint8_t type = carry[0] & 0x7F;
                    bool last = (carry[0] & 0x80) != 0;
                    size_t len = ((size_t)carry[1] << 16) | ((size_t)carry[2] << 8) | (size_t)carry[3];
                    if (4 + len != carry_len) {
                        snprintf(err, err_len,
                                 "Ogg FLAC: metadata packet length mismatch (header says %llu, packet holds %llu)",
                                 (unsigned long long)len, (unsigned long long)(carry_len - 4));
                        goto finish;
                    }
                    if (!gui_playback_ogg_collect_block(h, type, carry + 4, len, err, err_len)) {
                        goto finish;
                    }
                    if (last) done = true;
                }
                packets_seen++;
                carry_len = 0;
            }
        }
        if (!done) {
            snprintf(err, err_len,
                     "Ogg FLAC header is incomplete (no last-metadata-block flag found)");
            break;
        }
        if (carry_len != 0) {
            snprintf(err, err_len,
                     "Ogg FLAC audio data shares a page with the header packets (unsupported layout)");
            break;
        }
        h->audio_offset = pg.offset + pg.page_len;
        ok = true;
    } while (0);

finish:
    free(carry);
    free(pg.buf);
    fclose(file);
    if (!ok) {
        gui_playback_ogg_header_cleanup(h);
    }
    return ok;
}

typedef enum {
    PLAYBACK_OGG_BUF_PAGE_VALID,
    PLAYBACK_OGG_BUF_PAGE_INVALID,
    PLAYBACK_OGG_BUF_PAGE_INCOMPLETE,  // structurally sound; continues past the buffered range
} playback_ogg_buf_page_status_t;

// Validate one candidate page fully inside a byte buffer (used by the
// streaming tail scan; the page bytes need not be page-aligned in the file).
static playback_ogg_buf_page_status_t gui_playback_ogg_parse_page_in_buffer(
    const uint8_t *p, size_t avail, size_t *out_page_len, uint64_t *out_granule,
    uint32_t *out_serial) {
    if (avail < PLAYBACK_OGG_PAGE_HEADER_BYTES) return PLAYBACK_OGG_BUF_PAGE_INCOMPLETE;
    if (memcmp(p, "OggS", 4) != 0 || p[4] != 0) return PLAYBACK_OGG_BUF_PAGE_INVALID;
    uint32_t nseg = p[26];
    size_t seg_end = PLAYBACK_OGG_PAGE_HEADER_BYTES + (size_t)nseg;
    if (seg_end > avail) return PLAYBACK_OGG_BUF_PAGE_INCOMPLETE;
    uint64_t payload_len = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        payload_len += p[PLAYBACK_OGG_PAGE_HEADER_BYTES + i];
    }
    uint64_t total = (uint64_t)seg_end + payload_len;
    if (total > PLAYBACK_OGG_MAX_PAGE_BYTES) return PLAYBACK_OGG_BUF_PAGE_INVALID;
    if (total > avail) return PLAYBACK_OGG_BUF_PAGE_INCOMPLETE;

    // Verify the CRC over the in-buffer page with the field zeroed.
    uint32_t stored = (uint32_t)p[22] | ((uint32_t)p[23] << 8) |
                      ((uint32_t)p[24] << 16) | ((uint32_t)p[25] << 24);
    uint32_t crc = 0;
    crc = gui_playback_ogg_crc(crc, p, 22);
    static const uint8_t zeros[4] = {0};
    crc = gui_playback_ogg_crc(crc, zeros, 4);
    crc = gui_playback_ogg_crc(crc, p + 26, total - 26);
    if (crc != stored) return PLAYBACK_OGG_BUF_PAGE_INVALID;

    *out_page_len = (size_t)total;
    *out_granule = 0;
    for (int k = 7; k >= 0; k--) {
        *out_granule = (*out_granule << 8) | (uint64_t)p[6 + k];
    }
    *out_serial = (uint32_t)p[14] | ((uint32_t)p[15] << 8) |
                  ((uint32_t)p[16] << 16) | ((uint32_t)p[17] << 24);
    return PLAYBACK_OGG_BUF_PAGE_VALID;
}

// Byte-scan [start, end) for CRC-valid pages of `serial` with a granule,
// reading in bounded chunks (the FLAC-Chop windowed scan kept whole windows
// in memory; this form never holds more than a chunk + one page). Returns
// true with the LAST such page's granule (the exact sample count of the
// audio physically present - a truncated tail still yields the length of
// the complete pages before it).
static bool gui_playback_ogg_scan_window(const char *filepath, uint64_t start, uint64_t end,
                                         uint32_t serial, uint64_t *out_granule) {
    *out_granule = 0;
    if (start >= end) return false;
    FILE *file = file_open_read_utf8(filepath);
    if (!file) return false;
    size_t const cap = PLAYBACK_OGG_SCAN_CHUNK_BYTES + PLAYBACK_OGG_MAX_PAGE_BYTES;
    uint8_t *buf = (uint8_t *)malloc(cap);
    uint64_t best = 0;
    bool found = false;
    if (buf && gui_playback_file_seek_abs(file, start)) {
        uint64_t base = start;  // file offset of buf[0]
        size_t fill = 0;
        while (base + fill < end) {
            uint64_t left = end - (base + fill);
            size_t want = left < (uint64_t)(cap - fill) ? (size_t)left : (cap - fill);
            size_t got = fread(buf + fill, 1, want, file);
            if (got == 0) break;
            fill += got;

            size_t i = 0;
            while (i + PLAYBACK_OGG_PAGE_HEADER_BYTES <= fill) {
                size_t page_len = 0;
                uint64_t granule = 0;
                uint32_t page_serial = 0;
                playback_ogg_buf_page_status_t st = gui_playback_ogg_parse_page_in_buffer(
                    buf + i, fill - i, &page_len, &granule, &page_serial);
                if (st == PLAYBACK_OGG_BUF_PAGE_INCOMPLETE) break;  // keep as the carried tail
                if (st == PLAYBACK_OGG_BUF_PAGE_VALID && base + i + page_len <= end) {
                    if (page_serial == serial && granule != PLAYBACK_OGG_NO_GRANULE) {
                        best = granule;
                        found = true;
                    }
                    i += page_len;
                } else {
                    i++;
                }
            }
            if (i < fill) {
                memmove(buf, buf + i, fill - i);
                fill -= i;
            } else {
                fill = 0;
            }
            base += i;
        }
    }
    free(buf);
    fclose(file);
    if (found) *out_granule = best;
    return found;
}

// Exact total sample count: the granule of the last CRC-valid FLAC-serial
// page that carries one. Tail windows grow (the last page of a healthy
// capture is in the first window); the final fallback scans the whole audio
// span (full I/O, still bounded memory) for damaged captures.
static bool gui_playback_ogg_last_granule(const char *filepath, const playback_ogg_header_t *h,
                                         uint64_t file_size, uint64_t *out_granule) {
    *out_granule = 0;
    if (file_size <= h->audio_offset) return false;
    static const uint64_t windows[3] = {
        (uint64_t)256 << 10, (uint64_t)4 << 20, (uint64_t)64 << 20
    };
    for (int w = 0; w < 3; w++) {
        uint64_t start = file_size > windows[w] ? file_size - windows[w] : 0;
        if (start < h->audio_offset) break;
        if (gui_playback_ogg_scan_window(filepath, start, file_size, h->serial, out_granule)) {
            return true;
        }
    }
    return gui_playback_ogg_scan_window(filepath, h->audio_offset, file_size, h->serial,
                                        out_granule);
}

static bool gui_playback_stat_file_size(const char *filepath, uint64_t *out_size) {
    if (!filepath || !out_size) return false;
    FILE *file = file_open_read_utf8(filepath);
    if (!file) return false;
    bool ok = PLAYBACK_FSEEK(file, 0, SEEK_END) == 0 && gui_playback_file_tell(file, out_size);
    fclose(file);
    return ok;
}

// Remux the whole Ogg FLAC file into a standalone native FLAC at `dst`:
// "fLaC" + the source STREAMINFO (total field patched from the copied frame
// sum once known) + the VORBIS_COMMENT block, then every frame packet copied
// verbatim - Ogg packets ARE FLAC frames and a whole-file remux starts at
// frame 0, so no renumbering or CRC recompute is needed. Each packet's frame
// header is validated (sync + CRC-8) like the FLAC-Chop demuxer, and the
// block sizes sum to the exact sample count that is patched into the
// header. The remux is streaming (one page buffer + one packet buffer).
static bool gui_playback_ogg_remux(const char *src, uint64_t file_size,
                                   const playback_ogg_header_t *h, const char *dst,
                                   const atomic_bool *cancel, uint64_t *out_total_samples,
                                   char *err, size_t err_len) {
    *out_total_samples = 0;
    if (err && err_len) err[0] = '\0';
    FILE *in = file_open_read_utf8(src);
    FILE *out = NULL;
    playback_ogg_page_t pg = {.file = in};
    uint8_t *packet = NULL;
    bool ok = false;

    do {
        if (!in) {
            snprintf(err, err_len, "open failed: %s", src);
            break;
        }
        pg.file_size = file_size;
        pg.buf = (uint8_t *)malloc(PLAYBACK_OGG_MAX_PAGE_BYTES);
        packet = (uint8_t *)malloc(PLAYBACK_OGG_MAX_PACKET_BYTES);
        if (!pg.buf || !packet) {
            snprintf(err, err_len, "out of memory for the Ogg FLAC remux");
            break;
        }
        if (!gui_playback_file_seek_abs(in, h->audio_offset)) {
            snprintf(err, err_len, "seek failed: %s", src);
            break;
        }
        // "w+b": the STREAMINFO total is patched in place afterwards, which
        // needs read access (seek between the write and the read flushes).
        out = file_open_utf8(dst, "w+b");
        if (!out) {
            snprintf(err, err_len, "cannot create %s", dst);
            break;
        }

        // Native header: fLaC + STREAMINFO (+ VORBIS_COMMENT). The total
        // field is zeroed now and patched from the frame sum at the end.
        uint8_t si[34];
        memcpy(si, h->streaminfo, sizeof(si));
        uint64_t packed = 0;
        for (int k = 0; k < 8; k++) {
            packed = (packed << 8) | si[PLAYBACK_OGG_SI_PACKED_OFF + k];
        }
        packed &= ~PLAYBACK_OGG_SI_TOTAL_MASK;  // total = 0 (unknown) for now
        for (int k = 0; k < 8; k++) {
            si[PLAYBACK_OGG_SI_PACKED_OFF + k] = (uint8_t)(packed >> (56 - 8 * k));
        }
        bool last_block = !h->has_vorbis_comment;
        uint8_t block_hdr[4] = {(uint8_t)(last_block ? 0x80u : 0x00u), 0, 0, 34};
        if (fwrite("fLaC", 1, 4, out) != 4 || fwrite(block_hdr, 1, 4, out) != 4 ||
            fwrite(si, 1, sizeof(si), out) != sizeof(si)) {
            snprintf(err, err_len, "write failed: %s", dst);
            break;
        }
        if (h->has_vorbis_comment) {
            uint8_t vc_hdr[4] = {0x84, (uint8_t)(h->vorbis_comment_len >> 16),
                                  (uint8_t)(h->vorbis_comment_len >> 8),
                                  (uint8_t)h->vorbis_comment_len};
            if (fwrite(vc_hdr, 1, 4, out) != 4 ||
                fwrite(h->vorbis_comment, 1, h->vorbis_comment_len, out) != h->vorbis_comment_len) {
                snprintf(err, err_len, "write failed: %s", dst);
                break;
            }
        }

        // Frames: copy packets verbatim, validating each frame header.
        uint64_t sum = 0;
        uint64_t frames = 0;
        size_t carry_len = 0;
        while (true) {
            if (cancel && atomic_load(cancel)) {
                snprintf(err, err_len, "cancelled");
                goto remux_finish;
            }
            playback_ogg_page_status_t st = gui_playback_ogg_read_page(&pg);
            if (st == PLAYBACK_OGG_PAGE_EOF) break;
            if (st == PLAYBACK_OGG_PAGE_TRUNCATED || st == PLAYBACK_OGG_PAGE_BAD) {
                // Stop at the damage: the pages before it are intact and form
                // a complete FLAC prefix; the total reflects exactly what the
                // temp holds (the honest playable length).
                fprintf(stderr,
                        "[PLAYBACK] Ogg FLAC remux stopped at %s page at offset %llu; "
                        "keeping the intact prefix\n",
                        st == PLAYBACK_OGG_PAGE_TRUNCATED ? "a truncated" : "a damaged",
                        (unsigned long long)pg.offset);
                break;
            }
            if (pg.serial != h->serial) continue;  // foreign logical stream
            if (!(pg.header_type & 0x01) && carry_len != 0) carry_len = 0;
            size_t pos = 0;
            for (uint32_t s = 0; s < pg.nseg; s++) {
                size_t seg = pg.segments[s];
                if (carry_len + seg > PLAYBACK_OGG_MAX_PACKET_BYTES) {
                    snprintf(err, err_len, "Ogg FLAC: frame packet too large");
                    goto remux_finish;
                }
                memcpy(packet + carry_len, pg.payload + pos, seg);
                carry_len += seg;
                pos += seg;
                if (pg.segments[s] == 255) continue;

                uint16_t block_size = 0;
                size_t header_len = 0;
                uint64_t number = 0;
                bool variable = false;
                if (!gui_playback_parse_frame_header(packet, carry_len, &block_size,
                                                     &header_len, &number, &variable)) {
                    snprintf(err, err_len,
                             "Ogg packet at offset %llu is not a valid FLAC frame (bad sync / header CRC-8)",
                             (unsigned long long)pg.offset);
                    goto remux_finish;
                }
                if (frames == 0 && number != 0) {
                    // Whole-file remux assumes stream-position frame numbers;
                    // a mid-stream starting file still plays sequentially.
                    fprintf(stderr,
                            "[PLAYBACK] Ogg FLAC: first frame number is %llu (not 0); "
                            "seeking may be offset on this capture\n",
                            (unsigned long long)number);
                }
                if (fwrite(packet, 1, carry_len, out) != carry_len) {
                    snprintf(err, err_len, "write failed: %s", dst);
                    goto remux_finish;
                }
                sum += block_size;
                frames++;
                carry_len = 0;
            }
        }
        if (frames == 0) {
            snprintf(err, err_len, "Ogg FLAC stream holds no audio frames");
            break;
        }
        if (fflush(out) != 0) {
            snprintf(err, err_len, "write failed: %s", dst);
            break;
        }
        // Patch the STREAMINFO total (36-bit field; 0 = unknown when the sum
        // cannot fit - the file info keeps the exact 64-bit sum).
        uint64_t field = sum < PLAYBACK_IO_SI_TOTAL_MOD ? sum : 0;
        uint8_t word[8];
        if (!gui_playback_file_seek_abs(out, PLAYBACK_IO_SI_TOTAL_OFF) ||
            fread(word, 1, 8, out) != 8) {
            snprintf(err, err_len, "cannot patch the remux STREAMINFO total");
            break;
        }
        packed = 0;
        for (int k = 0; k < 8; k++) {
            packed = (packed << 8) | word[k];
        }
        packed = (packed & ~PLAYBACK_OGG_SI_TOTAL_MASK) | (field & PLAYBACK_OGG_SI_TOTAL_MASK);
        for (int k = 0; k < 8; k++) {
            word[k] = (uint8_t)(packed >> (56 - 8 * k));
        }
        if (!gui_playback_file_seek_abs(out, PLAYBACK_IO_SI_TOTAL_OFF) ||
            fwrite(word, 1, 8, out) != 8 || fflush(out) != 0) {
            snprintf(err, err_len, "cannot patch the remux STREAMINFO total");
            break;
        }
        *out_total_samples = sum;
        ok = true;
    } while (0);

remux_finish:
    if (out) fclose(out);
    free(packet);
    free(pg.buf);
    if (in) fclose(in);
    return ok;
}

static const char *gui_playback_input_format_name(playback_input_format_t fmt) {
    switch (fmt) {
        case PLAYBACK_INPUT_FLAC: return "flac";
        case PLAYBACK_INPUT_OGG_FLAC: return "ogg-flac";
        case PLAYBACK_INPUT_WAV: return "wav";
        case PLAYBACK_INPUT_U8: return "u8";
        case PLAYBACK_INPUT_S8: return "s8";
        case PLAYBACK_INPUT_U16: return "u16";
        case PLAYBACK_INPUT_S16: return "s16";
        case PLAYBACK_INPUT_LDS: return "lds";
        default: return "?";
    }
}

// Probe an Ogg FLAC file for validation (no remux, no whole-file work): the
// mapping header gives rate/bits/channels and the RF tags, and the last
// CRC-valid page's granule position gives the EXACT 64-bit sample count -
// correct even when a piped ffmpeg writer left STREAMINFO total_samples = 0
// (the real-world .ldf case). Cheap: header pages plus a bounded tail scan.
static bool gui_playback_ogg_probe(playback_file_info_t *info, const char *filepath,
                                   char *err, size_t err_len) {
    if (!info) return false;
    playback_ogg_header_t h;
    if (!gui_playback_ogg_read_header(filepath, &h, err, err_len)) {
        snprintf(info->error, sizeof(info->error), "%s", err ? err : "unreadable Ogg FLAC");
        return false;
    }
    // STREAMINFO payload: min/max block (4) + min/max frame (6) + the packed
    // 64-bit word (rate 20 bits, channels-1 3, bps-1 5, total 36) + MD5 (16).
    uint64_t packed = 0;
    for (int k = 0; k < 8; k++) {
        packed = (packed << 8) | h.streaminfo[PLAYBACK_OGG_SI_PACKED_OFF + k];
    }
    uint32_t rate = (uint32_t)(packed >> 44);
    uint32_t channels = (uint32_t)((packed >> 41) & 0x7u) + 1;
    uint32_t bits = (uint32_t)((packed >> 36) & 0x1Fu) + 1;
    uint64_t declared = packed & PLAYBACK_OGG_SI_TOTAL_MASK;

    uint64_t file_size = 0;
    uint64_t granule = 0;
    bool have_granule = gui_playback_stat_file_size(filepath, &file_size) &&
                        gui_playback_ogg_last_granule(filepath, &h, file_size, &granule);

    info->format = PLAYBACK_INPUT_OGG_FLAC;
    info->sample_rate = rate;
    info->bits_per_sample = (uint8_t)bits;
    info->header_total_samples = declared;  // the raw source STREAMINFO total
    info->total_samples = have_granule ? granule : declared;
    info->real_sample_rate_hz =
        gui_playback_resolve_real_rate_hz(filepath, rate, h.has_tags ? &h.tags : NULL);
    info->duration_seconds = info->real_sample_rate_hz > 0.0 && info->total_samples > 0
        ? (double)info->total_samples / info->real_sample_rate_hz : 0.0;
    if (channels != 1) {
        snprintf(info->error, sizeof(info->error),
                 "Ogg FLAC has %u channels (expected mono - one playback file per RF channel)",
                 channels);
        info->valid = false;
    } else {
        info->valid = true;
    }
    gui_playback_ogg_header_cleanup(&h);
    return true;  // the info carries the verdict (mono check) like the FLAC probe
}

// Build the session temp path for an Ogg-FLAC remux (per process + channel;
// a previous session's temp was unlinked on release, so no collision).
static bool gui_playback_temp_path(char *out, size_t out_len, int channel) {
#if defined(_WIN32) || defined(_WIN64)
    /* The temp path must be UTF-8 like every other path in the file pipeline
     * (the remux temp is created and removed through the UTF-8 file helpers),
     * so a non-ASCII TEMP directory has to round-trip via the wide API. */
    wchar_t wide_dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(sizeof(wide_dir) / sizeof(*wide_dir), wide_dir);
    if (n == 0 || n >= sizeof(wide_dir) / sizeof(*wide_dir)) return false;
    char dir[MAX_PATH * 3 + 1];
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_dir, -1,
                                    dir, sizeof(dir), NULL, NULL);
    if (bytes <= 0) return false;
    dir[bytes - 1] = '\0';  /* drop the converted terminator for concatenation */
    int w = snprintf(out, out_len, "%smisrc_playback_ogg_%lu_%d.flac",
                     dir, (unsigned long)GetCurrentProcessId(), channel);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = getenv("TMP");
    if (!dir || !dir[0]) dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = "/tmp";
    int w = snprintf(out, out_len, "%s/misrc_playback_ogg_%d_%d.flac",
                     dir, (int)getpid(), channel);
#endif
    return w > 0 && (size_t)w < out_len;
}

static uint64_t gui_playback_clock_us(void);

// This is a bounded endpoint hint, not a frame-chain/integrity validator.
// find_total_samples seeks and may suppress recoverable decoder errors. Only
// the separate reader below may publish a verified result or persist V2 cache.
typedef struct {
    FILE *file;
    uint64_t file_size, tail_start, metadata_bytes_read, tail_bytes_read, deadline_us;
    const atomic_bool *cancel;
    bool tail, failed, metadata_error;
} playback_tail_io_t;

static bool gui_playback_tail_allowed(playback_tail_io_t *io) {
    if (io->failed || (io->cancel && atomic_load(io->cancel)) ||
        gui_playback_clock_us() >= io->deadline_us) {
        io->failed = true;
        return false;
    }
    return true;
}

static FLAC__StreamDecoderReadStatus gui_playback_tail_read_cb(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *userdata) {
    (void)decoder;
    playback_tail_io_t *io = userdata;
    uint64_t position = 0;
    if (!bytes || !*bytes || !gui_playback_tail_allowed(io) ||
        !gui_playback_file_tell(io->file, &position) || position > io->file_size ||
        (io->tail && position < io->tail_start)) goto abort_tail;
    if (position == io->file_size) {
        *bytes = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    uint64_t used = io->tail ? io->tail_bytes_read : io->metadata_bytes_read;
    uint64_t limit = io->tail ? PLAYBACK_TAIL_MAX_BYTES : PLAYBACK_TAIL_METADATA_BYTES;
    if (used >= limit) goto abort_tail;
    if (*bytes > PLAYBACK_TAIL_READ_BYTES) *bytes = PLAYBACK_TAIL_READ_BYTES;
    if (*bytes > limit - used) *bytes = (size_t)(limit - used);
    if (*bytes > io->file_size - position) *bytes = (size_t)(io->file_size - position);
    size_t n = fread(buffer, 1, *bytes, io->file);
    if (io->tail) io->tail_bytes_read += n; else io->metadata_bytes_read += n;
    if (ferror(io->file) || !n || !gui_playback_tail_allowed(io)) goto abort_tail;
    *bytes = n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
abort_tail:
    io->failed = true;
    if (bytes) *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

static FLAC__StreamDecoderSeekStatus gui_playback_tail_seek_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 position, void *userdata) {
    (void)decoder;
    playback_tail_io_t *io = userdata;
    if (!gui_playback_tail_allowed(io) || position > io->file_size ||
        (io->tail && position < io->tail_start) ||
        (!io->tail && position > PLAYBACK_TAIL_METADATA_BYTES) ||
        !gui_playback_file_seek_abs(io->file, position) || !gui_playback_tail_allowed(io)) {
        io->failed = true; return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus gui_playback_tail_tell_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *position, void *userdata) {
    (void)decoder;
    playback_tail_io_t *io = userdata;
    return gui_playback_tail_allowed(io) && gui_playback_file_tell(io->file, position)
        ? FLAC__STREAM_DECODER_TELL_STATUS_OK : FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
}

static FLAC__StreamDecoderLengthStatus gui_playback_tail_length_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *size, void *userdata) {
    (void)decoder;
    playback_tail_io_t *io = userdata;
    *size = io->file_size;
    return gui_playback_tail_allowed(io) ? FLAC__STREAM_DECODER_LENGTH_STATUS_OK
        : FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
}

static FLAC__bool gui_playback_tail_eof_cb(const FLAC__StreamDecoder *decoder, void *userdata) {
    (void)decoder;
    playback_tail_io_t *io = userdata;
    uint64_t position = 0;
    return !gui_playback_tail_allowed(io) ||
        !gui_playback_file_tell(io->file, &position) || position >= io->file_size;
}

static FLAC__StreamDecoderWriteStatus gui_playback_tail_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const samples[], void *userdata) {
    (void)decoder; (void)frame; (void)samples;
    return gui_playback_tail_allowed(userdata) ? FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE
        : FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}

static void gui_playback_tail_error_cb(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus error, void *userdata) {
    (void)decoder; (void)error;
    playback_tail_io_t *io = userdata;
    if (!io->tail) io->metadata_error = true;
}

static bool gui_playback_tail_samples(const char *filepath, FILE *held_source,
    const gui_playback_length_cache_key_t *loaded_key, const atomic_bool *cancel,
    uint64_t *out_samples) {
    if (!out_samples) return false;
    *out_samples = 0;
    // Optional length discovery never turns unavailable fingerprints into a
    // playback failure, and never binds a path result to a different open FILE.
    if (!filepath || !held_source || !loaded_key || (cancel && atomic_load(cancel))) return false;
    playback_tail_io_t io = {.cancel = cancel,
        .deadline_us = gui_playback_clock_us() + PLAYBACK_TAIL_BUDGET_US};
    gui_playback_length_cache_key_t before, after;
    FLAC__StreamDecoder *decoder = NULL;
    bool initialized = false, ok = false;
    if (!gui_playback_length_cache_snapshot(filepath, &before) ||
        !gui_playback_length_cache_keys_equal(&before, loaded_key) ||
        !gui_playback_length_cache_matches_open_file(held_source, loaded_key)) return false;
    io.file_size = loaded_key->file_size;
    io.tail_start = io.file_size > PLAYBACK_TAIL_MAX_BYTES
        ? io.file_size - PLAYBACK_TAIL_MAX_BYTES : 0;
    io.file = file_open_read_utf8(filepath);
    if (!io.file) return false;
    if (!gui_playback_length_cache_matches_open_file(io.file, loaded_key) ||
        !gui_playback_tail_allowed(&io)) goto done_tail;
    // Avoid unobserved stdio read-ahead outside the declared windows/budgets.
    if (setvbuf(io.file, NULL, _IONBF, 0) != 0) goto done_tail;
    decoder = FLAC__stream_decoder_new();
    if (!decoder) goto done_tail;
    FLAC__stream_decoder_set_md5_checking(decoder, false);
    if (FLAC__stream_decoder_init_stream(decoder, gui_playback_tail_read_cb,
        gui_playback_tail_seek_cb, gui_playback_tail_tell_cb, gui_playback_tail_length_cb,
        gui_playback_tail_eof_cb, gui_playback_tail_write_cb, NULL,
        gui_playback_tail_error_cb, &io) != FLAC__STREAM_DECODER_INIT_STATUS_OK) goto done_tail;
    initialized = true;
    if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder) ||
        io.metadata_error || !gui_playback_tail_allowed(&io)) goto done_tail;
    io.tail = true;
    uint64_t samples = FLAC__stream_decoder_find_total_samples(decoder);
    if (!samples || !gui_playback_tail_allowed(&io) ||
        FLAC__stream_decoder_get_state(decoder) != FLAC__STREAM_DECODER_END_OF_STREAM ||
        !gui_playback_length_cache_matches_open_file(io.file, loaded_key) ||
        !gui_playback_length_cache_matches_open_file(held_source, loaded_key) ||
        !gui_playback_length_cache_snapshot(filepath, &after) ||
        !gui_playback_length_cache_keys_equal(&before, &after) ||
        !gui_playback_tail_allowed(&io)) goto done_tail;
    *out_samples = samples;
    ok = true;
done_tail:
    if (initialized) FLAC__stream_decoder_finish(decoder);
    if (decoder) FLAC__stream_decoder_delete(decoder);
    fclose(io.file);
    return ok;
}

typedef struct {
    buffer_manager_t *scan_buffers;
    const atomic_int *state;
    uint64_t bytes_per_second[3]; // RF, record A, record B; immutable source rates
    uint64_t last_written;
    uint64_t last_progress_us;
    uint64_t last_drops;
    uint64_t drop_until_us;
    bool initialized;
} playback_scan_pressure_t;

typedef struct {
    atomic_uint_fast64_t *bytes_scanned;
    atomic_bool *waiting;
    playback_scan_pressure_t *pressure;
    const gui_playback_length_cache_key_t *expected_key;
    FILE *held_source_file;
} playback_scan_observer_t;

// Read monotonically advancing atomic counters in consumer-before-producer
// order. No ringbuffer size/initialized flag or ordinary app state is accessed:
// record buffers may be lazily allocated while this reader is running.
static bool gui_playback_scan_has_pressure(playback_scan_pressure_t *pressure, uint64_t now) {
    if (!pressure || !pressure->scan_buffers) return false;
    static const buffer_id_t ids[3] = {BUF_CAPTURE_RF, BUF_RECORD_A, BUF_RECORD_B};
    uint64_t backlog[3], written = 0, drops = 0;
    for (int n = 0; n < 3; ++n) {
        const buffer_stats_t *stats = &pressure->scan_buffers->stats[ids[n]];
        uint64_t read = atomic_load(&stats->bytes_read);
        uint64_t output = atomic_load(&stats->bytes_written);
        backlog[n] = output > read ? output - read : 0;
        drops += atomic_load(&stats->write_drops);
        if (n == 0) written = output;
    }
    if (!pressure->initialized) {
        pressure->initialized = true;
        pressure->last_written = written;
        pressure->last_progress_us = now;
        pressure->last_drops = drops;
    }
    if (drops != pressure->last_drops) {
        pressure->last_drops = drops;
        pressure->drop_until_us = now + 250000u;
    }
    int state = pressure->state ? atomic_load(pressure->state) : PLAYBACK_STATE_STOPPED;
    if (written != pressure->last_written || state != PLAYBACK_STATE_PLAYING) {
        pressure->last_written = written;
        pressure->last_progress_us = now;
    }
    for (int n = 0; n < 3; ++n) {
        uint64_t rate = pressure->bytes_per_second[n];
        // Prefer the active readers before RF has 200 ms, or a recorder has
        // 500 ms, waiting. The thresholds use source Hz, not container Hz.
        if (rate && backlog[n] >= rate / (n == 0 ? 5u : 2u)) return true;
    }
    uint64_t raw_rate = pressure->bytes_per_second[0];
    bool starved = state == PLAYBACK_STATE_PLAYING && raw_rate &&
                   backlog[0] < raw_rate / 20u &&
                   now - pressure->last_progress_us >= 250000u;
    return starved || now < pressure->drop_until_us;
}

static bool gui_playback_scan_wait(const atomic_bool *cancel, unsigned milliseconds,
                                   atomic_bool *waiting) {
    if (waiting) atomic_store(waiting, true);
    uint64_t deadline = gui_playback_clock_us() + (uint64_t)milliseconds * 1000u;
    bool ok = true;
    do {
        if (cancel && atomic_load(cancel)) { ok = false; break; }
        thrd_sleep_ms(10); // Cancellation is checked between bounded sleeps.
    } while (gui_playback_clock_us() < deadline);
    if (waiting) atomic_store(waiting, false);
    return ok && !(cancel && atomic_load(cancel));
}

// libFLAC establishes actual frame boundaries without PCM reconstruction.
// The input remains a single 8 MiB sequential cache owned by this scanner.
typedef struct {
    FILE *file;
    uint8_t *window;
    size_t window_size;
    size_t window_position;
    uint64_t window_offset;
    uint64_t file_size;
    uint64_t file_read_offset;
    uint64_t delivered_offset;
    uint64_t audio_offset;
    uint64_t since_yield;
    const atomic_bool *cancel;
    playback_scan_observer_t *observer;
    bool background;
    bool failed;
} playback_scan_io_t;

static FLAC__StreamDecoderReadStatus gui_playback_scan_read_cb(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *userdata) {
    (void)decoder;
    playback_scan_io_t *io = userdata;
    if (!bytes || *bytes == 0 || io->failed || (io->cancel && atomic_load(io->cancel))) {
        if (bytes) *bytes = 0;
        io->failed = true;
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    if (io->window_position == io->window_size) {
        if (io->file_read_offset >= io->file_size) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        io->window_offset = io->file_read_offset;
        size_t n = fread(io->window, 1, PLAYBACK_FRAME_SCAN_CHUNK_BYTES, io->file);
        if (ferror(io->file) || n == 0 || n > io->file_size - io->file_read_offset) {
            io->failed = true; *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        io->window_size = n;
        io->window_position = 0;
        uint64_t previous_audio = io->file_read_offset > io->audio_offset
            ? io->file_read_offset - io->audio_offset : 0;
        io->file_read_offset += n;
        uint64_t audio = io->file_read_offset > io->audio_offset
            ? io->file_read_offset - io->audio_offset : 0;
        if (io->observer && io->observer->bytes_scanned)
            atomic_store(io->observer->bytes_scanned, audio);
        if (io->background) {
            if (io->observer && gui_playback_scan_has_pressure(io->observer->pressure, gui_playback_clock_us()) &&
                !gui_playback_scan_wait(io->cancel, 50u, io->observer->waiting)) {
                io->failed = true; *bytes = 0;
                return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
            }
            io->since_yield += audio - previous_audio;
            if (io->since_yield >= PLAYBACK_FRAME_SCAN_YIELD_BYTES) {
                io->since_yield %= PLAYBACK_FRAME_SCAN_YIELD_BYTES;
                if (!gui_playback_scan_wait(io->cancel, 10u, NULL)) {
                    io->failed = true; *bytes = 0;
                    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
                }
            }
        }
    }
    if (io->cancel && atomic_load(io->cancel)) {
        io->failed = true; *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    size_t available = io->window_size - io->window_position;
    if (*bytes > available) *bytes = available;
    memcpy(buffer, io->window + io->window_position, *bytes);
    io->window_position += *bytes;
    io->delivered_offset += *bytes;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderTellStatus gui_playback_scan_tell_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *offset, void *userdata) {
    (void)decoder;
    // Report bytes supplied to libFLAC, not physical 8 MiB read-ahead. libFLAC
    // subtracts its own bitreader's remaining input for get_decode_position().
    *offset = ((playback_scan_io_t *)userdata)->delivered_offset;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus gui_playback_scan_length_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *length, void *userdata) {
    (void)decoder;
    *length = ((playback_scan_io_t *)userdata)->file_size;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool gui_playback_scan_eof_cb(const FLAC__StreamDecoder *decoder, void *userdata) {
    (void)decoder;
    playback_scan_io_t *io = userdata;
    return io->delivered_offset >= io->file_size;
}

static FLAC__StreamDecoderWriteStatus gui_playback_scan_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const samples[], void *userdata) {
    (void)decoder; (void)frame; (void)samples;
    // skip_single_frame must not reconstruct/output PCM. Never quietly turn
    // this background boundary reader into another playback decoder.
    ((playback_scan_io_t *)userdata)->failed = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}

static void gui_playback_scan_error_cb(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus error, void *userdata) {
    (void)decoder; (void)error;
    // Even a recoverable lost-sync/CRC error makes this full-length result
    // unavailable. libFLAC resynchronization cannot validate a partial count.
    ((playback_scan_io_t *)userdata)->failed = true;
}

static bool gui_playback_scan_header(playback_scan_io_t *io, uint64_t position,
                                      uint8_t header[PLAYBACK_MAX_HEADER_LEN], size_t *size) {
    if (position >= io->file_size) return false;
    *size = io->file_size - position < PLAYBACK_MAX_HEADER_LEN
        ? (size_t)(io->file_size - position) : PLAYBACK_MAX_HEADER_LEN;
    if (position >= io->window_offset && position - io->window_offset <= io->window_size &&
        *size <= io->window_size - (size_t)(position - io->window_offset)) {
        memcpy(header, io->window + (size_t)(position - io->window_offset), *size);
        return true;
    }
    // Rare header crossing an 8 MiB cache boundary, or decoder read-ahead.
    // Preserve the FILE's physical cache-refill offset; do not move its codec
    // cursor or replace the cache being consumed by libFLAC.
    uint64_t restore = 0;
    if (!gui_playback_file_tell(io->file, &restore) || !gui_playback_file_seek_abs(io->file, position)) return false;
    bool ok = fread(header, 1, *size, io->file) == *size && !ferror(io->file);
    if (!gui_playback_file_seek_abs(io->file, restore)) ok = false;
    return ok;
}

static bool gui_playback_scan_samples(const char *filepath, uint64_t audio_offset,
                                      uint32_t min_frame_size, uint32_t max_frame_size,
                                      uint64_t *out_samples, const atomic_bool *cancel,
                                      bool background, playback_scan_observer_t *observer) {
    (void)min_frame_size; (void)max_frame_size; // Unknown frame-size metadata is legal.
    if (!filepath || !out_samples) return false;
    *out_samples = 0;
    if (cancel && atomic_load(cancel)) return false;
    FILE *file = file_open_read_utf8(filepath);
    if (!file) return false;
    FLAC__StreamDecoder *decoder = NULL;
    bool initialized = false;
    bool ok = false;
    playback_scan_io_t io = {.file = file, .audio_offset = audio_offset, .cancel = cancel,
                             .observer = observer, .background = background};
    if (observer && observer->expected_key &&
        (!gui_playback_length_cache_matches_open_file(file, observer->expected_key) ||
         (observer->held_source_file &&
          !gui_playback_length_cache_matches_open_file(observer->held_source_file, observer->expected_key)))) goto done_scan;
    if (PLAYBACK_FSEEK(file, 0, SEEK_END) != 0 || !gui_playback_file_tell(file, &io.file_size) ||
        io.file_size <= audio_offset || !gui_playback_file_seek_abs(file, 0)) goto done_scan;
    io.window = (uint8_t *)malloc(PLAYBACK_FRAME_SCAN_CHUNK_BYTES + 64u);
    if (!io.window) goto done_scan;
    decoder = FLAC__stream_decoder_new();
    if (!decoder) goto done_scan;
    FLAC__stream_decoder_set_md5_checking(decoder, false);
    if (FLAC__stream_decoder_init_stream(decoder, gui_playback_scan_read_cb, NULL,
        gui_playback_scan_tell_cb, gui_playback_scan_length_cb, gui_playback_scan_eof_cb,
        gui_playback_scan_write_cb, NULL, gui_playback_scan_error_cb, &io) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK) goto done_scan;
    initialized = true;
    if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder) || io.failed) goto done_scan;
    FLAC__uint64 position = 0;
    if (!FLAC__stream_decoder_get_decode_position(decoder, &position) || position != audio_offset) goto done_scan;
    uint64_t running = 0, frame_index = 0, last_complete = audio_offset;
    while (true) {
        if (io.failed || (cancel && atomic_load(cancel))) goto done_scan;
        bool has_header = position < io.file_size;
        uint16_t block = 0;
        size_t header_len = 0, bytes = 0;
        uint64_t number = 0;
        bool variable = false;
        if (has_header) {
            uint8_t header[PLAYBACK_MAX_HEADER_LEN];
            if (!gui_playback_scan_header(&io, position, header, &bytes) ||
                !gui_playback_parse_frame_header(header, bytes, &block, &header_len, &number, &variable) ||
                number != (variable ? running : frame_index)) goto done_scan;
        }
        if (!FLAC__stream_decoder_skip_single_frame(decoder) || io.failed ||
            (cancel && atomic_load(cancel))) goto done_scan;
        FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(decoder);
        if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
            // EOS retains the old blocksize and can consume an incomplete
            // next header without an error callback. Require the last fully
            // completed grammar boundary, not EOS's current cursor, at EOF.
            if (has_header || last_complete != io.file_size || running == 0) goto done_scan;
            break;
        }
        uint32_t decoded_block = FLAC__stream_decoder_get_blocksize(decoder);
        FLAC__uint64 next = 0;
        if (!has_header || decoded_block != block || decoded_block == 0 ||
            !FLAC__stream_decoder_get_decode_position(decoder, &next) ||
            next <= position || next > io.file_size || UINT64_MAX - running < decoded_block) goto done_scan;
        running += decoded_block;
        ++frame_index;
        last_complete = next;
        position = next;
    }
    if (observer && observer->expected_key &&
        (!gui_playback_length_cache_matches_open_file(file, observer->expected_key) ||
         (observer->held_source_file &&
          !gui_playback_length_cache_matches_open_file(observer->held_source_file, observer->expected_key)))) goto done_scan;
    if (cancel && atomic_load(cancel)) goto done_scan;
    *out_samples = running;
    ok = true;

done_scan:
    if (observer && observer->waiting) atomic_store(observer->waiting, false);
    if (initialized) FLAC__stream_decoder_finish(decoder);
    if (decoder) FLAC__stream_decoder_delete(decoder);
    free(io.window);
    fclose(file);
    return ok;
}

static bool gui_playback_check_total_samples(uint64_t declared, uint64_t audio_bytes,
                                             uint16_t min_block, uint16_t max_block,
                                             uint32_t min_frame, uint32_t max_frame,
                                             uint32_t channels, uint32_t bps,
                                             uint64_t *out_corrected) {
    if (out_corrected) *out_corrected = 0;
    if (audio_bytes == 0) return true;
    if (declared == 0) return false;

    long double bytes_per_sample = (long double)channels * (long double)((bps + 7u) / 8u);
    if (bytes_per_sample <= 0.0L) bytes_per_sample = 1.0L;

    long double declared_max_bytes = 0.0L;
    if (min_block > 0 && max_frame > 0) {
        declared_max_bytes = (((long double)declared / (long double)min_block) + 1.0L) * (long double)max_frame;
    } else {
        declared_max_bytes = ((long double)declared * bytes_per_sample * 1.05L) + 65536.0L;
    }

    if ((long double)audio_bytes <= declared_max_bytes) {
        return true;
    }

    if (min_block > 0 && max_block > 0 && min_frame > 0 && max_frame > 0) {
        long double lower = ((long double)audio_bytes / (long double)max_frame) * (long double)min_block;
        long double upper = ((long double)audio_bytes / (long double)min_frame) * (long double)max_block;
        uint64_t unique = 0;
        int candidates = 0;
        for (uint64_t k = 1; k < 4096; k++) {
            uint64_t candidate = declared + k * PLAYBACK_FLAC_TOTAL_SAMPLES_FIELD_MOD;
            long double cand_ld = (long double)candidate;
            if (cand_ld > upper) break;
            if (cand_ld >= lower) {
                unique = candidate;
                candidates++;
                if (candidates > 1) break;
            }
        }
        if (candidates == 1 && out_corrected) {
            *out_corrected = unique;
        }
    }

    return false;
}

static bool gui_playback_has_timestamp_at(const char *s, size_t i, size_t len) {
    if (!s || i + 19u > len) return false;
    const unsigned char *b = (const unsigned char *)s;
    return isdigit(b[i + 0]) && isdigit(b[i + 1]) && isdigit(b[i + 2]) && isdigit(b[i + 3]) &&
           b[i + 4] == '.' &&
           isdigit(b[i + 5]) && isdigit(b[i + 6]) &&
           b[i + 7] == '.' &&
           isdigit(b[i + 8]) && isdigit(b[i + 9]) &&
           b[i + 10] == '_' &&
           isdigit(b[i + 11]) && isdigit(b[i + 12]) &&
           b[i + 13] == '.' &&
           isdigit(b[i + 14]) && isdigit(b[i + 15]) &&
           b[i + 16] == '.' &&
           isdigit(b[i + 17]) && isdigit(b[i + 18]);
}

static void gui_playback_capture_base(const char *stem, char *out_base, size_t out_size) {
    if (!out_base || out_size == 0) return;
    out_base[0] = '\0';
    if (!stem || !stem[0]) return;
    size_t len = strlen(stem);
    size_t keep_len = len;
    for (size_t i = 0; i + 19u <= len; i++) {
        if (gui_playback_has_timestamp_at(stem, i, len)) {
            keep_len = i + 19u;
            break;
        }
    }
    if (keep_len >= out_size) keep_len = out_size - 1u;
    memcpy(out_base, stem, keep_len);
    out_base[keep_len] = '\0';
}

static bool gui_playback_split_path(const char *filepath,
                                    char *out_dir, size_t out_dir_size,
                                    char *out_stem, size_t out_stem_size) {
    if (!filepath || !out_dir || out_dir_size == 0 || !out_stem || out_stem_size == 0) {
        return false;
    }

    const char *last_slash = strrchr(filepath, '/');
    const char *sep = last_slash;
#if defined(_WIN32) || defined(_WIN64)
    const char *last_backslash = strrchr(filepath, '\\');
    if (last_backslash && (!sep || last_backslash > sep)) sep = last_backslash;
#endif

    const char *filename = filepath;
    if (sep) {
        size_t dir_len = (size_t)(sep - filepath);
        if (dir_len == 0) {
            snprintf(out_dir, out_dir_size, "%c", PLAYBACK_PATH_SEPARATOR);
        } else {
            if (dir_len >= out_dir_size) return false;
            memcpy(out_dir, filepath, dir_len);
            out_dir[dir_len] = '\0';
        }
        filename = sep + 1;
    } else {
        snprintf(out_dir, out_dir_size, ".");
    }

    const char *dot = strrchr(filename, '.');
    size_t stem_len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (stem_len == 0) {
        return false;
    }
    if (stem_len >= out_stem_size) return false;
    memcpy(out_stem, filename, stem_len);
    out_stem[stem_len] = '\0';
    return true;
}

static bool gui_playback_filename_matches(const char *name, const char *base_prefix, const char *ext_without_dot) {
    if (!name || !base_prefix || !ext_without_dot) return false;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || dot[1] == '\0') return false;
    if (strncmp(name, base_prefix, strlen(base_prefix)) != 0) return false;
    return gui_playback_eq_ascii_ci_bytes((const uint8_t *)(dot + 1), strlen(dot + 1), ext_without_dot);
}

static bool gui_playback_parse_log_duration_text(const uint8_t *text, size_t len, double *out_seconds) {
    if (!text || !out_seconds) return false;
    size_t limit = (len < PLAYBACK_COMPANION_READ_LIMIT) ? len : PLAYBACK_COMPANION_READ_LIMIT;
    const char needle[] = "duration";
    const size_t needle_len = sizeof(needle) - 1u;

    for (size_t i = 0; i + needle_len <= limit; i++) {
        if (!gui_playback_eq_ascii_ci_bytes(text + i, needle_len, needle)) continue;
        size_t j = i + needle_len;
        while (j < limit && (text[j] == ':' || text[j] == '=' || isspace((unsigned char)text[j]))) j++;
        size_t num_start = j;
        while (j < limit && (isdigit((unsigned char)text[j]) || text[j] == '.')) j++;
        if (j <= num_start) continue;
        double secs = 0.0;
        if (gui_playback_parse_f64_bytes(text + num_start, j - num_start, &secs) && secs > 0.0) {
            *out_seconds = secs;
            return true;
        }
    }
    return false;
}

static bool gui_playback_read_log_duration_seconds(const char *path, double *out_seconds) {
    if (!path || !out_seconds) return false;
    FILE *file = file_open_read_utf8(path);
    if (!file) return false;
    uint8_t *buf = (uint8_t *)malloc(PLAYBACK_COMPANION_READ_LIMIT);
    if (!buf) {
        fclose(file);
        return false;
    }
    size_t n = fread(buf, 1, PLAYBACK_COMPANION_READ_LIMIT, file);
    fclose(file);
    bool ok = gui_playback_parse_log_duration_text(buf, n, out_seconds);
    free(buf);
    return ok;
}

static bool gui_playback_read_wav_duration_seconds(const char *path, double *out_seconds) {
    if (!path || !out_seconds) return false;
    FILE *file = file_open_read_utf8(path);
    if (!file) return false;

    uint8_t head[128];
    size_t n = fread(head, 1, sizeof(head), file);
    fclose(file);
    if (n < 12) return false;
    if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) return false;

    size_t i = 12;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;
    bool have_fmt = false;
    bool have_data = false;
    while (i + 8 <= n) {
        const uint8_t *id = &head[i];
        uint32_t chunk_size = (uint32_t)head[i + 4] |
                              ((uint32_t)head[i + 5] << 8) |
                              ((uint32_t)head[i + 6] << 16) |
                              ((uint32_t)head[i + 7] << 24);
        size_t body = i + 8u;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16u <= n) {
            channels = (uint16_t)head[body + 2] | (uint16_t)((uint16_t)head[body + 3] << 8);
            sample_rate = (uint32_t)head[body + 4] |
                          ((uint32_t)head[body + 5] << 8) |
                          ((uint32_t)head[body + 6] << 16) |
                          ((uint32_t)head[body + 7] << 24);
            bits_per_sample = (uint16_t)head[body + 14] | (uint16_t)((uint16_t)head[body + 15] << 8);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            data_size = chunk_size;
            have_data = true;
            break;
        }

        size_t next = body + (size_t)chunk_size + ((chunk_size & 1u) ? 1u : 0u);
        if (next <= i) break;
        i = next;
    }

    if (!have_fmt || !have_data || sample_rate == 0 || channels == 0 || bits_per_sample == 0) {
        return false;
    }
    long double bytes_per_frame = (long double)channels * ((long double)bits_per_sample / 8.0L);
    if (bytes_per_frame <= 0.0L) return false;
    long double frames = (long double)data_size / bytes_per_frame;
    long double duration = frames / (long double)sample_rate;
    if (!(duration > 0.0L) || !isfinite((double)duration)) return false;
    *out_seconds = (double)duration;
    return true;
}

static bool gui_playback_try_companion_duration(const char *filepath, double *out_seconds) {
    if (!filepath || !out_seconds) return false;

    char dir[FILE_PATH_MAX];
    char stem[FILE_PATH_MAX];
    char base[FILE_PATH_MAX];
    if (!gui_playback_split_path(filepath, dir, sizeof(dir), stem, sizeof(stem))) {
        return false;
    }
    gui_playback_capture_base(stem, base, sizeof(base));
    if (base[0] == '\0') return false;

#if defined(_WIN32) || defined(_WIN64)
    char pattern[FILE_PATH_MAX];
    int length = snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    wchar_t wide_pattern[FILE_PATH_MAX];
    if (length < 0 || (size_t)length >= sizeof(pattern) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1,
                             wide_pattern, sizeof(wide_pattern) / sizeof(*wide_pattern))) {
        return false;
    }
    /* Keep LOG preferred over WAV, with both names and directories in UTF-8. */
    for (unsigned int kind = 0; kind < 2; kind++) {
        struct _wfinddata_t entry;
        intptr_t handle = _wfindfirst(wide_pattern, &entry);
        if (handle == -1) continue;
        do {
            if (entry.attrib & _A_SUBDIR) continue;
            char name[3 * MAX_PATH];
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.name, -1,
                                     name, sizeof(name), NULL, NULL)) continue;
            if (!gui_playback_filename_matches(name, base, kind == 0 ? "log" : "wav")) continue;
            char candidate[FILE_PATH_MAX];
            length = snprintf(candidate, sizeof(candidate), "%s\\%s", dir, name);
            if (length < 0 || (size_t)length >= sizeof(candidate)) continue;
            bool found = kind == 0
                ? gui_playback_read_log_duration_seconds(candidate, out_seconds)
                : gui_playback_read_wav_duration_seconds(candidate, out_seconds);
            if (found && *out_seconds > 0.0) {
                _findclose(handle);
                return true;
            }
        } while (_wfindnext(handle, &entry) == 0);
        _findclose(handle);
    }
#else
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *entry = NULL;
        while ((entry = readdir(dp)) != NULL) {
            if (!gui_playback_filename_matches(entry->d_name, base, "log")) continue;
            char candidate[FILE_PATH_MAX];
            int length = snprintf(candidate, sizeof(candidate), "%s/%s", dir, entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(candidate)) continue;
            if (gui_playback_read_log_duration_seconds(candidate, out_seconds) && *out_seconds > 0.0) {
                closedir(dp);
                return true;
            }
        }
        closedir(dp);
    }

    dp = opendir(dir);
    if (dp) {
        struct dirent *entry = NULL;
        while ((entry = readdir(dp)) != NULL) {
            if (!gui_playback_filename_matches(entry->d_name, base, "wav")) continue;
            char candidate[FILE_PATH_MAX];
            int length = snprintf(candidate, sizeof(candidate), "%s/%s", dir, entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(candidate)) continue;
            if (gui_playback_read_wav_duration_seconds(candidate, out_seconds) && *out_seconds > 0.0) {
                closedir(dp);
                return true;
            }
        }
        closedir(dp);
    }
#endif

    return false;
}

static uint64_t gui_playback_resolve_total_samples(const char *filepath,
                                                   uint32_t header_sample_rate,
                                                   uint8_t bits_per_sample,
                                                   uint64_t declared_total_samples,
                                                   const playback_streaminfo_aux_t *aux,
                                                   playback_total_source_t *out_source,
                                                   double *out_real_rate_hz,
                                                   const atomic_bool *cancel,
                                                   const gui_playback_length_cache_key_t *loaded_key,
                                                   FILE *held_source_file) {
    if (out_source) {
        *out_source = (declared_total_samples > 0)
            ? PLAYBACK_TOTAL_SOURCE_STREAMINFO
            : PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    }
    if (out_real_rate_hz) {
        *out_real_rate_hz = gui_playback_resolve_real_rate_hz(filepath, header_sample_rate, NULL);
    }
    if (!filepath || !aux) {
        return declared_total_samples;
    }

    playback_vorbis_rf_tags_t tags = {0};
    (void)gui_playback_read_vorbis_rf_tags(filepath, &tags);
    double real_rate_hz = gui_playback_resolve_real_rate_hz(filepath, header_sample_rate, &tags);
    if (out_real_rate_hz) {
        *out_real_rate_hz = real_rate_hz;
    }

    if (tags.has_rf_total_samples || tags.has_rf_sample_rate || tags.has_duration_seconds) {
        if (tags.has_rf_total_samples) {
            if (tags.rf_total_samples > 0) {
                if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_VORBIS_TAG;
                return tags.rf_total_samples;
            }
        }
        if (tags.has_duration_seconds && real_rate_hz > 0.0) {
            uint64_t resolved = gui_playback_round_positive_ld((long double)tags.duration_seconds * (long double)real_rate_hz);
            if (resolved > 0) {
                if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_VORBIS_DURATION;
                return resolved;
            }
        }
    }

    uint64_t audio_offset = 0;
    uint64_t file_size = 0;
    bool have_audio_bounds = gui_playback_find_audio_offset_and_size(filepath, &audio_offset, &file_size) &&
                             file_size > audio_offset;
    uint64_t resolved_total = declared_total_samples;

    if (declared_total_samples > 0 && have_audio_bounds) {
        uint64_t corrected = 0;
        bool trustworthy = gui_playback_check_total_samples(
            declared_total_samples,
            file_size - audio_offset,
            aux->min_block_size,
            aux->max_block_size,
            aux->min_frame_size,
            aux->max_frame_size,
            aux->channels,
            bits_per_sample,
            &corrected);

        if (!trustworthy) {
            if (corrected > 0) {
                resolved_total = corrected;
                if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_CORRECTED;
            } else {
                uint64_t bytes_per_sample = (uint64_t)aux->channels * (uint64_t)((bits_per_sample + 7u) / 8u);
                if (bytes_per_sample > 0) {
                    uint64_t audio_bytes = file_size - audio_offset;
                    uint64_t lower = (audio_bytes + bytes_per_sample - 1u) / bytes_per_sample;
                    for (uint64_t k = 1; k <= 4096; k++) {
                        uint64_t candidate = declared_total_samples + k * PLAYBACK_FLAC_TOTAL_SAMPLES_FIELD_MOD;
                        if (candidate >= lower) {
                            resolved_total = candidate;
                            if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_STREAMINFO_WRAP_ESTIMATED;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (resolved_total == 0 && real_rate_hz > 0.0) {
        double companion_seconds = 0.0;
        if (gui_playback_try_companion_duration(filepath, &companion_seconds) &&
            companion_seconds > 0.0 && isfinite(companion_seconds)) {
            resolved_total = gui_playback_round_positive_ld((long double)companion_seconds * (long double)real_rate_hz);
            if (resolved_total > 0 && out_source) {
                *out_source = PLAYBACK_TOTAL_SOURCE_COMPANION;
            }
        }
    }

    if (resolved_total == 0 && (!held_source_file || loaded_key) && !(cancel && atomic_load(cancel))) {
        gui_playback_length_cache_key_t key;
        uint64_t cached_total = 0;
        if (gui_playback_length_cache_snapshot(filepath, &key) &&
            (!loaded_key || gui_playback_length_cache_keys_equal(loaded_key, &key)) &&
            (!held_source_file || gui_playback_length_cache_matches_open_file(held_source_file, &key)) &&
            gui_playback_length_cache_lookup(&key, &cached_total) && cached_total > 0) {
            resolved_total = cached_total;
            if (out_source) *out_source = PLAYBACK_TOTAL_SOURCE_LENGTH_CACHE;
        }
    }

    return resolved_total;
}

static double gui_playback_apply_resolved_file_info(playback_file_info_t *info,
                                                   const playback_streaminfo_aux_t *aux,
                                                   const char *channel_label,
                                                   const atomic_bool *cancel,
                                                   const gui_playback_length_cache_key_t *loaded_key,
                                                   FILE *held_source_file) {
    if (!info || !aux) return 0.0;
    playback_total_source_t source = PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    double real_rate_hz = 0.0;
    uint64_t resolved_total = gui_playback_resolve_total_samples(
        info->filepath,
        info->sample_rate,
        info->bits_per_sample,
        info->header_total_samples,
        aux,
        &source,
        &real_rate_hz, cancel, loaded_key, held_source_file);

    if (resolved_total > 0) {
        info->total_samples = resolved_total;
    }
    // Finalized RF captures may advertise RF/1000 totals in STREAMINFO. A
    // bounded tail read also handles that convention without delaying startup
    // for a full scan or trusting a too-small seek clamp. Persist only the
    // separate verifier's result; a disagreeing endpoint remains provisional.
    uint64_t tail_total = 0;
    if (source != PLAYBACK_TOTAL_SOURCE_LENGTH_CACHE &&
        gui_playback_tail_samples(info->filepath, held_source_file, loaded_key, cancel, &tail_total) &&
        tail_total != info->total_samples) {
        info->total_samples = tail_total;
        info->length_provisional = true;
        source = PLAYBACK_TOTAL_SOURCE_TAIL_ENDPOINT;
    }
    if (real_rate_hz <= 0.0) {
        real_rate_hz = gui_playback_resolve_real_rate_hz(info->filepath, info->sample_rate, NULL);
    }
    if (real_rate_hz > 0.0 && isfinite(real_rate_hz)) {
        info->real_sample_rate_hz = real_rate_hz;
    } else {
        info->real_sample_rate_hz = 0.0;
        real_rate_hz = 0.0;
    }
    if (real_rate_hz > 0.0 && info->total_samples > 0) {
        info->duration_seconds = (double)info->total_samples / real_rate_hz;
    } else {
        info->duration_seconds = 0.0;
    }

    fprintf(stderr,
            "[PLAYBACK] %s header total=%" PRIu64 " resolved total=%" PRIu64
            " (source=%s, real_rate=%.0fHz, %.2f sec)\n",
            channel_label ? channel_label : "File",
            info->header_total_samples,
            info->total_samples,
            gui_playback_total_source_name(source),
            real_rate_hz,
            info->duration_seconds);
    // The RF rate is independent of whether a timeline length is available.
    return real_rate_hz;
}

#endif // LIBFLAC_ENABLED

//-----------------------------------------------------------------------------
// FLAC Decoder Callbacks
//-----------------------------------------------------------------------------

#if LIBFLAC_ENABLED == 1

// Channel A write callback
static FLAC__StreamDecoderWriteStatus decoder_write_cb_a(
    const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;
    if (!ctx) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    uint32_t samples = frame->header.blocksize;
    uint8_t bps = frame->header.bits_per_sample;

    // Ensure buffer space
    if (ctx->decode_available_a + samples > ctx->decode_buf_size) {
        // Buffer overflow - shouldn't happen with proper flow control
        fprintf(stderr, "[PLAYBACK] Channel A decode buffer overflow\n");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // Convert to int16 and store
    int16_t *out = ctx->decode_buf_a + ctx->decode_available_a;
    const FLAC__int32 *in = buffer[0];  // Mono FLAC

    // Debug: print first frame's sample info
    static int debug_count_a = 0;
    if (debug_count_a < 5) {
        int32_t min_val = in[0], max_val = in[0];
        for (uint32_t i = 1; i < samples && i < 1000; i++) {
            if (in[i] < min_val) min_val = in[i];
            if (in[i] > max_val) max_val = in[i];
        }
        fprintf(stderr, "[PLAYBACK] Ch A frame %d: bps=%u, samples=%u, range=[%d, %d]\n",
                debug_count_a, bps, samples, min_val, max_val);
        debug_count_a++;
    }

    // FLAC returns samples as int32 with the full bit depth. The 12-bit
    // display container (raw packing, extraction ((2047 - field) << 4), VU
    // meters, peaks and clipping) matches the live capture preview 1:1 for
    // recorder files. 16-bit files now carry TWO real-world conventions
    // (measured on real captures): the recorder's 12-bit container << 4
    // (every sample on a 16-grid) AND generic tools writing the sample
    // values DIRECTLY (a 20000 Hz tape FLAC with +-2190 peaks, 93.8%
    // non-multiples of 16). A blanket >> 4 crushed the second kind to 1/16
    // amplitude - the waveform-playback accuracy bug - so the convention
    // is detected from the first non-silent frame and is sticky per
    // channel per file (see gui_playback_decide_conv16):
    //   16-bit FLAC: recorder 12-bit container << 4 -> >> 4 restore; direct-
    //               domain values -> pass through; full-range generic ->
    //               >> 4 display downscale
    //   12-bit FLAC: 12-bit container as-is (range -2048 to 2047)
    //   8-bit FLAC:  12-bit container >> 4 -> << 4 restore (also the correct
    //               display scaling for generic full-range 8-bit audio)
    if (bps == 16) {
        gui_playback_decide_conv16(&ctx->conv16_state[0], in, samples);
        if (ctx->conv16_state[0] == PLAYBACK_CONV16_PASS) {
            // Direct-domain values: pass through unchanged (encode_raw_sample
            // clamps into the raw 12-bit packing domain downstream).
            for (uint32_t i = 0; i < samples; i++) {
                out[i] = (int16_t)in[i];
            }
        } else {
            // Recorder << 4 restore, or a full-range generic file scaled
            // down into the 12-bit display container: both >> 4.
            for (uint32_t i = 0; i < samples; i++) {
                out[i] = (int16_t)(in[i] >> 4);
            }
        }
    } else if (bps == 12) {
        // 12-bit FLAC: already in 12-bit range, just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else if (bps == 8) {
        // 8-bit FLAC: shift left 4 to restore the 12-bit container the live
        // capture pipeline used (8-bit was stored as container >> 4)
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] << 4);
        }
    } else {
        // Unknown bit depth - just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    }

    ctx->decode_available_a += samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// Channel B write callback
static FLAC__StreamDecoderWriteStatus decoder_write_cb_b(
    const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;
    if (!ctx) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    uint32_t samples = frame->header.blocksize;
    uint8_t bps = frame->header.bits_per_sample;

    if (ctx->decode_available_b + samples > ctx->decode_buf_size) {
        fprintf(stderr, "[PLAYBACK] Channel B decode buffer overflow\n");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    int16_t *out = ctx->decode_buf_b + ctx->decode_available_b;
    const FLAC__int32 *in = buffer[0];

    // Debug: print first frame's sample info
    static int debug_count_b = 0;
    if (debug_count_b < 5) {
        int32_t min_val = in[0], max_val = in[0];
        for (uint32_t i = 1; i < samples && i < 1000; i++) {
            if (in[i] < min_val) min_val = in[i];
            if (in[i] > max_val) max_val = in[i];
        }
        fprintf(stderr, "[PLAYBACK] Ch B frame %d: bps=%u, samples=%u, range=[%d, %d]\n",
                debug_count_b, bps, samples, min_val, max_val);
        debug_count_b++;
    }

    // Same convention detection as Channel A (see the write-callback note
    // there): restore the 12-bit container for recorder files, pass direct-
    // domain 16-bit values through, downscale full-range generic 16-bit.
    if (bps == 16) {
        gui_playback_decide_conv16(&ctx->conv16_state[1], in, samples);
        if (ctx->conv16_state[1] == PLAYBACK_CONV16_PASS) {
            for (uint32_t i = 0; i < samples; i++) {
                out[i] = (int16_t)in[i];
            }
        } else {
            for (uint32_t i = 0; i < samples; i++) {
                out[i] = (int16_t)(in[i] >> 4);
            }
        }
    } else if (bps == 12) {
        // 12-bit FLAC: already in 12-bit range, just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else if (bps == 8) {
        // 8-bit FLAC: shift left 4 to restore the 12-bit container
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] << 4);
        }
    } else {
        // Unknown bit depth - just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    }

    ctx->decode_available_b += samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// Metadata callback (extracts stream info)
static void decoder_metadata_cb_a(
    const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata,
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;
    if (!ctx) return;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        ctx->info_a.sample_rate = metadata->data.stream_info.sample_rate;
        ctx->info_a.bits_per_sample = metadata->data.stream_info.bits_per_sample;
        ctx->info_a.header_total_samples = ctx->io_a.file
            ? ctx->io_a.header_total : metadata->data.stream_info.total_samples;
        ctx->info_a.total_samples = ctx->info_a.header_total_samples;
        ctx->stream_a.min_block_size = metadata->data.stream_info.min_blocksize;
        ctx->stream_a.max_block_size = metadata->data.stream_info.max_blocksize;
        ctx->stream_a.min_frame_size = metadata->data.stream_info.min_framesize;
        ctx->stream_a.max_frame_size = metadata->data.stream_info.max_framesize;
        ctx->stream_a.channels = metadata->data.stream_info.channels;
        if (ctx->info_a.sample_rate > 0) {
            ctx->info_a.duration_seconds = (double)ctx->info_a.total_samples / ctx->info_a.sample_rate;
        }
        ctx->info_a.valid = true;
        fprintf(stderr, "[PLAYBACK] File A header: %u Hz, %u-bit, %u ch, %" PRIu64 " samples\n",
                ctx->info_a.sample_rate, ctx->info_a.bits_per_sample,
                ctx->stream_a.channels, ctx->info_a.total_samples);
    }
}

static void decoder_metadata_cb_b(
    const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata,
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;
    if (!ctx) return;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        ctx->info_b.sample_rate = metadata->data.stream_info.sample_rate;
        ctx->info_b.bits_per_sample = metadata->data.stream_info.bits_per_sample;
        ctx->info_b.header_total_samples = ctx->io_b.file
            ? ctx->io_b.header_total : metadata->data.stream_info.total_samples;
        ctx->info_b.total_samples = ctx->info_b.header_total_samples;
        ctx->stream_b.min_block_size = metadata->data.stream_info.min_blocksize;
        ctx->stream_b.max_block_size = metadata->data.stream_info.max_blocksize;
        ctx->stream_b.min_frame_size = metadata->data.stream_info.min_framesize;
        ctx->stream_b.max_frame_size = metadata->data.stream_info.max_framesize;
        ctx->stream_b.channels = metadata->data.stream_info.channels;
        if (ctx->info_b.sample_rate > 0) {
            ctx->info_b.duration_seconds = (double)ctx->info_b.total_samples / ctx->info_b.sample_rate;
        }
        ctx->info_b.valid = true;
        fprintf(stderr, "[PLAYBACK] File B header: %u Hz, %u-bit, %u ch, %" PRIu64 " samples\n",
                ctx->info_b.sample_rate, ctx->info_b.bits_per_sample,
                ctx->stream_b.channels, ctx->info_b.total_samples);
    }
}

// Error callback
static void decoder_error_cb(
    const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus status,
    void *client_data)
{
    (void)decoder;
    (void)client_data;
    fprintf(stderr, "[PLAYBACK] FLAC decoder error: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

// Forward IO callbacks to the owning per-channel decode buffers.
static FLAC__StreamDecoderWriteStatus decoder_io_write_cb_a(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *userdata) {
    playback_io_t *io = userdata;
    return decoder_write_cb_a(decoder, frame, buffer, io ? io->owner : NULL);
}
static void decoder_io_metadata_cb_a(const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata, void *userdata) {
    playback_io_t *io = userdata;
    decoder_metadata_cb_a(decoder, metadata, io ? io->owner : NULL);
}

// Forward IO callbacks to the owning per-channel decode buffers.
static FLAC__StreamDecoderWriteStatus decoder_io_write_cb_b(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *userdata) {
    playback_io_t *io = userdata;
    return decoder_write_cb_b(decoder, frame, buffer, io ? io->owner : NULL);
}
static void decoder_io_metadata_cb_b(const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata, void *userdata) {
    playback_io_t *io = userdata;
    decoder_metadata_cb_b(decoder, metadata, io ? io->owner : NULL);
}

#endif // LIBFLAC_ENABLED

//-----------------------------------------------------------------------------
// File Validation
//-----------------------------------------------------------------------------

bool gui_playback_validate_file(const char *filepath, playback_file_info_t *info) {
    if (!filepath || !info) return false;

    memset(info, 0, sizeof(*info));
    if (strlen(filepath) >= sizeof(info->filepath)) return false;
    strcpy(info->filepath, filepath);

#if LIBFLAC_ENABLED == 1
    // Sniff the container first (content, then extension for headerless raw
    // PCM): native FLAC keeps the metadata-API probe below; Ogg FLAC, WAV and
    // the raw formats have their own probes. Unsupported containers get a
    // specific, actionable error instead of a FLAC parse failure.
    {
        char probe_err[192] = {0};
        playback_input_format_t fmt = PLAYBACK_INPUT_FLAC;
        uint8_t magic[4] = {0};
        if (!gui_playback_sniff_format(filepath, &fmt, magic, probe_err, sizeof(probe_err))) {
            snprintf(info->error, sizeof(info->error), "%s", probe_err);
            fprintf(stderr, "[PLAYBACK] %s (%s)\n", probe_err, filepath);
            return false;
        }
        info->format = (int)fmt;
        if (fmt != PLAYBACK_INPUT_FLAC) {
            if (fmt == PLAYBACK_INPUT_OGG_FLAC) {
                if (!gui_playback_ogg_probe(info, filepath, probe_err, sizeof(probe_err))) {
                    fprintf(stderr, "[PLAYBACK] %s (%s)\n",
                            info->error[0] ? info->error : probe_err, filepath);
                    return false;
                }
            } else {
                playback_raw_reader_t r;
                if (!gui_playback_raw_open(&r, info, filepath, fmt, probe_err, sizeof(probe_err))) {
                    fprintf(stderr, "[PLAYBACK] %s (%s)\n",
                            info->error[0] ? info->error : probe_err, filepath);
                    return false;
                }
                if (r.file) {
                    fclose(r.file);
                    r.file = NULL;
                }
            }
            fprintf(stderr,
                    "[PLAYBACK] Validate: format=%s, total=%" PRIu64
                    " (real_rate=%.0fHz, %.2f sec)\n",
                    gui_playback_input_format_name((playback_input_format_t)info->format),
                    info->total_samples, info->real_sample_rate_hz, info->duration_seconds);
            return info->valid;
        }
    }

    // Use FLAC metadata API to read stream info without full decode
    // FLAC__metadata_get_streaminfo fills in an existing struct
    FLAC__StreamMetadata metadata;

    if (!FLAC__metadata_get_streaminfo(filepath, &metadata)) {
        fprintf(stderr, "[PLAYBACK] Failed to read FLAC metadata from: %s\n", filepath);
        return false;
    }

    FLAC__StreamMetadata_StreamInfo *si = &metadata.data.stream_info;
    playback_streaminfo_aux_t stream = {
        .min_block_size = si->min_blocksize,
        .max_block_size = si->max_blocksize,
        .min_frame_size = si->min_framesize,
        .max_frame_size = si->max_framesize,
        .channels = si->channels
    };

    info->sample_rate = si->sample_rate;
    info->bits_per_sample = si->bits_per_sample;
    info->header_total_samples = si->total_samples;
    info->total_samples = si->total_samples;
    playback_total_source_t source = PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    double real_rate_hz = 0.0;
    uint64_t resolved_total = gui_playback_resolve_total_samples(
        filepath,
        info->sample_rate,
        info->bits_per_sample,
        info->total_samples,
        &stream,
        &source,
        &real_rate_hz, NULL, NULL, NULL);
    if (resolved_total > 0) {
        info->total_samples = resolved_total;
    }

    if (real_rate_hz <= 0.0) {
        real_rate_hz = gui_playback_resolve_real_rate_hz(filepath, info->sample_rate, NULL);
    }
    info->real_sample_rate_hz = real_rate_hz > 0.0 && isfinite(real_rate_hz) ? real_rate_hz : 0.0;
    if (real_rate_hz > 0.0 && info->total_samples > 0) {
        info->duration_seconds = (double)info->total_samples / real_rate_hz;
    } else {
        info->duration_seconds = 0.0;
    }
    fprintf(stderr,
            "[PLAYBACK] Validate: resolved total=%" PRIu64 " (source=%s, real_rate=%.0fHz, %.2f sec)\n",
            info->total_samples,
            gui_playback_total_source_name(source),
            real_rate_hz,
            info->duration_seconds);

    // Validate compatibility with MISRC format
    // Expected: 40kHz, 8/12/16-bit, mono
    bool compatible = true;

    if (info->sample_rate != 40000) {
        fprintf(stderr, "[PLAYBACK] Warning: Sample rate %u Hz (expected 40000 Hz)\n", info->sample_rate);
        // Allow non-40kHz files, but warn
    }

    if (stream.channels != 1) {
        fprintf(stderr, "[PLAYBACK] Error: FLAC has %u channels (expected mono)\n", stream.channels);
        compatible = false;
    }

    if (info->bits_per_sample != 8 && info->bits_per_sample != 12 && info->bits_per_sample != 16) {
        fprintf(stderr, "[PLAYBACK] Warning: Bit depth %u (expected 8, 12, or 16)\n", info->bits_per_sample);
        // Still allow, will attempt conversion
    }

    info->valid = compatible;
    return compatible;

#else
    fprintf(stderr, "[PLAYBACK] FLAC support not compiled in\n");
    return false;
#endif
}

//-----------------------------------------------------------------------------
// Drag-and-drop: audio source + file loading (the full capture set)
//-----------------------------------------------------------------------------
// A complete capture set is RF A + RF B + clockgen audio. The drop handler
// classifies every dropped file by CONTENT (audio-rate vs RF-domain) and
// routes it: audio -> the playback monitor source (48 k stereo via the same
// play_q the Demod panel feeds; Audio Mon gates audibility), RF -> the file
// of the channel whose panel zone the drop landed on. The audio feed is
// POSITION-DRIVEN (target = playhead seconds x audio rate), so seeks, loop
// resets, speed changes and pause all follow the RF timeline; a jump flushes
// the monitor queue so stale audio never plays after a scrub.

#if LIBFLAC_ENABLED == 1

#define PLAYBACK_AUDIO_CLASSIFY_MAX_HZ 400000u  // RF files resolve to MHz; audio <= 384 k
#define PLAYBACK_AUDIO_FEED_LOOKAHEAD_MS 50     // feed ahead of the playhead (rides jitter)
#define PLAYBACK_AUDIO_FEED_SPAN_MAX_FRAMES 2400 // <= 50 ms decoded per span call
#define PLAYBACK_AUDIO_OUT48_CAP 65536           // push buffer (any rate >= 8k at 0.25x)

typedef struct playback_audio_source {
    FILE *file;                  // Owned FILE (WAV direct; FLAC via the IO layer)
    bool is_wav;                 // WAV direct reads vs private FLAC decoder
    uint32_t rate_hz;            // Resolved TRUE audio rate (Hz domain)
    uint32_t channels;           // Source channels (the feed takes CH1/CH2; mono -> both)
    uint8_t bits;                // Source bits per sample (8/16/24/32)
    uint64_t total_frames;       // Audio frames in the span
    uint64_t fed_frames;         // Frames already fed (position-synced cursor)
    double out_pos;              // Fractional 48 k output cursor, in source frames
    bool exhausted;              // Decoder/reader hit EOF mid-timeline
    // WAV path
    uint64_t data_offset;        // First audio byte (data chunk body)
    uint64_t data_bytes;         // Audio bytes present
    uint32_t frame_bytes;        // channels * bits/8
    // FLAC path
    FLAC__StreamDecoder *decoder; // Private decoder (analysis-worker pattern)
    playback_io_t io;              // IO layer + owner back-pointer (no SI patch:
                                   // audio STREAMINFO is honest)
    uint64_t flac_next;           // Decoder position after the last span (skip
                                   // the seek when the next span is contiguous)
    // Staging + push buffers
    int16_t *stage;               // Converted STEREO int16 frames per span
    size_t stage_cap;              // Capacity in frames
    size_t staged;                // Frames staged in the current span
    size_t want;                  // Frames requested for the current span
    int16_t *out48;               // Linear-resampled 48 k stereo push buffer
    size_t out48_cap;              // Capacity in frames
    size_t out48_used;             // Frames filled since the last push
} playback_audio_source_t;

static int16_t gui_playback_audio_to_i16(int32_t v, unsigned bits) {
    switch (bits) {
        case 8:  return (int16_t)(v << 8);   // caller pre-centres unsigned
        case 16: return (int16_t)v;
        case 24: return (int16_t)(v >> 8);
        default: return (int16_t)(v >> 16); // 32
    }
}

static FLAC__StreamDecoderWriteStatus gui_playback_audio_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    playback_audio_source_t *a = io ? (playback_audio_source_t *)io->owner : NULL;
    if (!a || !frame || !buffer || !buffer[0] || !a->stage) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    const FLAC__int32 *ch0 = buffer[0];
    const FLAC__int32 *ch1 = (a->channels > 1 && buffer[1]) ? buffer[1] : ch0;
    int shift = (a->bits > 16) ? (int)a->bits - 16 : 0;
    uint32_t n = frame->header.blocksize;
    for (uint32_t i = 0; i < n; i++) {
        if (a->staged >= a->want) break;
        a->stage[2 * a->staged] = (int16_t)(ch0[i] >> shift);
        a->stage[2 * a->staged + 1] = (int16_t)(ch1[i] >> shift);
        a->staged++;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void gui_playback_audio_meta_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata,
    void *client_data)
{
    (void)decoder; (void)metadata; (void)client_data;
}

static void gui_playback_audio_error_cb(
    const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status,
    void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    playback_audio_source_t *a = io ? (playback_audio_source_t *)io->owner : NULL;
    if (a && !a->exhausted) {
        fprintf(stderr, "[PLAYBACK] Audio decoder error: %s\n",
                FLAC__StreamDecoderErrorStatusString[status]);
    }
}

static void gui_playback_audio_destroy(playback_audio_source_t **ap) {
    if (!ap || !*ap) return;
    playback_audio_source_t *a = *ap;
    if (a->decoder) {
        FLAC__stream_decoder_finish(a->decoder);
        FLAC__stream_decoder_delete(a->decoder);
    }
    if (a->file) fclose(a->file);
    free(a->stage);
    free(a->out48);
    free(a);
    *ap = NULL;
}

// Open + resolve the monitor audio source. WAV (the clockgen PCM1802
// captures, 1-8 ch) reads frames directly; native FLAC uses a private
// decoder + the shared IO layer (seek_absolute + process_single, the
// analysis-worker pattern). Ogg-FLAC audio is rejected (remux the RF way
// is not worth it for monitor audio).
static playback_audio_source_t *gui_playback_audio_open(const char *filepath,
                                                          char *err, size_t err_len) {
    playback_audio_source_t *a = NULL;
    FILE *file = NULL;
    if (err && err_len) err[0] = '\0';
    if (!filepath || !filepath[0]) {
        snprintf(err, err_len, "no audio file");
        return NULL;
    }
    playback_input_format_t fmt = PLAYBACK_INPUT_FLAC;
    uint8_t magic[4] = {0};
    if (!gui_playback_sniff_format(filepath, &fmt, magic, err, err_len)) {
        return NULL;
    }
    a = calloc(1, sizeof(*a));
    if (!a) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }

    if (fmt == PLAYBACK_INPUT_WAV) {
        uint64_t file_size = 0;
        uint32_t rate = 0, bits = 0;
        uint16_t channels = 0;
        file = file_open_read_utf8(filepath);
        if (!file ||
            PLAYBACK_FSEEK(file, 0, SEEK_END) != 0 ||
            !gui_playback_file_tell(file, &file_size) ||
            !gui_playback_file_seek_abs(file, 0) ||
            !gui_playback_parse_wav(file, file_size, &rate, &bits, &channels,
                                     &a->data_offset, &a->data_bytes, err, err_len)) {
            if (file) fclose(file);
            goto fail;
        }
        // Same rate convention as the raw reader (FLAC-Chop's rule): a
        // standard audio rate is real audio as-is; > 1 MHz is a true RF
        // rate; anything else is the RF /1000 convention.
        double real_hz = (gui_playback_is_audio_sample_rate(rate) || rate > 1000000u)
                         ? (double)rate : (double)rate * 1000.0;
        if (!gui_playback_is_audio_sample_rate((uint32_t)llround(real_hz))) {
            snprintf(err, err_len, "WAV rate %u Hz is not an audio rate", rate);
            fclose(file);
            goto fail;
        }
        a->is_wav = true;
        a->rate_hz = (uint32_t)llround(real_hz);
        a->channels = channels;
        a->bits = (uint8_t)bits;
        a->frame_bytes = (uint32_t)channels * (bits / 8u);
        a->total_frames = a->frame_bytes ? a->data_bytes / a->frame_bytes : 0;
        if (a->total_frames == 0) {
            snprintf(err, err_len, "WAV has no audio data: %s", filepath);
            fclose(file);
            goto fail;
        }
        if (!gui_playback_file_seek_abs(file, a->data_offset)) {
            snprintf(err, err_len, "seek failed: %s", filepath);
            fclose(file);
            goto fail;
        }
        a->file = file;
    } else if (fmt == PLAYBACK_INPUT_FLAC) {
        FLAC__StreamMetadata si;
        if (!FLAC__metadata_get_streaminfo(filepath, &si)) {
            snprintf(err, err_len, "not a valid FLAC stream: %s", filepath);
            goto fail;
        }
        double real_hz = gui_playback_resolve_real_rate_hz(
            filepath, si.data.stream_info.sample_rate, NULL);
        if (!gui_playback_is_audio_sample_rate((uint32_t)llround(real_hz))) {
            snprintf(err, err_len,
                     "FLAC rate %u Hz is not an audio rate (RF file?)",
                     si.data.stream_info.sample_rate);
            goto fail;
        }
        if (si.data.stream_info.total_samples == 0) {
            snprintf(err, err_len, "audio FLAC has an unknown/zero length (streaming write?)");
            goto fail;
        }
        if (si.data.stream_info.channels == 0 || si.data.stream_info.channels > 8) {
            snprintf(err, err_len, "FLAC has %u channels (expected 1-8)",
                     si.data.stream_info.channels);
            goto fail;
        }
        if (si.data.stream_info.bits_per_sample < 8 ||
            si.data.stream_info.bits_per_sample > 32) {
            snprintf(err, err_len, "FLAC bit depth %u is not 8-32",
                     si.data.stream_info.bits_per_sample);
            goto fail;
        }
        file = file_open_read_utf8(filepath);
        if (!file) {
            snprintf(err, err_len, "open failed: %s", filepath);
            goto fail;
        }
        a->rate_hz = (uint32_t)llround(real_hz);
        a->channels = si.data.stream_info.channels;
        a->bits = (uint8_t)si.data.stream_info.bits_per_sample;
        a->total_frames = si.data.stream_info.total_samples;
        // Audio STREAMINFO is honest (audio-rate domain, true total): no
        // STREAMINFO patch (patch_total = 0 -> plain fread IO layer).
        gui_playback_io_reset(&a->io, file, a, 0);
        a->io.cancel = NULL;
        a->decoder = FLAC__stream_decoder_new();
        if (!a->decoder ||
            FLAC__stream_decoder_init_stream(
                a->decoder,
                gui_playback_io_read_cb, gui_playback_io_seek_cb,
                gui_playback_io_tell_cb, gui_playback_io_length_cb,
                gui_playback_io_eof_cb,
                gui_playback_audio_write_cb, gui_playback_audio_meta_cb,
                gui_playback_audio_error_cb, &a->io) != FLAC__STREAM_DECODER_INIT_STATUS_OK ||
            !FLAC__stream_decoder_process_until_end_of_metadata(a->decoder)) {
            snprintf(err, err_len, "failed to init the audio decoder: %s", filepath);
            if (a->decoder) {
                FLAC__stream_decoder_finish(a->decoder);
                FLAC__stream_decoder_delete(a->decoder);
                a->decoder = NULL;
            }
            fclose(file);
            goto fail;
        }
        a->file = file;  // owned here for the destroy path (IO layer reads it)
    } else {
        snprintf(err, err_len,
                 "unsupported audio container (%s) - use WAV or native FLAC",
                 gui_playback_input_format_name(fmt));
        goto fail;
    }

    a->stage = malloc((size_t)PLAYBACK_AUDIO_FEED_SPAN_MAX_FRAMES * 2 * sizeof(int16_t));
    a->out48 = malloc((size_t)PLAYBACK_AUDIO_OUT48_CAP * 2 * sizeof(int16_t));
    if (!a->stage || !a->out48) {
        snprintf(err, err_len, "out of memory for the audio feed buffers");
        goto fail;
    }
    a->stage_cap = PLAYBACK_AUDIO_FEED_SPAN_MAX_FRAMES;
    a->out48_cap = PLAYBACK_AUDIO_OUT48_CAP;
    a->out48_used = 0;
    a->fed_frames = 0;
    a->out_pos = 0.0;
    return a;

fail:
    // The pre-open failure paths fclosed `file` with a->file still NULL; the
    // post-open failures (staging malloc) leave a->file owned by `a` - the
    // destroy closes exactly the owned handle either way.
    gui_playback_audio_destroy(&a);
    return NULL;
}

// Decode the next `want` WAV frames at fed_frames into the stereo staging.
static size_t gui_playback_audio_wav_span(playback_audio_source_t *a, size_t want) {
    if (!a || !a->file || !a->stage || want == 0 || a->frame_bytes == 0) return 0;
    if (a->fed_frames >= a->total_frames) return 0;
    uint64_t remaining = a->total_frames - a->fed_frames;
    if (want > remaining) want = (size_t)remaining;
    if (want > a->stage_cap) want = a->stage_cap;
    if (!gui_playback_file_seek_abs(a->file,
            a->data_offset + a->fed_frames * (uint64_t)a->frame_bytes)) {
        return 0;
    }
    size_t got = 0;
    uint8_t buf[8192];
    while (got < want) {
        size_t frames = want - got;
        if (frames > sizeof(buf) / a->frame_bytes) frames = sizeof(buf) / a->frame_bytes;
        size_t items = fread(buf, a->frame_bytes, frames, a->file);
        if (items == 0) break;
        for (size_t i = 0; i < items; i++) {
            const uint8_t *b = buf + i * a->frame_bytes;
            int32_t l, r;
            switch (a->bits) {
                case 8: {
                    l = (int32_t)b[0] - 128;
                    r = (a->channels > 1) ? (int32_t)b[1] - 128 : l;
                    break;
                }
                case 16: {
                    l = (int32_t)(int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
                    r = (a->channels > 1)
                        ? (int32_t)(int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8))
                        : l;
                    break;
                }
                case 24: {
                    uint32_t ul = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
                    if (ul & 0x800000u) ul |= 0xFF000000u;
                    l = (int32_t)ul;
                    if (a->channels > 1) {
                        const uint8_t *c = b + 3;
                        uint32_t ur = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16);
                        if (ur & 0x800000u) ur |= 0xFF000000u;
                        r = (int32_t)ur;
                    } else {
                        r = l;
                    }
                    break;
                }
                default: {  // 32
                    uint32_t ul = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                  ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                    l = (ul & 0x80000000u)
                        ? (int32_t)(ul - 0x80000000u) + INT32_MIN : (int32_t)ul;
                    if (a->channels > 1) {
                        const uint8_t *c = b + 4;
                        uint32_t ur = (uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                                      ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
                        r = (ur & 0x80000000u)
                            ? (int32_t)(ur - 0x80000000u) + INT32_MIN : (int32_t)ur;
                    } else {
                        r = l;
                    }
                    break;
                }
            }
            a->stage[2 * got] = gui_playback_audio_to_i16(l, a->bits);
            a->stage[2 * got + 1] = gui_playback_audio_to_i16(r, a->bits);
            got++;
        }
    }
    return got;
}

// Decode the next `want` FLAC frames at fed_frames into the stereo staging
// (seek_absolute then process_single until the span is staged or EOF; the
// contiguous case skips the seek).
static size_t gui_playback_audio_flac_span(playback_audio_source_t *a, size_t want) {
    if (!a || !a->decoder || !a->stage || want == 0) return 0;
    if (a->fed_frames >= a->total_frames) return 0;
    if (want > a->stage_cap) want = a->stage_cap;
    if (a->fed_frames != a->flac_next) {
        if (!FLAC__stream_decoder_seek_absolute(a->decoder, a->fed_frames)) {
            return 0;
        }
    }
    a->staged = 0;
    a->want = want;
    while (a->staged < a->want &&
           FLAC__stream_decoder_get_state(a->decoder) != FLAC__STREAM_DECODER_END_OF_STREAM) {
        if (!FLAC__stream_decoder_process_single(a->decoder)) break;
    }
    a->flac_next = a->fed_frames + a->staged;
    return a->staged;
}

// Linear-resample the staged span [fed, fed+got) into the 48 k push buffer
// and push it. The step folds in the CURRENT playback speed: the source
// span covers got/(rate*mult) seconds of wall time, so producing exactly
// 48000 * that many frames keeps the queue at 48 k frames/s at EVERY speed
// (the pitch shifts with the speed, like a tape; slow motion plays pitched
// down, 2x/4x plays pitched up - never gaps, never queue overflow).
static void gui_playback_audio_push_span(playback_audio_source_t *a, size_t got,
                                         float speed_mult) {
    if (!a || !a->stage || !a->out48 || got == 0 || speed_mult <= 0.0f) return;
    double out_per_src = 48000.0 / ((double)a->rate_hz * (double)speed_mult);
    if (!(out_per_src > 0.0) || !isfinite(out_per_src)) return;
    double end = (double)(a->fed_frames + got);
    while (a->out_pos < end) {
        if (a->out48_used >= a->out48_cap) {
            gui_audio_push_playback(a->out48, a->out48_used);
            a->out48_used = 0;
        }
        double p = a->out_pos - (double)a->fed_frames;
        if (p < 0.0) p = 0.0;                 // re-anchor landed inside the span
        size_t i0 = (size_t)p;
        int16_t l, r;
        if (i0 >= got) {
            // Defensive: out_pos at/after the span end (rounding) - stop.
            break;
        }
        double frac = p - (double)i0;
        if (frac > 0.0 && i0 + 1 < got) {
            l = (int16_t)((double)a->stage[2 * i0] * (1.0 - frac) +
                          (double)a->stage[2 * (i0 + 1)] * frac);
            r = (int16_t)((double)a->stage[2 * i0 + 1] * (1.0 - frac) +
                          (double)a->stage[2 * (i0 + 1) + 1] * frac);
        } else {
            // Integral position or the span's last frame (the next sample
            // arrives with the next span): exact copy / hold.
            l = a->stage[2 * i0];
            r = a->stage[2 * i0 + 1];
        }
        a->out48[2 * a->out48_used] = l;
        a->out48[2 * a->out48_used + 1] = r;
        a->out48_used++;
        a->out_pos += out_per_src;
    }
    if (a->out48_used > 0) {
        gui_audio_push_playback(a->out48, a->out48_used);
        a->out48_used = 0;
    }
}

// Position-driven feed (called from the playback thread after each paced
// batch). target = playhead seconds x audio rate, so seeks/loops/speed/pause
// all follow the RF timeline; a jump (seek back, loop reset, big forward
// scrub) flushes the monitor queue and re-anchors the feed. Max speed skips
// audio (the timeline runs free); every other speed feeds smoothly.
static void gui_playback_audio_feed(playback_ctx_t *ctx) {
    playback_audio_source_t *a = ctx->audio;
    if (!a || a->exhausted || a->rate_hz == 0) return;
    if (atomic_load(&ctx->state) != PLAYBACK_STATE_PLAYING) return;
    playback_speed_t speed = (playback_speed_t)atomic_load(&ctx->speed);
    float mult = speed_multipliers[speed];
    if (mult <= 0.0f) return;  // Max: the timeline runs free; no audio
    double rate = ctx->realtime_sample_rate_hz > 0.0 ? ctx->realtime_sample_rate_hz : 1.0;
    double pos_s = (double)atomic_load(&ctx->current_sample) / rate;
    uint64_t target = (uint64_t)(pos_s * (double)a->rate_hz);
    if (target >= a->total_frames) return;  // audio shorter than the RF timeline
    uint64_t slack_back = (uint64_t)a->rate_hz / 20u;   // 50 ms behind tolerance
    uint64_t slack_fwd = (uint64_t)a->rate_hz * 4u;     // 4 s catch-up bound
    if (a->fed_frames > target + slack_back || target > a->fed_frames + slack_fwd) {
        // Seek/loop/speed jump: drop the stale queued audio, re-anchor. A
        // loop restart (or a seek back before an exhausted EOF) resumes the
        // audio from the new playhead.
        gui_audio_reset_playback_queue();
        a->fed_frames = target;
        a->out_pos = (double)target;
        a->exhausted = false;
    }
    uint64_t lookahead = (uint64_t)a->rate_hz * (uint64_t)PLAYBACK_AUDIO_FEED_LOOKAHEAD_MS / 1000u;
    if (lookahead == 0) lookahead = 1;
    if (target + lookahead > a->total_frames) lookahead = a->total_frames - target;
    while (a->fed_frames < target + lookahead) {
        uint64_t want64 = target + lookahead - a->fed_frames;
        size_t want = want64 > (uint64_t)PLAYBACK_AUDIO_FEED_SPAN_MAX_FRAMES
                    ? PLAYBACK_AUDIO_FEED_SPAN_MAX_FRAMES : (size_t)want64;
        size_t got = a->is_wav ? gui_playback_audio_wav_span(a, want)
                               : gui_playback_audio_flac_span(a, want);
        gui_playback_audio_push_span(a, got, mult);
        a->fed_frames += got;
        if (got < want) {
            // EOF (or a failed seek) mid-span: stop feeding for this session
            // (the queue drains to silence; the RF timeline keeps playing).
            a->exhausted = true;
            break;
        }
    }
}

// The drop zone: the union of each channel's cached panel bounds (logical
// coordinates, refreshed every render by render_channel_panels); a drop
// outside the canvases (VU/stats/timeline rows) takes the vertically nearest
// channel row.
static Rectangle gui_playback_drop_zone_rect(const channel_panel_config_t *cfg) {
    Rectangle r = cfg->left_bounds;
    if (cfg->split) {
        Rectangle rr = cfg->right_bounds;
        if (rr.width > 0 && rr.height > 0) {
            if (r.width <= 0 || r.height <= 0) {
                r = rr;
            } else {
                float x0 = r.x < rr.x ? r.x : rr.x;
                float y0 = r.y < rr.y ? r.y : rr.y;
                float x1 = (r.x + r.width) > (rr.x + rr.width) ? (r.x + r.width) : (rr.x + rr.width);
                float y1 = (r.y + r.height) > (rr.y + rr.height) ? (r.y + r.height) : (rr.y + rr.height);
                r.x = x0; r.y = y0; r.width = x1 - x0; r.height = y1 - y0;
            }
        }
    }
    return r;
}

static int gui_playback_drop_zone(gui_app_t *app, Vector2 point) {
    if (!app) return 0;
    Rectangle ra = gui_playback_drop_zone_rect(&app->panel_config_a);
    Rectangle rb = gui_playback_drop_zone_rect(&app->panel_config_b);
    bool have_a = ra.width > 0 && ra.height > 0;
    bool have_b = rb.width > 0 && rb.height > 0;
    if (have_a && CheckCollisionPointRec(point, ra)) return 0;
    if (have_b && CheckCollisionPointRec(point, rb)) return 1;
    if (!have_a && !have_b) return 0;  // no rendered bounds yet (first frame)
    if (!have_a) return 1;
    if (!have_b) return 0;
    float ca = ra.y + ra.height * 0.5f;
    float cb = rb.y + rb.height * 0.5f;
    float da = point.y - ca;
    float db = point.y - cb;
    return (da * da) <= (db * db) ? 0 : 1;
}

typedef enum {
    PLAYBACK_DROP_REJECT = 0,
    PLAYBACK_DROP_RF,
    PLAYBACK_DROP_AUDIO,
} playback_drop_kind_t;

// Classify one dropped file by CONTENT. Audio-rate -> the monitor source
// slot; RF-domain -> the zone channel; unsupported -> REJECT with the
// sniffer/probe's specific reason. The rate domains are unambiguous (RF
// files resolve to MHz via headers/tags/hints; audio <= 384 k).
static playback_drop_kind_t gui_playback_classify_drop(const char *filepath,
                                                       char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    playback_input_format_t fmt = PLAYBACK_INPUT_FLAC;
    uint8_t magic[4] = {0};
    if (!gui_playback_sniff_format(filepath, &fmt, magic, err, err_len)) {
        return PLAYBACK_DROP_REJECT;
    }
    if (fmt == PLAYBACK_INPUT_WAV) {
        FILE *file = file_open_read_utf8(filepath);
        if (!file) {
            snprintf(err, err_len, "open failed: %s", filepath);
            return PLAYBACK_DROP_REJECT;
        }
        uint64_t file_size = 0, off = 0, span = 0;
        uint32_t rate = 0, bits = 0;
        uint16_t channels = 0;
        bool ok = PLAYBACK_FSEEK(file, 0, SEEK_END) == 0 &&
                  gui_playback_file_tell(file, &file_size) &&
                  gui_playback_file_seek_abs(file, 0) &&
                  gui_playback_parse_wav(file, file_size, &rate, &bits, &channels,
                                         &off, &span, err, err_len) && span > 0;
        fclose(file);
        if (!ok) {
            if (err && err[0] == '\0') {
                snprintf(err, err_len, "WAV has no audio data: %s", filepath);
            }
            return PLAYBACK_DROP_REJECT;
        }
        double real_hz = (gui_playback_is_audio_sample_rate(rate) || rate > 1000000u)
                         ? (double)rate : (double)rate * 1000.0;
        return (real_hz > 0.0 && real_hz <= (double)PLAYBACK_AUDIO_CLASSIFY_MAX_HZ)
                   ? PLAYBACK_DROP_AUDIO : PLAYBACK_DROP_RF;
    }
    if (fmt == PLAYBACK_INPUT_FLAC) {
        FLAC__StreamMetadata si;
        if (!FLAC__metadata_get_streaminfo(filepath, &si)) {
            snprintf(err, err_len, "not a valid FLAC stream: %s", filepath);
            return PLAYBACK_DROP_REJECT;
        }
        double real_hz = gui_playback_resolve_real_rate_hz(
            filepath, si.data.stream_info.sample_rate, NULL);
        if (real_hz > 0.0 && real_hz <= (double)PLAYBACK_AUDIO_CLASSIFY_MAX_HZ) {
            if (si.data.stream_info.total_samples == 0) {
                snprintf(err, err_len, "audio FLAC has an unknown/zero length: %s", filepath);
                return PLAYBACK_DROP_REJECT;
            }
            if (si.data.stream_info.channels == 0 || si.data.stream_info.channels > 8) {
                snprintf(err, err_len, "audio FLAC has %u channels (expected 1-8)",
                         si.data.stream_info.channels);
                return PLAYBACK_DROP_REJECT;
            }
            return PLAYBACK_DROP_AUDIO;
        }
        if (si.data.stream_info.channels != 1) {
            snprintf(err, err_len,
                     "FLAC has %u channels (RF playback needs mono)",
                     si.data.stream_info.channels);
            return PLAYBACK_DROP_REJECT;
        }
        return PLAYBACK_DROP_RF;
    }
    if (fmt == PLAYBACK_INPUT_OGG_FLAC) {
        playback_file_info_t info = {0};
        if (!gui_playback_ogg_probe(&info, filepath, err, err_len)) {
            return PLAYBACK_DROP_REJECT;
        }
        if (info.real_sample_rate_hz > 0.0 &&
            info.real_sample_rate_hz <= (double)PLAYBACK_AUDIO_CLASSIFY_MAX_HZ) {
            snprintf(err, err_len,
                     "Ogg FLAC audio is not supported as the audio source - use WAV or native FLAC");
            return PLAYBACK_DROP_REJECT;
        }
        return PLAYBACK_DROP_RF;
    }
    // Headerless raw PCM widths + packed .lds: RF domain by definition.
    return PLAYBACK_DROP_RF;
}

#endif // LIBFLAC_ENABLED

bool gui_playback_handle_dropped_files(gui_app_t *app, const char *const *paths,
                                       size_t count, float point_x, float point_y) {
    if (!app || !paths || count == 0) return false;
#if LIBFLAC_ENABLED == 1
    if (gui_playback_is_starting(app)) {
        gui_app_set_status(app, "Playback is still loading - wait for it to finish");
        return false;
    }
    Vector2 point = { point_x, point_y };
    int zone = gui_playback_drop_zone(app, point);
    const char *rf_a = NULL, *rf_b = NULL, *audio_file = NULL;
    size_t rejected = 0, extras = 0;
    for (size_t i = 0; i < count; i++) {
        const char *p = paths[i];
        if (!p || !p[0]) continue;
        char err[192] = {0};
        playback_drop_kind_t kind = gui_playback_classify_drop(p, err, sizeof(err));
        if (kind == PLAYBACK_DROP_AUDIO) {
            if (!audio_file) audio_file = p;
            else extras++;
        } else if (kind == PLAYBACK_DROP_RF) {
            if (zone == 0) {
                if (!rf_a) rf_a = p;
                else extras++;
            } else {
                if (!rf_b) rf_b = p;
                else extras++;
            }
        } else {
            rejected++;
            fprintf(stderr, "[PLAYBACK] Drop rejected: %s (%s)\n",
                    err[0] ? err : "unsupported file", p);
        }
    }
    if (!rf_a && !rf_b && !audio_file) {
        gui_app_set_status(app, "No playable files in the drop (see the log)");
        return false;
    }
    // Only slots that received a file this drop change (a B-only drop keeps
    // the loaded A; a set drop replaces both).
    if (rf_a) snprintf(app->settings.playback_file_a, sizeof(app->settings.playback_file_a), "%s", rf_a);
    if (rf_b) snprintf(app->settings.playback_file_b, sizeof(app->settings.playback_file_b), "%s", rf_b);
    if (audio_file) {
        snprintf(app->settings.playback_audio_file, sizeof(app->settings.playback_audio_file),
                 "%s", audio_file);
    }
    gui_settings_save(&app->settings);
    fprintf(stderr,
            "[PLAYBACK] Drop: zone=%c A=%s B=%s audio=%s (%zu rejected, %zu extra)\n",
            zone == 0 ? 'A' : 'B',
            app->settings.playback_file_a[0] ? app->settings.playback_file_a : "(keep)",
            app->settings.playback_file_b[0] ? app->settings.playback_file_b : "(keep)",
            app->settings.playback_audio_file[0] ? app->settings.playback_audio_file : "(none)",
            rejected, extras);
    // Select the Playback device; stop anything running (capture or an
    // earlier playback); (re)start through the normal start path.
    int pb = -1;
    for (int i = 0; i < app->device_count; i++) {
        if (app->devices[i].type == DEVICE_TYPE_PLAYBACK) {
            pb = i;
            break;
        }
    }
    if (pb < 0) {
        gui_app_set_status(app, "No playback device available");
        return false;
    }
    app->selected_device = pb;
    if (app->is_capturing || gui_playback_is_running(app)) {
        gui_app_stop_capture(app);
    }
    if (app->settings.playback_file_a[0] || app->settings.playback_file_b[0]) {
        // start_async sets "Loading playback..."; the commit sets "Playback started".
        return gui_app_start_capture(app) == 0;
    }
    gui_app_set_status(app, "Audio loaded - drop RF files on a channel panel to play");
    return true;
#else
    (void)paths; (void)count; (void)point_x; (void)point_y;
    gui_app_set_status(app, "Playback drag-and-drop needs FLAC support (not compiled in)");
    return false;
#endif
}

//-----------------------------------------------------------------------------
// Playback Thread
//-----------------------------------------------------------------------------

// Raw buffer write size (must match extraction thread's expected read size)
#define RAW_BUFFER_SAMPLES 65536
#define RAW_BUFFER_BYTES (RAW_BUFFER_SAMPLES * sizeof(uint32_t))
// Paused-scrub backfill window. DURATION-based (sample rates vary across
// files: 5/10/20/40 MSPS): converted to samples via the channel's resolved
// real rate, capped by the spectrogram panel's slice capacity (one display
// frame per batch = one history slice, 256 slices = SPEC_TIME_BINS in
// gui_spectrogram.c; keep in sync) and floored at one batch.
#define PLAYBACK_PREVIEW_WINDOW_SECONDS 3.0
#define PLAYBACK_PREVIEW_WINDOW_BATCHES 256

#if LIBFLAC_ENABLED == 1
// libFLAC seek synchronously supplies the exact target slice to write_cb.
static bool gui_playback_seek_attempt(playback_ctx_t *ctx, int channel, uint64_t target) {
    FLAC__StreamDecoder *decoder = channel == 1 ? ctx->decoder_b : ctx->decoder_a;
    if (!decoder) return false;
    if (channel == 1) { ctx->decode_available_b = 0; ctx->decode_pos_b = 0; }
    else { ctx->decode_available_a = 0; ctx->decode_pos_a = 0; }
    if (!FLAC__stream_decoder_seek_absolute(decoder, target)) return false;
    FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(decoder);
    size_t available = channel == 1 ? ctx->decode_available_b : ctx->decode_available_a;
    return state == FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC && available > 0;
}

// A provisional endpoint may be too long. Never leave SEEK_ERROR, an empty
// apparent success, or a made-up target cursor behind. Flush retains immutable
// STREAMINFO and avoids re-running metadata callbacks on active UI-owned info.
static bool gui_playback_seek_decoder(playback_ctx_t *ctx, int channel, uint64_t target) {
    FLAC__StreamDecoder *decoder = channel == 1 ? ctx->decoder_b : ctx->decoder_a;
    if (!decoder) return false;
    uint64_t previous = atomic_load(channel == 1 ? &ctx->current_sample_b : &ctx->current_sample_a);
    bool empty = channel == 1 ? ctx->decode_available_b == ctx->decode_pos_b : ctx->decode_available_a == ctx->decode_pos_a;
    FLAC__uint64 boundary = 0;
    bool previous_eof = empty && (FLAC__stream_decoder_get_state(decoder) == FLAC__STREAM_DECODER_END_OF_STREAM ||
        (ctx->loaded_key_valid[channel] && FLAC__stream_decoder_get_decode_position(decoder, &boundary) &&
         boundary == ctx->loaded_key[channel].file_size));
    ctx->seek_recovery_failed[channel] = false;
    if (gui_playback_seek_attempt(ctx, channel, target)) return true;
    bool restored = FLAC__stream_decoder_flush(decoder) &&
        gui_playback_seek_attempt(ctx, channel, previous_eof && previous ? previous - 1 : previous);
    if (restored && previous_eof) {
        size_t available = channel == 1 ? ctx->decode_available_b : ctx->decode_available_a;
        // Saved EOF has no buffered samples. Its last real sample must be the
        // sole callback slice, then process exactly to the real endpoint.
        restored = previous > 0 && available == 1;
        if (channel == 1) { ctx->decode_available_b = 0; ctx->decode_pos_b = 0; }
        else { ctx->decode_available_a = 0; ctx->decode_pos_a = 0; }
        restored = restored && FLAC__stream_decoder_process_single(decoder) &&
            FLAC__stream_decoder_get_state(decoder) == FLAC__STREAM_DECODER_END_OF_STREAM;
    }
    if (!restored) {
        if (channel == 1) { ctx->decode_available_b = 0; ctx->decode_pos_b = 0; }
        else { ctx->decode_available_a = 0; ctx->decode_pos_a = 0; }
    }
    ctx->seek_recovery_failed[channel] = !restored;
    return false;
}
#endif
static uint64_t gui_playback_clock_us(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&counter);
    uint64_t hz = (uint64_t)frequency.QuadPart, ticks = (uint64_t)counter.QuadPart;
    return ticks/hz*1000000u + ticks%hz*1000000u/hz;
#else
    return get_time_us();
#endif
}
static void gui_playback_wait_clock(playback_ctx_t *ctx, uint64_t deadline, void *timer) {
    while (atomic_load(&ctx->running)) {
        uint64_t now = gui_playback_clock_us();
        if (now >= deadline) break;
        uint64_t remain = deadline-now;
        if (remain > 10000) remain = 10000;
#ifdef _WIN32
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(remain*10u);
            if (SetWaitableTimer((HANDLE)timer,&due,0,NULL,NULL,FALSE)) {
                WaitForSingleObject((HANDLE)timer,20); continue;
            }
        }
        if (remain > 1500) Sleep((DWORD)(remain/1000u)); else Sleep(0);
#else
        (void)timer;
        struct timespec delay = {0,(long)(remain*1000u)};
        nanosleep(&delay,NULL);
#endif
    }
}
static int playback_thread_func(void *ctx_ptr) {
    playback_ctx_t *ctx = (playback_ctx_t *)ctx_ptr;
    gui_app_t *app = ctx->app;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    fprintf(stderr, "[PLAYBACK] Playback thread started (writing to BUF_CAPTURE_RF)\n");

    // Allocate decode buffers for FLAC output
    int16_t *buf_a = (int16_t *)malloc(RAW_BUFFER_SAMPLES * sizeof(int16_t));
    int16_t *buf_b = (int16_t *)malloc(RAW_BUFFER_SAMPLES * sizeof(int16_t));

    if (!buf_a || !buf_b) {
        fprintf(stderr, "[PLAYBACK] Failed to allocate output buffers\n");
        free(buf_a);
        free(buf_b);
        atomic_store(&ctx->state, PLAYBACK_STATE_STOPPED);
        return -1;
    }

    atomic_store(&app->stream_synced, true);
    uint32_t runtime_rate_hz = (uint32_t)llround(ctx->realtime_sample_rate_hz);
    if (runtime_rate_hz == 0) runtime_rate_hz = PLAYBACK_SAMPLE_RATE * 1000u;
    atomic_store(&app->sample_rate, runtime_rate_hz);

    uint64_t batch_count = 0;
    uint64_t epochs[2] = {atomic_load(&app->rf_stream_epoch), atomic_load(&app->rf_stream_epoch)};
    const uint32_t raw_silence = encode_raw_sample(0, 0);
    double deadline_us = 0;
    playback_speed_t clock_speed = PLAYBACK_SPEED_MAX;
    void *pace_timer = NULL;
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    pace_timer = CreateWaitableTimerEx(NULL,NULL,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
#endif

    while (atomic_load(&ctx->running)) {
        // Check for seek requests (must work even while paused)
        if (atomic_load(&ctx->seek_requested_a)) {
            uint64_t target = atomic_load(&ctx->seek_target_a);
            uint64_t total_a = atomic_load(&ctx->channel_total_samples[0]);
            if (total_a > 0 && target >= total_a) {
                target = total_a - 1;
            }
            atomic_store(&ctx->seek_requested_a, false);

        // 08.2024 - Windows no flac output issue fix
            bool paused = atomic_load(&ctx->state) == PLAYBACK_STATE_PAUSED;
            uint64_t seek_target = target;
#if LIBFLAC_ENABLED == 1
            if (paused) {
                uint64_t window = (uint64_t)(PLAYBACK_PREVIEW_WINDOW_SECONDS * ctx->channel_rate_hz[0]);
                uint64_t cap = (uint64_t)PLAYBACK_PREVIEW_WINDOW_BATCHES * RAW_BUFFER_SAMPLES;
                if (window > cap) window = cap;
                if (window < RAW_BUFFER_SAMPLES) window = RAW_BUFFER_SAMPLES;
                seek_target = target > window ? target - window : 0;
            }
            bool seek_ok;
            if (ctx->input_is_raw[0]) {
                // Raw readers position exactly at the target sample (the raw
                // analogue of the FLAC seek attempt's exact-target slice;
                // totals are exact so no provisional-endpoint validation).
                seek_ok = gui_playback_raw_seek(ctx, 0, seek_target);
                if (seek_ok) {
                    ctx->decode_available_a = 0;
                    ctx->decode_pos_a = 0;
                }
            } else {
                // Validate the requested playhead before seeking its backfill
                // window: a provisional endpoint can exceed the real stream.
                seek_ok = !((paused && seek_target != target &&
                             !gui_playback_seek_decoder(ctx, 0, target)) ||
                            !gui_playback_seek_decoder(ctx, 0, seek_target));
            }
            if (!seek_ok) {
                fprintf(stderr,"[PLAYBACK] Channel A seek failed at sample %llu\n",(unsigned long long)target);
                if (ctx->seek_recovery_failed[0]) atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED);
                deadline_us = 0; continue;
            }
#endif
            fprintf(stderr, "[DBGA] seek store %llu (state=%d)\n", (unsigned long long)seek_target, (int)atomic_load(&ctx->state));
            atomic_store(&ctx->current_sample_a, seek_target);
            atomic_fetch_add(&ctx->seek_seq_a, 1);
            ctx->backfill_active_a = paused;
            ctx->backfill_target_a = target;
            epochs[0] = atomic_fetch_add(&app->rf_stream_epoch, 1) + 1;
            deadline_us = 0;
            if (!ctx->file_b) {
                atomic_store(&ctx->current_sample, seek_target);
            }
            continue;
        }

        if (atomic_load(&ctx->seek_requested_b)) {
            uint64_t target = atomic_load(&ctx->seek_target_b);
            uint64_t total_b = atomic_load(&ctx->channel_total_samples[1]);
            if (total_b > 0 && target >= total_b) {
                target = total_b - 1;
            }
            atomic_store(&ctx->seek_requested_b, false);

            bool paused = atomic_load(&ctx->state) == PLAYBACK_STATE_PAUSED;
            uint64_t seek_target = target;
#if LIBFLAC_ENABLED == 1
            if (paused) {
                uint64_t window = (uint64_t)(PLAYBACK_PREVIEW_WINDOW_SECONDS * ctx->channel_rate_hz[1]);
                uint64_t cap = (uint64_t)PLAYBACK_PREVIEW_WINDOW_BATCHES * RAW_BUFFER_SAMPLES;
                if (window > cap) window = cap;
                if (window < RAW_BUFFER_SAMPLES) window = RAW_BUFFER_SAMPLES;
                seek_target = target > window ? target - window : 0;
            }
            bool seek_ok;
            if (ctx->input_is_raw[1]) {
                seek_ok = gui_playback_raw_seek(ctx, 1, seek_target);
                if (seek_ok) {
                    ctx->decode_available_b = 0;
                    ctx->decode_pos_b = 0;
                }
            } else {
                seek_ok = !((paused && seek_target != target &&
                             !gui_playback_seek_decoder(ctx, 1, target)) ||
                            !gui_playback_seek_decoder(ctx, 1, seek_target));
            }
            if (!seek_ok) {
                fprintf(stderr,"[PLAYBACK] Channel B seek failed at sample %llu\n",(unsigned long long)target);
                if (ctx->seek_recovery_failed[1]) atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED);
                deadline_us = 0; continue;
            }
#endif
            atomic_store(&ctx->current_sample_b, seek_target);
            atomic_fetch_add(&ctx->seek_seq_b, 1);
            ctx->backfill_active_b = paused;
            ctx->backfill_target_b = target;
            epochs[1] = atomic_fetch_add(&app->rf_stream_epoch, 1) + 1;
            deadline_us = 0;
            if (!ctx->file_a) {
                atomic_store(&ctx->current_sample, seek_target);
            }
            continue;
        }

        // Check for pause state. While holding (no batches written), keep
        // the no-data watchdog fed: gui_capture_device_timeout (2 s) would
        // otherwise fire auto-reconnect "Connection lost" and tear the
        // playback session down — seen as playback "crashing" and resetting
        // a couple of seconds after pausing.
        // A paused scrub keeps the backfill running (window -> playhead),
        // falling through the hold until the position passes the target.
        if (atomic_load(&ctx->state) == PLAYBACK_STATE_PAUSED) {
            if (ctx->backfill_active_a && ctx->file_a &&
                atomic_load(&ctx->current_sample_a) > ctx->backfill_target_a) {
                ctx->backfill_active_a = false;
                fprintf(stderr, "[PLAYBACK] CH A backfill complete at %" PRIu64 "\n",
                        atomic_load(&ctx->current_sample_a));
            }
            if (ctx->backfill_active_b && ctx->file_b &&
                atomic_load(&ctx->current_sample_b) > ctx->backfill_target_b) {
                ctx->backfill_active_b = false;
                fprintf(stderr, "[PLAYBACK] CH B backfill complete at %" PRIu64 "\n",
                        atomic_load(&ctx->current_sample_b));
            }
            if (!ctx->backfill_active_a && !ctx->backfill_active_b) {
                deadline_us = 0;
                atomic_store(&app->last_callback_time_ms, get_time_ms());
                thrd_sleep_ms(10);
                continue;
            }
        }

        // A paused scrub emits only its target channel. Leave the other
        // decoder, cursor and buffered samples untouched, including at EOF.
        bool paused_output = atomic_load(&ctx->state) == PLAYBACK_STATE_PAUSED;
        bool channel_a_active = ctx->file_a && (!paused_output || ctx->backfill_active_a);
        bool channel_b_active = ctx->file_b && (!paused_output || ctx->backfill_active_b);
        if (!channel_a_active && !channel_b_active) {
            deadline_us = 0;
            atomic_store(&app->last_callback_time_ms, get_time_ms());
            thrd_sleep_ms(10);
            continue;
        }

        // Decode more samples if needed
#if LIBFLAC_ENABLED == 1
        // Decode channel A
        while (channel_a_active && ctx->decoder_a && ctx->decode_available_a - ctx->decode_pos_a < RAW_BUFFER_SAMPLES) {
            if (FLAC__stream_decoder_get_state(ctx->decoder_a) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(ctx->decoder_a)) {
                break;
            }
        }

        // Raw readers (u8/s8/u16/s16/.lds/WAV): convert bytes straight into
        // the decode buffer; these channels have no libFLAC decoder. Reads
        // stop exactly at the size-derived total (the exact audio span).
        if (channel_a_active && ctx->input_is_raw[0]) {
            while (ctx->decode_available_a - ctx->decode_pos_a < RAW_BUFFER_SAMPLES) {
                size_t space = ctx->decode_buf_size - ctx->decode_available_a;
                size_t got = gui_playback_raw_read(ctx, 0,
                                                   ctx->decode_buf_a + ctx->decode_available_a, space);
                ctx->decode_available_a += got;
                if (got == 0) break;
            }
        }

        // Decode channel B
        while (channel_b_active && ctx->decoder_b && ctx->decode_available_b - ctx->decode_pos_b < RAW_BUFFER_SAMPLES) {
            if (FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(ctx->decoder_b)) {
                break;
            }
        }

        if (channel_b_active && ctx->input_is_raw[1]) {
            while (ctx->decode_available_b - ctx->decode_pos_b < RAW_BUFFER_SAMPLES) {
                size_t space = ctx->decode_buf_size - ctx->decode_available_b;
                size_t got = gui_playback_raw_read(ctx, 1,
                                                   ctx->decode_buf_b + ctx->decode_available_b, space);
                ctx->decode_available_b += got;
                if (got == 0) break;
            }
        }
#endif

        // Calculate how many samples we can output
        size_t avail_a = ctx->decode_available_a - ctx->decode_pos_a;
        size_t avail_b = ctx->decode_available_b - ctx->decode_pos_b;
        size_t samples_to_output = RAW_BUFFER_SAMPLES;

        // Check for EOF on each channel independently and loop each one
#if LIBFLAC_ENABLED == 1
        // Raw channels end when their reader consumed the exact size-derived
        // total; FLAC channels when the decoder reached end-of-stream.
        bool eof_a = channel_a_active && avail_a == 0 &&
                      ((ctx->input_is_raw[0] && gui_playback_raw_eof(ctx, 0)) ||
                       (ctx->decoder_a != NULL &&
                        FLAC__stream_decoder_get_state(ctx->decoder_a) == FLAC__STREAM_DECODER_END_OF_STREAM));
        bool eof_b = channel_b_active && avail_b == 0 &&
                      ((ctx->input_is_raw[1] && gui_playback_raw_eof(ctx, 1)) ||
                       (ctx->decoder_b != NULL &&
                        FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM));

        if ((eof_a || eof_b) && paused_output) {
            // EOF terminates a paused analysis backfill; it must not loop.
            if (eof_a) ctx->backfill_active_a = false;
            if (eof_b) ctx->backfill_active_b = false;
            deadline_us = 0;
            continue;
        }
        if ((eof_a || eof_b) && !atomic_load(&ctx->loop_enabled)) {
            atomic_store(&ctx->state, PLAYBACK_STATE_PAUSED);
            continue; // Seek remains available; no samples cross the EOF boundary.
        }
        // Loop channel A independently
        if (eof_a) {
            fprintf(stderr, "[PLAYBACK] Channel A EOF, looping\n");
            bool loop_ok = ctx->input_is_raw[0] ? gui_playback_raw_seek(ctx, 0, 0)
                                                : gui_playback_seek_decoder(ctx, 0, 0);
            if (loop_ok && ctx->input_is_raw[0]) {
                ctx->decode_available_a = 0;
                ctx->decode_pos_a = 0;
            }
            if (!loop_ok) {
                fprintf(stderr,"[PLAYBACK] Channel A loop seek failed\n");
                atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED); continue;
            }
            fprintf(stderr, "[DBGA] loop store 0\n");
            atomic_store(&ctx->current_sample_a, 0);
            epochs[0] = atomic_fetch_add(&app->rf_stream_epoch, 1) + 1;
            deadline_us = 0;
        }

        // Loop channel B independently (a paused backfill that reaches the
        // file end just stops the backfill instead of looping).
        if (eof_b) {
            fprintf(stderr, "[PLAYBACK] Channel B EOF, looping\n");
            bool loop_ok = ctx->input_is_raw[1] ? gui_playback_raw_seek(ctx, 1, 0)
                                                : gui_playback_seek_decoder(ctx, 1, 0);
            if (loop_ok && ctx->input_is_raw[1]) {
                ctx->decode_available_b = 0;
                ctx->decode_pos_b = 0;
            }
            if (!loop_ok) {
                fprintf(stderr,"[PLAYBACK] Channel B loop seek failed\n");
                atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED); continue;
            }
            atomic_store(&ctx->current_sample_b, 0);
            epochs[1] = atomic_fetch_add(&app->rf_stream_epoch, 1) + 1;
            deadline_us = 0;
        }

        // If either channel just looped, continue to refill buffers
        if (eof_a || eof_b) {
            continue;
        }
#endif

        // Limit output to available samples
        if (channel_a_active && avail_a < samples_to_output) {
            samples_to_output = avail_a;
        }
        if (channel_b_active && avail_b < samples_to_output) {
            samples_to_output = avail_b;
        }

        if (samples_to_output == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        // Fill output buffers
        if (channel_a_active && avail_a > 0) {
            size_t to_copy = (avail_a < samples_to_output) ? avail_a : samples_to_output;
            memcpy(buf_a, ctx->decode_buf_a + ctx->decode_pos_a, to_copy * sizeof(int16_t));
            ctx->decode_pos_a += to_copy;
            // Zero-pad if needed
            if (to_copy < samples_to_output) {
                memset(buf_a + to_copy, 0, (samples_to_output - to_copy) * sizeof(int16_t));
            }
        } else {
            memset(buf_a, 0, samples_to_output * sizeof(int16_t));
        }

        if (channel_b_active && avail_b > 0) {
            size_t to_copy = (avail_b < samples_to_output) ? avail_b : samples_to_output;
            memcpy(buf_b, ctx->decode_buf_b + ctx->decode_pos_b, to_copy * sizeof(int16_t));
            ctx->decode_pos_b += to_copy;
            if (to_copy < samples_to_output) {
                memset(buf_b + to_copy, 0, (samples_to_output - to_copy) * sizeof(int16_t));
            }
        } else {
            memset(buf_b, 0, samples_to_output * sizeof(int16_t));
        }

        // Compact decode buffers if needed (shift remaining data to start)
        if (channel_a_active && ctx->decode_pos_a > ctx->decode_buf_size / 2) {
            size_t remaining = ctx->decode_available_a - ctx->decode_pos_a;
            if (remaining > 0) {
                memmove(ctx->decode_buf_a, ctx->decode_buf_a + ctx->decode_pos_a,
                        remaining * sizeof(int16_t));
            }
            ctx->decode_available_a = remaining;
            ctx->decode_pos_a = 0;
        }
        if (channel_b_active && ctx->decode_pos_b > ctx->decode_buf_size / 2) {
            size_t remaining = ctx->decode_available_b - ctx->decode_pos_b;
            if (remaining > 0) {
                memmove(ctx->decode_buf_b, ctx->decode_buf_b + ctx->decode_pos_b,
                        remaining * sizeof(int16_t));
            }
            ctx->decode_available_b = remaining;
            ctx->decode_pos_b = 0;
        }

        // Encode decoded samples to raw 32-bit format and write to BUF_CAPTURE_RF
        // The extraction thread will read this, update statistics, display, CVBS, and recording
        uint32_t *raw_buf = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                                           RAW_BUFFER_BYTES, NULL);
        if (raw_buf) {
            // Encode int16 samples to raw format
            for (size_t i = 0; i < samples_to_output; i++) {
                raw_buf[i] = encode_raw_sample(buf_a[i], buf_b[i]);
            }
            for (size_t i = samples_to_output; i < RAW_BUFFER_SAMPLES; i++) {
                raw_buf[i] = raw_silence;
            }
            gui_rf_block_info_t info[2] = {
                {.sample_rate_hz = atomic_load(&app->rf_source_rate_hz[0]),
                 .start_sample = atomic_load(&ctx->current_sample_a), .epoch = epochs[0]},
                {.sample_rate_hz = atomic_load(&app->rf_source_rate_hz[1]),
                 .start_sample = atomic_load(&ctx->current_sample_b), .epoch = epochs[1]}
            };
            size_t counts[2] = {channel_a_active ? samples_to_output : 0,
                                channel_b_active ? samples_to_output : 0};
            if (gui_extract_stamp_playback(info, counts)) {
                bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, RAW_BUFFER_BYTES);
            } else {
                fprintf(stderr, "[PLAYBACK] RF stamp queue full; uncommitted block discarded\n");
            }
        } else {
            // Buffer full - this shouldn't happen often with proper sizing
            // The extraction thread may be slow or not running
            fprintf(stderr, "[PLAYBACK] Warning: BUF_CAPTURE_RF full, frame dropped\n");
        }

        // Update playback position (extraction thread updates total_samples, etc.)
        // Keep the endpoint visible until an actual loop seek resets it. A
        // newly published length must not turn an EOF position into zero.
        if (channel_a_active) {
            uint64_t next_a = atomic_load(&ctx->current_sample_a) + (uint64_t)samples_to_output;
            fprintf(stderr, "[DBGA] output store %llu (state=%d)\n", (unsigned long long)next_a, (int)atomic_load(&ctx->state));
            atomic_store(&ctx->current_sample_a, next_a);
        }
        if (channel_b_active) {
            uint64_t next_b = atomic_load(&ctx->current_sample_b) + (uint64_t)samples_to_output;
            atomic_store(&ctx->current_sample_b, next_b);
        }
        if (channel_a_active) {
            atomic_store(&ctx->current_sample, atomic_load(&ctx->current_sample_a));
        } else if (channel_b_active) {
            atomic_store(&ctx->current_sample, atomic_load(&ctx->current_sample_b));
        } else {
            atomic_fetch_add(&ctx->current_sample, samples_to_output);
        }
        atomic_store(&app->last_callback_time_ms, get_time_ms());

#if LIBFLAC_ENABLED == 1
        // Position-synced monitor audio (the drag-and-drop source): follows
        // the playhead through seeks/loops/speed changes; a jump flushes the
        // monitor queue (see the drag-and-drop section above).
        gui_playback_audio_feed(ctx);
#endif

        batch_count++;

        // Accumulate an absolute source-sample clock. A fractional millisecond
        // block (80 MHz) retains its duration; speed/pause/seek changes re-anchor.
        playback_speed_t speed = atomic_load(&ctx->speed);
        bool backfilling = atomic_load(&ctx->state) == PLAYBACK_STATE_PAUSED &&
                           (ctx->backfill_active_a || ctx->backfill_active_b);
        if (speed == PLAYBACK_SPEED_MAX || backfilling) deadline_us = 0;
        else {
            float multiplier = speed_multipliers[speed];
            if (multiplier > 0) {
                uint64_t now = gui_playback_clock_us();
                if (!deadline_us || clock_speed != speed || deadline_us+100000.0 < now) deadline_us = (double)now;
                deadline_us += (double)samples_to_output*1000000.0/ctx->realtime_sample_rate_hz/multiplier;
                gui_playback_wait_clock(ctx,(uint64_t)deadline_us,pace_timer);
            }
        }
        clock_speed = speed;
    }

    fprintf(stderr, "[PLAYBACK] Playback thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

#ifdef _WIN32
    if (pace_timer) CloseHandle((HANDLE)pace_timer);
#endif
    free(buf_a);
    free(buf_b);

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

static void gui_playback_update_length(playback_ctx_t *ctx) {
    ctx->total_samples = ctx->info_a.total_samples;
    if (ctx->info_b.total_samples > ctx->total_samples) {
        ctx->total_samples = ctx->info_b.total_samples;
    }
    ctx->total_duration_seconds = ctx->realtime_sample_rate_hz > 0.0
        ? (double)ctx->total_samples / ctx->realtime_sample_rate_hz : 0.0;
}

static void gui_playback_finish_index(void) {
    if (!s_playback_index.pending) return;
    atomic_store(&s_playback_index.cancel, true);
    thrd_join(s_playback_index.thread, NULL);
    s_playback_index.pending = false;
    s_playback_index.owner = NULL;
    for (int channel = 0; channel < 2; ++channel) {
        atomic_store(&s_playback_index.bytes_scanned[channel], 0);
        atomic_store(&s_playback_index.total_bytes[channel], 0);
        atomic_store(&s_playback_index.queued[channel], false);
        atomic_store(&s_playback_index.waiting[channel], false);
    }
}

static void gui_playback_poll_index(void) {
#if LIBFLAC_ENABLED == 1
    if (!s_playback_index.pending) return;
    // Observe completion before results so the final channel cannot finish
    // between the loop and join and lose its last publication.
    bool finished = atomic_load(&s_playback_index.done);
    for (int channel = 0; channel < 2; ++channel) {
        if (!s_playback_index.requested[channel] || s_playback_index.published[channel] ||
            !atomic_load(&s_playback_index.channel_done[channel])) continue;
        s_playback_index.published[channel] = true;
        uint64_t samples = s_playback_index.samples[channel];
        if (s_playback_index.owner == s_playback && !atomic_load(&s_playback_index.cancel)) {
            playback_file_info_t *info = channel == 1 ? &s_playback->info_b : &s_playback->info_a;
            info->total_samples = samples;
            info->duration_seconds = samples / s_playback->channel_rate_hz[channel];
            info->length_verification_failed = samples == 0;
            info->length_provisional = false;
            atomic_store(&s_playback->channel_total_samples[channel], samples);
            gui_playback_update_length(s_playback);
            fprintf(stderr, "[PLAYBACK] Channel %c length ready: %" PRIu64 " samples (%.2f sec)\n",
                    channel == 1 ? 'B' : 'A', samples, info->duration_seconds);
        }
    }
    if (finished) {
        thrd_join(s_playback_index.thread, NULL);
        s_playback_index.pending = false;
        s_playback_index.owner = NULL;
    }
#endif
}

#if LIBFLAC_ENABLED == 1
static int gui_playback_index_thread(void *unused) {
    (void)unused;
#if defined(_WIN32) || defined(_WIN64)
    // Windows background mode also lowers this reader's I/O and memory priority.
    if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
#elif defined(__APPLE__)
    (void)pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#elif defined(__linux__)
    (void)thrd_set_nice_target(10);
#endif
    playback_ctx_t *ctx = s_playback_index.owner;
    playback_scan_pressure_t pressure = {.scan_buffers = &ctx->app->buffers, .state = &ctx->state};
    double raw_rate = fmax(ctx->channel_rate_hz[0], ctx->channel_rate_hz[1]);
    pressure.bytes_per_second[0] = raw_rate > 0.0 ? (uint64_t)(raw_rate * 4.0) : 0;
    for (int channel = 0; channel < 2; ++channel)
        pressure.bytes_per_second[channel + 1] = ctx->channel_rate_hz[channel] > 0.0
            ? (uint64_t)(ctx->channel_rate_hz[channel] * 2.0) : 0;
    for (int channel = 0; channel < 2; ++channel) {
        if (!s_playback_index.requested[channel]) continue;
        atomic_store(&s_playback_index.queued[channel], false);
        uint64_t offset = 0, size = 0, samples = 0;
        gui_playback_length_cache_key_t before, after;
        playback_scan_observer_t observer = {
            .bytes_scanned = &s_playback_index.bytes_scanned[channel],
            .waiting = &s_playback_index.waiting[channel], .pressure = &pressure,
            .expected_key = &ctx->loaded_key[channel],
            .held_source_file = channel == 1 ? ctx->file_b : ctx->file_a};
        if (!atomic_load(&s_playback_index.cancel) &&
            ctx->loaded_key_valid[channel] &&
            gui_playback_length_cache_matches_open_file(observer.held_source_file, observer.expected_key) &&
            gui_playback_length_cache_snapshot(s_playback_index.info[channel].filepath, &before) &&
            gui_playback_length_cache_keys_equal(&before, observer.expected_key) &&
            gui_playback_find_audio_offset_and_size(s_playback_index.info[channel].filepath, &offset, &size) &&
            size > offset) {
            atomic_store(&s_playback_index.total_bytes[channel], size - offset);
            if (gui_playback_scan_samples(s_playback_index.info[channel].filepath, offset,
                                      s_playback_index.aux[channel].min_frame_size,
                                      s_playback_index.aux[channel].max_frame_size, &samples,
                                      &s_playback_index.cancel, true, &observer) &&
                !atomic_load(&s_playback_index.cancel) &&
                gui_playback_length_cache_matches_open_file(observer.held_source_file, observer.expected_key) &&
                gui_playback_length_cache_snapshot(s_playback_index.info[channel].filepath, &after) &&
                gui_playback_length_cache_keys_equal(&after, observer.expected_key) &&
                gui_playback_length_cache_keys_equal(&before, &after)) {
                s_playback_index.samples[channel] = samples;
                if (!atomic_load(&s_playback_index.cancel))
                    (void)gui_playback_length_cache_store(&after, samples);
            }
        }
        if (s_playback_index.samples[channel] == 0 && !atomic_load(&s_playback_index.cancel)) {
            fprintf(stderr, "[PLAYBACK] Channel %c length unavailable; continuing playback\n",
                    channel == 1 ? 'B' : 'A');
        }
        atomic_store(&s_playback_index.channel_done[channel], true);
    }
    atomic_store(&s_playback_index.done, true);
    return 0;
}
#endif

static void gui_playback_start_index(playback_ctx_t *ctx) {
#if LIBFLAC_ENABLED == 1
    s_playback_index.info[0] = ctx->info_a;
    s_playback_index.info[1] = ctx->info_b;
    s_playback_index.aux[0] = ctx->stream_a;
    s_playback_index.aux[1] = ctx->stream_b;
    bool needed = false;
    for (int channel = 0; channel < 2; ++channel) {
        s_playback_index.requested[channel] = s_playback_index.info[channel].valid &&
                                             (s_playback_index.info[channel].total_samples == 0 ||
                                              s_playback_index.info[channel].length_provisional) &&
                                             ctx->loaded_key_valid[channel];
        needed = needed || s_playback_index.requested[channel];
        s_playback_index.published[channel] = false;
        s_playback_index.samples[channel] = 0;
        atomic_store(&s_playback_index.channel_done[channel], false);
        atomic_store(&s_playback_index.bytes_scanned[channel], 0);
        atomic_store(&s_playback_index.total_bytes[channel], 0);
        atomic_store(&s_playback_index.queued[channel], s_playback_index.requested[channel]);
        atomic_store(&s_playback_index.waiting[channel], false);
    }
    if (!needed) return;
    s_playback_index.owner = ctx;
    atomic_store(&s_playback_index.cancel, false);
    atomic_store(&s_playback_index.done, false);
    if (thrd_create(&s_playback_index.thread, gui_playback_index_thread, NULL) == thrd_success) {
        s_playback_index.pending = true;
    } else {
        s_playback_index.owner = NULL;
        for (int channel = 0; channel < 2; ++channel) {
            playback_file_info_t *info = channel == 1 ? &ctx->info_b : &ctx->info_a;
            if (!info->length_provisional) continue;
            info->total_samples = 0; info->duration_seconds = 0;
            info->length_provisional = false; info->length_verification_failed = true;
            atomic_store(&ctx->channel_total_samples[channel], 0);
        }
        gui_playback_update_length(ctx);
        fprintf(stderr, "[PLAYBACK] Length reader unavailable; continuing with unknown length\n");
    }
#else
    (void)ctx;
#endif
}

static void gui_playback_release_context(playback_ctx_t *ctx) {
    if (!ctx) return;
#if LIBFLAC_ENABLED == 1
    if (ctx->decoder_a) {
        FLAC__stream_decoder_finish(ctx->decoder_a);
        FLAC__stream_decoder_delete(ctx->decoder_a);
        ctx->decoder_a = NULL;
    }
    if (ctx->decoder_b) {
        FLAC__stream_decoder_finish(ctx->decoder_b);
        FLAC__stream_decoder_delete(ctx->decoder_b);
        ctx->decoder_b = NULL;
    }
    // Drag-and-drop audio source (NULL when none was set/loaded)
    gui_playback_audio_destroy(&ctx->audio);
#endif
    if (ctx->file_a && !ctx->decoder_owns_file_a) fclose(ctx->file_a);
    if (ctx->file_b && !ctx->decoder_owns_file_b) fclose(ctx->file_b);
    ctx->file_a = NULL; ctx->file_b = NULL;
    free(ctx->decode_buf_a); free(ctx->decode_buf_b);
    ctx->decode_buf_a = NULL; ctx->decode_buf_b = NULL;
    // Raw-reader + Ogg-remux session state: the FILE handles were closed above
    // via file_a/file_b. Drop the reader state and unlink any session temp
    // remux (decode_path equals the original path for every on-disk format,
    // so nothing is removed for FLAC/raw channels).
    for (int ch = 0; ch < 2; ch++) {
        if (ctx->decode_path_temp[ch] && ctx->decode_path[ch][0]) {
            file_remove_utf8(ctx->decode_path[ch]);
        }
        ctx->decode_path[ch][0] = '\0';
        ctx->decode_path_temp[ch] = false;
        ctx->input_is_raw[ch] = false;
        ctx->input_format[ch] = PLAYBACK_INPUT_FLAC;
        ctx->conv16_state[ch] = PLAYBACK_CONV16_PENDING;
        memset(&ctx->raw[ch], 0, sizeof(ctx->raw[ch]));
    }
    atomic_store(&ctx->running, false);
    atomic_store(&ctx->state, PLAYBACK_STATE_STOPPED);
}

static void gui_playback_retire_context(void) {
    gui_playback_finish_index();
#if LIBFLAC_ENABLED == 1
    // Analysis workers retain private decoder resources and immutable session
    // snapshots; join before releasing the active playback context.
    gui_playback_analysis_shutdown(&s_analysis[0]);
    gui_playback_analysis_shutdown(&s_analysis[1]);
#endif
    gui_playback_release_context(s_playback);
    if (s_playback != &s_idle_playback) {
        // Keep stopped timeline metadata, but release the loaded context itself.
        s_idle_playback = *s_playback;
        free(s_playback);
        s_playback = &s_idle_playback;
    }
}

#if LIBFLAC_ENABLED == 1
static bool gui_playback_loaded_file_matches(playback_ctx_t *ctx, int channel) {
    playback_file_info_t *info = channel == 1 ? &ctx->info_b : &ctx->info_a;
    FILE *file = channel == 1 ? ctx->file_b : ctx->file_a;
    gui_playback_length_cache_key_t path_key;
    if (!file) return false;
    // Length is optional: unavailable keys/path fingerprints disable cache or
    // the index, but must not prevent a usable already-open decoder playing.
    if (!ctx->loaded_key_valid[channel]) return true;
    if (!gui_playback_length_cache_matches_open_file(file, &ctx->loaded_key[channel])) return false;
    // Identity is checked in the DECODE domain (the Ogg-FLAC session remux),
    // which is what the loaded key was fingerprinted from; every on-disk
    // native format keeps decode_path == the info filepath, so behavior is
    // unchanged for FLAC/raw channels.
    const char *decode = ctx->decode_path[channel][0] ? ctx->decode_path[channel] : info->filepath;
    if (!gui_playback_length_cache_snapshot(decode, &path_key)) return true;
    return gui_playback_length_cache_keys_equal(&path_key, &ctx->loaded_key[channel]);
}
#endif

// The three channel-preparation helpers are libFLAC-only as a block:
// open_native_flac drives the FLAC decoder API and the guarded ctx members
// (decoder_a/b, io_a/b), prepare_ogg remuxes through it (the Ogg header
// model also lives in the guarded region above), and prepare_raw's reader
// (gui_playback_raw_open) is defined inside that same guarded region.
// gui_playback_prepare() below rejects startup entirely when
// LIBFLAC_ENABLED != 1, so none of this is reachable without libFLAC;
// keeping the block unguarded broke the dep-less build (CI optional-dep
// guard, 2026-10-19).
#if LIBFLAC_ENABLED == 1
// Open + init the libFLAC decoder for one channel against a native FLAC path
// (the original file, or the Ogg-FLAC session remux): the IO layer stages the
// STREAMINFO patch, and the metadata pass fills the file info header fields.
// Identical operation order to the previous inline prepare path.
static bool gui_playback_open_native_flac(playback_ctx_t *ctx, int channel, const char *path,
                                          const atomic_bool *cancel, char *err, size_t err_len) {
    bool channel_b = channel == 1;
    FILE **file = channel_b ? &ctx->file_b : &ctx->file_a;
    FLAC__StreamDecoder **decoder = channel_b ? &ctx->decoder_b : &ctx->decoder_a;
    playback_io_t *io = channel_b ? &ctx->io_b : &ctx->io_a;
    playback_file_info_t *info = channel_b ? &ctx->info_b : &ctx->info_a;
    playback_streaminfo_aux_t *stream = channel_b ? &ctx->stream_b : &ctx->stream_a;
    gui_playback_length_cache_key_t *loaded_key = &ctx->loaded_key[channel];
    bool *loaded_key_valid = &ctx->loaded_key_valid[channel];

    if (err && err_len) err[0] = '\0';
    *file = file_open_read_utf8(path);
    if (!*file) {
        snprintf(err, err_len, "open failed: %s", path);
        return false;
    }
    if (gui_playback_length_cache_snapshot(path, loaded_key)) {
        if (!gui_playback_length_cache_matches_open_file(*file, loaded_key)) {
            snprintf(err, err_len, "file changed while loading: %s", path);
            return false;
        }
        *loaded_key_valid = true;
    }

    *decoder = FLAC__stream_decoder_new();
    if (!*decoder) {
        snprintf(err, err_len, "failed to create the decoder");
        return false;
    }

    FLAC__stream_decoder_set_md5_checking(*decoder, false);

    // Unknown STREAMINFO total removes finalized RF/provisional endpoint
    // clamps while the UI independently tracks the resolved true length.
    gui_playback_io_reset(io, *file, ctx, PLAYBACK_IO_SI_TOTAL_MOD);
    io->cancel = cancel;
    FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
        *decoder,
        gui_playback_io_read_cb,
        gui_playback_io_seek_cb,
        gui_playback_io_tell_cb,
        gui_playback_io_length_cb,
        gui_playback_io_eof_cb,
        channel_b ? decoder_io_write_cb_b : decoder_io_write_cb_a,
        channel_b ? decoder_io_metadata_cb_b : decoder_io_metadata_cb_a,
        decoder_error_cb,
        io
    );

    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        snprintf(err, err_len, "failed to init decoder: %s",
                 FLAC__StreamDecoderInitStatusString[status]);
        return false;
    }

    // Process metadata to get file info
    if (!FLAC__stream_decoder_process_until_end_of_metadata(*decoder) || !info->valid) {
        snprintf(err, err_len, "not a valid FLAC stream (unreadable metadata)");
        return false;
    }
    if (stream->channels != 1) {
        snprintf(err, err_len, "FLAC has %u channels (expected mono)", stream->channels);
        return false;
    }
    if (cancel && atomic_load(cancel)) {
        snprintf(err, err_len, "cancelled");
        return false;
    }
    return true;
}

// Raw / WAV / .lds channel preparation: open the sample reader and publish
// its resolved metadata (rate, exact total). No libFLAC decoder exists for
// this channel; the playback thread's raw branches drive the reader.
static bool gui_playback_prepare_raw(playback_ctx_t *ctx, int channel, const char *filepath,
                                     playback_input_format_t fmt, char *err, size_t err_len) {
    playback_raw_reader_t *r = &ctx->raw[channel];
    playback_file_info_t *info = channel ? &ctx->info_b : &ctx->info_a;
    if (err && err_len) err[0] = '\0';
    if (!gui_playback_raw_open(r, info, filepath, fmt, err, err_len)) {
        if (err && err[0]) {
            snprintf(info->error, sizeof(info->error), "%s", err);
        }
        return false;
    }
    if (channel) ctx->file_b = r->file;
    else ctx->file_a = r->file;
    ctx->input_is_raw[channel] = true;
    ctx->channel_rate_hz[channel] = info->real_sample_rate_hz;
    fprintf(stderr,
            "[PLAYBACK] File %c (%s): header rate=%u Hz, %u-bit, total=%" PRIu64
            " (real_rate=%.0fHz, %.2f sec)\n",
            channel ? 'B' : 'A', gui_playback_input_format_name(fmt),
            info->sample_rate, info->bits_per_sample, info->total_samples,
            info->real_sample_rate_hz, info->duration_seconds);
    return true;
}

// Ogg FLAC channel preparation: remux the whole file into a session temp
// native FLAC, then run the ordinary native-FLAC open path on the temp (the
// existing machinery - IO-layer seeks, the length resolution, the paused
// analysis decoders - then works on it unchanged). The temp path is
// registered BEFORE the remux writes, so any failure path unlinks the
// partial file on release; the exact length is the copied-frame sum (the
// last-page granule only cross-checks - it describes the source, the sum
// describes exactly what the temp holds).
static bool gui_playback_prepare_ogg(playback_ctx_t *ctx, int channel, const char *filepath,
                                     const atomic_bool *cancel, char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    playback_ogg_header_t h;
    if (!gui_playback_ogg_read_header(filepath, &h, err, err_len)) {
        return false;
    }
    bool ok = false;
    uint64_t file_size = 0;
    uint64_t granule = 0;
    uint64_t sum = 0;
    do {
        if (!gui_playback_stat_file_size(filepath, &file_size)) {
            snprintf(err, err_len, "stat failed: %s", filepath);
            break;
        }
        bool have_granule = gui_playback_ogg_last_granule(filepath, &h, file_size, &granule);
        if (!gui_playback_temp_path(ctx->decode_path[channel],
                                    sizeof(ctx->decode_path[channel]), channel)) {
            snprintf(err, err_len, "cannot build a temp path for the Ogg FLAC remux");
            break;
        }
        ctx->decode_path_temp[channel] = true;
        if (!gui_playback_ogg_remux(filepath, file_size, &h, ctx->decode_path[channel],
                                   cancel, &sum, err, err_len)) {
            break;
        }
        if (have_granule && granule != sum) {
            fprintf(stderr,
                    "[PLAYBACK] Ogg FLAC: last-page granule (%" PRIu64
                    ") differs from the copied frames (%" PRIu64 " samples); using the copied frames\n",
                    granule, sum);
        }
        if (!gui_playback_open_native_flac(ctx, channel, ctx->decode_path[channel],
                                           cancel, err, err_len)) {
            break;
        }
        playback_file_info_t *info = channel ? &ctx->info_b : &ctx->info_a;
        info->format = PLAYBACK_INPUT_OGG_FLAC;
        info->total_samples = sum;  // the remux is the authority (its STREAMINFO
                                    // total already holds the sum when it fits)
        double real_rate = gui_playback_resolve_real_rate_hz(
            filepath, info->sample_rate, h.has_tags ? &h.tags : NULL);
        info->real_sample_rate_hz = real_rate > 0.0 ? real_rate : 0.0;
        info->duration_seconds = info->real_sample_rate_hz > 0.0
            ? (double)sum / info->real_sample_rate_hz : 0.0;
        ctx->channel_rate_hz[channel] = info->real_sample_rate_hz;
        fprintf(stderr,
                "[PLAYBACK] File %c (Ogg FLAC remux): header rate=%u Hz, %u-bit, total=%" PRIu64
                " (real_rate=%.0fHz, %.2f sec)\n",
                channel ? 'B' : 'A', info->sample_rate, info->bits_per_sample, sum,
                info->real_sample_rate_hz, info->duration_seconds);
        ok = true;
    } while (0);
    gui_playback_ogg_header_cleanup(&h);
    return ok;
}
#endif // LIBFLAC_ENABLED

static int gui_playback_prepare(playback_ctx_t *ctx, gui_app_t *app,
                                const char *file_a, const char *file_b,
                                const atomic_bool *cancel) {
    if (!ctx || (!file_a && !file_b) || (cancel && atomic_load(cancel))) return -1;
#if LIBFLAC_ENABLED != 1
    (void)app; (void)cancel;
    return -1;
#else
    fprintf(stderr, "[PLAYBACK] Starting playback\n");
    if (file_a) fprintf(stderr, "[PLAYBACK]   Channel A: %s\n", file_a);
    if (file_b) fprintf(stderr, "[PLAYBACK]   Channel B: %s\n", file_b);

    ctx->app = app;
    if ((file_a && strlen(file_a) >= sizeof(ctx->info_a.filepath)) ||
        (file_b && strlen(file_b) >= sizeof(ctx->info_b.filepath))) return -1;

    // Allocate decode buffers (larger than output buffer to allow streaming)
    ctx->decode_buf_size = PLAYBACK_BUFFER_SIZE * 8;
    ctx->decode_buf_a = (int16_t *)malloc(ctx->decode_buf_size * sizeof(int16_t));
    ctx->decode_buf_b = (int16_t *)malloc(ctx->decode_buf_size * sizeof(int16_t));

    if (!ctx->decode_buf_a || !ctx->decode_buf_b) {
        fprintf(stderr, "[PLAYBACK] Failed to allocate decode buffers\n");
        return -1;
    }

    // Open and initialize the per-channel readers/decoders
    if (file_a) {
        if (file_a != ctx->info_a.filepath)
            snprintf(ctx->info_a.filepath, sizeof(ctx->info_a.filepath), "%s", file_a);

        // Sniff the container (content first, then the raw extensions):
        // native FLAC keeps the libFLAC path verbatim; Ogg FLAC remuxes to a
        // session temp native FLAC; the raw formats open the sample readers
        // with no decoder at all. Unsupported containers fail with the
        // sniffer's specific message instead of a FLAC parse failure.
        char open_err[192] = {0};
        playback_input_format_t fmt = PLAYBACK_INPUT_FLAC;
        uint8_t magic[4] = {0};
        if (!gui_playback_sniff_format(file_a, &fmt, magic, open_err, sizeof(open_err))) {
            fprintf(stderr, "[PLAYBACK] Channel A rejected: %s (%s)\n", open_err, file_a);
            snprintf(ctx->info_a.error, sizeof(ctx->info_a.error), "%s", open_err);
            return -1;
        }
        ctx->input_format[0] = (int)fmt;
        ctx->info_a.format = (int)fmt;
        snprintf(ctx->decode_path[0], sizeof(ctx->decode_path[0]), "%s", file_a);

        if (fmt == PLAYBACK_INPUT_OGG_FLAC) {
            if (!gui_playback_prepare_ogg(ctx, 0, file_a, cancel, open_err, sizeof(open_err))) {
                fprintf(stderr, "[PLAYBACK] Channel A (Ogg FLAC): %s (%s)\n", open_err, file_a);
                snprintf(ctx->info_a.error, sizeof(ctx->info_a.error), "%s", open_err);
                return -1;
            }
        } else if (fmt != PLAYBACK_INPUT_FLAC) {
            if (!gui_playback_prepare_raw(ctx, 0, file_a, fmt, open_err, sizeof(open_err))) {
                fprintf(stderr, "[PLAYBACK] Channel A (%s): %s (%s)\n",
                        gui_playback_input_format_name(fmt), open_err, file_a);
                snprintf(ctx->info_a.error, sizeof(ctx->info_a.error), "%s", open_err);
                return -1;
            }
        } else {
            // Native FLAC (fLaC magic): the original decoder path, unchanged
            // behavior through the shared open helper + length resolution.
            if (!gui_playback_open_native_flac(ctx, 0, file_a, cancel, open_err, sizeof(open_err))) {
                fprintf(stderr, "[PLAYBACK] Channel A: %s (%s)\n", open_err, file_a);
                snprintf(ctx->info_a.error, sizeof(ctx->info_a.error), "%s", open_err);
                return -1;
            }
            if (cancel && atomic_load(cancel)) return -1;
            ctx->channel_rate_hz[0] = gui_playback_apply_resolved_file_info(
                &ctx->info_a, &ctx->stream_a, "File A", cancel,
                ctx->loaded_key_valid[0] ? &ctx->loaded_key[0] : NULL, ctx->file_a);
        }
    }

    if (cancel && atomic_load(cancel)) return -1;

    if (file_b) {
        if (file_b != ctx->info_b.filepath)
            snprintf(ctx->info_b.filepath, sizeof(ctx->info_b.filepath), "%s", file_b);

        char open_err[192] = {0};
        playback_input_format_t fmt = PLAYBACK_INPUT_FLAC;
        uint8_t magic[4] = {0};
        if (!gui_playback_sniff_format(file_b, &fmt, magic, open_err, sizeof(open_err))) {
            fprintf(stderr, "[PLAYBACK] Channel B rejected: %s (%s)\n", open_err, file_b);
            snprintf(ctx->info_b.error, sizeof(ctx->info_b.error), "%s", open_err);
            return -1;
        }
        ctx->input_format[1] = (int)fmt;
        ctx->info_b.format = (int)fmt;
        snprintf(ctx->decode_path[1], sizeof(ctx->decode_path[1]), "%s", file_b);

        if (fmt == PLAYBACK_INPUT_OGG_FLAC) {
            if (!gui_playback_prepare_ogg(ctx, 1, file_b, cancel, open_err, sizeof(open_err))) {
                fprintf(stderr, "[PLAYBACK] Channel B (Ogg FLAC): %s (%s)\n", open_err, file_b);
                snprintf(ctx->info_b.error, sizeof(ctx->info_b.error), "%s", open_err);
                return -1;
            }
        } else if (fmt != PLAYBACK_INPUT_FLAC) {
            if (!gui_playback_prepare_raw(ctx, 1, file_b, fmt, open_err, sizeof(open_err))) {
                fprintf(stderr, "[PLAYBACK] Channel B (%s): %s (%s)\n",
                        gui_playback_input_format_name(fmt), open_err, file_b);
                snprintf(ctx->info_b.error, sizeof(ctx->info_b.error), "%s", open_err);
                return -1;
            }
        } else {
            if (!gui_playback_open_native_flac(ctx, 1, file_b, cancel, open_err, sizeof(open_err))) {
                fprintf(stderr, "[PLAYBACK] Channel B: %s (%s)\n", open_err, file_b);
                snprintf(ctx->info_b.error, sizeof(ctx->info_b.error), "%s", open_err);
                return -1;
            }
            if (cancel && atomic_load(cancel)) return -1;
            ctx->channel_rate_hz[1] = gui_playback_apply_resolved_file_info(
                &ctx->info_b, &ctx->stream_b, "File B", cancel,
                ctx->loaded_key_valid[1] ? &ctx->loaded_key[1] : NULL, ctx->file_b);
        }
    }

    // Drag-and-drop audio source (settings.playback_audio_file): opened
    // off-thread with the channels. A failure is non-fatal (playback works
    // without the monitor audio); the error names the file + reason.
    if (app->settings.playback_audio_file[0]) {
        char audio_err[192] = {0};
        ctx->audio = gui_playback_audio_open(app->settings.playback_audio_file,
                                             audio_err, sizeof(audio_err));
        if (ctx->audio) {
            fprintf(stderr,
                    "[PLAYBACK] Audio source: %u Hz, %u ch, %u-bit, %" PRIu64 " frames (%s)\n",
                    ctx->audio->rate_hz, ctx->audio->channels, ctx->audio->bits,
                    ctx->audio->total_frames, app->settings.playback_audio_file);
        } else {
            fprintf(stderr, "[PLAYBACK] Audio source unavailable: %s (%s)\n",
                    audio_err[0] ? audio_err : "open failed",
                    app->settings.playback_audio_file);
        }
    }

    if ((file_a && !gui_playback_loaded_file_matches(ctx, 0)) ||
        (file_b && !gui_playback_loaded_file_matches(ctx, 1))) return -1;
    return cancel && atomic_load(cancel) ? -1 : 0;
#endif
}

static int gui_playback_commit(gui_app_t *app, playback_ctx_t *prepared) {
#if LIBFLAC_ENABLED == 1
    // The private loader may have completed several UI frames ago. Reject an
    // observed path/held-file change before cached lengths become active.
    if ((prepared->info_a.valid && !gui_playback_loaded_file_matches(prepared, 0)) ||
        (prepared->info_b.valid && !gui_playback_loaded_file_matches(prepared, 1))) {
        gui_playback_release_context(prepared);
        free(prepared);
        gui_app_set_status(app, "Playback file changed while loading");
        return -1;
    }
#endif
    if (gui_playback_is_running(app)) gui_playback_stop(app);
    gui_playback_retire_context();
    s_playback = prepared;
#if LIBFLAC_ENABLED == 1
    // The loader has joined. Playback no longer belongs to its cancel job.
    s_playback->io_a.cancel = NULL;
    s_playback->io_b.cancel = NULL;
#endif
    gui_extract_prepare_stream(app);

    atomic_store(&s_playback->channel_total_samples[0], s_playback->info_a.total_samples);
    atomic_store(&s_playback->channel_total_samples[1], s_playback->info_b.total_samples);

    double rate_a_hz = s_playback->channel_rate_hz[0];
    double rate_b_hz = s_playback->channel_rate_hz[1];
    s_playback->realtime_sample_rate_hz = 0.0;
    if (rate_a_hz > 0.0) {
        s_playback->realtime_sample_rate_hz = rate_a_hz;
    }
    if (rate_b_hz > 0.0 &&
        (s_playback->realtime_sample_rate_hz <= 0.0 || rate_b_hz > s_playback->realtime_sample_rate_hz)) {
        s_playback->realtime_sample_rate_hz = rate_b_hz;
    }
    if (s_playback->realtime_sample_rate_hz <= 0.0) {
        s_playback->realtime_sample_rate_hz = (double)PLAYBACK_SAMPLE_RATE * 1000.0;
    }
    gui_playback_update_length(s_playback);
    fprintf(stderr,
            "[PLAYBACK] Runtime rate=%.0fHz total_samples=%" PRIu64 " duration=%.2f sec\n",
            s_playback->realtime_sample_rate_hz,
            s_playback->total_samples,
            s_playback->total_duration_seconds);
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    // Reset app statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    // Stop requests belong to the preceding session. The idle render loop
    // leaves them unconsumed; clear them before this session becomes active.
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    app->low_signal_time = 0.0f;
    app->low_signal_armed = false;
    uint32_t playback_rate_hz = (uint32_t)llround(s_playback->realtime_sample_rate_hz);
    if (playback_rate_hz == 0) playback_rate_hz = PLAYBACK_SAMPLE_RATE * 1000u;
    atomic_store(&app->sample_rate, playback_rate_hz);
    atomic_store(&app->rf_source_rate_hz[0], s_playback->file_a ? (uint32_t)llround(rate_a_hz) : 0);
    atomic_store(&app->rf_source_rate_hz[1], s_playback->file_b ? (uint32_t)llround(rate_b_hz) : 0);
    atomic_store(&app->rf_preview_input_supported,
                  !s_playback->file_a || !s_playback->file_b || llround(rate_a_hz) == llround(rate_b_hz));
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = (s_playback->file_b != NULL);
    /* Playback files are their own fixed A/B mapping (file A = channel A;
     * the per-channel seek sliders scrub their own file). The MISRC
     * hardware-input swap must never leak into playback — it crossed the
     * panel feeds against the sliders. */
    app->capture_mode_runtime_misrc = false;
    app->capture_start_time = gui_runtime_time();
    app->reconnect_pending = false;
    app->reconnect_attempts = 0;
    {
        time_t t = time(NULL);
        struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        snprintf(app->capture_timestamp, sizeof(app->capture_timestamp), "%04d.%02d.%02d_%02d.%02d.%02d",
                 (tmv.tm_year + 1900),
                 tmv.tm_mon + 1,
                 tmv.tm_mday,
                 tmv.tm_hour,
                 tmv.tm_min,
                 tmv.tm_sec);
    }

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Initialize playback state
    atomic_store(&s_playback->state, PLAYBACK_STATE_PLAYING);
    atomic_store(&s_playback->speed, PLAYBACK_SPEED_1X);
    atomic_store(&s_playback->loop_enabled, true);  // Loop by default
    atomic_store(&s_playback->seek_requested_a, false);
    atomic_store(&s_playback->seek_requested_b, false);
    atomic_store(&s_playback->seek_target_a, 0);
    atomic_store(&s_playback->seek_target_b, 0);
    atomic_store(&s_playback->current_sample, 0);
    atomic_store(&s_playback->current_sample_a, 0);
    atomic_store(&s_playback->current_sample_b, 0);
    atomic_store(&s_playback->running, true);

    app->is_capturing = true;

    // Start extraction thread - reads from BUF_CAPTURE_RF, writes to BUF_DISPLAY
    // Also handles statistics, peak detection, and recording if enabled
    int r = gui_extract_start_tagged(app);
    if (r < 0) {
        fprintf(stderr, "[PLAYBACK] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        goto commit_failed;
    }

    // Start display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[PLAYBACK] Failed to start display thread (non-fatal)\n");
            // Non-fatal - display will use legacy path
        }
    }

    // Start playback thread - decodes FLAC and writes to BUF_CAPTURE_RF
    thrd_t thread;
    if (thrd_create_with_priority(&thread,
                                  playback_thread_func,
                                  s_playback,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[PLAYBACK] Failed to create playback thread\n");
        // Stop extraction thread before cleanup
        gui_extract_stop();
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        app->is_capturing = false;
        goto commit_failed;
    }
    s_playback->playback_thread = (void *)(uintptr_t)thread;
    gui_playback_start_index(s_playback);

    gui_app_set_status(app, "Playback started");

    return 0;

commit_failed:
    app->is_capturing = false;
    app->capture_timestamp[0] = '\0';
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = true;
    gui_playback_retire_context();
    gui_app_set_status(app, "Playback failed to start");
    return -1;
}

int gui_playback_start(gui_app_t *app, const char *file_a, const char *file_b) {
    if (!app || s_playback_start.pending || (!file_a && !file_b)) return -1;
    playback_ctx_t *prepared = calloc(1, sizeof(*prepared));
    if (!prepared) return -1;
    if (gui_playback_prepare(prepared, app, file_a, file_b, NULL) != 0) {
        gui_playback_release_context(prepared); free(prepared);
        gui_app_set_status(app, "Playback failed to start"); return -1;
    }
    return gui_playback_commit(app, prepared);
}

static int gui_playback_prepare_thread(void *unused) {
    (void)unused;
    thrd_set_priority(THRD_PRIORITY_NORMAL);
    playback_ctx_t *ctx = s_playback_start.prepared;
    s_playback_start.result = gui_playback_prepare(ctx, ctx->app,
        ctx->info_a.filepath[0] ? ctx->info_a.filepath : NULL,
        ctx->info_b.filepath[0] ? ctx->info_b.filepath : NULL,
        &s_playback_start.cancel);
    atomic_store(&s_playback_start.done, true);
    return 0;
}

bool gui_playback_is_starting(gui_app_t *app) {
    (void)app;
    return s_playback_start.pending;
}

int gui_playback_start_async(gui_app_t *app, const char *file_a, const char *file_b) {
    if (!app || s_playback_start.pending || app->is_capturing || (!file_a && !file_b)) return -1;
    if ((file_a && strlen(file_a) >= FILE_PATH_MAX) ||
        (file_b && strlen(file_b) >= FILE_PATH_MAX)) {
        gui_app_set_status(app, "Playback path is too long"); return -1;
    }
    playback_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { gui_app_set_status(app, "Failed to allocate playback loader"); return -1; }
    ctx->app = app;
    if (file_a) snprintf(ctx->info_a.filepath, sizeof(ctx->info_a.filepath), "%s", file_a);
    if (file_b) snprintf(ctx->info_b.filepath, sizeof(ctx->info_b.filepath), "%s", file_b);
    s_playback_start.prepared = ctx;
    atomic_store(&s_playback_start.done, false);
    atomic_store(&s_playback_start.cancel, false);
    s_playback_start.pending = true;
    if (thrd_create_with_priority(&s_playback_start.thread, gui_playback_prepare_thread,
                                  NULL, THRD_PRIORITY_NORMAL) != thrd_success) {
        s_playback_start.pending = false; s_playback_start.prepared = NULL;
        free(ctx); gui_app_set_status(app, "Failed to start playback loader"); return -1;
    }
    gui_app_set_status(app, "Loading playback...");
    return 0;
}

void gui_playback_cancel_start(gui_app_t *app) {
    if (!s_playback_start.pending) return;
    atomic_store(&s_playback_start.cancel, true);
    if (app) gui_app_set_status(app, "Cancelling playback load...");
}

int gui_playback_poll_start(gui_app_t *app) {
    gui_playback_poll_index();
    if (!s_playback_start.pending || !atomic_load(&s_playback_start.done)) return 0;
    thrd_join(s_playback_start.thread, NULL);
    playback_ctx_t *prepared = s_playback_start.prepared;
    bool cancelled = atomic_load(&s_playback_start.cancel);
    int result = s_playback_start.result;
    s_playback_start.pending = false; s_playback_start.prepared = NULL;
    if (cancelled || result != 0) {
        gui_playback_release_context(prepared); free(prepared);
        gui_app_set_status(app, cancelled ? "Playback load cancelled" : "Playback failed to start");
        return -1;
    }
    return gui_playback_commit(app, prepared) == 0 ? 1 : -1;
}

void gui_playback_finish_start(gui_app_t *app) {
    (void)app;
    if (!s_playback_start.pending) return;
    atomic_store(&s_playback_start.cancel, true);
    thrd_join(s_playback_start.thread, NULL);
    gui_playback_release_context(s_playback_start.prepared);
    free(s_playback_start.prepared);
    s_playback_start.prepared = NULL; s_playback_start.pending = false;
}

void gui_playback_stop(gui_app_t *app) {
    if (!atomic_load(&s_playback->running)) return;

    fprintf(stderr, "[PLAYBACK] Stopping playback\n");

    // Set is_capturing to false BEFORE stopping extraction thread
    // The extraction thread checks this flag to know when to exit
    app->is_capturing = false;

    atomic_store(&s_playback->running, false);
    atomic_store(&s_playback->state, PLAYBACK_STATE_STOPPED);
    // Let both readers wind down together before any active context is freed.
    if (s_playback_index.pending) atomic_store(&s_playback_index.cancel, true);

    // Stop playback thread first (it writes to BUF_CAPTURE_RF)
    if (s_playback->playback_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)s_playback->playback_thread;
        thrd_join(thread, NULL);
        s_playback->playback_thread = NULL;
    }

    // Stop display thread (reads from BUF_DISPLAY written by extraction)
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop extraction thread (reads BUF_CAPTURE_RF, writes BUF_DISPLAY)
    gui_extract_stop();

    gui_playback_retire_context();

    // All old producer/extraction posts have stopped before clearing flags.
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    app->low_signal_time = 0.0f;
    app->low_signal_armed = false;
    app->capture_timestamp[0] = '\0';
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = true;

    atomic_store(&app->stream_synced, false);

    gui_app_set_status(app, "Playback stopped");
}

bool gui_playback_is_running(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_playback->running);
}

bool gui_playback_is_indexing(gui_app_t *app, int channel) {
    (void)app;
    return channel >= 0 && channel < 2 && s_playback_index.pending &&
           s_playback_index.owner == s_playback && s_playback_index.requested[channel] &&
           !s_playback_index.published[channel];
}

bool gui_playback_get_index_progress(gui_app_t *app, int channel, playback_index_progress_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!gui_playback_is_indexing(app, channel)) return false;
    out->active = true;
    out->queued = atomic_load(&s_playback_index.queued[channel]);
    out->waiting = atomic_load(&s_playback_index.waiting[channel]);
    out->total_bytes = atomic_load(&s_playback_index.total_bytes[channel]);
    out->bytes_scanned = atomic_load(&s_playback_index.bytes_scanned[channel]);
    if (out->bytes_scanned > out->total_bytes) out->bytes_scanned = out->total_bytes;
    return true;
}

double gui_playback_get_channel_rate_hz(gui_app_t *app, int channel) {
    (void)app;
    return channel >= 0 && channel < 2 ? s_playback->channel_rate_hz[channel] : 0.0;
}

playback_state_t gui_playback_get_state(gui_app_t *app) {
    (void)app;
    return (playback_state_t)atomic_load(&s_playback->state);
}

void gui_playback_pause(gui_app_t *app) {
    (void)app;
    if (atomic_load(&s_playback->state) == PLAYBACK_STATE_PLAYING) {
        atomic_store(&s_playback->state, PLAYBACK_STATE_PAUSED);
    }
}

void gui_playback_resume(gui_app_t *app) {
    (void)app;
    if (atomic_load(&s_playback->state) == PLAYBACK_STATE_PAUSED) {
        atomic_store(&s_playback->state, PLAYBACK_STATE_PLAYING);
    }
}

void gui_playback_toggle_pause(gui_app_t *app) {
    playback_state_t state = gui_playback_get_state(app);
    if (state == PLAYBACK_STATE_PLAYING) {
        gui_playback_pause(app);
    } else if (state == PLAYBACK_STATE_PAUSED) {
        gui_playback_resume(app);
    }
}

void gui_playback_seek_normalized(gui_app_t *app, double position) {
    if (position < 0.0) position = 0.0;
    if (position > 1.0) position = 1.0;
    if (s_playback->info_a.total_samples > 0) {
        uint64_t target_a = (uint64_t)floor(position * (double)s_playback->info_a.total_samples);
        if (target_a >= s_playback->info_a.total_samples) {
            target_a = s_playback->info_a.total_samples - 1;
        }
        gui_playback_seek_sample_channel(app, 0, target_a);
    }
    if (s_playback->info_b.total_samples > 0) {
        uint64_t target_b = (uint64_t)floor(position * (double)s_playback->info_b.total_samples);
        if (target_b >= s_playback->info_b.total_samples) {
            target_b = s_playback->info_b.total_samples - 1;
        }
        gui_playback_seek_sample_channel(app, 1, target_b);
    }
}

void gui_playback_seek_sample_channel(gui_app_t *app, int channel, uint64_t sample) {
    (void)app;
    bool channel_b = (channel == 1);
    uint64_t total_samples = channel_b ? s_playback->info_b.total_samples : s_playback->info_a.total_samples;
    if (total_samples == 0) {
        return;
    }
    if (sample >= total_samples) {
        sample = total_samples - 1;
    }
    if (channel_b) {
        atomic_store(&s_playback->seek_target_b, sample);
        atomic_store(&s_playback->seek_requested_b, true);
    } else {
        atomic_store(&s_playback->seek_target_a, sample);
        atomic_store(&s_playback->seek_requested_a, true);
    }
}

void gui_playback_seek_sample(gui_app_t *app, uint64_t sample) {
    (void)app;
    if (s_playback->info_a.total_samples > 0) {
        gui_playback_seek_sample_channel(app, 0, sample);
    }
    if (s_playback->info_b.total_samples > 0) {
        gui_playback_seek_sample_channel(app, 1, sample);
    }
}

uint64_t gui_playback_get_position_samples_channel(gui_app_t *app, int channel) {
    (void)app;
    if (channel == 1) {
        return atomic_load(&s_playback->current_sample_b);
    }
    return atomic_load(&s_playback->current_sample_a);
}

uint64_t gui_playback_get_seek_seq_channel(gui_app_t *app, int channel) {
    (void)app;
    if (channel == 1) {
        return atomic_load(&s_playback->seek_seq_b);
    }
    return atomic_load(&s_playback->seek_seq_a);
}

uint64_t gui_playback_get_position_samples(gui_app_t *app) {
    (void)app;
    if (s_playback->file_a != NULL) {
        return atomic_load(&s_playback->current_sample_a);
    }
    if (s_playback->file_b != NULL) {
        return atomic_load(&s_playback->current_sample_b);
    }
    return atomic_load(&s_playback->current_sample);
}

double gui_playback_get_position_normalized(gui_app_t *app) {
    (void)app;
    if (s_playback->total_samples == 0) return 0.0;
    uint64_t sample = atomic_load(&s_playback->current_sample);
    if (sample >= s_playback->total_samples) {
        sample = s_playback->total_samples;
    }
    return (double)sample / (double)s_playback->total_samples;
}

double gui_playback_get_position_seconds(gui_app_t *app) {
    (void)app;
    uint64_t samples = atomic_load(&s_playback->current_sample);
    bool provisional = s_playback->info_a.length_provisional || s_playback->info_b.length_provisional;
    if (!provisional && s_playback->total_samples > 0 && samples >= s_playback->total_samples)
        samples = s_playback->total_samples;
    if (s_playback->realtime_sample_rate_hz > 0.0)
        return (double)samples / s_playback->realtime_sample_rate_hz;
    return (double)samples / ((double)PLAYBACK_SAMPLE_RATE * 1000.0);
}

uint64_t gui_playback_get_total_samples(gui_app_t *app) {
    (void)app;
    return s_playback->total_samples;
}

double gui_playback_get_duration_seconds(gui_app_t *app) {
    (void)app;
    if (s_playback->total_duration_seconds > 0.0) {
        return s_playback->total_duration_seconds;
    }
    if (s_playback->realtime_sample_rate_hz > 0.0 && s_playback->total_samples > 0) {
        return (double)s_playback->total_samples / s_playback->realtime_sample_rate_hz;
    }
    return (double)s_playback->total_samples / ((double)PLAYBACK_SAMPLE_RATE * 1000.0);
}

void gui_playback_set_speed(gui_app_t *app, playback_speed_t speed) {
    (void)app;
    if (speed >= PLAYBACK_SPEED_COUNT) speed = PLAYBACK_SPEED_1X;
    atomic_store(&s_playback->speed, speed);
}

playback_speed_t gui_playback_get_speed(gui_app_t *app) {
    (void)app;
    return (playback_speed_t)atomic_load(&s_playback->speed);
}

bool gui_playback_get_file_info_a(gui_app_t *app, playback_file_info_t *info) {
    (void)app;
    if (!info) return false;
    *info = s_playback->info_a;
    return s_playback->info_a.valid;
}

bool gui_playback_get_file_info_b(gui_app_t *app, playback_file_info_t *info) {
    (void)app;
    if (!info) return false;
    *info = s_playback->info_b;
    return s_playback->info_b.valid;
}

// --- Selected playback file info (idle readout support) ---
// While playback runs, the live file info is authoritative. When idle, the
// file selected in settings is validated once per path change and cached, so
// per-frame UI readout calls stay cheap. The cache does not watch for on-disk
// changes to the same path; re-selecting the file refreshes it.
typedef struct {
    char path[FILE_PATH_MAX];
    playback_file_info_t info;
    bool cached;
} playback_selected_cache_t;

static playback_selected_cache_t s_selected_cache[2] = {0};

bool gui_playback_get_selected_file_info(gui_app_t *app, int channel, playback_file_info_t *info) {
    if (!app || !info) return false;
    if (channel != 0 && channel != 1) return false;
    memset(info, 0, sizeof(*info));

    // Live playback info when this channel is actually playing.
    if (gui_playback_is_running(app)) {
        playback_file_info_t live = {0};
        bool ok = (channel == 0)
            ? gui_playback_get_file_info_a(app, &live)
            : gui_playback_get_file_info_b(app, &live);
        if (ok && live.valid) {
            *info = live;
            return true;
        }
    }

    const char *path = (channel == 0) ? app->settings.playback_file_a
                                      : app->settings.playback_file_b;
    if (!path || path[0] == '\0') {
        return false;
    }

    playback_selected_cache_t *cache = &s_selected_cache[channel];
    if (!cache->cached || strncmp(cache->path, path, sizeof(cache->path)) != 0) {
        snprintf(cache->path, sizeof(cache->path), "%s", path);
        // validate_file fills info even when it returns false (e.g. wrong
        // channel count), so the readout can still show bit depth / rate.
        (void)gui_playback_validate_file(path, &cache->info);
        cache->cached = true;
    }

    *info = cache->info;
    return cache->info.valid;
}

void gui_playback_set_loop(gui_app_t *app, bool loop) {
    (void)app;
    atomic_store(&s_playback->loop_enabled, loop);
}

bool gui_playback_get_loop(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_playback->loop_enabled);
}

//-----------------------------------------------------------------------------
// DAW-Style Paused Analysis Window (direct file random access)
//-----------------------------------------------------------------------------
/* While PAUSED the display panels render analysis windows straight from the
 * FLAC file: the rolling live pipeline only carries one 65,536-sample display
 * frame (3.3 ms at 20 MSPS) — 1:1-correct for playing, far too small for
 * paused analysis. Each channel gets a dedicated analysis decoder (own FILE
 * handle + IO layer + STREAMINFO patch, fully isolated from the playback
 * thread's decoders) plus a background worker:
 *   - request (render thread): stores center + span (duration-based; sample
 *     rates vary across files 5/10/20/40 MSPS) and bumps req_seq (release).
 *   - worker: one seek to the window start, sequential decode across the
 *     span (per-slice seeks would re-decode seektable gaps), capturing one
 *     FFT-sized slice at each strided boundary; publishes the double-
 *     buffered slices, bumps done_seq (release).
 *   - poll (render thread): takes the newest completed result via the
 *     seq-ordered acquire chain, or returns false.
 */
#if LIBFLAC_ENABLED == 1

#define PLAYBACK_ANALYSIS_SPAN_MIN_SECONDS 0.1
#define PLAYBACK_ANALYSIS_SPAN_MAX_SECONDS 60.0

// Segment seektable gap (the recorder's seektable point spacing — see
// flac_writer.c seektable spacing; keep in sync). Each parallel segment pays
// one gap of forward decode after its seek; parallel decoding only wins when
// the window span amortizes that cost across segments.
#define PLAYBACK_ANALYSIS_SEEK_GAP_SAMPLES (8388608ULL)

// playback_analysis_t, s_analysis and the shutdown forward declaration
// live at the IO layer section above (gui_playback_stop calls the shutdown).

#if LIBFFTW_ENABLED
static void gui_playback_analysis_flush_column(playback_analysis_segment_t *s);
#endif
static bool gui_playback_analysis_init(playback_analysis_t *a);

// Linear resample of a magnitude frame (src -> dst bins), same math as the
// spectrogram panel's resample_magnitude so analysis spectra render 1:1.
static void gui_playback_analysis_resample(const float *src, int n_src, float *dst, int n_dst) {
    if (!dst || n_dst <= 0) return;
    if (!src || n_src <= 0) {
        for (int i = 0; i < n_dst; i++) dst[i] = 0.0f;
        return;
    }
    if (n_src == 1) {
        for (int i = 0; i < n_dst; i++) dst[i] = src[0];
        return;
    }
    if (n_dst == 1) {
        dst[0] = src[n_src / 2];
        return;
    }
    for (int i = 0; i < n_dst; i++) {
        float pos = (float)i * (float)(n_src - 1) / (float)(n_dst - 1);
        if (pos < 0.0f) pos = 0.0f;
        int i0 = (int)pos;
        if (i0 >= n_src - 1) { dst[i] = src[n_src - 1]; continue; }
        float frac = pos - (float)i0;
        dst[i] = src[i0] * (1.0f - frac) + src[i0 + 1] * frac;
    }
}

// Segment write callback: accumulate EVERY decoded sample contiguously into
// the current column's scratch; when the column's hop is full, flush it (one
// true FFT over the column's full contiguous samples). No sub-window
// sampling, no averaging, no max-hold — the result is a true full-resolution
// spectrogram column, exactly like an FFT-size setting in a DAW (the span
// trades time vs frequency resolution, never detail).
static FLAC__StreamDecoderWriteStatus gui_playback_analysis_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    playback_analysis_segment_t *s = io ? (playback_analysis_segment_t *)io->owner : NULL;
    if (!s || !frame || !buffer || !buffer[0] || !s->dst) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
#if LIBFFTW_ENABLED
    uint32_t samples = frame->header.blocksize;
    if (samples == 0) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    const FLAC__int32 *in = buffer[0];
    for (uint32_t i = 0; i < samples; i++) {
        if (s->slices_done >= s->slice_count) break;
        if (s->col_pos < s->hop) {
            s->fft_in[s->col_pos] = (float)in[i];
            s->col_dc_sum += (int64_t)in[i];
            s->col_pos++;
            if (s->col_pos == s->hop) {
                gui_playback_analysis_flush_column(s);
            }
        } else {
            // Defensive: hop already filled (flush pending); skip.
            s->col_pos++;
        }
    }
#endif
    s->decoded += samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void gui_playback_analysis_meta_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
    (void)decoder; (void)metadata; (void)client_data;
}

static void gui_playback_analysis_error_cb(
    const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    (void)decoder;
    playback_io_t *io = (playback_io_t *)client_data;
    playback_analysis_segment_t *s = io ? (playback_analysis_segment_t *)io->owner : NULL;
    if (s && s->errors_logged < 5) {
        s->errors_logged++;
        fprintf(stderr, "[PLAYBACK] Analysis decoder error: %s\n",
                FLAC__StreamDecoderErrorStatusString[status]);
    }
}

// Decode one segment: seek to its start, decode its span. The final
// PARTIAL column (EOF or segment end) is flushed zero-padded so no sample
// is dropped; columns an EOF cut short stay zero (buffer pre-zeroed).
static void gui_playback_analysis_segment_decode(playback_analysis_segment_t *s) {
    if (!s || !s->decoder_ready || !s->decoder || s->slice_count == 0) return;
    if (!FLAC__stream_decoder_seek_absolute(s->decoder, s->seg_start)) {
        return;
    }
    while (s->decoded < s->seg_span &&
           FLAC__stream_decoder_get_state(s->decoder) !=
                FLAC__STREAM_DECODER_END_OF_STREAM) {
        if (!FLAC__stream_decoder_process_single(s->decoder)) break;
    }
#if LIBFFTW_ENABLED
    if (s->col_pos > 0 && s->slices_done < s->slice_count) {
        gui_playback_analysis_flush_column(s);
    }
#endif
}

#if LIBFFTW_ENABLED
// Flush the current column: DC-remove + Hanning-window the accumulated
// contiguous samples, zero-pad to the plan size, execute ONE true FFT,
// normalize to 0-1 intensity (same dB scale as gui_fft), and resample the
// hop/2+1 meaningful bins into the fixed-width result frame. This is the
// full-resolution core: one FFT per column covering the column's entire
// contiguous sample span.
static void gui_playback_analysis_flush_column(playback_analysis_segment_t *s) {
    if (!s || !s->dst || !s->fft_plan || !s->fft_in || !s->fft_out || !s->frame_buf) return;
    size_t n = s->col_pos;
    if (n > s->hop) n = s->hop;
    if (n == 0) return;
    int pad = s->fft_plan_size;

    // DC removal over the ACTUAL samples.
    float dc = (float)s->col_dc_sum / (float)n;
    const float scale = 1.0f / 2048.0f;   // matches gui_fft int16 normalization
    // Hanning window + normalize (same shape as gui_fft: hann(i) = 0.5*(1-cos)).
    for (size_t i = 0; i < n; i++) {
        float w = n > 1 ? 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1))) : 1.0f;
        s->fft_in[i] = (s->fft_in[i] - dc) * scale * w;
    }
    // Zero-pad to the plan size.
    for (int i = (int)n; i < pad; i++) {
        s->fft_in[i] = 0.0f;
    }

    fftwf_execute(s->fft_plan);

    // Magnitude in dB -> 0-1 intensity (same normalization as gui_fft).
    // hop_bins scales with the span (up to HOP_MAX/2+1 = 131073): use the
    // per-segment HEAP frame_buf — a fixed 2049-float stack buffer here
    // overflowed the thread stack on large spans (verified segfault).
    const int db_min = -120;
    const int db_max = 0;
    int hop_bins = (int)(s->hop / 2) + 1;
    if (hop_bins > pad / 2 + 1) hop_bins = pad / 2 + 1;
    if (hop_bins > (int)(PLAYBACK_ANALYSIS_HOP_MAX / 2 + 1)) {
        hop_bins = (int)(PLAYBACK_ANALYSIS_HOP_MAX / 2 + 1);
    }
    for (int j = 0; j < hop_bins; j++) {
        float re = s->fft_out[j][0];
        float im = s->fft_out[j][1];
        float mag = sqrtf(re * re + im * im);
        mag /= (float)n;       // normalize by actual sample count
        mag *= 2.0f;          // Hanning coherent gain correction
        float db = 20.0f * log10f(mag + 1e-10f);
        if (db < (float)db_min) db = (float)db_min;
        if (db > (float)db_max) db = (float)db_max;
        s->frame_buf[j] = (db - (float)db_min) / (float)(db_max - db_min);
    }
    float *cell = s->dst +
        (size_t)(s->slice_first + s->slices_done) * (size_t)PLAYBACK_ANALYSIS_BINS;
    gui_playback_analysis_resample(s->frame_buf, hop_bins, cell, PLAYBACK_ANALYSIS_BINS);

    s->slices_done++;
    s->col_pos = 0;
    s->col_dc_sum = 0;
}
#endif

static int gui_playback_analysis_segment_thread(void *arg) {
    gui_playback_analysis_segment_decode((playback_analysis_segment_t *)arg);
    return 0;
}

static int gui_playback_analysis_worker(void *arg) {
    playback_analysis_t *a = (playback_analysis_t *)arg;
    while (!atomic_load(&a->worker_stop)) {
        gui_playback_analysis_lock(a);
        uint64_t req = atomic_load(&a->req_seq);
        uint64_t span = a->req_span;
        uint64_t center = a->req_center;
        uint64_t total = a->req_total;
        gui_playback_analysis_unlock(a);
        if (req == 0 || req == atomic_load(&a->done_seq)) {
            thrd_sleep_ms(10);
            continue;
        }
        // Lazy RESOURCE init runs HERE (worker thread), never on the render
        // thread: the init opens the file 8x + parses metadata per handle,
        // which is file IO the render loop must never block on. (The worker
        // thread itself is created separately — init creates no threads.)
        if (!a->inited && !a->failed) {
            a->total_samples = total;
            if (!gui_playback_analysis_init(a)) {
                atomic_store(&a->done_seq, req);
                continue;
            }
        }
        if (!a->inited) {
            atomic_store(&a->done_seq, req);
            continue;
        }
        // Request params were published before req_seq was bumped.
        if (total == 0) {
            atomic_store(&a->done_seq, req);
            continue;
        }
        if (span > total) span = total;
        if (span == 0) span = 1;
        if (center >= total) center = total - 1;
        uint64_t half = span / 2;
        uint64_t start = (center > half) ? (center - half) : 0;
        if (start + span > total) {
            start = (total > span) ? (total - span) : 0;
        }
        a->win_start = start;
        a->win_span = span;
        // Full-resolution contiguous hop: each column = ONE FFT over the
        // column's entire contiguous samples. hop caps at HOP_MAX (per-column
        // FFT budget); spans beyond the cap clamp to hop = HOP_MAX coverage.
        size_t hop = (size_t)((span + PLAYBACK_ANALYSIS_SLICES - 1) /
                            (uint64_t)PLAYBACK_ANALYSIS_SLICES);
        if (hop == 0) hop = 1;
        if (hop > PLAYBACK_ANALYSIS_HOP_MAX) hop = PLAYBACK_ANALYSIS_HOP_MAX;
        a->hop = hop;

        // Pre-zero the unpublished magnitude buffer.
        gui_playback_analysis_lock(a);
        int dst_idx = 0;
        while (dst_idx == atomic_load(&a->publish_idx) || dst_idx == a->reader_idx) dst_idx++;
        gui_playback_analysis_unlock(a);
        float *dst = a->mag_buf[dst_idx];
        memset(dst, 0, (size_t)PLAYBACK_ANALYSIS_SLICES *
                              (size_t)PLAYBACK_ANALYSIS_BINS * sizeof(float));

        // Padded FFT size (next power of two >= hop).
        int pad = 1;
        while (pad < (int)hop) pad <<= 1;

        // Column-aligned segment layout (segments never share columns).
        size_t segs = (a->seg_count > 0) ? a->seg_count : 1;
        size_t per = (PLAYBACK_ANALYSIS_SLICES + segs - 1) / segs;
        size_t active = 0;
        for (size_t i = 0; i < a->seg_count; i++) {
            playback_analysis_segment_t *s = &a->segs[i];
            size_t first = i * per;
            if (first >= PLAYBACK_ANALYSIS_SLICES || !s->decoder_ready) {
                s->slice_count = 0;
                continue;
            }
            size_t count = per;
            if (first + count > PLAYBACK_ANALYSIS_SLICES) {
                count = PLAYBACK_ANALYSIS_SLICES - first;
            }
            s->slice_first = first;
            s->slice_count = count;
            s->hop = hop;
#if LIBFFTW_ENABLED
            // (Re)create this segment's FFTW plan for the padded size — in
            // THIS worker thread, sequentially (plan creation is not
            // thread-safe; executing distinct plans is).
            if (s->fft_plan_size != pad) {
                if (s->fft_plan) {
                    gui_fft_plan_destroy(s->fft_plan);
                    s->fft_plan = NULL;
                }
                s->fft_plan = gui_fft_plan_create_r2c(pad, s->fft_in, s->fft_out,
                                                    FFTW_ESTIMATE | FFTW_DESTROY_INPUT);
                s->fft_plan_size = s->fft_plan ? pad : 0;
            }
#endif
            s->seg_start = start + (uint64_t)first * (uint64_t)hop;
            s->seg_span = (uint64_t)count * (uint64_t)hop;
            active++;
        }
        // Extend the FINAL active segment to the window end so the last
        // slice's FFT data is complete even when stride < slice samples.
        if (active > 0) {
            for (size_t i = a->seg_count; i > 0; i--) {
                playback_analysis_segment_t *s = &a->segs[i - 1];
                if (s->slice_count == 0) continue;
                uint64_t seg_end = s->seg_start + s->seg_span;
                uint64_t win_end = start + span;
                if (seg_end < win_end) s->seg_span += win_end - seg_end;
                break;
            }
        }
        for (size_t i = 0; i < a->seg_count; i++) {
            playback_analysis_segment_t *s = &a->segs[i];
            if (s->slice_count == 0) continue;
            s->decoded = 0;
            s->slices_done = 0;
            s->col_pos = 0;
            s->col_dc_sum = 0;
            s->dst = dst;
        }

        if (active == 0) {
            gui_playback_analysis_lock(a);
            atomic_store(&a->publish_idx, dst_idx);
            atomic_store(&a->done_seq, req);
            gui_playback_analysis_unlock(a);
            continue;
        }

        // Parallel decode when the window is large enough to amortize each
        // segment's seektable-gap forward decode; sequential for small
        // windows (the per-segment gap cost would exceed the window itself).
        bool parallel =
            span > (2 * PLAYBACK_ANALYSIS_SEEK_GAP_SAMPLES) && active > 1;
        thrd_t tids[PLAYBACK_ANALYSIS_DECODE_SEGMENTS];
        size_t spawned = 0;
        for (size_t i = 0; i < a->seg_count; i++) {
            playback_analysis_segment_t *s = &a->segs[i];
            if (s->slice_count == 0) continue;
            if (parallel &&
                thrd_create_with_priority(&tids[spawned],
                                          gui_playback_analysis_segment_thread,
                                          s, THRD_PRIORITY_NORMAL) == thrd_success) {
                spawned++;
            } else {
                gui_playback_analysis_segment_decode(s);
            }
        }
        for (size_t i = 0; i < spawned; i++) {
            thrd_join(tids[i], NULL);
        }
        gui_playback_analysis_lock(a);
        atomic_store(&a->publish_idx, dst_idx);
        atomic_store(&a->done_seq, req);
        gui_playback_analysis_unlock(a);
    }
    return 0;
}

static void gui_playback_analysis_shutdown(playback_analysis_t *a) {
    if (!a) return;
    if (a->worker_valid) {
        atomic_store(&a->worker_stop, true);
        thrd_join(a->worker, NULL);
        a->worker_valid = false;
    }
    gui_playback_analysis_release(a);
    memset(a, 0, sizeof(*a));
}

static void gui_playback_analysis_release(playback_analysis_t *a) {
    if (!a) return;
    for (size_t i = 0; i < PLAYBACK_ANALYSIS_DECODE_SEGMENTS; i++) {
        playback_analysis_segment_t *s = &a->segs[i];
        if (s->decoder) {
            FLAC__stream_decoder_finish(s->decoder);
            FLAC__stream_decoder_delete(s->decoder);
            s->decoder = NULL;
        }
        if (s->file) {
            fclose(s->file);
            s->file = NULL;
        }
#if LIBFFTW_ENABLED
        if (s->fft_plan) {
            gui_fft_plan_destroy(s->fft_plan);
            s->fft_plan = NULL;
        }
        if (s->fft_in) {
            gui_fft_free(s->fft_in);
            s->fft_in = NULL;
        }
        if (s->fft_out) {
            gui_fft_free(s->fft_out);
            s->fft_out = NULL;
        }
        free(s->frame_buf);
        s->frame_buf = NULL;
        s->fft_plan_size = 0;
#endif
        s->decoder_ready = false;
    }
    for (size_t i = 0; i < 3; i++) {
        free(a->mag_buf[i]);
        a->mag_buf[i] = NULL;
    }
    a->seg_count = 0;
    atomic_store(&a->inited, false);
}

static bool gui_playback_analysis_init(playback_analysis_t *a) {
    const char *path = a->session_path;
    uint64_t total = a->total_samples;
    if (!path || !path[0] || total == 0) {
        a->failed = true;
        return false;
    }
    for (size_t i = 0; i < 3; i++) {
        a->mag_buf[i] = (float *)calloc(
            (size_t)PLAYBACK_ANALYSIS_SLICES * PLAYBACK_ANALYSIS_BINS, sizeof(float));
    }
    if (!a->mag_buf[0] || !a->mag_buf[1] || !a->mag_buf[2]) {
        fprintf(stderr, "[PLAYBACK] Analysis: failed to allocate magnitude buffers\n");
        gui_playback_analysis_release(a);
        a->failed = true;
        return false;
    }
    a->total_samples = total;

    // One decoder + FILE handle per parallel segment (libFLAC decoders are
    // single-threaded; segments decode disjoint slice ranges concurrently).
    // Partial pools work: fewer segments = less parallelism, same result.
    for (size_t i = 0; i < PLAYBACK_ANALYSIS_DECODE_SEGMENTS; i++) {
        playback_analysis_segment_t *s = &a->segs[i];
        s->file = file_open_read_utf8(path);
        if (!s->file) break;
        int channel = a == &s_analysis[0] ? 0 : 1;
        if (a->owner->loaded_key_valid[channel] &&
            !gui_playback_length_cache_matches_open_file(s->file, &a->owner->loaded_key[channel])) {
            fclose(s->file);
            s->file = NULL;
            break;
        }
        // The IO layer patches the STREAMINFO total with the TRUE stream
        // total (writing 0 when it exceeds the 36-bit field) so seeks work
        // across the real stream — same rules as the playback decoders.
        gui_playback_io_reset(&s->io, s->file, s, PLAYBACK_IO_SI_TOTAL_MOD);
        s->io.cancel = &a->worker_stop;
        s->decoder = FLAC__stream_decoder_new();
        if (!s->decoder) break;
        FLAC__stream_decoder_set_md5_checking(s->decoder, false);
        FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_stream(
            s->decoder,
            gui_playback_io_read_cb,
            gui_playback_io_seek_cb,
            gui_playback_io_tell_cb,
            gui_playback_io_length_cb,
            gui_playback_io_eof_cb,
            gui_playback_analysis_write_cb,
            gui_playback_analysis_meta_cb,
            gui_playback_analysis_error_cb,
            &s->io);
        if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            fprintf(stderr, "[PLAYBACK] Analysis: segment init failed: %s\n",
                    FLAC__StreamDecoderInitStatusString[st]);
            break;
        }
        if (!FLAC__stream_decoder_process_until_end_of_metadata(s->decoder)) {
            fprintf(stderr, "[PLAYBACK] Analysis: segment metadata pass failed\n");
            break;
        }
#if LIBFFTW_ENABLED
        // Per-segment FFTW scratch (max-hop sized once; plans are created
        // per-request in the worker thread where creation is sequential).
        s->fft_in = (float *)gui_fft_alloc(sizeof(float) * (size_t)PLAYBACK_ANALYSIS_HOP_MAX);
        s->fft_out = (fftwf_complex *)gui_fft_alloc(
            sizeof(fftwf_complex) * (size_t)(PLAYBACK_ANALYSIS_HOP_MAX / 2 + 1));
        s->frame_buf = (float *)malloc(
            sizeof(float) * (size_t)(PLAYBACK_ANALYSIS_HOP_MAX / 2 + 1));
        if (!s->fft_in || !s->fft_out || !s->frame_buf) {
            fprintf(stderr, "[PLAYBACK] Analysis: segment FFT scratch alloc failed\n");
            break;
        }
        s->fft_plan = NULL;
        s->fft_plan_size = 0;
#endif
        s->hop = 1;
        s->col_pos = 0;
        s->col_dc_sum = 0;
        s->decoder_ready = true;
        a->seg_count++;
    }
    if (a->seg_count == 0) {
        fprintf(stderr, "[PLAYBACK] Analysis: no segment decoders (%s)\n", path);
        gui_playback_analysis_release(a);
        a->failed = true;
        return false;
    }
    // Resources ready. The WORKER thread is created separately (cheap, no
    // file IO) by the request path; this init creates no threads.
    a->inited = true;
    return true;
}

#endif // LIBFLAC_ENABLED

void gui_playback_request_analysis_window(gui_app_t *app, int channel,
                                           uint64_t center_sample,
                                           double span_seconds) {
    (void)app;
    if (channel != 0 && channel != 1) return;
#if LIBFLAC_ENABLED == 1
    playback_analysis_t *a = &s_analysis[channel];
    if (a->failed) return;
    if (!(span_seconds >= PLAYBACK_ANALYSIS_SPAN_MIN_SECONDS)) {
        span_seconds = PLAYBACK_ANALYSIS_SPAN_MIN_SECONDS;
    }
    double rate = (channel == 0) ? (double)s_playback->info_a.real_sample_rate_hz
                                  : (double)s_playback->info_b.real_sample_rate_hz;
    uint64_t total = (channel == 0) ? s_playback->info_a.total_samples
                                    : s_playback->info_b.total_samples;
    FILE *file = channel == 1 ? s_playback->file_b : s_playback->file_a;
    if (!atomic_load(&s_playback->running) || !file || !(rate > 0.0) || total == 0) return;
    if (!a->worker_valid) {
        a->owner = s_playback;
        // The analysis decoders need a NATIVE FLAC path: the Ogg-FLAC session
        // remux (decode_path), which equals the info filepath for every
        // on-disk format. Raw channels have no FLAC analysis (their session
        // init fails cleanly and reports unavailability).
        const char *session = channel == 1
            ? (s_playback->decode_path[1][0] ? s_playback->decode_path[1] : s_playback->info_b.filepath)
            : (s_playback->decode_path[0][0] ? s_playback->decode_path[0] : s_playback->info_a.filepath);
        snprintf(a->session_path, sizeof(a->session_path), "%s", session);
        a->reader_idx = -1;
    }
    // Worker thread creation is cheap (no file IO): safe on the render
    // thread. The worker lazily does the file-IO init on the first request.
    if (!a->worker_valid) {
        atomic_store(&a->worker_stop, false);
        if (thrd_create_with_priority(&a->worker, gui_playback_analysis_worker, a,
                                      THRD_PRIORITY_NORMAL) != thrd_success) {
            fprintf(stderr, "[PLAYBACK] Analysis: worker create failed\n");
            a->failed = true;
            return;
        }
        a->worker_valid = true;
    }

    // Full-resolution span cap: the contiguous model needs hop = span/SLICES
    // <= HOP_MAX samples per column FFT, so the span caps at
    // SLICES x HOP_MAX / rate (duration-based: rate-dependent, e.g. ~6.7 s
    // at 20 MSPS, ~13.4 s at 10 MSPS, ~26.8 s at 5 MSPS).
    double span_max =
        (double)PLAYBACK_ANALYSIS_SLICES * (double)PLAYBACK_ANALYSIS_HOP_MAX / rate;
    if (span_seconds > span_max) {
        span_seconds = span_max;
    }
    if (center_sample >= total) center_sample = total - 1;
    gui_playback_analysis_lock(a);
    a->req_center = center_sample;
    a->req_total = total;
    a->req_span = (uint64_t)(span_seconds * rate);
    if (a->req_span == 0) a->req_span = 1;
    atomic_fetch_add(&a->req_seq, 1);
    gui_playback_analysis_unlock(a);
#else
    (void)center_sample;
    (void)span_seconds;
#endif
}

bool gui_playback_poll_analysis_window(gui_app_t *app, int channel,
                                        uint64_t *io_seq,
                                        const float **out_magnitudes,
                                        size_t *out_slice_count,
                                        size_t *out_bins) {
    (void)app;
    if (channel != 0 && channel != 1) return false;
#if LIBFLAC_ENABLED == 1
    playback_analysis_t *a = &s_analysis[channel];
    FILE *file = channel == 1 ? s_playback->file_b : s_playback->file_a;
    const playback_file_info_t *info = channel == 1 ? &s_playback->info_b : &s_playback->info_a;
    if (!atomic_load(&s_playback->running) || !file || !info->valid || info->total_samples == 0 ||
        a->owner != s_playback || !atomic_load(&a->inited)) return false;
    gui_playback_analysis_lock(a);
    uint64_t done = atomic_load(&a->done_seq);
    uint64_t seen = (io_seq ? *io_seq : 0);
    if (done == 0 || done <= seen) {
        gui_playback_analysis_unlock(a);
        return false;
    }
    if (io_seq) *io_seq = done;
    a->reader_idx = atomic_load(&a->publish_idx);
    if (out_magnitudes) *out_magnitudes = a->mag_buf[a->reader_idx];
    if (out_slice_count) *out_slice_count = PLAYBACK_ANALYSIS_SLICES;
    if (out_bins) *out_bins = (size_t)PLAYBACK_ANALYSIS_BINS;
    gui_playback_analysis_unlock(a);
    return true;
#else
    (void)io_seq; (void)out_magnitudes; (void)out_slice_count; (void)out_bins;
    return false;
#endif
}
