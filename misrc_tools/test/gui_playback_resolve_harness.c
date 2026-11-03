/*
 * CPU-only regression tests for the playback file-info resolution, the
 * decoder IO layer (STREAMINFO patch), the input sample-convention detection
 * and the new playback reader formats. Include the implementation to
 * exercise its private probe/IO helpers. No GUI, window or capture backend
 * is needed; framework symbols are stubbed.
 *
 * Two on-disk FLAC header conventions are synthesized with libFLAC and must
 * both resolve to the TRUE stream sample count and real RF rate:
 *   OLD (pre-finalize): STREAMINFO total = raw sample count, header rate is
 *                       a kHz-domain value (20000 = 20 MSPS).
 *   NEW (finalized):    STREAMINFO total = samples/1000 (kHz domain, so the
 *                       header duration is truthful) + RF vorbis tags.
 * The NEW convention previously made libFLAC reject every seek past the
 * header total; the IO layer must patch the decoder-visible STREAMINFO so
 * seeking works across the real stream.
 *
 * The reader tests (FLAC-Chop's supported-input model) cover: 16-bit source
 * conventions (recorder 12-bit << 4 grid vs direct-domain values vs
 * full-range), headerless raw PCM (.u8/.s8/.u16/.s16, the reversed
 * .8u/.8s/.16u/.16s, and .raw/.bin/.pcm defaulting to u8) with the REQUIRED
 * <n>msps hint, PCM WAV (audio/RF-/1000/real-MHz rates, data-size clamp,
 * stereo rejection), packed 10-bit .lds (group unpack, mid-group seek,
 * trailing-byte drop, 40 MSPS default + hint override), Ogg FLAC
 * (.ldf/.oga: mapping-header parse, exact last-page granule totals on
 * zero-total STREAMINFO, the RF_SAMPLE_RATE tag, truncated tails, nil EOS
 * packets versus nonempty 255-byte frame completion, malformed audio, and a
 * full remux-to-session-temp prepare that decodes sample-exact), and
 * magic-first sniffing with specific errors (non-FLAC Ogg codecs,
 * ID3-prefixed FLAC, unknown types).
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <wchar.h>
#endif

#include "FLAC/stream_encoder.h"
#include "FLAC/metadata.h"

#include "../misrc_gui/input/gui_playback.c"

// ---- Framework stubs (not under test) ----
void gui_app_set_status(gui_app_t *app, const char *message) { (void)app; (void)message; }
void gui_extract_prepare_stream(gui_app_t *app) { (void)app; }
int gui_extract_start_tagged(gui_app_t *app) { (void)app; return 0; }
bool gui_extract_stamp_playback(const gui_rf_block_info_t info[2], const size_t counts[2])
{ (void)info; (void)counts; return true; }
void gui_extract_stop(void) {}
int gui_display_thread_start(display_thread_t *dt,
                             struct gui_app *app,
                             struct buffer_manager *bufmgr)
{ (void)dt; (void)app; (void)bufmgr; return 0; }
void gui_display_thread_stop(display_thread_t *dt) { (void)dt; }
void bufmgr_reset_stats(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; }
void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id,
                          size_t bytes, const backpressure_policy_t *policy)
{ (void)id; (void)bytes; (void)policy; return mgr ? NULL : NULL; }
void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes)
{ (void)mgr; (void)id; (void)bytes; }
void bufmgr_signal_data(buffer_manager_t *mgr, buffer_id_t id)
{ (void)mgr; (void)id; }
double GetTime(void) { return 0.0; }  /* raylib, not linked */

static int failures;
static int checks;
static const char *test_case;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

static void expect_u64(uint64_t got, uint64_t want, const char *message)
{
    checks++;
    if (got != want) {
        fprintf(stderr, "FAIL [%s]: %s (got %llu, want %llu)\n",
                test_case, message,
                (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void expect_near(double got, double want, const char *message)
{
    checks++;
    if (!(fabs(got - want) < 1.0e-9)) {
        fprintf(stderr, "FAIL [%s]: %s (got %.9f, want %.9f)\n",
                test_case, message, got, want);
        failures++;
    }
}

static void expect_str_contains(const char *got, const char *needle, const char *message)
{
    checks++;
    if (!got || !needle || !strstr(got, needle)) {
        fprintf(stderr, "FAIL [%s]: %s (got \"%s\", want substring \"%s\")\n",
                test_case, message, got ? got : "(null)", needle ? needle : "(null)");
        failures++;
    }
}

static void expect_i64(long long got, long long want, const char *message)
{
    checks++;
    if (got != want) {
        fprintf(stderr, "FAIL [%s]: %s (got %lld, want %lld)\n",
                test_case, message, got, want);
        failures++;
    }
}

// ---- Synthetic capture generation ----

#define TEST_SAMPLE_RATE_KHZ 20000u   /* kHz-domain header rate: 20 MSPS RF */
#define TEST_RF_RATE_HZ 20000000.0
#define TEST_SAMPLES 3000000u         /* 0.15 s at 20 MSPS; NEW total = 3000 */

static bool write_test_flac(const char *path, uint32_t num_samples)
{
    FILE *f = fopen(path, "w+b");
    if (!f) return false;

    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) { fclose(f); return false; }

    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata *pad = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
    if (!vc || !pad) {
        if (vc) FLAC__metadata_object_delete(vc);
        if (pad) FLAC__metadata_object_delete(pad);
        FLAC__stream_encoder_delete(enc);
        fclose(f);
        return false;
    }
    pad->length = 4096;
    FLAC__StreamMetadata *meta[2] = { vc, pad };

    bool ok = FLAC__stream_encoder_set_channels(enc, 1) &&
              FLAC__stream_encoder_set_bits_per_sample(enc, 8) &&
              FLAC__stream_encoder_set_sample_rate(enc, TEST_SAMPLE_RATE_KHZ) &&
              FLAC__stream_encoder_set_compression_level(enc, 0) &&
              FLAC__stream_encoder_set_verify(enc, false) &&
              FLAC__stream_encoder_set_total_samples_estimate(enc, 0) &&
              FLAC__stream_encoder_set_metadata(enc, meta, 2);
    bool encoder_owns_file = false;
    if (ok) {
        ok = FLAC__stream_encoder_init_FILE(enc, f, NULL, NULL) ==
             FLAC__STREAM_ENCODER_INIT_STATUS_OK;
        /* After a successful init_FILE the encoder owns the FILE* and closes
         * it in finish()/delete(). */
        encoder_owns_file = ok;
    }
    if (ok) {
        /* Deterministic 8-bit signed noise-ish pattern. */
        FLAC__int32 buffer[4096];
        uint32_t remaining = num_samples;
        uint32_t seed = 12345;
        while (remaining > 0 && ok) {
            uint32_t block = (remaining > 4096) ? 4096 : remaining;
            for (uint32_t i = 0; i < block; i++) {
                seed = seed * 1103515245u + 12345u;
                buffer[i] = (FLAC__int32)((seed >> 16) & 0xFFu) - 128;
            }
            ok = FLAC__stream_encoder_process_interleaved(enc, buffer, block);
            remaining -= block;
        }
        FLAC__stream_encoder_finish(enc);
        ok = ok && (FLAC__stream_encoder_get_state(enc) == FLAC__STREAM_ENCODER_UNINITIALIZED);
    }

    FLAC__metadata_object_delete(vc);
    FLAC__metadata_object_delete(pad);
    FLAC__stream_encoder_delete(enc);
    if (!encoder_owns_file) {
        fclose(f);
    }
    return ok;
}

static bool append_tag(FLAC__StreamMetadata *block, const char *name, const char *value)
{
    FLAC__StreamMetadata_VorbisComment_Entry entry;
    if (!FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, name, value)) {
        return false;
    }
    if (!FLAC__metadata_object_vorbiscomment_append_comment(block, entry, /*copy=*/false)) {
        free(entry.entry);
        return false;
    }
    return true;
}

/* Mimic the recorder finalize: scale STREAMINFO total to the kHz domain and
 * embed the RF truth in vorbis tags. */
static bool finalize_test_flac(const char *path, uint32_t num_samples)
{
    FLAC__Metadata_SimpleIterator *it = FLAC__metadata_simple_iterator_new();
    if (!it) return false;
    if (!FLAC__metadata_simple_iterator_init(it, path, /*read_only=*/false,
                                             /*preserve_file_stats=*/true)) {
        FLAC__metadata_simple_iterator_delete(it);
        return false;
    }

    bool ok = false;
    do {
        FLAC__StreamMetadata *si = FLAC__metadata_simple_iterator_get_block(it);
        if (!si || si->type != FLAC__METADATA_TYPE_STREAMINFO) {
            if (si) FLAC__metadata_object_delete(si);
            break;
        }
        si->data.stream_info.total_samples = ((uint64_t)num_samples + 500u) / 1000u;
        if (si->data.stream_info.total_samples == 0) si->data.stream_info.total_samples = 1;
        ok = FLAC__metadata_simple_iterator_set_block(it, si, /*use_padding=*/false);
        FLAC__metadata_object_delete(si);
        if (!ok) break;

        while (FLAC__metadata_simple_iterator_next(it)) {
            FLAC__StreamMetadata *blk = FLAC__metadata_simple_iterator_get_block(it);
            if (!blk) continue;
            if (blk->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
                char total_str[32];
                char rate_str[32];
                char rate_khz_str[32];
                char duration_str[64];
                char length_str[32];
                snprintf(total_str, sizeof(total_str), "%u", (unsigned)num_samples);
                snprintf(rate_str, sizeof(rate_str), "%u", (unsigned)TEST_RF_RATE_HZ);
                snprintf(rate_khz_str, sizeof(rate_khz_str), "%u", TEST_SAMPLE_RATE_KHZ);
                snprintf(duration_str, sizeof(duration_str), "%.6f",
                         (double)num_samples / TEST_RF_RATE_HZ);
                snprintf(length_str, sizeof(length_str), "%u",
                         (unsigned)((double)num_samples / TEST_RF_RATE_HZ * 1000.0));
                ok = append_tag(blk, "RF_TOTAL_SAMPLES", total_str) &&
                     append_tag(blk, "RF_SAMPLE_RATE", rate_str) &&
                     append_tag(blk, "RF_SAMPLE_RATE_KHZ", rate_khz_str) &&
                     append_tag(blk, "DURATION_SECONDS", duration_str) &&
                     append_tag(blk, "LENGTH", length_str);
                if (ok) {
                    ok = FLAC__metadata_simple_iterator_set_block(it, blk, /*use_padding=*/true);
                }
                FLAC__metadata_object_delete(blk);
                break;
            }
            FLAC__metadata_object_delete(blk);
        }
    } while (0);

    FLAC__metadata_simple_iterator_delete(it);
    return ok;
}

// Write a 16-bit mono FLAC from a deterministic generator (no metadata
// blocks - keeps the frame splitting below trivial). Used by the Ogg-FLAC
// fixtures and the 16-bit convention cases.
typedef int32_t (*pattern_fn)(uint32_t index);
static int32_t pattern_recorder_x16(uint32_t i)
{
    /* Recorder convention: 12-bit container << 4, all samples on a 16-grid. */
    return (int32_t)((int)(i % 4096u) - 2048) * 16;
}
static int32_t pattern_direct_domain(uint32_t i)
{
    /* Externally RESAMPLED container-domain capture (the Tape_03 class):
     * mostly NOT multiples of 16 (the resample broke the grid), peaks
     * overshooting the capture's rails (resampler ripple). */
    return (int32_t)((int)((i * 7u) % 4381u) - 2190);
}
static int32_t pattern_direct_le2047(uint32_t i)
{
    /* Container-domain values stored raw, within the rails: peaks at
     * +-1450, mostly NOT multiples of 16. */
    return (int32_t)((int)((i * 11u) % 2901u) - 1450);
}
static int32_t pattern_full_range(uint32_t i)
{
    /* Full-range generic 16-bit audio (loud, not on any grid). */
    return (int32_t)((int)((i * 13u) % 60000u) - 30000);
}
static int32_t pattern_zeros(uint32_t i)
{
    (void)i;
    return 0;
}

static bool write_pattern_flac(const char *path, uint32_t num_samples,
                               uint32_t sample_rate, uint32_t block, pattern_fn gen)
{
    FILE *f = fopen(path, "w+b");
    if (!f) return false;
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) { fclose(f); return false; }
    bool ok = FLAC__stream_encoder_set_channels(enc, 1) &&
              FLAC__stream_encoder_set_bits_per_sample(enc, 16) &&
              FLAC__stream_encoder_set_sample_rate(enc, sample_rate) &&
              FLAC__stream_encoder_set_compression_level(enc, 0) &&
              FLAC__stream_encoder_set_blocksize(enc, block) &&
              FLAC__stream_encoder_set_verify(enc, false) &&
              FLAC__stream_encoder_set_total_samples_estimate(enc, num_samples) &&
              FLAC__stream_encoder_init_FILE(enc, f, NULL, NULL) ==
                 FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        FLAC__int32 buffer[8192];
        uint32_t remaining = num_samples;
        while (remaining > 0 && ok) {
            uint32_t n = (remaining > 8192) ? 8192 : remaining;
            for (uint32_t i = 0; i < n; i++) buffer[i] = gen(num_samples - remaining + i);
            ok = FLAC__stream_encoder_process_interleaved(enc, buffer, n);
            remaining -= n;
        }
        FLAC__stream_encoder_finish(enc);
        ok = ok && (FLAC__stream_encoder_get_state(enc) == FLAC__STREAM_ENCODER_UNINITIALIZED);
    }
    FLAC__stream_encoder_delete(enc);  /* owns + closes the FILE on init_FILE */
    return ok;
}

