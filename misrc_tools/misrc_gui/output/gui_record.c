/*
 * MISRC - hsdaoh-rp2350 GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#include "gui_record.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../ui/gui_popup.h"
#include "gui_audio.h"
#include "../input/gui_capture.h"

#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/buffer_manager.h"
#include "../../common/flac_writer.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
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

// Buffer sizes
#define BUFFER_READ_SIZE 65536
// Use larger CLI-style writer chunks for record-path throughput stability.
// CLI writer path uses 65536*32 byte blocks; GUI records int16 samples, so this
// maps to 65536*32 bytes == 1048576 samples per channel per writer read.
#define GUI_RECORD_WRITER_BLOCK_BYTES ((size_t)65536 * 32)
#define GUI_RECORD_WRITER_BLOCK_SAMPLES (GUI_RECORD_WRITER_BLOCK_BYTES / sizeof(int16_t))

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
    if (!app || !free_bytes_out || !app->settings.output_path[0]) {
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
            return "fx3";
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
static bool s_writer_threads_running = false;
static FILE *s_file_a = NULL;
static FILE *s_file_b = NULL;
static FILE *s_capture_log_file = NULL;
static char s_capture_log_path[512];
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
    fprintf(s_capture_log_file, "[%s] [%s] %s\n", ts, (level && level[0]) ? level : "INFO", message);
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

// Overwrite confirmation pending state
static bool s_overwrite_pending = false;
static gui_app_t *s_pending_app = NULL;

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
    char path[512];
    atomic_uint_fast64_t write_offset;
    atomic_uint_fast64_t read_offset;
    atomic_uint_fast64_t last_backlog_log_mark;
    atomic_bool forced_mode;
    atomic_bool opened;
    atomic_flag io_lock;
} gui_record_spill_channel_t;

static gui_record_spill_channel_t s_record_spill[GUI_RECORD_SPILL_CHANNELS];

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
    if (!path || !path[0] || !free_bytes_out) {
        return false;
    }

#if defined(_WIN32) || defined(_WIN64)
    ULARGE_INTEGER free_bytes_available;
    if (!GetDiskFreeSpaceExA(path, &free_bytes_available, NULL, NULL)) {
        return false;
    }
    *free_bytes_out = (uint64_t)free_bytes_available.QuadPart;
    return true;
#else
    struct statvfs fs_stats;
    if (statvfs(path, &fs_stats) != 0) {
        return false;
    }
    *free_bytes_out = (uint64_t)fs_stats.f_bavail * (uint64_t)fs_stats.f_frsize;
    return true;
#endif
}

#if defined(_WIN32) || defined(_WIN64)
#define GUI_RECORD_FSEEK(stream, offset, whence) _fseeki64((stream), (__int64)(offset), (whence))
#else
#define GUI_RECORD_FSEEK(stream, offset, whence) fseeko((stream), (off_t)(offset), (whence))
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

    char template_path[512];
    snprintf(template_path, sizeof(template_path), "%s/.misrc_record_spill_%s_XXXXXX",
             base_dir, gui_record_spill_channel_name(channel));

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
    if (!gui_record_spill_valid_channel(channel)) {
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    if (atomic_load(&spill->opened)) {
        return true;
    }

    FILE *fp = NULL;
    char path_buf[512] = {0};
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

static uint64_t gui_record_spill_backlog_total_bytes(void) {
    uint64_t total = 0;
    for (int channel = 0; channel < GUI_RECORD_SPILL_CHANNELS; channel++) {
        total += gui_record_spill_backlog_bytes_locked(channel);
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
    if (app->settings.use_flac) {
        return (encoded_total > 0) ? encoded_total : raw_total;
    }
    return raw_total;
}

static uint64_t gui_record_estimate_output_bytes_from_raw_backlog(const gui_app_t *app,
                                                                  uint64_t backlog_raw_bytes,
                                                                  uint64_t raw_total,
                                                                  uint64_t encoded_total) {
    if (!app || backlog_raw_bytes == 0) return 0;
    if (!app->settings.use_flac) return backlog_raw_bytes;

    double encoded_per_raw = 1.0;
    if (raw_total > 0 && encoded_total > 0) {
        encoded_per_raw = (double)encoded_total / (double)raw_total;
        if (encoded_per_raw < 0.10) encoded_per_raw = 0.10;
        if (encoded_per_raw > 1.00) encoded_per_raw = 1.00;
    }

    double estimate = (double)backlog_raw_bytes * encoded_per_raw;
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

static bool gui_record_spill_read_block(int channel, int16_t *dst, size_t bytes) {
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

bool gui_record_spill_is_forced(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return false;
    }
    return atomic_load(&s_record_spill[channel].forced_mode);
}

bool gui_record_spill_enqueue(gui_app_t *app, int channel, const int16_t *samples, size_t bytes,
                              uint32_t frame_index, char *error_msg, size_t error_msg_size) {
    if (!gui_record_spill_valid_channel(channel) || !samples || bytes == 0) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Invalid spill enqueue request");
        }
        return false;
    }

    if (!gui_record_spill_open_channel(app, channel, error_msg, error_msg_size)) {
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    bool first_force = !atomic_exchange(&spill->forced_mode, true);
    if (first_force && app) {
        char msg[640];
        snprintf(msg, sizeof(msg),
                 "Record buffer backpressure on channel %s at frame=%u. Enabling temp spill file: %s",
                 gui_record_spill_channel_name(channel), frame_index,
                 spill->path[0] ? spill->path : "(temp)");
        gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
    }

    uint64_t write_off = atomic_load(&spill->write_offset);
    bool ok = false;
    gui_record_spill_lock(spill);
    if (GUI_RECORD_FSEEK(spill->fp, write_off, SEEK_SET) == 0) {
        size_t nwritten = fwrite(samples, 1, bytes, spill->fp);
        if (nwritten == bytes) {
            ok = true;
        } else {
            clearerr(spill->fp);
        }
    }
    gui_record_spill_unlock(spill);

    if (!ok) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Failed writing spill data for channel %s: %s",
                     gui_record_spill_channel_name(channel), strerror(errno));
        }
        return false;
    }

    atomic_store(&spill->write_offset, write_off + bytes);
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
    gui_app_t *app;  // For error reporting
} writer_ctx_t;

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

    // Only support downsampling (or passthrough) like CLI
    if (out_rate_khz <= 0.0f || out_rate_khz >= 40000.0f) {
        return NULL;
    }

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

    soxr_t s = soxr_create(40000.0, (double)out_rate_khz, 1, &err, &io_spec, &qual_spec, NULL);
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

#if LIBFLAC_ENABLED == 1
// FLAC writers (managed by shared library)
static flac_writer_t *s_flac_writer_a = NULL;
static flac_writer_t *s_flac_writer_b = NULL;

// Error callback for GUI FLAC writer
static void gui_flac_error_callback(void *user_data, flac_writer_error_t error, const char *message) {
    (void)error;
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->app) {
        gui_app_set_status(wctx->app, message);
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

static void convert_i16_to_flac_i32(int32_t *dst, const int16_t *src, size_t n, uint8_t bits) {
    if (!dst || !src || n == 0) return;

    if (bits == 8) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = (int32_t)gui_record_sample_12bit_to_i8(src[i]);
        }
        return;
    }

    if (bits == 12) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = (int32_t)src[i];
        }
        return;
    }

    // 16-bit output: expand 12-bit capture samples to 16-bit range
    for (size_t i = 0; i < n; i++) {
        dst[i] = (int32_t)src[i] << 4;
    }
}

static bool gui_record_get_next_block(writer_ctx_t *wctx, size_t block_bytes, int timeout_ms,
                                      int16_t *spill_block, const int16_t **out_samples,
                                      bool *from_ringbuffer) {
    if (!wctx || !spill_block || !out_samples || !from_ringbuffer) {
        return false;
    }

    void *buf = bufmgr_read_begin(wctx->bufmgr, wctx->buf_id, block_bytes, timeout_ms);
    if (buf) {
        *out_samples = (const int16_t *)buf;
        *from_ringbuffer = true;
        return true;
    }

    if (gui_record_spill_read_block(wctx->channel, spill_block, block_bytes)) {
        *out_samples = spill_block;
        *from_ringbuffer = false;
        return true;
    }

    return false;
}

// FLAC file writer thread
static int flac_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t len = GUI_RECORD_WRITER_BLOCK_BYTES;
    size_t raw_bytes_per_block = GUI_RECORD_WRITER_BLOCK_BYTES;
    bool flac_encoder_error_logged = false;

    // Boost thread priority to avoid backpressure when window is minimized
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    // Scratch buffers
    int16_t *tmp_i16 = NULL;
    int32_t *tmp_i32 = NULL;
    size_t tmp_cap = 0;

#if LIBSOXR_ENABLED
    // Max output samples per block (downsampling, so <= input, but keep some slack)
    size_t max_out = GUI_RECORD_WRITER_BLOCK_SAMPLES;
    tmp_i16 = (int16_t *)aligned_alloc(32, max_out * sizeof(int16_t));
    tmp_i32 = (int32_t *)aligned_alloc(32, max_out * sizeof(int32_t));
    tmp_cap = max_out;
#else
    // No soxr: only need int32 conversion buffer
    tmp_i32 = (int32_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * sizeof(int32_t));
    tmp_cap = GUI_RECORD_WRITER_BLOCK_SAMPLES;
#endif

    if (!tmp_i32) {
        fprintf(stderr, "[FLAC] Failed to allocate conversion buffers\n");
        if (wctx && wctx->app) {
            gui_record_log_capture_event(wctx->app, "ERROR", "FLAC writer failed to allocate conversion buffers",
                                         GUI_ERROR_CLASS_SYSTEM, 1);
        }
        return 0;
    }
    int16_t *spill_i16 = (int16_t *)aligned_alloc(32, len);
    if (!spill_i16) {
#if LIBSOXR_ENABLED
        if (tmp_i16) aligned_free(tmp_i16);
#endif
        aligned_free(tmp_i32);
        fprintf(stderr, "[FLAC] Failed to allocate spill read buffer\n");
        if (wctx && wctx->app) {
            gui_record_log_capture_event(wctx->app, "ERROR", "FLAC writer failed to allocate spill buffer",
                                         GUI_ERROR_CLASS_SYSTEM, 1);
        }
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
        bool encode_failed = false;
        int timeout_ms = (atomic_load(&do_exit) || !s_recording_app || !s_recording_app->is_recording) ? 0 : 10;
        if (!gui_record_get_next_block(wctx, len, timeout_ms, spill_i16, &in, &from_ringbuffer)) {
            if (timeout_ms == 0) {
                break;
            }
            continue;
        }
        size_t out_n = GUI_RECORD_WRITER_BLOCK_SAMPLES;

#if LIBSOXR_ENABLED
        if (wctx->enable_resample && wctx->resample_rate_khz > 0.0f) {
            soxr_t s = ensure_soxr(wctx, wctx->resample_rate_khz);
            if (s) {
                size_t in_done = 0, out_done = 0;
                soxr_error_t err = soxr_process(s, in, GUI_RECORD_WRITER_BLOCK_SAMPLES, &in_done,
                                               tmp_i16, tmp_cap, &out_done);
                if (!err && out_done > 0) {
                    out_n = out_done;
                    convert_i16_to_flac_i32(tmp_i32, tmp_i16, out_n, wctx->flac_bits_per_sample);
                    int result = flac_writer_process(wctx->writer, tmp_i32, (uint32_t)out_n);
                    if (result < 0 && !flac_encoder_error_logged) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "FLAC encoder error on channel %c", wctx->channel == 0 ? 'A' : 'B');
                        fprintf(stderr, "%s\n", msg);
                        if (wctx && wctx->app) {
                            gui_record_log_capture_event(wctx->app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
                        }
                        flac_encoder_error_logged = true;
                    }
                    if (result < 0) {
                        encode_failed = true;
                    }
                }
            }
        } else
#endif
        {
            convert_i16_to_flac_i32(tmp_i32, in, GUI_RECORD_WRITER_BLOCK_SAMPLES, wctx->flac_bits_per_sample);
            int result = flac_writer_process(wctx->writer, tmp_i32, GUI_RECORD_WRITER_BLOCK_SAMPLES);
            if (result < 0 && !flac_encoder_error_logged) {
                char msg[128];
                snprintf(msg, sizeof(msg), "FLAC encoder error on channel %c", wctx->channel == 0 ? 'A' : 'B');
                fprintf(stderr, "%s\n", msg);
                if (wctx && wctx->app) {
                    gui_record_log_capture_event(wctx->app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
                }
                flac_encoder_error_logged = true;
            }
            if (result < 0) {
                encode_failed = true;
            }
        }

        // Mark ringbuffer blocks as consumed; spill blocks are consumed by file read offset.
        if (from_ringbuffer) {
            bufmgr_read_end(wctx->bufmgr, wctx->buf_id, len);
        }

        if (encode_failed) {
            if (wctx && wctx->app && !atomic_load(&wctx->app->dropout_stop_requested)) {
                atomic_store(&wctx->app->dropout_stop_reason, GUI_DROPOUT_DEVICE_ERROR);
                atomic_store(&wctx->app->dropout_stop_requested, true);
            }
            break;
        }

        if (s_recording_app) {
            atomic_fetch_add(&s_recording_app->recording_bytes, len);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, raw_bytes_per_block);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, raw_bytes_per_block);
            }
        }
    }

#if LIBSOXR_ENABLED
    if (wctx->soxr) {
        soxr_delete((soxr_t)wctx->soxr);
        wctx->soxr = NULL;
        wctx->soxr_rate_khz = 0.0f;
    }
#endif

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

// RAW file writer thread
static int raw_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;

    // Input is always int16 blocks from BUF_RECORD
    size_t in_len = GUI_RECORD_WRITER_BLOCK_BYTES;

    // Output bytes per sample (1=8-bit, 2=16-bit)
    size_t bps = (wctx->raw_bytes_per_sample == 1) ? 1 : 2;

    // Boost thread priority to avoid backpressure when window is minimized
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

#if LIBSOXR_ENABLED
    int16_t *tmp_i16 = (int16_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * sizeof(int16_t));
    if (!tmp_i16) {
        fprintf(stderr, "[RAW] Failed to allocate resample buffer\n");
        return 0;
    }
#endif
    uint8_t *tmp_out = (uint8_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * bps);
    if (!tmp_out) {
        fprintf(stderr, "[RAW] Failed to allocate output buffer\n");
#if LIBSOXR_ENABLED
        aligned_free(tmp_i16);
#endif
        return 0;
    }
    int16_t *spill_i16 = (int16_t *)aligned_alloc(32, in_len);
    if (!spill_i16) {
        fprintf(stderr, "[RAW] Failed to allocate spill read buffer\n");
#if LIBSOXR_ENABLED
        aligned_free(tmp_i16);
#endif
        aligned_free(tmp_out);
        return 0;
    }

    fprintf(stderr, "[RAW] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    while (1) {
        const int16_t *in = NULL;
        bool from_ringbuffer = false;
        int timeout_ms = (atomic_load(&do_exit) || !s_recording_app || !s_recording_app->is_recording) ? 0 : 10;
        if (!gui_record_get_next_block(wctx, in_len, timeout_ms, spill_i16, &in, &from_ringbuffer)) {
            if (timeout_ms == 0) {
                break;
            }
            continue;
        }
        size_t out_n = GUI_RECORD_WRITER_BLOCK_SAMPLES;

#if LIBSOXR_ENABLED
        if (wctx->enable_resample && wctx->resample_rate_khz > 0.0f) {
            soxr_t s = ensure_soxr(wctx, wctx->resample_rate_khz);
            if (s) {
                size_t in_done = 0, out_done = 0;
                soxr_error_t err = soxr_process(s, in, GUI_RECORD_WRITER_BLOCK_SAMPLES, &in_done,
                                               tmp_i16, GUI_RECORD_WRITER_BLOCK_SAMPLES, &out_done);
                if (!err && out_done > 0) {
                    out_n = out_done;
                    convert_i16_to_raw_bytes(tmp_out, tmp_i16, out_n, wctx->rf_bits);
                    fwrite(tmp_out, 1, out_n * bps, wctx->file);
                }
            }
        } else
#endif
        {
            convert_i16_to_raw_bytes(tmp_out, in, out_n, wctx->rf_bits);
            fwrite(tmp_out, 1, out_n * bps, wctx->file);
        }

        if (from_ringbuffer) {
            bufmgr_read_end(wctx->bufmgr, wctx->buf_id, in_len);
        }

        if (s_recording_app) {
            // Approximate byte accounting: count input bytes consumed
            atomic_fetch_add(&s_recording_app->recording_bytes, in_len);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, in_len);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, in_len);
            }
        }
    }

#if LIBSOXR_ENABLED
    if (wctx->soxr) {
        soxr_delete((soxr_t)wctx->soxr);
        wctx->soxr = NULL;
        wctx->soxr_rate_khz = 0.0f;
    }
    aligned_free(tmp_i16);
#endif
    aligned_free(spill_i16);
    aligned_free(tmp_out);

    fprintf(stderr, "[RAW] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}

// Initialize recording subsystem
void gui_record_init(void) {
    gui_record_reset_disk_guard_state();
    gui_record_spill_reset_all();
}

// Cleanup recording subsystem
void gui_record_cleanup(void) {
    gui_record_reset_disk_guard_state();
    gui_record_spill_reset_all();
    gui_record_close_session_log();
}

// Check if recording is active
bool gui_record_is_active(void) {
    return s_recording_app != NULL && s_recording_app->is_recording;
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

    uint64_t spill_backlog_raw = gui_record_spill_backlog_total_bytes();
    uint64_t spill_backlog_output = gui_record_estimate_output_bytes_from_raw_backlog(
        app, spill_backlog_raw, raw_total, encoded_total);

    uint64_t projected_growth = 0;
    if (output_rate_bps > 0) {
        double projected = (double)output_rate_bps * GUI_RECORD_DISK_GUARD_LOOKAHEAD_SECONDS;
        projected_growth = (projected >= (double)UINT64_MAX) ? UINT64_MAX : (uint64_t)projected;
    }

    uint64_t required_headroom = GUI_RECORD_DISK_GUARD_MIN_HEADROOM_BYTES;
    if (projected_growth > required_headroom) {
        required_headroom = projected_growth;
    }
    if (spill_backlog_output > (UINT64_MAX - required_headroom)) {
        required_headroom = UINT64_MAX;
    } else {
        required_headroom += spill_backlog_output;
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
    format_file_size_u64(spill_backlog_output, backlog_buf, sizeof(backlog_buf));
    snprintf(msg, sizeof(msg),
             "Disk-space guard triggered at extract_frame=%u: free=%s required=%s (lookahead=%.0fs, output_rate=%.2f MB/s, spill_backlog=%s) on output path %s; requesting safe capture stop.",
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
static int gui_record_start_confirmed(gui_app_t *app);

static uint8_t clamp_rf_bits_flac(uint8_t bits) {
    if (bits == 8 || bits == 12 || bits == 16) return bits;
    return 16;
}

static uint8_t rf_bits_for_raw(uint8_t requested) {
    // RAW supports 8/16 only; treat 12 as 16.
    return (requested == 8) ? 8 : 16;
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

    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        char c = src[i];
        // allow [A-Za-z0-9._-], map spaces to '-'
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            dst[j++] = c;
        } else if (c == ' ' || c == '\t') {
            dst[j++] = '-';
        } else {
            // skip other chars (slashes etc.)
        }
    }
    dst[j] = '\0';

    // Trim trailing '-'
    while (j > 0 && dst[j - 1] == '-') {
        dst[--j] = '\0';
    }
}

static bool gui_record_cvbs_preview_enabled_locked(const channel_panel_config_t *config) {
    if (!config) return false;
    return (config->left_view == PANEL_VIEW_CVBS) ||
           (config->split && config->right_view == PANEL_VIEW_CVBS);
}

static void gui_record_open_session_log(gui_app_t *app, const char *path_a, const char *path_b) {
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
    if (app->settings.capture_a && path_a && path_a[0]) {
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

    snprintf(s_capture_log_path, sizeof(s_capture_log_path), "%s/%s_%s_misrc_capture.log",
             app->settings.output_path, base_name, date_tag);

    bool cvbs_preview_a = false;
    bool cvbs_preview_b = false;
    while (atomic_flag_test_and_set(&app->panel_config_lock)) {}
    cvbs_preview_a = gui_record_cvbs_preview_enabled_locked(&app->panel_config_a);
    cvbs_preview_b = gui_record_cvbs_preview_enabled_locked(&app->panel_config_b);
    atomic_flag_clear(&app->panel_config_lock);

    gui_record_log_lock();
    s_capture_log_file = fopen(s_capture_log_path, "w");
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
    snprintf(msg, sizeof(msg), "capture_format: %s", app->settings.use_flac ? "FLAC" : "RAW");
    gui_record_log_write_line_locked("INFO", msg);

    snprintf(msg, sizeof(msg), "Capture channels: A=%s B=%s", app->settings.capture_a ? "on" : "off", app->settings.capture_b ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "CVBS preview state: A=%s B=%s",
             cvbs_preview_a ? "on" : "off",
             cvbs_preview_b ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);

    uint8_t bits_a = app->settings.use_flac ? clamp_rf_bits_flac(app->settings.rf_bits_a) : rf_bits_for_raw(app->settings.rf_bits_a);
    uint8_t bits_b = app->settings.use_flac ? clamp_rf_bits_flac(app->settings.rf_bits_b) : rf_bits_for_raw(app->settings.rf_bits_b);
    snprintf(msg, sizeof(msg), "RF settings: bitsA=%u bitsB=%u resampleA=%s(%.1f kHz) resampleB=%s(%.1f kHz)",
             (unsigned)bits_a, (unsigned)bits_b,
             app->settings.enable_resample_a ? "on" : "off", app->settings.resample_rate_a,
             app->settings.enable_resample_b ? "on" : "off", app->settings.resample_rate_b);
    gui_record_log_write_line_locked("INFO", msg);
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
    snprintf(msg, sizeof(msg), "Dropout handling: stop_on_dropout=%s",
             app->settings.stop_on_dropout ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);

    if (app->settings.use_flac) {
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

    if (app->settings.capture_a && path_a && path_a[0]) {
        snprintf(msg, sizeof(msg), "FILE_PATH_A: %s", path_a);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (app->settings.capture_b && path_b && path_b[0]) {
        snprintf(msg, sizeof(msg), "FILE_PATH_B: %s", path_b);
        gui_record_log_write_line_locked("INFO", msg);
    }

    snprintf(msg, sizeof(msg), "Audio outputs: 4ch=%s 2ch12=%s 2ch34=%s",
             app->settings.enable_audio_4ch ? "on" : "off",
             app->settings.enable_audio_2ch_12 ? "on" : "off",
             app->settings.enable_audio_2ch_34 ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);

    if (app->settings.enable_audio_4ch && app->settings.audio_4ch_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_4CH_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_4ch_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (app->settings.enable_audio_2ch_12 && app->settings.audio_2ch_12_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_2CH_12_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_2ch_12_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (app->settings.enable_audio_2ch_34 && app->settings.audio_2ch_34_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_2CH_34_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_2ch_34_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    for (int i = 0; i < 4; i++) {
        if (app->settings.enable_audio_1ch[i] && app->settings.audio_1ch_filenames[i][0]) {
            snprintf(msg, sizeof(msg), "AUDIO_1CH_%d_FILE_PATH: %s/%s",
                     i + 1, app->settings.output_path, app->settings.audio_1ch_filenames[i]);
            gui_record_log_write_line_locked("INFO", msg);
        }
    }

    gui_record_log_unlock();
}

static void gui_record_apply_auto_names(gui_app_t *app) {
    if (!app) return;
    if (!app->settings.auto_names_enabled) return;

    const char *base = app->settings.output_base_name[0] ? app->settings.output_base_name : "capture";

    // Optionally append system timestamp sampled at record-start.
    // This does not mutate output_base_name.
    char base_with_ts[256];
    if (app->settings.append_timestamp_on_capture_start) {
        char timestamp_now[32];
        gui_record_build_system_timestamp(timestamp_now, sizeof(timestamp_now));
        if (timestamp_now[0]) {
            snprintf(base_with_ts, sizeof(base_with_ts), "%s_%s", base, timestamp_now);
            base = base_with_ts;
        }
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
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.flac", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.flac", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.flac", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.flac", base, (unsigned)bits_b);
        }
    } else {
        // RAW: 8/16 only
        uint8_t bits_a = rf_bits_for_raw(app->settings.rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(app->settings.rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (app->settings.enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), app->settings.resample_rate_a);
        if (app->settings.enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), app->settings.resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), app->settings.rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), app->settings.rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.raw", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.raw", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.raw", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.raw", base, (unsigned)bits_b);
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
        snprintf(app->settings.audio_4ch_filename, MAX_FILENAME_LEN, "%s_%s_quad_4ch.wav", base, audio_tag_4ch);
    } else {
        snprintf(app->settings.audio_4ch_filename, MAX_FILENAME_LEN, "%s_quad_4ch.wav", base);
    }
    if (audio_tag_12[0]) {
        snprintf(app->settings.audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch1_ch2.wav", base, audio_tag_12);
    } else {
        snprintf(app->settings.audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_stereo_ch1_ch2.wav", base);
    }
    if (audio_tag_34[0]) {
        snprintf(app->settings.audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch3_ch4.wav", base, audio_tag_34);
    } else {
        snprintf(app->settings.audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_stereo_ch3_ch4.wav", base);
    }

    for (int i = 0; i < 4; i++) {
        char tag[40];
        sanitize_tag(tag, sizeof(tag), app->settings.audio_1ch_labels[i]);
        if (tag[0]) {
            snprintf(app->settings.audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_%s_audio_ch%d.wav", base, tag, i + 1);
        } else {
            snprintf(app->settings.audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_audio_ch%d.wav", base, i + 1);
        }
    }
}

// Start recording - checks for file existence first
int gui_record_start(gui_app_t *app) {

    if (!app->is_capturing) {
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

    // Apply auto naming (must happen before overwrite checks)
    gui_record_apply_auto_names(app);

    // Build full output paths (output_path + filenames)
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof(path_a), "%s/%s", app->settings.output_path, app->settings.output_filename_a);
    snprintf(path_b, sizeof(path_b), "%s/%s", app->settings.output_path, app->settings.output_filename_b);

    // Check if output files already exist
    struct stat stat_a, stat_b;
    bool file_a_exists = app->settings.capture_a && (stat(path_a, &stat_a) == 0);
    bool file_b_exists = app->settings.capture_b && (stat(path_b, &stat_b) == 0);

    if (file_a_exists || file_b_exists) {
        // Build detailed message with file info
        char message[512];
        char size_buf[32];
        int offset = 0;

        offset += snprintf(message + offset, sizeof(message) - offset,
            "The following files will be overwritten:\n\n");

        if (file_a_exists) {
            format_file_size_u64((uint64_t)stat_a.st_size, size_buf, sizeof(size_buf));
            offset += snprintf(message + offset, sizeof(message) - offset,
                "CH A: %s (%s)\n", path_a, size_buf);
        }

        if (file_b_exists) {
            format_file_size_u64((uint64_t)stat_b.st_size, size_buf, sizeof(size_buf));
            offset += snprintf(message + offset, sizeof(message) - offset,
                "CH B: %s (%s)\n", path_b, size_buf);
        }

        // Show confirmation popup with detailed info
        gui_popup_confirm("Overwrite Files?", message, "Overwrite", "Cancel", app);
        s_overwrite_pending = true;
        s_pending_app = app;
        return RECORD_PENDING;
    }

    // No files exist, start recording directly
    return gui_record_start_confirmed(app);
}

// Check popup result and continue recording if confirmed
void gui_record_check_popup(gui_app_t *app) {
    if (!s_overwrite_pending) {
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
        gui_record_start_confirmed(app);
    } else {
        // User cancelled
        gui_app_set_status(app, "Recording cancelled");
    }

    s_pending_app = NULL;
}

// Internal: Start recording after confirmation
static int gui_record_start_confirmed(gui_app_t *app) {
    gui_record_reset_disk_guard_state();

    // Build full output paths (output_path + filenames)
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof(path_a), "%s/%s", app->settings.output_path, app->settings.output_filename_a);
    snprintf(path_b, sizeof(path_b), "%s/%s", app->settings.output_path, app->settings.output_filename_b);

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
    if ((app->settings.capture_a && bufmgr_ensure_init(&app->buffers, BUF_RECORD_A) < 0) ||
        (app->settings.capture_b && bufmgr_ensure_init(&app->buffers, BUF_RECORD_B) < 0)) {
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
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    app->last_recording_duration_s = 0.0;
    gui_record_spill_reset_all();

    // Reset record buffers before starting
    gui_extract_reset_record_rbs(app);

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac) {
        // Open FLAC files (respect per-channel enable)
        s_file_a = app->settings.capture_a ? fopen(path_a, "wb") : NULL;
        s_file_b = app->settings.capture_b ? fopen(path_b, "wb") : NULL;

        if ((app->settings.capture_a && !s_file_a) || (app->settings.capture_b && !s_file_b)) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            s_recording_app = NULL;
            return RECORD_ERROR;
        }
        gui_record_open_session_log(app, path_a, path_b);
        // Boost process priority before creating FLAC encoders/worker threads.
        proc_set_priority(PROC_PRIORITY_ABOVE);

        // Determine per-channel RF bit depth
        uint8_t bits_a = clamp_rf_bits_flac(app->settings.rf_bits_a);
        uint8_t bits_b = clamp_rf_bits_flac(app->settings.rf_bits_b);

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
        s_ctx_b.resample_rate_khz = app->settings.resample_rate_b;
        s_ctx_b.resample_quality = app->settings.resample_quality_b;
        s_ctx_b.resample_gain_db = app->settings.resample_gain_b;
#if LIBSOXR_ENABLED
        s_ctx_b.soxr = NULL;
        s_ctx_b.soxr_rate_khz = 0.0f;
#endif
        s_ctx_b.app = app;

        // Configure FLAC writers using shared library
        flac_writer_config_t config_a = flac_writer_default_config();
        flac_writer_config_t config_b = flac_writer_default_config();

        // Sample rate is stored in kHz for RF capture (40000 = 40 MSPS)
        config_a.sample_rate = (app->settings.enable_resample_a && app->settings.resample_rate_a > 0.0f)
                                 ? (uint32_t)(app->settings.resample_rate_a)
                                 : 40000;
        config_b.sample_rate = (app->settings.enable_resample_b && app->settings.resample_rate_b > 0.0f)
                                 ? (uint32_t)(app->settings.resample_rate_b)
                                 : 40000;

        // bits_per_sample is set per-channel below
        config_a.bits_per_sample = 16;
        config_b.bits_per_sample = 16;
        config_a.compression_level = app->settings.flac_level;
        config_b.compression_level = app->settings.flac_level;
        config_a.verify = app->settings.flac_verification;
        config_b.verify = app->settings.flac_verification;
        config_a.num_threads = (app->settings.flac_threads > 0) ? (uint32_t)app->settings.flac_threads : 0;  // 0 = auto
        config_b.num_threads = (app->settings.flac_threads > 0) ? (uint32_t)app->settings.flac_threads : 0;
        config_a.affinity_enabled = app->settings.flac_affinity_enabled;
        config_b.affinity_enabled = app->settings.flac_affinity_enabled;
        snprintf(config_a.affinity_cpu_list, sizeof(config_a.affinity_cpu_list), "%s", app->settings.flac_affinity_cpu_list);
        snprintf(config_b.affinity_cpu_list, sizeof(config_b.affinity_cpu_list), "%s", app->settings.flac_affinity_cpu_list);
        config_a.enable_seektable = true;
        config_b.enable_seektable = true;

        // Create writer for channel A
        config_a.error_cb = gui_flac_error_callback;
        config_a.bytes_cb = gui_flac_bytes_callback;

        if (app->settings.capture_a) {
            config_a.bits_per_sample = s_ctx_a.flac_bits_per_sample;
            config_a.callback_user_data = &s_ctx_a;
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
        if (app->settings.capture_b) {
            config_b.error_cb = gui_flac_error_callback;
            config_b.bytes_cb = gui_flac_bytes_callback;
            config_b.bits_per_sample = s_ctx_b.flac_bits_per_sample;
            config_b.callback_user_data = &s_ctx_b;
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
        app->recording_start_time = GetTime();

        // Start writer threads BEFORE enabling recording in extraction thread
        // This ensures consumers are ready before producer starts filling buffers
        if (app->settings.capture_a) {
            if (thrd_create_with_priority(&s_writer_thread_a,
                                          flac_writer_thread,
                                          &s_ctx_a,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start FLAC writer A");
                gui_record_log_capture_event(app, "ERROR", "Failed to start FLAC writer A",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                app->is_recording = false;
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
        if (app->settings.capture_b) {
            if (thrd_create_with_priority(&s_writer_thread_b,
                                          flac_writer_thread,
                                          &s_ctx_b,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start FLAC writer B");
                gui_record_log_capture_event(app, "ERROR", "Failed to start FLAC writer B",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                app->is_recording = false;
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
        s_writer_threads_running = started_a || started_b;
#if defined(__APPLE__)
        /* Recording startup may create late helper threads in encoder/runtime
         * paths; promote them immediately so capture load stays P-core-biased. */
        macos_promote_all_task_threads();
