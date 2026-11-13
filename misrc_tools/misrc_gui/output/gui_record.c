#include "../input/gui_cxadc_macos.h"
/*
 * MISRC - hsdaoh-rp2350 GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#include "gui_record.h"
#include "../net/monitor_local_save_proxy.h"
#include "../net/gui_monitor_local_save.h"
#include "../net/gui_net.h" /* GUI_NET_MODE_MONITOR_SERVER */
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../ui/gui_popup.h"
#include "gui_audio.h"
#include "../input/gui_capture.h"
#include "../input/gui_cxadc.h"
#ifdef ENABLE_RTLSDR
#include "../input/gui_rtlsdr.h"
#include "../signal/gui_vhs_hifi.h"
#endif
#include "../processing/gui_sdr_rf.h"
#include "gui_record_direct.h"

#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/buffer_manager.h"
#include "../../common/flac_writer.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"
#include "../../common/file_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include "version.h"

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#include <io.h>
#define access _access
#define F_OK 0
#endif

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#endif

#if LIBFLAC_ENABLED == 1
#include "FLAC/metadata.h"
#endif

/* All output FILE* are either genuine stdio streams or M-Client proxies;
 * only Local Save causes the latter to be used. */
#define fwrite mls_proxy_write
#define fclose mls_proxy_close
#define fflush mls_proxy_flush
#define ferror mls_proxy_error
#define clearerr mls_proxy_clearerr
#define file_open_utf8 mls_proxy_open

/* Headless mode has no raylib window to advance GetTime(). Use the
 * monotonic clock for either recording protocol on the unified headless server. */
static double gui_record_clock(const gui_app_t *app) {
    if (app && app->headless_server)
        return (double)get_time_us() / 1000000.0;
    return GetTime();
}

// Buffer sizes
#define BUFFER_READ_SIZE 65536
#define GUI_RECORD_PATH_MAX (FILE_PATH_MAX + MAX_FILENAME_LEN)
// Use larger CLI-style writer chunks for record-path throughput stability.
// CLI writer path uses 65536*32 byte blocks; GUI records int16 samples, so this
// maps to 65536*32 bytes == 1048576 samples per channel per writer read.
#define GUI_RECORD_WRITER_BLOCK_BYTES ((size_t)65536 * 32)
#define GUI_RECORD_WRITER_BLOCK_SAMPLES (GUI_RECORD_WRITER_BLOCK_BYTES / sizeof(int16_t))
// stdio buffer for capture output files. libFLAC emits one small fwrite() per
// frame (often <1 KB); with the default 4 KB CRT buffer each flush becomes a
// separate write, which is round-trip bound on SMB/NFS targets.
#define GUI_RECORD_OUTPUT_STDIO_BUFFER_BYTES ((size_t)8 * 1024 * 1024)

static void gui_record_set_output_buffer(FILE *fp) {
    if (fp) {
        (void)mls_proxy_setvbuf(fp, NULL, _IOFBF, GUI_RECORD_OUTPUT_STDIO_BUFFER_BYTES);
    }
}

// Format file size into human-readable string
static void format_file_size_u64(uint64_t size, char *buf, size_t buf_size) {
    if (size >= 1073741824ULL) {  // >= 1 GB
        snprintf(buf, buf_size, "%.2f GB", (double)size / 1073741824.0);
    } else if (size >= 1048576ULL) {  // >= 1 MB
        snprintf(buf, buf_size, "%.2f MB", (double)size / 1048576.0);
    } else if (size >= 1024ULL) {  // >= 1 KB
        snprintf(buf, buf_size, "%.2f KB", (double)size / 1024.0);
    } else {
        snprintf(buf, buf_size, "%llu bytes", (unsigned long long)size);
    }
}

static void format_duration_hhmmss(double seconds, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    if (seconds < 0.0) seconds = 0.0;
    uint64_t total_seconds = (uint64_t)seconds;
    uint64_t hh = total_seconds / 3600ULL;
    uint64_t mm = (total_seconds / 60ULL) % 60ULL;
    uint64_t ss = total_seconds % 60ULL;
    snprintf(dst, dst_len, "%02" PRIu64 ".%02" PRIu64 ".%02" PRIu64, hh, mm, ss);
}

// Format data sizes for capture logs using clear MB/GB units
static void format_log_data_size_u64(uint64_t size, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;

    double mb = (double)size / 1048576.0;
    double gb = (double)size / 1073741824.0;

    if (gb >= 1.0) {
        snprintf(buf, buf_size, "%.3f GB (%" PRIu64 " bytes)", gb, size);
    } else {
        snprintf(buf, buf_size, "%.2f MB (%" PRIu64 " bytes)", mb, size);
    }
}

static void gui_record_build_system_timestamp(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    time_t t = time(NULL);
    if (t == (time_t)-1) return;
    struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
    if (localtime_s(&tmv, &t) != 0) return;
#else
    if (!localtime_r(&t, &tmv)) return;
#endif
    snprintf(dst, dst_len, "%04d.%02d.%02d_%02d.%02d.%02d",
             (tmv.tm_year + 1900),
             tmv.tm_mon + 1,
             tmv.tm_mday,
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec);
}

static bool gui_record_extract_timestamp_token(const char *path, char *dst, size_t dst_len) {
    if (!path || !path[0] || !dst || dst_len == 0) return false;
    dst[0] = '\0';

    const char *name = path;
    const char *slash = strrchr(path, '/');
#if defined(_WIN32) || defined(_WIN64)
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
#endif
    if (slash && slash[1]) {
        name = slash + 1;
    }

    size_t len = strlen(name);
    for (size_t i = 0; i + 19 <= len; i++) {
        const char *p = name + i;
        bool match =
            isdigit((unsigned char)p[0]) &&
            isdigit((unsigned char)p[1]) &&
            isdigit((unsigned char)p[2]) &&
            isdigit((unsigned char)p[3]) &&
            p[4] == '.' &&
            isdigit((unsigned char)p[5]) &&
            isdigit((unsigned char)p[6]) &&
            p[7] == '.' &&
            isdigit((unsigned char)p[8]) &&
            isdigit((unsigned char)p[9]) &&
            p[10] == '_' &&
            isdigit((unsigned char)p[11]) &&
            isdigit((unsigned char)p[12]) &&
            p[13] == '.' &&
            isdigit((unsigned char)p[14]) &&
            isdigit((unsigned char)p[15]) &&
            p[16] == '.' &&
            isdigit((unsigned char)p[17]) &&
            isdigit((unsigned char)p[18]);
        if (match) {
            snprintf(dst, dst_len, "%.*s", 19, p);
            return true;
        }
    }

    return false;
}

static void gui_record_build_iso8601_timestamp(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    time_t t = time(NULL);
    if (t == (time_t)-1) return;
    struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
    if (localtime_s(&tmv, &t) != 0) return;
#else
    if (!localtime_r(&t, &tmv)) return;
#endif
    snprintf(dst, dst_len, "%04d-%02d-%02dT%02d:%02d:%02d",
             (tmv.tm_year + 1900),
             tmv.tm_mon + 1,
             tmv.tm_mday,
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec);
}

static void gui_record_get_host_name(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    const char *host = getenv("COMPUTERNAME");
    if (host && host[0]) {
        snprintf(dst, dst_len, "%s", host);
    }
#else
    if (gethostname(dst, dst_len - 1) == 0) {
        dst[dst_len - 1] = '\0';
    }
#endif
    if (!dst[0]) {
        snprintf(dst, dst_len, "%s", "unknown");
    }
}

static void gui_record_get_user_name(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    const char *user = getenv("USERNAME");
#else
    const char *user = getenv("USER");
    if (!user || !user[0]) {
        user = getenv("LOGNAME");
    }
#endif
    snprintf(dst, dst_len, "%s", (user && user[0]) ? user : "unknown");
}

static void gui_record_get_os_string(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    snprintf(dst, dst_len, "%s", "Windows");
#else
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(dst, dst_len, "%s %s (%s)", uts.sysname, uts.release, uts.machine);
    } else {
        snprintf(dst, dst_len, "%s", "unknown");
    }
#endif
}
static bool gui_record_get_free_space_bytes(const char *path, uint64_t *free_bytes_out);

bool gui_record_get_output_free_space_bytes(const gui_app_t *app, uint64_t *free_bytes_out) {
    if (!app || !free_bytes_out || (!app->settings.output_path[0] && !gui_monitor_local_save_recording())) {
        return false;
    }
    return gui_record_get_free_space_bytes(app->settings.output_path, free_bytes_out);
}


static uint32_t gui_record_get_cpu_core_count(void) {
#if defined(_WIN32) || defined(_WIN64)
    const char *cores_env = getenv("NUMBER_OF_PROCESSORS");
    if (cores_env && cores_env[0]) {
        int cores = atoi(cores_env);
        if (cores > 0) return (uint32_t)cores;
    }
    return 0;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    return (cores > 0) ? (uint32_t)cores : 0;
#endif
}

// Resolve the per-channel FLAC encoder thread count.
//
// user_threads == 0  -> auto: pick a real parallel count with a 4-core target
//   base so a single level-8 encoder stream can keep up with 40 MSPS on modern
//   desktop CPUs. We use half the online cores (leaving headroom for capture /
//   extract / display / audio / GUI), clamped to [4, 8]. On an 18-core box this
//   resolves to 8 threads per channel; on an 8-core box it resolves to 4.
// user_threads > 0   -> honor the user's explicit override verbatim.
//
// Returns 0 only when the core count cannot be determined, in which case the
// caller falls back to libFLAC's own default.
static uint32_t gui_record_resolve_flac_threads(int32_t user_threads) {
    if (user_threads > 0) {
        return (uint32_t)user_threads;
    }
    uint32_t cores = gui_record_get_cpu_core_count();
    if (cores == 0) {
        return 0;  // unknown -> let libFLAC pick its default
    }
    uint32_t half = cores / 2;
    if (half < 4) half = 4;    // 4-core target base
    if (half > 8) half = 8;    // cap: diminishing returns + leave headroom
    return half;
}

static void gui_record_get_cpu_model(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(__linux__)
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        snprintf(dst, dst_len, "%s", "unknown");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (strncmp(line, "model name", 10) == 0) {
            char *sep = strchr(line, ':');
            if (sep) {
                sep++;
                while (*sep == ' ' || *sep == '\t') sep++;
                size_t len = strlen(sep);
                while (len > 0 && (sep[len - 1] == '\n' || sep[len - 1] == '\r')) {
                    sep[--len] = '\0';
                }
                snprintf(dst, dst_len, "%s", sep);
                break;
            }
        }
    }
    fclose(cpuinfo);
#endif
    if (!dst[0]) {
        snprintf(dst, dst_len, "%s", "unknown");
    }
}

static const char *gui_record_device_type_name(const gui_app_t *app) {
    if (!app || app->selected_device < 0 || app->selected_device >= app->device_count) {
        return "unknown";
    }
    device_type_t type = app->devices[app->selected_device].type;
    // MISRC mode is an hsdaoh/MS2130 capture mode (the user_capture_mode_misrc
    // toggle). When it is enabled, report the operational mode ("misrc") as the
    // device type so the capture log reflects how the device is being driven,
    // not just the backend driver name. Matches the misrc_mode field already
    // logged on the Audio monitor line (gui_record.c:2040-2043).
    if (type == DEVICE_TYPE_HSDAOH && app->user_capture_mode_misrc) {
        return "misrc";
    }
    switch (type) {
        case DEVICE_TYPE_HSDAOH:
            return "hsdaoh";
        case DEVICE_TYPE_SIMPLE_CAPTURE:
            return "simple_capture";
        case DEVICE_TYPE_CXADC:
            return "cxadc";
        case DEVICE_TYPE_SIMULATED:
            return "simulated";
        case DEVICE_TYPE_PLAYBACK:
            return "playback";
#ifdef ENABLE_FX3
        case DEVICE_TYPE_FX3:
            return "fx3adc";
#endif
#ifdef ENABLE_DDD
        case DEVICE_TYPE_DDD:
            return "ddd";
#endif
        default:
            return "unknown";
    }
}

static void gui_record_build_log_timestamp(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    time_t t = time(NULL);
    if (t == (time_t)-1) return;
    struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
    if (localtime_s(&tmv, &t) != 0) return;
#else
    if (!localtime_r(&t, &tmv)) return;
#endif
    snprintf(dst, dst_len, "%04d-%02d-%02d %02d:%02d:%02d",
             (tmv.tm_year + 1900),
             tmv.tm_mon + 1,
             tmv.tm_mday,
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec);
}

// do_exit is declared in gui_capture.h (defined in misrc_gui.c)

// Writer threads
static thrd_t s_writer_thread_a;
static thrd_t s_writer_thread_b;
static thrd_t s_finalize_thread;
static atomic_bool s_finalize_thread_running = ATOMIC_VAR_INIT(false);
static bool s_writer_started[2];
static atomic_bool s_record_writer_draining = true;
static atomic_bool s_record_stop_finalizing = ATOMIC_VAR_INIT(false);
static atomic_bool s_record_stop_finalize_done = ATOMIC_VAR_INIT(false);
static atomic_bool s_record_stop_failed;
static atomic_bool s_record_stop_commit_unconfirmed;
static atomic_bool s_record_producer_lost_data;
static atomic_bool s_rtlsdr_hifi_accepting;
static atomic_bool s_rtlsdr_hifi_interrupted;
static atomic_uint s_rtlsdr_hifi_inflight;
static uint64_t s_rtlsdr_hifi_tuning;
static uint32_t s_rtlsdr_hifi_source_rate;
enum {
    GUI_RECORD_HIFI_OUTPUT_NONE,
    GUI_RECORD_HIFI_OUTPUT_RAW,
    GUI_RECORD_HIFI_OUTPUT_FLAC
};
static atomic_uint s_rtlsdr_hifi_output;
static char s_rtlsdr_hifi_filename[MAX_FILENAME_LEN];
static atomic_uint_fast64_t s_capture_source_loss_count;
static uint_fast64_t s_capture_source_loss_start;
static atomic_uint_fast64_t s_capture_audio_loss_count;
static uint_fast64_t s_capture_audio_loss_start;
static uint_fast64_t s_capture_rf_drop_start;
static uint_fast64_t s_client_audio_loss_start;
static bool s_record_uses_capture_rf;
static bool s_record_uses_client_audio;
static bool s_record_uses_capture_audio;
static FILE *s_file_a = NULL;
static FILE *s_file_b = NULL;
static char s_record_path_a[GUI_RECORD_PATH_MAX];
static char s_record_path_b[GUI_RECORD_PATH_MAX];
#if LIBFLAC_ENABLED == 1
static uint32_t s_record_sample_rate_a = 0;
static uint32_t s_record_sample_rate_b = 0;
#endif
static FILE *s_capture_log_file = NULL;
static char s_capture_log_path[GUI_RECORD_PATH_MAX];
static atomic_flag s_capture_log_lock = ATOMIC_FLAG_INIT;

static void gui_record_log_lock(void) {
    while (atomic_flag_test_and_set(&s_capture_log_lock)) {
        thrd_sleep_ms(1);
    }
}

static void gui_record_log_unlock(void) {
    atomic_flag_clear(&s_capture_log_lock);
}

static void gui_record_close_session_log(void) {
    gui_record_log_lock();
    if (s_capture_log_file) {
        fclose(s_capture_log_file);
        s_capture_log_file = NULL;
    }
    s_capture_log_path[0] = '\0';
    gui_record_log_unlock();
}

static void gui_record_trim_trailing_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static void gui_record_log_write_line_locked(const char *level, const char *message) {
    if (!s_capture_log_file || !message || !message[0]) return;
    char ts[32];
    gui_record_build_log_timestamp(ts, sizeof(ts));
    mls_proxy_printf(s_capture_log_file, "[%s] [%s] %s\n", ts, (level && level[0]) ? level : "INFO", message);
    fflush(s_capture_log_file);
}

static void gui_record_log_write_line(const char *level, const char *message) {
    gui_record_log_lock();
    gui_record_log_write_line_locked(level, message);
    gui_record_log_unlock();
}

static void gui_record_log_writef(const char *level, const char *format, ...) {
    if (!format || !format[0]) return;
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    gui_record_trim_trailing_newline(message);
    if (!message[0]) return;
    gui_record_log_write_line(level, message);
}

// Global app pointer for threads
static gui_app_t *s_recording_app = NULL;

enum { GUI_RECORD_RF_OUTPUT_COUNT = 2,
       GUI_RECORD_OUTPUT_COUNT = GUI_RECORD_RF_OUTPUT_COUNT + GUI_AUDIO_RECORD_OUTPUT_COUNT };
typedef struct {
    char paths[GUI_RECORD_OUTPUT_COUNT][GUI_RECORD_PATH_MAX];
    unsigned existing_mask;
    bool rtl_hifi;
    bool rtl_hifi_flac;
} gui_record_output_targets_t;

// Overwrite confirmation pending state
static gui_record_output_targets_t s_pending_output_targets;
static bool s_overwrite_pending = false;
static gui_app_t *s_pending_app = NULL;
static double s_pending_capture_start;
static int s_pending_network_mode;

typedef struct {
    gui_app_t *app;
    double stop_request_time;
} gui_record_finalize_ctx_t;

static bool gui_record_collect_finalize_if_done(void) {
    // The worker can finish before the caller publishes the thread handle.
    // Leave its completion pending until that handle is available to join.
    if (!atomic_load(&s_finalize_thread_running) ||
        !atomic_exchange(&s_record_stop_finalize_done, false)) return false;
    thrd_join(s_finalize_thread, NULL);
    atomic_store(&s_finalize_thread_running, false);
    atomic_store(&s_record_stop_finalizing, false);
    return true;
}

// Record-buffer backpressure stats at recording start (to compute per-session deltas)
static uint32_t s_start_rec_a_wait_count = 0;
static uint32_t s_start_rec_a_drop_count = 0;
static uint32_t s_start_rec_b_wait_count = 0;
static uint32_t s_start_rec_b_drop_count = 0;

#define GUI_RECORD_SPILL_CHANNELS 2
#define GUI_RECORD_SPILL_LOG_STEP_BYTES ((uint64_t)256 * 1024 * 1024)
#define GUI_RECORD_DISK_GUARD_CHECK_INTERVAL_MS 1000ULL
#define GUI_RECORD_DISK_GUARD_MIN_HEADROOM_BYTES ((uint64_t)2 * 1000 * 1000 * 1000)
#define GUI_RECORD_DISK_GUARD_LOOKAHEAD_SECONDS 180.0

static atomic_bool s_disk_guard_tripped = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t s_disk_guard_last_check_ms = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_last_free_bytes = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_last_output_bytes = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_output_rate_bps = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_last_required_bytes = ATOMIC_VAR_INIT(0);

typedef struct {
    FILE *fp;
    char path[GUI_RECORD_PATH_MAX];
    atomic_uint_fast64_t write_offset;
    atomic_uint_fast64_t read_offset;
    atomic_uint_fast64_t last_backlog_log_mark;
    atomic_bool forced_mode;
    atomic_bool opened;
    atomic_flag io_lock;
} gui_record_spill_channel_t;

static gui_record_spill_channel_t s_record_spill[GUI_RECORD_SPILL_CHANNELS];

// Per-channel output-file write error flags. Set by the writer threads when
// the FLAC encoder or RAW fwrite fails (e.g. file locked by another app for
// viewing). When set, the writer spills raw data to the temp file to protect
// the capture, and the UI flashes the finalizing icon at capture stop.
static atomic_bool s_record_write_error[GUI_RECORD_SPILL_CHANNELS] = {
    ATOMIC_VAR_INIT(false), ATOMIC_VAR_INIT(false)
};

static const char *gui_record_spill_channel_name(int channel) {
    return (channel == 0) ? "A" : "B";
}

static void gui_record_spill_lock(gui_record_spill_channel_t *spill) {
    while (atomic_flag_test_and_set(&spill->io_lock)) {
        thrd_sleep_ms(0);
    }
}

static void gui_record_spill_unlock(gui_record_spill_channel_t *spill) {
    atomic_flag_clear(&spill->io_lock);
}

static bool gui_record_spill_valid_channel(int channel) {
    return (channel >= 0 && channel < GUI_RECORD_SPILL_CHANNELS);
}

static void gui_record_reset_disk_guard_state(void) {
    atomic_store(&s_disk_guard_tripped, false);
    atomic_store(&s_disk_guard_last_check_ms, 0);
    atomic_store(&s_disk_guard_last_free_bytes, 0);
    atomic_store(&s_disk_guard_last_output_bytes, 0);
    atomic_store(&s_disk_guard_output_rate_bps, 0);
    atomic_store(&s_disk_guard_last_required_bytes, 0);
}