// Reference decoder: collect a native FLAC's raw int32 samples.
typedef struct {
    FLAC__int32 *samples;
    uint64_t count;
    uint64_t cap;
    bool failed;
} decode_ref_t;

static FLAC__StreamDecoderWriteStatus decode_ref_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data)
{
    (void)decoder;
    decode_ref_t *ref = (decode_ref_t *)client_data;
    if (!ref || !frame || !buffer || !buffer[0]) {
        ref->failed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    if (ref->count + frame->header.blocksize > ref->cap) {
        ref->failed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    for (uint32_t i = 0; i < frame->header.blocksize; i++) {
        ref->samples[ref->count++] = buffer[0][i];
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void decode_ref_error_cb(const FLAC__StreamDecoder *decoder,
                                 FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    (void)decoder; (void)client_data;
    fprintf(stderr, "harness decode error: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
}

static bool decode_flac_i32(const char *path, decode_ref_t *ref, uint64_t cap)
{
    memset(ref, 0, sizeof(*ref));
    ref->samples = (FLAC__int32 *)calloc(cap, sizeof(FLAC__int32));
    ref->cap = cap;
    if (!ref->samples) return false;
    FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
    if (!dec) { free(ref->samples); ref->samples = NULL; return false; }
    FLAC__stream_decoder_set_md5_checking(dec, false);
    bool ok = FLAC__stream_decoder_init_FILE(dec, fopen(path, "rb"),
        decode_ref_write_cb, NULL, decode_ref_error_cb, ref) ==
        FLAC__STREAM_DECODER_INIT_STATUS_OK;
    if (ok) {
        ok = FLAC__stream_decoder_process_until_end_of_stream(dec) && !ref->failed;
    }
    FLAC__stream_decoder_finish(dec);   /* also closes the FILE */
    FLAC__stream_decoder_delete(dec);
    if (!ok) {
        free(ref->samples);
        ref->samples = NULL;
        ref->count = 0;
    }
    return ok;
}

static void decode_ref_free(decode_ref_t *ref)
{
    free(ref->samples);
    ref->samples = NULL;
    ref->count = 0;
}

// Build a PCM WAV: RIFF + an odd-sized JUNK chunk (exercises the pad-byte
// walk) + fmt + data. `data_size_field` is written verbatim into the data
// chunk header (0xFFFFFFFF exercises the streaming-writer EOF clamp).
static void wav_test_write(const char *path, uint32_t rate, uint16_t bits,
                           uint16_t channels, uint32_t data_size_field,
                           const uint8_t *data, size_t data_len)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    char junk[11] = "JUNKpayload";  /* odd size: the chunk walk must pad */
    uint8_t fmt[16];
    memset(fmt, 0, sizeof(fmt));
    /* fmt layout: format(2) channels(2) rate(4) byte_rate(4) align(2) bits(2) */
    fmt[0] = 1; fmt[1] = 0;                             /* PCM */
    fmt[2] = (uint8_t)(channels & 0xFF); fmt[3] = (uint8_t)(channels >> 8);
    fmt[4] = (uint32_t)rate & 0xFF;  fmt[5] = ((uint32_t)rate >> 8) & 0xFF;
    fmt[6] = ((uint32_t)rate >> 16) & 0xFF;  fmt[7] = ((uint32_t)rate >> 24) & 0xFF;
    uint16_t block_align = (uint16_t)(channels * (bits / 8));
    uint32_t byte_rate = rate * block_align;
    fmt[8] = (uint32_t)byte_rate & 0xFF;   fmt[9] = (byte_rate >> 8) & 0xFF;
    fmt[10] = (byte_rate >> 16) & 0xFF;   fmt[11] = (byte_rate >> 24) & 0xFF;
    fmt[12] = (uint8_t)(block_align & 0xFF); fmt[13] = (uint8_t)(block_align >> 8);
    fmt[14] = (uint8_t)(bits & 0xFF); fmt[15] = (uint8_t)(bits >> 8);
    uint32_t riff_size = 4 + (8 + sizeof(junk) + 1) + (8 + 16) + (8 + (uint32_t)data_len);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 1, 4, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("JUNK", 1, 4, f);
    uint32_t junk_len = (uint32_t)sizeof(junk);
    fwrite(&junk_len, 1, 4, f);
    fwrite(junk, 1, sizeof(junk), f);
    fputc(0, f);  /* the odd-chunk pad byte */
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_len = 16;
    fwrite(&fmt_len, 1, 4, f);
    fwrite(fmt, 1, sizeof(fmt), f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size_field, 1, 4, f);
    fwrite(data, 1, data_len, f);
    fclose(f);
}

// Pack four 10-bit words (0..1023) into one 5-byte .lds group - the exact
// reference layout (ld-lds-converter / FLAC-Chop pack_group).
static void lds_test_pack(uint8_t out[5], const uint32_t w[4])
{
    out[0] = (uint8_t)((w[0] & 0x03FC) >> 2);
    out[1] = (uint8_t)(((w[0] & 0x0003) << 6) | ((w[1] & 0x03F0) >> 4));
    out[2] = (uint8_t)(((w[1] & 0x000F) << 4) | ((w[2] & 0x03C0) >> 6));
    out[3] = (uint8_t)(((w[2] & 0x003F) << 2) | ((w[3] & 0x0300) >> 8));
    out[4] = (uint8_t)(w[3] & 0x00FF);
}

// ---- Synthetic Ogg FLAC builder (the FLAC-Chop testutil model) ----

static uint32_t ogg_test_crc_table[256];
static void ogg_test_crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; k++) {
            r = (r & 0x80000000u) ? ((r << 1) ^ 0x04C11DB7u) : (r << 1);
        }
        ogg_test_crc_table[i] = r;
    }
}

static uint32_t ogg_test_crc(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ ogg_test_crc_table[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc;
}

static void ogg_test_put_le(uint8_t *p, uint64_t v, size_t len)
{
    for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// Explicit lacing keeps a nil packet (one 0 entry) distinct from an empty
// page (no entries), and from a nonempty 255-byte multiple ending in 0.
static void ogg_test_write_laced_page(FILE *f,uint8_t header_type,uint64_t granule,
                                      uint32_t serial,uint32_t sequence,
                                      const uint8_t *laces,size_t nseg,
                                      const uint8_t *payload,size_t payload_len)
{
    assert(nseg<=255);
    size_t laced_bytes=0;
    for(size_t n=0;n<nseg;n++) laced_bytes+=laces[n];
    assert(laced_bytes==payload_len);
    size_t total=27+nseg+payload_len;
    uint8_t *page=(uint8_t *)malloc(total);assert(page);
    memcpy(page,"OggS",4);page[4]=0;page[5]=header_type;
    ogg_test_put_le(page+6,granule,8);
    ogg_test_put_le(page+14,serial,4);
    ogg_test_put_le(page+18,sequence,4);
    memset(page+22,0,4);page[26]=(uint8_t)nseg;
    if(nseg) memcpy(page+27,laces,nseg);
    if(payload_len) memcpy(page+27+nseg,payload,payload_len);
    ogg_test_put_le(page+22,ogg_test_crc(0,page,total),4);
    size_t written=fwrite(page,1,total,f);
    assert(written==total);(void)written;
    free(page);
}

// Legacy one-packet/empty-page builder retained for the existing fixtures.
static void ogg_test_write_page(FILE *f, uint8_t header_type, uint64_t granule,
                                uint32_t serial, uint32_t sequence,
                                const uint8_t *payload, size_t payload_len)
{
    size_t nseg=payload_len==0 ? 0 : payload_len/255+1;
    assert(nseg<=255);
    uint8_t laces[255];
    for(size_t n=0;n<nseg;n++) laces[n]=(uint8_t)(n+1<nseg ? 255 : payload_len%255);
    ogg_test_write_laced_page(f,header_type,granule,serial,sequence,
                               laces,nseg,payload,payload_len);
}

// Split a metadata-less native FLAC (fLaC + STREAMINFO only) into its
// STREAMINFO payload + frame packets, locating each next frame by its
// frame header (sequential frame numbers), like the FLAC-Chop testutil.
typedef struct {
    uint8_t si[34];
    uint8_t **frames;
    size_t *lens;
    uint16_t *blocks;   /* blocksize of each frame (for granule positions) */
    size_t count;
} native_split_t;

static void native_split_free(native_split_t *sp)
{
    for (size_t i = 0; i < sp->count; i++) free(sp->frames[i]);
    free(sp->frames);
    free(sp->lens);
    free(sp->blocks);
    memset(sp, 0, sizeof(*sp));
}

static bool native_split(const char *path, native_split_t *sp)
{
    memset(sp, 0, sizeof(*sp));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *buf = NULL;
    bool ok = false;
    do {
        if (fseek(f, 0, SEEK_END) != 0) break;
        long size = ftell(f);
        if (size <= 42) break;
        if (fseek(f, 0, SEEK_SET) != 0) break;
        buf = (uint8_t *)malloc((size_t)size);
        if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) break;
        if (memcmp(buf, "fLaC", 4) != 0) break;
        /* Walk the metadata blocks (libFLAC always appends a vendor
         * VORBIS_COMMENT, so STREAMINFO is never the last block here): the
         * first must be a 34-byte STREAMINFO; skip the rest to the audio. */
        if ((buf[4] & 0x7F) != 0 || buf[5] || buf[6] || buf[7] != 34) break;
        memcpy(sp->si, buf + 8, 34);
        size_t pos = 4;
        bool last = false;
        while (!last) {
            if (pos + 4 > (size_t)size) break;
            last = (buf[pos] & 0x80) != 0;
            uint32_t len = ((uint32_t)buf[pos + 1] << 16) |
                           ((uint32_t)buf[pos + 2] << 8) | (uint32_t)buf[pos + 3];
            pos += 4 + len;
        }
        if (!last || pos >= (size_t)size) break;  /* no audio */
        uint64_t expect = 0;
        size_t cap = 0;
        while (pos < (size_t)size) {
            uint16_t block_size = 0;
            size_t header_len = 0;
            uint64_t number = 0;
            bool variable = false;
            if (!gui_playback_parse_frame_header(buf + pos, (size_t)size - pos, &block_size,
                                                 &header_len, &number, &variable) ||
                number != expect) {
                break;
            }
            /* find the next frame start: a byte that parses as a frame with
             * the NEXT sequential number (fixed blocksize in these files) */
            size_t next = pos + header_len;
            bool found = false;
            while (next + 16 <= (size_t)size) {
                uint16_t nb = 0;
                size_t nh = 0;
                uint64_t nn = 0;
                bool nv = false;
                if (buf[next] == 0xFF &&
                    gui_playback_parse_frame_header(buf + next, (size_t)size - next,
                                                    &nb, &nh, &nn, &nv) &&
                    nn == expect + 1) {
                    found = true;
                    break;
                }
                next++;
            }
            size_t end = found ? next : (size_t)size;
            if (sp->count == cap) {
                cap = cap ? cap * 2 : 16;
                sp->frames = (uint8_t **)realloc(sp->frames, cap * sizeof(*sp->frames));
                sp->lens = (size_t *)realloc(sp->lens, cap * sizeof(*sp->lens));
                sp->blocks = (uint16_t *)realloc(sp->blocks, cap * sizeof(*sp->blocks));
                if (!sp->frames || !sp->lens || !sp->blocks) goto done;
            }
            sp->frames[sp->count] = (uint8_t *)malloc(end - pos);
            if (!sp->frames[sp->count]) goto done;
            memcpy(sp->frames[sp->count], buf + pos, end - pos);
            sp->lens[sp->count] = end - pos;
            sp->blocks[sp->count] = block_size;
            sp->count++;
            expect++;
            pos = end;
            if (!found) break;
        }
        ok = sp->count > 0;
    } while (0);
done:
    free(buf);
    fclose(f);
    if (!ok) native_split_free(sp);
    return ok;
}

// Wrap a metadata-less native FLAC into an Ogg FLAC .ldf: BOS mapping page
// (0x7F "FLAC" + STREAMINFO [+ a second header page with an optional
// RF_SAMPLE_RATE vorbis comment]), then the frame packets in laced pages
// with at most `max_segs` lacing values each (small values force packets to
// span pages: continued pages + granule-less pages). `zero_total` clears the
// STREAMINFO total like a piped ffmpeg writer.
static bool ogg_test_wrap(const char *dst, const native_split_t *sp,
                          uint32_t num_samples, uint32_t max_segs,
                          bool zero_total, const char *rf_rate_tag)
{
    FILE *f = fopen(dst, "wb");
    if (!f) return false;
    const uint32_t serial = 0x12345678u;
    uint32_t seq = 0;
    uint8_t si[34];
    memcpy(si, sp->si, sizeof(si));
    if (zero_total) {
        /* the 36-bit total is the low bits of the packed word at SI bytes 10..17 */
        uint64_t packed = 0;
        for (int k = 0; k < 8; k++) packed = (packed << 8) | si[10 + k];
        packed &= ~0xFFFFFFFFFULL;
        for (int k = 0; k < 8; k++) si[10 + k] = (uint8_t)(packed >> (56 - 8 * k));
    }
    /* mapping packet: 0x7F "FLAC" 1 0 nheaders(BE16) "fLaC" + first block */
    uint8_t map[13 + 4 + 34];
    map[0] = 0x7F;
    memcpy(map + 1, "FLAC", 4);
    map[5] = 1;   /* major version */
    map[6] = 0;   /* minor version */
    uint32_t extra_headers = rf_rate_tag ? 1u : 0u;  /* ADDITIONAL header packets */
    map[7] = (uint8_t)(extra_headers >> 8);
    map[8] = (uint8_t)extra_headers;
    memcpy(map + 9, "fLaC", 4);
    map[13] = rf_rate_tag ? 0x00 : 0x80;  /* STREAMINFO block header, last? */
    map[14] = 0;
    map[15] = 0;
    map[16] = 34;
    memcpy(map + 17, si, 34);
    ogg_test_write_page(f, 0x02, 0, serial, seq++, map, sizeof(map));
    if (rf_rate_tag) {
        /* VORBIS_COMMENT header packet: vendor "t" + RF_SAMPLE_RATE=tag */
        size_t entry_len = 15 + strlen(rf_rate_tag);  /* "RF_SAMPLE_RATE=" + value */
        size_t vc_len = 4 + 1 + 4 + 4 + entry_len;
        uint8_t *vc = (uint8_t *)malloc(vc_len);
        assert(vc);
        size_t p = 0;
        ogg_test_put_le(vc + p, 1, 4); p += 4;  /* vendor length 1 */
        vc[p++] = 't';
        ogg_test_put_le(vc + p, 1, 4); p += 4;  /* one comment */
        ogg_test_put_le(vc + p, entry_len, 4); p += 4;
        memcpy(vc + p, "RF_SAMPLE_RATE=", 15); p += 15;
        memcpy(vc + p, rf_rate_tag, strlen(rf_rate_tag));
        uint8_t pkt[4 + 64];
        pkt[0] = 0x84;  /* VORBIS_COMMENT | last */
        pkt[1] = (uint8_t)(vc_len >> 16);
        pkt[2] = (uint8_t)(vc_len >> 8);
        pkt[3] = (uint8_t)vc_len;
        assert(vc_len <= 64);
        memcpy(pkt + 4, vc, vc_len);
        free(vc);
        ogg_test_write_page(f, 0x00, 0, serial, seq++, pkt, 4 + vc_len);
    }
    /* audio frames */
    uint64_t cum = 0;
    uint32_t segs_on_page = 0;
    uint8_t *page_payload = (uint8_t *)malloc(256 * 1024);
    assert(page_payload);
    size_t fill = 0;
    bool continued = false;
    uint64_t page_granule = 0;
    bool page_has_packet = false;
    for (size_t fi = 0; fi < sp->count; fi++) {
        const uint8_t *frame = sp->frames[fi];
        size_t len = sp->lens[fi];
        size_t lacing = len / 255 + 1;
        size_t pos = 0;
        bool last_seg_of_packet = false;
        for (size_t s = 0; s < lacing; s++) {
            if (segs_on_page == max_segs) {
                ogg_test_write_page(f, continued ? 0x01 : 0x00,
                                    page_has_packet ? page_granule : UINT64_MAX,
                                    serial, seq++, page_payload, fill);
                continued = true;  /* the packet continues on the next page */
                segs_on_page = 0;
                fill = 0;
                page_has_packet = false;
            }
            size_t seg = (s == lacing - 1) ? (len - pos) : 255;
            last_seg_of_packet = (s == lacing - 1);
            memcpy(page_payload + fill, frame + pos, seg);
            fill += seg;
            pos += seg;
            segs_on_page++;
            if (last_seg_of_packet) {
                cum += sp->blocks[fi];
                page_granule = (fi + 1 == sp->count) ? num_samples : cum;
                page_has_packet = true;
            }
        }
    }
    /* EOS page: flush the pending lacing (or an empty EOS page). */
    ogg_test_write_page(f, (uint8_t)((continued ? 0x01 : 0x00) | 0x04),
                        page_has_packet ? page_granule : UINT64_MAX,
                        serial, seq++, page_payload, fill);
    free(page_payload);
    fclose(f);
    return true;
}

// ---- IO-layer decoder harness (STREAMINFO patch + seek) ----

static uint64_t io_test_seen_total;
static uint64_t io_test_decoded_samples;

static FLAC__StreamDecoderWriteStatus io_test_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data)
{
    (void)decoder; (void)buffer; (void)client_data;
    io_test_decoded_samples += frame->header.blocksize;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void io_test_meta_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
    (void)decoder; (void)client_data;
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        io_test_seen_total = metadata->data.stream_info.total_samples;
    }
}

