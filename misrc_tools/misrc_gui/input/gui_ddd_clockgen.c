/*
 * MISRC GUI - DdD + Clockgen Lite audio sync mode (implementation)
 *
 * See gui_ddd_clockgen.h for the design. This file owns the Clockgen Lite
 * ALSA audio capture lifecycle that runs in parallel with the DdD RF capture
 * (driven by gui_ddd.c via the existing DEVICE_TYPE_DDD path).
 *
 * Linux: full ALSA implementation under LIBASOUND_ENABLED.
 * Non-Linux / no-ALSA: stubs (detect=false, start=-1, stop no-op) so the rest
 * of the DdD path still builds and runs RF-only.
 */

#ifdef ENABLE_DDD

#include "gui_ddd_clockgen.h"

#include "../core/gui_app.h"
#include "../net/gui_monitor_audio.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <ctype.h>
#include <errno.h>

// Global exit flag (defined in misrc_gui.c).
extern volatile atomic_int do_exit;

// Clockgen Lite native rate. Matches ddd-capture-toolkit CLOCKGEN_SAMPLE_RATE
// and the GUI audio pipeline's AUDIO_DEFAULT_SAMPLE_RATE_HZ.
#define DDD_CLOCKGEN_SAMPLE_RATE_HZ   78125U
#define DDD_CLOCKGEN_CHANNELS         2
#define DDD_CLOCKGEN_READ_FRAMES      1024
// Output frame = 4ch s24le interleaved (12 bytes), matching BUF_CAPTURE_AUDIO.
#define DDD_CLOCKGEN_PACKED_FRAME_BYTES 12

//-----------------------------------------------------------------------------

bool gui_ddd_clockgen_device_mode(const device_info_t *dev)
{
    if (!dev) return false;
    if (dev->type != DEVICE_TYPE_DDD) return false;
    return strcmp(dev->serial, DDD_CLOCKGEN_MARKER_SERIAL) == 0;
}

//-----------------------------------------------------------------------------

#if !defined(_WIN32) && LIBASOUND_ENABLED

#include <unistd.h>
#include <fcntl.h>
#include <alsa/asoundlib.h>

typedef enum {
    DDD_CG_FMT_NONE = 0,
    DDD_CG_FMT_S24_3LE,
    DDD_CG_FMT_S32_LE,
    DDD_CG_FMT_S16_LE,
} ddd_cg_format_t;

typedef struct {
    gui_app_t *app;
    atomic_bool running;
    thrd_t audio_thread;
    bool audio_thread_started;
    snd_pcm_t *pcm;
    ddd_cg_format_t format;
    size_t sample_bytes;          // bytes per channel sample
    uint32_t sample_rate_hz;      // actual configured rate (~78125)
    char device_name[64];
} ddd_cg_ctx_t;

static ddd_cg_ctx_t s_ddd_cg = {0};

static inline void ddd_cg_store_s24le(uint8_t *dst, int32_t sample)
{
    if (sample > 8388607) sample = 8388607;
    if (sample < -8388608) sample = -8388608;
    uint32_t raw = (uint32_t)(sample & 0x00FFFFFF);
    dst[0] = (uint8_t)(raw & 0xFF);
    dst[1] = (uint8_t)((raw >> 8) & 0xFF);
    dst[2] = (uint8_t)((raw >> 16) & 0xFF);
}

static bool ddd_cg_str_contains_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

// Match an ALSA card name/longname against Clockgen Lite identifiers.
// Mirrors ddd-capture-toolkit CLOCKGEN_DEVICE_PATTERNS (config.py) plus the
// cxadc clockgen name match (gui_cxadc.c).
static bool ddd_cg_card_name_matches(const char *name, const char *longname)
{
    static const char *patterns[] = {
        "clockgen", "cxadc", "pcm270", "pcm2707", "pcm2706", NULL
    };
    for (int i = 0; patterns[i]; i++) {
        if (ddd_cg_str_contains_nocase(name, patterns[i]) ||
            ddd_cg_str_contains_nocase(longname, patterns[i])) {
            return true;
        }
    }
    return false;
}

// Read /proc/asound/cardN/stream0 and look for the 78125 Hz Clockgen Lite
// rate (matches ddd-capture-toolkit get_device_info_linux rate detection).
static bool ddd_cg_card_supports_78125(int card)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/asound/card%d/stream0", card);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    return strstr(buf, "78125") != NULL;
}

