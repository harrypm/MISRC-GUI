#include "gui_cxadc.h"

#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>

#ifndef LIBASOUND_ENABLED
#define LIBASOUND_ENABLED 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Shield raylib symbols from conflicting Win32 API declarations
// (same pattern as gui_capture.c): raylib.h is already included via gui_app.h.
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#undef Rectangle
#undef CloseWindow
#undef ShowCursor
// Define PKEY_Device_FriendlyName locally to avoid initguid conflicts.
static const PROPERTYKEY s_PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};
// PKEY_Device_InstanceId: returns device instance path like USB\VID_1209&PID_0002&MI_00\...
static const PROPERTYKEY s_PKEY_Device_InstanceId = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0xfc } }, 42
};
// cxadc-win clockgen USB identifiers (Raspberry Pi Pico + Si5351 clockgen mod).
#define CXADC_CLOCKGEN_USB_VID_W L"vid_1209"
#define CXADC_CLOCKGEN_USB_PID_W L"pid_0002"
#else
#include <unistd.h>
#include <fcntl.h>
#if LIBASOUND_ENABLED
#include <alsa/asoundlib.h>
#endif
#endif

extern volatile atomic_int do_exit;

#define CXADC_MAX_CARDS 2
#define CXADC_SAMPLE_RATE_HZ 40000000U
#define CXADC_READ_CHUNK_BYTES 65536
#define CXADC_AUDIO_SAMPLE_RATE_HZ 46875U
#define CXADC_AUDIO_CHANNEL_COUNT 3
#define CXADC_AUDIO_READ_FRAMES 1024
#define CXADC_AUDIO_PACKED_FRAME_BYTES 12

typedef enum {
    CXADC_AUDIO_FMT_NONE = 0,
    CXADC_AUDIO_FMT_S24_3LE,
    CXADC_AUDIO_FMT_S32_LE,
    CXADC_AUDIO_FMT_S16_LE,
    CXADC_AUDIO_FMT_FLOAT32
} cxadc_audio_format_t;

typedef struct {
    gui_app_t *app;
    atomic_bool running;
    thrd_t rf_thread;
    bool rf_thread_started;
    thrd_t audio_thread;
    bool audio_thread_started;
    int card_count;
    // Audio format (shared across platforms)
    cxadc_audio_format_t audio_format;
    size_t audio_sample_bytes;
    uint32_t audio_sample_rate_hz;
    char audio_device_name[64];
    int audio_channels;
#if defined(_WIN32)
    HANDLE card_handles[CXADC_MAX_CARDS];
    // WASAPI clockgen audio capture
    IMMDevice *audio_endpoint;
    IAudioClient *audio_client;
    IAudioCaptureClient *audio_capture;
    bool audio_com_initialized;
#else
    int card_fds[CXADC_MAX_CARDS];
#if LIBASOUND_ENABLED
    snd_pcm_t *audio_pcm;
#endif
#endif
} cxadc_ctx_t;

static cxadc_ctx_t s_cxadc = {0};

static inline uint32_t cxadc_encode_raw_sample(int16_t sample_a, int16_t sample_b)
{
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    return ch_a | (ch_b << 20);
}

static void cxadc_reset_stats(gui_app_t *app)
{
    if (!app) return;
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

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
    atomic_store(&app->sample_rate, CXADC_SAMPLE_RATE_HZ);
    atomic_store(&app->audio_sample_rate, CXADC_AUDIO_SAMPLE_RATE_HZ);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    for (int i = 0; i < 4; i++) {
        atomic_store(&app->audio_peak[i], 0);
    }

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;
}