static bool gui_record_get_free_space_bytes(const char *path, uint64_t *free_bytes_out) {
    return file_disk_free_space_utf8(path, free_bytes_out);
}

#if defined(_WIN32) || defined(_WIN64)
#define GUI_RECORD_FSEEK(stream, offset, whence) mls_proxy_seek((stream),(offset),(whence))
#else
#define GUI_RECORD_FSEEK(stream, offset, whence) mls_proxy_seek((stream),(offset),(whence))
#endif

#if !defined(_WIN32) && !defined(_WIN64)
static bool gui_record_spill_open_channel_in_dir(const char *base_dir,
                                                 int channel,
                                                 FILE **out_fp,
                                                 char *out_path,
                                                 size_t out_path_size) {
    if (!base_dir || !base_dir[0] || !out_fp || !out_path || out_path_size == 0) {
        return false;
    }

    char template_path[GUI_RECORD_PATH_MAX];
    int written = snprintf(template_path, sizeof(template_path), "%s/.misrc_record_spill_%s_XXXXXX",
                           base_dir, gui_record_spill_channel_name(channel));
    if (written < 0 || (size_t)written >= sizeof(template_path) ||
        (size_t)written >= out_path_size) {
        errno = ENAMETOOLONG;
        return false;
    }

    int fd = mkstemp(template_path);
    if (fd < 0) {
        return false;
    }

    FILE *fp = fdopen(fd, "w+b");
    if (!fp) {
        close(fd);
        unlink(template_path);
        return false;
    }

    (void)setvbuf(fp, NULL, _IOFBF, 1024 * 1024);
    *out_fp = fp;
    snprintf(out_path, out_path_size, "%s", template_path);
    return true;
}
#endif

static bool gui_record_spill_open_channel(gui_app_t *app, int channel, char *error_msg, size_t error_msg_size) {
    if(gui_monitor_local_save_recording()){
        if(error_msg && error_msg_size){
            char diag[160]={0};gui_monitor_local_save_diagnostics(diag,sizeof(diag));
            snprintf(error_msg,error_msg_size,"Local Save backpressure (%s); server-side disk spill disabled",diag);
        }
        return false;
    }
    if (!gui_record_spill_valid_channel(channel)) {
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    if (atomic_load(&spill->opened)) {
        return true;
    }

    FILE *fp = NULL;
    char path_buf[GUI_RECORD_PATH_MAX] = {0};
    const char *output_dir = NULL;

#if defined(_WIN32) || defined(_WIN64)
    fp = tmpfile();
    if (fp) {
        snprintf(path_buf, sizeof(path_buf), "%s", "(tmpfile)");
    }
    if (fp) {
        (void)setvbuf(fp, NULL, _IOFBF, 1024 * 1024);
    }
#else
    output_dir = (app && app->settings.output_path[0]) ? app->settings.output_path : NULL;
    const char *tmp_dir = getenv("TMPDIR");
    if (!tmp_dir || !tmp_dir[0]) {
        tmp_dir = "/tmp";
    }
    int last_err = 0;
    // Keep spill files on the capture target filesystem so free-space
    // accounting tracks the real recording destination.
    if (output_dir && output_dir[0]) {
        if (!gui_record_spill_open_channel_in_dir(output_dir, channel, &fp, path_buf, sizeof(path_buf))) {
            last_err = errno;
        }
    } else if (!gui_record_spill_open_channel_in_dir(tmp_dir, channel, &fp, path_buf, sizeof(path_buf))) {
        last_err = errno;
    }
    if (!fp && last_err != 0) {
        errno = last_err;
    }
#endif

    if (!fp) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size,
                     "Failed to open spill temp file for channel %s in capture path %s: %s",
                     gui_record_spill_channel_name(channel),
                     (output_dir && output_dir[0]) ? output_dir : "(unset)",
                     strerror(errno));
        }
        return false;
    }

    spill->fp = fp;
    snprintf(spill->path, sizeof(spill->path), "%s", path_buf);
    atomic_store(&spill->write_offset, 0);
    atomic_store(&spill->read_offset, 0);
    atomic_store(&spill->last_backlog_log_mark, 0);
    atomic_store(&spill->opened, true);

    return true;
}

static void gui_record_spill_close_channel(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    FILE *fp = spill->fp;
    spill->fp = NULL;
    if (fp) {
        fclose(fp);
    }
#if !defined(_WIN32) && !defined(_WIN64)
    if (spill->path[0] != '\0' && strcmp(spill->path, "(tmpfile)") != 0) {
        unlink(spill->path);
    }
#endif
    spill->path[0] = '\0';
    atomic_store(&spill->write_offset, 0);
    atomic_store(&spill->read_offset, 0);
    atomic_store(&spill->last_backlog_log_mark, 0);
    atomic_store(&spill->forced_mode, false);
    atomic_store(&spill->opened, false);
}

static void gui_record_spill_reset_all(void) {
    for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
        atomic_flag_clear(&s_record_spill[i].io_lock);
        gui_record_spill_close_channel(i);
    }
}

static uint64_t gui_record_spill_backlog_bytes_locked(int channel) {
    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    uint64_t write_off = atomic_load(&spill->write_offset);
    uint64_t read_off = atomic_load(&spill->read_offset);
    return (write_off >= read_off) ? (write_off - read_off) : 0;
}

static uint64_t gui_record_queued_bytes(buffer_manager_t *buffers) {
    uint64_t total = 0;
    for (int channel = 0; channel < GUI_RECORD_SPILL_CHANNELS; channel++) {
        uint64_t spill = gui_record_spill_backlog_bytes_locked(channel);
        if (spill > UINT64_MAX - total) return UINT64_MAX;
        total += spill;
        buffer_id_t id = channel == 0 ? BUF_RECORD_A : BUF_RECORD_B;
        size_t ring = bufmgr_fill_level(buffers, id);
        if (ring > UINT64_MAX - total) return UINT64_MAX;
        total += ring;
    }
    return total;
}

static uint64_t gui_record_get_raw_total_bytes(const gui_app_t *app) {
    if (!app) return 0;
    return atomic_load(&app->recording_raw_a) + atomic_load(&app->recording_raw_b);
}

static uint64_t gui_record_get_encoded_total_bytes(const gui_app_t *app) {
    if (!app) return 0;
    return atomic_load(&app->recording_compressed_a) + atomic_load(&app->recording_compressed_b);
}

static uint64_t gui_record_get_effective_output_total_bytes(const gui_app_t *app,
                                                            uint64_t raw_total,
                                                            uint64_t encoded_total) {
    if (!app) return 0;
    bool flac = app->settings.use_flac;
    if (gui_record_rtlsdr_active()) gui_record_get_rtlsdr_hifi_output(&flac);
    if (flac) {
        return (encoded_total > 0) ? encoded_total : raw_total;
    }
    return raw_total;
}

// Live on-disk size of each channel's output file, via stat(). Used by the UI
// readout so the displayed RAW/FLAC size is exactly the real file size on disk
// (1:1 with `ls`/file-manager), not the writer-thread atomic which can lead
// the flushed file by the FILE* buffer size (the "readout over the file"
// report) and is also in 1024^2 units that don't match decimal file-manager MB.
// Returns false when not recording or neither path is set; individual channels
// are 0 when that channel's file isn't open / stat fails.
bool gui_record_get_live_output_bytes(uint64_t *out_a, uint64_t *out_b) {
    if (out_a) *out_a = 0;
    if (out_b) *out_b = 0;
    if (!s_recording_app || !s_recording_app->is_recording) return false;
    bool any = false;
    if (out_a && s_record_path_a[0]) {
        if (file_path_info_utf8(s_record_path_a, out_a, NULL)) any = true;
    }
    if (out_b && s_record_path_b[0]) {
        if (file_path_info_utf8(s_record_path_b, out_b, NULL)) any = true;
    }
    return any;
}

static uint64_t gui_record_estimate_output_bytes_from_raw_backlog(const gui_app_t *app,
                                                                  uint64_t backlog_raw_bytes,
                                                                  uint64_t raw_total,
                                                                  uint64_t encoded_total) {
    if (!app || backlog_raw_bytes == 0) return 0;
    bool hifi = gui_record_rtlsdr_active();
    bool flac = app->settings.use_flac;
    if (hifi) gui_record_get_rtlsdr_hifi_output(&flac);
    if (!hifi && !flac) return backlog_raw_bytes;

    // The merged queue holds uint8 I/Q pairs. Each output RF sample is int16.
    double expansion = hifi && s_rtlsdr_hifi_source_rate
        ? (double)GUI_SDR_RF_OUTPUT_RATE_HZ / s_rtlsdr_hifi_source_rate : 1.0;
    double encoded_per_raw = 1.0;
    if (flac && raw_total > 0 && encoded_total > 0) {
        encoded_per_raw = (double)encoded_total / (double)raw_total;
        if (encoded_per_raw < 0.10) encoded_per_raw = 0.10;
        if (encoded_per_raw > 1.00) encoded_per_raw = 1.00;
    }

    double estimate = (double)backlog_raw_bytes * expansion * encoded_per_raw;
    if (estimate >= (double)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)estimate;
}
static void gui_record_spill_recycle_if_drained(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    if (!atomic_load(&spill->opened) || !spill->fp) {
        return;
    }

    uint64_t write_off = atomic_load(&spill->write_offset);
    uint64_t read_off = atomic_load(&spill->read_offset);
    if (write_off != read_off) {
        return;
    }

    gui_record_spill_lock(spill);
    write_off = atomic_load(&spill->write_offset);
    read_off = atomic_load(&spill->read_offset);
    if (write_off == read_off && spill->fp) {
        bool truncated_ok = false;
        (void)GUI_RECORD_FSEEK(spill->fp, 0, SEEK_SET);
#if defined(_WIN32) || defined(_WIN64)
        int fd = _fileno(spill->fp);
        if (fd >= 0) {
            if (fflush(spill->fp) == 0 && _chsize_s(fd, 0) == 0) {
                truncated_ok = true;
            }
        }
#else
        int fd = fileno(spill->fp);
        if (fd >= 0) {
            if (fflush(spill->fp) == 0 && ftruncate(fd, 0) == 0) {
                truncated_ok = true;
            }
        }
#endif
        if (!truncated_ok) {
            clearerr(spill->fp);
        }
        (void)GUI_RECORD_FSEEK(spill->fp, 0, SEEK_SET);
        atomic_store(&spill->write_offset, 0);
        atomic_store(&spill->read_offset, 0);
        atomic_store(&spill->last_backlog_log_mark, 0);
        atomic_store(&spill->forced_mode, false);
    }
    gui_record_spill_unlock(spill);
}

bool gui_record_spill_read_block(int channel, void *dst, size_t bytes) {
    if (!gui_record_spill_valid_channel(channel) || !dst || bytes == 0) {
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    if (!atomic_load(&spill->opened) || !spill->fp) {
        return false;
    }

    uint64_t write_off = atomic_load(&spill->write_offset);
    uint64_t read_off = atomic_load(&spill->read_offset);
    if (write_off < read_off || (write_off - read_off) < bytes) {
        return false;
    }

    bool ok = false;
    gui_record_spill_lock(spill);
    if (GUI_RECORD_FSEEK(spill->fp, read_off, SEEK_SET) == 0) {
        size_t nread = fread((void *)dst, 1, bytes, spill->fp);
        if (nread == bytes) {
            ok = true;
        } else {
            clearerr(spill->fp);
        }
    }
    gui_record_spill_unlock(spill);

    if (ok) {
        atomic_store(&spill->read_offset, read_off + bytes);
        gui_record_spill_recycle_if_drained(channel);
    }

    return ok;
}

uint64_t gui_record_spill_backlog_bytes(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return 0;
    }
    return gui_record_spill_backlog_bytes_locked(channel);
}

bool gui_record_spill_enqueue(gui_app_t *app, int channel, const void *data, size_t bytes,
                              uint32_t frame_index, char *error_msg, size_t error_msg_size) {
    if (!gui_record_spill_valid_channel(channel) || !data || bytes == 0) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Invalid spill enqueue request");
        }
        return false;
    }

    if (!gui_record_spill_open_channel(app, channel, error_msg, error_msg_size)) {
        atomic_store(&s_record_producer_lost_data, true);
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    bool ok = false;
    gui_record_spill_lock(spill);
    bool first_force = !atomic_exchange(&spill->forced_mode, true);
    uint64_t write_off = atomic_load(&spill->write_offset);
    if (GUI_RECORD_FSEEK(spill->fp, write_off, SEEK_SET) == 0) {
        size_t nwritten = fwrite(data, 1, bytes, spill->fp);
        if (nwritten == bytes) {
            atomic_store(&spill->write_offset, write_off + bytes);
            ok = true;
        } else {
            clearerr(spill->fp);
        }
    }
    gui_record_spill_unlock(spill);

    if (first_force && app) {
        char msg[640];
        snprintf(msg, sizeof(msg),
                 "Record buffer backpressure on channel %s at frame=%u. Enabling temp spill file: %s",
                 gui_record_spill_channel_name(channel), frame_index,
                 spill->path[0] ? spill->path : "(temp)");
        gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
    }

    if (!ok) {
        atomic_store(&s_record_producer_lost_data, true);
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Failed writing spill data for channel %s: %s",
                     gui_record_spill_channel_name(channel), strerror(errno));
        }
        return false;
    }

    uint64_t backlog = gui_record_spill_backlog_bytes_locked(channel);
    uint64_t mark = backlog / GUI_RECORD_SPILL_LOG_STEP_BYTES;
    uint64_t last_mark = atomic_load(&spill->last_backlog_log_mark);
    if (mark > last_mark) {
        atomic_store(&spill->last_backlog_log_mark, mark);
        if (app) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Spill backlog channel %s: %.2f MB",
                     gui_record_spill_channel_name(channel),
                     (double)backlog / (1024.0 * 1024.0));
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
    }

    return true;
}

// Converted capture uses the same brief ring wait as direct RAW. Once
// bytes spill, keep newer bytes behind them until that backlog is drained.
static const backpressure_policy_t s_record_write_policy = {
    .max_wait_attempts = 2,
    .wait_timeout_ms = 1,
};

bool gui_record_push_converted(gui_app_t *app, int channel, const int16_t *samples,
                               size_t bytes, uint32_t frame_index,
                               char *error_msg, size_t error_msg_size) {
    buffer_id_t id = channel == 0 ? BUF_RECORD_A : BUF_RECORD_B;
    bool try_ring = gui_record_spill_backlog_bytes(channel) == 0;
    if (try_ring) {
        void *out = bufmgr_write_begin(&app->buffers, id, bytes, &s_record_write_policy);
        if (out) {
            memcpy(out, samples, bytes);
            bufmgr_write_end(&app->buffers, id, bytes);
            return true;
        }
    }
    if (!gui_record_spill_enqueue(app, channel, samples, bytes, frame_index, error_msg, error_msg_size))
        return false;
    if (try_ring && atomic_load(&app->buffers.stats[id].write_drops))
        atomic_fetch_sub(&app->buffers.stats[id].write_drops, 1);
    return true;
}

// File writer context
typedef struct {
    buffer_manager_t *bufmgr;  // Buffer manager pointer
    buffer_id_t buf_id;        // BUF_RECORD_A or BUF_RECORD_B
    FILE *file;
    int channel;  // 0 = A, 1 = B

    // RF bit depth requested for output
    // - FLAC: 8/12/16
    // - RAW:  8/16
    uint8_t rf_bits;

    // For RAW writer: bytes per sample (1=8-bit, 2=16-bit)
    size_t raw_bytes_per_sample;

    // RF resampling (rate in kHz; 0 or disabled = passthrough)
    bool enable_resample;
    float input_sample_rate_khz;  // source/capture sample rate in kHz
    float resample_rate_khz;
    int resample_quality;      // 0-4
    float resample_gain_db;

#if LIBSOXR_ENABLED
    void *soxr;                // soxr_t (NULL if not initialized)
    float soxr_rate_khz;       // configured output rate (kHz)
#endif

#if LIBFLAC_ENABLED == 1
    flac_writer_t *writer;
    atomic_uint_fast64_t *compressed_bytes;
    uint8_t flac_bits_per_sample;
#endif
    gui_sdr_rf_t *rtl_converter;
    gui_app_t *app;  // For error reporting
} writer_ctx_t;

static bool gui_record_resample_requested(bool enabled, float input_khz, float output_khz) {
    return enabled && output_khz > 0.0f &&
        output_khz < (input_khz > 0.0f ? input_khz : 40000.0f);
}

#if LIBSOXR_ENABLED
#include <soxr.h>

static soxr_quality_spec_t soxr_quality_from_setting(int q) {
    // Map GUI setting 0-4 to soxr quality presets
    // 0=QQ, 1=LQ, 2=MQ, 3=HQ, 4=VHQ
    switch (q) {
        case 0: return soxr_quality_spec(SOXR_QQ, 0);
        case 1: return soxr_quality_spec(SOXR_LQ, 0);
        case 2: return soxr_quality_spec(SOXR_MQ, 0);
        case 3: return soxr_quality_spec(SOXR_HQ, 0);
        case 4: return soxr_quality_spec(SOXR_VHQ, 0);
        default: return soxr_quality_spec(SOXR_HQ, 0);
    }
}

static soxr_t ensure_soxr(writer_ctx_t *wctx, float out_rate_khz) {
    if (!wctx) return NULL;
    float in_rate_khz = (wctx->input_sample_rate_khz > 0.0f) ? wctx->input_sample_rate_khz : 40000.0f;

    // Reuse existing if already configured
    if (wctx->soxr && fabsf(wctx->soxr_rate_khz - out_rate_khz) < 1e-3f) {
        return (soxr_t)wctx->soxr;
    }

    if (wctx->soxr) {
        soxr_delete((soxr_t)wctx->soxr);
        wctx->soxr = NULL;
        wctx->soxr_rate_khz = 0.0f;
    }

    soxr_error_t err = NULL;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);

    // Only apply user gain (no implicit scaling)
    io_spec.scale = pow(10.0, (double)wctx->resample_gain_db / 20.0);

    soxr_quality_spec_t qual_spec = soxr_quality_from_setting(wctx->resample_quality);

    soxr_t s = soxr_create((double)in_rate_khz, (double)out_rate_khz, 1, &err, &io_spec, &qual_spec, NULL);
    if (!s || err) {
        if (s) soxr_delete(s);
        return NULL;
    }

    wctx->soxr = (void *)s;
    wctx->soxr_rate_khz = out_rate_khz;
    return s;
}
#endif

static writer_ctx_t s_ctx_a;
static writer_ctx_t s_ctx_b;

static inline int8_t gui_record_sample_12bit_to_i8(int16_t sample) {
    // BUF_RECORD samples are signed 12-bit values carried in int16_t.
    // Downscale to signed 8-bit with nearest rounding.
    int32_t v = sample;
    if (v >= 0) {
        v = (v + 8) >> 4;
    } else {
        v = -(((-v) + 8) >> 4);
    }
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (int8_t)v;
}

static void gui_record_account_writer_bytes(writer_ctx_t *wctx, size_t bytes) {
    gui_app_t *app = wctx->app;
    if (app) {
        atomic_fetch_add(&app->recording_bytes, bytes);
        atomic_fetch_add(wctx->channel == 0 ? &app->recording_raw_a : &app->recording_raw_b, bytes);
    }
}

static size_t gui_record_read_ring_tail(writer_ctx_t *wctx, const int16_t **samples) {
    size_t bytes = bufmgr_fill_level(wctx->bufmgr, wctx->buf_id);
    if (bytes > GUI_RECORD_WRITER_BLOCK_BYTES) bytes = GUI_RECORD_WRITER_BLOCK_BYTES;
    bytes -= bytes % sizeof(int16_t);
    *samples = bytes ? bufmgr_read_begin(wctx->bufmgr, wctx->buf_id, bytes, 0) : NULL;
    return *samples ? bytes : 0;
}

static size_t gui_record_get_next_block(writer_ctx_t *wctx, bool draining,
                                        int16_t *spill_block, const int16_t **samples,
                                        bool *from_ringbuffer) {
    // Use full-size chunks during recording. During final drain the producer
    // has stopped, so consume the exact tail without padding. Read an older
    // partial ring chunk before spill data even while recording.
    size_t bytes = GUI_RECORD_WRITER_BLOCK_BYTES;
    *samples = bufmgr_read_begin(wctx->bufmgr, wctx->buf_id, bytes, draining ? 0 : 10);
    uint64_t backlog = 0;
    if (!*samples) {
        backlog = gui_record_spill_backlog_bytes(wctx->channel);
        if (draining || backlog) bytes = gui_record_read_ring_tail(wctx, samples);
    }
    *from_ringbuffer = *samples != NULL;
    if (*from_ringbuffer) return bytes;
    // A zero snapshot did not establish that the ring was checked after
    // spill publication. Retry from the ring if data was queued since then.
    if (!backlog) return 0;

    if (draining && backlog < GUI_RECORD_WRITER_BLOCK_BYTES) {
        bytes = (size_t)backlog;
        bytes -= bytes % sizeof(int16_t);
    } else {
        bytes = GUI_RECORD_WRITER_BLOCK_BYTES;
    }
    if (bytes && gui_record_spill_read_block(wctx->channel, spill_block, bytes)) {
        *samples = spill_block;
        return bytes;
    }
    return 0;
}