static void io_test_error_cb(
    const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    (void)decoder; (void)client_data;
    fprintf(stderr, "harness decoder error: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

/* Open the finalized-convention file through the production IO layer with the
 * patch armed and verify the decoder sees the TRUE total and can seek past
 * the (lying) header total. */
static void test_io_layer_patch_and_seek(const char *path, bool finalized)
{
    test_case = "io_layer_patch_and_seek";

    playback_file_info_t info = {0};
    bool have_info = gui_playback_validate_file(path, &info);
    expect_true(have_info, "validation must read the header and RF tags");
    expect_u64(info.total_samples, TEST_SAMPLES, "resolved true total");
    expect_near(info.real_sample_rate_hz, TEST_RF_RATE_HZ, "resolved RF sample rate");
    expect_near(info.duration_seconds, (double)TEST_SAMPLES / TEST_RF_RATE_HZ, "resolved RF duration");
    expect_u64(info.header_total_samples, finalized ? ((uint64_t)TEST_SAMPLES + 500u) / 1000u : TEST_SAMPLES,
               "header total matches the fixture convention");

    FILE *f = file_open_read_utf8(path);
    expect_true(f != NULL, "reopen file through the production UTF-8 reader");
    if (!f) return;

    playback_io_t io;
    gui_playback_io_reset(&io, f, NULL, info.total_samples);
    expect_true(io.stage == PLAYBACK_IO_STAGE_SERVING, "patch staging armed");

    FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
    expect_true(dec != NULL, "decoder allocated");
    if (!dec) { fclose(f); return; }
    FLAC__stream_decoder_set_md5_checking(dec, false);
    FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_stream(
        dec, gui_playback_io_read_cb, gui_playback_io_seek_cb, gui_playback_io_tell_cb,
        gui_playback_io_length_cb, gui_playback_io_eof_cb,
        io_test_write_cb, io_test_meta_cb, io_test_error_cb, &io);
    expect_true(st == FLAC__STREAM_DECODER_INIT_STATUS_OK, "init_stream via IO layer");
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(dec);
        fclose(f);
        return;
    }

    expect_true(FLAC__stream_decoder_process_until_end_of_metadata(dec),
                "metadata pass through patched IO layer");
    expect_u64(io_test_seen_total, TEST_SAMPLES,
               "decoder-visible STREAMINFO total must be the patched true total");

    /* Without the patch these seeks fail: target >= header total (3000). */
    io_test_decoded_samples = 0;
    expect_true(FLAC__stream_decoder_seek_absolute(dec, TEST_SAMPLES / 2),
                "seek to mid-file (past header total) must succeed");
    expect_true(FLAC__stream_decoder_process_single(dec),
                "decode after mid-file seek");
    expect_true(io_test_decoded_samples > 0, "mid-file seek produces decoded samples");
    io_test_decoded_samples = 0;
    expect_true(FLAC__stream_decoder_seek_absolute(dec, TEST_SAMPLES - 10),
                "seek near end of real stream must succeed");
    expect_true(FLAC__stream_decoder_process_single(dec),
                "decode after end seek");
    expect_true(io_test_decoded_samples > 0, "end seek produces decoded samples");
    io_test_decoded_samples = 0;
    expect_true(FLAC__stream_decoder_seek_absolute(dec, 0),
                "seek back to start must succeed");
    expect_true(FLAC__stream_decoder_process_single(dec),
                "decode after seek to start");
    expect_true(io_test_decoded_samples > 0, "start seek produces decoded samples");

    FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    fclose(f);
}

#ifdef _WIN32
/* Create the fixture with wide Win32 APIs, independently of the reader under
 * test. Both directory and basenames contain characters outside legacy ACPs. */
#define TEST_UNICODE_NAME_W L"\u300C\u30FB \u300D\u4E2D\u6587\xD83D\xDCFC"
static void test_unicode_paths(const char *ascii_old, const char *ascii_new)
{
    wchar_t temporary[1024], directory[1024], source_old[1024], source_new[1024];
    wchar_t path_old[1024], path_new[1024], path_capture[1024], path_log[1024], path_wav[1024];
    char utf8_old[2048], utf8_new[2048], utf8_capture[2048], utf8_log[2048], utf8_wav[2048];
    test_case = "unicode_fixture";
    DWORD temp_len = GetTempPathW(sizeof(temporary) / sizeof(*temporary), temporary);
    bool ready = temp_len > 0 && temp_len < sizeof(temporary) / sizeof(*temporary) &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ascii_old, -1,
                            source_old, sizeof(source_old) / sizeof(*source_old)) > 0 &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ascii_new, -1,
                            source_new, sizeof(source_new) / sizeof(*source_new)) > 0 &&
        swprintf(directory, sizeof(directory) / sizeof(*directory),
                 L"%lsmisrc_pb_%lu_" TEST_UNICODE_NAME_W, temporary, GetCurrentProcessId()) > 0;
    expect_true(ready, "prepare independent wide fixture paths");
    if (!ready) return;
    bool created = CreateDirectoryW(directory, NULL) != 0;
    expect_true(created, "create Unicode directory");
    if (!created) return;
    swprintf(path_old, sizeof(path_old) / sizeof(*path_old),
             L"%ls\\" TEST_UNICODE_NAME_W L"_old_8-bit_20msps.flac", directory);
    swprintf(path_new, sizeof(path_new) / sizeof(*path_new),
             L"%ls\\" TEST_UNICODE_NAME_W L"_new_8-bit_20msps.flac", directory);
    swprintf(path_capture, sizeof(path_capture) / sizeof(*path_capture),
             L"%ls\\" TEST_UNICODE_NAME_W L"_2026.10.16_01.02.03_8-bit_20msps.flac", directory);
    swprintf(path_log, sizeof(path_log) / sizeof(*path_log),
             L"%ls\\" TEST_UNICODE_NAME_W L"_2026.10.16_01.02.03_capture.log", directory);
    swprintf(path_wav, sizeof(path_wav) / sizeof(*path_wav),
             L"%ls\\" TEST_UNICODE_NAME_W L"_2026.10.16_01.02.03_audio.wav", directory);
    ready = CopyFileW(source_old, path_old, TRUE) && CopyFileW(source_new, path_new, TRUE) &&
        CopyFileW(source_old, path_capture, TRUE) &&
        WideCharToMultiByte(CP_UTF8, 0, path_old, -1, utf8_old, sizeof(utf8_old), NULL, NULL) > 0 &&
        WideCharToMultiByte(CP_UTF8, 0, path_new, -1, utf8_new, sizeof(utf8_new), NULL, NULL) > 0 &&
        WideCharToMultiByte(CP_UTF8, 0, path_capture, -1, utf8_capture, sizeof(utf8_capture), NULL, NULL) > 0 &&
        WideCharToMultiByte(CP_UTF8, 0, path_log, -1, utf8_log, sizeof(utf8_log), NULL, NULL) > 0 &&
        WideCharToMultiByte(CP_UTF8, 0, path_wav, -1, utf8_wav, sizeof(utf8_wav), NULL, NULL) > 0;
    expect_true(ready, "copy real Unicode files and convert their paths to UTF-8");
    if (!ready) goto cleanup;

    test_io_layer_patch_and_seek(utf8_old, false);
    test_io_layer_patch_and_seek(utf8_new, true);
    test_case = "unicode_source_identity";
    gui_playback_length_cache_key_t key = {0};
    bool fingerprinted = gui_playback_length_cache_snapshot(utf8_new, &key);
    expect_true(fingerprinted, "Unicode source fingerprint");
    FILE *held = file_open_read_utf8(utf8_new);
    expect_true(held != NULL, "Unicode source read handle");
    if (held) {
        expect_true(fingerprinted && gui_playback_length_cache_matches_open_file(held, &key),
                    "UTF-8 reader and wide fingerprint open the same file");
        fclose(held);
    }

    test_case = "unicode_companion";
    FILE *file = _wfopen(path_log, L"wb");
    expect_true(file != NULL, "create Unicode companion log independently");
    if (!file) goto cleanup;
    const char log[] = "Capture duration: 7.5 seconds\n";
    bool written = fwrite(log, 1, sizeof(log) - 1, file) == sizeof(log) - 1;
    expect_true(fclose(file) == 0 && written, "write companion log");
    file = _wfopen(path_wav, L"wb");
    expect_true(file != NULL, "create Unicode companion WAV independently");
    if (!file) goto cleanup;
    /* 3 seconds of 48 kHz mono 16-bit PCM, only its duration header is needed. */
    const unsigned char wav[44] = {
        'R','I','F','F', 0x24,0x65,0x04,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x80,0xbb,0,0,
        0,0x77,1,0, 2,0, 16,0, 'd','a','t','a', 0,0x65,4,0
    };
    written = fwrite(wav, 1, sizeof(wav), file) == sizeof(wav);
    expect_true(fclose(file) == 0 && written, "write companion WAV duration header");
    double seconds = 0;
    expect_true(gui_playback_read_log_duration_seconds(utf8_log, &seconds), "read Unicode companion log");
    expect_near(seconds, 7.5, "log duration");
    expect_true(gui_playback_read_wav_duration_seconds(utf8_wav, &seconds), "read Unicode companion WAV");
    expect_near(seconds, 3.0, "WAV duration");
    expect_true(gui_playback_try_companion_duration(utf8_capture, &seconds), "find Unicode companions");
    expect_near(seconds, 7.5, "log takes priority when WAV is also present");
    expect_true(DeleteFileW(path_log) != 0, "remove log to exercise WAV fallback");
    expect_true(gui_playback_try_companion_duration(utf8_capture, &seconds), "find Unicode WAV fallback");
    expect_near(seconds, 3.0, "WAV fallback duration");