#if defined(_WIN32)
static HANDLE cxadc_open_card(int card_idx)
{
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\cxadc%d", card_idx);
    return CreateFileA(path,
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

static int cxadc_read_card(HANDLE handle, uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    DWORD read_bytes = 0;
    BOOL ok = ReadFile(handle, buf, (DWORD)len, &read_bytes, NULL);
    if (!ok) {
        return -1;
    }
    return (int)read_bytes;
}

static void cxadc_close_handle(HANDLE *handle)
{
    if (!handle) return;
    if (*handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
}
#else
static int cxadc_open_card(int card_idx)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/cxadc%d", card_idx);
    return open(path, O_RDONLY);
}

static int cxadc_read_card(int fd, uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || fd < 0) {
        return -1;
    }

    ssize_t r = read(fd, buf, len);
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    return (int)r;
}

static void cxadc_close_fd(int *fd)
{
    if (!fd) return;
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
#endif

static void cxadc_close_cards(cxadc_ctx_t *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        cxadc_close_handle(&ctx->card_handles[i]);
#else
        cxadc_close_fd(&ctx->card_fds[i]);
#endif
    }
}

static int cxadc_open_cards(cxadc_ctx_t *ctx, int card_count)
{
    if (!ctx) return -1;
    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    for (int i = 0; i < card_count; i++) {
#if defined(_WIN32)
        ctx->card_handles[i] = cxadc_open_card(i);
        if (ctx->card_handles[i] == INVALID_HANDLE_VALUE) {
            cxadc_close_cards(ctx);
            return -1;
        }
#else
        ctx->card_fds[i] = cxadc_open_card(i);
        if (ctx->card_fds[i] < 0) {
            cxadc_close_cards(ctx);
            return -1;
        }
#endif
    }
    return 0;
}

int gui_cxadc_detect_cards(void)
{
    int count = 0;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        HANDLE h = cxadc_open_card(i);
        if (h != INVALID_HANDLE_VALUE) {
            count++;
            CloseHandle(h);
        }
#else
        char path[32];
        snprintf(path, sizeof(path), "/dev/cxadc%d", i);
        if (access(path, R_OK) == 0 || access(path, W_OK) == 0) {
            count++;
        }
#endif
    }
    return count;
}

static inline void cxadc_store_s24le(uint8_t *dst, int32_t sample)
{
    if (sample > 8388607) sample = 8388607;
    if (sample < -8388608) sample = -8388608;
    uint32_t raw = (uint32_t)(sample & 0x00FFFFFF);
    dst[0] = (uint8_t)(raw & 0xFF);
    dst[1] = (uint8_t)((raw >> 8) & 0xFF);
    dst[2] = (uint8_t)((raw >> 16) & 0xFF);
}

#if !defined(_WIN32) && LIBASOUND_ENABLED
typedef struct {
    snd_pcm_format_t alsa_format;
    cxadc_audio_format_t cxadc_format;
    size_t sample_bytes;
} cxadc_audio_format_desc_t;

static int cxadc_configure_audio_pcm(snd_pcm_t *pcm,
                                     snd_pcm_format_t format,
                                     unsigned int *out_rate_hz)
{
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_hw_params_alloca(&params);

    if (snd_pcm_hw_params_any(pcm, params) < 0) return -1;
    if (snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) return -1;
    if (snd_pcm_hw_params_set_format(pcm, params, format) < 0) return -1;
    if (snd_pcm_hw_params_set_channels(pcm, params, CXADC_AUDIO_CHANNEL_COUNT) < 0) return -1;

    unsigned int rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
    int dir = 0;
    if (snd_pcm_hw_params_set_rate_near(pcm, params, &rate_hz, &dir) < 0) return -1;
    if (rate_hz < 46000 || rate_hz > 48000) return -1;

    snd_pcm_uframes_t period_frames = CXADC_AUDIO_READ_FRAMES;
    snd_pcm_uframes_t buffer_frames = CXADC_AUDIO_READ_FRAMES * 8;
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

static int cxadc_try_open_audio_device(cxadc_ctx_t *ctx, const char *device_name)
{
    if (!ctx || !device_name || !device_name[0]) return -1;

    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        return -1;
    }

    static const cxadc_audio_format_desc_t formats[] = {
        { SND_PCM_FORMAT_S24_3LE, CXADC_AUDIO_FMT_S24_3LE, 3 },
        { SND_PCM_FORMAT_S32_LE,  CXADC_AUDIO_FMT_S32_LE,  4 },
        { SND_PCM_FORMAT_S16_LE,  CXADC_AUDIO_FMT_S16_LE,  2 },
    };

    for (size_t i = 0; i < (sizeof(formats) / sizeof(formats[0])); i++) {
        unsigned int configured_rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
        if (cxadc_configure_audio_pcm(pcm, formats[i].alsa_format, &configured_rate_hz) == 0) {
            ctx->audio_pcm = pcm;
            ctx->audio_format = formats[i].cxadc_format;
            ctx->audio_sample_bytes = formats[i].sample_bytes;
            ctx->audio_sample_rate_hz = configured_rate_hz;
            snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name), "%s", device_name);
            return 0;
        }
    }

    snd_pcm_close(pcm);
    return -1;
}