typedef bool (*gui_record_write_samples_fn)(writer_ctx_t *, const int16_t *, size_t, void *);

static bool gui_record_process_samples(writer_ctx_t *wctx, const int16_t *in, size_t count,
                                       int16_t *resampled, gui_record_write_samples_fn write_samples,
                                       void *scratch) {
    bool downsample = in && gui_record_resample_requested(wctx->enable_resample,
        wctx->input_sample_rate_khz, wctx->resample_rate_khz);
#if LIBSOXR_ENABLED
    soxr_t resampler = downsample ? ensure_soxr(wctx, wctx->resample_rate_khz) : (soxr_t)wctx->soxr;
    if (downsample && !resampler) return false;
    if (resampler) {
        size_t produced;
        do {
            size_t consumed = 0;
            soxr_error_t err = soxr_process(resampler, in, count, &consumed,
                                            resampled, GUI_RECORD_WRITER_BLOCK_SAMPLES, &produced);
            if (err || (count && !consumed && !produced)) return false;
            if (produced && !write_samples(wctx, resampled, produced, scratch)) return false;
            if (in) in += consumed;
            count -= consumed;
        } while (in ? count != 0 : produced != 0);
        // Zero output while the filter acquires is successful. At EOF a
        // NULL input drains the filter instead of appending unresampled RF.
        return true;
    }
#else
    (void)resampled;
    if (downsample) return false;
#endif
    return !in || write_samples(wctx, in, count, scratch);
}

static bool gui_record_finish_resampler(writer_ctx_t *wctx, int16_t *resampled,
                                       gui_record_write_samples_fn write_samples, void *scratch,
                                       bool flush) {
#if LIBSOXR_ENABLED
    if (wctx->soxr) {
        bool ok = !flush || gui_record_process_samples(wctx, NULL, 0, resampled, write_samples, scratch);
        soxr_delete((soxr_t)wctx->soxr);
        wctx->soxr = NULL;
        wctx->soxr_rate_khz = 0.0f;
        return ok;
    }
#else
    (void)wctx;
    (void)resampled;
    (void)write_samples;
    (void)scratch;
    (void)flush;
#endif
    return true;
}

static void gui_record_rtlsdr_interrupt(void) {
    atomic_store(&s_rtlsdr_hifi_accepting, false);
    atomic_store(&s_record_producer_lost_data, true);
    atomic_store(&s_rtlsdr_hifi_interrupted, true);
}

bool gui_record_rtlsdr_active(void) {
    return atomic_load(&s_rtlsdr_hifi_accepting);
}

bool gui_record_get_rtlsdr_hifi_output(bool *use_flac) {
    unsigned output = atomic_load(&s_rtlsdr_hifi_output);
    if (output == GUI_RECORD_HIFI_OUTPUT_NONE) return false;
    if (use_flac) *use_flac = output == GUI_RECORD_HIFI_OUTPUT_FLAC;
    return true;
}

void gui_record_rtlsdr_gap(gui_app_t *app) {
    if (!app) return;
    atomic_fetch_add(&s_rtlsdr_hifi_inflight, 1);
    if (gui_record_rtlsdr_active()) gui_record_rtlsdr_interrupt();
    atomic_fetch_sub(&s_rtlsdr_hifi_inflight, 1);
}

void gui_record_rtlsdr_push(gui_app_t *app, const uint8_t *iq, size_t pairs, uint64_t tuning) {
    if (!app || !iq || !pairs) return;

    // The finalizer disables the tap and waits for in-flight pushes before
    // the writer drains. Only this capture thread produces merged RF input.
    atomic_fetch_add(&s_rtlsdr_hifi_inflight, 1);
    if (atomic_load(&s_rtlsdr_hifi_accepting)) {
        static const backpressure_policy_t no_wait = {0};
        const size_t bytes = 2 * pairs;
        void *dst = tuning == s_rtlsdr_hifi_tuning
            ? bufmgr_write_begin(&app->buffers, BUF_RECORD_A, bytes, &no_wait)
            : NULL;
        if (dst) {
            memcpy(dst, iq, bytes);
            bufmgr_write_end(&app->buffers, BUF_RECORD_A, bytes);
        } else {
            gui_record_rtlsdr_interrupt();
        }
    }
    atomic_fetch_sub(&s_rtlsdr_hifi_inflight, 1);
}

static void gui_record_writer_failed(writer_ctx_t *wctx, const char *message) {
    atomic_store(&s_record_write_error[wctx->channel], true);
    fprintf(stderr, "[REC] Channel %c: %s\n", wctx->channel == 0 ? 'A' : 'B', message);
    if (wctx->app) {
        gui_record_log_capture_event(wctx->app, "ERROR", message, GUI_ERROR_CLASS_SYSTEM, 1);
        if (gui_monitor_local_save_recording()) {
            atomic_store(&wctx->app->dropout_stop_reason, GUI_DROPOUT_BACKPRESSURE);
            atomic_store(&wctx->app->dropout_stop_requested, true);
        }
    }
}

#if LIBFLAC_ENABLED == 1
// FLAC writers (managed by shared library)
static flac_writer_t *s_flac_writer_a = NULL;
static flac_writer_t *s_flac_writer_b = NULL;

// Error callback for GUI FLAC writer
static void gui_flac_error_callback(void *user_data, flac_writer_error_t error, const char *message) {
    (void)error;
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->app) {
        gui_record_log_capture_event(wctx->app, "ERROR", message, GUI_ERROR_CLASS_SYSTEM, 1);
    }
    fprintf(stderr, "FLAC ERROR: %s\n", message);
}

// Bytes written callback for compression ratio tracking
static void gui_flac_bytes_callback(void *user_data, size_t bytes_written) {
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->compressed_bytes) {
        atomic_fetch_add(wctx->compressed_bytes, bytes_written);
    }
}
static void convert_i16_to_flac_i32(int32_t *dst, const int16_t *src, size_t n, uint8_t bits) {
    if (!dst || !src || n == 0) return;

    if (bits == 8) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = (int32_t)gui_record_sample_12bit_to_i8(src[i]);
        }
        return;
    }

    if (bits == 12) {
        // BUF_RECORD samples are signed 12-bit carried in int16_t, but the
        // soxr resampler can overshoot the 12-bit range on clipped input
        // (e.g. when downsampling on the fly). Clamp to the signed 12-bit
        // range declared to the FLAC encoder, otherwise samples exceed the
        // declared bit depth and FLAC__STREAM_ENCODER_CLIENT_ERROR fires.
        for (size_t i = 0; i < n; i++) {
            int32_t v = (int32_t)src[i];
            if (v > 2047) v = 2047;
            if (v < -2048) v = -2048;
            dst[i] = v;
        }
        return;
    }

    // 16-bit output: expand 12-bit capture samples to 16-bit range. The
    // resampler can overshoot the source 12-bit range; after the <<4 shift
    // that can exceed the signed 16-bit range declared to the FLAC encoder.
    // Clamp the shifted result so the encoder never receives out-of-range
    // samples (mirrors the 8-bit path's clamp behavior).
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)src[i] << 4;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = v;
    }
}

static bool gui_record_write_flac_samples(writer_ctx_t *wctx, const int16_t *in,
                                          size_t count, void *scratch) {
    int32_t *samples = scratch;
    convert_i16_to_flac_i32(samples, in, count, wctx->flac_bits_per_sample);
    return flac_writer_process(wctx->writer, samples, (uint32_t)count) >= 0;
}

// FLAC file writer thread
static int flac_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    bool failed = false;

    // Boost thread priority to avoid backpressure when window is minimized.
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    int16_t *tmp_i16 = NULL;
#if LIBSOXR_ENABLED
    tmp_i16 = (int16_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_BYTES);
#endif
    int32_t *tmp_i32 = (int32_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * sizeof(int32_t));
    int16_t *spill_i16 = (int16_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_BYTES);
    if (!tmp_i32 || !spill_i16
#if LIBSOXR_ENABLED
        || !tmp_i16
#endif
    ) {
        if (tmp_i16) aligned_free(tmp_i16);
        if (tmp_i32) aligned_free(tmp_i32);
        if (spill_i16) aligned_free(spill_i16);
        gui_record_writer_failed(wctx, "FLAC writer failed to allocate buffers");
        return 0;
    }

    fprintf(stderr, "[FLAC] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    if (wctx->writer) {
        flac_writer_error_t aff_err = flac_writer_apply_thread_affinity(wctx->writer);
        if (aff_err != FLAC_WRITER_OK) {
            fprintf(stderr, "[FLAC] Writer thread %c affinity warning: %s\n",
                    wctx->channel == 0 ? 'A' : 'B',
                    flac_writer_get_error_string(wctx->writer));
        }
    }

    while (1) {
        const int16_t *in = NULL;
        bool from_ringbuffer = false;
        bool draining = atomic_load(&s_record_writer_draining);
        size_t len = gui_record_get_next_block(wctx, draining, spill_i16, &in, &from_ringbuffer);
        if (!len) {
            if (draining) break;
            continue;
        }

        failed = !gui_record_process_samples(wctx, in, len / sizeof(*in),
                                             tmp_i16, gui_record_write_flac_samples, tmp_i32);
        if (from_ringbuffer) bufmgr_read_end(wctx->bufmgr, wctx->buf_id, len);
        if (failed) {
            // A failed encoder cannot safely consume a different block.
            gui_record_writer_failed(wctx, "FLAC encoding failed");
            break;
        }
        gui_record_account_writer_bytes(wctx, len);
    }

    if (!gui_record_finish_resampler(wctx, tmp_i16, gui_record_write_flac_samples, tmp_i32, !failed))
        gui_record_writer_failed(wctx, "FLAC resampler drain failed");

    if (tmp_i16) aligned_free(tmp_i16);
    if (tmp_i32) aligned_free(tmp_i32);

    if (spill_i16) aligned_free(spill_i16);
    fprintf(stderr, "[FLAC] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}
#endif

static void convert_i16_to_raw_bytes(uint8_t *dst, const int16_t *src, size_t n, uint8_t bits) {
    if (!dst || !src || n == 0) return;

    if (bits == 8) {
        int8_t *d = (int8_t *)dst;
        for (size_t i = 0; i < n; i++) {
            d[i] = gui_record_sample_12bit_to_i8(src[i]);
        }
        return;
    }

    // 16-bit raw: write int16 as-is
    memcpy(dst, src, n * sizeof(int16_t));
}

static bool gui_record_write_raw_bytes(writer_ctx_t *wctx, const uint8_t *output, size_t bytes) {
    size_t offset = 0;
    bool final_retry = false, retrying = false;
    for (;;) {
        bool ok;
        if (offset < bytes) {
            offset += fwrite(output + offset, 1, bytes - offset, wctx->file);
            ok = offset == bytes && !ferror(wctx->file);
        } else {
            ok = fflush(wctx->file) == 0;
        }
        if (ok) break;
        if (!retrying) {
            gui_record_writer_failed(wctx, "RAW output write failed");
            retrying = true;
        }
        if (final_retry || gui_monitor_local_save_recording()) return false;

        // Retry the same converted bytes before consuming another input
        // block. At stop allow one final attempt without a retry delay.
        uint64_t retry_at = get_time_ms() + 2000;
        while (!atomic_load(&s_record_writer_draining) && get_time_ms() < retry_at)
            thrd_sleep_ms(10);
        final_retry = atomic_load(&s_record_writer_draining);
        clearerr(wctx->file);
    }
    if (retrying) {
        atomic_store(&s_record_write_error[wctx->channel], false);
        if (wctx->app)
            gui_record_log_capture_event(wctx->app, "INFO", "RAW output file write recovered",
                                         GUI_ERROR_CLASS_NONE, 0);
    }
    // RAW counters describe the actual bytes written, including resampler
    // output at EOF. The FLAC path separately counts accepted input bytes.
    gui_record_account_writer_bytes(wctx, bytes);
    return true;
}

static bool gui_record_write_raw_samples(writer_ctx_t *wctx, const int16_t *in,
                                         size_t count, void *scratch) {
    uint8_t *output = scratch;
    convert_i16_to_raw_bytes(output, in, count, wctx->rf_bits);
    return gui_record_write_raw_bytes(wctx, output, count * wctx->raw_bytes_per_sample);
}

static bool gui_record_write_rtlsdr_hifi(void *opaque, const int16_t *samples, size_t count) {
    writer_ctx_t *wctx = opaque;
#if LIBFLAC_ENABLED == 1
    if (wctx->writer) {
        if (flac_writer_process_int16(wctx->writer, samples, (uint32_t)count) < 0) return false;
        gui_record_account_writer_bytes(wctx, count * sizeof(*samples));
        return true;
    }
#endif
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return gui_record_write_raw_bytes(wctx, (const uint8_t *)samples, count * sizeof(*samples));
#else
    uint8_t bytes[8192];
    while (count) {
        size_t chunk = count < sizeof(bytes) / 2 ? count : sizeof(bytes) / 2;
        for (size_t i = 0; i < chunk; ++i) {
            uint16_t sample = (uint16_t)samples[i];
            bytes[2 * i] = (uint8_t)sample;
            bytes[2 * i + 1] = (uint8_t)(sample >> 8);
        }
        if (!gui_record_write_raw_bytes(wctx, bytes, chunk * 2)) return false;
        samples += chunk;
        count -= chunk;
    }
    return true;
#endif
}

static int rtlsdr_hifi_writer_thread(void *ctx) {
    writer_ctx_t *wctx = ctx;
#if LIBFLAC_ENABLED == 1
    if (wctx->writer && flac_writer_apply_thread_affinity(wctx->writer) != FLAC_WRITER_OK)
        fprintf(stderr, "[FLAC] Hi-Fi RF writer affinity warning: %s\n",
                flac_writer_get_error_string(wctx->writer));
#endif

    bool ok = true;
    while (ok) {
        bool draining = atomic_load(&s_record_writer_draining);
        size_t bytes = 65536;
        const int16_t *input = bufmgr_read_begin(wctx->bufmgr, wctx->buf_id, bytes,
                                                draining ? 0 : 10);
        if (!input && draining) {
            bytes = gui_record_read_ring_tail(wctx, &input);
            if (!bytes) break;
        }
        if (!input) continue;

        ok = gui_sdr_rf_process(wctx->rtl_converter, (const uint8_t *)input, bytes / 2,
                                 gui_record_write_rtlsdr_hifi, wctx);
        bufmgr_read_end(wctx->bufmgr, wctx->buf_id, bytes);
    }
    if (ok && !atomic_load(&s_rtlsdr_hifi_interrupted))
        ok = gui_sdr_rf_finish(wctx->rtl_converter, gui_record_write_rtlsdr_hifi, wctx);
    if (!ok) {
        gui_record_rtlsdr_interrupt();
        gui_record_writer_failed(wctx, gui_sdr_rf_error(wctx->rtl_converter));
    }
    gui_sdr_rf_destroy(wctx->rtl_converter);
    wctx->rtl_converter = NULL;
    return 0;
}

// RAW file writer thread
static int raw_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;

    // Output bytes per sample (1=8-bit, 2=16-bit)
    size_t bps = wctx->raw_bytes_per_sample;

    // Boost thread priority to avoid backpressure when window is minimized
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    int16_t *tmp_i16 = NULL;
#if LIBSOXR_ENABLED
    tmp_i16 = (int16_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_BYTES);
    if (!tmp_i16) {
        gui_record_writer_failed(wctx, "RAW writer failed to allocate resample buffer");
        return 0;
    }
#endif
    uint8_t *tmp_out = (uint8_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * bps);
    if (!tmp_out) {
        gui_record_writer_failed(wctx, "RAW writer failed to allocate output buffer");
#if LIBSOXR_ENABLED
        aligned_free(tmp_i16);
#endif
        return 0;
    }
    int16_t *spill_i16 = (int16_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_BYTES);
    if (!spill_i16) {
        gui_record_writer_failed(wctx, "RAW writer failed to allocate spill buffer");
#if LIBSOXR_ENABLED
        aligned_free(tmp_i16);
#endif
        aligned_free(tmp_out);
        return 0;
    }

    fprintf(stderr, "[RAW] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    bool failed = false;
    while (1) {
        const int16_t *in = NULL;
        bool from_ringbuffer = false;
        bool draining = atomic_load(&s_record_writer_draining);
        size_t in_len = gui_record_get_next_block(wctx, draining, spill_i16, &in, &from_ringbuffer);
        if (!in_len) {
            if (draining) break;
            continue;
        }
        failed = !gui_record_process_samples(wctx, in, in_len / sizeof(*in),
                                             tmp_i16, gui_record_write_raw_samples, tmp_out);
        if (from_ringbuffer) bufmgr_read_end(wctx->bufmgr, wctx->buf_id, in_len);
        if (failed) {
            if (!atomic_load(&s_record_write_error[wctx->channel]))
                gui_record_writer_failed(wctx, "RAW sample conversion failed");
            break;
        }
    }

    if (!gui_record_finish_resampler(wctx, tmp_i16, gui_record_write_raw_samples, tmp_out, !failed))
        gui_record_writer_failed(wctx, "RAW resampler drain failed");
    if (tmp_i16) aligned_free(tmp_i16);
    aligned_free(spill_i16);
    aligned_free(tmp_out);

    fprintf(stderr, "[RAW] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}

// Direct native RAW passthrough contexts (CXADC, FLAC off, no resampling;
// see the eligibility/tap wiring below, after raw_ext_for_bits).
static gui_record_direct_ctx_t s_direct_ctx_a;
static gui_record_direct_ctx_t s_direct_ctx_b;
static atomic_uint_fast64_t s_direct_bytes_read_a;
static atomic_uint_fast64_t s_direct_bytes_read_b;
// Latched at record start for the end-of-recording summary.
static bool s_direct_channel_a_active = false;
static bool s_direct_channel_b_active = false;

void gui_record_reset_stats(gui_app_t *app) {
    atomic_store(&s_rtlsdr_hifi_output, GUI_RECORD_HIFI_OUTPUT_NONE);
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    app->last_recording_duration_s = 0.0;
}

// Initialize recording subsystem
void gui_record_init(void) {
    memset(&s_direct_ctx_a, 0, sizeof(s_direct_ctx_a));
    memset(&s_direct_ctx_b, 0, sizeof(s_direct_ctx_b));
    atomic_store(&s_record_stop_finalizing, false);
    atomic_store(&s_record_stop_finalize_done, false);
    gui_record_reset_disk_guard_state();
    gui_record_spill_reset_all();
}

void gui_record_wait_for_finalize(void) {
    while (!gui_record_collect_finalize_if_done() &&
           (atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running)))
        thrd_sleep_ms(10);
}

bool gui_record_last_stop_failed(void) {
    return atomic_load(&s_record_stop_failed);
}

static const char *gui_record_stop_status(void) {
    if (atomic_load(&s_record_stop_commit_unconfirmed))
        return "Local Save publication unconfirmed; check client";
    return atomic_load(&s_record_stop_failed) ? "Recording incomplete" : "Recording stopped";
}

// Cleanup recording subsystem
void gui_record_cleanup(void) {
    gui_record_wait_for_finalize();
    memset(&s_direct_ctx_a, 0, sizeof(s_direct_ctx_a));
    memset(&s_direct_ctx_b, 0, sizeof(s_direct_ctx_b));
    gui_record_reset_disk_guard_state();
    gui_record_spill_reset_all();
    gui_record_close_session_log();
}

// Check if recording is active
bool gui_record_is_active(void) {
    return s_recording_app != NULL && s_recording_app->is_recording;
}

void gui_record_mark_source_loss(void) {
    atomic_fetch_add(&s_capture_source_loss_count, 1);
}

void gui_record_mark_audio_source_loss(void) {
    atomic_fetch_add(&s_capture_audio_loss_count, 1);
}

static bool gui_record_wav_requested(const gui_record_output_targets_t *targets) {
    for (int i = GUI_RECORD_RF_OUTPUT_COUNT; i < GUI_RECORD_OUTPUT_COUNT; ++i)
        if (targets->paths[i][0]) return true;
    return false;
}

static bool gui_record_local_wav_ready(const gui_app_t *app) {
    if (app->settings.net_mode == GUI_NET_MODE_CLIENT || app->selected_device < 0 ||
        app->selected_device >= app->device_count) return true;
    int type = app->devices[app->selected_device].type;
    return (type != DEVICE_TYPE_CXADC && type != DEVICE_TYPE_MISRC_CLOCKGEN) ||
           atomic_load(&app->cxadc_audio_ready);
}

static void gui_record_start_source_tracking(const gui_app_t *app,
        const gui_record_output_targets_t *targets, bool direct_a, bool direct_b) {
    const bool wav = gui_record_wav_requested(targets);
    s_record_uses_capture_rf = (app->settings.capture_a && !direct_a) ||
        (app->settings.capture_b && !direct_b);
    s_record_uses_client_audio = wav && app->settings.net_mode == GUI_NET_MODE_CLIENT;
    s_record_uses_capture_audio = wav && !s_record_uses_client_audio;
    s_capture_audio_loss_start = atomic_load(&s_capture_audio_loss_count);
    s_capture_source_loss_start = atomic_load(&s_capture_source_loss_count);
    s_capture_rf_drop_start = atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].write_drops);
    s_client_audio_loss_start = atomic_load(&app->net_audio_client_interruptions);
    if ((app->settings.net_mode == GUI_NET_MODE_CLIENT &&
         (!atomic_load(&app->net_rf_client_stream_live) ||
          (s_record_uses_client_audio && !gui_net_client_wav_ready(app)))) ||
        (s_record_uses_capture_audio && !gui_record_local_wav_ready(app)))
        atomic_store(&s_record_producer_lost_data, true);
}