cleanup:
    DeleteFileW(path_old);
    DeleteFileW(path_new);
    DeleteFileW(path_capture);
    DeleteFileW(path_log);
    DeleteFileW(path_wav);
    expect_true(RemoveDirectoryW(directory) != 0, "remove Unicode fixture directory");
}
#undef TEST_UNICODE_NAME_W
#endif

static void test_remove_path(const char *path, bool directory) {
#ifdef _WIN32
    wchar_t wide[FILE_PATH_MAX];
    expect_true(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
        wide, FILE_PATH_MAX) > 0, "convert cleanup path");
    expect_true(directory ? RemoveDirectoryW(wide) != 0 : DeleteFileW(wide) != 0,
                "remove test path");
#else
    expect_true((directory ? rmdir(path) : unlink(path)) == 0, "remove test path");
#endif
}

static void test_long_path(const char *temporary, const char *source) {
    test_case = "long_multibyte_path";
    char component[247], root[FILE_PATH_MAX], first[FILE_PATH_MAX], second[FILE_PATH_MAX];
    char path[FILE_PATH_MAX];
    for (int i = 0; i < 82; i++) memcpy(component + i * 3, "\xE4\xB8\xAD", 3);
    component[246] = '\0';
    snprintf(root, sizeof(root), "%s/misrc_long_%d", temporary, (int)getpid());
    snprintf(first, sizeof(first), "%s/%s", root, component);
    snprintf(second, sizeof(second), "%s/%s", first, component);
    snprintf(path, sizeof(path), "%s/8-bit_20msps.flac", second);
    expect_true(strlen(path) > 512, "fixture exceeds the old byte cap");
    expect_true(file_mkdir_utf8(root, 0700) == 0, "create test root");
    expect_true(file_mkdir_utf8(first, 0700) == 0, "create first directory");
    expect_true(file_mkdir_utf8(second, 0700) == 0, "create second directory");
    FILE *input = file_open_read_utf8(source), *output = file_open_utf8(path, "wb");
    expect_true(input && output, "copy FLAC into multibyte path");
    if (input && output) {
        unsigned char buffer[4096]; size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), input)) != 0)
            expect_true(fwrite(buffer, 1, n, output) == n, "copy fixture bytes");
    }
    if (input) fclose(input);
    if (output) fclose(output);
    playback_file_info_t info;
    expect_true(gui_playback_validate_file(path, &info), "long FLAC validates");
    expect_true(!strcmp(info.filepath, path), "retain the complete path");
    test_io_layer_patch_and_seek(path, false);
    test_remove_path(path, false); test_remove_path(second, true);
    test_remove_path(first, true); test_remove_path(root, true);

    char oversized[FILE_PATH_MAX + 1]; memset(oversized, 'a', FILE_PATH_MAX);
    oversized[FILE_PATH_MAX] = '\0';
    expect_true(!gui_playback_validate_file(oversized, &info), "reject oversize instead of truncating");
#ifndef _WIN32
    test_case = "posix_literal_backslash_companion";
    snprintf(path, sizeof(path), "%s/literal\\tape_%d_2026.10.17_01.02.03_8-bit.flac", temporary, (int)getpid());
    snprintf(first, sizeof(first), "%s/literal\\tape_%d_2026.10.17_01.02.03_capture.log", temporary, (int)getpid());
    output = fopen(first, "wb");
    expect_true(output != NULL, "create POSIX companion");
    if (output) { fputs("Capture duration: 48.5 seconds\n", output); fclose(output); }
    double seconds = 0;
    expect_true(gui_playback_try_companion_duration(path, &seconds), "find literal backslash companion");
    expect_near(seconds, 48.5, "companion duration");
    test_remove_path(first, false);
#endif
}

// ---- 16-bit source-convention detection (per-file, sticky) ----

// One frame through the production write callback: the conversion must match
// the file's real convention (recorder grid vs direct-domain vs full-range).
static void conv16_frame(playback_ctx_t *ctx, pattern_fn gen, uint32_t n,
                         int want_state, const char *message)
{
    FLAC__Frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.header.blocksize = n;
    frame.header.bits_per_sample = 16;
    static FLAC__int32 in[8192];
    for (uint32_t i = 0; i < n; i++) in[i] = gen(i);
    const FLAC__int32 *in_ptr = in;
    ctx->decode_available_a = 0;
    ctx->decode_pos_a = 0;
    expect_true(decoder_write_cb_a(NULL, &frame, &in_ptr, ctx) ==
                FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE, message);
    expect_u64(ctx->decode_available_a, n, "frame stored");
    expect_i64(ctx->conv16_state[0], want_state, "detected convention");
}

