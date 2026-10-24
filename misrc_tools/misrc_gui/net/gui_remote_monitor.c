#include "gui_remote_monitor.h"
#include "gui_remote_display.h"
#include "../../common/threading.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

_Static_assert(sizeof(gui_rm_packet_t) == GUI_RM_PACKET_BYTES, "remote monitor protocol layout changed");
_Static_assert(sizeof(gui_rm_scope_packet_t) == GUI_RM_SCOPE_PACKET_BYTES, "remote scope layout changed");

/* A single latest-frame mailbox: no queue, malloc, back-pressure or waiting in
 * the recording path. Two independent mailboxes (server and client). */
static gui_rm_packet_t s_server, s_client;
static gui_rm_scope_packet_t s_scope_server, s_scope_client;
static atomic_flag s_scope_server_lock = ATOMIC_FLAG_INIT;
static atomic_flag s_scope_client_lock = ATOMIC_FLAG_INIT;
static atomic_bool s_scope_server_valid = ATOMIC_VAR_INIT(false);
static atomic_bool s_scope_client_valid = ATOMIC_VAR_INIT(false);
static atomic_flag s_server_lock = ATOMIC_FLAG_INIT;
static atomic_flag s_client_lock = ATOMIC_FLAG_INIT;
static atomic_bool s_server_valid = ATOMIC_VAR_INIT(false);
static atomic_bool s_client_valid = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t s_last_capture_ms = ATOMIC_VAR_INIT(0);
static atomic_uint s_sequence = ATOMIC_VAR_INIT(0);

static void put_u16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
}
static void put_u32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
uint16_t gui_rm_get_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
int16_t gui_rm_get_s16(const uint8_t *p) {
    uint16_t n = gui_rm_get_u16(p);
    return (int16_t)(n < 0x8000u ? (int32_t)n : (int32_t)n - 65536);
}
float gui_rm_waveform_range(uint16_t peak) {
    /* Leave headroom while keeping weak ADC signals visible. All quiet
     * channels show a flat line instead of amplifying a single ADC LSB. */
    float r = 1.15f * (float)peak;
    return (r < 64.0f) ? 64.0f : r;
}
uint32_t gui_rm_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* Compute the same real 4096-point Hann FFT as Harry's local gui_fft:
 * signed ADC / 2048, DC removal, Hann window, 2/N coherent-gain
 * correction, -120..0 dB mapped linearly. Radix-2 float FFT has minor
 * rounding differences from FFTW but identical binning/normalization.
 * Done on the server only (<=10 Hz); no RF samples transmitted for FFT. */
/* FFT window is fixed at 4096 points. Construct it once, not at
 * every <=10 Hz monitor update. Atomic once-state is Windows/MSYS2 C11 safe.
 * 0=uninitialized, 1=building, 2=ready. Only monitor work ever waits here. */
static float s_fft_hann[GUI_RM_FFT_SIZE];
static atomic_int s_fft_hann_state = ATOMIC_VAR_INIT(0);
static const float *gui_rm_hann_window(void) {
    if (atomic_load_explicit(&s_fft_hann_state, memory_order_acquire) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&s_fft_hann_state, &expected, 1,
                memory_order_acq_rel, memory_order_acquire)) {
            const float pi = 3.14159265358979323846f;
            for (int i = 0; i < GUI_RM_FFT_SIZE; ++i) {
                s_fft_hann[i] = 0.5f * (1.0f - cosf(2.0f * pi * (float)i / (float)(GUI_RM_FFT_SIZE-1)));
            }
            atomic_store_explicit(&s_fft_hann_state, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&s_fft_hann_state, memory_order_acquire) != 2) { }
        }
    }
    return s_fft_hann;
}