static void gui_record_check_source_tracking(const gui_app_t *app) {
    bool source_loss = atomic_load(&s_capture_source_loss_count) != s_capture_source_loss_start ||
        (app->settings.net_mode == GUI_NET_MODE_CLIENT && !atomic_load(&app->net_rf_client_stream_live));
    bool rf_loss = s_record_uses_capture_rf &&
        atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].write_drops) != s_capture_rf_drop_start;
    bool audio_loss = s_record_uses_client_audio &&
        atomic_load(&app->net_audio_client_interruptions) != s_client_audio_loss_start;
    bool capture_audio_loss = s_record_uses_capture_audio &&
        (atomic_load(&s_capture_audio_loss_count) != s_capture_audio_loss_start ||
         !gui_record_local_wav_ready(app));
    if (source_loss || rf_loss || audio_loss || capture_audio_loss)
        atomic_store(&s_record_producer_lost_data, true);
}

bool gui_record_has_write_error(void) {
    for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
        if (atomic_load(&s_record_write_error[i])) {
            return true;
        }
    }
    return false;
}

// Check if waiting for popup confirmation
bool gui_record_is_pending(void) {
    return s_overwrite_pending;
}
static bool gui_record_level_is_error(const char *level) {
    if (!level) return false;
    return (strcmp(level, "ERROR") == 0 || strcmp(level, "CRITICAL") == 0);
}

void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t error_count) {
    if (!app || !message || !message[0]) {
        return;
    }

    char clean[1024];
    snprintf(clean, sizeof(clean), "%s", message);
    gui_record_trim_trailing_newline(clean);
    if (!clean[0]) {
        return;
    }

    if (gui_record_level_is_error(level)) {
        uint32_t increment = (error_count > 0) ? error_count : 1;
        if (error_class == GUI_ERROR_CLASS_PARSER) {
            gui_app_count_parser_errors(app, increment);
        } else if (error_class == GUI_ERROR_CLASS_SYSTEM) {
            gui_app_count_system_errors(app, increment);
        }
    }
    if (app == s_recording_app) {
        gui_record_log_write_line(level, clean);
    }
}

bool gui_record_check_disk_space_guard(gui_app_t *app, uint32_t frame_index,
                                       char *status_msg, size_t status_msg_size) {
    if (status_msg && status_msg_size > 0) {
        status_msg[0] = '\0';
    }

    if (!app || !app->is_recording || !app->settings.output_path[0]) {
        return false;
    }
    /* Client-owned files: the server's disk free space is irrelevant. */
    if(gui_monitor_local_save_recording())return false;

    if (atomic_load(&s_disk_guard_tripped)) {
        return true;
    }

    uint64_t now_ms = get_time_ms();
    uint64_t last_check_ms = atomic_load(&s_disk_guard_last_check_ms);
    if (last_check_ms != 0 && now_ms >= last_check_ms &&
        (now_ms - last_check_ms) < GUI_RECORD_DISK_GUARD_CHECK_INTERVAL_MS) {
        return false;
    }
    atomic_store(&s_disk_guard_last_check_ms, now_ms);

    uint64_t free_bytes = 0;
    if (!gui_record_get_free_space_bytes(app->settings.output_path, &free_bytes)) {
        return false;
    }
    atomic_store(&s_disk_guard_last_free_bytes, free_bytes);
    uint64_t raw_total = gui_record_get_raw_total_bytes(app);
    uint64_t encoded_total = gui_record_get_encoded_total_bytes(app);
    uint64_t output_total = gui_record_get_effective_output_total_bytes(app, raw_total, encoded_total);
    uint64_t previous_output_total = atomic_load(&s_disk_guard_last_output_bytes);
    uint64_t output_rate_bps = atomic_load(&s_disk_guard_output_rate_bps);

    if (last_check_ms != 0 && now_ms > last_check_ms && output_total >= previous_output_total) {
        uint64_t elapsed_ms = now_ms - last_check_ms;
        if (elapsed_ms > 0) {
            uint64_t delta_bytes = output_total - previous_output_total;
            uint64_t instant_bps = (delta_bytes * 1000ULL) / elapsed_ms;
            if (output_rate_bps == 0) {
                output_rate_bps = instant_bps;
            } else {
                output_rate_bps = (output_rate_bps * 3ULL + instant_bps) / 4ULL;
            }
            atomic_store(&s_disk_guard_output_rate_bps, output_rate_bps);
        }
    }
    atomic_store(&s_disk_guard_last_output_bytes, output_total);

    uint64_t queued_output = gui_record_estimate_output_bytes_from_raw_backlog(
        app, gui_record_queued_bytes(&app->buffers), raw_total, encoded_total);

    uint64_t projected_growth = 0;
    if (output_rate_bps > 0) {
        double projected = (double)output_rate_bps * GUI_RECORD_DISK_GUARD_LOOKAHEAD_SECONDS;
        projected_growth = (projected >= (double)UINT64_MAX) ? UINT64_MAX : (uint64_t)projected;
    }

    uint64_t required_headroom = GUI_RECORD_DISK_GUARD_MIN_HEADROOM_BYTES;
    if (projected_growth > required_headroom) {
        required_headroom = projected_growth;
    }
    if (queued_output > (UINT64_MAX - required_headroom)) {
        required_headroom = UINT64_MAX;
    } else {
        required_headroom += queued_output;
    }
    atomic_store(&s_disk_guard_last_required_bytes, required_headroom);

    if (free_bytes >= required_headroom) {
        return false;
    }

    char free_buf[32];
    char required_buf[32];
    char backlog_buf[32];
    char msg[640];
    double rate_mb_s = (double)output_rate_bps / (1024.0 * 1024.0);
    format_file_size_u64(free_bytes, free_buf, sizeof(free_buf));
    format_file_size_u64(required_headroom, required_buf, sizeof(required_buf));
    format_file_size_u64(queued_output, backlog_buf, sizeof(backlog_buf));
    snprintf(msg, sizeof(msg),
             "Disk-space guard triggered at extract_frame=%u: free=%s required=%s (lookahead=%.0fs, output_rate=%.2f MB/s, queued_output=%s) on output path %s; requesting safe capture stop.",
             frame_index, free_buf, required_buf, GUI_RECORD_DISK_GUARD_LOOKAHEAD_SECONDS,
             rate_mb_s, backlog_buf, app->settings.output_path);
    if (status_msg && status_msg_size > 0) {
        snprintf(status_msg, status_msg_size, "%s", msg);
    }

    if (!atomic_exchange(&s_disk_guard_tripped, true)) {
        gui_record_log_capture_event(app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
    }
    return true;
}

// Forward declaration of actual recording start (after confirmation)
static int gui_record_start_confirmed(gui_app_t *app,
                                      const gui_record_output_targets_t *confirmed);

static uint8_t clamp_rf_bits_flac(uint8_t bits) {
    if (bits == 8 || bits == 12 || bits == 16) return bits;
    return 16;
}

#if LIBFLAC_ENABLED == 1
static bool gui_record_flac_append_comment(FLAC__StreamMetadata *block,
                                           const char *name,
                                           const char *value)
{
    if (!block || !name || !name[0] || !value) return false;
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

/* Finalize FLAC metadata in a single in-place pass using the simple iterator.
 *
 * Two updates are needed after encoding finishes:
 *   1. STREAMINFO.total_samples / sample_rate — the encoder writes raw Hz-domain
 *      sample counts but the RF FLAC stores sample_rate in kHz (40 MHz exceeds
 *      FLAC's 20-bit Hz field), so total_samples must be scaled to the kHz
 *      domain for the file's reported duration to match real time.
 *   2. Vorbis comment duration tags (DURATION_SECONDS, LENGTH, RF_TOTAL_SAMPLES,
 *      RF_SAMPLE_RATE, RF_SAMPLE_RATE_KHZ) for post-processing tools.
 *
 * Both are done in place with the simple iterator: STREAMINFO is a fixed-size
 * block so set_block writes it without a temp file; the Vorbis comment entries
 * are appended to the existing VC block (created by the encoder with adjacent
 * padding), so set_block with use_padding=true grows it by consuming padding —
 * also in place. This replaces the old chain-write path which rewrote the
 * entire multi-GB FLAC file to a temp copy for every metadata change. */
static void gui_record_finalize_flac_metadata(gui_app_t *app,
                                              const char *path,
                                              const char *channel_label,
                                              uint64_t total_samples,
                                              uint32_t sample_rate_hz)
{
    if (!path || !path[0] || total_samples == 0 || sample_rate_hz == 0) return;
    uint64_t rf_sample_rate_hz = (uint64_t)sample_rate_hz * 1000ULL;
    double duration_seconds = (double)total_samples / (double)rf_sample_rate_hz;
    if (duration_seconds < 0.0) duration_seconds = 0.0;
    uint64_t length_ms = (uint64_t)llround(duration_seconds * 1000.0);

    char duration_seconds_str[64];
    char length_ms_str[32];
    char total_samples_str[32];
    char sample_rate_str[32];
    char sample_rate_khz_str[32];
    snprintf(duration_seconds_str, sizeof(duration_seconds_str), "%.6f", duration_seconds);
    snprintf(length_ms_str, sizeof(length_ms_str), "%" PRIu64, length_ms);
    snprintf(total_samples_str, sizeof(total_samples_str), "%" PRIu64, total_samples);
    snprintf(sample_rate_str, sizeof(sample_rate_str), "%" PRIu64, rf_sample_rate_hz);
    snprintf(sample_rate_khz_str, sizeof(sample_rate_khz_str), "%u", sample_rate_hz);

    // STREAMINFO total_samples in the kHz-scaled domain.
    FLAC__uint64 duration_total_samples = (total_samples + 500ULL) / 1000ULL;
    if (duration_total_samples == 0 && total_samples > 0) duration_total_samples = 1;
    FLAC__uint64 max_total_samples = ((FLAC__uint64)1 << 36) - 1;
    FLAC__uint64 clamped_total = (duration_total_samples > max_total_samples)
                                   ? max_total_samples
                                   : duration_total_samples;

    FLAC__Metadata_SimpleIterator *it = FLAC__metadata_simple_iterator_new();
    if (!it) {
        if (app) {
            gui_record_log_capture_event(app, "WARN",
                "Failed to allocate FLAC metadata iterator for finalize",
                GUI_ERROR_CLASS_NONE, 0);
        }
        return;
    }

    if (!FLAC__metadata_simple_iterator_init(it, path, /*read_only=*/false, /*preserve_file_stats=*/true)) {
        if (app) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Failed to open FLAC metadata iterator for %s (%s)",
                     channel_label ? channel_label : "RF", path);
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
        FLAC__metadata_simple_iterator_delete(it);
        return;
    }

    /* 1. Update STREAMINFO (first block, fixed 34-byte size → in-place write). */
    FLAC__StreamMetadata *si_block = FLAC__metadata_simple_iterator_get_block(it);
    if (!si_block || si_block->type != FLAC__METADATA_TYPE_STREAMINFO) {
        if (app) {
            char msg[512];
            snprintf(msg, sizeof(msg), "STREAMINFO block not found for %s (%s)",
                     channel_label ? channel_label : "RF", path);
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
        if (si_block) FLAC__metadata_object_delete(si_block);
        FLAC__metadata_simple_iterator_delete(it);
        return;
    }
    si_block->data.stream_info.total_samples = clamped_total;
    si_block->data.stream_info.sample_rate = sample_rate_hz;
    if (!FLAC__metadata_simple_iterator_set_block(it, si_block, /*use_padding=*/false)) {
        if (app) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Failed to write STREAMINFO duration for %s (%s)",
                     channel_label ? channel_label : "RF", path);
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
    }
    FLAC__metadata_object_delete(si_block);

    /* 2. Find the existing Vorbis comment block and append duration tags.
    *    The encoder creates a VC block (with adjacent padding when the writer
    *    is configured for it), so set_block with use_padding=true can grow it
    *    in place by consuming the adjacent padding — no temp file. */
    bool vc_updated = false;
    while (FLAC__metadata_simple_iterator_next(it)) {
        FLAC__StreamMetadata *vc_block = FLAC__metadata_simple_iterator_get_block(it);
        if (!vc_block) continue;
        if (vc_block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            bool ok =
                gui_record_flac_append_comment(vc_block, "DURATION_SECONDS", duration_seconds_str) &&
                gui_record_flac_append_comment(vc_block, "LENGTH", length_ms_str) &&
                gui_record_flac_append_comment(vc_block, "RF_TOTAL_SAMPLES", total_samples_str) &&
                gui_record_flac_append_comment(vc_block, "RF_SAMPLE_RATE", sample_rate_str) &&
                gui_record_flac_append_comment(vc_block, "RF_SAMPLE_RATE_KHZ", sample_rate_khz_str);
            if (ok && s_rtlsdr_hifi_source_rate) {
                char source_rate[16], center[16];
                snprintf(source_rate, sizeof(source_rate), "%" PRIu32, s_rtlsdr_hifi_source_rate);
                snprintf(center, sizeof(center), "%" PRIu32, (uint32_t)s_rtlsdr_hifi_tuning);
                ok = gui_record_flac_append_comment(vc_block, "RF_SOURCE", "RTLSDR_HIFI") &&
                     gui_record_flac_append_comment(vc_block, "SOURCE_SAMPLE_RATE", source_rate) &&
                     gui_record_flac_append_comment(vc_block, "SOURCE_CENTER_FREQUENCY", center);
            }
            if (ok) {
                if (!FLAC__metadata_simple_iterator_set_block(it, vc_block, /*use_padding=*/true)) {
                    if (app) {
                        char msg[512];
                        snprintf(msg, sizeof(msg), "Failed writing FLAC duration tags for %s (%s)",
                                 channel_label ? channel_label : "RF", path);
                        gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
                    }
                } else {
                    vc_updated = true;
                }
            } else {
                if (app) {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "Failed building FLAC duration tags for %s (%s)",
                             channel_label ? channel_label : "RF", path);
                    gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
                }
            }
            FLAC__metadata_object_delete(vc_block);
            break;
        }
        FLAC__metadata_object_delete(vc_block);
    }

    if (!vc_updated && app) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Vorbis comment block not found for %s (%s) — duration tags not embedded",
                 channel_label ? channel_label : "RF", path);
        gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
    }

    FLAC__metadata_simple_iterator_delete(it);
}
#endif

static uint8_t rf_bits_for_raw(uint8_t requested) {
    // RAW supports 8/16 only; treat 12 as 16.
    return (requested == 8) ? 8 : 16;
}

// RAW file extension, named by what the file contains (matches the
// ld-decode/cxadc raw-sample convention so downstream tools pick the right
// sample width and signedness from the extension without a sidecar):
// - direct native passthrough (CXADC, FLAC off, no resample on the channel):
//   the card's bytes unchanged -> unsigned .u8 (8-bit mode) / .u16 (tenbit)
// - converted output (resampled channels, non-CXADC devices): the signed
//   samples produced by the decode/resample writer -> .s8 / .s16
static const char *raw_ext_for_bits(uint8_t bits, bool direct_native) {
    if (direct_native) {
        return (bits == 8) ? "u8" : "u16";
    }
    return (bits == 8) ? "s8" : "s16";
}

// --- Direct native RAW passthrough (CXADC, FLAC off, no resampling) ---
// The CXADC capture thread pushes the exact bytes it reads from the card
// straight into the channel's record ringbuffer (tap below), and the
// byte-exact writer thread fwrites them unchanged, so the output file is
// the card's native stream (unsigned .u8/.u16). All other RAW paths
// (resampled channels, non-CXADC devices) keep the converting legacy writer
// (signed .s8/.s16). Contexts + counters live above (gui_record_init /
// gui_record_cleanup reset them before any session can start).

// Shared eligibility: direct native passthrough applies when the selected
// device is a running CXADC capture, FLAC compression is off, the channel's
// resampler is off, and the card behind the channel actually delivers
// samples of the requested width (8-bit mode -> native u8 bytes, tenbit
// mode -> native u16 words). Otherwise the converted legacy RAW path runs.
// Used by gui_record_apply_auto_names (file naming), gui_record_open_session_log
// (capture log) and gui_record_start_confirmed (writer/tap selection) so all
// three always agree with the on-disk content.
static bool gui_record_direct_channel_eligible(const gui_app_t *app, int channel) {
    if(gui_monitor_local_save_recording())return false;
    if (!app || app->settings.use_flac) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    if (app->devices[app->selected_device].type != DEVICE_TYPE_CXADC) return false;
    if (channel != 0 && channel != 1) return false;
    if (!gui_cxadc_is_running()) return false;
    bool channel_enabled = (channel == 0) ? app->settings.capture_a : app->settings.capture_b;
    if (!channel_enabled) return false;
    bool resample_on = (channel == 0) ? app->settings.enable_resample_a : app->settings.enable_resample_b;
    if (resample_on) return false;
    uint8_t raw_bits = rf_bits_for_raw((channel == 0) ? app->settings.rf_bits_a : app->settings.rf_bits_b);
    size_t bytes_per_sample = 0;
    if (!gui_cxadc_direct_record_available(channel, raw_bits, &bytes_per_sample)) return false;
    return true;
}

// gui_record_direct callbacks: bridge the module's injectable interface to
// this file's spill temp file, capture log and per-channel write-error flags.
static bool gui_record_direct_spill_enqueue_cb(void *user, int channel, const void *data,
                                               size_t len, uint32_t frame_index) {
    char err[256] = {0};
    if (!gui_record_spill_enqueue((gui_app_t *)user, channel, data, len,
                                  frame_index, err, sizeof(err))) {
        if (err[0]) {
            gui_record_log_capture_event((gui_app_t *)user, "ERROR", err, GUI_ERROR_CLASS_SYSTEM, 1);
        }
        return false;
    }
    return true;
}

static bool gui_record_direct_spill_read_cb(void *user, int channel, void *dst, size_t len) {
    (void)user;
    return gui_record_spill_read_block(channel, dst, len);
}

static uint64_t gui_record_direct_spill_backlog_cb(void *user, int channel) {
    (void)user;
    return gui_record_spill_backlog_bytes(channel);
}

static void gui_record_direct_set_write_error_cb(void *user, int channel, bool active) {
#if defined(__APPLE__)
    if (active && gui_cxadc_macos_active()) {
        gui_app_t *app = user;
        gui_cxadc_macos_mark_failed();
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_BACKPRESSURE);
        atomic_store(&app->dropout_stop_requested, true);
    }
#else
    (void)user;
#endif
    if (gui_record_spill_valid_channel(channel)) {
        atomic_store(&s_record_write_error[channel], active);
    }
}

static void gui_record_direct_log_cb(void *user, int channel, const char *level, const char *message) {
    if (!message || !message[0]) return;
    char prefixed[768];
    snprintf(prefixed, sizeof(prefixed), "Direct RAW channel %c: %s",
             channel == 0 ? 'A' : 'B', message);
    gui_error_class_t error_class = gui_record_level_is_error(level)
                                    ? GUI_ERROR_CLASS_SYSTEM : GUI_ERROR_CLASS_NONE;
    gui_record_log_capture_event((gui_app_t *)user, level ? level : "INFO",
                                 prefixed, error_class, 1);
}