static int cxadc_open_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;

    const char *env_device = getenv("MISRC_CXADC_ALSA_DEVICE");
    const char *candidates[] = {
        env_device,
        "hw:CARD=CXADCADCClockGe",
        "hw:CARD=CXADCADCClockGen",
        "plughw:CARD=CXADCADCClockGe",
        "plughw:CARD=CXADCADCClockGen",
        "default",
        NULL
    };

    for (size_t i = 0; candidates[i]; i++) {
        if (cxadc_try_open_audio_device(ctx, candidates[i]) == 0) {
            return 0;
        }
    }
    return -1;
}

static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
}

static void cxadc_close_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
    snd_pcm_close(ctx->audio_pcm);
    ctx->audio_pcm = NULL;
    ctx->audio_format = CXADC_AUDIO_FMT_NONE;
    ctx->audio_sample_bytes = 0;
    ctx->audio_sample_rate_hz = 0;
    ctx->audio_device_name[0] = '\0';
}
#elif defined(_WIN32)
// --- WASAPI clockgen audio capture (cxadc-win) ---

static const GUID s_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID s_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
// Explicit COM interface/class GUIDs for MinGW builds where the extern IID/CLSID
// symbols are declared by headers but not provided by import libs.
static const GUID s_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID s_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID s_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID s_IID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48a0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

static bool cxadc_guid_equal(const GUID *a, const GUID *b)
{
    return a->Data1 == b->Data1 && a->Data2 == b->Data2 &&
           a->Data3 == b->Data3 && memcmp(a->Data4, b->Data4, 8) == 0;
}

