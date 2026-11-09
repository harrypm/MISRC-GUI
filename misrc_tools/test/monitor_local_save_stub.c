/* CPU-only stub of net/monitor_local_save_proxy.c for the test harnesses.
 *
 * Since the M-Server/M-Client modes landed (bdcbb7a), the production output
 * paths (gui_record.c / gui_audio.c / gui_extract.c) write through the
 * mls_proxy_* FILE* API instead of stdio. Harnesses that #include those
 * production files (gui_record_paths_test, gui_audio_soxr_lifecycle_test,
 * gui_rf_input_stamps_test) must link SOMETHING for that API - the real
 * proxy drags the networked local-save stack (zstd wire protocol, sockets),
 * which these CPU-only tests deliberately avoid. The stub is exactly the
 * real proxy's "not recording locally" semantics: plain stdio passthrough.
 * Same pattern as test/gui_playback_audio_stub.c (its header comment sets
 * the rule: extend the stub, never leak the heavy dependency into harnesses).
 */
#include "../misrc_gui/net/monitor_local_save_proxy.h"
#include "../misrc_gui/net/gui_monitor_local_save.h"
#include "../misrc_gui/net/gui_monitor_transport.h"
#include "../misrc_gui/net/gui_remote_display.h"
#include "../common/file_utils.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

/* gui_audio.c taps the capture stream for the M-Server monitor display only
 * while the unified server runs; idle in these harnesses. */
bool gui_monitor_server_active(void) {
    return false;
}

/* gui_extract.c offers raw ADC samples to the remote-display mailbox; no
 * monitor session exists in these harnesses, so drop them. */
void gui_rd_capture(const int16_t *a, const int16_t *b, size_t count,
                    uint32_t rate, uint32_t seq) {
    (void)a; (void)b; (void)count; (void)rate; (void)seq;
}

/* Local-save STATE stubs: the same production files also ask the local-save
 * module directly ("are we recording to the server-side proxy?"). In the
 * CPU-only harnesses the answer is always no / idle, which matches the real
 * module's state when M-Server/M-Client is not active. */
bool gui_monitor_local_save_recording(void) {
    return false;
}
bool gui_monitor_local_save_failed(void) {
    return false;
}
int gui_monitor_local_save_begin(void) {
    return -1;
}
int gui_monitor_local_save_send(unsigned type, unsigned id, uint64_t offset,
                                const void *data, size_t bytes) {
    (void)type; (void)id; (void)offset; (void)data; (void)bytes;
    return -1;
}
bool gui_monitor_local_save_commit_unconfirmed(void) {
    return false;
}
bool gui_monitor_local_save_end(bool commit) {
    (void)commit;
    return false;
}
void gui_monitor_local_save_diagnostics(char *buf, size_t cap) {
    if (buf && cap) buf[0] = '\0';
}

FILE *mls_proxy_open(const char *path, const char *mode) {
    return file_open_utf8(path, mode);
}
int mls_proxy_is(const FILE *file) {
    (void)file;
    return 0;
}
size_t mls_proxy_write(const void *data, size_t size, size_t count, FILE *file) {
    return fwrite(data, size, count, file);
}
int mls_proxy_seek(FILE *file, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(file, offset, whence);
#else
    return fseeko(file, (off_t)offset, whence);
#endif
}
int64_t mls_proxy_tell(FILE *file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return ftello(file);
#endif
}
int mls_proxy_flush(FILE *file) {
    return fflush(file);
}
int mls_proxy_error(FILE *file) {
    return ferror(file);
}
void mls_proxy_clearerr(FILE *file) {
    clearerr(file);
}
int mls_proxy_setvbuf(FILE *file, char *buffer, int mode, size_t size) {
    return setvbuf(file, buffer, mode, size);
}
int mls_proxy_close(FILE *file) {
    return fclose(file);
}
int mls_proxy_printf(FILE *file, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vfprintf(file, format, ap);
    va_end(ap);
    return n;
}
unsigned mls_proxy_open_count(void) {
    return 0;
}
void mls_proxy_reset_session_ids(void) {
}