// Capture-thread entry point (called from gui_cxadc.c after each card read).
// Drop handling mirrors the extraction thread's record-drop path: capture
// log entry + optional stop_on_dropout.
static void gui_record_direct_report_drop(int card, uint32_t frame_index, size_t len) {
    gui_app_t *app = s_recording_app;
    if (!app) return;
    char msg[320];
    snprintf(msg, sizeof(msg),
             "Direct record data loss on channel %c at frame=%u (%zu bytes: record buffer full and spill enqueue failed)",
             card == 0 ? 'A' : 'B', frame_index, len);
    gui_record_log_capture_event(app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
    fprintf(stderr, "[DIRECT] %s\n", msg);
#if defined(__APPLE__)
    if (gui_cxadc_macos_active()) gui_cxadc_macos_mark_failed();
#endif
    if ((app->settings.stop_on_dropout
#if defined(__APPLE__)
         || gui_cxadc_macos_active()
#endif
        ) && !atomic_load(&app->dropout_stop_requested)) {
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_BACKPRESSURE);
        atomic_store(&app->dropout_stop_requested, true);
    }
}

void gui_record_direct_tap_push(int card, const uint8_t *bytes, size_t len, uint32_t frame_index) {
    if (card != 0 && card != 1) return;
    gui_record_direct_ctx_t *ctx = (card == 0) ? &s_direct_ctx_a : &s_direct_ctx_b;
    gui_record_direct_push_result_t result = gui_record_direct_push(ctx, bytes, len, frame_index);
    if (result != GUI_RECORD_DIRECT_PUSH_DROPPED) return;
    gui_record_direct_report_drop(card, frame_index, len);
}

bool gui_record_direct_tap_channel_active(int channel) {
    if (channel != 0 && channel != 1) return false;
    return (channel == 0) ? s_direct_channel_a_active : s_direct_channel_b_active;
}

bool gui_record_tap_iteration_enter(int channel) {
    if (channel != 0 && channel != 1) return false;
    gui_record_direct_ctx_t *ctx = (channel == 0) ? &s_direct_ctx_a : &s_direct_ctx_b;
    return gui_record_direct_tap_iteration_enter(ctx);
}

void gui_record_tap_iteration_leave(int channel) {
    if (channel != 0 && channel != 1) return;
    gui_record_direct_ctx_t *ctx = (channel == 0) ? &s_direct_ctx_a : &s_direct_ctx_b;
    gui_record_direct_tap_iteration_leave(ctx);
}

void gui_record_direct_tap_push_latched(int channel, bool latched_on,
                                         const uint8_t *bytes, size_t len,
                                         uint32_t frame_index) {
    if (channel != 0 && channel != 1) return;
    gui_record_direct_ctx_t *ctx = (channel == 0) ? &s_direct_ctx_a : &s_direct_ctx_b;
    gui_record_direct_push_result_t result =
        gui_record_direct_push_latched(ctx, latched_on, bytes, len, frame_index);
    if (result != GUI_RECORD_DIRECT_PUSH_DROPPED) return;
    gui_record_direct_report_drop(channel, frame_index, len);
}

static void format_msps_from_khz(char *dst, size_t dst_len, float khz) {
    if (!dst || dst_len == 0) return;
    double msps = (double)khz / 1000.0;
    // Render without trailing .0 when possible.
    if (fabs(msps - (double)((int)msps)) < 1e-6) {
        snprintf(dst, dst_len, "%dmsps", (int)msps);
    } else {
        snprintf(dst, dst_len, "%.1fmsps", msps);
    }
}

static void sanitize_tag(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    size_t i = 0, j = 0;
    for (; src[i] && j + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        // Preserve non-ASCII filename bytes; map spaces to '-'.
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c >= 0x80) {
            dst[j++] = (char)c;
        } else if (c == ' ' || c == '\t') {
            dst[j++] = '-';
        }
    }
    // Do not split a UTF-8 sequence when shortening the capture-log base.
    if (src[i] && ((unsigned char)src[i] & 0xc0) == 0x80) {
        while (j > 0 && ((unsigned char)dst[j - 1] & 0xc0) == 0x80) j--;
        if (j > 0 && (unsigned char)dst[j - 1] >= 0xc0) j--;
    }
    while (j > 0 && dst[j - 1] == '-') j--;
    dst[j] = '\0';
}

static bool gui_record_cvbs_preview_enabled_locked(const channel_panel_config_t *config) {
    if (!config) return false;
    return (config->left_view == PANEL_VIEW_CVBS) ||
           (config->split && config->right_view == PANEL_VIEW_CVBS);
}

static bool gui_record_same_filename(const char *a, const char *b);

static FILE *gui_record_create_session_log(const gui_app_t *app,
    const gui_record_output_targets_t *targets, const char *base, const char *timestamp)
{
    static unsigned remote_log_sequence;
    bool remote = gui_monitor_local_save_recording();
    unsigned sequence = remote ? ++remote_log_sequence : 0;
    for (unsigned attempt = 0; attempt < 1000; ++attempt, ++sequence) {
        char name[MAX_FILENAME_LEN];
        int length = sequence
            ? snprintf(name, sizeof(name), "%s_%s_misrc_capture-%u.log", base, timestamp, sequence)
            : snprintf(name, sizeof(name), "%s_%s_misrc_capture.log", base, timestamp);
        if (length < 0 || (size_t)length >= sizeof(name)) return NULL;

        bool collision = false;
        for (size_t i = 0; i < GUI_RECORD_OUTPUT_COUNT; ++i) {
            const char *filename = strrchr(targets->paths[i], '/');
            if (filename && gui_record_same_filename(filename + 1, name)) {
                collision = true;
                break;
            }
        }
        if (collision) continue;

        length = snprintf(s_capture_log_path, sizeof(s_capture_log_path), "%s/%s",
                          app->settings.output_path, name);
        if (length < 0 || (size_t)length >= sizeof(s_capture_log_path)) return NULL;
        if (remote) remote_log_sequence = sequence;
        FILE *file = file_open_utf8(s_capture_log_path, "wx");
        if (file || remote || errno != EEXIST) return file;
    }
    return NULL;
}

static void gui_record_open_session_log(gui_app_t *app, const gui_record_output_targets_t *targets) {
    const char *path_a = targets->paths[0];
    const char *path_b = targets->paths[1];
    if (!app) return;

    gui_record_close_session_log();

    const char *base_src = app->settings.output_base_name[0] ? app->settings.output_base_name : "capture";
    char base_name[128];
    sanitize_tag(base_name, sizeof(base_name), base_src);
    if (!base_name[0]) {
        snprintf(base_name, sizeof(base_name), "%s", "capture");
    }

    char date_tag[32];
    date_tag[0] = '\0';
    bool have_date_tag = false;
    if (path_a && path_a[0]) {
        have_date_tag = gui_record_extract_timestamp_token(path_a, date_tag, sizeof(date_tag));
    }
    if (!have_date_tag && app->settings.capture_b && path_b && path_b[0]) {
        have_date_tag = gui_record_extract_timestamp_token(path_b, date_tag, sizeof(date_tag));
    }
    if (!have_date_tag && app->capture_timestamp[0]) {
        snprintf(date_tag, sizeof(date_tag), "%s", app->capture_timestamp);
    }
    if (!date_tag[0]) {
        gui_record_build_system_timestamp(date_tag, sizeof(date_tag));
    }
    if (!date_tag[0]) {
        snprintf(date_tag, sizeof(date_tag), "%s", "session");
    }

    bool cvbs_preview_a = false;
    bool cvbs_preview_b = false;
    while (atomic_flag_test_and_set(&app->panel_config_lock)) {}
    cvbs_preview_a = gui_record_cvbs_preview_enabled_locked(&app->panel_config_a);
    cvbs_preview_b = gui_record_cvbs_preview_enabled_locked(&app->panel_config_b);
    atomic_flag_clear(&app->panel_config_lock);

    gui_record_log_lock();
    s_capture_log_file = gui_record_create_session_log(app, targets, base_name, date_tag);
    if (!s_capture_log_file) {
        s_capture_log_path[0] = '\0';
        gui_record_log_unlock();
        return;
    }

    char msg[1024];
    char iso_ts[32];
    char host_name[128];
    char user_name[128];
    char os_name[256];
    char cpu_model[256];
    gui_record_build_iso8601_timestamp(iso_ts, sizeof(iso_ts));
    gui_record_get_host_name(host_name, sizeof(host_name));
    gui_record_get_user_name(user_name, sizeof(user_name));
    gui_record_get_os_string(os_name, sizeof(os_name));
    gui_record_get_cpu_model(cpu_model, sizeof(cpu_model));
    uint32_t cpu_cores = gui_record_get_cpu_core_count();

    const char *device_name = "unknown";
    if (app->selected_device >= 0 && app->selected_device < app->device_count) {
        device_name = app->devices[app->selected_device].name;
    }

    snprintf(msg, sizeof(msg), "MISRC capture log started (%s)", app->settings.use_flac ? "FLAC" : "RAW");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "computer_name: %s", host_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "computer_model_name: %s", cpu_model);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "computer_cores: %u", (unsigned)cpu_cores);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "user_name: %s", user_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "operating_system_VERSION: %s", os_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "misrc_tools_version: %s", MIRSC_TOOLS_VERSION);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "datetime_start: %s", iso_ts[0] ? iso_ts : "unknown");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_log_path: %s", s_capture_log_path);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_base_name: %s", base_src);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "output_path: %s", app->settings.output_path);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_device_name: %s", device_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_device_type: %s", gui_record_device_type_name(app));
    gui_record_log_write_line_locked("INFO", msg);
    const bool hifi = targets->rtl_hifi;
    bool direct_raw_a = false;
    bool direct_raw_b = false;
    if (!app->settings.use_flac) {
        direct_raw_a = gui_record_direct_channel_eligible(app, 0);
        direct_raw_b = gui_record_direct_channel_eligible(app, 1);
    }
    snprintf(msg, sizeof(msg), "capture_format: %s",
             hifi ? (targets->rtl_hifi_flac ? "FLAC (RTL-SDR Hi-Fi RF)" : "RAW (RTL-SDR Hi-Fi RF)")
             : app->settings.use_flac ? "FLAC"
             : ((direct_raw_a || direct_raw_b) ? "RAW direct native passthrough" : "RAW converted"));
    gui_record_log_write_line_locked("INFO", msg);

    snprintf(msg, sizeof(msg), "Capture channels: A=%s B=%s", path_a && path_a[0] ? "on" : "off", path_b && path_b[0] ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "CVBS preview state: A=%s B=%s",
             cvbs_preview_a ? "on" : "off",
             cvbs_preview_b ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);

    uint8_t bits_a = hifi ? 16 : app->settings.use_flac ? clamp_rf_bits_flac(app->settings.rf_bits_a) : rf_bits_for_raw(app->settings.rf_bits_a);
    uint8_t bits_b = app->settings.use_flac ? clamp_rf_bits_flac(app->settings.rf_bits_b) : rf_bits_for_raw(app->settings.rf_bits_b);
    snprintf(msg, sizeof(msg), "RF settings: bitsA=%u bitsB=%u resampleA=%s(%.1f kHz) resampleB=%s(%.1f kHz)",
             (unsigned)bits_a, (unsigned)bits_b,
             !hifi && app->settings.enable_resample_a ? "on" : "off", app->settings.resample_rate_a,
             !hifi && app->settings.enable_resample_b ? "on" : "off", app->settings.resample_rate_b);
    gui_record_log_write_line_locked("INFO", msg);
    if (!hifi && !app->settings.use_flac) {
        char raw_a[40];
        char raw_b[40];
        if (!app->settings.capture_a) {
            snprintf(raw_a, sizeof(raw_a), "off");
        } else {
            snprintf(raw_a, sizeof(raw_a), "%s %s",
                     direct_raw_a ? "direct native" : "converted",
                     raw_ext_for_bits(bits_a, direct_raw_a));
        }
        if (!app->settings.capture_b) {
            snprintf(raw_b, sizeof(raw_b), "off");
        } else {
            snprintf(raw_b, sizeof(raw_b), "%s %s",
                     direct_raw_b ? "direct native" : "converted",
                     raw_ext_for_bits(bits_b, direct_raw_b));
        }
        snprintf(msg, sizeof(msg), "RAW output: A=%s B=%s", raw_a, raw_b);
        gui_record_log_write_line_locked("INFO", msg);
    }
    snprintf(msg, sizeof(msg), "Capture limits: capture_limit_seconds=%u record_limit_seconds=%u (%s)",
             (unsigned)app->settings.capture_limit_seconds,
             (unsigned)app->settings.record_limit_seconds,
             app->settings.record_limit_seconds > 0 ? "record_timer_armed" : "record_timer_disarmed");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Audio monitor: playback=%s monitor_ch34=%s misrc_mode=%s",
             app->settings.audio_monitor_playback ? "on" : "off",
             app->settings.audio_monitor_ch34 ? "on" : "off",
             app->user_capture_mode_misrc ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "MISRC V1.5/V2.5 A/B swap override: %s",
             app->settings.misrc_v15_v25_ab_swap ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Dropout handling: stop_on_dropout=%s",
             app->settings.stop_on_dropout ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata project: %s",
             app->settings.ingest_project[0] ? app->settings.ingest_project : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_id: %s",
             app->settings.ingest_tape_id[0] ? app->settings.ingest_tape_id : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_format: %s",
             app->settings.ingest_tape_format[0] ? app->settings.ingest_tape_format : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_size: %s",
             app->settings.ingest_tape_size[0] ? app->settings.ingest_tape_size : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_speed: %s",
             app->settings.ingest_tape_speed[0] ? app->settings.ingest_tape_speed : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_condition: %s",
             app->settings.ingest_tape_condition[0] ? app->settings.ingest_tape_condition : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata operator: %s",
             app->settings.ingest_operator[0] ? app->settings.ingest_operator : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata location: %s",
             app->settings.ingest_location[0] ? app->settings.ingest_location : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata notes: %s",
             app->settings.ingest_notes[0] ? app->settings.ingest_notes : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);

    if (hifi) {
        snprintf(msg, sizeof(msg), "RTL-SDR Hi-Fi RF: source_rate=%" PRIu32 " Hz center=%" PRIu32
                 " Hz output_rate=%" PRIu32 " Hz signed 16-bit little-endian mono",
                 s_rtlsdr_hifi_source_rate, (uint32_t)s_rtlsdr_hifi_tuning, (uint32_t)GUI_SDR_RF_OUTPUT_RATE_HZ);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (hifi ? targets->rtl_hifi_flac : app->settings.use_flac) {
        snprintf(msg, sizeof(msg), "FLAC settings: level=%d verify=%s threads=%d",
                 app->settings.flac_level,
                 app->settings.flac_verification ? "on" : "off",
                 app->settings.flac_threads);
        gui_record_log_write_line_locked("INFO", msg);
        snprintf(msg, sizeof(msg), "FLAC affinity: enabled=%s cpu_list=%s support=%s",
                 app->settings.flac_affinity_enabled ? "on" : "off",
                 app->settings.flac_affinity_cpu_list[0] ? app->settings.flac_affinity_cpu_list : "(none)",
                 flac_writer_affinity_supported() ? "linux" : "unsupported");
        gui_record_log_write_line_locked("INFO", msg);
    }

    if (path_a && path_a[0]) {
        snprintf(msg, sizeof(msg), "FILE_PATH_A: %s", path_a);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (path_b && path_b[0]) {
        snprintf(msg, sizeof(msg), "FILE_PATH_B: %s", path_b);
        gui_record_log_write_line_locked("INFO", msg);
    }

    snprintf(msg, sizeof(msg), "Audio outputs: 4ch=%s 2ch12=%s 2ch34=%s",
             !hifi && app->settings.enable_audio_4ch ? "on" : "off",
             !hifi && app->settings.enable_audio_2ch_12 ? "on" : "off",
             !hifi && app->settings.enable_audio_2ch_34 ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);

    if (!hifi && app->settings.enable_audio_4ch && app->settings.audio_4ch_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_4CH_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_4ch_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (!hifi && app->settings.enable_audio_2ch_12 && app->settings.audio_2ch_12_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_2CH_12_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_2ch_12_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (!hifi && app->settings.enable_audio_2ch_34 && app->settings.audio_2ch_34_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_2CH_34_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_2ch_34_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    for (int i = 0; i < 4; i++) {
        if (!hifi && app->settings.enable_audio_1ch[i] && app->settings.audio_1ch_filenames[i][0]) {
            snprintf(msg, sizeof(msg), "AUDIO_1CH_%d_FILE_PATH: %s/%s",
                     i + 1, app->settings.output_path, app->settings.audio_1ch_filenames[i]);
            gui_record_log_write_line_locked("INFO", msg);
        }
    }

    gui_record_log_unlock();
}

static bool gui_record_format_name(char *dst, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(dst, MAX_FILENAME_LEN, format, args);
    va_end(args);
    if (written < 0 || written >= MAX_FILENAME_LEN) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

static bool gui_record_rtlsdr_hifi_selected(const gui_app_t *app) {
#ifdef ENABLE_RTLSDR
    return app->settings.rtlsdr_hifi_rf_record && app->settings.net_mode != GUI_NET_MODE_CLIENT &&
           app->selected_device >= 0 && app->selected_device < app->device_count &&
           app->devices[app->selected_device].type == DEVICE_TYPE_RTLSDR;
#else
    (void)app;
    return false;
#endif
}

static bool gui_record_rtlsdr_hifi_source(gui_app_t *app, uint32_t *rate, uint64_t *tuning) {
#if defined(ENABLE_RTLSDR) && LIBSOXR_ENABLED
#if LIBFLAC_ENABLED != 1
    if (app->settings.use_flac) {
        gui_app_set_status(app, "Hi-Fi RF FLAC requires FLAC support; disable RF FLAC compression for RAW");
        return false;
    }
#endif
    if (gui_monitor_local_save_recording()) {
        gui_app_set_status(app, "Hi-Fi RF requires a local recording destination");
        return false;
    }
    if (!gui_rtlsdr_get_rf_source(app, rate, tuning)) {
        gui_app_set_status(app, gui_rtlsdr_is_running(app)
            ? "RTL-SDR rate or tuning is not ready for Hi-Fi RF recording"
            : "Hi-Fi RF requires a running RTL-SDR");
        return false;
    }
    const uint32_t center_hz = (uint32_t)*tuning;
    char error[128];
    if (!gui_sdr_rf_validate(*rate, center_hz, error, sizeof(error))) {
        gui_app_set_status(app, error);
        return false;
    }
    const double passband_hz = gui_sdr_rf_passband_hz(*rate);
    if (!gui_vhs_hifi_iq_band_fits_passband(GUI_RF_NTSC, *rate, center_hz, passband_hz) &&
        !gui_vhs_hifi_iq_band_fits_passband(GUI_RF_PAL, *rate, center_hz, passband_hz)) {
        gui_app_set_status(app, "Hi-Fi RF bandwidth misses a VHS carrier band; adjust RTL-SDR center frequency or sample rate");
        return false;
    }
    return true;
#else
    (void)rate;
    (void)tuning;
    gui_app_set_status(app, "Hi-Fi RF requires RTL-SDR and SoXR support");
    return false;
#endif
}

static bool gui_record_apply_auto_names(gui_app_t *app) {
    if (!app) return false;
    bool hifi = gui_record_rtlsdr_hifi_selected(app);
    if (!app->settings.auto_names_enabled && !hifi) return true;

    const char *base = app->settings.output_base_name[0] ? app->settings.output_base_name : "capture";

    // Optionally append system timestamp sampled at record-start.
    // This does not mutate output_base_name.
    char base_with_ts[MAX_FILENAME_LEN + 32];
    if (app->settings.auto_names_enabled && app->settings.append_timestamp_on_capture_start) {
        char timestamp_now[32];
        gui_record_build_system_timestamp(timestamp_now, sizeof(timestamp_now));
        if (timestamp_now[0]) {
            snprintf(base_with_ts, sizeof(base_with_ts), "%s_%s", base, timestamp_now);
            base = base_with_ts;
        }
    }

    if (hifi) {
        char tag[sizeof(app->settings.rtlsdr_hifi_rf_tag)];
        sanitize_tag(tag, sizeof(tag), app->settings.rtlsdr_hifi_rf_tag);
        const uint32_t khz = GUI_SDR_RF_OUTPUT_RATE_HZ / 1000u;
        return gui_record_format_name(s_rtlsdr_hifi_filename, "%s%s%s_hifi_rf_16-bit_%" PRIu32 ".%03" PRIu32 "msps.%s",
                                      base, tag[0] ? "_" : "", tag, khz / 1000u, khz % 1000u,
                                      app->settings.use_flac ? "flac" : "s16");
    }

    // RF filenames
    if (app->settings.use_flac) {
        uint8_t bits_a = clamp_rf_bits_flac(app->settings.rf_bits_a);
        uint8_t bits_b = clamp_rf_bits_flac(app->settings.rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (app->settings.enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), app->settings.resample_rate_a);
        if (app->settings.enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), app->settings.resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), app->settings.rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), app->settings.rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            if (!gui_record_format_name(app->settings.output_filename_a, "%s_%s_%u-bit_%s.flac", base, rf_tag_a, (unsigned)bits_a, rate_tag_a) && app->settings.capture_a) return false;
        } else if (rf_tag_a[0]) {
            if (!gui_record_format_name(app->settings.output_filename_a, "%s_%s_%u-bit.flac", base, rf_tag_a, (unsigned)bits_a) && app->settings.capture_a) return false;
        } else if (rate_tag_a[0]) {
            if (!gui_record_format_name(app->settings.output_filename_a, "rfA_%s_%u-bit_%s.flac", base, (unsigned)bits_a, rate_tag_a) && app->settings.capture_a) return false;
        } else {
            if (!gui_record_format_name(app->settings.output_filename_a, "rfA_%s_%u-bit.flac", base, (unsigned)bits_a) && app->settings.capture_a) return false;
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            if (!gui_record_format_name(app->settings.output_filename_b, "%s_%s_%u-bit_%s.flac", base, rf_tag_b, (unsigned)bits_b, rate_tag_b) && app->settings.capture_b) return false;
        } else if (rf_tag_b[0]) {
            if (!gui_record_format_name(app->settings.output_filename_b, "%s_%s_%u-bit.flac", base, rf_tag_b, (unsigned)bits_b) && app->settings.capture_b) return false;
        } else if (rate_tag_b[0]) {
            if (!gui_record_format_name(app->settings.output_filename_b, "rfB_%s_%u-bit_%s.flac", base, (unsigned)bits_b, rate_tag_b) && app->settings.capture_b) return false;
        } else {
            if (!gui_record_format_name(app->settings.output_filename_b, "rfB_%s_%u-bit.flac", base, (unsigned)bits_b) && app->settings.capture_b) return false;
        }
    } else {
        // RAW: 8/16 only. Extension is per-channel and named by content:
        // direct native passthrough (CXADC + no resample on the channel)
        // stores the card's unsigned bytes (.u8/.u16); everything else is
        // the converting writer's signed samples (.s8/.s16). Same predicate
        // as record start, so the name always matches the on-disk content.
        bool direct_a = gui_record_direct_channel_eligible(app, 0);
        bool direct_b = gui_record_direct_channel_eligible(app, 1);
        uint8_t bits_a = rf_bits_for_raw(app->settings.rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(app->settings.rf_bits_b);
        const char *ext_a = raw_ext_for_bits(bits_a, direct_a);
        const char *ext_b = raw_ext_for_bits(bits_b, direct_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (app->settings.enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), app->settings.resample_rate_a);
        if (app->settings.enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), app->settings.resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), app->settings.rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), app->settings.rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            if (!gui_record_format_name(app->settings.output_filename_a, "%s_%s_%u-bit_%s.%s", base, rf_tag_a, (unsigned)bits_a, rate_tag_a, ext_a) && app->settings.capture_a) return false;
        } else if (rf_tag_a[0]) {
            if (!gui_record_format_name(app->settings.output_filename_a, "%s_%s_%u-bit.%s", base, rf_tag_a, (unsigned)bits_a, ext_a) && app->settings.capture_a) return false;
        } else if (rate_tag_a[0]) {
            if (!gui_record_format_name(app->settings.output_filename_a, "rfA_%s_%u-bit_%s.%s", base, (unsigned)bits_a, rate_tag_a, ext_a) && app->settings.capture_a) return false;
        } else {
            if (!gui_record_format_name(app->settings.output_filename_a, "rfA_%s_%u-bit.%s", base, (unsigned)bits_a, ext_a) && app->settings.capture_a) return false;
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            if (!gui_record_format_name(app->settings.output_filename_b, "%s_%s_%u-bit_%s.%s", base, rf_tag_b, (unsigned)bits_b, rate_tag_b, ext_b) && app->settings.capture_b) return false;
        } else if (rf_tag_b[0]) {
            if (!gui_record_format_name(app->settings.output_filename_b, "%s_%s_%u-bit.%s", base, rf_tag_b, (unsigned)bits_b, ext_b) && app->settings.capture_b) return false;
        } else if (rate_tag_b[0]) {
            if (!gui_record_format_name(app->settings.output_filename_b, "rfB_%s_%u-bit_%s.%s", base, (unsigned)bits_b, rate_tag_b, ext_b) && app->settings.capture_b) return false;
        } else {
            if (!gui_record_format_name(app->settings.output_filename_b, "rfB_%s_%u-bit.%s", base, (unsigned)bits_b, ext_b) && app->settings.capture_b) return false;
        }
    }

    // Audio filenames (WAV)
    char audio_tag_4ch[40] = {0};
    char audio_tag_12[40] = {0};
    char audio_tag_34[40] = {0};
    sanitize_tag(audio_tag_4ch, sizeof(audio_tag_4ch), app->settings.audio_output_tags[0]);
    sanitize_tag(audio_tag_12, sizeof(audio_tag_12), app->settings.audio_output_tags[1]);
    sanitize_tag(audio_tag_34, sizeof(audio_tag_34), app->settings.audio_output_tags[2]);

    if (audio_tag_4ch[0]) {
        if (!gui_record_format_name(app->settings.audio_4ch_filename, "%s_%s_quad_4ch.wav", base, audio_tag_4ch) && app->settings.enable_audio_4ch) return false;
    } else {
        if (!gui_record_format_name(app->settings.audio_4ch_filename, "%s_quad_4ch.wav", base) && app->settings.enable_audio_4ch) return false;
    }
    if (audio_tag_12[0]) {
        if (!gui_record_format_name(app->settings.audio_2ch_12_filename, "%s_%s_stereo_ch1_ch2.wav", base, audio_tag_12) && app->settings.enable_audio_2ch_12) return false;
    } else {
        if (!gui_record_format_name(app->settings.audio_2ch_12_filename, "%s_stereo_ch1_ch2.wav", base) && app->settings.enable_audio_2ch_12) return false;
    }
    if (audio_tag_34[0]) {
        if (!gui_record_format_name(app->settings.audio_2ch_34_filename, "%s_%s_stereo_ch3_ch4.wav", base, audio_tag_34) && app->settings.enable_audio_2ch_34) return false;
    } else {
        if (!gui_record_format_name(app->settings.audio_2ch_34_filename, "%s_stereo_ch3_ch4.wav", base) && app->settings.enable_audio_2ch_34) return false;
    }

    for (int i = 0; i < 4; i++) {
        char tag[40];
        sanitize_tag(tag, sizeof(tag), app->settings.audio_1ch_labels[i]);
        if (tag[0]) {
            if (!gui_record_format_name(app->settings.audio_1ch_filenames[i], "%s_%s_audio_ch%d.wav", base, tag, i + 1) && app->settings.enable_audio_1ch[i]) return false;
        } else {
            if (!gui_record_format_name(app->settings.audio_1ch_filenames[i], "%s_audio_ch%d.wav", base, i + 1) && app->settings.enable_audio_1ch[i]) return false;
        }
    }
    return true;
}