static int cxadc_wasapi_parse_format(const WAVEFORMATEX *wfx, cxadc_ctx_t *ctx)
{
    if (!wfx || !ctx) return -1;

    WORD bits = wfx->wBitsPerSample;
    WORD valid_bits = bits;
    WORD format_tag = wfx->wFormatTag;
    bool is_float = false;

    if (format_tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE *wfxe = (WAVEFORMATEXTENSIBLE *)wfx;
        valid_bits = wfxe->Samples.wValidBitsPerSample;
        if (cxadc_guid_equal(&wfxe->SubFormat, &s_KSDATAFORMAT_SUBTYPE_PCM)) {
            format_tag = WAVE_FORMAT_PCM;
        } else if (cxadc_guid_equal(&wfxe->SubFormat, &s_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            is_float = true;
        } else {
            return -1;
        }
    } else if (format_tag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (format_tag != WAVE_FORMAT_PCM) {
        return -1;
    }

    if (is_float) {
        if (bits != 32) return -1;
        ctx->audio_format = CXADC_AUDIO_FMT_FLOAT32;
        ctx->audio_sample_bytes = 4;
    } else if (valid_bits <= 16 && bits <= 16) {
        ctx->audio_format = CXADC_AUDIO_FMT_S16_LE;
        ctx->audio_sample_bytes = 2;
    } else if (valid_bits <= 24 && bits <= 24) {
        ctx->audio_format = CXADC_AUDIO_FMT_S24_3LE;
        ctx->audio_sample_bytes = 3;
    } else if (valid_bits <= 24 && bits <= 32) {
        ctx->audio_format = CXADC_AUDIO_FMT_S32_LE;
        ctx->audio_sample_bytes = 4;
    } else if (valid_bits <= 32 && bits <= 32) {
        ctx->audio_format = CXADC_AUDIO_FMT_S32_LE;
        ctx->audio_sample_bytes = 4;
    } else {
        return -1;
    }

    ctx->audio_channels = (int)wfx->nChannels;
    if (ctx->audio_channels < 1) ctx->audio_channels = 1;
    ctx->audio_sample_rate_hz = (uint32_t)wfx->nSamplesPerSec;
    return 0;
}

// Match a WASAPI capture endpoint against the cxadc-win clockgen.
// Checks (a) explicit env override, (b) USB instance path VID/PID, (c) friendly name.
static bool cxadc_wasapi_device_matches(const wchar_t *friendly_name,
                                        const wchar_t *instance_id)
{
    const char *env_device = getenv("MISRC_CXADC_WASAPI_DEVICE");
    if (env_device && env_device[0]) {
        wchar_t env_w[256];
        int len = MultiByteToWideChar(CP_UTF8, 0, env_device, -1, env_w, 256);
        if (len > 0) {
            if (friendly_name && _wcsicmp(friendly_name, env_w) == 0) return true;
            if (friendly_name && wcsstr(friendly_name, env_w) != NULL) return true;
            if (instance_id && wcsstr(instance_id, env_w) != NULL) return true;
        }
    }
    // Clockgen mod is USB VID 1209 PID 0002 (Raspberry Pi Pico + Si5351).
    if (instance_id) {
        wchar_t id_lower[512];
        wcsncpy(id_lower, instance_id, 511);
        id_lower[511] = L'\0';
        _wcslwr(id_lower);
        if (wcsstr(id_lower, CXADC_CLOCKGEN_USB_VID_W) &&
            wcsstr(id_lower, CXADC_CLOCKGEN_USB_PID_W)) {
            return true;
        }
    }
    // Fallback: match clockgen identifiers in friendly name.
    if (friendly_name) {
        const wchar_t *patterns[] = { L"CXADC", L"ClockGen", L"Clockgen", L"clockgen", NULL };
        for (int i = 0; patterns[i]; i++) {
            if (wcsstr(friendly_name, patterns[i]) != NULL) return true;
        }
    }
    return false;
}

// Clockgen ADC supports 48000 or 46875 Hz (configurable via CxadcClockGen module).
static bool cxadc_wasapi_rate_supported(uint32_t rate_hz)
{
    return rate_hz == 48000U || rate_hz == 46875U;
}

static int cxadc_open_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return -1;
    }
    bool com_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator *pEnum = NULL;
    hr = CoCreateInstance(&s_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &s_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) {
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IMMDeviceCollection *pCollection = NULL;
    hr = pEnum->lpVtbl->EnumAudioEndpoints(pEnum, eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    UINT count = 0;
    pCollection->lpVtbl->GetCount(pCollection, &count);

    IMMDevice *pDevice = NULL;
    bool found = false;
    for (UINT i = 0; i < count && !found; i++) {
        hr = pCollection->lpVtbl->Item(pCollection, i, &pDevice);
        if (FAILED(hr)) continue;

        IPropertyStore *pProps = NULL;
        hr = pDevice->lpVtbl->OpenPropertyStore(pDevice, STGM_READ, &pProps);
        if (FAILED(hr)) {
            pDevice->lpVtbl->Release(pDevice);
            pDevice = NULL;
            continue;
        }

        wchar_t *friendly_name_w = NULL;
        PROPVARIANT pv_name;
        memset(&pv_name, 0, sizeof(pv_name));
        hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_FriendlyName, &pv_name);
        if (SUCCEEDED(hr) && pv_name.vt == VT_LPWSTR && pv_name.pwszVal) {
            friendly_name_w = pv_name.pwszVal;
        }

        wchar_t *instance_id_w = NULL;
        PROPVARIANT pv_id;
        memset(&pv_id, 0, sizeof(pv_id));
        hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_InstanceId, &pv_id);
        if (SUCCEEDED(hr) && pv_id.vt == VT_LPWSTR && pv_id.pwszVal) {
            instance_id_w = pv_id.pwszVal;
        }

        if (cxadc_wasapi_device_matches(friendly_name_w, instance_id_w)) {
            found = true;
            const wchar_t *label = friendly_name_w ? friendly_name_w : instance_id_w;
            snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name),
                     "WASAPI:%ls", label ? label : L"clockgen");
        }

        if (pv_name.vt == VT_LPWSTR && pv_name.pwszVal) CoTaskMemFree(pv_name.pwszVal);
        if (pv_id.vt == VT_LPWSTR && pv_id.pwszVal) CoTaskMemFree(pv_id.pwszVal);
        pProps->lpVtbl->Release(pProps);
        if (!found) {
            pDevice->lpVtbl->Release(pDevice);
            pDevice = NULL;
        }
    }
    pCollection->lpVtbl->Release(pCollection);

    if (!found || !pDevice) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IAudioClient *pClient = NULL;
    hr = pDevice->lpVtbl->Activate(pDevice, &s_IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void **)&pClient);
    if (FAILED(hr)) {
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    WAVEFORMATEX *pwfx = NULL;
    hr = pClient->lpVtbl->GetMixFormat(pClient, &pwfx);
    if (FAILED(hr) || !pwfx) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    if (cxadc_wasapi_parse_format(pwfx, ctx) != 0) {
        CoTaskMemFree(pwfx);
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    // Clockgen ADC supports 48000 or 46875 Hz. Warn (but continue) if the
    // device reports a different rate — the user may need to configure the
    // clockgen ADC rate via the CxadcClockGen PowerShell module first.
    if (!cxadc_wasapi_rate_supported(ctx->audio_sample_rate_hz)) {
        fprintf(stderr, "[CXADC] WASAPI: device rate %u Hz is not 48000/46875; "
                "configure clockgen ADC rate via CxadcClockGen module\n",
                ctx->audio_sample_rate_hz);
    }

    REFERENCE_TIME buffer_duration = 500000; // 50ms in 100ns units
    hr = pClient->lpVtbl->Initialize(pClient, AUDCLNT_SHAREMODE_SHARED, 0,
                                     buffer_duration, 0, pwfx, NULL);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IAudioCaptureClient *pCapture = NULL;
    hr = pClient->lpVtbl->GetService(pClient, &s_IID_IAudioCaptureClient,
                                     (void **)&pCapture);
    if (FAILED(hr)) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    hr = pClient->lpVtbl->Start(pClient);
    if (FAILED(hr)) {
        pCapture->lpVtbl->Release(pCapture);
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    ctx->audio_endpoint = pDevice;
    ctx->audio_client = pClient;
    ctx->audio_capture = pCapture;
    ctx->audio_com_initialized = com_initialized;
    pEnum->lpVtbl->Release(pEnum);
    return 0;
}

static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_client) return;
    ctx->audio_client->lpVtbl->Stop(ctx->audio_client);
}

