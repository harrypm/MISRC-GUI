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

typedef struct playback_ctx {
    // File handles and decoders
#if LIBFLAC_ENABLED == 1
    FLAC__StreamDecoder *decoder_a;
    FLAC__StreamDecoder *decoder_b;
    playback_io_t io_a;         // Per-channel IO layer (callbacks + patch state)
    playback_io_t io_b;
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

    // FLAC returns samples as int32 with the full bit depth.
    // Recorder conventions (gui_record.c):
    //   16-bit FLAC: 12-bit ADC container << 4 (range -32768 to 32752)
    //   12-bit FLAC: 12-bit ADC container as-is (range -2048 to 2047)
    //   8-bit FLAC:  12-bit ADC container >> 4 (range -128 to 127)
    // Playback restores the 12-bit container for EVERY depth so the raw
    // packing, extraction ((2047 - field) << 4 into the 16-bit display
    // container), VU meters, peaks and clipping all match the live capture
    // preview 1:1 (verified against the recorder's own scaling code).
    if (bps == 16) {
        // 16-bit FLAC: shift right 4 to get back to 12-bit range
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] >> 4);
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

    // Same conversion as Channel A - restore the 12-bit container for every
    // depth so the display matches the live capture preview 1:1.
    if (bps == 16) {
        // 16-bit FLAC: shift right 4 to get back to 12-bit range
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] >> 4);
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
            // Validate the requested playhead before seeking its backfill
            // window: a provisional endpoint can exceed the real stream.
            if ((paused && seek_target != target && !gui_playback_seek_decoder(ctx,0,target)) ||
                !gui_playback_seek_decoder(ctx,0,seek_target)) {
                fprintf(stderr,"[PLAYBACK] Channel A seek failed at sample %llu\n",(unsigned long long)target);
                if (ctx->seek_recovery_failed[0]) atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED);
                deadline_us = 0; continue;
            }