// All recording outputs share one directory; require distinct filename components.
static bool gui_record_valid_filename(const char *name) {
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        strchr(name, '/')) return false;
#if defined(_WIN32) || defined(_WIN64)
    size_t len = strlen(name);
    if (name[len - 1] == '.' || name[len - 1] == ' ') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 32 || strchr("\\:*?\"<>|", *p)) return false;
    }
    wchar_t wide[MAX_FILENAME_LEN];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1,
                             wide, MAX_FILENAME_LEN)) return false;
#endif
    return true;
}

static bool gui_record_same_filename(const char *a, const char *b) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wa[MAX_FILENAME_LEN], wb[MAX_FILENAME_LEN];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a, -1, wa, MAX_FILENAME_LEN) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b, -1, wb, MAX_FILENAME_LEN)) return false;
    return CompareStringOrdinal(wa, -1, wb, -1, TRUE) == CSTR_EQUAL;
#else
    return strcmp(a, b) == 0;
#endif
}

static bool gui_record_check_output_paths(gui_app_t *app, unsigned *existing_count,
                                          char *message, size_t message_size,
                                          gui_record_output_targets_t *targets) {
    bool hifi = gui_record_rtlsdr_hifi_selected(app);
    const char *names[GUI_RECORD_OUTPUT_COUNT] = {
        hifi ? s_rtlsdr_hifi_filename : app->settings.output_filename_a, app->settings.output_filename_b,
        app->settings.audio_4ch_filename, app->settings.audio_2ch_12_filename,
        app->settings.audio_2ch_34_filename, app->settings.audio_1ch_filenames[0],
        app->settings.audio_1ch_filenames[1], app->settings.audio_1ch_filenames[2],
        app->settings.audio_1ch_filenames[3]
    };
    const bool enabled[GUI_RECORD_OUTPUT_COUNT] = {
        hifi || app->settings.capture_a, !hifi && app->settings.capture_b,
        !hifi && app->settings.enable_audio_4ch, !hifi && app->settings.enable_audio_2ch_12,
        !hifi && app->settings.enable_audio_2ch_34, !hifi && app->settings.enable_audio_1ch[0],
        !hifi && app->settings.enable_audio_1ch[1], !hifi && app->settings.enable_audio_1ch[2],
        !hifi && app->settings.enable_audio_1ch[3]
    };
    const char *labels[GUI_RECORD_OUTPUT_COUNT] = { "RF A", "RF B", "Audio quad", "Audio 1/2", "Audio 3/4",
                             "Audio 1", "Audio 2", "Audio 3", "Audio 4" };
    bool directory = false;
    if (!gui_monitor_local_save_recording() && (!file_path_info_utf8(app->settings.output_path, NULL, &directory) || !directory)) {
        gui_app_set_status(app, "Output directory does not exist or cannot be read");
        return false;
    }
    unsigned count = 0;
    if (targets) {
        memset(targets, 0, sizeof(*targets));
        targets->rtl_hifi = hifi;
        targets->rtl_hifi_flac = hifi && app->settings.use_flac;
    }
    if (message && message_size) message[0] = '\0';
    for (size_t i = 0; i < GUI_RECORD_OUTPUT_COUNT; i++) {
        if (!enabled[i]) continue;
        if (!gui_record_valid_filename(names[i])) {
            gui_app_set_status(app, "Output filename is empty or invalid");
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (enabled[j] && gui_record_same_filename(names[i], names[j])) {
                gui_app_set_status(app, "Recording outputs must have different filenames");
                return false;
            }
        }
        char path[GUI_RECORD_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, names[i]);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            gui_app_set_status(app, "Output path is too long");
            return false;
        }
        if (targets) memcpy(targets->paths[i], path, (size_t)written + 1);
        uint64_t bytes = 0;
        if (!gui_monitor_local_save_recording() && file_path_info_utf8(path, &bytes, &directory)) {
            if (directory) {
                gui_app_set_status(app, "Output filename refers to a directory");
                return false;
            }
            count++;
            if (targets) targets->existing_mask |= 1u << i;
            if (message && message_size) {
                char size_buf[32];
                format_file_size_u64(bytes, size_buf, sizeof(size_buf));
                size_t offset = strlen(message);
                // snprintf returns the required size, not the bytes appended.
                // Derive each next offset from the bounded destination instead.
                snprintf(message + offset, message_size - offset,
                         "%s: %s (%s)\n", labels[i], names[i], size_buf);
            }
        }
    }
    if (existing_count) *existing_count = count;
    return true;
}

static bool gui_record_match_output_targets(gui_app_t *app,
                                            const gui_record_output_targets_t *confirmed,
                                            gui_record_output_targets_t *current) {
    if (!gui_record_check_output_paths(app, NULL, NULL, 0, current)) return false;
    if (!confirmed || (current->existing_mask == confirmed->existing_mask &&
        current->rtl_hifi == confirmed->rtl_hifi &&
        current->rtl_hifi_flac == confirmed->rtl_hifi_flac &&
        memcmp(current->paths, confirmed->paths, sizeof(current->paths)) == 0))
        return true;
    gui_app_set_status(app, "Recording outputs changed. Start recording again to confirm.");
    return false;
}

static bool gui_record_capture_ready(const gui_app_t *app) {
    if (!app || gui_net_transition_busy()) return false;
#ifdef __ANDROID__
    if (gui_app_capture_busy() || gui_net_capture_controls_locked(app)) return false;
#endif
    if (app->settings.net_mode == GUI_NET_MODE_CLIENT &&
        (!atomic_load(&app->net_rf_client_stream_live) ||
         !gui_net_client_peer_capturing(app))) return false;
    return app->is_capturing;
}

// Start recording - checks for file existence first
int gui_record_start(gui_app_t *app) {
#if defined(__APPLE__)
    if(gui_cxadc_macos_active() && (app->settings.use_flac || app->settings.enable_resample_a ||
        app->settings.rf_bits_a!=8 || app->settings.capture_b || app->settings.enable_audio_4ch || app->settings.enable_audio_2ch_12 ||
        app->settings.enable_audio_2ch_34 || app->settings.enable_audio_1ch[0] || app->settings.enable_audio_1ch[1] ||
        app->settings.enable_audio_1ch[2] || app->settings.enable_audio_1ch[3])) {
        gui_app_set_status(app,"macOS CXADC recording requires RAW8 channel A, no resampling or audio");return -1;
    }
#endif

    (void)gui_record_collect_finalize_if_done();

    if (atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running)) {
        gui_app_set_status(app, "Finalizing previous recording...");
        return RECORD_ERROR;
    }

    if (!gui_record_capture_ready(app)) {
        gui_app_set_status(app, "Start capture first");
        return RECORD_ERROR;
    }

    if (app->is_recording) {
        return RECORD_OK;
    }

    // If already pending confirmation, don't show another popup
    if (s_overwrite_pending) {
        return RECORD_PENDING;
    }

    uint32_t source_rate;
    uint64_t tuning;
    if (gui_record_rtlsdr_hifi_selected(app) && !gui_record_rtlsdr_hifi_source(app, &source_rate, &tuning))
        return RECORD_ERROR;

    // Apply auto naming before any file can be opened or overwritten.
    if (!gui_record_apply_auto_names(app)) {
        gui_app_set_status(app, "Generated filename is too long; shorten the base name or tags");
        return RECORD_ERROR;
    }

    unsigned existing_count = 0;
    char details[384];
    if (!gui_record_check_output_paths(app, &existing_count, details, sizeof(details),
                                       &s_pending_output_targets)) {
        return RECORD_ERROR;
    }
    if (existing_count && !app->settings.overwrite_files) {
        /* Headless Server never enters the GUI frame loop which consumes
         * overwrite popup results.  Do not leave this request pending forever,
         * and never overwrite files without explicit permission. */
        if (app->headless_server) {
            gui_app_set_status(app, "Recording blocked: output file exists; use a unique name or enable overwrite");
            fprintf(stderr,
                    "[RECORD] Headless recording refused: %u output file(s) already exist. "
                    "Set a new output base name, enable timestamped names, or explicitly enable overwrite_files.\n",
                    existing_count);
            return RECORD_ERROR;
        }
        char message[512];
        snprintf(message, sizeof(message),
                 "%u existing output file(s) will be overwritten:\n\n%s",
                 existing_count, details);
        gui_popup_confirm("Overwrite Files?", message, "Overwrite", "Cancel", &s_overwrite_pending);
        s_overwrite_pending = true;
        s_pending_app = app;
        s_pending_capture_start = app->capture_start_time;
        s_pending_network_mode = app->settings.net_mode;
        return RECORD_PENDING;
    }

    // No files exist, start recording directly
    int native_record_result = gui_record_start_confirmed(app,
        app->settings.overwrite_files ? NULL : &s_pending_output_targets);
#if defined(__APPLE__)
    if(native_record_result==0)gui_cxadc_macos_log(app);
#endif
    return native_record_result;
}

void gui_record_cancel_capture_confirmation(gui_app_t *app) {
    if (!s_overwrite_pending || s_pending_app != app) return;
    if (gui_popup_get_user_data() == &s_overwrite_pending) gui_popup_dismiss();
    s_overwrite_pending = false;
    s_pending_app = NULL;
}

// Check popup result and continue recording if confirmed
void gui_record_check_popup(gui_app_t *app) {
    if (gui_record_collect_finalize_if_done()) {
        gui_app_set_status(app, gui_record_stop_status());
    }

    if (app->is_recording && gui_record_rtlsdr_active() &&
        gui_record_check_disk_space_guard(app, 0, NULL, 0)) {
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_DISK_SPACE);
        atomic_store(&app->dropout_stop_requested, true);
        gui_record_stop(app);
    }
    if (app->is_recording && atomic_load(&s_rtlsdr_hifi_interrupted)) {
        gui_record_log_capture_event(app, "ERROR", "RTL-SDR Hi-Fi RF input interrupted",
                                     GUI_ERROR_CLASS_SYSTEM, 1);
        gui_record_stop(app);
    }
    if (!s_overwrite_pending) {
        return;
    }

    if (s_pending_app != app || gui_popup_get_user_data() != &s_overwrite_pending) {
        gui_record_cancel_capture_confirmation(s_pending_app);
        return;
    }
    if (!gui_record_capture_ready(app) ||
        app->capture_start_time != s_pending_capture_start ||
        app->settings.net_mode != s_pending_network_mode) {
        gui_record_cancel_capture_confirmation(app);
        return;
    }

    popup_result_t result = gui_popup_get_result();

    if (result == POPUP_RESULT_NONE) {
        // Popup still open, wait
        return;
    }

    // Popup closed, clear pending state
    s_overwrite_pending = false;

    if (result == POPUP_RESULT_YES) {
        // User confirmed, start recording
        int start_result = gui_record_start_confirmed(app, &s_pending_output_targets);
#if defined(__APPLE__)
        if (start_result == RECORD_OK) gui_cxadc_macos_log(app);
#else
        (void)start_result;
#endif
    } else {
        // User cancelled
        gui_app_set_status(app, "Recording cancelled");
    }

    s_pending_app = NULL;
}

// Internal: Start recording after confirmation
#if LIBFLAC_ENABLED == 1
static flac_writer_config_t gui_record_flac_config(const gui_app_t *app) {
    flac_writer_config_t config = flac_writer_default_config();
    config.bits_per_sample = 16;
    config.compression_level = app->settings.flac_level;
    config.verify = app->settings.flac_verification;
    // Auto resolves to parallel encoding so level 8 keeps up without relying on spill.
    config.num_threads = gui_record_resolve_flac_threads(app->settings.flac_threads);
    config.affinity_enabled = app->settings.flac_affinity_enabled;
    snprintf(config.affinity_cpu_list, sizeof(config.affinity_cpu_list), "%s", app->settings.flac_affinity_cpu_list);
    config.enable_seektable = true;
    config.error_cb = gui_flac_error_callback;
    config.bytes_cb = gui_flac_bytes_callback;
    return config;
}
#endif

static int gui_record_start_rtlsdr_hifi(gui_app_t *app, const gui_record_output_targets_t *targets,
                                       const char *mode, uint32_t source_rate, uint64_t tuning) {
    char error[128];
    gui_sdr_rf_t *converter = gui_sdr_rf_create(source_rate, (uint32_t)tuning, error, sizeof(error));
    if (!converter) {
        gui_app_set_status(app, error);
        goto fail;
    }
    s_file_a = file_open_utf8(targets->paths[0], mode);
    if (!s_file_a) {
        gui_app_set_status(app, "Failed to open Hi-Fi RF output file");
        goto fail;
    }
    gui_record_set_output_buffer(s_file_a);
    s_ctx_a = (writer_ctx_t){
        .bufmgr = &app->buffers, .buf_id = BUF_RECORD_A, .file = s_file_a,
        .channel = 0, .rtl_converter = converter, .app = app
    };
    s_rtlsdr_hifi_source_rate = source_rate;
    gui_record_open_session_log(app, targets);
    proc_set_priority(PROC_PRIORITY_ABOVE);
#if LIBFLAC_ENABLED == 1
    if (targets->rtl_hifi_flac) {
        flac_writer_config_t config = gui_record_flac_config(app);
        // The shared FLAC encoder stores RF sample rate in kHz.
        config.sample_rate = GUI_SDR_RF_OUTPUT_RATE_HZ / 1000u;
        config.callback_user_data = &s_ctx_a;
        s_ctx_a.compressed_bytes = &app->recording_compressed_a;
        s_flac_writer_a = flac_writer_create_stream(s_file_a, &config);
        if (!s_flac_writer_a) {
            gui_app_set_status(app, "Failed to create Hi-Fi RF FLAC encoder");
            goto fail;
        }
        s_ctx_a.writer = s_flac_writer_a;
        s_record_sample_rate_a = config.sample_rate;
    }
#endif
    s_start_rec_a_wait_count = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
    s_start_rec_a_drop_count = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
    s_start_rec_b_wait_count = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
    s_start_rec_b_drop_count = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);
    atomic_store(&s_record_writer_draining, false);
    if (thrd_create_with_priority(&s_writer_thread_a, rtlsdr_hifi_writer_thread,
                                  &s_ctx_a, THRD_PRIORITY_CRITICAL) != thrd_success) {
        gui_app_set_status(app, "Failed to start Hi-Fi RF writer");
        goto fail;
    }
    s_writer_started[0] = true;