static void spectrum(const int16_t *samples, size_t count,
                     uint8_t out[GUI_RM_FFT_BINS][2]) {
    if (!samples || count < GUI_RM_FFT_SIZE) {
        memset(out, 0, GUI_RM_FFT_BINS * 2);
        return;
    }
    float re[GUI_RM_FFT_SIZE], im[GUI_RM_FFT_SIZE];
    double mean = 0.0;
    for (int i=0; i<GUI_RM_FFT_SIZE; ++i) mean += samples[i];
    mean /= GUI_RM_FFT_SIZE;
    const float pi = 3.14159265358979323846f;
    const float *hann = gui_rm_hann_window();
    for (int i=0; i<GUI_RM_FFT_SIZE; ++i) {
        re[i] = ((float)samples[i] - (float)mean) * (1.0f/2048.0f) * hann[i];
        im[i] = 0.0f;
    }
    for (int i=1,j=0; i<GUI_RM_FFT_SIZE; ++i) {
        int bit = GUI_RM_FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i<j) {
            float t=re[i]; re[i]=re[j]; re[j]=t;
            t=im[i]; im[i]=im[j]; im[j]=t;
        }
    }
    for (int size=2; size<=GUI_RM_FFT_SIZE; size <<= 1) {
        float angle=-2.0f*pi/(float)size;
        float step_re=cosf(angle), step_im=sinf(angle);
        for (int start=0; start<GUI_RM_FFT_SIZE; start+=size) {
            float wr=1.0f, wi=0.0f;
            for (int j=0; j<size/2; ++j) {
                int a=start+j, b=a+size/2;
                float tr=wr*re[b]-wi*im[b], ti=wr*im[b]+wi*re[b];
                re[b]=re[a]-tr; im[b]=im[a]-ti;
                re[a]+=tr; im[a]+=ti;
                float nr=wr*step_re-wi*step_im;
                wi=wr*step_im+wi*step_re; wr=nr;
            }
        }
    }
    for (int k=0; k<GUI_RM_FFT_BINS; ++k) {
        float mag=sqrtf(re[k]*re[k]+im[k]*im[k]);
        mag=(mag / (float)GUI_RM_FFT_SIZE) * 2.0f;
        float db=20.0f*log10f(mag+1.0e-10f);
        if (db < -120.0f) db=-120.0f;
        if (db > 0.0f) db=0.0f;
        uint16_t val=(uint16_t)(((db+120.0f)/120.0f)*65535.0f+0.5f);
        put_u16(out[k],val);
    }
}

static void channel_build(gui_rm_channel_t *ch, const int16_t *buf, size_t count) {
    memset(ch, 0, sizeof(*ch));
    if (!buf || count < GUI_RM_FFT_SIZE) return;
    uint32_t peak=0;
    uint16_t peak_pos=0, peak_neg=0;
    double squares=0.0;
    for (int x=0; x<GUI_RM_POINTS; x++) {
        size_t first = (size_t)x * count / GUI_RM_POINTS;
        size_t last = (size_t)(x+1) * count / GUI_RM_POINTS;
        int16_t lo=32767, hi=-32768;
        for (size_t i=first; i<last; i++) {
            int16_t v = buf[i];
            if (i < 1000) {
                if (v > 0 && (uint16_t)v > peak_pos) peak_pos=(uint16_t)v;
                if (v < 0 && (uint16_t)-(int32_t)v > peak_neg) peak_neg=(uint16_t)-(int32_t)v;
            }
            if (v < lo) lo=v;
            if (v > hi) hi=v;
            uint32_t abs_v = (v == INT16_MIN) ? 32768u : (uint32_t)(v < 0 ? -v : v);
            if (abs_v > peak) peak=abs_v;
            squares += (double)v*(double)v;
        }
        put_u16(ch->scope_min_le[x], (uint16_t)lo);
        put_u16(ch->scope_max_le[x], (uint16_t)hi);
    }
    put_u16(ch->peak_le, (uint16_t)(peak > 32767u ? 32767u : peak));
    put_u16(ch->peak_pos_le, peak_pos);
    put_u16(ch->peak_neg_le, peak_neg);
    double rms = sqrt(squares / (double)count);
    put_u16(ch->rms_le, (uint16_t)(rms > 32767.0 ? 32767.0 : rms));
    spectrum(buf, count, ch->spectrum_le);
}