static void cxadc_close_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->audio_client) {
        ctx->audio_client->lpVtbl->Stop(ctx->audio_client);
    }
    if (ctx->audio_capture) {
        ctx->audio_capture->lpVtbl->Release(ctx->audio_capture);
        ctx->audio_capture = NULL;
    }
    if (ctx->audio_client) {
        ctx->audio_client->lpVtbl->Release(ctx->audio_client);
        ctx->audio_client = NULL;
    }
    if (ctx->audio_endpoint) {
        ctx->audio_endpoint->lpVtbl->Release(ctx->audio_endpoint);
        ctx->audio_endpoint = NULL;
    }
    ctx->audio_format = CXADC_AUDIO_FMT_NONE;
    ctx->audio_sample_bytes = 0;
    ctx->audio_sample_rate_hz = 0;
    ctx->audio_channels = 0;
    ctx->audio_device_name[0] = '\0';
    if (ctx->audio_com_initialized) {
        CoUninitialize();
        ctx->audio_com_initialized = false;
    }
}
#else
static int cxadc_open_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; return -1; }
static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
static void cxadc_close_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
#endif

static int cxadc_audio_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;
    gui_app_t *app = ctx->app;

#if defined(_WIN32)
    if (!ctx->audio_capture || !ctx->audio_client ||
        ctx->audio_format == CXADC_AUDIO_FMT_NONE || ctx->audio_sample_bytes == 0) {
        return 0;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);
    int dev_channels = ctx->audio_channels;
    if (dev_channels < 1) dev_channels = 1;
    size_t input_frame_bytes = (size_t)dev_channels * ctx->audio_sample_bytes;

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        UINT32 packet_frames = 0;
        HRESULT hr = ctx->audio_capture->lpVtbl->GetNextPacketSize(ctx->audio_capture, &packet_frames);
        if (FAILED(hr)) {
            gui_app_set_status(app, "CXADC WASAPI audio read error");
            break;
        }
        if (packet_frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        while (packet_frames > 0) {
            BYTE *data = NULL;
            UINT32 frames_available = 0;
            DWORD flags = 0;

            hr = ctx->audio_capture->lpVtbl->GetBuffer(ctx->audio_capture, &data,
                                                       &frames_available, &flags,
                                                       NULL, NULL);
            if (FAILED(hr)) {
                gui_app_set_status(app, "CXADC WASAPI GetBuffer error");
                goto cxadc_wasapi_done;
            }

            if (data && frames_available > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                size_t output_bytes = (size_t)frames_available * CXADC_AUDIO_PACKED_FRAME_BYTES;
                uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO,
                                                             output_bytes, NULL);
                if (out) {
                    for (UINT32 i = 0; i < frames_available; i++) {
                        const uint8_t *src_frame = data + ((size_t)i * input_frame_bytes);
                        uint8_t *dst_frame = out + ((size_t)i * CXADC_AUDIO_PACKED_FRAME_BYTES);
                        for (int ch = 0; ch < CXADC_AUDIO_CHANNEL_COUNT; ch++) {
                            uint8_t *dst = dst_frame + ((size_t)ch * 3);
                            if (ch < dev_channels) {
                                const uint8_t *src = src_frame + ((size_t)ch * ctx->audio_sample_bytes);
                                if (ctx->audio_format == CXADC_AUDIO_FMT_S24_3LE) {
                                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S32_LE) {
                                    int32_t s32 = (int32_t)((uint32_t)src[0] |
                                                            ((uint32_t)src[1] << 8) |
                                                            ((uint32_t)src[2] << 16) |
                                                            ((uint32_t)src[3] << 24));
                                    cxadc_store_s24le(dst, s32 >> 8);
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S16_LE) {
                                    int16_t s16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                                    cxadc_store_s24le(dst, ((int32_t)s16) << 8);
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_FLOAT32) {
                                    float f;
                                    memcpy(&f, src, 4);
                                    int32_t s32 = (int32_t)(f * 8388607.0f);
                                    cxadc_store_s24le(dst, s32);
                                } else {
                                    dst[0] = dst[1] = dst[2] = 0;
                                }
                            } else {
                                dst[0] = dst[1] = dst[2] = 0;
                            }
                        }
                        dst_frame[9] = 0;
                        dst_frame[10] = 0;
                        dst_frame[11] = 0;
                    }
                    bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
                    bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
                    atomic_store(&app->audio_sample_rate, ctx->audio_sample_rate_hz);
                    atomic_store(&app->last_callback_time_ms, get_time_ms());
                } else {
                    atomic_fetch_add(&app->rb_drop_count, 1);
                }
            }

            ctx->audio_capture->lpVtbl->ReleaseBuffer(ctx->audio_capture, frames_available);
            hr = ctx->audio_capture->lpVtbl->GetNextPacketSize(ctx->audio_capture, &packet_frames);
            if (FAILED(hr)) {
                packet_frames = 0;
                break;
            }
        }
    }