#if defined(__APPLE__)
    macos_promote_all_task_threads();
#endif
    app->recording_start_time = gui_record_clock(app);
    gui_record_start_source_tracking(app, targets, true, true);
    atomic_store(&s_rtlsdr_hifi_output, targets->rtl_hifi_flac
                 ? GUI_RECORD_HIFI_OUTPUT_FLAC : GUI_RECORD_HIFI_OUTPUT_RAW);
    app->is_recording = true;
    atomic_store(&s_rtlsdr_hifi_accepting, true);
    gui_app_set_status(app, targets->rtl_hifi_flac
        ? "Recording (RTL-SDR Hi-Fi RF FLAC)..." : "Recording (RTL-SDR Hi-Fi RF RAW)...");
    return RECORD_OK;

fail:
#if LIBFLAC_ENABLED == 1
    if (s_flac_writer_a) { flac_writer_abort(s_flac_writer_a); s_flac_writer_a = NULL; }
#endif
    if (s_file_a) {
        proc_set_priority(PROC_PRIORITY_NORMAL);
        fclose(s_file_a);
        s_file_a = NULL;
    }
    gui_sdr_rf_destroy(converter);
    s_ctx_a = (writer_ctx_t){0};
    s_recording_app = NULL;
    s_rtlsdr_hifi_source_rate = 0;
    atomic_store(&s_record_writer_draining, true);
    gui_record_close_session_log();
    return RECORD_ERROR;
}

static int gui_record_start_confirmed(gui_app_t *app,
                                      const gui_record_output_targets_t *confirmed) {
    if (!gui_record_capture_ready(app) || app->is_recording || gui_record_is_finalizing())
        return RECORD_ERROR;
    gui_record_output_targets_t targets;
    if (!gui_record_match_output_targets(app, confirmed, &targets)) return RECORD_ERROR;
    const bool hifi = targets.rtl_hifi;
    const bool record_a = targets.paths[0][0] != '\0';
    const bool record_b = targets.paths[1][0] != '\0';
    uint32_t source_rate = 0;
    uint64_t tuning = 0;
    if (hifi && !gui_record_rtlsdr_hifi_source(app, &source_rate, &tuning)) return RECORD_ERROR;
    if (app->settings.net_mode != GUI_NET_MODE_CLIENT && app->capture_has_channel_b &&
        app->selected_device >= 0 && app->selected_device < app->device_count &&
        app->devices[app->selected_device].type == DEVICE_TYPE_CXADC &&
        atomic_load(&app->rf_source_rate_hz[0]) != atomic_load(&app->rf_source_rate_hz[1]) &&
        ((targets.paths[0][0] && !gui_record_direct_channel_eligible(app, 0)) ||
         (targets.paths[1][0] && !gui_record_direct_channel_eligible(app, 1)))) {
        gui_app_set_status(app, "CXADC cards at different sample rates require native RAW recording");
        return RECORD_ERROR;
    }
    if (gui_record_wav_requested(&targets)) {
        if (app->settings.net_mode == GUI_NET_MODE_CLIENT && !gui_net_client_wav_ready(app)) {
            gui_app_set_status(app, "Waiting for audio from the current RF stream");
            return RECORD_ERROR;
        }
        if (!gui_record_local_wav_ready(app)) {
            gui_app_set_status(app, "CXADC audio unavailable. Reconnect capture or disable WAV recording.");
            return RECORD_ERROR;
        }
    }
    gui_record_reset_disk_guard_state();
#if !LIBSOXR_ENABLED
    float input_khz = (float)atomic_load(&app->sample_rate) / 1000.0f;
    if (!hifi && ((record_a && gui_record_resample_requested(
             app->settings.enable_resample_a, input_khz, app->settings.resample_rate_a)) ||
        (record_b && gui_record_resample_requested(
             app->settings.enable_resample_b, input_khz, app->settings.resample_rate_b)))) {
        gui_app_set_status(app, "RF resampling requires SoXR");
        return RECORD_ERROR;
    }
#endif

    // Build full output paths (output_path + filenames)
    const char *path_a = targets.paths[0];
    const char *path_b = targets.paths[1];
    // A confirmation covers only targets that were already present.
    // An initially absent target still requires an exclusive create.
    unsigned overwrite_mask = confirmed ? confirmed->existing_mask : UINT_MAX;
    const char *mode_a = overwrite_mask & 1u ? "wb" : "wbx";
    const char *mode_b = overwrite_mask & 2u ? "wb" : "wbx";
    s_record_path_a[0] = '\0';
    s_record_path_b[0] = '\0';
#if LIBFLAC_ENABLED == 1
    s_record_sample_rate_a = 0;
    s_record_sample_rate_b = 0;
#endif
    if (record_a) {
        snprintf(s_record_path_a, sizeof(s_record_path_a), "%s", path_a);
    }
    if (record_b) {
        snprintf(s_record_path_b, sizeof(s_record_path_b), "%s", path_b);
    }

    // Check if using simulated device (doesn't use extraction thread)
    bool is_simulated = false;
    if (app->device_count > 0 && app->selected_device < app->device_count) {
        is_simulated = (app->devices[app->selected_device].type == DEVICE_TYPE_SIMULATED);
    }

    // Verify extraction thread is running (or simulated capture)
    if (!gui_extract_is_running() && !is_simulated) {
        gui_app_set_status(app, "Extraction not running");
        return RECORD_ERROR;
    }

    // For simulated capture, ensure record buffers are initialized
    if (is_simulated) {
        gui_extract_init_record_rbs(app);
    }
    // Ensure record buffers are initialized in buffer manager for enabled channels
    if ((record_a && bufmgr_ensure_init(&app->buffers, BUF_RECORD_A) < 0) ||
        (record_b && bufmgr_ensure_init(&app->buffers, BUF_RECORD_B) < 0)) {
        gui_app_set_status(app, "Record buffers not initialized");
        return RECORD_ERROR;
    }

    // Recording uses a hard user-set mode latched at record start.
    bool prev_runtime_mode = app->capture_mode_runtime_misrc;
    app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
    TraceLog(LOG_INFO,
             "MODE TRACE: source=gui_record_start_confirmed latch_runtime old=%s new=%s user=%s",
             prev_runtime_mode ? "MISRC" : "HSDAOH",
             app->capture_mode_runtime_misrc ? "MISRC" : "HSDAOH",
             app->user_capture_mode_misrc ? "MISRC" : "HSDAOH");

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac && app->settings.flac_affinity_enabled) {
        if (!flac_writer_affinity_supported()) {
            gui_app_set_status(app, "FLAC affinity is only supported on Linux");
            return RECORD_ERROR;
        }
        char aff_err[256] = {0};
        if (!flac_writer_validate_affinity_cpu_list(app->settings.flac_affinity_cpu_list, aff_err, sizeof(aff_err))) {
            char status_msg[320];
            snprintf(status_msg, sizeof(status_msg), "Invalid FLAC affinity CPU list: %s",
                     aff_err[0] ? aff_err : "parse failure");
            gui_app_set_status(app, status_msg);
            return RECORD_ERROR;
        }
    }
#endif

    s_recording_app = app;
    gui_record_reset_stats(app);
    gui_record_spill_reset_all();
    atomic_store(&s_record_producer_lost_data, false);
    // Clear per-channel write-error flags for the new recording session.
    for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
        atomic_store(&s_record_write_error[i], false);
    }

    atomic_store(&s_rtlsdr_hifi_interrupted, false);
    s_rtlsdr_hifi_tuning = tuning;

    // Reset record buffers before starting
    gui_extract_reset_record_rbs(app);

    if (hifi) return gui_record_start_rtlsdr_hifi(app, &targets, mode_a, source_rate, tuning);

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac) {
        // Open FLAC files (respect per-channel enable)
        s_file_a = record_a ? file_open_utf8(path_a, mode_a) : NULL;
        s_file_b = record_b ? file_open_utf8(path_b, mode_b) : NULL;
        gui_record_set_output_buffer(s_file_a);
        gui_record_set_output_buffer(s_file_b);

        if ((record_a && !s_file_a) || (record_b && !s_file_b)) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            s_recording_app = NULL;
            return RECORD_ERROR;
        }
        gui_record_open_session_log(app, &targets);
        // Boost process priority before creating FLAC encoders/worker threads.
        proc_set_priority(PROC_PRIORITY_ABOVE);

        // Determine per-channel RF bit depth
        uint8_t bits_a = clamp_rf_bits_flac(app->settings.rf_bits_a);
        uint8_t bits_b = clamp_rf_bits_flac(app->settings.rf_bits_b);
        uint32_t capture_rate_hz = (uint32_t)atomic_load(&app->sample_rate);
        if (capture_rate_hz == 0) {
            capture_rate_hz = 40000000u;
        }
        uint32_t capture_rate_khz = (capture_rate_hz + 500u) / 1000u;
        if (capture_rate_khz == 0) {
            capture_rate_khz = 40000u;
        }
        float capture_rate_khz_f = ((float)capture_rate_hz) / 1000.0f;
        if (capture_rate_khz_f <= 0.0f) {
            capture_rate_khz_f = (float)capture_rate_khz;
        }

        // Setup writer contexts
        s_ctx_a.bufmgr = &app->buffers;
        s_ctx_a.buf_id = BUF_RECORD_A;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;
        s_ctx_a.compressed_bytes = &app->recording_compressed_a;
        s_ctx_a.flac_bits_per_sample = bits_a;
        s_ctx_a.rf_bits = bits_a;
        s_ctx_a.raw_bytes_per_sample = 2;  // input blocks are int16
        s_ctx_a.enable_resample = app->settings.enable_resample_a;
        s_ctx_a.input_sample_rate_khz = capture_rate_khz_f;
        s_ctx_a.resample_rate_khz = app->settings.resample_rate_a;
        s_ctx_a.resample_quality = app->settings.resample_quality_a;
        s_ctx_a.resample_gain_db = app->settings.resample_gain_a;
#if LIBSOXR_ENABLED
        s_ctx_a.soxr = NULL;
        s_ctx_a.soxr_rate_khz = 0.0f;
#endif
        s_ctx_a.app = app;

        s_ctx_b.bufmgr = &app->buffers;
        s_ctx_b.buf_id = BUF_RECORD_B;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;
        s_ctx_b.compressed_bytes = &app->recording_compressed_b;
        s_ctx_b.flac_bits_per_sample = bits_b;
        s_ctx_b.rf_bits = bits_b;
        s_ctx_b.raw_bytes_per_sample = 2;  // input blocks are int16
        s_ctx_b.enable_resample = app->settings.enable_resample_b;
        s_ctx_b.input_sample_rate_khz = capture_rate_khz_f;
        s_ctx_b.resample_rate_khz = app->settings.resample_rate_b;
        s_ctx_b.resample_quality = app->settings.resample_quality_b;
        s_ctx_b.resample_gain_db = app->settings.resample_gain_b;
#if LIBSOXR_ENABLED
        s_ctx_b.soxr = NULL;
        s_ctx_b.soxr_rate_khz = 0.0f;