bool gui_ddd_clockgen_detect(void)
{
    // Env override: if set, treat as detected (the start path will try to
    // open exactly that ALSA device id).
    const char *env_device = getenv("MISRC_DDD_CLOCKGEN_ALSA_DEVICE");
    if (env_device && env_device[0]) {
        return true;
    }

    int card = -1;
    if (snd_card_next(&card) < 0) {
        return false;
    }
    while (card >= 0) {
        char *name = NULL;
        char *longname = NULL;
        (void)snd_card_get_name(card, &name);
        (void)snd_card_get_longname(card, &longname);

        bool matched = ddd_cg_card_name_matches(name ? name : "",
                                                longname ? longname : "");
        if (!matched) {
            // Fall back to the 78125 Hz sample-rate signature.
            matched = ddd_cg_card_supports_78125(card);
        }

        if (name) free(name);
        if (longname) free(longname);

        if (matched) {
            return true;
        }

        if (snd_card_next(&card) < 0) break;
    }
    return false;
}

static int ddd_cg_configure_pcm(snd_pcm_t *pcm,
                                snd_pcm_format_t format,
                                unsigned int *out_rate_hz)
{
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_hw_params_alloca(&params);

    if (snd_pcm_hw_params_any(pcm, params) < 0) return -1;
    if (snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) return -1;
    if (snd_pcm_hw_params_set_format(pcm, params, format) < 0) return -1;
    if (snd_pcm_hw_params_set_channels(pcm, params, DDD_CLOCKGEN_CHANNELS) < 0) return -1;

    unsigned int rate_hz = DDD_CLOCKGEN_SAMPLE_RATE_HZ;
    int dir = 0;
    if (snd_pcm_hw_params_set_rate_near(pcm, params, &rate_hz, &dir) < 0) return -1;
    // Accept the Clockgen Lite native rate (allow a small near-tolerance).
    if (rate_hz < 78000 || rate_hz > 78200) return -1;

    snd_pcm_uframes_t period_frames = DDD_CLOCKGEN_READ_FRAMES;
    snd_pcm_uframes_t buffer_frames = DDD_CLOCKGEN_READ_FRAMES * 8;
    (void)snd_pcm_hw_params_set_period_size_near(pcm, params, &period_frames, &dir);
    (void)snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_frames);

    if (snd_pcm_hw_params(pcm, params) < 0) return -1;
    if (snd_pcm_prepare(pcm) < 0) return -1;
    if (snd_pcm_nonblock(pcm, 1) < 0) return -1;

    if (out_rate_hz) {
        *out_rate_hz = rate_hz;
    }
    return 0;
}

static int ddd_cg_try_open_device(ddd_cg_ctx_t *ctx, const char *device_name)
{
    if (!ctx || !device_name || !device_name[0]) return -1;

    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        return -1;
    }

    static const struct {
        snd_pcm_format_t alsa_format;
        ddd_cg_format_t cg_format;
        size_t sample_bytes;
    } formats[] = {
        { SND_PCM_FORMAT_S24_3LE, DDD_CG_FMT_S24_3LE, 3 },
        { SND_PCM_FORMAT_S32_LE,  DDD_CG_FMT_S32_LE,  4 },
        { SND_PCM_FORMAT_S16_LE,  DDD_CG_FMT_S16_LE,  2 },
    };

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        unsigned int configured_rate = DDD_CLOCKGEN_SAMPLE_RATE_HZ;
        if (ddd_cg_configure_pcm(pcm, formats[i].alsa_format, &configured_rate) == 0) {
            ctx->pcm = pcm;
            ctx->format = formats[i].cg_format;
            ctx->sample_bytes = formats[i].sample_bytes;
            ctx->sample_rate_hz = configured_rate;
            snprintf(ctx->device_name, sizeof(ctx->device_name), "%s", device_name);
            return 0;
        }
    }

    snd_pcm_close(pcm);
    return -1;
}