cxadc_wasapi_done:
    return 0;
#elif !LIBASOUND_ENABLED
    (void)app;
    return 0;
#else
    if (!ctx->audio_pcm || ctx->audio_format == CXADC_AUDIO_FMT_NONE || ctx->audio_sample_bytes == 0) {
        return 0;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);

    size_t input_frame_bytes = (size_t)CXADC_AUDIO_CHANNEL_COUNT * ctx->audio_sample_bytes;
    size_t input_buffer_bytes = (size_t)CXADC_AUDIO_READ_FRAMES * input_frame_bytes;
    uint8_t *input_buf = (uint8_t *)malloc(input_buffer_bytes);
    if (!input_buf) {
        gui_app_set_status(app, "CXADC: failed to allocate ALSA audio buffer");
        return -1;
    }

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        snd_pcm_sframes_t frames = snd_pcm_readi(ctx->audio_pcm, input_buf, CXADC_AUDIO_READ_FRAMES);
        if (frames == -EAGAIN || frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }
        if (frames == -EPIPE) {
            (void)snd_pcm_prepare(ctx->audio_pcm);
            continue;
        }
        if (frames < 0) {
            int rec = snd_pcm_recover(ctx->audio_pcm, (int)frames, 1);
            if (rec >= 0) {
                continue;
            }
            gui_app_set_status(app, "CXADC ALSA audio read error");
            break;
        }

        size_t output_bytes = (size_t)frames * CXADC_AUDIO_PACKED_FRAME_BYTES;
        uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes, NULL);
        if (!out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            const uint8_t *src_frame = input_buf + ((size_t)i * input_frame_bytes);
            uint8_t *dst_frame = out + ((size_t)i * CXADC_AUDIO_PACKED_FRAME_BYTES);
            for (int ch = 0; ch < CXADC_AUDIO_CHANNEL_COUNT; ch++) {
                const uint8_t *src = src_frame + ((size_t)ch * ctx->audio_sample_bytes);
                uint8_t *dst = dst_frame + ((size_t)ch * 3);
                if (ctx->audio_format == CXADC_AUDIO_FMT_S24_3LE) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S32_LE) {
                    int32_t sample32 = (int32_t)((uint32_t)src[0] |
                                                 ((uint32_t)src[1] << 8) |
                                                 ((uint32_t)src[2] << 16) |
                                                 ((uint32_t)src[3] << 24));
                    cxadc_store_s24le(dst, sample32 >> 8);
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S16_LE) {
                    int16_t sample16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                    cxadc_store_s24le(dst, ((int32_t)sample16) << 8);
                } else {
                    dst[0] = dst[1] = dst[2] = 0;
                }
            }
            dst_frame[9] = 0;
            dst_frame[10] = 0;
            dst_frame[11] = 0;
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
        atomic_store(&app->audio_sample_rate, ctx->audio_sample_rate_hz);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
    }

    free(input_buf);
    return 0;