#endif
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
            if ((paused && seek_target != target && !gui_playback_seek_decoder(ctx,1,target)) ||
                !gui_playback_seek_decoder(ctx,1,seek_target)) {
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

        // Decode channel B
        while (channel_b_active && ctx->decoder_b && ctx->decode_available_b - ctx->decode_pos_b < RAW_BUFFER_SAMPLES) {
            if (FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(ctx->decoder_b)) {
                break;
            }
        }
#endif

        // Calculate how many samples we can output
        size_t avail_a = ctx->decode_available_a - ctx->decode_pos_a;
        size_t avail_b = ctx->decode_available_b - ctx->decode_pos_b;
        size_t samples_to_output = RAW_BUFFER_SAMPLES;

        // Check for EOF on each channel independently and loop each one
#if LIBFLAC_ENABLED == 1
        bool eof_a = channel_a_active && (ctx->decoder_a != NULL) && (avail_a == 0 &&
                      FLAC__stream_decoder_get_state(ctx->decoder_a) == FLAC__STREAM_DECODER_END_OF_STREAM);
        bool eof_b = channel_b_active && (ctx->decoder_b != NULL) && (avail_b == 0 &&
                      FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM);

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
            if (!gui_playback_seek_decoder(ctx,0,0)) {
                fprintf(stderr,"[PLAYBACK] Channel A loop seek failed\n");
                atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED); continue;
            }
            atomic_store(&ctx->current_sample_a, 0);
            epochs[0] = atomic_fetch_add(&app->rf_stream_epoch, 1) + 1;
            deadline_us = 0;
        }

        // Loop channel B independently (a paused backfill that reaches the
        // file end just stops the backfill instead of looping).
        if (eof_b) {
            fprintf(stderr, "[PLAYBACK] Channel B EOF, looping\n");
            if (!gui_playback_seek_decoder(ctx,1,0)) {
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
#endif
    if (ctx->file_a && !ctx->decoder_owns_file_a) fclose(ctx->file_a);
    if (ctx->file_b && !ctx->decoder_owns_file_b) fclose(ctx->file_b);
    ctx->file_a = NULL; ctx->file_b = NULL;
    free(ctx->decode_buf_a); free(ctx->decode_buf_b);
    ctx->decode_buf_a = NULL; ctx->decode_buf_b = NULL;
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
    if (!gui_playback_length_cache_snapshot(info->filepath, &path_key)) return true;
    return gui_playback_length_cache_keys_equal(&path_key, &ctx->loaded_key[channel]);
}
#endif

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

    // Open and initialize decoders
    if (file_a) {
        if (file_a != ctx->info_a.filepath)
            snprintf(ctx->info_a.filepath, sizeof(ctx->info_a.filepath), "%s", file_a);

        ctx->file_a = file_open_read_utf8(file_a);
        if (!ctx->file_a) {
            fprintf(stderr, "[PLAYBACK] Failed to open file A: %s\n", file_a);
            return -1;
        }
        if (gui_playback_length_cache_snapshot(file_a, &ctx->loaded_key[0])) {
            if (!gui_playback_length_cache_matches_open_file(ctx->file_a, &ctx->loaded_key[0])) return -1;
            ctx->loaded_key_valid[0] = true;
        }

        ctx->decoder_a = FLAC__stream_decoder_new();
        if (!ctx->decoder_a) {
            fprintf(stderr, "[PLAYBACK] Failed to create decoder A\n");
            return -1;
        }

        FLAC__stream_decoder_set_md5_checking(ctx->decoder_a, false);

        // Unknown STREAMINFO total removes finalized RF/provisional endpoint
        // clamps while the UI independently tracks the resolved true length.
        gui_playback_io_reset(&ctx->io_a, ctx->file_a, ctx, PLAYBACK_IO_SI_TOTAL_MOD);
        ctx->io_a.cancel = cancel;
        FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
            ctx->decoder_a,
            gui_playback_io_read_cb,
            gui_playback_io_seek_cb,
            gui_playback_io_tell_cb,
            gui_playback_io_length_cb,
            gui_playback_io_eof_cb,
            decoder_io_write_cb_a,
            decoder_io_metadata_cb_a,
            decoder_error_cb,
            &ctx->io_a
        );

        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            fprintf(stderr, "[PLAYBACK] Failed to init decoder A: %s\n",
                    FLAC__StreamDecoderInitStatusString[status]);
            return -1;
        }

        // Process metadata to get file info
        if (!FLAC__stream_decoder_process_until_end_of_metadata(ctx->decoder_a) ||
            !ctx->info_a.valid || ctx->stream_a.channels != 1) return -1;
        if (cancel && atomic_load(cancel)) return -1;
        ctx->channel_rate_hz[0] = gui_playback_apply_resolved_file_info(
            &ctx->info_a, &ctx->stream_a, "File A", cancel,
            ctx->loaded_key_valid[0] ? &ctx->loaded_key[0] : NULL, ctx->file_a);
    }

    if (cancel && atomic_load(cancel)) return -1;

    if (file_b) {
        if (file_b != ctx->info_b.filepath)
            snprintf(ctx->info_b.filepath, sizeof(ctx->info_b.filepath), "%s", file_b);

        ctx->file_b = file_open_read_utf8(file_b);
        if (!ctx->file_b) {
            fprintf(stderr, "[PLAYBACK] Failed to open file B: %s\n", file_b);
            return -1;
        }
        if (gui_playback_length_cache_snapshot(file_b, &ctx->loaded_key[1])) {
            if (!gui_playback_length_cache_matches_open_file(ctx->file_b, &ctx->loaded_key[1])) return -1;
            ctx->loaded_key_valid[1] = true;
        }

        ctx->decoder_b = FLAC__stream_decoder_new();
        if (!ctx->decoder_b) {
            fprintf(stderr, "[PLAYBACK] Failed to create decoder B\n");
            return -1;
        }

        FLAC__stream_decoder_set_md5_checking(ctx->decoder_b, false);

        // Unknown STREAMINFO total removes finalized RF/provisional endpoint
        // clamps while the UI independently tracks the resolved true length.
        gui_playback_io_reset(&ctx->io_b, ctx->file_b, ctx, PLAYBACK_IO_SI_TOTAL_MOD);
        ctx->io_b.cancel = cancel;
        FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
            ctx->decoder_b,
            gui_playback_io_read_cb,
            gui_playback_io_seek_cb,
            gui_playback_io_tell_cb,
            gui_playback_io_length_cb,
            gui_playback_io_eof_cb,
            decoder_io_write_cb_b,
            decoder_io_metadata_cb_b,
            decoder_error_cb,
            &ctx->io_b
        );

        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            fprintf(stderr, "[PLAYBACK] Failed to init decoder B: %s\n",
                    FLAC__StreamDecoderInitStatusString[status]);
            return -1;
        }

        if (!FLAC__stream_decoder_process_until_end_of_metadata(ctx->decoder_b) ||
            !ctx->info_b.valid || ctx->stream_b.channels != 1) return -1;
        if (cancel && atomic_load(cancel)) return -1;
        ctx->channel_rate_hz[1] = gui_playback_apply_resolved_file_info(
            &ctx->info_b, &ctx->stream_b, "File B", cancel,
            ctx->loaded_key_valid[1] ? &ctx->loaded_key[1] : NULL, ctx->file_b);
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
    app->capture_start_time = GetTime();
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
        snprintf(a->session_path, sizeof(a->session_path), "%s", channel == 1
            ? s_playback->info_b.filepath : s_playback->info_a.filepath);
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