// Probe ALSA cards for a clockgen device and open the first match. Tries the
// named-card ALSA ids (usbstream/hw/plughw:CARD=...) for each matching card,
// then falls back to probing every card.
static int ddd_cg_open_audio_capture(ddd_cg_ctx_t *ctx)
{
    if (!ctx) return -1;

    const char *env_device = getenv("MISRC_DDD_CLOCKGEN_ALSA_DEVICE");
    if (env_device && env_device[0]) {
        if (ddd_cg_try_open_device(ctx, env_device) == 0) {
            return 0;
        }
        // Fall through to probing if the override didn't open.
    }

    int card = -1;
    if (snd_card_next(&card) < 0) return -1;

    bool matched_any = false;
    while (card >= 0) {
        char *name = NULL;
        char *longname = NULL;
        (void)snd_card_get_name(card, &name);
        (void)snd_card_get_longname(card, &longname);

        bool matched = ddd_cg_card_name_matches(name ? name : "",
                                                longname ? longname : "");
        if (!matched) {
            matched = ddd_cg_card_supports_78125(card);
        }

        if (name) free(name);
        if (longname) free(longname);

        if (matched) {
            matched_any = true;
            char usbstream_dev[40];
            char hw_dev[24];
            char plughw_dev[28];
            snprintf(usbstream_dev, sizeof(usbstream_dev), "usbstream:CARD=%d", card);
            snprintf(hw_dev, sizeof(hw_dev), "hw:%d", card);
            snprintf(plughw_dev, sizeof(plughw_dev), "plughw:%d", card);
            if (ddd_cg_try_open_device(ctx, usbstream_dev) == 0 ||
                ddd_cg_try_open_device(ctx, hw_dev) == 0 ||
                ddd_cg_try_open_device(ctx, plughw_dev) == 0) {
                return 0;
            }
        }

        if (snd_card_next(&card) < 0) break;
    }

    // Last resort: if no card name/rate matched, walk every card once more.
    if (matched_any) {
        return -1;  // a clockgen card was found but wouldn't open; don't grab a random one.
    }

    card = -1;
    if (snd_card_next(&card) < 0) return -1;
    while (card >= 0) {
        char usbstream_dev[40];
        char hw_dev[24];
        char plughw_dev[28];
        snprintf(usbstream_dev, sizeof(usbstream_dev), "usbstream:CARD=%d", card);
        snprintf(hw_dev, sizeof(hw_dev), "hw:%d", card);
        snprintf(plughw_dev, sizeof(plughw_dev), "plughw:%d", card);
        if (ddd_cg_try_open_device(ctx, usbstream_dev) == 0 ||
            ddd_cg_try_open_device(ctx, hw_dev) == 0 ||
            ddd_cg_try_open_device(ctx, plughw_dev) == 0) {
            return 0;
        }
        if (snd_card_next(&card) < 0) break;
    }

    return -1;
}

static int ddd_cg_audio_capture_thread(void *ctx_ptr)
{
    ddd_cg_ctx_t *ctx = (ddd_cg_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app || !ctx->pcm ||
        ctx->format == DDD_CG_FMT_NONE || ctx->sample_bytes == 0) {
        return 0;
    }
    gui_app_t *app = ctx->app;

    thrd_set_priority(THRD_PRIORITY_ABOVE);

    const size_t input_frame_bytes = (size_t)DDD_CLOCKGEN_CHANNELS * ctx->sample_bytes;
    const size_t input_buffer_bytes = (size_t)DDD_CLOCKGEN_READ_FRAMES * input_frame_bytes;
    uint8_t *input_buf = (uint8_t *)malloc(input_buffer_bytes);
    if (!input_buf) {
        gui_app_set_status(app, "DdD Clockgen: failed to allocate ALSA audio buffer");
        return -1;
    }

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        snd_pcm_sframes_t frames = snd_pcm_readi(ctx->pcm, input_buf, DDD_CLOCKGEN_READ_FRAMES);
        if (frames == -EAGAIN || frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }
        if (frames == -EPIPE) {
            (void)snd_pcm_prepare(ctx->pcm);
            continue;
        }
        if (frames < 0) {
            int rec = snd_pcm_recover(ctx->pcm, (int)frames, 1);
            if (rec >= 0) {
                continue;
            }
            gui_app_set_status(app, "DdD Clockgen ALSA audio read error");
            break;
        }

        const size_t output_bytes = (size_t)frames * DDD_CLOCKGEN_PACKED_FRAME_BYTES;
        uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO,
                                                     output_bytes, NULL);
        if (!out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        // Pack 2ch input -> 4ch s24le interleaved (CH1=L, CH2=R, CH3=0, CH4=0).
        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            const uint8_t *src_frame = input_buf + ((size_t)i * input_frame_bytes);
            uint8_t *dst_frame = out + ((size_t)i * DDD_CLOCKGEN_PACKED_FRAME_BYTES);

            for (int ch = 0; ch < 2; ch++) {
                const uint8_t *src = src_frame + ((size_t)ch * ctx->sample_bytes);
                uint8_t *dst = dst_frame + ((size_t)ch * 3);
                if (ctx->format == DDD_CG_FMT_S24_3LE) {
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                } else if (ctx->format == DDD_CG_FMT_S32_LE) {
                    int32_t s32 = (int32_t)((uint32_t)src[0] |
                                            ((uint32_t)src[1] << 8) |
                                            ((uint32_t)src[2] << 16) |
                                            ((uint32_t)src[3] << 24));
                    ddd_cg_store_s24le(dst, s32 >> 8);
                } else if (ctx->format == DDD_CG_FMT_S16_LE) {
                    int16_t s16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                    ddd_cg_store_s24le(dst, ((int32_t)s16) << 8);
                } else {
                    dst[0] = dst[1] = dst[2] = 0;
                }
            }
            // CH3, CH4 are silent (Clockgen Lite has no headswitch channel).
            dst_frame[6] = dst_frame[7] = dst_frame[8] = 0;
            dst_frame[9] = dst_frame[10] = dst_frame[11] = 0;
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
        gui_monitor_audio_server_tap(app,out,output_bytes);
        atomic_store(&app->audio_sample_rate, ctx->sample_rate_hz);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
    }

    free(input_buf);
    return 0;
}