#endif
}

static int cxadc_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;

    gui_app_t *app = ctx->app;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *card_buf_a = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    uint8_t *card_buf_b = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    if (!card_buf_a || !card_buf_b) {
        free(card_buf_a);
        free(card_buf_b);
        gui_app_set_status(app, "CXADC: failed to allocate capture buffers");
        return -1;
    }

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, CXADC_SAMPLE_RATE_HZ);

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
#if defined(_WIN32)
        int read_a = cxadc_read_card(ctx->card_handles[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#else
        int read_a = cxadc_read_card(ctx->card_fds[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#endif
        if (read_a < 0) {
            gui_app_set_status(app, "CXADC read error on card 0");
            break;
        }
        if (read_a == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        int output_samples = read_a;
        if (ctx->card_count > 1) {
#if defined(_WIN32)
            int read_b = cxadc_read_card(ctx->card_handles[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#else
            int read_b = cxadc_read_card(ctx->card_fds[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#endif
            if (read_b < 0) {
                gui_app_set_status(app, "CXADC read error on card 1");
                break;
            }
            if (read_b == 0) {
                thrd_sleep_ms(1);
                continue;
            }
            if (read_b < output_samples) {
                output_samples = read_b;
            }
        }

        if (output_samples <= 0) {
            thrd_sleep_ms(1);
            continue;
        }

        size_t output_bytes = (size_t)output_samples * sizeof(uint32_t);
        uint32_t *raw_out = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, output_bytes, NULL);
        if (!raw_out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        for (int i = 0; i < output_samples; i++) {
            int16_t sample_a = (int16_t)(((int)card_buf_a[i] - 128) << 4);
            int16_t sample_b = 0;
            if (ctx->card_count > 1) {
                sample_b = (int16_t)(((int)card_buf_b[i] - 128) << 4);
            }
            raw_out[i] = cxadc_encode_raw_sample(sample_a, sample_b);
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

        atomic_store(&app->last_callback_time_ms, get_time_ms());
        atomic_fetch_add(&app->frame_count, 1);
    }

    free(card_buf_a);
    free(card_buf_b);
    return 0;
}

int gui_cxadc_start(gui_app_t *app, int card_count)
{
    if (!app) return -1;
    if (atomic_load(&s_cxadc.running)) return 0;

    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    memset(&s_cxadc, 0, sizeof(s_cxadc));
    s_cxadc.app = app;
    s_cxadc.card_count = card_count;
#if defined(_WIN32)
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_handles[i] = INVALID_HANDLE_VALUE;
    }
    s_cxadc.audio_endpoint = NULL;
    s_cxadc.audio_client = NULL;
    s_cxadc.audio_capture = NULL;
    s_cxadc.audio_com_initialized = false;
#else
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_fds[i] = -1;
    }
#if LIBASOUND_ENABLED
    s_cxadc.audio_pcm = NULL;
#endif
#endif
    // Shared audio format fields (zeroed by memset, set explicitly for clarity)
    s_cxadc.audio_format = CXADC_AUDIO_FMT_NONE;
    s_cxadc.audio_sample_bytes = 0;
    s_cxadc.audio_sample_rate_hz = 0;
    s_cxadc.audio_channels = 0;
    s_cxadc.audio_device_name[0] = '\0';

    if (cxadc_open_cards(&s_cxadc, card_count) != 0) {
        gui_app_set_status(app, "CXADC: failed to open card device(s)");
        return -1;
    }

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        gui_app_set_status(app, "CXADC: failed to initialize RF buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        gui_app_set_status(app, "CXADC: failed to initialize audio buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    cxadc_reset_stats(app);

    bool audio_capture_available = false;
    if (cxadc_open_audio_capture(&s_cxadc) == 0) {
        audio_capture_available = true;
        if (s_cxadc.audio_sample_rate_hz > 0) {
            atomic_store(&app->audio_sample_rate, s_cxadc.audio_sample_rate_hz);
        }
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] audio capture device: %s (%u Hz, %d ch)\n",
                s_cxadc.audio_device_name,
                s_cxadc.audio_sample_rate_hz,
                s_cxadc.audio_channels);
#endif
    } else {
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] clockgen audio device not available; continuing RF-only\n");
#endif
    }

    app->is_capturing = true;
    int r = gui_extract_start(app);
    if (r < 0) {
        gui_app_set_status(app, "CXADC: failed to start extraction thread");
        app->is_capturing = false;
        cxadc_close_audio_capture(&s_cxadc);
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[CXADC] Failed to start display thread (non-fatal)\n");
        }
    }

    (void)gui_audio_start(app, &app->buffers);

    atomic_store(&s_cxadc.running, true);
    if (thrd_create_with_priority(&s_cxadc.rf_thread,
                                  cxadc_capture_thread,
                                  &s_cxadc,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        gui_app_set_status(app, "CXADC: failed to start RF capture thread");
        atomic_store(&s_cxadc.running, false);
        gui_audio_stop(app);
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        gui_extract_stop();
        app->is_capturing = false;
        cxadc_close_audio_capture(&s_cxadc);
        cxadc_close_cards(&s_cxadc);
        return -1;
    }
    s_cxadc.rf_thread_started = true;

    if (audio_capture_available) {
        if (thrd_create_with_priority(&s_cxadc.audio_thread,
                                      cxadc_audio_capture_thread,
                                      &s_cxadc,
                                      THRD_PRIORITY_ABOVE) != thrd_success) {
            fprintf(stderr, "[CXADC] Failed to start audio capture thread; continuing RF-only\n");
            cxadc_close_audio_capture(&s_cxadc);
        } else {
            s_cxadc.audio_thread_started = true;
        }
    }

    gui_app_set_status(app, (card_count > 1) ? "CXADC Clockgen capture running" : "CXADC capture running");
    return 0;
}

void gui_cxadc_stop(gui_app_t *app)
{
    if (!atomic_load(&s_cxadc.running) && !s_cxadc.rf_thread_started && !s_cxadc.audio_thread_started) {
        return;
    }

    if (!app) {
        app = s_cxadc.app;
    }

    if (app) {
        app->is_capturing = false;
    }
    atomic_store(&s_cxadc.running, false);
    cxadc_abort_audio_capture(&s_cxadc);

    if (s_cxadc.rf_thread_started) {
        thrd_join(s_cxadc.rf_thread, NULL);
        s_cxadc.rf_thread_started = false;
    }
    if (s_cxadc.audio_thread_started) {
        thrd_join(s_cxadc.audio_thread, NULL);
        s_cxadc.audio_thread_started = false;
    }

    if (app && app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }
    gui_audio_stop(app);
    gui_extract_stop();
    cxadc_close_audio_capture(&s_cxadc);

    cxadc_close_cards(&s_cxadc);

    if (app) {
        atomic_store(&app->stream_synced, false);
        atomic_store(&app->dropout_stop_requested, false);
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
        gui_app_set_status(app, "CXADC capture stopped");
    }

    memset(&s_cxadc, 0, sizeof(s_cxadc));
}

bool gui_cxadc_is_running(void)
{
    return atomic_load(&s_cxadc.running);
}