static void test_conv16_conventions(void)
{
    test_case = "conv16_conventions";
    playback_ctx_t ctx = {0};
    ctx.decode_buf_size = 8192;
    ctx.decode_buf_a = (int16_t *)calloc(ctx.decode_buf_size, sizeof(int16_t));
    expect_true(ctx.decode_buf_a != NULL, "decode buffer allocated");
    if (!ctx.decode_buf_a) return;

    /* (a) Recorder convention: all samples on a 16-grid -> bit-exact >> 4
     * restore (identical to the previous blanket behavior for these files). */
    conv16_frame(&ctx, pattern_recorder_x16, 4096, PLAYBACK_CONV16_SHIFT4,
                 "recorder frame accepted");
    for (uint32_t i = 0; i < 4096; i += 997) {
        expect_i64(ctx.decode_buf_a[i], (long long)(pattern_recorder_x16(i) >> 4),
                   "recorder sample >> 4 restored");
    }

    /* (b) Externally RESAMPLED container-domain 16-bit (the Tape_03 class,
     * +-2190 resampler ripple): pass through at container scale with the
     * overshoot clamped to the container rails - the same clamp the
     * recorder's own resample path applies when writing. */
    ctx.conv16_state[0] = PLAYBACK_CONV16_PENDING;  /* fresh file state */
    conv16_frame(&ctx, pattern_direct_domain, 4096, PLAYBACK_CONV16_PASS,
                 "resampled container-domain frame accepted");
    for (uint32_t i = 0; i < 4096; i += 997) {
        int32_t v = pattern_direct_domain(i);
        if (v > 2047) v = 2047;
        if (v < -2048) v = -2048;
        expect_i64(ctx.decode_buf_a[i], (long long)v,
                   "resampled sample raw with rail clamp");
    }

    /* (b2) Container-domain 16-bit WITHIN the rails (peaks +-1450): pass
     * through unchanged - already at the live container scale (in = out). */
    ctx.conv16_state[0] = PLAYBACK_CONV16_PENDING;
    conv16_frame(&ctx, pattern_direct_le2047, 4096, PLAYBACK_CONV16_PASS,
                 "in-rails container-domain frame accepted");
    for (uint32_t i = 0; i < 4096; i += 997) {
        expect_i64(ctx.decode_buf_a[i], (long long)pattern_direct_le2047(i),
                   "in-rails sample passed through unchanged");
    }

    /* (c) Full-range generic 16-bit: >> 4 as an explicit display downscale. */
    ctx.conv16_state[0] = PLAYBACK_CONV16_PENDING;
    conv16_frame(&ctx, pattern_full_range, 4096, PLAYBACK_CONV16_SHIFT4,
                 "full-range frame accepted");
    for (uint32_t i = 0; i < 4096; i += 997) {
        expect_i64(ctx.decode_buf_a[i], (long long)(pattern_full_range(i) >> 4),
                   "full-range sample downscaled");
    }

    /* (d) A silent lead stays PENDING (zeros convert identically under every
     * convention) and the first non-silent frame then decides. */
    ctx.conv16_state[0] = PLAYBACK_CONV16_PENDING;
    conv16_frame(&ctx, pattern_zeros, 4096, PLAYBACK_CONV16_PENDING,
                 "silent lead frame accepted");
    expect_i64(ctx.decode_buf_a[100], 0, "silent lead converted to zeros");
    conv16_frame(&ctx, pattern_direct_domain, 4096, PLAYBACK_CONV16_PASS,
                 "first non-silent frame decides");
    for (uint32_t i = 0; i < 4096; i += 997) {
        int32_t v = pattern_direct_domain(i);
        if (v > 2047) v = 2047;
        if (v < -2048) v = -2048;
        expect_i64(ctx.decode_buf_a[i], (long long)v,
                   "post-lead samples raw with rail clamp");
    }

    /* 8-bit frames: << 4 restores the recorder container AND is the correct
     * full-scale display mapping for generic 8-bit audio (no detection). */
    {
        FLAC__Frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.header.blocksize = 4096;
        frame.header.bits_per_sample = 8;
        static FLAC__int32 in[8192];
        for (uint32_t i = 0; i < frame.header.blocksize; i++) {
            in[i] = (FLAC__int32)((i * 37u) % 256u) - 128;
        }
        const FLAC__int32 *in_ptr = in;
        ctx.decode_available_a = 0;
        ctx.decode_pos_a = 0;
        expect_true(decoder_write_cb_a(NULL, &frame, &in_ptr, &ctx) ==
                    FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE, "8-bit frame accepted");
        for (uint32_t i = 0; i < frame.header.blocksize; i += 997) {
            expect_i64(ctx.decode_buf_a[i], (long long)(in[i] << 4), "8-bit sample << 4 restored");
        }
    }

    free(ctx.decode_buf_a);
}

// ---- Raw / WAV / .lds probe + reader (rate resolution, totals, seek) ----

static gui_app_t s_test_app;

// Prepare one raw channel through the production path, then exercise the
// playback thread's own refill shape: a bounded first read (crosses .lds
// group boundaries -> exercises the group-mate carry), a full decode-buffer
// fill with sample-exact comparison, an exact-position seek (mid-group for
// .lds) and EOF at the exact size-derived total.
static void raw_harness_channel(const char *path, playback_input_format_t fmt,
                                uint64_t total, uint64_t bounded_first,
                                uint64_t seek_point, const int16_t *expected)
{
    playback_ctx_t *ctx = (playback_ctx_t *)calloc(1, sizeof(*ctx));
    expect_true(ctx != NULL, "context allocated");
    if (!ctx) return;
    expect_true(gui_playback_prepare(ctx, &s_test_app, path, NULL, NULL) == 0,
               "raw channel prepares");
    expect_true(ctx->input_is_raw[0], "raw channel flagged");
    expect_i64(ctx->input_format[0], (long long)fmt, "sniffed input format");
    expect_true(ctx->decoder_a == NULL, "no libFLAC decoder for raw channels");
    expect_u64(ctx->info_a.total_samples, total, "exact total from the file size");
    expect_u64(ctx->raw[0].total_samples, total, "reader total");

    if (bounded_first > 0) {
        int16_t tmp[512];
        size_t n = gui_playback_raw_read(ctx, 0, tmp, (size_t)bounded_first);
        expect_u64(n, bounded_first, "bounded first read length");
        for (uint64_t i = 0; i < bounded_first && i < 512; i++) {
            if (tmp[i] != expected[i]) {
                expect_i64(tmp[i], expected[i], "bounded read sample mismatch");
                break;
            }
        }
    }

    while (ctx->decode_available_a - ctx->decode_pos_a < RAW_BUFFER_SAMPLES) {
        size_t space = ctx->decode_buf_size - ctx->decode_available_a;
        size_t got = gui_playback_raw_read(ctx, 0,
                                           ctx->decode_buf_a + ctx->decode_available_a, space);
        ctx->decode_available_a += got;
        if (got == 0) break;
    }
    expect_u64(ctx->decode_available_a, total - bounded_first, "full decode buffer fill");
    {
        bool all_match = true;
        for (uint64_t i = bounded_first; i < total; i++) {
            if (ctx->decode_buf_a[i - bounded_first] != expected[i]) {
                fprintf(stderr, "FAIL [%s]: sample %llu got %d want %d\n",
                        test_case, (unsigned long long)i,
                        (int)ctx->decode_buf_a[i - bounded_first], (int)expected[i]);
                all_match = false;
                break;
            }
        }
        checks++;
        if (!all_match) failures++;
    }
    /* The bounded read plus the fill consumed the whole span. */
    expect_true(gui_playback_raw_eof(ctx, 0), "EOF at the exact total");

    /* Exact-position seek: the next read starts at exactly the sample. */
    ctx->decode_available_a = 0;
    ctx->decode_pos_a = 0;
    expect_true(gui_playback_raw_seek(ctx, 0, seek_point), "seek accepted");
    expect_u64(ctx->raw[0].cursor_sample, seek_point, "cursor at the seek point");
    while (ctx->decode_available_a < total - seek_point) {
        size_t space = ctx->decode_buf_size - ctx->decode_available_a;
        size_t got = gui_playback_raw_read(ctx, 0,
                                           ctx->decode_buf_a + ctx->decode_available_a, space);
        ctx->decode_available_a += got;
        if (got == 0) break;
    }
    expect_u64(ctx->decode_available_a, total - seek_point, "post-seek fill");
    {
        bool all_match = true;
        for (uint64_t i = seek_point; i < total; i++) {
            if (ctx->decode_buf_a[i - seek_point] != expected[i]) {
                fprintf(stderr, "FAIL [%s]: post-seek sample %llu got %d want %d\n",
                        test_case, (unsigned long long)i,
                        (int)ctx->decode_buf_a[i - seek_point], (int)expected[i]);
                all_match = false;
                break;
            }
        }
        checks++;
        if (!all_match) failures++;
    }
    expect_true(gui_playback_raw_eof(ctx, 0), "EOF after post-seek read");

    gui_playback_release_context(ctx);
    free(ctx);
}

static void test_raw_read_seek(const char *temp_dir)
{
    static char path[512];
    static int16_t expected[4096];
    static uint8_t bytes[8192];
    const uint32_t pid = (uint32_t)getpid();
    const uint64_t N = 1000;

    /* .u8: cxadc unsigned, centre 128 -> << 4 into the 12-bit container. */
    test_case = "raw_read_u8";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_8-bit_20msps.u8", temp_dir, pid);
    for (uint64_t i = 0; i < N; i++) {
        bytes[i] = (uint8_t)((i * 37u + 11u) % 256u);
        expected[i] = (int16_t)(((int32_t)bytes[i] - 128) << 4);
    }
    FILE *f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, N, f) == N, "u8 fixture written");
    if (f) fclose(f);
    raw_harness_channel(path, PLAYBACK_INPUT_U8, N, 0, 437, expected);

    /* .s16 recorder-style grid: all mod-16 -> >> 4 restore. */
    test_case = "raw_read_s16_recorder";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_s16_20msps.s16", temp_dir, pid);
    for (uint64_t i = 0; i < N; i++) {
        int32_t v = (int32_t)(((i % 200u) - 100) * 16);  /* on the 16-grid */
        bytes[2 * i] = (uint8_t)((uint16_t)(int16_t)v & 0xFF);
        bytes[2 * i + 1] = (uint8_t)(((uint16_t)(int16_t)v >> 8) & 0xFF);
        expected[i] = (int16_t)(v >> 4);
    }
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 2 * N, f) == 2 * N, "s16 recorder fixture written");
    if (f) fclose(f);
    raw_harness_channel(path, PLAYBACK_INPUT_S16, N, 0, 437, expected);

    /* .16s (the reversed extension) with resampled container-domain data:
     * raw with the container-rail clamp. */
    test_case = "raw_read_16s_direct";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_direct_20msps.16s", temp_dir, pid);
    for (uint64_t i = 0; i < N; i++) {
        int32_t v = pattern_direct_domain((uint32_t)i);  /* peak 2190, non-grid */
        if (v > 2047) v = 2047;
        if (v < -2048) v = -2048;
        bytes[2 * i] = (uint8_t)((uint16_t)(int16_t)v & 0xFF);
        bytes[2 * i + 1] = (uint8_t)(((uint16_t)(int16_t)v >> 8) & 0xFF);
        expected[i] = (int16_t)v;
    }
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 2 * N, f) == 2 * N, "16s direct fixture written");
    if (f) fclose(f);
    raw_harness_channel(path, PLAYBACK_INPUT_S16, N, 0, 437, expected);

    /* .u16: cxadc tenbit, unsigned 0..1023, centre 512 -> << 2. */
    test_case = "raw_read_u16_tenbit";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_10msps.u16", temp_dir, pid);
    for (uint64_t i = 0; i < N; i++) {
        uint32_t w = (i * 331u + 17u) % 1024u;
        bytes[2 * i] = (uint8_t)(w & 0xFF);
        bytes[2 * i + 1] = (uint8_t)(w >> 8);
        expected[i] = (int16_t)(((int32_t)w - 512) << 2);
    }
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 2 * N, f) == 2 * N, "u16 fixture written");
    if (f) fclose(f);
    raw_harness_channel(path, PLAYBACK_INPUT_U16, N, 0, 437, expected);

    /* .lds: 4 ten-bit words per 5 bytes; a bounded first read of 5 crosses
     * a group boundary (exercising the mate carry) and seek 5 lands mid-group
     * (exercising the leading skip). 13 groups = 52 samples + 1 trailing
     * byte that must be dropped (total 52). */
    test_case = "raw_read_lds";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_capture.lds", temp_dir, pid);
    {
        const uint64_t lds_n = 52;
        uint8_t group[5];
        uint32_t w[4];
        f = fopen(path, "wb");
        expect_true(f != NULL, "lds fixture open");
        for (uint64_t i = 0; i < lds_n; i++) {
            w[i % 4] = (uint32_t)((i * 173u + 29u) % 1024u);
            expected[i] = (int16_t)(((int32_t)w[i % 4] - 512) << 2);
            if (i % 4 == 3) {
                lds_test_pack(group, w);
                fwrite(group, 1, 5, f);
            }
        }
        fputc(0xAB, f);  /* the trailing partial group byte: dropped */
        fclose(f);
        raw_harness_channel(path, PLAYBACK_INPUT_LDS, lds_n, 5, 5, expected);
    }

    /* WAV 16-bit resampled container-domain: raw with the rail clamp;
     * WAV 24-bit: top 12 bits. */
    test_case = "raw_read_wav16";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav16_20msps.wav", temp_dir, pid);
    for (uint64_t i = 0; i < N; i++) {
        int32_t v = pattern_direct_domain((uint32_t)i);
        if (v > 2047) v = 2047;
        if (v < -2048) v = -2048;
        bytes[2 * i] = (uint8_t)((uint16_t)(int16_t)v & 0xFF);
        bytes[2 * i + 1] = (uint8_t)(((uint16_t)(int16_t)v >> 8) & 0xFF);
        expected[i] = (int16_t)v;
    }
    wav_test_write(path, 20000, 16, 1, (uint32_t)(2 * N), bytes, 2 * N);
    raw_harness_channel(path, PLAYBACK_INPUT_WAV, N, 0, 437, expected);

    test_case = "raw_read_wav24";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav24.wav", temp_dir, pid);
    for (uint64_t i = 0; i < N; i++) {
        int32_t v = (int32_t)((i * 140017u) % 16777216u) - 8388608;  /* 24-bit signed */
        bytes[3 * i] = (uint8_t)((uint32_t)v & 0xFF);
        bytes[3 * i + 1] = (uint8_t)(((uint32_t)v >> 8) & 0xFF);
        bytes[3 * i + 2] = (uint8_t)(((uint32_t)v >> 16) & 0xFF);
        expected[i] = (int16_t)(v >> 12);
    }
    wav_test_write(path, 48000, 24, 1, (uint32_t)(3 * N), bytes, 3 * N);
    raw_harness_channel(path, PLAYBACK_INPUT_WAV, N, 0, 437, expected);

    test_case = "raw_read_wav8";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav8.wav", temp_dir, pid);
    for (uint64_t i = 0; i < N; i++) {
        bytes[i] = (uint8_t)((i * 97u + 5u) % 256u);
        expected[i] = (int16_t)(((int32_t)bytes[i] - 128) << 4);
    }
    wav_test_write(path, 48000, 8, 1, N, bytes, N);
    raw_harness_channel(path, PLAYBACK_INPUT_WAV, N, 0, 437, expected);
}