#endif

        // Small delay to let writer threads initialize and start waiting on buffers
        thrd_sleep_ms(10);

        // Now enable recording in extraction thread - data will start flowing
        gui_extract_set_recording(true, true, bits_a, bits_b);

        // Start audio output/monitoring (if enabled)
        gui_audio_start(app, &app->buffers);

        gui_app_set_status(app, "Recording (FLAC)...");
    } else
#endif
    {
        // RAW recording (respect per-channel enable)
        s_file_a = app->settings.capture_a ? fopen(path_a, "wb") : NULL;
        s_file_b = app->settings.capture_b ? fopen(path_b, "wb") : NULL;

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

        s_ctx_a.bufmgr = &app->buffers;
        s_ctx_a.buf_id = BUF_RECORD_A;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;
        s_ctx_a.rf_bits = bits_a;
        s_ctx_a.raw_bytes_per_sample = (bits_a == 8) ? 1 : 2;
        s_ctx_a.enable_resample = app->settings.enable_resample_a;
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
        s_ctx_b.enable_resample = app->settings.enable_resample_b;
        s_ctx_b.resample_rate_khz = app->settings.resample_rate_b;
        s_ctx_b.resample_quality = app->settings.resample_quality_b;
        s_ctx_b.resample_gain_db = app->settings.resample_gain_b;
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
        app->recording_start_time = GetTime();

        // Start writer threads BEFORE enabling recording in extraction thread
        // This ensures consumers are ready before producer starts filling buffers
        if (app->settings.capture_a) {
            if (thrd_create_with_priority(&s_writer_thread_a,
                                          raw_writer_thread,
                                          &s_ctx_a,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start RAW writer A");
                app->is_recording = false;
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
            if (thrd_create_with_priority(&s_writer_thread_b,
                                          raw_writer_thread,
                                          &s_ctx_b,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start RAW writer B");
                app->is_recording = false;
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
        s_writer_threads_running = started_a || started_b;
#if defined(__APPLE__)
        /* Recording startup may create late helper threads in encoder/runtime
         * paths; promote them immediately so capture load stays P-core-biased. */
        macos_promote_all_task_threads();
#endif

        // Small delay to let writer threads initialize and start waiting on buffers
        thrd_sleep_ms(10);

        // Now enable recording in extraction thread - data will start flowing
        gui_extract_set_recording(true, false, bits_a, bits_b);

        // Start audio output/monitoring (if enabled)
        gui_audio_start(app, &app->buffers);
        gui_record_open_session_log(app, path_a, path_b);

        gui_app_set_status(app, "Recording (RAW)...");
    }

    return RECORD_OK;
}

// Stop recording
void gui_record_stop(gui_app_t *app) {
    if (!app->is_recording) {
        return;
    }
    gui_record_reset_disk_guard_state();
    double stop_request_time = GetTime();

    // Disable recording in extraction thread first
    // This stops new data from being written to record ringbuffers
    gui_extract_set_recording(false, false, 16, 16);

    // Signal threads to stop FIRST so the audio restart cannot reopen WAVs in record mode - 070226 - MA
    app->is_recording = false;

    // Stop audio output/monitoring (file writing). --- 070226 - MA - Changed to resolve wav file corruption.
    // Then restart audio thread in monitor-only mode if we are still capturing,
    // so audio capture stays always-on without filling BUF_CAPTURE_AUDIO.
    
    //gui_audio_stop(app);
    //if (app->is_capturing) {
    //    (void)gui_audio_start(app, &app->buffers);
    //}
    gui_audio_stop(app);
    if (app->is_capturing) {
        (void)gui_audio_start(app, &app->buffers);
    }

    // Restore normal process priority
    proc_set_priority(PROC_PRIORITY_NORMAL);

    // Signal threads to stop
    //app->is_recording = false;

    // Wait for writer threads to drain and exit
    if (s_writer_threads_running) {
        if (app->settings.capture_a) thrd_join(s_writer_thread_a, NULL);
        if (app->settings.capture_b) thrd_join(s_writer_thread_b, NULL);
        s_writer_threads_running = false;
    }
    gui_record_spill_reset_all();

#if LIBFLAC_ENABLED == 1
    // Finalize FLAC writers (this also cleans them up)
    if (s_flac_writer_a) {
        flac_writer_finish(s_flac_writer_a);
        s_flac_writer_a = NULL;
    }
    if (s_flac_writer_b) {
        flac_writer_finish(s_flac_writer_b);
        s_flac_writer_b = NULL;
    }
#endif

    // Close files
    if (s_file_a) {
        fclose(s_file_a);
        s_file_a = NULL;
    }
    if (s_file_b) {
        fclose(s_file_b);
        s_file_b = NULL;
    }

    // Print recording summary with backpressure stats
    double stop_complete_time = GetTime();
    double total_duration = stop_complete_time - app->recording_start_time;
    double capture_duration = stop_request_time - app->recording_start_time;
    double processing_duration = stop_complete_time - stop_request_time;
    if (capture_duration < 0.0) capture_duration = 0.0;
    if (processing_duration < 0.0) processing_duration = 0.0;
    if (total_duration < 0.0) total_duration = 0.0;
    app->last_recording_duration_s = capture_duration;
    uint64_t raw_a = atomic_load(&app->recording_raw_a);
    uint64_t raw_b = atomic_load(&app->recording_raw_b);
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
    {
        char end_iso[32];
        gui_record_build_iso8601_timestamp(end_iso, sizeof(end_iso));
        gui_record_log_writef("INFO", "datetime_end: %s", end_iso[0] ? end_iso : "unknown");
    }
    gui_record_log_writef("INFO", "Session complete");
    gui_record_close_session_log();

    s_recording_app = NULL;
    gui_app_set_status(app, "Recording stopped");
}