static void scope_channel_fill(gui_rm_scope_channel_t *ch,
                               const int16_t *samples, size_t count) {
    if (!samples) return;
    for (size_t i = 0; i < GUI_RM_SCOPE_SAMPLES; ++i) {
        put_u16(ch->native_le[i], (uint16_t)samples[i]);
        /* Wide preview previously box-averaged the source, cancelling
         * chroma and other high-frequency ADC excursions before the local
         * scope's own resampler. Use a real point sample instead; this
         * preserves ADC amplitude (at the expense of aliasing at wide zoom).
         * Exact local reconstruction still requires zoom-aware server-side
         * resampling in a later phase. */
        size_t first = i * count / GUI_RM_SCOPE_SAMPLES;
        size_t last = (i+1) * count / GUI_RM_SCOPE_SAMPLES;
        size_t sample_index = first + (last-first)/2;
        int16_t value = samples[sample_index];
        put_u16(ch->wide_le[i], (uint16_t)value);
    }
}

void gui_rm_submit(const int16_t *a, const int16_t *b, size_t count, uint32_t sample_rate) {
    if (!a || count < GUI_RM_FFT_SIZE) return;
    uint64_t now = get_time_ms();
    uint64_t previous = atomic_load_explicit(&s_last_capture_ms, memory_order_relaxed);
    if (now >= previous && now - previous < 100) return;
    if (!atomic_compare_exchange_strong(&s_last_capture_ms, &previous, now)) return;
    gui_rm_packet_t packet = {0};
    memcpy(packet.magic, "MRM1", 4);
    packet.version = GUI_RM_VERSION;
    packet.channels = b ? 2 : 1;
    put_u32(packet.sample_rate_le, sample_rate);
    put_u32(packet.sequence_le, atomic_fetch_add(&s_sequence, 1) + 1);
    gui_rd_capture(a,b,count,sample_rate,gui_rm_get_u32(packet.sequence_le));
    channel_build(&packet.ch[0], a, count);
    channel_build(&packet.ch[1], b, count);
    /* Trylock; never stall extraction for readers. */
    if (atomic_flag_test_and_set_explicit(&s_server_lock, memory_order_acquire)) return;
    memcpy(&s_server, &packet, sizeof(packet));
    atomic_store_explicit(&s_server_valid, true, memory_order_release);
    atomic_flag_clear_explicit(&s_server_lock, memory_order_release);

    /* A second latest-only mailbox supplies the original oscilloscope with
     * band-limited payload size. Never wait for an HTTP reader. */
    if (count >= GUI_RM_SCOPE_SAMPLES && count <= UINT32_MAX) {
        gui_rm_scope_packet_t scope = {0};
        memcpy(scope.magic, "MRS1", 4);
        scope.version = GUI_RM_SCOPE_VERSION;
        scope.channels = b ? 2 : 1;
        put_u32(scope.sample_rate_le, sample_rate);
        put_u32(scope.sequence_le, gui_rm_get_u32(packet.sequence_le));
        put_u32(scope.source_count_le, (uint32_t)count);
        scope_channel_fill(&scope.ch[0], a, count);
        scope_channel_fill(&scope.ch[1], b, count);
        if (!atomic_flag_test_and_set_explicit(&s_scope_server_lock, memory_order_acquire)) {
            memcpy(&s_scope_server, &scope, sizeof(scope));
            atomic_store_explicit(&s_scope_server_valid, true, memory_order_release);
            atomic_flag_clear_explicit(&s_scope_server_lock, memory_order_release);
        }
    }
}
bool gui_rm_server_snapshot(gui_rm_packet_t *out) {
    if (!out || !atomic_load(&s_server_valid)) return false;
    uint64_t last = atomic_load(&s_last_capture_ms), now = get_time_ms();
    if (now < last || now - last > 500) return false;
    while (atomic_flag_test_and_set_explicit(&s_server_lock, memory_order_acquire)) { /* HTTP only */ }
    memcpy(out, &s_server, sizeof(*out));
    atomic_flag_clear_explicit(&s_server_lock, memory_order_release);
    return true;
}
bool gui_rm_client_accept(const void *data, size_t bytes) {
    if (!data || bytes != GUI_RM_PACKET_BYTES) return false;
    const gui_rm_packet_t *p = (const gui_rm_packet_t *)data;
    if (memcmp(p->magic, "MRM1", 4) || p->version != GUI_RM_VERSION || p->channels < 1 || p->channels > 2) return false;
    while (atomic_flag_test_and_set_explicit(&s_client_lock, memory_order_acquire)) { /* brief */ }
    memcpy(&s_client, p, sizeof(*p));
    atomic_store_explicit(&s_client_valid, true, memory_order_release);
    atomic_flag_clear_explicit(&s_client_lock, memory_order_release);
    return true;
}
bool gui_rm_client_snapshot(gui_rm_packet_t *out) {
    if (!out || !atomic_load(&s_client_valid)) return false;
    while (atomic_flag_test_and_set_explicit(&s_client_lock, memory_order_acquire)) { /* brief */ }
    memcpy(out, &s_client, sizeof(*out));
    atomic_flag_clear_explicit(&s_client_lock, memory_order_release);
    return true;
}
void gui_rm_client_clear(void) {
    /* Clear between connections; don't show the last server's data. */
    atomic_store_explicit(&s_client_valid, false, memory_order_release);
}