static void test_raw_probe(const char *temp_dir)
{
    static char path[512];
    static uint8_t bytes[2048];
    const uint32_t pid = (uint32_t)getpid();
    playback_file_info_t info;

    /* Rate resolution: the REQUIRED <n>msps hint is the only rate source. */
    test_case = "raw_probe_u8";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_probe_8-bit_20msps.u8", temp_dir, pid);
    FILE *f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 1000, f) == 1000, "u8 probe fixture");
    if (f) fclose(f);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "u8 with hint validates");
    expect_i64(info.format, PLAYBACK_INPUT_U8, "u8 format sniffed");
    expect_u64(info.total_samples, 1000, "u8 total from the file size");
    expect_u64(info.header_total_samples, 1000, "u8 header total is the exact count");
    expect_u64((uint64_t)info.real_sample_rate_hz, 20000000, "20msps hint rate");
    expect_u64(info.sample_rate, 20000, "kHz-domain equivalent header rate");
    expect_u64(info.bits_per_sample, 8, "u8 width");
    expect_near(info.duration_seconds, 1000.0 / 20000000.0, "u8 duration");

    /* The reversed <bits><sign> naming and the /1000 rule resolution. */
    test_case = "raw_probe_16u";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_probe_8u_20msps.8u", temp_dir, pid);
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 500, f) == 500, "8u probe fixture");
    if (f) fclose(f);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "reversed .8u validates");
    expect_i64(info.format, PLAYBACK_INPUT_U8, ".8u = unsigned 8-bit");
    expect_u64(info.total_samples, 500, ".8u total");

    /* Missing hint: a hard error naming the fix, never a guessed rate. */
    test_case = "raw_probe_missing_hint";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_probe_nohint.s16", temp_dir, pid);
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 1000, f) == 1000, "no-hint fixture");
    if (f) fclose(f);
    memset(&info, 0, sizeof(info));
    expect_true(!gui_playback_validate_file(path, &info), "missing hint rejected");
    expect_str_contains(info.error, "msps", "error names the <n>msps requirement");
    expect_str_contains(info.error, "s16", "error names the raw format");

    /* .raw/.bin/.pcm default to u8 (the most common cxadc mode) + warn. */
    test_case = "raw_probe_raw_default";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_probe_8-bit_20msps.raw", temp_dir, pid);
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 750, f) == 750, ".raw probe fixture");
    if (f) fclose(f);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), ".raw with hint validates");
    expect_i64(info.format, PLAYBACK_INPUT_U8, ".raw defaults to u8");
    expect_u64(info.total_samples, 750, ".raw total");

    /* .lds: exact totals from the packing, the 40 MSPS format rate (used
     * silently) and the hint override for renamed files. */
    test_case = "raw_probe_lds";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_probe_lds.lds", temp_dir, pid);
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 25, f) == 25, ".lds probe fixture (5 groups)");
    if (f) fclose(f);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), ".lds validates (no hint needed)");
    expect_i64(info.format, PLAYBACK_INPUT_LDS, ".lds format");
    expect_u64(info.total_samples, 20, ".lds total (25 bytes = 5 groups x 4)");
    expect_u64(info.bits_per_sample, 10, ".lds 10-bit source depth");
    expect_u64((uint64_t)info.real_sample_rate_hz, 40000000, ".lds 40 MSPS format rate");
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_probe_20msps.lds", temp_dir, pid);
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 25, f) == 25, ".lds hint fixture");
    if (f) fclose(f);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), ".lds with hint validates");
    expect_u64((uint64_t)info.real_sample_rate_hz, 20000000, ".lds hint overrides 40 MSPS");

    /* Hint precedence over a wrong-looking name: 10msps wins. */
    test_case = "raw_probe_hint_precedence";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_probe_10msps.u8", temp_dir, pid);
    f = fopen(path, "wb");
    expect_true(f && fwrite(bytes, 1, 100, f) == 100, "hint precedence fixture");
    if (f) fclose(f);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "10msps u8 validates");
    expect_u64((uint64_t)info.real_sample_rate_hz, 10000000, "10msps hint rate");
}

static void test_wav_probe(const char *temp_dir)
{
    static char path[512];
    static uint8_t bytes[2048];
    const uint32_t pid = (uint32_t)getpid();
    playback_file_info_t info;
    memset(bytes, 0x5A, sizeof(bytes));

    /* Standard audio rate: used as-is. */
    test_case = "wav_probe_audio";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav_48k.wav", temp_dir, pid);
    wav_test_write(path, 48000, 16, 1, 2000, bytes, 2000);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "48 kHz WAV validates");
    expect_i64(info.format, PLAYBACK_INPUT_WAV, "WAV sniffed by RIFF magic");
    expect_u64(info.total_samples, 1000, "WAV total from the data chunk");
    expect_u64((uint64_t)info.real_sample_rate_hz, 48000, "audio rate as-is");
    expect_u64(info.bits_per_sample, 16, "WAV width");

    /* kHz-domain RF header: the /1000 rule. */
    test_case = "wav_probe_rf_khz";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav_khz.wav", temp_dir, pid);
    wav_test_write(path, 20000, 16, 1, 2000, bytes, 2000);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "20 kHz RF WAV validates");
    expect_u64((uint64_t)info.real_sample_rate_hz, 20000000, "RF /1000 rule");

    /* Real MHz RF header: used as-is. */
    test_case = "wav_probe_rf_real";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav_real.wav", temp_dir, pid);
    wav_test_write(path, 40000000, 16, 1, 2000, bytes, 2000);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "40 MHz RF WAV validates");
    expect_u64((uint64_t)info.real_sample_rate_hz, 40000000, "real RF rate as-is");

    /* Streaming-writer data size 0xFFFFFFFF: clamped to EOF. */
    test_case = "wav_probe_size_clamp";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav_clamp.wav", temp_dir, pid);
    wav_test_write(path, 48000, 8, 1, 0xFFFFFFFFu, bytes, 500);
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "0xFFFFFFFF data size validates");
    expect_u64(info.total_samples, 500, "data span clamped to EOF");

    /* Stereo: one playback file per RF channel - a clear rejection. */
    test_case = "wav_probe_stereo";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_wav_stereo.wav", temp_dir, pid);
    wav_test_write(path, 48000, 16, 2, 2000, bytes, 2000);
    memset(&info, 0, sizeof(info));
    expect_true(!gui_playback_validate_file(path, &info), "stereo WAV rejected");
    expect_str_contains(info.error, "channels", "error names the channel count");
}

static void test_sniff_errors(const char *temp_dir)
{
    static char path[512];
    static uint8_t blob[256];
    const uint32_t pid = (uint32_t)getpid();
    playback_file_info_t info;

    /* Non-FLAC Ogg (Vorbis): the codec is named, never a decode failure. */
    test_case = "sniff_ogg_vorbis";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_vorbis.ogg", temp_dir, pid);
    {
        FILE *f = fopen(path, "wb");
        expect_true(f != NULL, "vorbis fixture open");
        memcpy(blob, "OggS", 4);
        blob[4] = 0;
        blob[5] = 0x02;              /* BOS */
        memset(blob + 6, 0, 16);     /* granule + serial + sequence */
        memset(blob + 22, 0, 4);    /* CRC (not checked by the sniffer) */
        blob[26] = 1;                /* one lacing value */
        blob[27] = 30;
        memcpy(blob + 28, "\x01vorbis\x00\x00\x00\x00\x00\x00\x00", 15);
        if (f) {
            fwrite(blob, 1, 43, f);
            fclose(f);
        }
    }
    memset(&info, 0, sizeof(info));
    expect_true(!gui_playback_validate_file(path, &info), "Ogg Vorbis rejected");
    expect_str_contains(info.error, "Vorbis", "error names the codec");

    /* ID3-prefixed FLAC: a specific message with the strip command. */
    test_case = "sniff_id3";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_id3.flac", temp_dir, pid);
    {
        FILE *f = fopen(path, "wb");
        expect_true(f != NULL, "id3 fixture open");
        if (f) {
            fwrite("ID3\x04\x00\x00\x00\x00\x00\x0A", 1, 10, f);
            fwrite("garbagegarbage", 1, 14, f);
            fclose(f);
        }
    }
    memset(&info, 0, sizeof(info));
    expect_true(!gui_playback_validate_file(path, &info), "ID3-prefixed FLAC rejected");
    expect_str_contains(info.error, "ID3v2", "error names ID3v2");

    /* Unknown extension without any known magic. */
    test_case = "sniff_unknown";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_mystery.xyz", temp_dir, pid);
    {
        FILE *f = fopen(path, "wb");
        expect_true(f != NULL, "unknown fixture open");
        if (f) {
            fwrite("NOTHING RECOGNISABLE HERE", 1, 25, f);
            fclose(f);
        }
    }
    memset(&info, 0, sizeof(info));
    expect_true(!gui_playback_validate_file(path, &info), "unknown type rejected");
    expect_str_contains(info.error, "unsupported", "error says unsupported");

    /* Magic-first: renamed containers keep their content identity. */
    test_case = "sniff_renamed_flac";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_renamed.bin", temp_dir, pid);
    expect_true(write_pattern_flac(path, 4096, TEST_SAMPLE_RATE_KHZ, 4096,
                                    pattern_recorder_x16), "renamed .bin FLAC fixture");
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path, &info), "renamed .bin with fLaC magic validates");
    expect_i64(info.format, PLAYBACK_INPUT_FLAC, ".bin with fLaC magic is native FLAC");

    test_case = "sniff_renamed_wav";
    snprintf(path, sizeof(path), "%s/misrc_pb_%u_renamed.raw", temp_dir, pid);
    wav_test_write(path, 48000, 16, 1, 2000, (const uint8_t *)"\x00\x01", 2);
    memset(&info, 0, sizeof(info));
    expect_true(!gui_playback_validate_file(path, &info) || info.format == PLAYBACK_INPUT_WAV,
                ".raw with RIFF magic routes to the WAV prober");
    expect_i64(info.format, PLAYBACK_INPUT_WAV, ".raw with RIFF magic is WAV");
}