int gui_ddd_clockgen_start(gui_app_t *app)
{
    if (!app) return -1;
    if (atomic_load(&s_ddd_cg.running)) return 0;

    memset(&s_ddd_cg, 0, sizeof(s_ddd_cg));
    s_ddd_cg.app = app;

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        gui_app_set_status(app, "DdD Clockgen: failed to initialize audio buffer");
        return -1;
    }

    if (ddd_cg_open_audio_capture(&s_ddd_cg) != 0) {
        fprintf(stderr, "[DdD Clockgen] clockgen audio device not available; "
                        "continuing RF-only\n");
        fprintf(stderr, "[DdD Clockgen] hint: set MISRC_DDD_CLOCKGEN_ALSA_DEVICE "
                        "to an ALSA device id (e.g. hw:CARD=2) to force a device\n");
        return -1;
    }

    if (s_ddd_cg.sample_rate_hz > 0) {
        atomic_store(&app->audio_sample_rate, s_ddd_cg.sample_rate_hz);
    }
    fprintf(stderr, "[DdD Clockgen] audio capture device: %s (%u Hz, %d ch)\n",
            s_ddd_cg.device_name, s_ddd_cg.sample_rate_hz, DDD_CLOCKGEN_CHANNELS);

    atomic_store(&s_ddd_cg.running, true);
    if (thrd_create_with_priority(&s_ddd_cg.audio_thread,
                                   ddd_cg_audio_capture_thread,
                                   &s_ddd_cg,
                                   THRD_PRIORITY_ABOVE) != thrd_success) {
        fprintf(stderr, "[DdD Clockgen] Failed to start audio capture thread; "
                        "continuing RF-only\n");
        if (s_ddd_cg.pcm) {
            snd_pcm_drop(s_ddd_cg.pcm);
            snd_pcm_close(s_ddd_cg.pcm);
            s_ddd_cg.pcm = NULL;
        }
        atomic_store(&s_ddd_cg.running, false);
        return -1;
    }
    s_ddd_cg.audio_thread_started = true;
    return 0;
}

void gui_ddd_clockgen_stop(gui_app_t *app)
{
    (void)app;
    if (!s_ddd_cg.audio_thread_started && !s_ddd_cg.pcm) {
        return;
    }

    atomic_store(&s_ddd_cg.running, false);
    if (s_ddd_cg.pcm) {
        (void)snd_pcm_drop(s_ddd_cg.pcm);
    }

    if (s_ddd_cg.audio_thread_started) {
        thrd_join(s_ddd_cg.audio_thread, NULL);
        s_ddd_cg.audio_thread_started = false;
    }

    if (s_ddd_cg.pcm) {
        snd_pcm_close(s_ddd_cg.pcm);
        s_ddd_cg.pcm = NULL;
    }
    s_ddd_cg.format = DDD_CG_FMT_NONE;
    s_ddd_cg.sample_bytes = 0;
    s_ddd_cg.sample_rate_hz = 0;
    s_ddd_cg.device_name[0] = '\0';
}

bool gui_ddd_clockgen_is_running(void)
{
    return atomic_load(&s_ddd_cg.running);
}

#else // !_WIN32 && LIBASOUND_ENABLED -> stubs (no ALSA on this platform/build)

bool gui_ddd_clockgen_detect(void)
{
    // No ALSA on this platform; the env override is the only way to force one.
    const char *env_device = getenv("MISRC_DDD_CLOCKGEN_ALSA_DEVICE");
    return env_device && env_device[0];
}

int gui_ddd_clockgen_start(gui_app_t *app)
{
    (void)app;
    fprintf(stderr, "[DdD Clockgen] ALSA audio capture not built; continuing RF-only\n");
    return -1;
}

void gui_ddd_clockgen_stop(gui_app_t *app)
{
    (void)app;
}

bool gui_ddd_clockgen_is_running(void)
{
    return false;
}

#endif // LIBASOUND_ENABLED

#endif // ENABLE_DDD