/* The scope mailbox has its own lock; monitor FFT polling cannot stall it. */
bool gui_rm_scope_server_snapshot(gui_rm_scope_packet_t *out) {
    if (!out || !atomic_load(&s_scope_server_valid)) return false;
    uint64_t last = atomic_load(&s_last_capture_ms), now = get_time_ms();
    if (now < last || now - last > 500) return false;
    while (atomic_flag_test_and_set_explicit(&s_scope_server_lock, memory_order_acquire)) { }
    memcpy(out, &s_scope_server, sizeof(*out));
    atomic_flag_clear_explicit(&s_scope_server_lock, memory_order_release);
    return true;
}
bool gui_rm_scope_client_accept(const void *data, size_t bytes) {
    if (!data || bytes != GUI_RM_SCOPE_PACKET_BYTES) return false;
    const gui_rm_scope_packet_t *p = (const gui_rm_scope_packet_t *)data;
    uint32_t n = gui_rm_get_u32(p->source_count_le);
    if (memcmp(p->magic, "MRS1", 4) || p->version != GUI_RM_SCOPE_VERSION ||
        p->channels < 1 || p->channels > 2 ||
        n < GUI_RM_SCOPE_SAMPLES || n > 262144 ||
        gui_rm_get_u32(p->sample_rate_le) == 0) return false;
    while (atomic_flag_test_and_set_explicit(&s_scope_client_lock, memory_order_acquire)) { }
    memcpy(&s_scope_client, p, sizeof(*p));
    atomic_store_explicit(&s_scope_client_valid, true, memory_order_release);
    atomic_flag_clear_explicit(&s_scope_client_lock, memory_order_release);
    return true;
}
bool gui_rm_scope_client_snapshot(gui_rm_scope_packet_t *out) {
    if (!out || !atomic_load_explicit(&s_scope_client_valid, memory_order_acquire)) return false;
    while (atomic_flag_test_and_set_explicit(&s_scope_client_lock, memory_order_acquire)) { }
    memcpy(out, &s_scope_client, sizeof(*out));
    atomic_flag_clear_explicit(&s_scope_client_lock, memory_order_release);
    return true;
}
void gui_rm_scope_client_clear(void) {
    atomic_store_explicit(&s_scope_client_valid, false, memory_order_release);
}