// ---- Ogg FLAC (.ldf/.oga): probe, last granule, remux-to-temp prepare ----


typedef enum {
    OGG_NIL_SEPARATE_EOS,
    OGG_NIL_MIXED_EOS,
    OGG_NIL_NO_EXTRA_PACKET,
    OGG_NIL_ONLY,
    OGG_NIL_DAMAGED_FRAME
} ogg_nil_layout_t;

static bool ogg_test_wrap_nil(const char *path,const native_split_t *sp,
                               ogg_nil_layout_t layout)
{
    FILE *f=fopen(path,"wb");if(!f) return false;
    const uint32_t serial=0x24681357u;uint32_t sequence=0;
    uint8_t map[51]={0x7f,'F','L','A','C',1,0,0,0,'f','L','a','C',0x80,0,0,34};
    memcpy(map+17,sp->si,34);
    /* STREAMINFO total0,like a piped writer; remux must count real frames. */
    map[30]&=0xf0;memset(map+31,0,4);
    ogg_test_write_page(f,2,0,serial,sequence++,map,sizeof(map));
    uint64_t total=0;
    if(layout!=OGG_NIL_ONLY) {
        for(size_t n=0;n<sp->count;n++) {
            const uint8_t *frame=sp->frames[n];size_t bytes=sp->lens[n];
            uint8_t *damaged=NULL;
            if(layout==OGG_NIL_DAMAGED_FRAME && n==0) {
                damaged=(uint8_t *)malloc(bytes);assert(damaged && bytes);
                memcpy(damaged,frame,bytes);damaged[0]^=1;frame=damaged;
            }
            bool last=n+1==sp->count;
            bool eos=last && (layout==OGG_NIL_MIXED_EOS || layout==OGG_NIL_NO_EXTRA_PACKET);
            bool extra_nil=last && layout==OGG_NIL_MIXED_EOS;
            size_t frame_laces=bytes/255+1,nseg=frame_laces+(extra_nil ? 1 : 0);
            assert(nseg<=255);uint8_t laces[255];
            for(size_t k=0;k<frame_laces;k++)
                laces[k]=(uint8_t)(k+1<frame_laces ? 255 : bytes%255);
            if(extra_nil) laces[frame_laces]=0;
            total+=sp->blocks[n];
            ogg_test_write_laced_page(f,eos ? 4 : 0,total,serial,sequence++,
                                       laces,nseg,frame,bytes);
            free(damaged);
        }
    }
    if(layout!=OGG_NIL_MIXED_EOS && layout!=OGG_NIL_NO_EXTRA_PACKET) {
        const uint8_t nil_lace=0;
        ogg_test_write_laced_page(f,4,total,serial,sequence++, &nil_lace,1,NULL,0);
    }
    return fclose(f)==0;
}

static void ogg_test_expect_nil_decode(const char *path,const decode_ref_t *ref,
                                        unsigned bits)
{
    playback_ctx_t *ctx=(playback_ctx_t *)calloc(1,sizeof(*ctx));assert(ctx);
    int result=gui_playback_prepare(ctx,&s_test_app,path,NULL,NULL);
    expect_i64(result,0,"nil-compatible Ogg prepares through production remux");
    if(result==0) {
        expect_u64(ctx->info_a.total_samples,ref->count,"nil packets add no samples");
        expect_u64(ctx->info_a.bits_per_sample,bits,"remux preserves source depth");
        expect_true(ctx->decode_path_temp[0],"nil remux temp registered");
        expect_true(ctx->decoder_a!=NULL,"nil remux opens a native decoder");
        FLAC__StreamMetadata md;
        bool metadata_ok=FLAC__metadata_get_streaminfo(ctx->decode_path[0],&md);
        expect_true(metadata_ok,"nil remux has native STREAMINFO");
        if(metadata_ok) expect_u64(md.data.stream_info.total_samples,ref->count,
                                     "nil-free frame sum patches STREAMINFO");
        while(ctx->decoder_a && ctx->decode_available_a<ref->count &&
              FLAC__stream_decoder_get_state(ctx->decoder_a)!=FLAC__STREAM_DECODER_END_OF_STREAM) {
            if(!FLAC__stream_decoder_process_single(ctx->decoder_a)) break;
        }
        expect_u64(ctx->decode_available_a,ref->count,"nil remux decodes every real sample");
        bool match=ctx->decode_available_a==ref->count;
        for(uint64_t n=0;match && n<ref->count;n++) {
            int16_t want=(int16_t)(bits==8 ? ref->samples[n]*16 : ref->samples[n]>>4);
            if(ctx->decode_buf_a[n]!=want) match=false;
        }
        expect_true(match,"nil remux PCM matches independent source values");
    }
    char temp_path[512];snprintf(temp_path,sizeof(temp_path),"%s",ctx->decode_path[0]);
    gui_playback_release_context(ctx);free(ctx);
    if(temp_path[0]) {
        FILE *gone=fopen(temp_path,"rb");
        expect_true(gone==NULL,"nil remux temp removed on release");
        if(gone) fclose(gone);
    }
}

static uint16_t ogg_test_frame_crc(const uint8_t *bytes,size_t count,unsigned bits,
                                   uint16_t polynomial)
{
    uint16_t crc=0,mask=bits==8 ? 0xff : 0xffff;
    for(size_t n=0;n<count;n++) {
        crc^=(uint16_t)((uint16_t)bytes[n]<<(bits-8));
        for(int bit=0;bit<8;bit++)
            crc=(uint16_t)(((crc<<1)^((crc&(1u<<(bits-1))) ? polynomial : 0))&mask);
    }
    return crc;
}

static void test_ogg_nil_packets(const char *temp_dir,const native_split_t *sp,
                                  const decode_ref_t *ref)
{
    char path[512],remux[512];uint32_t pid=(uint32_t)getpid();
    snprintf(path,sizeof(path),"%s/misrc_pb_%u_ogg_nil.ldf",temp_dir,pid);
    snprintf(remux,sizeof(remux),"%s/misrc_pb_%u_ogg_nil_remux.flac",temp_dir,pid);
    test_case="ogg_nil_separate_eos";
    expect_true(ogg_test_wrap_nil(path,sp,OGG_NIL_SEPARATE_EOS),"EOS page nseg1,lace0 written");
    ogg_test_expect_nil_decode(path,ref,16);
    test_case="ogg_nil_mixed_eos";
    expect_true(ogg_test_wrap_nil(path,sp,OGG_NIL_MIXED_EOS),"EOS real frame then extra lace0 written");
    ogg_test_expect_nil_decode(path,ref,16);

    /* Independently specified legal mono 8-bit FLAC: seven-byte header,one-byte
     * verbatim subframe header,245 PCM bytes,two-byte CRC16 =255 bytes. Its
     * lacing[255,0] completes REAL audio;[255,0,0] adds a separate nil packet.
     * libFLAC plus the explicit sample oracle checks the full frame grammar. */
    uint8_t frame[255]={0xff,0xf8,0x60,0x02,0x00,0xf4,0x00,0x02};
    FLAC__int32 samples[245];
    for(unsigned n=0;n<245;n++) {
        samples[n]=(int32_t)((n*13u)%251u)-125;
        frame[8+n]=(uint8_t)(int8_t)samples[n];
    }
    frame[6]=(uint8_t)ogg_test_frame_crc(frame,6,8,0x07);
    uint16_t crc=ogg_test_frame_crc(frame,253,16,0x8005);
    frame[253]=(uint8_t)(crc>>8);frame[254]=(uint8_t)crc;
    native_split_t exact={0};
    exact.si[1]=exact.si[3]=245; /* min/max blocksize */
    exact.si[6]=exact.si[9]=255; /* min/max frame size */
    uint64_t packed=((uint64_t)TEST_SAMPLE_RATE_KHZ<<44)|(UINT64_C(7)<<36);
    for(int n=0;n<8;n++) exact.si[10+n]=(uint8_t)(packed>>(56-8*n));
    uint8_t *packet=frame;size_t bytes=sizeof(frame);uint16_t block=245;
    exact.frames=&packet;exact.lens=&bytes;exact.blocks=&block;exact.count=1;
    decode_ref_t exact_ref={.samples=samples,.count=245,.cap=245};
    test_case="ogg_nonempty_255_zero_terminator";
    expect_true(ogg_test_wrap_nil(path,&exact,OGG_NIL_NO_EXTRA_PACKET),"255-byte real frame with zero completion written");
    ogg_test_expect_nil_decode(path,&exact_ref,8);
    test_case="ogg_255_frame_then_nil";
    expect_true(ogg_test_wrap_nil(path,&exact,OGG_NIL_MIXED_EOS),"255-byte frame completion then independent nil written");
    ogg_test_expect_nil_decode(path,&exact_ref,8);

    const ogg_nil_layout_t bad[]={OGG_NIL_ONLY,OGG_NIL_DAMAGED_FRAME};
    const char *reasons[]={"holds no audio frames","not a valid FLAC frame"};
    for(size_t n=0;n<sizeof(bad)/sizeof(bad[0]);n++) {
        test_case=n==0 ? "ogg_nil_only_reject":"ogg_nonempty_damaged_header_reject";
        expect_true(ogg_test_wrap_nil(path,sp,bad[n]),"negative nil fixture written with valid Ogg CRC");
        playback_ogg_header_t header;char error[256];uint64_t size=0,total=99;
        bool header_ok=gui_playback_ogg_read_header(path,&header,error,sizeof(error));
        expect_true(header_ok,"negative fixture retains valid Ogg FLAC metadata");
        if(header_ok) {
            expect_true(gui_playback_stat_file_size(path,&size),"negative fixture file size");
            expect_true(!gui_playback_ogg_remux(path,size,&header,remux,NULL,&total,error,sizeof(error)),
                          "nil support does not accept missing/damaged real audio");
            expect_u64(total,0,"rejected remux publishes no sample total");
            expect_str_contains(error,reasons[n],"negative remux retains specific frame error");
            gui_playback_ogg_header_cleanup(&header);
        }
        unlink(remux);
    }
    unlink(path);
}