#endif
        s_ctx_b.app = app;

        // Configure FLAC writers using shared library
        flac_writer_config_t config_a = gui_record_flac_config(app);
        flac_writer_config_t config_b = config_a;

        // Sample rate is stored in kHz for RF capture.
        bool use_resample_a = gui_record_resample_requested(
            s_ctx_a.enable_resample, capture_rate_khz_f, s_ctx_a.resample_rate_khz);
        bool use_resample_b = gui_record_resample_requested(
            s_ctx_b.enable_resample, capture_rate_khz_f, s_ctx_b.resample_rate_khz);
        config_a.sample_rate = use_resample_a
                                 ? (uint32_t)lroundf(app->settings.resample_rate_a)
                                 : capture_rate_khz;
        config_b.sample_rate = use_resample_b
                                 ? (uint32_t)lroundf(app->settings.resample_rate_b)
                                 : capture_rate_khz;
        s_record_sample_rate_a = config_a.sample_rate;
        s_record_sample_rate_b = config_b.sample_rate;

        if (record_a) {
            config_a.bits_per_sample = s_ctx_a.flac_bits_per_sample;
            config_a.callback_user_data = &s_ctx_a;
            if(gui_monitor_local_save_recording()){
                config_a.sink_write=mls_proxy_write;
                config_a.sink_seek=mls_proxy_seek;
                config_a.sink_tell=mls_proxy_tell;
            }
            s_flac_writer_a = flac_writer_create_stream(s_file_a, &config_a);
            if (!s_flac_writer_a) {
                gui_app_set_status(app, "Failed to create FLAC encoder A");
                gui_record_log_capture_event(app, "ERROR", "Failed to create FLAC encoder A",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                gui_record_close_session_log();
                s_recording_app = NULL;
                return RECORD_ERROR;
            }
            s_ctx_a.writer = s_flac_writer_a;
        } else {
            s_flac_writer_a = NULL;
            s_ctx_a.writer = NULL;
        }

        // Create writer for channel B
        if (record_b) {
            config_b.bits_per_sample = s_ctx_b.flac_bits_per_sample;
            config_b.callback_user_data = &s_ctx_b;
            if(gui_monitor_local_save_recording()){
                config_b.sink_write=mls_proxy_write;
                config_b.sink_seek=mls_proxy_seek;
                config_b.sink_tell=mls_proxy_tell;
            }
            s_flac_writer_b = flac_writer_create_stream(s_file_b, &config_b);
            if (!s_flac_writer_b) {
                gui_app_set_status(app, "Failed to create FLAC encoder B");
                gui_record_log_capture_event(app, "ERROR", "Failed to create FLAC encoder B",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (s_flac_writer_a) { flac_writer_abort(s_flac_writer_a); s_flac_writer_a = NULL; }
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                gui_record_close_session_log();
                s_recording_app = NULL;
                return RECORD_ERROR;
            }
            s_ctx_b.writer = s_flac_writer_b;
        } else {
            s_flac_writer_b = NULL;
            s_ctx_b.writer = NULL;
        }
        bool started_a = false;
        bool started_b = false;

        // Capture record-buffer backpressure stats at recording start
        s_start_rec_a_wait_count = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
        s_start_rec_a_drop_count = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
        s_start_rec_b_wait_count = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
        s_start_rec_b_drop_count = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);

        // Mark as recording
        app->is_recording = true;
        app->recording_start_time = gui_record_clock(app);

        atomic_store(&s_record_writer_draining, false);

        // Start writer threads BEFORE enabling recording in extraction thread
        // This ensures consumers are ready before producer starts filling buffers
        if (record_a) {
            if (thrd_create_with_priority(&s_writer_thread_a,
                                          flac_writer_thread,
                                          &s_ctx_a,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start FLAC writer A");
                gui_record_log_capture_event(app, "ERROR", "Failed to start FLAC writer A",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                app->is_recording = false;
                atomic_store(&s_record_writer_draining, true);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (s_flac_writer_a) { flac_writer_abort(s_flac_writer_a); s_flac_writer_a = NULL; }
                if (s_flac_writer_b) { flac_writer_abort(s_flac_writer_b); s_flac_writer_b = NULL; }
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                gui_record_close_session_log();
                s_recording_app = NULL;
                return RECORD_ERROR;
            }
            started_a = true;
        }
        if (record_b) {
            if (thrd_create_with_priority(&s_writer_thread_b,
                                          flac_writer_thread,
                                          &s_ctx_b,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start FLAC writer B");
                gui_record_log_capture_event(app, "ERROR", "Failed to start FLAC writer B",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                app->is_recording = false;
                atomic_store(&s_record_writer_draining, true);
                if (started_a) thrd_join(s_writer_thread_a, NULL);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (s_flac_writer_a) { flac_writer_abort(s_flac_writer_a); s_flac_writer_a = NULL; }
                if (s_flac_writer_b) { flac_writer_abort(s_flac_writer_b); s_flac_writer_b = NULL; }
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                gui_record_close_session_log();
                s_recording_app = NULL;
                return RECORD_ERROR;
            }
            started_b = true;
        }
        s_writer_started[0] = started_a;
        s_writer_started[1] = started_b;
#if defined(__APPLE__)
        /* Recording startup may create late helper threads in encoder/runtime
         * paths; promote them immediately so capture load stays P-core-biased. */
        macos_promote_all_task_threads();
#endif

        // Small delay to let writer threads initialize and start waiting on buffers
        thrd_sleep_ms(10);

        // Now enable recording in extraction thread - data will start flowing
        // (FLAC mode has no direct channels)
        gui_record_start_source_tracking(app, &targets, false, false);
        gui_extract_set_recording(true, true, bits_a, bits_b, false, false);
        gui_audio_start(app, &app->buffers);
        gui_audio_begin_recording(app, targets.paths + GUI_RECORD_RF_OUTPUT_COUNT,
                                  overwrite_mask >> GUI_RECORD_RF_OUTPUT_COUNT);
        gui_app_set_status(app, "Recording (FLAC)...");
    } else
#endif
    {
        // RAW recording (respect per-channel enable)
        s_file_a = app->settings.capture_a ? file_open_utf8(path_a, mode_a) : NULL;
        s_file_b = app->settings.capture_b ? file_open_utf8(path_b, mode_b) : NULL;
        gui_record_set_output_buffer(s_file_a);
        gui_record_set_output_buffer(s_file_b);

        if ((app->settings.capture_a && !s_file_a) || (app->settings.capture_b && !s_file_b)) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            s_recording_app = NULL;
            return RECORD_ERROR;
        }

        uint8_t bits_a = rf_bits_for_raw(app->settings.rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(app->settings.rf_bits_b);
        uint32_t capture_rate_hz = (uint32_t)atomic_load(&app->sample_rate);
        if (capture_rate_hz == 0) {
            capture_rate_hz = 40000000u;
        }
        float capture_rate_khz_f = ((float)capture_rate_hz) / 1000.0f;
        if (capture_rate_khz_f <= 0.0f) {
            capture_rate_khz_f = 40000.0f;
        }

        s_ctx_a.bufmgr = &app->buffers;
        s_ctx_a.buf_id = BUF_RECORD_A;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;
        s_ctx_a.rf_bits = bits_a;
        s_ctx_a.raw_bytes_per_sample = (bits_a == 8) ? 1 : 2;
        s_ctx_a.app = app;
        s_ctx_a.enable_resample = app->settings.enable_resample_a;
        s_ctx_a.input_sample_rate_khz = capture_rate_khz_f;
        s_ctx_a.resample_rate_khz = app->settings.resample_rate_a;
        s_ctx_a.resample_quality = app->settings.resample_quality_a;
        s_ctx_a.resample_gain_db = app->settings.resample_gain_a;
#if LIBSOXR_ENABLED
        s_ctx_a.soxr = NULL;
        s_ctx_a.soxr_rate_khz = 0.0f;
#endif

        s_ctx_b.bufmgr = &app->buffers;
        s_ctx_b.buf_id = BUF_RECORD_B;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;
        s_ctx_b.rf_bits = bits_b;
        s_ctx_b.raw_bytes_per_sample = (bits_b == 8) ? 1 : 2;
        s_ctx_b.app = app;
        s_ctx_b.enable_resample = app->settings.enable_resample_b;
        s_ctx_b.input_sample_rate_khz = capture_rate_khz_f;
        s_ctx_b.resample_rate_khz = app->settings.resample_rate_b;
        s_ctx_b.resample_quality = app->settings.resample_quality_b;
        s_ctx_b.resample_gain_db = app->settings.resample_gain_b;

#if LIBSOXR_ENABLED
        s_ctx_b.soxr = NULL;
        s_ctx_b.soxr_rate_khz = 0.0f;
#endif

        // Direct native passthrough decision (per channel). Computed once
        // here and reused for the writer threads, the capture-side taps and
        // the extraction skip mask, so a direct channel's record ringbuffer
        // has exactly one producer (the CXADC tap) and one consumer.
        bool direct_a = gui_record_direct_channel_eligible(app, 0);
        bool direct_b = gui_record_direct_channel_eligible(app, 1);
        memset(&s_direct_ctx_a, 0, sizeof(s_direct_ctx_a));
        memset(&s_direct_ctx_b, 0, sizeof(s_direct_ctx_b));
        atomic_store(&s_direct_bytes_read_a, 0);
        atomic_store(&s_direct_bytes_read_b, 0);
        s_direct_channel_a_active = direct_a;
        s_direct_channel_b_active = direct_b;
        if (direct_a) {
            s_direct_ctx_a.bufmgr = &app->buffers;
            s_direct_ctx_a.buf_id = BUF_RECORD_A;
            s_direct_ctx_a.channel = 0;
            s_direct_ctx_a.file = s_file_a;
            s_direct_ctx_a.writer_draining = &s_record_writer_draining;
            s_direct_ctx_a.bytes_read_total = &s_direct_bytes_read_a;
            s_direct_ctx_a.bytes_written_total = &app->recording_bytes;
            s_direct_ctx_a.bytes_written_channel = &app->recording_raw_a;
            s_direct_ctx_a.cb.user = app;
            s_direct_ctx_a.cb.spill_enqueue = gui_record_direct_spill_enqueue_cb;
            s_direct_ctx_a.cb.spill_read = gui_record_direct_spill_read_cb;
            s_direct_ctx_a.cb.spill_backlog = gui_record_direct_spill_backlog_cb;
            s_direct_ctx_a.cb.set_write_error = gui_record_direct_set_write_error_cb;
            s_direct_ctx_a.cb.log = gui_record_direct_log_cb;
        }
        if (direct_b) {
            s_direct_ctx_b.bufmgr = &app->buffers;
            s_direct_ctx_b.buf_id = BUF_RECORD_B;
            s_direct_ctx_b.channel = 1;
            s_direct_ctx_b.file = s_file_b;
            s_direct_ctx_b.writer_draining = &s_record_writer_draining;
            s_direct_ctx_b.bytes_read_total = &s_direct_bytes_read_b;
            s_direct_ctx_b.bytes_written_total = &app->recording_bytes;
            s_direct_ctx_b.bytes_written_channel = &app->recording_raw_b;
            s_direct_ctx_b.cb.user = app;
            s_direct_ctx_b.cb.spill_enqueue = gui_record_direct_spill_enqueue_cb;
            s_direct_ctx_b.cb.spill_read = gui_record_direct_spill_read_cb;
            s_direct_ctx_b.cb.spill_backlog = gui_record_direct_spill_backlog_cb;
            s_direct_ctx_b.cb.set_write_error = gui_record_direct_set_write_error_cb;
            s_direct_ctx_b.cb.log = gui_record_direct_log_cb;
        }
#if LIBSOXR_ENABLED
        s_ctx_b.soxr = NULL;
        s_ctx_b.soxr_rate_khz = 0.0f;
#endif
        bool started_a = false;
        bool started_b = false;

        // Capture record-buffer backpressure stats at recording start
        s_start_rec_a_wait_count = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
        s_start_rec_a_drop_count = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
        s_start_rec_b_wait_count = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
        s_start_rec_b_drop_count = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);

        // Boost process priority during recording
        proc_set_priority(PROC_PRIORITY_ABOVE);

        // Mark as recording
        app->is_recording = true;
        app->recording_start_time = gui_record_clock(app);

        atomic_store(&s_record_writer_draining, false);

        // Start writer threads BEFORE enabling recording in extraction thread
        // This ensures consumers are ready before producer starts filling buffers
        if (app->settings.capture_a) {
            // Direct channels use the byte-exact direct writer instead of
            // the converting raw_writer_thread.
            int (*writer_a_fn)(void *) = direct_a ? gui_record_direct_writer_thread
                                                  : raw_writer_thread;
            void *writer_a_arg = direct_a ? (void *)&s_direct_ctx_a : (void *)&s_ctx_a;
            if (thrd_create_with_priority(&s_writer_thread_a,
                                          writer_a_fn,
                                          writer_a_arg,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start RAW writer A");
                app->is_recording = false;
                atomic_store(&s_record_writer_draining, true);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                s_recording_app = NULL;
                return RECORD_ERROR;
            }
            started_a = true;
        }
        if (app->settings.capture_b) {
            int (*writer_b_fn)(void *) = direct_b ? gui_record_direct_writer_thread
                                                  : raw_writer_thread;
            void *writer_b_arg = direct_b ? (void *)&s_direct_ctx_b : (void *)&s_ctx_b;
            if (thrd_create_with_priority(&s_writer_thread_b,
                                          writer_b_fn,
                                          writer_b_arg,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start RAW writer B");
                app->is_recording = false;
                atomic_store(&s_record_writer_draining, true);
                if (started_a) thrd_join(s_writer_thread_a, NULL);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                s_recording_app = NULL;
                return RECORD_ERROR;
            }
            started_b = true;
        }
        s_writer_started[0] = started_a;
        s_writer_started[1] = started_b;
#if defined(__APPLE__)
        /* Recording startup may create late helper threads in encoder/runtime
         * paths; promote them immediately so capture load stays P-core-biased. */
        macos_promote_all_task_threads();
#endif

        // Small delay to let writer threads initialize and start waiting on buffers
        thrd_sleep_ms(10);

        // Enable the capture-side native taps now that the writers are up,
        // then enable extraction recording with the direct skip mask, so each
        // record ringbuffer has exactly one producer (the tap for direct
        // channels, extraction for converted channels).
        gui_record_start_source_tracking(app, &targets, direct_a, direct_b);
        if (direct_a) gui_record_direct_tap_enable(&s_direct_ctx_a);
        if (direct_b) gui_record_direct_tap_enable(&s_direct_ctx_b);
        gui_extract_set_recording(true, false, bits_a, bits_b, direct_a, direct_b);

        // Start audio output/monitoring (if enabled)
        gui_audio_start(app, &app->buffers);
        gui_audio_begin_recording(app, targets.paths + GUI_RECORD_RF_OUTPUT_COUNT,
                                  overwrite_mask >> GUI_RECORD_RF_OUTPUT_COUNT);
        gui_record_open_session_log(app, &targets);

        gui_app_set_status(app, (direct_a || direct_b)
                                 ? "Recording (RAW direct)..."
                                 : "Recording (RAW)...");
    }

    return RECORD_OK;
}

static void gui_record_close_output(FILE **file, int channel) {
    if (!*file) return;
    if (fclose(*file) != 0) {
        atomic_store(&s_record_write_error[channel], true);
        fprintf(stderr, "[REC] Channel %c output close failed\n", channel == 0 ? 'A' : 'B');
    }
    *file = NULL;
}

// After the producer and writer have both stopped, residual data cannot be
// a transient short read. This includes unread spill after seek/read errors,
// where the writer's ordinary "no next block" result cannot signal failure.
static void gui_record_check_writer_drained(gui_app_t *app, int channel) {
    buffer_id_t id = channel == 0 ? BUF_RECORD_A : BUF_RECORD_B;
    size_t ring = bufmgr_fill_level(&app->buffers, id);
    uint64_t spill = gui_record_spill_backlog_bytes(channel);
    if (!ring && !spill) return;
    atomic_store(&s_record_write_error[channel], true);
    gui_record_log_writef("ERROR", "Channel %c stopped with %zu ring bytes and %" PRIu64 " spill bytes unread",
                          channel ? 'B' : 'A', ring, spill);
}

// Heavy finalization runs outside the UI thread.
static void gui_record_finalize_stop_sync(gui_app_t *app, double stop_request_time) {
    const bool hifi = s_rtlsdr_hifi_source_rate != 0;
    // The UI has already disarmed the taps and converted producer. Wait for
    // their in-flight blocks before telling any file writer to exit.
    gui_record_direct_tap_wait_idle(&s_direct_ctx_a);
    gui_record_direct_tap_wait_idle(&s_direct_ctx_b);
    while (atomic_load(&s_rtlsdr_hifi_inflight)) thrd_sleep_ms(1);
    if (!hifi && !gui_extract_wait_recording_idle())
        atomic_store(&s_record_producer_lost_data, true);
    atomic_store(&s_record_writer_draining, true);
    for (int channel = 0; channel < 2; ++channel) {
        if (!s_writer_started[channel]) continue;
        thrd_join(channel ? s_writer_thread_b : s_writer_thread_a, NULL);
        gui_record_check_writer_drained(app, channel);
        s_writer_started[channel] = false;
    }
    gui_record_spill_reset_all();
    bool audio_ok = hifi || gui_audio_wait_recording_idle();

#if LIBFLAC_ENABLED == 1
    bool flac_a = s_flac_writer_a != NULL;
    bool flac_b = s_flac_writer_b != NULL;
    uint64_t flac_samples_a = 0, flac_samples_b = 0;
    if (flac_a) {
        flac_samples_a = flac_writer_get_samples_written(s_flac_writer_a);
        if (flac_writer_finish(s_flac_writer_a) != FLAC_WRITER_OK)
            atomic_store(&s_record_write_error[0], true);
        s_flac_writer_a = NULL;
    }
    if (flac_b) {
        flac_samples_b = flac_writer_get_samples_written(s_flac_writer_b);
        if (flac_writer_finish(s_flac_writer_b) != FLAC_WRITER_OK)
            atomic_store(&s_record_write_error[1], true);
        s_flac_writer_b = NULL;
    }
#endif

    gui_record_close_output(&s_file_a, 0);
    gui_record_close_output(&s_file_b, 1);

#if LIBFLAC_ENABLED == 1
    if (!gui_monitor_local_save_recording()) {
        if (flac_a && !atomic_load(&s_record_write_error[0])) {
            gui_record_finalize_flac_metadata(app, s_record_path_a,
                                               hifi ? "Hi-Fi RF" : "CH A",
                                              flac_samples_a, s_record_sample_rate_a);
        }
        if (flac_b && !atomic_load(&s_record_write_error[1])) {
            gui_record_finalize_flac_metadata(app, s_record_path_b, "CH B",
                                              flac_samples_b, s_record_sample_rate_b);
        }
    }
#endif

    if (gui_record_has_write_error() || !audio_ok) {
        gui_record_log_writef("ERROR", "Recording output failed while writing or closing files");
    }

    // Print recording summary with backpressure stats
    double stop_complete_time = gui_record_clock(app);
    double total_duration = stop_complete_time - app->recording_start_time;
    double capture_duration = stop_request_time - app->recording_start_time;
    double processing_duration = stop_complete_time - stop_request_time;
    if (capture_duration < 0.0) capture_duration = 0.0;
    if (processing_duration < 0.0) processing_duration = 0.0;
    if (total_duration < 0.0) total_duration = 0.0;
    app->last_recording_duration_s = capture_duration;
    uint64_t raw_a = atomic_load(&app->recording_raw_a);
    uint64_t raw_b = atomic_load(&app->recording_raw_b);
    if (hifi)
        gui_record_log_writef("INFO", "RTL-SDR Hi-Fi RF samples written: %" PRIu64, raw_a / sizeof(int16_t));
    uint64_t comp_a = atomic_load(&app->recording_compressed_a);
    uint64_t comp_b = atomic_load(&app->recording_compressed_b);
    uint64_t raw_total = raw_a + raw_b;
    uint64_t comp_total = comp_a + comp_b;
    double ratio_total = (comp_total > 0) ? ((double)raw_total / (double)comp_total) : 0.0;
    uint32_t end_rec_a_waits = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
    uint32_t end_rec_a_drops = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
    uint32_t end_rec_b_waits = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
    uint32_t end_rec_b_drops = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);
    uint32_t rec_a_waits = end_rec_a_waits - s_start_rec_a_wait_count;
    uint32_t rec_a_drops = end_rec_a_drops - s_start_rec_a_drop_count;
    uint32_t rec_b_waits = end_rec_b_waits - s_start_rec_b_wait_count;
    uint32_t rec_b_drops = end_rec_b_drops - s_start_rec_b_drop_count;
    uint32_t rec_waits = rec_a_waits + rec_b_waits;
    uint32_t rec_drops = rec_a_drops + rec_b_drops;

    char size_a[64], size_b[64], size_comp_a[64], size_comp_b[64], size_raw_total[64], size_comp_total[64];
    char total_hms[32], capture_hms[32], processing_hms[32];
    format_log_data_size_u64(raw_a, size_a, sizeof(size_a));
    format_log_data_size_u64(raw_b, size_b, sizeof(size_b));
    format_log_data_size_u64(comp_a, size_comp_a, sizeof(size_comp_a));
    format_log_data_size_u64(comp_b, size_comp_b, sizeof(size_comp_b));
    format_log_data_size_u64(raw_total, size_raw_total, sizeof(size_raw_total));
    format_log_data_size_u64(comp_total, size_comp_total, sizeof(size_comp_total));
    format_duration_hhmmss(total_duration, total_hms, sizeof(total_hms));
    format_duration_hhmmss(capture_duration, capture_hms, sizeof(capture_hms));
    format_duration_hhmmss(processing_duration, processing_hms, sizeof(processing_hms));

    fprintf(stderr, "[REC] Recording stopped: total=%.1fs (%s), capture=%.1fs (%s), processing=%.1fs (%s), A=%s, B=%s, waits=%u, drops=%u\n",
            total_duration, total_hms, capture_duration, capture_hms, processing_duration, processing_hms,
            size_a, size_b, rec_waits, rec_drops);
    fprintf(stderr, "[REC] Record buffers: A waits=%u drops=%u, B waits=%u drops=%u\n",
            rec_a_waits, rec_a_drops, rec_b_waits, rec_b_drops);

    if (rec_drops > 0) {
        fprintf(stderr, "[REC] WARNING: %u frames were dropped during recording due to backpressure!\n", rec_drops);
    }

    gui_record_log_writef("INFO", "Recording stopped: duration=%.2fs (%s) rawA=%s rawB=%s waits=%u drops=%u",
                          total_duration, total_hms, size_a, size_b, rec_waits, rec_drops);
    gui_record_log_writef("INFO", "Capture time: %.2fs (%s)", capture_duration, capture_hms);
    gui_record_log_writef("INFO", "Processing time: %.2fs (%s)", processing_duration, processing_hms);
    gui_record_log_writef("INFO", "Output data: compressedA=%s compressedB=%s",
                          size_comp_a, size_comp_b);
    if (comp_total > 0) {
        gui_record_log_writef("INFO", "Compression ratio: total_raw=%s total_compressed=%s ratio=%.3fx",
                              size_raw_total, size_comp_total, ratio_total);
    } else {
        gui_record_log_writef("INFO", "Compression ratio: N/A (non-compressed recording mode)");
    }
    if (rec_drops > 0) {
        gui_record_log_writef("WARN", "Backpressure drops detected: %u frame blocks dropped", rec_drops);
    }
    if (s_direct_channel_a_active) {
        uint64_t read_bytes = atomic_load(&s_direct_bytes_read_a);
        uint64_t written_bytes = atomic_load(&app->recording_raw_a);
        char read_buf[64], written_buf[64];
        format_log_data_size_u64(read_bytes, read_buf, sizeof(read_buf));
        format_log_data_size_u64(written_bytes, written_buf, sizeof(written_buf));
        fprintf(stderr, "[REC] Direct RAW channel A: read=%s written=%s\n", read_buf, written_buf);
        gui_record_log_writef("INFO", "Direct RAW channel A: bytes_read=%s bytes_written=%s%s",
                              read_buf, written_buf,
                              (read_bytes == written_bytes)
                              ? "" : " (MISMATCH: data was dropped or left in the spill backlog)");
    }
    if (s_direct_channel_b_active) {
        uint64_t read_bytes = atomic_load(&s_direct_bytes_read_b);
        uint64_t written_bytes = atomic_load(&app->recording_raw_b);
        char read_buf[64], written_buf[64];
        format_log_data_size_u64(read_bytes, read_buf, sizeof(read_buf));
        format_log_data_size_u64(written_bytes, written_buf, sizeof(written_buf));
        fprintf(stderr, "[REC] Direct RAW channel B: read=%s written=%s\n", read_buf, written_buf);
        gui_record_log_writef("INFO", "Direct RAW channel B: bytes_read=%s bytes_written=%s%s",
                              read_buf, written_buf,
                              (read_bytes == written_bytes)
                              ? "" : " (MISMATCH: data was dropped or left in the spill backlog)");
    }
    {
        char end_iso[32];
        gui_record_build_iso8601_timestamp(end_iso, sizeof(end_iso));
        gui_record_log_writef("INFO", "datetime_end: %s", end_iso[0] ? end_iso : "unknown");
    }
    /* FLAC/WAV close and header rewrites must succeed before COMMIT. */
    bool local_save = gui_monitor_local_save_recording();
    bool incomplete = !audio_ok || rec_drops != 0 || gui_record_has_write_error() ||
                      atomic_load(&s_record_producer_lost_data) ||
                      (local_save && gui_monitor_local_save_failed());
    gui_record_log_writef(incomplete ? "ERROR" : "INFO",
        incomplete ? (local_save ? "Local Save incomplete before COMMIT" : "Recording incomplete") :
        local_save ? "Payload finalized; client COMMIT pending" : "Session complete");
    gui_record_close_session_log();
    s_record_path_a[0] = '\0';
    s_record_path_b[0] = '\0';
    s_direct_channel_a_active = false;
    s_direct_channel_b_active = false;
    // Disarm the direct taps (the writer threads have drained and exited).
    gui_record_direct_tap_disable(&s_direct_ctx_a);
    gui_record_direct_tap_disable(&s_direct_ctx_b);
#if LIBFLAC_ENABLED == 1
    s_record_sample_rate_a = 0;
    s_record_sample_rate_b = 0;
#endif
    s_rtlsdr_hifi_source_rate = 0;

    bool unconfirmed = false;
    if (local_save) {
        bool good = !incomplete && mls_proxy_open_count() == 0;
        bool committed=gui_monitor_local_save_end(good);
        unconfirmed = good && !committed && gui_monitor_local_save_commit_unconfirmed();
        fprintf(stderr, "[M-LOCAL] %s\n", committed ? "client confirmed publication" :
                unconfirmed ? "publication unconfirmed; check client" : "incomplete session");
        if (!committed) {
            incomplete = true;
        }
    }
    atomic_store(&s_record_stop_commit_unconfirmed, unconfirmed);
    atomic_store(&s_record_stop_failed, incomplete);
    s_recording_app = NULL;
}

static int gui_record_finalize_thread(void *arg) {
    gui_record_finalize_ctx_t *ctx = (gui_record_finalize_ctx_t *)arg;
    if (ctx && ctx->app) {
        gui_record_finalize_stop_sync(ctx->app, ctx->stop_request_time);
    } else {
        atomic_store(&s_record_stop_failed, true);
    }
    free(ctx);
    atomic_store(&s_record_stop_finalize_done, true);
    return 0;
}

void gui_record_stop(gui_app_t *app) {
    if (!app->is_recording) {
        return;
    }
    bool expected = false;
    if (atomic_load(&s_finalize_thread_running) ||
        !atomic_compare_exchange_strong(&s_record_stop_finalizing, &expected, true)) {
        gui_app_set_status(app, "Finalizing previous recording...");
        return;
    }
    atomic_store(&s_record_stop_finalize_done, false);
    atomic_store(&s_record_stop_commit_unconfirmed, false);
    gui_record_reset_disk_guard_state();
    double stop_request_time = gui_record_clock(app);

    // Stop accepting new RF and WAV blocks immediately. The background
    // finalizer waits for the in-flight disk writes and drains both paths.
    gui_record_direct_tap_disable(&s_direct_ctx_a);
    gui_record_direct_tap_disable(&s_direct_ctx_b);
    atomic_store(&s_rtlsdr_hifi_accepting, false);
    gui_extract_request_record_stop();
    app->is_recording = false;
    gui_audio_request_record_stop(app);
    gui_record_check_source_tracking(app);

    // Restore normal process priority
    proc_set_priority(PROC_PRIORITY_NORMAL);

    gui_record_finalize_ctx_t *ctx = (gui_record_finalize_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        gui_record_finalize_stop_sync(app, stop_request_time);
        atomic_store(&s_record_stop_finalizing, false);
        gui_app_set_status(app, gui_record_stop_status());
        return;
    }

    ctx->app = app;
    ctx->stop_request_time = stop_request_time;

    if (thrd_create(&s_finalize_thread, gui_record_finalize_thread, ctx) != thrd_success) {
        free(ctx);
        gui_record_finalize_stop_sync(app, stop_request_time);
        atomic_store(&s_record_stop_finalizing, false);
        gui_app_set_status(app, gui_record_stop_status());
        return;
    }

    atomic_store(&s_finalize_thread_running, true);
    gui_app_set_status(app, "Finalizing recording...");
}

bool gui_record_is_finalizing(void) {
    // A completed, published handle can be joined by the next start or the
    // UI poll. This also lets a headless server accept the next session.
    if (atomic_load(&s_finalize_thread_running) && atomic_load(&s_record_stop_finalize_done))
        return false;
    return atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running);
}