static void test_ogg_flac(const char *temp_dir)
{
    static char native_path[512];
    static char ldf_path[512];
    static char path2[512];
    const uint32_t pid = (uint32_t)getpid();
    const uint32_t N = 8192;    /* 2 frames of 4096 */
    const uint32_t BLOCK = 4096;
    ogg_test_crc_init();

    test_case = "ogg_fixture";
    snprintf(native_path, sizeof(native_path), "%s/misrc_pb_%u_ogg_native.flac", temp_dir, pid);
    expect_true(write_pattern_flac(native_path, N, TEST_SAMPLE_RATE_KHZ, BLOCK,
                                    pattern_recorder_x16), "native 16-bit fixture written");
    decode_ref_t ref;
    expect_true(decode_flac_i32(native_path, &ref, N), "reference decode");
    expect_u64(ref.count, N, "reference sample count");

    native_split_t sp;
    expect_true(native_split(native_path, &sp), "native fixture splits into frames");
    expect_u64(sp.count, 2, "two 4096-sample frames");

    /* Wrap: small lacing budgets force packets to span pages (continued
     * pages, granule-less pages); zero_total mimics the piped ffmpeg writer
     * that never finalises STREAMINFO - the real-world .ldf case. */
    test_case = "ogg_probe";
    snprintf(ldf_path, sizeof(ldf_path), "%s/misrc_pb_%u_ogg.ldf", temp_dir, pid);
    expect_true(ogg_test_wrap(ldf_path, &sp, N, 3, true, NULL), "Ogg FLAC .ldf wrapped");
    playback_file_info_t info;
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(ldf_path, &info), ".ldf validates");
    expect_i64(info.format, PLAYBACK_INPUT_OGG_FLAC, "Ogg FLAC sniffed by OggS + mapping");
    expect_u64(info.sample_rate, TEST_SAMPLE_RATE_KHZ, "STREAMINFO header rate");
    expect_u64(info.bits_per_sample, 16, "Ogg FLAC width");
    expect_u64(info.header_total_samples, 0, "zero-total STREAMINFO (piped writer)");
    expect_u64(info.total_samples, N, "exact total from the last-page granule");
    expect_u64((uint64_t)info.real_sample_rate_hz, (uint64_t)TEST_RF_RATE_HZ,
               "RF /1000 rate resolution");
    expect_near(info.duration_seconds, (double)N / TEST_RF_RATE_HZ, "Ogg FLAC duration");

    /* A renamed container keeps its magic identity (.oga). */
    test_case = "ogg_probe_oga";
    snprintf(path2, sizeof(path2), "%s/misrc_pb_%u_ogg.oga", temp_dir, pid);
    expect_true(ogg_test_wrap(path2, &sp, N, 3, true, NULL), ".oga wrapped");
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path2, &info), ".oga validates");
    expect_i64(info.format, PLAYBACK_INPUT_OGG_FLAC, ".oga is Ogg FLAC by magic");

    /* RF_SAMPLE_RATE vorbis tag: the in-file record wins for the real rate. */
    test_case = "ogg_probe_tag";
    snprintf(path2, sizeof(path2), "%s/misrc_pb_%u_ogg_tag.ldf", temp_dir, pid);
    expect_true(ogg_test_wrap(path2, &sp, N, 3, true, "40000000"), "tagged .ldf wrapped");
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path2, &info), "tagged .ldf validates");
    expect_u64((uint64_t)info.real_sample_rate_hz, 40000000, "RF_SAMPLE_RATE tag rate");

    /* Truncated tail: the granule of the last COMPLETE page (a frame
     * boundary), not the head-end claim. */
    test_case = "ogg_probe_truncated";
    snprintf(path2, sizeof(path2), "%s/misrc_pb_%u_ogg_trunc.ldf", temp_dir, pid);
    expect_true(ogg_test_wrap(path2, &sp, N, 3, true, NULL), "trunc .ldf wrapped");
    {
        /* Rewrite the file minus its last 100 bytes (mid-last-page cut). */
        FILE *f = fopen(path2, "rb");
        expect_true(f != NULL, "trunc fixture open");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            uint8_t *all = (uint8_t *)malloc((size_t)size);
            expect_true(all != NULL, "trunc buffer");
            if (all) {
                fseek(f, 0, SEEK_SET);
                size_t got = fread(all, 1, (size_t)size, f);
                fclose(f);
                expect_true(got == (size_t)size, "trunc read");
                f = fopen(path2, "wb");
                expect_true(f != NULL, "trunc rewrite open");
                if (f) {
                    fwrite(all, 1, (size_t)size - 100, f);
                    fclose(f);
                }
                free(all);
            } else {
                fclose(f);
            }
        }
    }
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path2, &info), "truncated .ldf validates");
    expect_true(info.total_samples > 0 && info.total_samples < N,
                "truncated total is a real partial length");
    expect_u64(info.total_samples % BLOCK, 0, "truncated total at a frame boundary");

    /* Prepare through the production path: remux to a session temp native
     * FLAC, then decode sample-exact via the producer decoder + IO layer. */
    test_case = "ogg_prepare_decode";
    playback_ctx_t *ctx = (playback_ctx_t *)calloc(1, sizeof(*ctx));
    expect_true(ctx != NULL, "Ogg context allocated");
    if (ctx) {
        expect_true(gui_playback_prepare(ctx, &s_test_app, ldf_path, NULL, NULL) == 0,
                    "Ogg channel prepares via the remux");
        expect_i64(ctx->input_format[0], PLAYBACK_INPUT_OGG_FLAC, "Ogg input format");
        expect_true(ctx->decoder_a != NULL, "native decoder on the temp");
        expect_true(ctx->decode_path_temp[0], "session temp registered");
        expect_u64(ctx->info_a.total_samples, N, "Ogg total = copied frame sum");
        expect_u64((uint64_t)ctx->channel_rate_hz[0], (uint64_t)TEST_RF_RATE_HZ,
                  "Ogg resolved real rate");
        expect_true(ctx->info_a.filepath[0] != '\0', "info filepath kept");
        {
            FILE *tmp = fopen(ctx->decode_path[0], "rb");
            expect_true(tmp != NULL, "session temp exists");
            if (tmp) {
                FLAC__StreamMetadata md;
                expect_true(FLAC__metadata_get_streaminfo(ctx->decode_path[0], &md),
                            "temp is a standalone native FLAC");
                expect_u64(md.data.stream_info.total_samples, N,
                           "temp STREAMINFO total = frame sum");
                fclose(tmp);
            }
        }
        if (ctx->decoder_a) {
            while (ctx->decode_available_a < N &&
                   FLAC__stream_decoder_get_state(ctx->decoder_a) != FLAC__STREAM_DECODER_END_OF_STREAM) {
                if (!FLAC__stream_decoder_process_single(ctx->decoder_a)) break;
            }
        }
        expect_u64(ctx->decode_available_a, N, "producer decoded the full remux");
        {
            bool all_match = true;
            for (uint32_t i = 0; i < N; i++) {
                int16_t want = (int16_t)(ref.samples[i] >> 4);  /* recorder grid >> 4 */
                if (ctx->decode_buf_a[i] != want) {
                    fprintf(stderr, "FAIL [%s]: Ogg sample %u got %d want %d\n",
                            test_case, i, (int)ctx->decode_buf_a[i], (int)want);
                    all_match = false;
                    break;
                }
            }
            checks++;
            if (!all_match) failures++;
        }
        char temp_path[512];
        snprintf(temp_path, sizeof(temp_path), "%s", ctx->decode_path[0]);
        gui_playback_release_context(ctx);
        free(ctx);
        FILE *gone = fopen(temp_path, "rb");
        expect_true(gone == NULL, "session temp unlinked on release");
        if (gone) fclose(gone);
    }

    test_ogg_nil_packets(temp_dir,&sp,&ref);

    native_split_free(&sp);
    decode_ref_free(&ref);
    unlink(native_path);
    unlink(ldf_path);
    snprintf(path2, sizeof(path2), "%s/misrc_pb_%u_ogg.oga", temp_dir, pid);
    unlink(path2);
    snprintf(path2, sizeof(path2), "%s/misrc_pb_%u_ogg_tag.ldf", temp_dir, pid);
    unlink(path2);
    snprintf(path2, sizeof(path2), "%s/misrc_pb_%u_ogg_trunc.ldf", temp_dir, pid);
    unlink(path2);
}

int main(void)
{
    char path_old[256];
    char path_new[256];
    const char *temp_dir = getenv("TMPDIR");
#if defined(_WIN32) || defined(_WIN64)
    if (!temp_dir || !temp_dir[0]) temp_dir = getenv("TEMP");
    if (!temp_dir || !temp_dir[0]) temp_dir = getenv("TMP");
    if (!temp_dir || !temp_dir[0]) temp_dir = ".";
#else
    if (!temp_dir || !temp_dir[0]) temp_dir = "/tmp";
#endif
    int old_len = snprintf(path_old, sizeof(path_old),
                           "%s/misrc_pb_%d_test_rf_8-bit_20msps.flac",
                           temp_dir, (int)getpid());
    int new_len = snprintf(path_new, sizeof(path_new),
                           "%s/misrc_pb_%d_test_fin_8-bit_20msps.flac",
                           temp_dir, (int)getpid());
    if (old_len < 0 || (size_t)old_len >= sizeof(path_old) ||
        new_len < 0 || (size_t)new_len >= sizeof(path_new)) {
        fprintf(stderr, "temporary fixture path is too long\n");
        return 1;
    }

    test_case = "generate";
    expect_true(write_test_flac(path_old, TEST_SAMPLES), "write OLD-convention flac");
    expect_true(write_test_flac(path_new, TEST_SAMPLES), "write NEW-convention base flac");
    expect_true(finalize_test_flac(path_new, TEST_SAMPLES), "finalize NEW-convention flac");

    playback_file_info_t info;

    test_case = "resolve_old_convention";
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path_old, &info), "OLD file validates");
    expect_u64(info.header_total_samples, TEST_SAMPLES, "OLD header total is raw count");
    expect_u64(info.total_samples, TEST_SAMPLES, "OLD resolved total is the raw count");
    expect_u64((uint64_t)info.real_sample_rate_hz, (uint64_t)TEST_RF_RATE_HZ,
               "OLD resolved real rate (msps filename hint)");
    expect_near(info.duration_seconds, (double)TEST_SAMPLES / TEST_RF_RATE_HZ,
                "OLD true duration");
    expect_true(info.bits_per_sample == 8, "OLD bit depth");

    test_case = "resolve_new_convention";
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path_new, &info), "NEW file validates");
    expect_u64(info.header_total_samples, ((uint64_t)TEST_SAMPLES + 500u) / 1000u,
               "NEW header total is kHz-domain");
    expect_u64(info.total_samples, TEST_SAMPLES,
               "NEW resolved total must be the TRUE stream count, not the header value");
    expect_u64((uint64_t)info.real_sample_rate_hz, (uint64_t)TEST_RF_RATE_HZ,
               "NEW resolved real rate (RF tag)");
    expect_near(info.duration_seconds, (double)TEST_SAMPLES / TEST_RF_RATE_HZ,
                "NEW true duration");

    test_io_layer_patch_and_seek(path_new, true);
    test_long_path(temp_dir, path_old);
#ifdef _WIN32
    test_unicode_paths(path_old, path_new);
#endif

    /* New reader formats + the 16-bit sample-convention detection: headerless
     * raw PCM (.u8/.s8/.u16/.s16 + reversed + .raw/.bin/.pcm), PCM WAV,
     * packed 10-bit .lds, Ogg FLAC (.ldf/.oga), container sniffing with
     * specific errors, and the per-file 16-bit convention. */
    test_conv16_conventions();
    test_raw_probe(temp_dir);
    test_wav_probe(temp_dir);
    test_raw_read_seek(temp_dir);
    test_sniff_errors(temp_dir);
    test_ogg_flac(temp_dir);

    unlink(path_old);
    unlink(path_new);

    /* Every fixture the new sections created under temp_dir. */
    {
        const uint32_t pid = (uint32_t)getpid();
        char cleanup[512];
        static const char *names[] = {
            "8-bit_20msps.u8", "s16_20msps.s16", "direct_20msps.16s", "10msps.u16",
            "capture.lds", "wav16_20msps.wav", "wav24.wav", "wav8.wav",
            "probe_8-bit_20msps.u8", "probe_8u_20msps.8u", "probe_nohint.s16",
            "probe_8-bit_20msps.raw", "probe_lds.lds", "probe_20msps.lds", "probe_10msps.u8",
            "wav_48k.wav", "wav_khz.wav", "wav_real.wav", "wav_clamp.wav", "wav_stereo.wav",
            "vorbis.ogg", "id3.flac", "mystery.xyz", "renamed.bin", "renamed.raw",
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            snprintf(cleanup, sizeof(cleanup), "%s/misrc_pb_%u_%s", temp_dir, pid, names[i]);
            unlink(cleanup);
        }
    }

    printf("gui_playback_resolve: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
