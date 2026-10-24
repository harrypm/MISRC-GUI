/*
 * MISRC GUI - Server/Client networking implementation.
 * See gui_net.h for the protocol and thread-safety model.
 */

#include "gui_net.h"
#include "gui_remote_monitor.h"
#include "gui_remote_display.h"
#include "gui_remote_fs.h"
#include "local_save_files.h"
#include "local_save_protocol.h"
#include "local_save_proxy.h"
#include "../input/gui_playback.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../output/gui_record.h"
#include "../../common/file_utils.h"
#include "../../common/buffer_manager.h"
#include "../../common/rb_event.h"
#include "../../common/threading.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <wchar.h>

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif

/* -------------------------------------------------------------------------
 * Cross-platform sockets + sync primitives
 * ------------------------------------------------------------------------- */

#ifdef _WIN32
  /* Avoid Win32 API name collisions with raylib symbols (Rectangle, CloseWindow, ShowCursor). */
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOGDI
  #define NOGDI
  #endif
  #ifndef NOUSER
  #define NOUSER
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET net_sock_t;
  #define NET_INVALID_SOCKET INVALID_SOCKET
  #define net_close(s) closesocket(s)
  #define net_errno ((int)WSAGetLastError())
  typedef CRITICAL_SECTION net_mutex_t;
  typedef CONDITION_VARIABLE net_cond_t;
  #define net_mutex_init(m)   InitializeCriticalSection(m)
  #define net_mutex_destroy(m) DeleteCriticalSection(m)
  #define net_mutex_lock(m)   EnterCriticalSection(m)
  #define net_mutex_unlock(m) LeaveCriticalSection(m)
  #define net_cond_init(c)    InitializeConditionVariable(c)
  #define net_cond_destroy(c) ((void)0)
  #define net_cond_broadcast(c) WakeAllConditionVariable(c)
  #define mls_cond_wait(c,m) ((void)SleepConditionVariableCS((c),(m),INFINITE))
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <dirent.h>
  #include <sys/stat.h>
  typedef int net_sock_t;
  #define NET_INVALID_SOCKET (-1)
  #define net_close(s) close(s)
  #define net_errno errno
  typedef pthread_mutex_t net_mutex_t;
  typedef pthread_cond_t net_cond_t;
  #define net_mutex_init(m)   ((void)pthread_mutex_init(m, NULL))
  #define net_mutex_destroy(m) ((void)pthread_mutex_destroy(m))
  #define net_mutex_lock(m)   ((void)pthread_mutex_lock(m))
  #define net_mutex_unlock(m) ((void)pthread_mutex_unlock(m))
  #define net_cond_init(c)    ((void)pthread_cond_init(c, NULL))
  #define net_cond_destroy(c) ((void)pthread_cond_destroy(c))
  #define net_cond_broadcast(c) ((void)pthread_cond_broadcast(c))
  #define mls_cond_wait(c,m) ((void)pthread_cond_wait((c),(m)))
#endif

static inline int net_sock_valid(net_sock_t s) {
#ifdef _WIN32
    return s != NET_INVALID_SOCKET;
#else
    return s >= 0;
#endif
}

/* Portable thread handle + detach. The codebase's threading.h gives thrd_t /
 * thrd_create / thrd_join, but no detach. Detached worker threads (per-client
 * HTTP handlers) need a detach so their handles don't leak. */
static void net_thread_detach(thrd_t *t) {
#ifdef _WIN32
    if (t && *t) { CloseHandle(*t); *t = (thrd_t)0; }
#else
    if (t) pthread_detach(*t);
#endif
}

/* Set a socket non-blocking / blocking. */
static int net_set_nonblocking(net_sock_t fd) {
#ifdef _WIN32
    unsigned long mode = 1;
    return (ioctlsocket(fd, /* FIONBIO */ 0x8004667CUL, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) ? 0 : -1;
#endif
}

static int net_set_blocking(net_sock_t fd) {
#ifdef _WIN32
    unsigned long mode = 0;
    return (ioctlsocket(fd, 0x8004667CUL, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0) ? 0 : -1;
#endif
}

/* The Local Save protocol has small ACK/header packets. Disable Nagle
 * only on its dedicated TCP socket to avoid delaying every round trip. */
static void mls_enable_tcp_nodelay(net_sock_t fd) {
    int enabled=1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     (const char *)&enabled, sizeof(enabled));
}

/* Set SO_RCVTIMEO / SO_SNDTIMEO so blocking recv/send do not hang forever.
 * This lets networking threads wake up to check their stop flags. */
static void net_set_timeouts(net_sock_t fd, int rcv_ms, int snd_ms) {
    if (!net_sock_valid(fd)) return;
#ifdef _WIN32
    DWORD rcv = (DWORD)rcv_ms, snd = (DWORD)snd_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv, sizeof(rcv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&snd, sizeof(snd));
#else
    struct timeval tv;
    tv.tv_sec = rcv_ms / 1000;
    tv.tv_usec = (rcv_ms % 1000) * 1000L;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    tv.tv_sec = snd_ms / 1000;
    tv.tv_usec = (snd_ms % 1000) * 1000L;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#endif
}

/* Send all bytes; returns 0 on success, -1 on error. */
static int net_send_all(net_sock_t fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
#ifdef _WIN32
        int n = send(fd, p, (int)len, 0);
        if (n == SOCKET_ERROR) return -1;
#else
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
#endif
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int net_send_str(net_sock_t fd, const char *s) {
    return net_send_all(fd, s, strlen(s));
}

/* Read until we have seen "\r\n\r\n" (end of HTTP headers) or buffer full.
 * Returns total bytes read into buf (including the terminator), or -1 on
 * error/EOF before headers complete. Mirrors the reference http_thread loop. */
static int net_read_headers(net_sock_t fd, char *buf, size_t cap) {
    size_t len = 0;
    if (cap < 4) return -1;
    while (len < cap - 1) {
#ifdef _WIN32
        int n = recv(fd, buf + len, (int)(cap - 1 - len), 0);
        if (n == SOCKET_ERROR || n == 0) return -1;
        len += (size_t)n;
#else
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        len += (size_t)n;
#endif
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n")) return (int)len;
    }
    return -1;
}

/* Local Save: client-initiated persistent TCP session. The capture server
 * streams ordered file operations with cumulative ACKs (v3) and client-owned
 * paths. No SMB or mounted share. */
static net_mutex_t s_mls_mtx;
static bool s_mls_lock_ready = false;
static net_sock_t s_mls_fd = NET_INVALID_SOCKET;
static atomic_bool s_mls_active = false;
static atomic_bool s_mls_record = false;
static atomic_bool s_mls_failed = false;
static atomic_bool s_mls_request = false;
/* Diagnostics: time spent transferring and acknowledging payload bytes,
 * independent of the FLAC encoder time. Protected by s_mls_mtx. */
static uint64_t s_mls_write_bytes = 0;
static uint64_t s_mls_write_frames = 0;
static double s_mls_write_seconds = 0.0;
static uint64_t s_mls_next_report = 32u * 1024u * 1024u;
static double s_mls_last_ack_time = 0.0;
/* FLAC compression must not be held up by one TCP round-trip per MiB.
 * Encode threads enqueue immutable, ordered WRITE frames; a dedicated sender
 * serializes socket operations and ACKs. OPEN/CLOSE/COMMIT are barriers and
 * wait for all earlier data to be ACKed. 64 MiB is bounded RAM, not a disk
 * spill. A network that remains slower than the encoder will still cause
 * backpressure; monitoring that rate is essential. */
#define MLS_QUEUE_MAX_BYTES (64u * 1024u * 1024u)
typedef struct mls_job {
    struct mls_job *next;
    unsigned type, id;
    uint64_t offset;
    size_t bytes;
    bool synchronous, done;
    int result;
    uint8_t payload[];
} mls_job_t;
static net_mutex_t s_mls_q_mtx;
static net_cond_t s_mls_q_cond;
static bool s_mls_q_ready=false;
static bool s_mls_q_stop=false;
static bool s_mls_worker_joinable=false;
static thrd_t s_mls_worker;
static mls_job_t *s_mls_q_head=NULL, *s_mls_q_tail=NULL;
static size_t s_mls_q_bytes=0, s_mls_q_peak=0;
static uint64_t s_mls_q_frames=0;



static int mls_recv_exact(net_sock_t fd, void *dest, size_t n) {
    uint8_t *p = (uint8_t *)dest;
    while (n) {
#ifdef _WIN32
        int got = recv(fd,(char *)p,(int)(n > 16384 ? 16384 : n),0);
#else
        ssize_t got = recv(fd,p,n > 16384 ? 16384 : n,0);
#endif
        if (got <= 0) {
#ifndef _WIN32
            if (got < 0 && errno == EINTR) continue;
#endif
            return -1;
        }
        p += got; n -= (size_t)got;
    }
    return 0;
}
static void mls_mark_failed_locked(void) {
    s_mls_failed = true;
    s_mls_active = false;

    /* The socket belongs to the HTTP handler and must not be freed here. */
}
static int mls_sender_worker(void *unused);
bool gui_net_local_save_recording(void) { return s_mls_record; }
bool gui_net_local_save_failed(void) { return s_mls_failed; }
int gui_net_local_save_begin(void) {
    if (!s_mls_lock_ready) return -1;
    net_mutex_lock(&s_mls_mtx);
    int ok = s_mls_active && !s_mls_record && !s_mls_failed && net_sock_valid(s_mls_fd);
    if (ok) {
        mls_proxy_reset_session_ids();
        s_mls_write_bytes = 0;
        s_mls_write_frames = 0;
        s_mls_write_seconds = 0.0;
        s_mls_next_report = 32u * 1024u * 1024u;
        s_mls_last_ack_time = gui_runtime_time();
        if (!s_mls_q_ready) {
            net_mutex_init(&s_mls_q_mtx);
            net_cond_init(&s_mls_q_cond);
            s_mls_q_ready=true;
        }
        net_mutex_lock(&s_mls_q_mtx);
        s_mls_q_stop=false;
        s_mls_q_head=s_mls_q_tail=NULL;
        s_mls_q_bytes=s_mls_q_peak=0;
        s_mls_q_frames=0;
        net_mutex_unlock(&s_mls_q_mtx);
        s_mls_record = true;
        if (thrd_create(&s_mls_worker,mls_sender_worker,NULL)==thrd_success)
            s_mls_worker_joinable=true;
        else { s_mls_record=false; ok=false; }
    }
    net_mutex_unlock(&s_mls_mtx);
    return ok ? 0 : -1;
}
/* Only the dedicated sender thread calls this function. Every operation
 * remains wire-ordered, ACK success means the client flushed the file. */
static int mls_send_wire_sync(unsigned type, unsigned id, uint64_t offset,
                            const void *buf, size_t bytes) {
    if (!s_mls_lock_ready || id >= MLS_MAX_FILES || bytes > MLS_MAX_PAYLOAD) return -1;
    net_mutex_lock(&s_mls_mtx);
    if (!s_mls_record || !s_mls_active || s_mls_failed || !net_sock_valid(s_mls_fd)) {
        net_mutex_unlock(&s_mls_mtx); return -1;
    }
    mls_frame_t f = {0}, ack = {0};
    f.type=(uint8_t)type; f.file_id=(uint8_t)id; f.length=(uint32_t)bytes;
    f.offset=offset; f.crc = bytes ? mls_crc32(buf,bytes) : 0;
    uint8_t h[MLS_HEADER_SIZE];
    double write_started = type == MLS_WRITE ? gui_runtime_time() : 0.0;
    int ok=mls_encode_header(h,&f)==0 && net_send_all(s_mls_fd,h,sizeof(h))==0 &&
           (!bytes || net_send_all(s_mls_fd,buf,bytes)==0) &&
           mls_recv_exact(s_mls_fd,h,sizeof(h))==0 &&
           mls_decode_header(&ack,h,sizeof(h))==0 && ack.type==MLS_ACK &&
           ack.file_id==id && ack.offset==0 && ack.length==0;
    if(ok) s_mls_last_ack_time = gui_runtime_time();
    if(ok && type == MLS_WRITE) {
        double elapsed = gui_runtime_time() - write_started;
        if (elapsed > 0.0) s_mls_write_seconds += elapsed;
        s_mls_write_bytes += bytes;
        s_mls_write_frames++;
        if(s_mls_write_bytes >= s_mls_next_report) {
            double mib = (double)s_mls_write_bytes/(1024.0*1024.0);
            fprintf(stderr,"[Local Save] %.1f MiB acknowledged; %.1f MiB/s in transfer+ACK; %" PRIu64 " writes\n",
                    mib, s_mls_write_seconds > 0 ? mib/s_mls_write_seconds : 0.0,
                    s_mls_write_frames);
            s_mls_next_report = (s_mls_write_bytes/(32u*1024u*1024u)+1u)*(32u*1024u*1024u);
        }
    }
    if(!ok) mls_mark_failed_locked();
    net_mutex_unlock(&s_mls_mtx);
    return ok ? 0 : -1;
}
/* Fast streaming mode: WRITE_STREAM frames carry offsets and CRCs but
 * deliberately do not wait for per-frame ACK. MLS_SYNC is the cumulative
 * acknowledgement and flush barrier, at most 8 MiB of writes apart. */
static int mls_send_wire_stream(unsigned id, uint64_t offset,
                                const void *buf, size_t bytes) {
    net_mutex_lock(&s_mls_mtx);
    if (!s_mls_record || !s_mls_active || s_mls_failed ||
        !net_sock_valid(s_mls_fd)) {
        net_mutex_unlock(&s_mls_mtx);
        return -1;
    }
    mls_frame_t frame={0};
    frame.type=MLS_WRITE_STREAM;
    frame.file_id=(uint8_t)id;
    frame.offset=offset;
    frame.length=(uint32_t)bytes;
    frame.crc=mls_crc32(buf,bytes);
    uint8_t header[MLS_HEADER_SIZE];
    int ok=mls_encode_header(header,&frame)==0 &&
           net_send_all(s_mls_fd,header,sizeof(header))==0 &&
           net_send_all(s_mls_fd,buf,bytes)==0;
    if(!ok) mls_mark_failed_locked();
    net_mutex_unlock(&s_mls_mtx);
    return ok?0:-1;
}
static int mls_sender_worker(void *unused) {
    (void)unused;
    unsigned outstanding=0;
    size_t outstanding_bytes=0;
    double batch_started=0;
    for (;;) {
        net_mutex_lock(&s_mls_q_mtx);
        while (!s_mls_q_head && !s_mls_q_stop)
            mls_cond_wait(&s_mls_q_cond,&s_mls_q_mtx);
        if (!s_mls_q_head && s_mls_q_stop) {
            net_mutex_unlock(&s_mls_q_mtx);
            break;
        }
        mls_job_t *job=s_mls_q_head;
        s_mls_q_head=job->next;
        if (!s_mls_q_head) s_mls_q_tail=NULL;
        net_mutex_unlock(&s_mls_q_mtx);
        int result=0;
        if(job->type==MLS_WRITE) {
            if(!outstanding)batch_started=gui_runtime_time();
            result=mls_send_wire_stream(job->id,job->offset,job->payload,job->bytes);
            if(result==0){outstanding++;outstanding_bytes+=job->bytes;}
        }
        /* Limit the unacknowledged window to 8 MiB, and turn all control
         * frames (including CLOSE/COMMIT) into full ordering barriers. */
        if(result==0 && (outstanding>=8 ||
                          (outstanding && job->type!=MLS_WRITE))) {
            result=mls_send_wire_sync(MLS_SYNC,0,0,NULL,0);
            if(result==0){
                double elapsed=gui_runtime_time()-batch_started;
                net_mutex_lock(&s_mls_mtx);
                s_mls_write_bytes+=outstanding_bytes;
                s_mls_write_frames+=outstanding;
                if(elapsed>0)s_mls_write_seconds+=elapsed;
                if(s_mls_write_bytes>=s_mls_next_report){
                    double mib=(double)s_mls_write_bytes/(1024.0*1024.0);
                    fprintf(stderr,"[Local Save] %.1f MiB ACKed; %.1f MiB/s network+client flush; %" PRIu64 " writes\n",
                            mib,s_mls_write_seconds>0?mib/s_mls_write_seconds:0.0,s_mls_write_frames);
                    s_mls_next_report=(s_mls_write_bytes/(32u*1024u*1024u)+1u)*(32u*1024u*1024u);
                }
                net_mutex_unlock(&s_mls_mtx);
            }
            outstanding=0;outstanding_bytes=0;
        }
        if(result==0 && job->type!=MLS_WRITE)
            result=mls_send_wire_sync(job->type,job->id,job->offset,
                                      job->bytes?job->payload:NULL,job->bytes);
        net_mutex_lock(&s_mls_q_mtx);
        if (s_mls_q_bytes >= job->bytes) s_mls_q_bytes-=job->bytes;
        if (job->synchronous) {
            job->result=result;
            job->done=true;  /* the waiting caller owns and frees it */
        } else free(job);
        net_cond_broadcast(&s_mls_q_cond);
        net_mutex_unlock(&s_mls_q_mtx);
    }
    return 0;
}
/* Write calls only wait when 64 MiB of unsent compressed output accumulates.
 * Control operations block until their own ACK, preserving seek/rewrite order. */
int gui_net_local_save_send(unsigned type, unsigned id, uint64_t offset,
                            const void *buf, size_t bytes) {
    if (!s_mls_q_ready || id>=MLS_MAX_FILES || bytes>MLS_MAX_PAYLOAD ||
        (bytes && !buf) || !s_mls_worker_joinable) return -1;
    mls_job_t *job=malloc(sizeof(*job)+bytes);
    if (!job) return -1;
    job->next=NULL;job->type=type;job->id=id;job->offset=offset;
    job->bytes=bytes;job->synchronous=(type!=MLS_WRITE);
    job->done=false;job->result=-1;
    if (bytes) memcpy(job->payload,buf,bytes);
    net_mutex_lock(&s_mls_q_mtx);
    if (type==MLS_WRITE && s_mls_q_bytes+bytes>MLS_QUEUE_MAX_BYTES) {
        fprintf(stderr,"[Local Save] WARNING: compressed send queue full (%.1f/64 MiB); encoder backpressure\n",
                (double)s_mls_q_bytes/1048576.0);
    }
    while (type==MLS_WRITE && !s_mls_q_stop && !s_mls_failed &&
           s_mls_q_bytes+bytes>MLS_QUEUE_MAX_BYTES)
        mls_cond_wait(&s_mls_q_cond,&s_mls_q_mtx);
    if (s_mls_q_stop || s_mls_failed || !s_mls_active || !s_mls_record) {
        net_mutex_unlock(&s_mls_q_mtx);
        free(job);
        return -1;
    }
    if (s_mls_q_tail) s_mls_q_tail->next=job;
    else s_mls_q_head=job;
    s_mls_q_tail=job;
    s_mls_q_bytes+=bytes;
    if (s_mls_q_bytes>s_mls_q_peak) s_mls_q_peak=s_mls_q_bytes;
    if (type==MLS_WRITE) s_mls_q_frames++;
    net_cond_broadcast(&s_mls_q_cond);
    int result=0;
    if (job->synchronous) {
        while (!job->done) mls_cond_wait(&s_mls_q_cond,&s_mls_q_mtx);
        result=job->result;
        free(job);
    }
    net_mutex_unlock(&s_mls_q_mtx);
    return result;
}

bool gui_net_local_save_end(bool commit) {
    if(!s_mls_lock_ready)return false;
    bool complete = false;
    if(s_mls_record && commit && !s_mls_failed) {
        complete = gui_net_local_save_send(MLS_COMMIT,0,0,NULL,0)==0;
        if(!complete)
            fprintf(stderr,"[Local Save] ERROR: client did not commit recording\n");
    } else if(s_mls_record && s_mls_active && !s_mls_failed) {
        (void)gui_net_local_save_send(MLS_ABORT,0,0,NULL,0);
    }
    if (s_mls_worker_joinable) {
        net_mutex_lock(&s_mls_q_mtx);
        s_mls_q_stop=true;
        net_cond_broadcast(&s_mls_q_cond);
        net_mutex_unlock(&s_mls_q_mtx);
        thrd_join(s_mls_worker,NULL);
        s_mls_worker_joinable=false;
        fprintf(stderr,"[Local Save] send queue peak %.1f MiB (limit 64 MiB); %" PRIu64 " queued writes\n",
                (double)s_mls_q_peak/(1024.0*1024.0),s_mls_q_frames);
    }
    net_mutex_lock(&s_mls_mtx);
    s_mls_record=false;
    net_mutex_unlock(&s_mls_mtx);
    return complete;
}

/* Capture-side diagnostic snapshot for an actual record-buffer overflow.
 * Never block on TCP here; lock queue and connection statistics separately.
 * No filesystem writes or dynamic allocations on the RF extraction thread. */
void gui_net_local_save_diagnostics(char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    if (!s_mls_lock_ready || !s_mls_q_ready) return;
    size_t queued = 0, peak = 0;
    bool failed = false;
    uint64_t acknowledged = 0;
    double last_ack = 0.0;
    net_mutex_lock(&s_mls_q_mtx);
    queued = s_mls_q_bytes;
    peak = s_mls_q_peak;
    net_mutex_unlock(&s_mls_q_mtx);
    net_mutex_lock(&s_mls_mtx);
    acknowledged = s_mls_write_bytes;
    last_ack = s_mls_last_ack_time;
    failed = s_mls_failed;
    net_mutex_unlock(&s_mls_mtx);
    double age = last_ack > 0.0 ? gui_runtime_time() - last_ack : 0.0;
    if (age < 0.0) age = 0.0;
    snprintf(buf, cap, "queue=%.1f/64MiB peak=%.1fMiB acked=%.1fMiB ack_age=%.1fs failed=%d",
             (double)queued / 1048576.0, (double)peak / 1048576.0,
             (double)acknowledged / 1048576.0, age, failed ? 1 : 0);
}

/* -------------------------------------------------------------------------
 * Broadcast fanout: one producer (capture tap), N subscribers (/rf clients).
 * Chunks are refcounted; a chunk is freed once no subscriber references it.
 * ------------------------------------------------------------------------- */

typedef struct net_chunk {
    size_t len;
    int refcount;
    struct net_chunk *next;
    uint8_t data[];
} net_chunk_t;

typedef struct {
    net_mutex_t mtx;
    net_cond_t cond;
    net_chunk_t *head;
    net_chunk_t *tail;
    int subscribers;
    bool shutdown;
    bool initialized;
} net_fanout_t;

typedef struct {
    net_fanout_t *f;
    net_chunk_t *cur;
    size_t off;
} net_fanout_sub_t;

static void net_fanout_init(net_fanout_t *f) {
    memset(f, 0, sizeof(*f));
    net_mutex_init(&f->mtx);
    net_cond_init(&f->cond);
    f->initialized = true;
}

static void net_fanout_free_chunk(net_chunk_t *c) {
    /* Decrement refcount; free if zero. Caller may pass NULL. */
    if (!c) return;
    if (--c->refcount <= 0) {
        free(c);
    }
}

static void net_fanout_destroy(net_fanout_t *f) {
    if (!f || !f->initialized) return;
    net_mutex_lock(&f->mtx);
    f->shutdown = true;
    net_cond_broadcast(&f->cond);
    net_chunk_t *c = f->head;
    while (c) {
        net_chunk_t *n = c->next;
        /* Drop producer reference. Subscribers should already be gone. */
        c->refcount = 0;
        free(c);
        c = n;
    }
    f->head = f->tail = NULL;
    net_mutex_unlock(&f->mtx);
    net_cond_destroy(&f->cond);
    net_mutex_destroy(&f->mtx);
    f->initialized = false;
}

static void net_fanout_subscribe(net_fanout_t *f, net_fanout_sub_t *s) {
    s->f = f;
    s->cur = NULL;
    s->off = 0;
    net_mutex_lock(&f->mtx);
    f->subscribers++;
    /* Start at head (oldest buffered) if any. */
    if (f->head) {
        s->cur = f->head;
        s->cur->refcount++;
    }
    net_mutex_unlock(&f->mtx);
}

static void net_fanout_unsubscribe(net_fanout_sub_t *s) {
    if (!s || !s->f) return;
    net_fanout_t *f = s->f;
    net_mutex_lock(&f->mtx);
    if (f->subscribers > 0) f->subscribers--;
    net_chunk_t *cur = s->cur;
    s->cur = NULL;
    s->off = 0;
    net_mutex_unlock(&f->mtx);
    net_fanout_free_chunk(cur);
    s->f = NULL;
}

/* Producer: append data. If there are no subscribers, drop it (no copy/alloc). */
static void net_fanout_push(net_fanout_t *f, const void *data, size_t len) {
    if (!f || !f->initialized || len == 0) return;
    net_mutex_lock(&f->mtx);
    if (f->shutdown || f->subscribers <= 0) {
        net_mutex_unlock(&f->mtx);
        return;
    }
    net_chunk_t *c = (net_chunk_t *)malloc(sizeof(net_chunk_t) + len);
    if (!c) {
        net_mutex_unlock(&f->mtx);
        return;
    }
    c->len = len;
    c->refcount = f->subscribers; /* each current subscriber will read it */
    c->next = NULL;
    memcpy(c->data, data, len);
    if (f->tail) f->tail->next = c; else f->head = c;
    f->tail = c;
    net_cond_broadcast(&f->cond);
    net_mutex_unlock(&f->mtx);
}

/* Subscriber: read up to cap bytes into dst. Blocks up to timeout_ms for data.
 * Returns bytes read (0 if timeout with no data), -1 on shutdown/EOF. */
static ssize_t net_fanout_read(net_fanout_sub_t *s, void *dst, size_t cap, int timeout_ms) {
    if (!s || !s->f) return -1;
    net_fanout_t *f = s->f;
    size_t copied = 0;
    net_mutex_lock(&f->mtx);
    while (cap > 0) {
        if (s->cur && s->off >= s->cur->len) {
            /* Advance to next chunk, release current. */
            net_chunk_t *next = s->cur->next;
            if (next) next->refcount++;
            net_chunk_t *done = s->cur;
            s->cur = next;
            s->off = 0;
            net_mutex_unlock(&f->mtx);
            net_fanout_free_chunk(done);
            net_mutex_lock(&f->mtx);
            continue;
        }
        if (s->cur) {
            size_t avail = s->cur->len - s->off;
            size_t n = (avail < cap) ? avail : cap;
            memcpy((char *)dst + copied, s->cur->data + s->off, n);
            s->off += n;
            copied += n;
            cap -= n;
            if (cap == 0) break;
            continue;
        }
        /* No current chunk: wait for producer or shutdown. */
        if (f->shutdown) {
            break;
        }
        if (timeout_ms <= 0) {
            break;
        }
        /* Wait with timeout. */
#ifdef _WIN32
        if (!SleepConditionVariableCS(&f->cond, &f->mtx, (unsigned long)timeout_ms)) {
            break;
        }
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int rc = pthread_cond_timedwait(&f->cond, &f->mtx, &ts);
        if (rc == ETIMEDOUT) {
            timeout_ms = 0;
            break;
        }
#endif
        /* Loop back: maybe a new chunk arrived or shutdown. */
        if (!s->cur && f->head) {
            s->cur = f->head;
            s->cur->refcount++;
        }
        if (f->shutdown) break;
    }
    net_mutex_unlock(&f->mtx);
    return (ssize_t)copied;
}

/* -------------------------------------------------------------------------
 * Net state
 * ------------------------------------------------------------------------- */

/* UDP discovery: servers broadcast a beacon on this port so clients can list
 * and select them without typing host:port. */
#define NET_DISCOVERY_PORT     8091
#define NET_MAX_DISCOVERED     8
#define NET_DISCOVERY_TTL_MS   8000

typedef struct {
    char     host[64];     /* IP address string of the server */
    uint16_t port;          /* TCP control port */
    char     name[64];      /* server hostname label */
    uint64_t last_seen_ms;
} net_discovered_t;

/* Server-owned capture settings. UI sends only explicit user edits; server
 * validates/applies them on its main thread, then mirrors authoritative state.
 * Values INT_MIN mean no pending request; resample rates are tenths of kHz. */
enum { GUI_NET_CTRL_COUNT = 25 };
static const char *const s_ctrl_keys[GUI_NET_CTRL_COUNT] = {
    "ab_swap",
    "capture_a",
    "capture_b",
    "misrc_mode",
    "use_flac",
    "flac_level",
    "flac_verify",
    "flac_threads",
    "overwrite",
    "rf_bits_a",
    "rf_bits_b",
    "resample_a",
    "resample_b",
    "resample_rate_a10",
    "resample_rate_b10",
    "auto_names_enabled", "append_timestamp_on_capture_start", "stop_on_dropout",
    "enable_audio_4ch", "enable_audio_2ch_12", "enable_audio_2ch_34",
    "enable_audio_1ch_1", "enable_audio_1ch_2", "enable_audio_1ch_3", "enable_audio_1ch_4"
};
static int gui_net_ctrl_find(const char *key) {
    if (!key) return -1;
    for (int i=0; i<GUI_NET_CTRL_COUNT; ++i)
        if (strcmp(s_ctrl_keys[i],key)==0) return i;
    return -1;
}
static bool gui_net_ctrl_valid(int id,int value) {
    switch(id) {
        case 0: return (value==0 || value==1);
        case 1: return (value==0 || value==1);
        case 2: return (value==0 || value==1);
        case 3: return (value==0 || value==1);
        case 4: return (value==0 || value==1);
        case 5: return value >= 0 && value <= 8;
        case 6: return (value==0 || value==1);
        case 7: return value >= 0 && value <= 64;
        case 8: return (value==0 || value==1);
        case 9: return (value==8 || value==12 || value==16);
        case 10: return (value==8 || value==12 || value==16);
        case 11: return (value==0 || value==1);
        case 12: return (value==0 || value==1);
        case 13: return value >= 1 && value <= 1000000;
        case 14: return value >= 1 && value <= 1000000;
        case 15:case 16:case 17:case 18:case 19:case 20:
        case 21:case 22:case 23:case 24: return value==0 || value==1;
        default: return false;
    }
}
static int gui_net_ctrl_value(const gui_app_t *app,int id) {
    switch(id) {
        case 0: return (int)app->settings.misrc_v15_v25_ab_swap;
        case 1: return (int)app->settings.capture_a;
        case 2: return (int)app->settings.capture_b;
        case 3: return (int)app->settings.misrc_mode;
        case 4: return (int)app->settings.use_flac;
        case 5: return (int)app->settings.flac_level;
        case 6: return (int)app->settings.flac_verification;
        case 7: return (int)app->settings.flac_threads;
        case 8: return (int)app->settings.overwrite_files;
        case 9: return (int)app->settings.rf_bits_a;
        case 10: return (int)app->settings.rf_bits_b;
        case 11: return (int)app->settings.enable_resample_a;
        case 12: return (int)app->settings.enable_resample_b;
        case 13: return (int)lroundf(app->settings.resample_rate_a*10.0f);
        case 14: return (int)lroundf(app->settings.resample_rate_b*10.0f);
        case 15: return app->settings.auto_names_enabled;
        case 16: return app->settings.append_timestamp_on_capture_start;
        case 17: return app->settings.stop_on_dropout;
        case 18: return app->settings.enable_audio_4ch;
        case 19: return app->settings.enable_audio_2ch_12;
        case 20: return app->settings.enable_audio_2ch_34;
        case 21:case 22:case 23:case 24: return app->settings.enable_audio_1ch[id-21];
        default: return 0;
    }
}
static void gui_net_ctrl_apply(gui_app_t *app,int id,int value) {
    switch(id) {
        case 0: app->settings.misrc_v15_v25_ab_swap = (bool)value; break;
        case 1: app->settings.capture_a = (bool)value; break;
        case 2: app->settings.capture_b = (bool)value; break;
        case 3: app->settings.misrc_mode = (bool)value; app->user_capture_mode_misrc=(bool)value; if(!app->is_recording) app->capture_mode_runtime_misrc=(bool)value; break;
        case 4: app->settings.use_flac = (bool)value; break;
        case 5: app->settings.flac_level = value; break;
        case 6: app->settings.flac_verification = (bool)value; break;
        case 7: app->settings.flac_threads = value; break;
        case 8: app->settings.overwrite_files = (bool)value; break;
        case 9: app->settings.rf_bits_a = value; break;
        case 10: app->settings.rf_bits_b = value; break;
        case 11: app->settings.enable_resample_a = (bool)value; break;
        case 12: app->settings.enable_resample_b = (bool)value; break;
        case 13: app->settings.resample_rate_a = (float)value/10.0f; break;
        case 14: app->settings.resample_rate_b = (float)value/10.0f; break;
        case 15: app->settings.auto_names_enabled = (bool)value; break;
        case 16: app->settings.append_timestamp_on_capture_start = (bool)value; break;
        case 17: app->settings.stop_on_dropout = (bool)value; break;
        case 18: app->settings.enable_audio_4ch = (bool)value; break;
        case 19: app->settings.enable_audio_2ch_12 = (bool)value; break;
        case 20: app->settings.enable_audio_2ch_34 = (bool)value; break;
        case 21:case 22:case 23:case 24: app->settings.enable_audio_1ch[id-21] = (bool)value; break;
        default: break;
    }
}

/* v7: Server-owned recording string settings. The client does not translate
 * path syntax: Windows drive/UNC and POSIX paths remain opaque UTF-8 strings.
 * Requests and mirrors are staged, never applied on socket worker threads. */
#define GUI_NET_BROWSE_MAX 12
#define GUI_NET_STR_COUNT 11
#define GUI_NET_STR_MAX FILE_PATH_MAX
static const char *const s_str_keys[GUI_NET_STR_COUNT]={
    "output_path", "output_base_name", "rf_tag_a", "rf_tag_b",
    "audio_tag_4ch", "audio_tag_2ch_12", "audio_tag_2ch_34",
    "audio_1ch_1_label", "audio_1ch_2_label", "audio_1ch_3_label", "audio_1ch_4_label"
};
static int gui_net_str_find(const char *key) {
    for(int i=0;i<GUI_NET_STR_COUNT;i++) if(!strcmp(key,s_str_keys[i]))return i;
    return -1;
}
static char *gui_net_str_field(gui_settings_t *s,int id,size_t *cap){
    switch(id){
        case 0:*cap=sizeof(s->output_path);return s->output_path;
        case 1:*cap=sizeof(s->output_base_name);return s->output_base_name;
        case 2:case 3:*cap=sizeof(s->rf_channel_tags[0]);return s->rf_channel_tags[id-2];
        case 4:case 5:case 6:*cap=sizeof(s->audio_output_tags[0]);return s->audio_output_tags[id-4];
        case 7:case 8:case 9:case 10:*cap=sizeof(s->audio_1ch_labels[0]);return s->audio_1ch_labels[id-7];
        default:*cap=0;return NULL;
    }
}
/* Encode UTF-8 bytes rather than serializing shell paths or raw query delimiters. */
static bool net_uri_encode(const char *input,char *out,size_t cap){
    static const char h[]="0123456789ABCDEF";
    size_t j=0;
    for(const unsigned char *p=(const unsigned char *)input;*p;p++){
        unsigned char c=*p;
        bool safe=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~';
        if(j+(safe?1u:3u)>=cap)return false;
        if(safe)out[j++]=(char)c;
        else{out[j++]='%';out[j++]=h[c>>4];out[j++]=h[c&15];}
    }
    if(j>=cap)return false;
    out[j]='\0';
    return true;
}
static int net_hex(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='A'&&c<='F')return c-'A'+10;
    if(c>='a'&&c<='f')return c-'a'+10;
    return -1;
}
static bool net_uri_decode(const char *input,char *out,size_t cap){
    size_t j=0;
    for(size_t i=0;input[i];i++){
        unsigned char c=(unsigned char)input[i];
        if(c=='%'){
            int a=net_hex(input[i+1]);
            if(!input[i+1]||!input[i+2]||a<0)return false;
            int b=net_hex(input[i+2]);if(b<0)return false;
            c=(unsigned char)((a<<4)|b);i+=2;
        }
        if(c==0||c<32||c==127||j+1>=cap)return false;
        out[j++]=(char)c;
    }
    out[j]=0;return true;
}
static bool net_validate_string(gui_app_t *app,int id,const char *value){
    if(!app||!value)return false;
    size_t cap=0;char *dst=gui_net_str_field(&app->settings,id,&cap);
    size_t n=strlen(value);
    if(!dst||n>=cap||(!n&&id<=1))return false;
    if(id==0){
        bool directory=false;
        if(!file_path_info_utf8(value,NULL,&directory)||!directory)return false;
        /* Probe actual write access on the SERVER, never client's OS.
         * Exclusive C11 mode prevents overwriting an existing file. */
        char probe[FILE_PATH_MAX];
        uint64_t stamp=get_time_ms();
        for(unsigned attempt=0;attempt<4;attempt++){
            int len=snprintf(probe,sizeof(probe),"%s/%s%llu-%u",
                value,".misrc-write-probe-",(unsigned long long)stamp,attempt);
            if(len<=0||(size_t)len>=sizeof(probe))return false;
            FILE *fp=file_open_utf8(probe,"wbx");
            if(fp){fclose(fp);file_remove_utf8(probe);return true;}
        }
        return false;
    }
    if(id!=0 && (strchr(value,'/')||strchr(value,'\\')))return false; /* filename/tag only */
    return true;
}
static size_t net_json_quoted(char *dst,size_t cap,const char *str){
    size_t pos=0;
    if(!cap)return 0;
    dst[pos++]='"';
    for(const unsigned char *p=(const unsigned char *)str;*p;p++){
        if(pos+7>=cap)return 0;
        if(*p=='"'||*p=='\\'){dst[pos++]='\\';dst[pos++]=(char)*p;}
        else if(*p<32){pos+=(size_t)snprintf(dst+pos,cap-pos,"\\u%04x",(unsigned)*p);}
        else dst[pos++]=(char)*p;
    }
    if(pos+2>cap)return 0;
    dst[pos++]='"';dst[pos]=0;return pos;
}
static bool net_json_get_str(const char *json,const char *key,char *dst,size_t cap){
    char pat[64];snprintf(pat,sizeof(pat),"\"%s\":\"",key);
    const char *p=strstr(json,pat);if(!p)return false;
    p+=strlen(pat);size_t n=0;
    while(*p&&*p!='"'){
        unsigned char c=(unsigned char)*p++;
        if(c=='\\'){
            c=(unsigned char)*p++;
            if(c=='u')return false; /* only UTF-8 or conventional escaped punctuation */
            if(c!='"'&&c!='\\'&&c!='/')return false;
        }
        if(n+1>=cap)return false;
        dst[n++]=(char)c;
    }
    if(*p!='"')return false;
    dst[n]=0;
    return true;
}

typedef struct {
    gui_app_t *app;
    uint16_t port;
    thrd_t listen_thread;
    atomic_bool running;
    atomic_bool stop_flag;
    net_sock_t listen_fd;
    net_fanout_t rf;
    net_fanout_t audio;
    atomic_int rf_clients;
    atomic_int audio_clients;
    atomic_int requested_controls[GUI_NET_CTRL_COUNT];
    atomic_int current_controls[GUI_NET_CTRL_COUNT];
    net_mutex_t str_mtx;
    char pending_strings[GUI_NET_STR_COUNT][GUI_NET_STR_MAX];
    bool pending_string[GUI_NET_STR_COUNT];
    char current_strings[GUI_NET_STR_COUNT][GUI_NET_STR_MAX];
    /* UDP discovery beacon broadcaster. */
    thrd_t beacon_thread;
    atomic_bool beacon_stop;
} net_server_t;

typedef struct {
    gui_app_t *app;
    char host[128];
    uint16_t port;
    thrd_t worker_thread;
    atomic_bool running;
    atomic_bool stop_flag;
    atomic_bool connected;
    atomic_bool error;
    /* Mirror snapshot (written by worker, read by main poll). */
    atomic_int peer_state;          /* 0 idle, 1 capturing, 2 recording */
    atomic_int peer_sample_rate;    /* Hz */
    atomic_int peer_device_count;
    atomic_int peer_selected;
    atomic_int peer_audio_frame_bytes; /* for /baseband re-framing */
    // Read-only /stats telemetry; no raw ingest is needed for GUI counters.
    atomic_uint_fast64_t peer_total_samples, peer_samples_a, peer_samples_b;
    atomic_uint_fast32_t peer_frames, peer_missed, peer_errors;
    atomic_uint_fast32_t peer_clip_a_pos, peer_clip_a_neg, peer_clip_b_pos, peer_clip_b_neg;
    atomic_uint_fast64_t peer_raw_a, peer_raw_b, peer_flac_a, peer_flac_b;
    atomic_uint_fast64_t peer_record_elapsed_ms, peer_last_record_ms;
    atomic_bool peer_synced;
    /* peer_free_known/peer_free_bytes also serve the existing remote FS browser;
     * keep their original declarations below (one shared snapshot per client). */
    atomic_int peer_rf_pct, peer_audio_pct;
    /* Staged mirrored device list (applied to app->devices by main thread). */
    net_mutex_t dev_mtx;
    device_info_t staged_devices[MAX_DEVICES];
    int staged_device_count;
    int staged_selected;
    bool staged_dirty;
    /* Ingest control (main thread starts/stops; pump threads run). */
    atomic_bool ingest_want;        /* legacy, always false in remote monitor mode */
    atomic_bool ingest_active;      /* main thread sets when ingest running */
    thrd_t rf_pump_thread;
    thrd_t audio_pump_thread;
    atomic_bool pump_stop;
    atomic_bool worker_started;   /* true once the stats/ingest worker thread is running */
    /* UDP discovery: listener thread + discovered-server list (mutex-guarded). */
    thrd_t discovery_thread;
    atomic_bool discovery_stop;
    net_mutex_t disc_mtx;
    net_discovered_t discovered[NET_MAX_DISCOVERED];
    int discovered_count;
    atomic_bool disc_dirty;
    atomic_int requested_controls[GUI_NET_CTRL_COUNT];
    atomic_int mirror_controls[GUI_NET_CTRL_COUNT];
    net_mutex_t str_mtx;
    char requested_strings[GUI_NET_STR_COUNT][GUI_NET_STR_MAX];
    bool requested_string[GUI_NET_STR_COUNT];
    bool waiting_string[GUI_NET_STR_COUNT];
    unsigned waiting_polls[GUI_NET_STR_COUNT];
    char mirrored_strings[GUI_NET_STR_COUNT][GUI_NET_STR_MAX];
    bool mirrored_string[GUI_NET_STR_COUNT];
    atomic_bool text_edit_active;
    atomic_bool browse_requested;
    char browse_relative[GUI_NET_STR_MAX];
    char browse_path[GUI_NET_STR_MAX];
    char browse_parent[GUI_NET_STR_MAX];
    bool browse_recovered;
    bool browse_computer;
    bool browse_error;
    unsigned browse_page;
    bool browse_has_more;
    atomic_bool peer_storage_seen;
    atomic_bool peer_directory_exists;
    atomic_bool peer_free_known;
    atomic_uint_fast64_t peer_free_bytes;
    char folders[GUI_NET_BROWSE_MAX][256];
    int folder_count;
    bool folders_ready;
    /* Forwarded-command flags are the shared app->net_cmd_* atomics; the
     * worker drains them and sends HTTP GETs. */
} net_client_t;

typedef struct {
    net_client_t *cli;
    bool is_audio;          /* false => /rf into BUF_CAPTURE_RF, true => /baseband into BUF_CAPTURE_AUDIO */
    buffer_id_t buf_id;
    int frame_bytes;        /* alignment for re-framing (4 for RF, peer_audio_frame_bytes for audio) */
} net_pump_ctx_t;

/* The single active net state (Local => NULL). Stored on app->net_state. */
static net_server_t *s_server = NULL;
static net_client_t *s_client = NULL;

/* Global init state. */
static bool s_globals_init = false;

void gui_net_init_globals(void) {
    if (!s_mls_lock_ready) { net_mutex_init(&s_mls_mtx); s_mls_lock_ready=true; }

    if (s_globals_init) return;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        s_globals_init = true;
    }
#else
    signal(SIGPIPE, SIG_IGN);
    s_globals_init = true;
#endif
}

void gui_net_cleanup_globals(void) {
    if (!s_globals_init) return;
#ifdef _WIN32
    WSACleanup();
#endif
    s_globals_init = false;
}

bool gui_net_is_client(const gui_app_t *app) {
    (void)app;
    return s_client != NULL;
}

bool gui_net_is_server(const gui_app_t *app) {
    (void)app;
    return s_server != NULL;
}

bool gui_net_active(const gui_app_t *app) {
    (void)app;
    if (s_server && atomic_load(&s_server->running)) return true;
    if (s_client && atomic_load(&s_client->connected)) return true;
    return false;
}

const char *gui_net_mode_name(int mode) {
    switch (mode) {
        case GUI_NET_MODE_SERVER: return "Server";
        case GUI_NET_MODE_CLIENT: return "Client";
        default: return "Local";
    }
}

/* Write the network status line shown ONLY in the info window's Network
 * section. Never touches app->status_message (the bottom status bar). */
void gui_net_set_status(gui_app_t *app, const char *msg) {
    if (!app || !msg) return;
    snprintf(app->net_status, sizeof(app->net_status), "%s", msg);
}

void gui_net_status_string(const gui_app_t *app, char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (s_server) {
        if (atomic_load(&s_server->running)) {
            snprintf(buf, len, "Listening on :%u", (unsigned)s_server->port);
        } else {
            snprintf(buf, len, "Server failed to listen on :%u", (unsigned)s_server->port);
        }
        return;
    }
    if (s_client) {
        if (atomic_load(&s_client->connected)) {
            snprintf(buf, len, "Connected to %s:%u", s_client->host, (unsigned)s_client->port);
        } else if (atomic_load(&s_client->worker_started) && s_client->host[0]) {
            snprintf(buf, len, "Connecting to %s:%u ...", s_client->host, (unsigned)s_client->port);
        } else if (atomic_load(&s_client->error)) {
            snprintf(buf, len, "Disconnected from %s:%u", s_client->host, (unsigned)s_client->port);
        } else if (s_client->host[0]) {
            snprintf(buf, len, "Client idle: %s:%u (press Connect)", s_client->host, (unsigned)s_client->port);
        } else {
            int n = 0;
            net_mutex_lock(&s_client->disc_mtx);
            n = s_client->discovered_count;
            net_mutex_unlock(&s_client->disc_mtx);
            if (n > 0) {
                snprintf(buf, len, "Client mode: %d server(s) found - select one", n);
            } else {
                snprintf(buf, len, "Client mode: scanning for servers on the LAN...");
            }
        }
        return;
    }
    if (app && app->net_status[0]) {
        snprintf(buf, len, "%s", app->net_status);
        return;
    }
    if (app) {
        if (app->settings.net_mode == GUI_NET_MODE_SERVER) {
            snprintf(buf, len, "Server mode: not active");
            return;
        }
        if (app->settings.net_mode == GUI_NET_MODE_CLIENT) {
            snprintf(buf, len, "Client mode: starting discovery...");
            return;
        }
    }
    snprintf(buf, len, "Local (no network)");
}

/* -------------------------------------------------------------------------
 * Server: HTTP request handling
 * ------------------------------------------------------------------------- */

/* Build a single lightweight metadata snapshot. This is control-plane
 * telemetry: the monitor client must not pull the RF/audio stream for stats.
 * New fields are additive, so existing clients continue to work. */
static int server_ring_pct(const ringbuffer_t *buf) {
    size_t size = buf->buffer_size;
    if (!size) return 0;
    size_t head = atomic_load(&buf->head);
    size_t tail = atomic_load(&buf->tail);
    size_t used = tail >= head ? tail - head : 0;
    if (used > size) used = size;
    return (int)(100 * used / size);
}

static void server_build_stats(gui_app_t *app, char *buf, size_t len) {
    int state = app->is_recording ? 2 : (app->is_capturing ? 1 : 0);
    uint32_t sr = atomic_load(&app->sample_rate);
    uint64_t total = atomic_load(&app->total_samples);
    uint32_t frames = atomic_load(&app->frame_count);
    uint32_t errors = atomic_load(&app->error_count);
    int sel = app->selected_device, dcount = app->device_count;
    char dname[80] = "none";
    int dtype = -1;
    if (sel >= 0 && sel < dcount) {
        snprintf(dname, sizeof(dname), "%s", app->devices[sel].name);
        dtype = (int)app->devices[sel].type;
    }
    double elapsed = 0.0;
    if (app->is_recording) elapsed = gui_runtime_time() - app->recording_start_time;
    if (elapsed < 0.0) elapsed = 0.0;
    double last_elapsed = app->last_recording_duration_s;
    if (last_elapsed < 0.0) last_elapsed = 0.0;
    uint64_t free_bytes = 0;
    bool free_known = gui_record_get_output_free_space_bytes(app, &free_bytes);
    // Capture callback pads 24-bit / four-channel audio to 12 bytes.
    snprintf(buf, len,
        "{\"state\":%d,\"sample_rate\":%u,\"total_samples\":%llu,"
        "\"frames\":%u,\"errors\":%u,\"selected_device\":%d,"
        "\"device_count\":%d,\"device_name\":\"%s\",\"device_type\":%d,"
        "\"audio_frame_bytes\":12,"
        "\"samples_a\":%llu,\"samples_b\":%llu,\"missed\":%u,\"synced\":%s,"
        "\"raw_a\":%llu,\"raw_b\":%llu,\"flac_a\":%llu,\"flac_b\":%llu,"
        "\"clip_a_pos\":%u,\"clip_a_neg\":%u,\"clip_b_pos\":%u,\"clip_b_neg\":%u,"
        "\"record_elapsed_ms\":%llu,\"last_record_ms\":%llu,"
        "\"rf_pct\":%d,\"audio_pct\":%d,\"free_known\":%s,\"free_bytes\":%llu}",
        state, (unsigned)sr, (unsigned long long)total,
        (unsigned)frames, (unsigned)errors, sel, dcount, dname, dtype,
        (unsigned long long)atomic_load(&app->samples_a),
        (unsigned long long)atomic_load(&app->samples_b),
        (unsigned)atomic_load(&app->missed_frame_count),
        atomic_load(&app->stream_synced) ? "true" : "false",
        (unsigned long long)atomic_load(&app->recording_raw_a),
        (unsigned long long)atomic_load(&app->recording_raw_b),
        (unsigned long long)atomic_load(&app->recording_compressed_a),
        (unsigned long long)atomic_load(&app->recording_compressed_b),
        (unsigned)atomic_load(&app->clip_count_a_pos),
        (unsigned)atomic_load(&app->clip_count_a_neg),
        (unsigned)atomic_load(&app->clip_count_b_pos),
        (unsigned)atomic_load(&app->clip_count_b_neg),
        (unsigned long long)(elapsed * 1000.0),
        (unsigned long long)(last_elapsed * 1000.0),
        server_ring_pct(&app->buffers.buffers[BUF_CAPTURE_RF]),
        server_ring_pct(&app->buffers.buffers[BUF_CAPTURE_AUDIO]),
        free_known ? "true" : "false", (unsigned long long)free_bytes);
}

static void server_build_devices(gui_app_t *app, char *buf, size_t len) {
    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off, "{\"count\":%d,\"selected\":%d,\"devices\":[",
                            app->device_count, app->selected_device);
    for (int i = 0; i < app->device_count && off + 64 < len; i++) {
        const device_info_t *d = &app->devices[i];
        off += (size_t)snprintf(buf + off, len - off,
            "%s{\"index\":%d,\"type\":%d,\"name\":\"%s\"}",
            (i ? "," : ""), i, (int)d->type, d->name);
    }
    if (off < len - 2) {
        buf[off++] = ']';
        buf[off++] = '}';
        buf[off] = '\0';
    } else if (len > 0) {
        buf[len - 1] = '\0';
    }
}

static void server_build_controls(gui_app_t *app, char *buf, size_t len) {
    size_t off=0;
    if (!buf || len==0) return;
    off += (size_t)snprintf(buf,len,"{");
    for(int id=0;id<GUI_NET_CTRL_COUNT && off<len;id++) {
        int value=s_server ? atomic_load(&s_server->current_controls[id]) : gui_net_ctrl_value(app,id);
        bool is_bool=(id==0 || id==1 || id==2 || id==3 || id==4 || id==6 ||
                      id==8 || id==11 || id==12 || id>=15);
        int n=snprintf(buf+off,len-off,"%s\"%s\":%s",id?",":"",s_ctrl_keys[id],
                       is_bool?(value?"true":"false"):"");
        if(n<0 || (size_t)n>=len-off) {buf[len-1]='\0';return;}
        off+=(size_t)n;
        if(!is_bool){
            n=snprintf(buf+off,len-off,"%d",value);
            if(n<0 || (size_t)n>=len-off){buf[len-1]='\0';return;}
            off+=(size_t)n;
        }
    }
    if(off+2<=len){buf[off++]='}';buf[off]='\0';}else buf[len-1]='\0';
}

/* Parse an integer query arg like "?N" or "?on=1" from the URI. Returns the
 * integer found, or def. The reference splits on '&'; we only need one arg. */
static int server_parse_arg(const char *uri_after_path, const char *key, int def) {
    if (!uri_after_path) return def;
    /* uri_after_path points at the char after '?'. Look for key= or a bare number. */
    const char *p = uri_after_path;
    if (key && key[0]) {
        size_t klen = strlen(key);
        while (p && *p) {
            if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
                return atoi(p + klen + 1);
            }
            p = strchr(p, '&');
            if (!p) break;
            p++;
        }
        return def;
    }
    /* Bare numeric arg (e.g. /device?2). */
    return atoi(p);
}

/* Split URI into path + query (in-place in a local copy). */
static void server_handle_request(gui_app_t *app, net_sock_t fd, const char *method, char *uri) {
    if (0 != strcmp(method, "GET")) {
        net_send_str(fd, "HTTP/1.0 405 Method Not Allowed\r\n\r\n");
        return;
    }
    char *query = strchr(uri, '?');
    if (query) *query++ = '\0';

    if (strcmp(uri, "/local-save/attach") == 0) {
        /* A single local-save receiver per server. This endpoint is LAN-only,
         * matching existing unauthenticated capture controls. */
        if (!s_mls_lock_ready) { net_send_str(fd,"HTTP/1.0 503 Service Unavailable\r\n\r\n"); return; }
        net_mutex_lock(&s_mls_mtx);
        if (s_mls_active || s_mls_record) {
            net_mutex_unlock(&s_mls_mtx);
            net_send_str(fd,"HTTP/1.0 409 Conflict\r\n\r\n");return;
        }
        mls_enable_tcp_nodelay(fd);
        /* A lost client ACK must not wedge the sender indefinitely while RF
         * buffers overflow. Only enable bounded I/O on the server data socket. */
        net_set_timeouts(fd,15000,15000);
        s_mls_fd=fd;s_mls_active=true;s_mls_failed=false;
        net_mutex_unlock(&s_mls_mtx);
        net_send_str(fd,"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n");
        /* Keep socket ownership until session ends. Only the sender accesses
         * the socket's receive side (ACKs). */
        while (s_server && !atomic_load(&s_server->stop_flag)) {
            net_mutex_lock(&s_mls_mtx);
            bool attached=s_mls_active && s_mls_fd==fd;
            net_mutex_unlock(&s_mls_mtx);
            if(!attached) break;
            if (!s_mls_record) {
                /* When idle only: do not consume the sender's ACKs. */
                fd_set rfds;FD_ZERO(&rfds);FD_SET(fd,&rfds);
                struct timeval z={0,0};
                if(select((int)fd+1,&rfds,NULL,NULL,&z)>0){
                    char c=0;
                    int available=recv(fd,&c,1,MSG_PEEK);
                    if(available<=0)break;
                }
            }
            thrd_sleep_ms(100);
        }
        net_mutex_lock(&s_mls_mtx);
        if (s_mls_fd==fd) { s_mls_fd=NET_INVALID_SOCKET;s_mls_active=false;s_mls_failed=true; }
        net_mutex_unlock(&s_mls_mtx);
        return;
    }
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/version") == 0) {
        char body[128];
        snprintf(body, sizeof(body), "MISRC %s\r\n", MIRSC_TOOLS_VERSION);
        char hdr[128];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n",
            strlen(body));
        net_send_str(fd, hdr);
        net_send_str(fd, body);
        return;
    }
    if (strcmp(uri, "/stats") == 0) {
        char json[2048];
        server_build_stats(app, json, sizeof(json));
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/json\r\nContent-Length: %zu\r\n\r\n",
            strlen(json));
        net_send_str(fd, hdr);
        net_send_str(fd, json);
        return;
    }
    if (strcmp(uri, "/devices") == 0) {
        char *json = (char *)malloc(8192);
        if (!json) { net_send_str(fd, "HTTP/1.0 500 Internal Error\r\n\r\n"); return; }
        server_build_devices(app, json, 8192);
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/json\r\nContent-Length: %zu\r\n\r\n",
            strlen(json));
        net_send_str(fd, hdr);
        net_send_str(fd, json);
        free(json);
        return;
    }
    if (strcmp(uri, "/set") == 0) {
        /* LAN-only API, explicitly allowlisted. HTTP threads enqueue only;
         * main/headless loop owns settings and capture engine state. */
        int id=-1, value=0;
        if (query && !strchr(query,'&')) {
            const char *eq=strchr(query,'=');
            if (eq && (size_t)(eq-query)<64) {
                char key[64]; size_t k=(size_t)(eq-query);
                memcpy(key,query,k);key[k]='\0';
                id=gui_net_ctrl_find(key);
                if (id>=0) {
                    char *end=NULL;errno=0;
                    long parsed=strtol(eq+1,&end,10);
                    if(errno!=0 || end==eq+1 || *end || parsed<INT_MIN || parsed>INT_MAX ||
                       !gui_net_ctrl_valid(id,(int)parsed)) id=-1;
                    else value=(int)parsed;
                }
            }
        }
        if (id<0 || !s_server) {
            net_send_str(fd,"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");return;
        }
        atomic_store(&s_server->requested_controls[id],value);
        net_send_str(fd,"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 7\r\n\r\nqueued\n");
        return;
    }
    if (strcmp(uri, "/controls") == 0) {
        char json[2048];
        server_build_controls(app, json, sizeof(json));
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/json\r\nContent-Length: %zu\r\n\r\n",
            strlen(json));
        net_send_str(fd, hdr);
        net_send_str(fd, json);
        return;
    }
    if(strcmp(uri,"/server/info")==0){
#ifdef _WIN32
        const char *os="windows",*arch="x86_64",*sep="\\";
#elif defined(__APPLE__)
        const char *os="macos",*sep="/";
        #if defined(__aarch64__) || defined(__arm64__)
          const char *arch="aarch64";
        #else
          const char *arch="x86_64";
        #endif
#else
        const char *os="linux",*sep="/";
        #if defined(__aarch64__)
          const char *arch="aarch64";
        #elif defined(__x86_64__)
          const char *arch="x86_64";
        #else
          const char *arch="other";
        #endif
#endif
        char body[256];
        snprintf(body,sizeof(body),"{\"os\":\"%s\",\"arch\":\"%s\",\"path_separator\":\"%s\",\"settings_api\":7}",
                 os,arch,sep[0]=='\\'?"\\\\":sep);
        char hdr[160];snprintf(hdr,sizeof(hdr),"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",strlen(body));
        net_send_str(fd,hdr);net_send_str(fd,body);return;
    }
    if(strcmp(uri,"/settings")==0){
        char body[16384];size_t off=0;
        off+=(size_t)snprintf(body,sizeof(body),"{\"settings_api\":7");
        if(!s_server){net_send_str(fd,"HTTP/1.0 503 Service Unavailable\r\n\r\n");return;}
        net_mutex_lock(&s_server->str_mtx);
        for(int id=0;id<GUI_NET_STR_COUNT;id++){
            int n=snprintf(body+off,sizeof(body)-off,",\"%s\":",s_str_keys[id]);
            if(n<0||(size_t)n>=sizeof(body)-off)break;
            off+=(size_t)n;
            size_t m=net_json_quoted(body+off,sizeof(body)-off,s_server->current_strings[id]);
            if(!m)break;off+=m;
        }
        net_mutex_unlock(&s_server->str_mtx);
        if(off+2>=sizeof(body)){net_send_str(fd,"HTTP/1.0 500 Internal Error\r\n\r\n");return;}
        body[off++]='}';body[off]=0;
        char hdr[160];snprintf(hdr,sizeof(hdr),"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",off);
        net_send_str(fd,hdr);net_send_str(fd,body);return;
    }
    if(strcmp(uri,"/settings/set")==0){
        /* Only the names in s_str_keys are writable. Never interpret the
         * submitted string as a command or a client-side path. */
        char decoded[GUI_NET_STR_MAX];char key[64]={0};int id=-1;
        const char *sep=query?strstr(query,"&value="):NULL;
        if(sep && !strncmp(query,"key=",4) && (size_t)(sep-(query+4))<sizeof(key)){
            size_t n=(size_t)(sep-(query+4));memcpy(key,query+4,n);key[n]=0;
            id=gui_net_str_find(key);
        }
        if(id<0||!net_uri_decode(sep?sep+7:"",decoded,sizeof(decoded))||
           !gui_net_str_field(&app->settings,id,&(size_t){0})){
            net_send_str(fd,"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");return;
        }
        if(app->is_recording){net_send_str(fd,"HTTP/1.0 409 Conflict\r\nContent-Length: 0\r\n\r\n");return;}
        if(!net_validate_string(app,id,decoded)){
            net_send_str(fd,"HTTP/1.0 422 Unprocessable Entity\r\nContent-Length: 0\r\n\r\n");return;
        }
        net_mutex_lock(&s_server->str_mtx);
        snprintf(s_server->pending_strings[id],GUI_NET_STR_MAX,"%s",decoded);
        s_server->pending_string[id]=true;
        net_mutex_unlock(&s_server->str_mtx);
        net_send_str(fd,"HTTP/1.0 200 OK\r\nContent-Length: 7\r\n\r\nqueued\n");return;
    }
    if(strcmp(uri,"/fs/browse")==0||strcmp(uri,"/fs/subdirs")==0){
        /* v7.3: absolute SERVER-side navigation. output_path is only a save
         * destination; it must never constrain the browsing root.  This
         * exposes directory names only and runs with server process rights.
         * The HTTP API is LAN-only: do not expose the port to the Internet. */
        char selected[GUI_REMOTE_FS_PATH]="";unsigned page=0;
        if(query){
            if(strncmp(query,"at=",3)!=0){net_send_str(fd,"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");return;}
            const char *pg=strstr(query,"&page=");
            char enc[GUI_REMOTE_FS_PATH*3];
            size_t n=pg?(size_t)(pg-(query+3)):strlen(query+3);
            if(n>=sizeof(enc)){net_send_str(fd,"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");return;}
            memcpy(enc,query+3,n);enc[n]=0;
            if(!net_uri_decode(enc,selected,sizeof(selected))){net_send_str(fd,"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");return;}
            if(pg){
                char *end=NULL;unsigned long u=strtoul(pg+6,&end,10);
                if(end==pg+6||*end||u>100000){net_send_str(fd,"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");return;}
                page=(unsigned)u;
            }
        }
        /* Keep allocations off per-connection thread stacks. */
        gui_remote_fs_result_t *result=calloc(1,sizeof(*result));
        if(!result){net_send_str(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");return;}
        if(!gui_remote_fs_browse(selected,page,result)){
            free(result);net_send_str(fd,"HTTP/1.0 403 Forbidden\r\nContent-Length: 0\r\n\r\n");return;
        }
        char body[GUI_NET_BROWSE_MAX*2048];size_t off=0;
        off=(size_t)snprintf(body,sizeof(body),"{\"count\":%u,\"page\":%u,\"has_more\":%s,\"recovered\":%s,\"computer\":%s,\"path\":",
            result->count,result->page,result->has_more?"true":"false",
            result->recovered?"true":"false",result->computer?"true":"false");
        size_t m=net_json_quoted(body+off,sizeof(body)-off,result->path);
        if(!m){free(result);net_send_str(fd,"HTTP/1.0 500 Internal Server Error\r\n\r\n");return;}off+=m;
        off+=(size_t)snprintf(body+off,sizeof(body)-off,",\"parent\":");
        m=net_json_quoted(body+off,sizeof(body)-off,result->parent);
        if(!m){free(result);net_send_str(fd,"HTTP/1.0 500 Internal Server Error\r\n\r\n");return;}off+=m;
        for(unsigned i=0;i<result->count;i++){
            int n=snprintf(body+off,sizeof(body)-off,",\"dir%u\":",i);
            if(n<0||(size_t)n>=sizeof(body)-off)break;off+=(size_t)n;
            m=net_json_quoted(body+off,sizeof(body)-off,result->names[i]);
            if(!m)break;off+=m;
        }
        free(result);
        if(off+2>=sizeof(body)){net_send_str(fd,"HTTP/1.0 500 Internal Server Error\r\n\r\n");return;}
        body[off++]='}';body[off]=0;
        char hdr[192];snprintf(hdr,sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",off);
        net_send_str(fd,hdr);net_send_str(fd,body);return;
    }
    if(strcmp(uri,"/fs/status")==0){
        /* Read-only server-owned output directory status. */
        char output[GUI_NET_STR_MAX];
        net_mutex_lock(&s_server->str_mtx);
        snprintf(output,sizeof(output),"%s",s_server->current_strings[0]);
        net_mutex_unlock(&s_server->str_mtx);
        bool exists=false;file_path_info_utf8(output,NULL,&exists);
        uint64_t free_bytes=0;
        bool free_ok=gui_record_get_output_free_space_bytes(app,&free_bytes);
        char body[GUI_NET_STR_MAX*2];size_t off=(size_t)snprintf(body,sizeof(body),"{\"path\":");
        size_t n=net_json_quoted(body+off,sizeof(body)-off,output);
        if(!n){net_send_str(fd,"HTTP/1.0 500 Internal Error\r\n\r\n");return;}
        off+=n;off+=(size_t)snprintf(body+off,sizeof(body)-off,
            ",\"directory_exists\":%s,\"free_bytes\":%llu,\"free_known\":%s}",
            exists?"true":"false",(unsigned long long)free_bytes,free_ok?"true":"false");
        char hdr[160];snprintf(hdr,sizeof(hdr),"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",off);
        net_send_str(fd,hdr);net_send_str(fd,body);return;
    }
    if (strcmp(uri,"/preview/display") == 0) {
        if(!app->is_capturing){
            net_send_str(fd,"HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");return;
        }
        gui_rd_config_t cfg[2];
        for(int c=0;c<2;c++){
            cfg[c]=(gui_rd_config_t){32.f,GUI_RD_WIDTH,0,0,0,c};
            cfg[c].zoom=(float)server_parse_arg(query,c?"zb":"za",32000)/1000.f;
            cfg[c].width=(uint32_t)server_parse_arg(query,c?"wb":"wa",GUI_RD_WIDTH);
            cfg[c].trigger_enabled=server_parse_arg(query,c?"tb":"ta",0);
            cfg[c].trigger_level=server_parse_arg(query,c?"lb":"la",0);
            cfg[c].trigger_mode=server_parse_arg(query,c?"mb":"ma",0);
            cfg[c].trigger_source=server_parse_arg(query,c?"sb":"sa",c);
        }
        void *wire=NULL;size_t bytes=0,original=0;
        if(!gui_rd_server_build(cfg,&wire,&bytes,&original)){
            net_send_str(fd,"HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");return;
        }
        char hdr[256];
        snprintf(hdr,sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: application/zstd\r\nCache-Control: no-store\r\n"
            "X-Remote-Raw-Bytes: %zu\r\nX-Remote-Compressed-Bytes: %zu\r\nContent-Length: %zu\r\n\r\n",
            original,bytes,bytes);
        if(net_send_str(fd,hdr)==0)(void)net_send_all(fd,wire,bytes);
        static atomic_uint rd_transfer_count=ATOMIC_VAR_INIT(0);
        static atomic_uint_fast64_t rd_transfer_bytes=ATOMIC_VAR_INIT(0);
        unsigned cnt=atomic_fetch_add(&rd_transfer_count,1)+1;
        atomic_fetch_add(&rd_transfer_bytes,bytes);
        if(cnt%100==0){
            uint64_t total=atomic_exchange(&rd_transfer_bytes,0);
            fprintf(stderr,"[RemoteMonitor v7] latest %u frames: compressed %.1f KiB/frame, ratio %.1f%% (Zstd-1)\n",
                    100,(double)total/100.0/1024.0,100.0*(double)total/100.0/(double)original);
        }
        free(wire);return;
    }
    if (strcmp(uri, "/preview/waveform") == 0) {
        /* Real input for the original Waveform renderer, never raw RF streaming.
         * Allocate outside the server handler stack due to fixed frame size. */
        gui_rm_scope_packet_t *packet = malloc(sizeof(*packet));
        if (!packet) {
            net_send_str(fd, "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        if (!app->is_capturing || !gui_rm_scope_server_snapshot(packet)) {
            net_send_str(fd, "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
            free(packet);
            return;
        }
        char hdr[192];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                 "Cache-Control: no-store\r\nContent-Length: %zu\r\n\r\n", sizeof(*packet));
        if (net_send_str(fd, hdr) == 0) (void)net_send_all(fd, packet, sizeof(*packet));
        free(packet);
        return;
    }
    if (strcmp(uri, "/preview/monitor") == 0) {
        /* Binary, fixed-size and versioned. No network client can cause the
         * capture path to block. Monitoring is never a streaming /rf feed. */
        gui_rm_packet_t packet;
        if (!app->is_capturing || !gui_rm_server_snapshot(&packet)) {
            net_send_str(fd, "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        char hdr[192];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
            "Cache-Control: no-store\r\nContent-Length: %zu\r\n\r\n", sizeof(packet));
        if (net_send_str(fd, hdr) == 0) (void)net_send_all(fd, &packet, sizeof(packet));
        return;
    }
    if (strcmp(uri, "/start") == 0) {
        atomic_store(&app->net_cmd_start, true);
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nstart requested\r\n");
        return;
    }
    if (strcmp(uri, "/stop") == 0) {
        atomic_store(&app->net_cmd_stop, true);
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nstop requested\r\n");
        return;
    }
    if (strcmp(uri, "/record") == 0) {
        int on = server_parse_arg(query, "on", 1);
        if (on) {
            atomic_store(&s_mls_request, query && strstr(query,"local=1") != NULL);
            atomic_store(&app->net_cmd_record_on, true);
        }
        else atomic_store(&app->net_cmd_record_off, true);
        const char *msg = on ? "record on requested\r\n" : "record off requested\r\n";
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", strlen(msg));
        net_send_str(fd, hdr);
        net_send_str(fd, msg);
        return;
    }
    if (strcmp(uri, "/device") == 0) {
        int n = server_parse_arg(query, NULL, 0);
        atomic_store(&app->net_cmd_select_device, true);
        atomic_store(&app->net_cmd_device_index, n);
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\ndevice select requested\r\n");
        return;
    }
    if (strcmp(uri, "/rf") == 0 || strcmp(uri, "/baseband") == 0) {
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n");
        net_fanout_t *f = (uri[1] == 'r') ? &s_server->rf : &s_server->audio;
        atomic_int *ctr = (uri[1] == 'r') ? &s_server->rf_clients : &s_server->audio_clients;
        atomic_fetch_add(ctr, 1);
        net_fanout_sub_t sub;
        net_fanout_subscribe(f, &sub);
        uint8_t buf[64 * 1024];
        for (;;) {
            ssize_t n = net_fanout_read(&sub, buf, sizeof(buf), 1000);
            if (n < 0) break;
            if (n == 0) {
                /* No data for 1s; probe the socket so a disconnected client
                 * is detected even when the server has no data to push
                 * (standby/idle feed). select() for read-ready: if the peer
                 * closed, recv would return 0 (read-ready, no data); if the
                 * socket is still open and idle, select times out (not
                 * read-ready). Without this the /rf thread lingered forever
                 * holding the subscription until a hard app close. */
                fd_set rset;
                FD_ZERO(&rset);
                FD_SET(fd, &rset);
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                int sr = select((int)fd + 1, &rset, NULL, NULL, &tv);
                if (sr > 0 && FD_ISSET(fd, &rset)) {
                    char probe[1];
#ifdef _WIN32
                    int pn = recv(fd, probe, 1, 0 /* normal */);
                    if (pn == 0 || pn == SOCKET_ERROR) break;
#else
                    ssize_t pn = recv(fd, probe, 1, MSG_PEEK);
                    if (pn == 0 || (pn < 0 && errno != EINTR)) break;
#endif
                }
                continue;
            }
            if (net_send_all(fd, buf, (size_t)n) != 0) break;
        }
        net_fanout_unsubscribe(&sub);
        atomic_fetch_sub(ctr, 1);
        return;
    }
    net_send_str(fd, "HTTP/1.0 404 Not Found\r\n\r\n");
}

/* Per-client thread: read one request, serve it, close. For /rf + /baseband
 * the serve loops until the client disconnects. */
static int server_client_thread(void *arg) {
    typedef struct { gui_app_t *app; net_sock_t fd; } ctx_t;
    ctx_t *c = (ctx_t *)arg;
    gui_app_t *app = c->app;
    net_sock_t fd = c->fd;
    free(c);

    /* Bound recv/send so a stuck/missing client cannot hold this (detached)
     * thread forever on a blocking call. */
    net_set_timeouts(fd, 5000, 5000);

    char buf[GUI_NET_STR_MAX*4];
    int len = net_read_headers(fd, buf, sizeof(buf));
    if (len <= 0) {
        net_close(fd);
        return 0;
    }
    char method[8] = {0};
    char uri[GUI_NET_STR_MAX*3] = {0};
    int v1 = 0, v2 = 0;
    if (4 != sscanf(buf, "%7s %12287s HTTP/%d.%d", method, uri, &v1, &v2)) {
        net_send_str(fd, "HTTP/1.0 400 Bad Request\r\n\r\n");
        net_close(fd);
        return 0;
    }
    server_handle_request(app, fd, method, uri);
    net_close(fd);
    return 0;
}

static int server_listen_thread(void *arg) {
    net_server_t *srv = (net_server_t *)arg;
    gui_app_t *app = srv->app;
    /* Non-blocking accept loop: select() with a short timeout so this thread
     * checks stop_flag frequently and thrd_join() returns within ~200ms on
     * server_stop(). A blocking accept() would never wake from close() alone
     * on POSIX (the thread holds its own fd reference), freezing the UI. */
    net_set_nonblocking(srv->listen_fd);
    while (!atomic_load(&srv->stop_flag)) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(srv->listen_fd, &rset);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  /* 200ms */
        int sr = select((int)srv->listen_fd + 1, &rset, NULL, NULL, &tv);
        if (sr <= 0) {
            /* timeout (sr==0) or error: re-check stop_flag and loop */
            continue;
        }
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        net_sock_t cfd = accept(srv->listen_fd, (struct sockaddr *)&caddr, &clen);
        if (!net_sock_valid(cfd)) {
            if (atomic_load(&srv->stop_flag)) break;
            continue;
        }
        typedef struct { gui_app_t *app; net_sock_t fd; } ctx_t;
        ctx_t *c = (ctx_t *)malloc(sizeof(ctx_t));
        if (!c) { net_close(cfd); continue; }
        c->app = app;
        c->fd = cfd;
        thrd_t t;
        if (thrd_create(&t, server_client_thread, c) != thrd_success) {
            net_close(cfd);
            free(c);
            continue;
        }
        net_thread_detach(&t);
    }
    return 0;
}

/* UDP discovery beacon: broadcast "MISRC\n<tcpport>\n<hostname>" every 2s to
 * NET_DISCOVERY_PORT so clients on the LAN can list and select this server
 * without typing host:port. */
static int server_beacon_thread(void *arg) {
    net_server_t *srv = (net_server_t *)arg;
    net_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!net_sock_valid(fd)) return 0;
    int bcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));
    char name[128] = {0};
    gethostname(name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char pkt[256];
    int plen = snprintf(pkt, sizeof(pkt), "MISRC\n%u\n%s", (unsigned)srv->port, name);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = INADDR_BROADCAST;
    dst.sin_port = htons(NET_DISCOVERY_PORT);
    while (!atomic_load(&srv->beacon_stop)) {
        sendto(fd, pkt, plen, 0, (struct sockaddr *)&dst, sizeof(dst));
        /* Sleep 2s in 100ms slices so beacon_stop is seen promptly on stop. */
        for (int i = 0; i < 20 && !atomic_load(&srv->beacon_stop); i++) {
            thrd_sleep_ms(100);
        }
    }
    net_close(fd);
    return 0;
}

static int server_start(gui_app_t *app, uint16_t port) {
    if (s_server) return 0;
    net_server_t *srv = (net_server_t *)calloc(1, sizeof(*srv));
    if (!srv) return -1;
    srv->app = app;
    srv->port = port;
    srv->listen_fd = NET_INVALID_SOCKET;
    atomic_store(&srv->running, false);
    atomic_store(&srv->stop_flag, false);
    atomic_store(&srv->rf_clients, 0);
    atomic_store(&srv->audio_clients, 0);
    for(int i=0;i<GUI_NET_CTRL_COUNT;i++) {
        atomic_init(&srv->requested_controls[i],INT_MIN);
        atomic_init(&srv->current_controls[i],gui_net_ctrl_value(app,i));
    }
    net_mutex_init(&srv->str_mtx);
    for(int id=0;id<GUI_NET_STR_COUNT;id++){
        size_t cap=0;char *field=gui_net_str_field(&app->settings,id,&cap);
        if(field)snprintf(srv->current_strings[id],GUI_NET_STR_MAX,"%s",field);
    }
    atomic_store(&srv->beacon_stop, false);
    net_fanout_init(&srv->rf);
    net_fanout_init(&srv->audio);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!net_sock_valid(srv->listen_fd)) {
        fprintf(stderr, "[NET] server: socket() failed\n");
        net_mutex_destroy(&srv->str_mtx);
        free(srv);
        return -1;
    }
    int reuse = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[NET] server: bind(:%u) failed\n", (unsigned)port);
        net_close(srv->listen_fd);
        net_mutex_destroy(&srv->str_mtx);
        free(srv);
        return -1;
    }
    if (listen(srv->listen_fd, 16) < 0) {
        fprintf(stderr, "[NET] server: listen() failed\n");
        net_close(srv->listen_fd);
        net_mutex_destroy(&srv->str_mtx);
        free(srv);
        return -1;
    }
    if (thrd_create(&srv->listen_thread, server_listen_thread, srv) != thrd_success) {
        net_close(srv->listen_fd);
        net_mutex_destroy(&srv->str_mtx);
        free(srv);
        return -1;
    }
    atomic_store(&srv->running, true);
    /* Start the UDP discovery beacon so clients can find us. */
    if (thrd_create(&srv->beacon_thread, server_beacon_thread, srv) != thrd_success) {
        fprintf(stderr, "[NET] server: beacon thread failed (non-fatal)\n");
    }
    s_server = srv;
    app->net_state = srv;
    atomic_store(&app->net_connected, true);
    fprintf(stderr, "[NET] server listening on :%u\n", (unsigned)port);
    return 0;
}

static void server_stop(net_server_t *srv) {
    if (!srv) return;
    atomic_store(&srv->stop_flag, true);
    atomic_store(&srv->running, false);
    atomic_store(&srv->beacon_stop, true);
    if (net_sock_valid(srv->listen_fd)) {
        /* shutdown() wakes any blocking select()/accept() on this socket in
         * addition to the non-blocking loop, then close(). */
#ifdef _WIN32
        shutdown(srv->listen_fd, 2 /* SD_BOTH */);
#else
        shutdown(srv->listen_fd, SHUT_RDWR);
#endif
        net_close(srv->listen_fd);
        srv->listen_fd = NET_INVALID_SOCKET;
    }
    thrd_join(srv->listen_thread, NULL);
    thrd_join(srv->beacon_thread, NULL);
    /* Wake any /rf + /baseband streaming subscribers so they exit. */
    net_fanout_destroy(&srv->rf);
    net_fanout_destroy(&srv->audio);
    if (srv->app) {
        atomic_store(&srv->app->net_connected, false);
        srv->app->net_state = NULL;
    }
    net_mutex_destroy(&srv->str_mtx);
    free(srv);
    s_server = NULL;
}

/* -------------------------------------------------------------------------
 * Client: HTTP helper (one short request), then the worker + pump threads.
 * ------------------------------------------------------------------------- */

/* Connect TCP to host:port with a bounded timeout. Non-blocking connect +
 * select() so an unreachable host does not block the caller for the full TCP
 * timeout (tens of seconds). Returns a blocking socket with SO_RCVTIMEO /
 * SO_SNDTIMEO set so later recv/send calls also cannot hang forever. */
#define NET_CONNECT_TIMEOUT_MS 2000
#define NET_IO_TIMEOUT_MS      1000

static net_sock_t client_connect(const char *host, uint16_t port) {
    net_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!net_sock_valid(fd)) return NET_INVALID_SOCKET;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        /* Not a dotted-quad; resolve hostname. */
        struct hostent *he = gethostbyname(host);
        if (!he || he->h_addrtype != AF_INET) {
            net_close(fd);
            return NET_INVALID_SOCKET;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }
    net_set_nonblocking(fd);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
#ifdef _WIN32
        int err = (int)WSAGetLastError();
        /* WSAEWOULDBLOCK == 10035; any other error is fatal here. */
        if (err != 10035) { net_close(fd); return NET_INVALID_SOCKET; }
#else
        if (errno != EINPROGRESS) { net_close(fd); return NET_INVALID_SOCKET; }
#endif
        /* Wait for the socket to become writable (connect completes) up to
         * NET_CONNECT_TIMEOUT_MS, so we don't block the UI thread on stop. */
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv;
        tv.tv_sec = NET_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (NET_CONNECT_TIMEOUT_MS % 1000) * 1000L;
        int sr = select((int)fd + 1, NULL, &wset, NULL, &tv);
        if (sr <= 0) { net_close(fd); return NET_INVALID_SOCKET; }
        /* Verify the connect actually succeeded (no SO_ERROR). */
        int soerr = 0;
        socklen_t solen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &solen) != 0 || soerr != 0) {
            net_close(fd);
            return NET_INVALID_SOCKET;
        }
    }
    net_set_blocking(fd);
    net_set_timeouts(fd, NET_IO_TIMEOUT_MS, NET_IO_TIMEOUT_MS);
    return fd;
}

/* Send "GET <path> HTTP/1.0\r\n\r\n", read headers + full body into body_out
 * (up to body_cap). HTTP/1.0 closes the connection at EOF, so keep reading
 * until recv returns 0 (EOF) or the body buffer is full. Returns 0 on
 * success, -1 on error. body_len receives the number of body bytes stored
 * (excluding the NUL terminator). */
static int client_get(const char *host, uint16_t port, const char *path,
                      char *body_out, size_t body_cap, size_t *body_len) {
    net_sock_t fd = client_connect(host, port);
    if (!net_sock_valid(fd)) return -1;
    char req[GUI_NET_STR_MAX*4];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    if (net_send_str(fd, req) != 0) { net_close(fd); return -1; }
    /* Read into a header buffer until we find the end-of-headers marker. */
    char hbuf[2048];
    size_t total = 0;
    int got_headers = 0;
    size_t body_off = 0;
    while (total < sizeof(hbuf) - 1) {
#ifdef _WIN32
        int n = recv(fd, hbuf + total, (int)(sizeof(hbuf) - 1 - total), 0);
        if (n == SOCKET_ERROR || n == 0) break;
        total += (size_t)n;
#else
        ssize_t n = recv(fd, hbuf + total, sizeof(hbuf) - 1 - total, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        total += (size_t)n;
#endif
        hbuf[total] = '\0';
        char *eoh = strstr(hbuf, "\r\n\r\n");
        if (eoh) {
            got_headers = 1;
            body_off = (size_t)(eoh + 4 - hbuf);
            break;
        }
    }
    int rc = -1;
    if (got_headers &&
        (strncmp(hbuf, "HTTP/1.0 200", 12) == 0 || strncmp(hbuf, "HTTP/1.1 200", 12) == 0)) {
        /* Copy any body bytes that arrived with the headers, then keep
         * reading until EOF (HTTP/1.0 closes the socket) so the full body
         * is captured - a small JSON body often arrives in a later TCP
         * packet than the headers, and the old code returned an empty
         * body in that case. */
        size_t have = total - body_off;
        if (body_out && body_cap) {
            size_t copy = (have < body_cap) ? have : body_cap - 1;
            memcpy(body_out, hbuf + body_off, copy);
            size_t body_total = copy;
            while (body_total < body_cap - 1) {
#ifdef _WIN32
                int n = recv(fd, body_out + body_total, (int)(body_cap - 1 - body_total), 0);
                if (n == SOCKET_ERROR) break;
                if (n == 0) break;  /* EOF: full body received */
                body_total += (size_t)n;
#else
                ssize_t n = recv(fd, body_out + body_total, body_cap - 1 - body_total, 0);
                if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
                body_total += (size_t)n;
#endif
            }
            body_out[body_total] = '\0';
            if (body_len) *body_len = body_total;
        }
        rc = 0;
    }else if(got_headers){
        rc=-2; /* HTTP rejection, different from network transport failure */
    }
    net_close(fd);
    return rc;
}

/* Tiny JSON field extractors for the mirror. */
static uint64_t json_u64(const char *j, const char *key, uint64_t def) {
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return def;
    return (uint64_t)strtoull(p, NULL, 10);
}

static int json_int(const char *j, const char *key, int def) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return def;
    return atoi(p + strlen(pat));
}

static void json_str(const char *j, const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static bool json_bool(const char *j, const char *key, bool def) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    return (strncmp(p, "true", 4) == 0);
}

/* Ingest pump thread: connect /rf (or /baseband), read body bytes, re-frame to
 * frame_bytes alignment, write into the buffer manager. Reconnects on any
 * transient stream drop (recv=0/error) with backoff so the feed stays alive
 * across server capture start/stop and brief network blips; only exits when
 * pump_stop is set (ingest torn down on real disconnect / mode change). */
static int client_pump_thread(void *arg) {
    net_pump_ctx_t *pc = (net_pump_ctx_t *)arg;
    net_client_t *cli = pc->cli;
    gui_app_t *app = cli->app;
    const char *path = pc->is_audio ? "/baseband" : "/rf";
    const int frame = pc->frame_bytes > 0 ? pc->frame_bytes : 4;
    uint8_t inbuf[128 * 1024];
    int reconnect_ms = 500;

    while (!atomic_load(&cli->pump_stop)) {
        net_sock_t fd = client_connect(cli->host, cli->port);
        if (!net_sock_valid(fd)) {
            /* Connect failed: backoff and retry (keep trying while the
             * server is unreachable but we're still in client mode). */
            thrd_sleep_ms(reconnect_ms);
            if (reconnect_ms < 3000) reconnect_ms += 500;
            continue;
        }
        reconnect_ms = 500;
        char req[256];
        snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, cli->host);
        if (net_send_str(fd, req) != 0) {
            net_close(fd);
            thrd_sleep_ms(reconnect_ms);
            continue;
        }
        /* Read headers. */
        char hbuf[1024];
        size_t total = 0;
        int got_headers = 0;
        size_t body_off = 0;
        while (total < sizeof(hbuf) - 1 && !atomic_load(&cli->pump_stop)) {
#ifdef _WIN32
            int n = recv(fd, hbuf + total, (int)(sizeof(hbuf) - 1 - total), 0);
            if (n == 0) break;
            if (n == SOCKET_ERROR) {
                int err = (int)WSAGetLastError();
                if (err == 10060 /* WSAETIMEDOUT */) { continue; }  /* recv timeout: re-check stop */
                break;
            }
            total += (size_t)n;
#else
            ssize_t n = recv(fd, hbuf + total, sizeof(hbuf) - 1 - total, 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            total += (size_t)n;
#endif
            hbuf[total] = '\0';
            char *eoh = strstr(hbuf, "\r\n\r\n");
            if (eoh) { got_headers = 1; body_off = (size_t)(eoh + 4 - hbuf); break; }
        }
        if (!got_headers) {
            net_close(fd);
            thrd_sleep_ms(reconnect_ms);
            continue;  /* no headers / stream closed: reconnect */
        }
        /* Carry over any body bytes already in hbuf. */
        size_t in_have = total - body_off;
        if (in_have > sizeof(inbuf)) in_have = sizeof(inbuf);
        memcpy(inbuf, hbuf + body_off, in_have);

        // A reconnect loses RF continuity even if the mirrored capture state
        // never became idle. Bind the new stream's first write to a new epoch.
        if (!pc->is_audio) atomic_fetch_add(&app->rf_stream_epoch, 1);
        fprintf(stderr, "[NET] client pump %s streaming (frame=%d)\n", path, frame);
        /* Body loop: read until the stream closes, then reconnect. */
        while (!atomic_load(&cli->pump_stop)) {
            if (in_have >= sizeof(inbuf)) {
                size_t aligned = (in_have / frame) * frame;
                if (aligned == 0) { in_have = 0; continue; }
                uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, pc->buf_id, aligned, NULL);
                if (out) { memcpy(out, inbuf, aligned); bufmgr_write_end(&app->buffers, pc->buf_id, aligned); bufmgr_signal_data(&app->buffers, pc->buf_id); }
                size_t leftover = in_have - aligned;
                if (leftover) memmove(inbuf, inbuf + aligned, leftover);
                in_have = leftover;
                continue;
            }
#ifdef _WIN32
            int n = recv(fd, (char *)(inbuf + in_have), (int)(sizeof(inbuf) - in_have), 0);
            if (n == 0) break;  /* stream closed: reconnect */
            if (n == SOCKET_ERROR) {
                int err = (int)WSAGetLastError();
                if (err == 10060 /* WSAETIMEDOUT */) { continue; }  /* timeout: keep waiting */
                break;  /* real error: reconnect */
            }
            in_have += (size_t)n;
#else
            ssize_t n = recv(fd, inbuf + in_have, sizeof(inbuf) - in_have, 0);
            if (n == 0) break;  /* stream closed: reconnect */
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;  /* real error: reconnect */
            }
            in_have += (size_t)n;
#endif
            size_t aligned = (in_have / frame) * frame;
            if (aligned > 0) {
                uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, pc->buf_id, aligned, NULL);
                if (out) { memcpy(out, inbuf, aligned); bufmgr_write_end(&app->buffers, pc->buf_id, aligned); bufmgr_signal_data(&app->buffers, pc->buf_id); }
                size_t leftover = in_have - aligned;
                if (leftover) memmove(inbuf, inbuf + aligned, leftover);
                in_have = leftover;
                atomic_store(&app->last_callback_time_ms, get_time_ms());
                atomic_store(&app->stream_synced, true);
            }
        }
        /* Stream ended (recv 0 / error) or pump_stop: close and either reconnect
         * or exit. */
        net_close(fd);
        if (atomic_load(&cli->pump_stop)) break;
        fprintf(stderr, "[NET] client pump %s stream dropped, reconnecting...\n", path);
        thrd_sleep_ms(reconnect_ms);
    }
    free(pc);
    fprintf(stderr, "[NET] client pump %s exiting\n", path);
    return 0;
}

/* Main-thread: start ingest backend (extraction + display + pump threads). */
static void client_start_ingest(gui_app_t *app, net_client_t *cli) {
    if (atomic_load(&cli->ingest_active)) return;
    if (app->is_capturing) return; /* already capturing locally somehow */
    fprintf(stderr, "[NET] client: starting ingest backend\n");

    /* Reset the capture/audio buffers' head/tail (not just stats) so a stale
     * fill level from a previous capture session doesn't pin RF Buffer at
     * ~99% on the client readout. bufmgr_reset_stats only clears stats;
     * bufmgr_reset also rewinds head/tail to 0. BUT bufmgr_reset is a no-op
     * if the buffer isn't initialized yet (lazy init), so ensure_init first. */
    (void)bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF);
    (void)bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO);
    (void)bufmgr_ensure_init(&app->buffers, BUF_DISPLAY);
    gui_extract_prepare_stream(app);
    bufmgr_reset(&app->buffers, BUF_CAPTURE_AUDIO);
    bufmgr_reset(&app->buffers, BUF_DISPLAY);
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    uint32_t peer_sr = (uint32_t)atomic_load(&cli->peer_sample_rate);
    if (peer_sr > 0) atomic_store(&app->sample_rate, peer_sr);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    /* Treat the network feed as a MISRC-style dual-channel raw stream. */
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = true;
    app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
    app->is_capturing = true;
    app->capture_start_time = gui_runtime_time();
    app->reconnect_pending = false;
    app->reconnect_attempts = 0;

    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[NET] client: failed to start extraction\n");
        app->is_capturing = false;
        return;
    }
    if (app->display_thread) {
        (void)gui_display_thread_start(app->display_thread, app, &app->buffers);
    }
    (void)gui_audio_start(app, &app->buffers);

    atomic_store(&cli->pump_stop, false);
    net_pump_ctx_t *rf = (net_pump_ctx_t *)calloc(1, sizeof(*rf));
    if (rf) {
        rf->cli = cli; rf->is_audio = false; rf->buf_id = BUF_CAPTURE_RF; rf->frame_bytes = 4;
        if (thrd_create(&cli->rf_pump_thread, client_pump_thread, rf) != thrd_success) {
            free(rf);
        }
    }
    int af = (int)atomic_load(&cli->peer_audio_frame_bytes);
    if (af <= 0) af = 12;
    net_pump_ctx_t *au = (net_pump_ctx_t *)calloc(1, sizeof(*au));
    if (au) {
        au->cli = cli; au->is_audio = true; au->buf_id = BUF_CAPTURE_AUDIO; au->frame_bytes = af;
        if (thrd_create(&cli->audio_pump_thread, client_pump_thread, au) != thrd_success) {
            free(au);
        }
    }
    atomic_store(&cli->ingest_active, true);
    gui_net_set_status(app, "Client ingest running (mirroring server capture)");
}

/* Main-thread: stop ingest backend. */
static void client_stop_ingest(gui_app_t *app, net_client_t *cli) {
    if (!atomic_load(&cli->ingest_active)) return;
    fprintf(stderr, "[NET] client: stopping ingest backend\n");
    atomic_store(&cli->pump_stop, true);
    /* The pump threads will exit when recv returns 0/error or pump_stop is seen.
     * They own their sockets; closing happens inside the thread. Join them. */
    thrd_join(cli->rf_pump_thread, NULL);
    thrd_join(cli->audio_pump_thread, NULL);

    app->is_capturing = false;
    if (app->display_thread) gui_display_thread_stop(app->display_thread);
    gui_audio_stop(app);
    gui_extract_stop();
    atomic_store(&app->stream_synced, false);
    gui_app_clear_display(app);
    atomic_store(&cli->ingest_active, false);
    gui_net_set_status(app, "Client ingest stopped");
}

/* Worker thread: connect, poll /stats + /devices + /controls, mirror, and
 * forward queued command flags to the server. Does NOT touch is_capturing or
 * buffers directly (the main thread starts/stops ingest on state changes). */
/* Client-side durable file receiver. v3 batches STREAM writes into SYNC ACKs.
 * Session is opened before Record ON.
 * Client path is never sent to the server; the latter only supplies names. */
static atomic_bool s_mls_client_stop=false;
static atomic_bool s_mls_client_ready=false;
static atomic_bool s_mls_client_running=false;
static thrd_t s_mls_client_thread;
static bool s_mls_client_thread_joinable=false;
static net_sock_t s_mls_client_fd=NET_INVALID_SOCKET;
static char s_mls_client_directory[MLS_PATH_MAX];
static char s_mls_client_host[256];
static uint16_t s_mls_client_port=0;
static int mls_receiver_loop(void *unused) {
    (void)unused;
    mls_receiver_t receiver;
    if(mls_receiver_init(&receiver,s_mls_client_directory,false)!=0)goto done;
    net_sock_t fd=client_connect(s_mls_client_host,s_mls_client_port);
    if(!net_sock_valid(fd))goto done;
    s_mls_client_fd=fd;
    mls_enable_tcp_nodelay(fd);
    net_set_timeouts(fd,5000,5000);
    if(net_send_str(fd,"GET /local-save/attach HTTP/1.0\r\n\r\n")!=0)goto close_fd;
    char headers[1024];
    if(net_read_headers(fd,headers,sizeof(headers))<0 || strncmp(headers,"HTTP/1.0 200",12)!=0)goto close_fd;
    /* The receiver can be idle between record start and the next FLAC block.
     * Do not treat five seconds without incoming bytes as a lost connection.
     * mls_client_stop_receiver() explicitly shuts this socket down on exit. */
    net_set_timeouts(fd,0,15000);
    atomic_store(&s_mls_client_ready,true);
    while(!atomic_load(&s_mls_client_stop)) {
        uint8_t h[MLS_HEADER_SIZE];
        mls_frame_t f={0},a={0};
        if(mls_recv_exact(fd,h,sizeof(h))!=0||mls_decode_header(&f,h,sizeof(h))!=0)break;
        uint8_t *payload=NULL;
        if(f.length){payload=malloc(f.length);if(!payload)break;
            if(mls_recv_exact(fd,payload,f.length)!=0){free(payload);break;}}
        int ok=mls_receiver_apply(&receiver,&f,payload);
        free(payload);
        if(ok==0 && f.type==MLS_COMMIT) {
            /* The socket can serve another recording with fresh file IDs. */
            (void)mls_receiver_init(&receiver,s_mls_client_directory,false);
        }
        if(f.type==MLS_WRITE_STREAM && ok==0) continue;
        a.type=MLS_ACK;a.file_id=f.file_id;a.offset=ok==0?0:1;
        if(mls_encode_header(h,&a)!=0||net_send_all(fd,h,sizeof(h))!=0||ok!=0)break;
        if(f.type==MLS_ABORT) break;
    }
    mls_receiver_abort(&receiver);
close_fd:
    atomic_store(&s_mls_client_ready,false);
    net_close(fd);
    s_mls_client_fd=NET_INVALID_SOCKET;
done:
    atomic_store(&s_mls_client_ready,false);
    atomic_store(&s_mls_client_running,false);
    return 0;
}
static int mls_client_start_receiver(net_client_t *cli) {
    if(!cli || !cli->app || !cli->app->settings.local_save_path[0])return -1;
    if(atomic_load(&s_mls_client_running))return atomic_load(&s_mls_client_ready)?0:-1;
    /* Reclaim an exited worker before starting a new one (Windows HANDLE). */
    if(s_mls_client_thread_joinable){
        thrd_join(s_mls_client_thread,NULL);
        s_mls_client_thread_joinable=false;
    }
    snprintf(s_mls_client_directory,sizeof(s_mls_client_directory),"%s",cli->app->settings.local_save_path);
    snprintf(s_mls_client_host,sizeof(s_mls_client_host),"%s",cli->host);
    s_mls_client_port=cli->port;
    atomic_store(&s_mls_client_stop,false);
    atomic_store(&s_mls_client_running,true);
    if(thrd_create(&s_mls_client_thread,mls_receiver_loop,NULL)!=thrd_success){
        atomic_store(&s_mls_client_running,false);return -1;
    }
    s_mls_client_thread_joinable=true;
    /* The server returns 200 only once the receiver owns the session. */
    for(int n=0;n<50;n++){
        if(atomic_load(&s_mls_client_ready))return 0;
        if(!atomic_load(&s_mls_client_running))break;
        thrd_sleep_ms(40);
    }
    atomic_store(&s_mls_client_stop,true);
    /* Socket recv is bounded by 5 s. Thread is joined on the client worker. */
    thrd_join(s_mls_client_thread,NULL);
    s_mls_client_thread_joinable=false;
    return -1;
}
static void mls_client_stop_receiver(void) {
    if(!s_mls_client_thread_joinable)return;
    atomic_store(&s_mls_client_stop,true);
    if(net_sock_valid(s_mls_client_fd)){
#ifdef _WIN32
        shutdown(s_mls_client_fd,SD_BOTH);
#else
        shutdown(s_mls_client_fd,SHUT_RDWR);
#endif
    }
    thrd_join(s_mls_client_thread,NULL);
    s_mls_client_thread_joinable=false;
    atomic_store(&s_mls_client_ready,false);
}

static int client_worker_thread(void *arg) {
    net_client_t *cli = (net_client_t *)arg;
    gui_app_t *app = cli->app;
    int backoff_ms = 500;
    int last_peer_state = -1;
    int miss_streak = 0;  /* consecutive /stats failures; tear down ingest only after several */
    bool first_contact = false;  /* fetch /devices immediately on first good /stats */
    while (!atomic_load(&cli->stop_flag)) {
        char stats[2048];
        size_t blen = 0;
        if (client_get(cli->host, cli->port, "/stats", stats, sizeof(stats), &blen) != 0) {
            /* Transient failure: don't immediately tear down ingest. A single
             * missed /stats (e.g. server briefly busy, network blip) used to
             * flap the whole ingest start/stop cycle. Only mark disconnected
             * and stop ingest after several consecutive misses. */
            miss_streak++;
            if (miss_streak >= 5) {
                atomic_store(&cli->connected, false);
                atomic_store(&cli->error, true);
                atomic_store(&cli->peer_state, 0);
                last_peer_state = -1;
                atomic_store(&cli->ingest_want, false);
                first_contact = false;  /* re-fetch /devices on reconnect */
                gui_rm_client_clear();
                gui_rm_scope_client_clear();
                gui_rd_client_clear();
            }
            thrd_sleep_ms(backoff_ms);
            if (backoff_ms < 2000) backoff_ms += 250;
            continue;
        }
        miss_streak = 0;
        atomic_store(&cli->connected, true);
        atomic_store(&cli->error, false);
        backoff_ms = 500;

        /* On the first successful /stats after (re)connect, fetch /devices
         * immediately so the toolbar device dropdown populates right away
         * instead of waiting up to ~3s for the periodic tick. */
        if (!first_contact) {
            first_contact = true;
            char *dj0 = (char *)malloc(16384);
            int drc = dj0 ? client_get(cli->host, cli->port, "/devices", dj0, 16384, NULL) : -1;
            if (drc == 0) {
                int count = json_int(dj0, "count", 0);
                int selected = json_int(dj0, "selected", -1);
                if (count < 0) count = 0;
                if (count > MAX_DEVICES) count = MAX_DEVICES;
                net_mutex_lock(&cli->dev_mtx);
                cli->staged_device_count = 0;
                const char *p = dj0;
                for (int i = 0; i < count; i++) {
                    const char *nm = strstr(p, "\"name\":\"");
                    if (!nm) break;
                    const char *idx = strstr(p, "\"index\":");
                    int ti = idx ? atoi(idx + 8) : i;
                    char name[80];
                    json_str(nm, "name", name, sizeof(name));
                    device_info_t *d = &cli->staged_devices[cli->staged_device_count];
                    snprintf(d->name, sizeof(d->name), "%s", name);
                    d->serial[0] = '\0';
                    d->type = DEVICE_TYPE_SIMPLE_CAPTURE;  /* generic remote tag */
                    d->index = ti;
                    cli->staged_device_count++;
                    p = nm + 8;
                }
                cli->staged_selected = selected;
                cli->staged_dirty = true;
                net_mutex_unlock(&cli->dev_mtx);
            }
            free(dj0);
        }

        int st = json_int(stats, "state", 0);
        int sr = json_int(stats, "sample_rate", 0);
        int sel = json_int(stats, "selected_device", -1);
        int dc = json_int(stats, "device_count", 0);
        int af = json_int(stats, "audio_frame_bytes", 12);
        atomic_store(&cli->peer_state, st);
        atomic_store(&cli->peer_sample_rate, sr);
        atomic_store(&cli->peer_selected, sel);
        atomic_store(&cli->peer_device_count, dc);
        atomic_store(&cli->peer_audio_frame_bytes, af);
        atomic_store(&cli->peer_total_samples, json_u64(stats, "total_samples", 0));
        atomic_store(&cli->peer_samples_a, json_u64(stats, "samples_a", 0));
        atomic_store(&cli->peer_samples_b, json_u64(stats, "samples_b", 0));
        atomic_store(&cli->peer_frames, (uint32_t)json_u64(stats, "frames", 0));
        atomic_store(&cli->peer_missed, (uint32_t)json_u64(stats, "missed", 0));
        atomic_store(&cli->peer_errors, (uint32_t)json_u64(stats, "errors", 0));
        atomic_store(&cli->peer_synced, json_bool(stats, "synced", false));
        atomic_store(&cli->peer_raw_a, json_u64(stats, "raw_a", 0));
        atomic_store(&cli->peer_raw_b, json_u64(stats, "raw_b", 0));
        atomic_store(&cli->peer_flac_a, json_u64(stats, "flac_a", 0));
        atomic_store(&cli->peer_flac_b, json_u64(stats, "flac_b", 0));
        atomic_store(&cli->peer_clip_a_pos, (uint32_t)json_u64(stats, "clip_a_pos", 0));
        atomic_store(&cli->peer_clip_a_neg, (uint32_t)json_u64(stats, "clip_a_neg", 0));
        atomic_store(&cli->peer_clip_b_pos, (uint32_t)json_u64(stats, "clip_b_pos", 0));
        atomic_store(&cli->peer_clip_b_neg, (uint32_t)json_u64(stats, "clip_b_neg", 0));
        atomic_store(&cli->peer_record_elapsed_ms, json_u64(stats, "record_elapsed_ms", 0));
        atomic_store(&cli->peer_last_record_ms, json_u64(stats, "last_record_ms", 0));
        atomic_store(&cli->peer_rf_pct, json_int(stats, "rf_pct", 0));
        atomic_store(&cli->peer_audio_pct, json_int(stats, "audio_pct", 0));
        atomic_store(&cli->peer_free_known, json_bool(stats, "free_known", false));
        atomic_store(&cli->peer_free_bytes, json_u64(stats, "free_bytes", 0));
        /* GET a single reduced preview frame, not the /rf or /baseband streams.
         * Unknown older servers will simply not expose monitor data. */
        if (st > 0) {
            gui_rd_config_t ca=gui_rd_get_request(0),cb=gui_rd_get_request(1);
            char path[360];
            snprintf(path,sizeof(path),
                "/preview/display?za=%d&zb=%d&wa=%u&wb=%u&ta=%d&tb=%d&la=%d&lb=%d&ma=%d&mb=%d&sa=%d&sb=%d",
                (int)(ca.zoom*1000.f),(int)(cb.zoom*1000.f),ca.width,cb.width,
                ca.trigger_enabled,cb.trigger_enabled,ca.trigger_level,cb.trigger_level,
                ca.trigger_mode,cb.trigger_mode,ca.trigger_source,cb.trigger_source);
            size_t capacity=gui_rd_wire_capacity()+1;
            void *wire=malloc(capacity);
            size_t nwire=0;
            if(wire && client_get(cli->host,cli->port,path,(char*)wire,capacity,&nwire)==0 &&
               gui_rd_client_accept(wire,nwire)){
                /* Local GUI renders exact server-computed waveform and FFT points. */
            }else gui_rd_client_clear();
            free(wire);
        } else {
            gui_rm_client_clear();
            gui_rm_scope_client_clear();
            gui_rd_client_clear();
        }

        /* Remote Monitor v1 explicitly does not spin up the raw RF/audio pumps.
         * The capture/record operation remains entirely on the server. */
        atomic_store(&cli->ingest_want, false);

        if (st != last_peer_state) {
            fprintf(stderr, "[NET] client: peer state -> %d (sr=%d)\n", st, sr);
            last_peer_state = st;
        }

        /* Client worker sends queued UI commands to the remote server.
         * Server-owned pending_string[] belongs exclusively to
         * gui_net_poll_commands() in SERVER mode.  Never access s_server
         * here: it is NULL by design in client mode. */
        if (atomic_exchange(&app->net_cmd_start, false)) {
            (void)client_get(cli->host, cli->port, "/start", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_stop, false)) {
            (void)client_get(cli->host, cli->port, "/stop", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_record_on, false)) {
            if(cli->app->settings.local_save_enabled) {
                if(mls_client_start_receiver(cli)==0)
                    (void)client_get(cli->host,cli->port,"/record?on=1&local=1",NULL,0,NULL);
                else gui_app_set_status(cli->app,"Local Save: cannot connect to local file receiver");
            } else (void)client_get(cli->host, cli->port, "/record?on=1", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_record_off, false)) {
            (void)client_get(cli->host, cli->port, "/record?on=0", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_select_device, false)) {
            int n = atomic_exchange(&app->net_cmd_device_index, 0);
            char path[64];
            snprintf(path, sizeof(path), "/device?%d", n);
            (void)client_get(cli->host, cli->port, path, NULL, 0, NULL);
        }

        /* Explicit UI edits; never forward the mirrored state back as edits.
         * Coalesce rapid adjustments by key and retry failed HTTP sends. */
        for (int id=0;id<GUI_NET_CTRL_COUNT;id++) {
            int value=atomic_exchange(&cli->requested_controls[id],INT_MIN);
            if (value==INT_MIN) continue;
            char path[128];
            snprintf(path,sizeof(path),"/set?%s=%d",s_ctrl_keys[id],value);
            if (client_get(cli->host,cli->port,path,NULL,0,NULL)!=0) {
                int expected=INT_MIN;
                atomic_compare_exchange_strong(&cli->requested_controls[id],&expected,value);
                fprintf(stderr,"[NET] client: control %s send failed; retry pending\n",s_ctrl_keys[id]);
            }
        }

        /* Forward user-committed path/name/tag edits. Only this worker does
         * network I/O, never the GUI thread. Preserve newest value on retry. */
        for(int id=0;id<GUI_NET_STR_COUNT;id++){
            char value[GUI_NET_STR_MAX]={0};bool send_now=false;
            net_mutex_lock(&cli->str_mtx);
            if(cli->requested_string[id]){
                snprintf(value,sizeof(value),"%s",cli->requested_strings[id]);
                cli->requested_string[id]=false;send_now=true;
            }
            net_mutex_unlock(&cli->str_mtx);
            if(!send_now)continue;
            char encoded[GUI_NET_STR_MAX*3];
            char path[GUI_NET_STR_MAX*3+96];
            bool enc_ok=net_uri_encode(value,encoded,sizeof(encoded));
            int rc=-2;
            if(enc_ok){
                snprintf(path,sizeof(path),"/settings/set?key=%s&value=%s",s_str_keys[id],encoded);
                rc=client_get(cli->host,cli->port,path,NULL,0,NULL);
            }
            net_mutex_lock(&cli->str_mtx);
            if(rc==0){
                cli->waiting_string[id]=true;
                cli->waiting_polls[id]=0;
            }else if(rc==-1 && !cli->requested_string[id]){
                snprintf(cli->requested_strings[id],GUI_NET_STR_MAX,"%s",value);
                cli->requested_string[id]=true;
            }else if(rc==-2){
                cli->waiting_string[id]=false;
                cli->mirrored_string[id]=false;
                fprintf(stderr,"[NET] client: server rejected %s (check directory/recording lock)\n",s_str_keys[id]);
            }
            net_mutex_unlock(&cli->str_mtx);
        }

        if(atomic_exchange(&cli->browse_requested,false)){
            char relative[GUI_NET_STR_MAX];unsigned page;
            net_mutex_lock(&cli->str_mtx);
            snprintf(relative,sizeof(relative),"%s",cli->browse_relative);
            page=cli->browse_page;
            net_mutex_unlock(&cli->str_mtx);
            char encoded[GUI_NET_STR_MAX*3],path[GUI_NET_STR_MAX*3+80];
            if(net_uri_encode(relative,encoded,sizeof(encoded))){
                snprintf(path,sizeof(path),"/fs/browse?at=%s&page=%u",encoded,page);
                char json[32768]={0};
                int ok=client_get(cli->host,cli->port,path,json,sizeof(json),NULL);
                int count=ok==0?json_int(json,"count",0):0;
                if(count<0)count=0;if(count>GUI_NET_BROWSE_MAX)count=GUI_NET_BROWSE_MAX;
                net_mutex_lock(&cli->str_mtx);
                /* Do not put stale replies from earlier navigation into the
                 * latest folder view. */
                if(!strcmp(relative,cli->browse_relative)&&page==cli->browse_page){
                    cli->folder_count=0;
                    cli->browse_has_more=ok==0&&json_bool(json,"has_more",false);
                    cli->browse_error=ok!=0;
                    cli->browse_recovered=ok==0&&json_bool(json,"recovered",false);
                    cli->browse_computer=ok==0&&json_bool(json,"computer",false);
                    cli->browse_path[0]=cli->browse_parent[0]=0;
                    if(ok==0){
                        net_json_get_str(json,"path",cli->browse_path,sizeof(cli->browse_path));
                        net_json_get_str(json,"parent",cli->browse_parent,sizeof(cli->browse_parent));
                    }
                    if(ok==0)for(int i=0;i<count;i++){
                        char key[32];snprintf(key,sizeof(key),"dir%d",i);
                        char name[256];
                        if(net_json_get_str(json,key,name,sizeof(name))){
                            snprintf(cli->folders[cli->folder_count++],sizeof(cli->folders[0]),"%s",name);
                        }
                    }
                    cli->folders_ready=true;
                }
                net_mutex_unlock(&cli->str_mtx);
            }
        }

        /* Periodically refresh the device list + controls (every ~3s). */
        static int dev_tick = 0;
        if ((dev_tick++ % 30) == 0) {
            char *dj = (char *)malloc(16384);
            if (dj && client_get(cli->host, cli->port, "/devices", dj, 16384, &blen) == 0) {
                int count = json_int(dj, "count", 0);
                int selected = json_int(dj, "selected", -1);
                if (count < 0) count = 0;
                if (count > MAX_DEVICES) count = MAX_DEVICES;
                /* Parse device entries by finding "name":"..." occurrences. */
                net_mutex_lock(&cli->dev_mtx);
                cli->staged_device_count = 0;
                const char *p = dj;
                for (int i = 0; i < count; i++) {
                    const char *idx = strstr(p, "\"index\":");
                    const char *nm = strstr(p, "\"name\":\"");
                    if (!nm) break;
                    int ti = idx ? atoi(idx + 8) : i;
                    char name[80];
                    json_str(nm, "name", name, sizeof(name));
                    device_info_t *d = &cli->staged_devices[cli->staged_device_count];
                    snprintf(d->name, sizeof(d->name), "%s", name);
                    d->serial[0] = '\0';
                    /* We don't know the real device type; mark as a generic
                     * "remote" entry using SIMPLE_CAPTURE as a neutral tag so
                     * the dropdown renders. The client never opens it. */
                    d->type = DEVICE_TYPE_SIMPLE_CAPTURE;
                    d->index = ti;
                    cli->staged_device_count++;
                    p = nm + 8;
                }
                cli->staged_selected = selected;
                cli->staged_dirty = true;
                net_mutex_unlock(&cli->dev_mtx);
            }
            free(dj);

            char *sj=(char*)malloc(16384);
            if(sj && client_get(cli->host,cli->port,"/settings",sj,16384,&blen)==0){
                net_mutex_lock(&cli->str_mtx);
                for(int id=0;id<GUI_NET_STR_COUNT;id++){
                    if(!cli->requested_string[id]){
                        char field[GUI_NET_STR_MAX];
                        if(net_json_get_str(sj,s_str_keys[id],field,sizeof(field))){
                            if(cli->waiting_string[id]){
                                if(!strcmp(field,cli->requested_strings[id]))cli->waiting_string[id]=false;
                                else if(++cli->waiting_polls[id]<4)continue;
                                else cli->waiting_string[id]=false; /* rejected in server main */
                            }
                            snprintf(cli->mirrored_strings[id],GUI_NET_STR_MAX,"%s",field);
                            cli->mirrored_string[id]=true;
                        }
                    }
                }
                net_mutex_unlock(&cli->str_mtx);
            }
            free(sj);

            char status[GUI_NET_STR_MAX*2]={0};
            if(client_get(cli->host,cli->port,"/fs/status",status,sizeof(status),NULL)==0){
                atomic_store(&cli->peer_directory_exists,json_bool(status,"directory_exists",false));
                atomic_store(&cli->peer_free_known,json_bool(status,"free_known",false));
                const char *f=strstr(status,"\"free_bytes\":");
                if(f)atomic_store(&cli->peer_free_bytes,strtoull(f+13,NULL,10));
                atomic_store(&cli->peer_storage_seen,true);
            }
            char cj[2048];
            if (client_get(cli->host, cli->port, "/controls", cj, sizeof(cj), &blen) == 0) {
                /* Stage authoritative controls atomically; the render thread
                 * applies them to app->settings (no worker-to-GUI data race). */
                for (int id=0;id<GUI_NET_CTRL_COUNT;id++) {
                    char pat[64];snprintf(pat,sizeof(pat),"\"%s\":",s_ctrl_keys[id]);
                    if (!strstr(cj,pat)) continue; /* older server fallback */
                    int v;
                    switch(id) {
                        case 0:case 1:case 2:case 3:case 4:
                        case 6:case 8:case 11:case 12:case 15:case 16:
                        case 17:case 18:case 19:case 20:case 21:case 22:case 23:case 24:
                            v=json_bool(cj,s_ctrl_keys[id],false)?1:0;break;
                        default:v=json_int(cj,s_ctrl_keys[id],INT_MIN);break;
                    }
                    if (v!=INT_MIN && gui_net_ctrl_valid(id,v))
                        atomic_store(&cli->mirror_controls[id],v);
                }
            }
        }

    /* <=10 reduced preview frames per second; device/control polls every 3s. */
    thrd_sleep_ms(100);
    }
    atomic_store(&cli->ingest_want, false);
    return 0;
}

/* UDP discovery listener: bind NET_DISCOVERY_PORT (SO_REUSEADDR so multiple
 * clients on one host coexist), collect server beacons into the discovered
 * list, and prune entries older than NET_DISCOVERY_TTL_MS. Sets disc_dirty so
 * the UI knows to re-read the list. */
static int client_discovery_thread(void *arg) {
    net_client_t *cli = (net_client_t *)arg;
    net_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!net_sock_valid(fd)) return 0;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    /* SO_REUSEPORT lets multiple clients on the same host all receive broadcast
     * datagrams on this port; without it the kernel delivers each datagram to
     * only one bound socket, hiding discovered servers from the others. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse, sizeof(reuse));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(NET_DISCOVERY_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[NET] discovery: bind(:%u) failed\n", NET_DISCOVERY_PORT);
        net_close(fd);
        return 0;
    }
    net_set_timeouts(fd, 1000, 1000);  /* 1s recv timeout */
    while (!atomic_load(&cli->discovery_stop)) {
        char buf[256];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&src, &slen);
        if (n > 0) {
            buf[n] = '\0';
            /* Beacon format: "MISRC\n<port>\n<name>" */
            if (strncmp(buf, "MISRC\n", 6) == 0) {
                char *p = buf + 6;
                long pport = strtol(p, &p, 10);
                if (p && *p == '\n') p++;
                char name[64] = {0};
                snprintf(name, sizeof(name), "%s", p ? p : "");
                /* Strip a trailing newline from the name. */
                char *nl = strchr(name, '\n');
                if (nl) *nl = '\0';
                if (pport >= 1 && pport <= 65535) {
                    char host[64];
#ifdef _WIN32
                    snprintf(host, sizeof(host), "%u.%u.%u.%u",
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b1),
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b2),
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b3),
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b4));
#else
                    inet_ntop(AF_INET, &src.sin_addr, host, sizeof(host));
#endif
                    uint64_t now = get_time_ms();
                    net_mutex_lock(&cli->disc_mtx);
                    /* Update existing or append. */
                    int slot = -1;
                    for (int i = 0; i < cli->discovered_count; i++) {
                        if (strcmp(cli->discovered[i].host, host) == 0 &&
                            cli->discovered[i].port == (uint16_t)pport) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot < 0 && cli->discovered_count < NET_MAX_DISCOVERED) {
                        slot = cli->discovered_count++;
                    }
                    if (slot >= 0) {
                        net_discovered_t *d = &cli->discovered[slot];
                        bool was_new = (d->last_seen_ms == 0);
                        snprintf(d->host, sizeof(d->host), "%s", host);
                        d->port = (uint16_t)pport;
                        snprintf(d->name, sizeof(d->name), "%s", name);
                        d->last_seen_ms = now;
                        if (was_new) {
                            fprintf(stderr, "[NET] discovery: found server %s:%u (%s)\n",
                                    d->host, (unsigned)d->port, d->name[0] ? d->name : "?");
                        }
                    }
                    atomic_store(&cli->disc_dirty, true);
                    net_mutex_unlock(&cli->disc_mtx);
                }
            }
        }
        /* Prune stale entries (not seen for > TTL). */
        uint64_t now = get_time_ms();
        net_mutex_lock(&cli->disc_mtx);
        bool changed = false;
        for (int i = 0; i < cli->discovered_count; ) {
            if (now - cli->discovered[i].last_seen_ms > NET_DISCOVERY_TTL_MS) {
                /* Remove by swapping with last. */
                cli->discovered[i] = cli->discovered[--cli->discovered_count];
                changed = true;
            } else {
                i++;
            }
        }
        if (changed) atomic_store(&cli->disc_dirty, true);
        net_mutex_unlock(&cli->disc_mtx);
    }
    net_close(fd);
    return 0;
}

/* Start just the stats/ingest worker thread (requires a known host). */
static int client_start_worker(net_client_t *cli) {
    if (atomic_load(&cli->worker_started)) return 0;
    if (!cli->host[0]) return -1;
    atomic_store(&cli->stop_flag, false);
    if (thrd_create(&cli->worker_thread, client_worker_thread, cli) != thrd_success) {
        return -1;
    }
    atomic_store(&cli->worker_started, true);
    fprintf(stderr, "[NET] client worker started -> %s:%u\n", cli->host, (unsigned)cli->port);
    return 0;
}

/* Stop just the stats/ingest worker (+ pump/ingest) thread. */
static void client_stop_worker(net_client_t *cli) {
    if (!atomic_load(&cli->worker_started)) return;
    atomic_store(&cli->stop_flag, true);
    atomic_store(&cli->ingest_want, false);
    thrd_join(cli->worker_thread, NULL);
    mls_client_stop_receiver();
    atomic_store(&cli->worker_started, false);
    if (atomic_load(&cli->ingest_active)) {
        client_stop_ingest(cli->app, cli);
    }
    atomic_store(&cli->connected, false);
    atomic_store(&cli->error, false);
    atomic_store(&cli->peer_state, 0);
    gui_rm_client_clear();
    gui_rd_client_clear();
    atomic_store(&cli->stop_flag, false);
}

static int client_start(gui_app_t *app, const char *host, uint16_t port) {
    if (s_client) return 0;
    net_client_t *cli = (net_client_t *)calloc(1, sizeof(*cli));
    if (!cli) return -1;
    cli->app = app;
    snprintf(cli->host, sizeof(cli->host), "%s", host);
    cli->port = port;
    atomic_store(&cli->running, false);
    atomic_store(&cli->stop_flag, false);
    atomic_store(&cli->connected, false);
    atomic_store(&cli->error, false);
    atomic_store(&cli->peer_state, 0);
    atomic_store(&cli->peer_sample_rate, 0);
    atomic_store(&cli->peer_device_count, 0);
    atomic_store(&cli->peer_selected, -1);
    atomic_store(&cli->peer_audio_frame_bytes, 12);
    atomic_store(&cli->ingest_want, false);
    atomic_store(&cli->ingest_active, false);
    atomic_store(&cli->pump_stop, false);
    atomic_store(&cli->worker_started, false);
    cli->staged_device_count = 0;
    cli->staged_dirty = false;
    cli->discovered_count = 0;
    atomic_store(&cli->disc_dirty, false);
    atomic_store(&cli->discovery_stop, false);
    for(int i=0;i<GUI_NET_CTRL_COUNT;i++){atomic_init(&cli->requested_controls[i],INT_MIN);atomic_init(&cli->mirror_controls[i],INT_MIN);}
    net_mutex_init(&cli->dev_mtx);
    net_mutex_init(&cli->disc_mtx);
    net_mutex_init(&cli->str_mtx);
    atomic_init(&cli->text_edit_active,false);
    atomic_init(&cli->browse_requested,false);
    atomic_init(&cli->peer_storage_seen,false);
    atomic_init(&cli->peer_directory_exists,false);
    atomic_init(&cli->peer_free_known,false);
    atomic_init(&cli->peer_free_bytes,0);

    /* Start the UDP discovery listener so the UI can show found servers, even
     * before a specific host is selected. */
    if (thrd_create(&cli->discovery_thread, client_discovery_thread, cli) != thrd_success) {
        fprintf(stderr, "[NET] client: discovery thread failed\n");
        net_mutex_destroy(&cli->dev_mtx);
        net_mutex_destroy(&cli->disc_mtx);
        net_mutex_destroy(&cli->str_mtx);
        free(cli);
        return -1;
    }
    /* Start the stats/ingest worker only if a host is already known. */
    if (host && host[0]) {
        (void)client_start_worker(cli);
    }
    atomic_store(&cli->running, true);
    s_client = cli;
    app->net_state = cli;
    fprintf(stderr, "[NET] client mode: discovery active%s\n",
            (host && host[0]) ? "" : " (no host selected yet)");
    return 0;
}

static void client_stop(net_client_t *cli) {
    if (!cli) return;
    atomic_store(&cli->discovery_stop, true);
    atomic_store(&cli->running, false);
    /* Stop the worker first (if running), then the discovery listener. */
    client_stop_worker(cli);
    thrd_join(cli->discovery_thread, NULL);
    if (cli->app) {
        atomic_store(&cli->app->net_connected, false);
        cli->app->net_state = NULL;
        /* This state was mirrored, not a locally running capture. */
        cli->app->is_capturing = false;
        cli->app->is_recording = false;
    }
    net_mutex_destroy(&cli->dev_mtx);
    net_mutex_destroy(&cli->disc_mtx);
    net_mutex_destroy(&cli->str_mtx);
    free(cli);
    s_client = NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void gui_net_stop(gui_app_t *app) {
    if (s_server) server_stop(s_server);
    if (s_client) client_stop(s_client);
    if (app) {
        atomic_store(&app->net_connected, false);
        app->net_state = NULL;
    }
}

int gui_net_apply_mode(gui_app_t *app) {
    if (!app) return -1;
    gui_net_init_globals();
    int want = app->settings.net_mode;
    /* Stop whatever is running if it doesn't match the desired mode. */
    if (s_server && want != GUI_NET_MODE_SERVER) {
        server_stop(s_server);
    }
    if (s_client && want != GUI_NET_MODE_CLIENT) {
        /* Do not carry the remote server's disk paths/record toggles into
         * standalone Local mode or a newly selected Server mode. */
        gui_settings_restore_local_record_profile(&app->settings);
        client_stop(s_client);
    }
    if (want == GUI_NET_MODE_SERVER) {
        long p = atol(app->settings.net_server_port_str);
        if (p < 1 || p > 65535) { p = (long)app->settings.net_server_port; }
        if (p < 1 || p > 65535) p = 8080;
        app->settings.net_server_port = (uint16_t)p;
        if (s_server) {
            /* Already running: restart only if the listen port changed. */
            if (s_server->port != (uint16_t)p) {
                server_stop(s_server);
                if (server_start(app, (uint16_t)p) != 0) {
                    gui_net_set_status(app, "Failed to restart network server");
                    return -1;
                }
                char msg[96];
                snprintf(msg, sizeof(msg), "Network server listening on :%u", (unsigned)p);
                gui_net_set_status(app, msg);
            }
        } else {
            if (server_start(app, (uint16_t)p) != 0) {
                gui_net_set_status(app, "Failed to start network server");
                return -1;
            }
            char msg[96];
            snprintf(msg, sizeof(msg), "Network server listening on :%u", (unsigned)p);
            gui_net_set_status(app, msg);
        }
    } else if (want == GUI_NET_MODE_CLIENT) {
        long p = atol(app->settings.net_client_port_str);
        if (p < 1 || p > 65535) { p = (long)app->settings.net_client_port; }
        if (p < 1 || p > 65535) p = 8080;
        app->settings.net_client_port = (uint16_t)p;
        const char *h = app->settings.net_client_host;
        /* Ensure the client (discovery listener) is running in client mode even
         * with no host selected, so the discovered-server list populates. */
        if (!s_client) {
            if (client_start(app, h ? h : "", (uint16_t)p) != 0) {
                gui_net_set_status(app, "Failed to start network client");
                return -1;
            }
        }
        if (h && h[0]) {
            /* A host is selected: ensure the stats/ingest worker is running for
             * that host. Restart it if the target changed. */
            if (s_client && (strcmp(s_client->host, h) != 0 || s_client->port != (uint16_t)p)) {
                client_stop_worker(s_client);
                snprintf(s_client->host, sizeof(s_client->host), "%s", h);
                s_client->port = (uint16_t)p;
                if (client_start_worker(s_client) != 0) {
                    gui_net_set_status(app, "Failed to start client worker");
                    return -1;
                }
                char msg[128];
                snprintf(msg, sizeof(msg), "Network client connecting to %s:%u", h, (unsigned)p);
                gui_net_set_status(app, msg);
            } else if (s_client && !atomic_load(&s_client->worker_started)) {
                if (client_start_worker(s_client) != 0) {
                    gui_net_set_status(app, "Failed to start client worker");
                    return -1;
                }
                char msg[128];
                snprintf(msg, sizeof(msg), "Network client connecting to %s:%u", h, (unsigned)p);
                gui_net_set_status(app, msg);
            }
        } else {
            /* No host selected yet: stop the worker if any, keep discovery alive. */
            if (s_client) {
                client_stop_worker(s_client);
            }
            gui_net_set_status(app, "Client mode: scanning for servers on the LAN...");
        }
    } else {
        /* Local: ensure stopped (already handled above). */
        if (!s_server && !s_client) {
            atomic_store(&app->net_connected, false);
        }
        gui_net_set_status(app, "Local (no network)");
    }
    return 0;
}

void gui_net_tap_rf(gui_app_t *app, const void *data, size_t bytes) {
    (void)app;
    if (s_server && s_server->rf.initialized) {
        net_fanout_push(&s_server->rf, data, bytes);
    }
}

void gui_net_tap_audio(gui_app_t *app, const void *data, size_t bytes) {
    (void)app;
    if (s_server && s_server->audio.initialized) {
        net_fanout_push(&s_server->audio, data, bytes);
    }
}

void gui_net_poll_commands(gui_app_t *app) {
    if (!app) return;
    /* Server mode: execute queued commands locally on the main thread. */
    if (s_server) {
        for (int id=0;id<GUI_NET_CTRL_COUNT;id++) {
            int value=atomic_exchange(&s_server->requested_controls[id],INT_MIN);
            if (value==INT_MIN) continue;
            /* Recording configuration is immutable during an active write.
             * Capture-related controls are changed on the server only. */
            if (app->is_recording || !gui_net_ctrl_valid(id,value)) {
                fprintf(stderr,"[NET] server: rejected control %s=%d (recording/invalid)\n",s_ctrl_keys[id],value);
                continue;
            }
            /* A/B swap only applies to HSDAOH/simple capture hardware. */
            if (id==0 && app->selected_device>=0 && app->selected_device<app->device_count) {
                device_type_t type=app->devices[app->selected_device].type;
                if (type!=DEVICE_TYPE_HSDAOH && type!=DEVICE_TYPE_SIMPLE_CAPTURE) {
                    fprintf(stderr,"[NET] server: A/B swap not supported by this backend\n");
                    continue;
                }
            }
            gui_net_ctrl_apply(app,id,value);
            atomic_store(&s_server->current_controls[id],gui_net_ctrl_value(app,id));
            /* v7: persist authoritative server settings, including when
             * headless --config is active. Existing network keys survive;
             * the six-field JSON is expanded to the standard settings file. */
            gui_settings_save(&app->settings);
            fprintf(stderr,"[NET] server: applied control %s=%d\n",s_ctrl_keys[id],value);
        }
        for(int id=0;id<GUI_NET_STR_COUNT;id++){
            char value[GUI_NET_STR_MAX]={0};bool pending=false;
            net_mutex_lock(&s_server->str_mtx);
            if(s_server->pending_string[id]){
                snprintf(value,sizeof(value),"%s",s_server->pending_strings[id]);
                s_server->pending_string[id]=false;pending=true;
            }
            net_mutex_unlock(&s_server->str_mtx);
            if(!pending)continue;
            if(app->is_recording||!net_validate_string(app,id,value)){
                fprintf(stderr,"[NET] server: rejected remote %s (recording, invalid or directory missing)\n",s_str_keys[id]);
                continue;
            }
            size_t cap=0;char *dst=gui_net_str_field(&app->settings,id,&cap);
            if(dst)snprintf(dst,cap,"%s",value);
            net_mutex_lock(&s_server->str_mtx);
            snprintf(s_server->current_strings[id],GUI_NET_STR_MAX,"%s",value);
            net_mutex_unlock(&s_server->str_mtx);
            gui_settings_save(&app->settings);
            fprintf(stderr,"[NET] server: applied remote %s\n",s_str_keys[id]);
        }
        if (atomic_exchange(&app->net_cmd_start, false)) {
            if (app->headless_server) app->headless_retry_capture = true;
            if (!app->is_capturing) {
                fprintf(stderr, "[NET] server: executing /start\n");
                (void)gui_app_start_capture(app);
            }
        }
        if (atomic_exchange(&app->net_cmd_stop, false)) {
            if (app->headless_server) {
                app->headless_retry_capture = false;
                app->reconnect_pending = false;
            }
            if (app->is_capturing || gui_playback_is_starting(app)) {
                fprintf(stderr, "[NET] server: executing /stop\n");
                gui_app_stop_capture(app);
            }
        }
        if (atomic_exchange(&app->net_cmd_record_on, false)) {
            if (app->is_capturing && !app->is_recording) {
                fprintf(stderr, "[NET] server: executing /record on\n");
                bool remote=atomic_exchange(&s_mls_request,false);
                if (!remote || gui_net_local_save_begin()==0) {
                    if (gui_app_start_recording(app)!=0 && remote) gui_net_local_save_end(false);
                } else {
                    fprintf(stderr,"[Local Save] Recording rejected: no ready client receiver\n");
                    gui_app_set_status(app,"Local Save receiver not connected");
                }
            }
        }
        /* Checked on the SERVER: only the server owns s_mls_record/s_mls_failed.
         * A failed receiver must stop the capture writer rather than silently
         * continue to discard data. */
        if (gui_net_local_save_recording() && gui_net_local_save_failed() && app->is_recording) {
            fprintf(stderr,"[Local Save] ERROR: receiver disconnected or disk failure; aborting recording\n");
            gui_app_set_status(app,"Local Save failed: client files incomplete");
            atomic_store(&app->net_cmd_record_off,true);
        }
        if (atomic_exchange(&app->net_cmd_record_off, false)) {
            if (app->is_recording) {
                fprintf(stderr, "[NET] server: executing /record off\n");
                gui_app_stop_recording(app);
            }
        }
        if (atomic_exchange(&app->net_cmd_select_device, false)) {
            int n = atomic_exchange(&app->net_cmd_device_index, 0);
            if (!app->is_capturing && !gui_playback_is_starting(app) &&
                n >= 0 && n < app->device_count) {
                fprintf(stderr, "[NET] server: executing /device %d\n", n);
                app->selected_device = n;
            }
        }
        for(int id=0;id<GUI_NET_CTRL_COUNT;id++)
            atomic_store(&s_server->current_controls[id],gui_net_ctrl_value(app,id));
        net_mutex_lock(&s_server->str_mtx);
        for(int id=0;id<GUI_NET_STR_COUNT;id++){
            size_t cap=0;char *value=gui_net_str_field(&app->settings,id,&cap);
            if(value)snprintf(s_server->current_strings[id],GUI_NET_STR_MAX,"%s",value);
        }
        net_mutex_unlock(&s_server->str_mtx);
    }
    /* Client mode: the worker drains the command flags and forwards them; we
     * must NOT execute them locally. Nothing to do here for client. */
}

void gui_net_poll_mirror(gui_app_t *app) {
    if (!app) return;
    if (s_client) {
        if (atomic_load(&s_client->connected)) {
            for(int id=0;id<GUI_NET_CTRL_COUNT;id++) {
                int value=atomic_exchange(&s_client->mirror_controls[id],INT_MIN);
                if(value!=INT_MIN &&
                    atomic_load(&s_client->requested_controls[id])==INT_MIN &&
                    gui_net_ctrl_valid(id,value)) gui_net_ctrl_apply(app,id,value);
            }
        }
        if(atomic_load(&s_client->connected) && !atomic_load(&s_client->text_edit_active)){
            net_mutex_lock(&s_client->str_mtx);
            for(int id=0;id<GUI_NET_STR_COUNT;id++){
                if(s_client->mirrored_string[id] && !s_client->requested_string[id] && !s_client->waiting_string[id]){
                    size_t cap=0;char *dst=gui_net_str_field(&app->settings,id,&cap);
                    if(dst)snprintf(dst,cap,"%s",s_client->mirrored_strings[id]);
                    s_client->mirrored_string[id]=false;
                }
            }
            net_mutex_unlock(&s_client->str_mtx);
        }
        /* Apply staged device list to the local app (main thread). */
        if (s_client->staged_dirty) {
            net_mutex_lock(&s_client->dev_mtx);
            int count = s_client->staged_device_count;
            if (count > MAX_DEVICES) count = MAX_DEVICES;
            for (int i = 0; i < count; i++) {
                app->devices[i] = s_client->staged_devices[i];
            }
            app->device_count = count;
            int sel = s_client->staged_selected;
            if (sel >= 0 && sel < count) app->selected_device = sel;
            s_client->staged_dirty = false;
            net_mutex_unlock(&s_client->dev_mtx);
        }
        /* No client capture/extract/display threads. Status only mirrors server. */
        bool online = atomic_load(&s_client->connected);
        int peer_state = atomic_load(&s_client->peer_state);
        app->is_capturing = online && peer_state >= 1;
        bool was_recording = app->is_recording;
        app->is_recording = online && peer_state >= 2;
        if (online) {
            atomic_store(&app->samples_a, atomic_load(&s_client->peer_samples_a));
            atomic_store(&app->samples_b, atomic_load(&s_client->peer_samples_b));
            atomic_store(&app->total_samples, atomic_load(&s_client->peer_total_samples));
            atomic_store(&app->frame_count, atomic_load(&s_client->peer_frames));
            atomic_store(&app->missed_frame_count, atomic_load(&s_client->peer_missed));
            atomic_store(&app->error_count, atomic_load(&s_client->peer_errors));
            atomic_store(&app->stream_synced, atomic_load(&s_client->peer_synced));
            atomic_store(&app->recording_raw_a, atomic_load(&s_client->peer_raw_a));
            atomic_store(&app->recording_raw_b, atomic_load(&s_client->peer_raw_b));
            atomic_store(&app->recording_compressed_a, atomic_load(&s_client->peer_flac_a));
            atomic_store(&app->recording_compressed_b, atomic_load(&s_client->peer_flac_b));
            atomic_store(&app->clip_count_a_pos, atomic_load(&s_client->peer_clip_a_pos));
            atomic_store(&app->clip_count_a_neg, atomic_load(&s_client->peer_clip_a_neg));
            atomic_store(&app->clip_count_b_pos, atomic_load(&s_client->peer_clip_b_pos));
            atomic_store(&app->clip_count_b_neg, atomic_load(&s_client->peer_clip_b_neg));
            atomic_store(&app->net_remote_rf_buffer_pct, atomic_load(&s_client->peer_rf_pct));
            atomic_store(&app->net_remote_audio_buffer_pct, atomic_load(&s_client->peer_audio_pct));
            atomic_store(&app->net_remote_free_known, atomic_load(&s_client->peer_free_known));
            atomic_store(&app->net_remote_free_bytes, atomic_load(&s_client->peer_free_bytes));
            if (app->is_recording) {
                /* Server reports elapsed time, not its monotonic timestamp.
                 * Never subtract the server clock directly from GetTime(). */
                double elapsed = (double)atomic_load(&s_client->peer_record_elapsed_ms) / 1000.0;
                double now = GetTime();
                if (elapsed < 0.0) elapsed = 0.0;
                if (!was_recording || elapsed < (now - app->recording_start_time) - 2.0)
                    app->recording_start_time = now - elapsed;
                /* Don't continually pull the timer backward on HTTP jitter. */
                app->last_recording_duration_s = 0.0;
            } else {
                app->last_recording_duration_s = (double)atomic_load(&s_client->peer_last_record_ms) / 1000.0;
            }
        }
        gui_rd_frame_t preview;
        if (app->is_capturing && gui_rd_client_snapshot(&preview)) {
            /* Mirror Local gui_extract_update_stats: signed 12-bit ADC
             * magnitudes from the first 1000 samples, NO /16 scaling.
             * The v3 division by 16 suppressed the remote meter by 24 dB. */
            atomic_store(&app->peak_a_pos, preview.ch[0].peak_pos);
            atomic_store(&app->peak_a_neg, preview.ch[0].peak_neg);
            atomic_store(&app->peak_b_pos, preview.ch[1].peak_pos);
            atomic_store(&app->peak_b_neg, preview.ch[1].peak_neg);
        } else {
            atomic_store(&app->peak_a_pos, 0);
            atomic_store(&app->peak_a_neg, 0);
            atomic_store(&app->peak_b_pos, 0);
            atomic_store(&app->peak_b_neg, 0);
        }
        /* Mirror peer sample rate into app for display. The local capture
         * path updates app->sample_rate from stream metadata every frame;
         * the network client only gets the rate from /stats, so update it
         * every mirror poll (not just at ingest start) so the display time
         * axis stays 1:1 with the server when the server's feed rate changes. */
        int psr = atomic_load(&s_client->peer_sample_rate);
        if (psr > 0) atomic_store(&app->sample_rate, (uint32_t)psr);
    }
}

void gui_net_client_request_start(gui_app_t *app) {
    if (!app) return;
    atomic_store(&app->net_cmd_start, true);
}

void gui_net_client_request_stop(gui_app_t *app) {
    if (!app) return;
    atomic_store(&app->net_cmd_stop, true);
}

void gui_net_client_request_record(gui_app_t *app, bool on) {
    if (!app) return;
    if (on) atomic_store(&app->net_cmd_record_on, true);
    else atomic_store(&app->net_cmd_record_off, true);
}

void gui_net_client_request_device(gui_app_t *app, int device_index) {
    if (!app) return;
    atomic_store(&app->net_cmd_device_index, device_index);
    atomic_store(&app->net_cmd_select_device, true);
}

/* Discovery: return the current count of discovered servers (client mode). */
int gui_net_discovered_count(void) {
    if (!s_client) return 0;
    net_mutex_lock(&s_client->disc_mtx);
    int n = s_client->discovered_count;
    net_mutex_unlock(&s_client->disc_mtx);
    return n;
}

/* Discovery: copy discovered server #index into the caller buffers. Returns
 * false if index is out of range or not in client mode. */
bool gui_net_get_discovered(int index, char *host, size_t host_cap,
                            uint16_t *port, char *name, size_t name_cap) {
    if (!s_client) return false;
    bool ok = false;
    net_mutex_lock(&s_client->disc_mtx);
    if (index >= 0 && index < s_client->discovered_count) {
        const net_discovered_t *d = &s_client->discovered[index];
        if (host && host_cap) {
            snprintf(host, host_cap, "%s", d->host);
        }
        if (port) *port = d->port;
        if (name && name_cap) {
            snprintf(name, name_cap, "%s", d->name);
        }
        ok = true;
    }
    net_mutex_unlock(&s_client->disc_mtx);
    return ok;
}

/* Discovery: select discovered server #index as the connection target. Sets
 * the client host/port settings and re-applies mode so the worker reconnects. */
void gui_net_select_discovered(gui_app_t *app, int index) {
    if (!app || !s_client) return;
    char host[64] = {0};
    uint16_t port = 0;
    char name[64] = {0};
    if (!gui_net_get_discovered(index, host, sizeof(host), &port, name, sizeof(name))) {
        return;
    }
    snprintf(app->settings.net_client_host, sizeof(app->settings.net_client_host), "%s", host);
    app->settings.net_client_port = port;
    snprintf(app->settings.net_client_port_str, sizeof(app->settings.net_client_port_str), "%u", (unsigned)port);
    gui_settings_save(&app->settings);
    (void)gui_net_apply_mode(app);
    char msg[160];
    snprintf(msg, sizeof(msg), "Connecting to discovered server %s:%u (%s)", host, (unsigned)port, name);
    gui_net_set_status(app, msg);
}

/* True when client connection is active at the worker level (connected or
 * currently trying). Used by the UI Action button label/state. */
bool gui_net_client_connection_running(const gui_app_t *app) {
    (void)app;
    if (!s_client) return false;
    return atomic_load(&s_client->worker_started);
}

bool gui_net_client_peer_capturing(const gui_app_t *app) {
    (void)app;
    if (!s_client) return false;
    if (!atomic_load(&s_client->connected)) return false;
    return atomic_load(&s_client->peer_state) >= 1;
}

/* Toggle client connect/disconnect while remaining in Client mode:
 * - if currently connected, stop worker (disconnect; keep discovery alive)
 * - if disconnected, (re)start worker using current host:port settings. */
void gui_net_client_toggle_connection(gui_app_t *app) {
    if (!app) return;
    if (app->settings.net_mode != GUI_NET_MODE_CLIENT) {
        return;
    }
    gui_net_init_globals();

    long p = atol(app->settings.net_client_port_str);
    if (p < 1 || p > 65535) p = (long)app->settings.net_client_port;
    if (p < 1 || p > 65535) p = 8080;
    app->settings.net_client_port = (uint16_t)p;
    const char *h = app->settings.net_client_host;

    if (!s_client) {
        (void)gui_net_apply_mode(app);
        return;
    }

    bool connected_now = atomic_load(&s_client->connected);
    if (connected_now) {
        client_stop_worker(s_client);
        if (h && h[0]) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Client disconnected from %s:%u", h, (unsigned)p);
            gui_net_set_status(app, msg);
        } else {
            gui_net_set_status(app, "Client disconnected (discovery still active)");
        }
        return;
    }

    if (!h || !h[0]) {
        gui_net_set_status(app, "Client mode: select a discovered server or enter host:port");
        return;
    }

    snprintf(s_client->host, sizeof(s_client->host), "%s", h);
    s_client->port = (uint16_t)p;
    if (atomic_load(&s_client->worker_started)) {
        client_stop_worker(s_client);
    }
    if (client_start_worker(s_client) != 0) {
        gui_net_set_status(app, "Failed to start client worker");
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Network client connecting to %s:%u", h, (unsigned)p);
    gui_net_set_status(app, msg);
}

void gui_net_client_request_control(gui_app_t *app, const char *key, int value) {
    if (!app || app->settings.net_mode!=GUI_NET_MODE_CLIENT || !s_client) return;
    int id=gui_net_ctrl_find(key);
    if (id<0 || !gui_net_ctrl_valid(id,value)) return;
    atomic_store(&s_client->requested_controls[id],value);
}

/* Remote Settings v7: the server is authoritative for recording paths and
 * filenames. The render thread queues explicit user commits; the worker sends
 * them and later mirrors the confirmed server state. */
void gui_net_client_request_string(gui_app_t *app,const char *key,const char *value){
    if(!app||!s_client||!value)return;
    int id=gui_net_str_find(key);if(id<0)return;
    size_t cap=0;char *field=gui_net_str_field(&app->settings,id,&cap);
    if(!field||strlen(value)>=cap)return;
    net_mutex_lock(&s_client->str_mtx);
    snprintf(s_client->requested_strings[id],GUI_NET_STR_MAX,"%s",value);
    s_client->requested_string[id]=true;
    s_client->waiting_string[id]=false;
    s_client->mirrored_string[id]=false;
    net_mutex_unlock(&s_client->str_mtx);
}
void gui_net_client_set_text_editing(gui_app_t *app,bool editing){
    if(app&&s_client)atomic_store(&s_client->text_edit_active,editing);
}

/* Browse absolute paths on the server. Nothing is interpreted on the client filesystem. */
void gui_net_client_browse_folders(gui_app_t *app,const char *relative,unsigned page){
    if(!app||!s_client||!gui_net_is_client(app))return;
    net_mutex_lock(&s_client->str_mtx);
    snprintf(s_client->browse_relative,sizeof(s_client->browse_relative),"%s",relative?relative:"");
    s_client->browse_page=page;
    s_client->folder_count=0;s_client->folders_ready=false;
    s_client->browse_has_more=false;
    net_mutex_unlock(&s_client->str_mtx);
    atomic_store(&s_client->browse_requested,true);
}
bool gui_net_client_folder_state(const gui_app_t *app,char *path,size_t pathcap,
    char *parent,size_t parentcap,bool *recovered,bool *computer,bool *error){
    if(!app||!s_client||!gui_net_is_client(app))return false;
    net_mutex_lock(&s_client->str_mtx);
    bool ready=s_client->folders_ready;
    if(ready){
        if(path&&pathcap)snprintf(path,pathcap,"%s",s_client->browse_path);
        if(parent&&parentcap)snprintf(parent,parentcap,"%s",s_client->browse_parent);
        if(recovered)*recovered=s_client->browse_recovered;
        if(computer)*computer=s_client->browse_computer;
        if(error)*error=s_client->browse_error;
    }
    net_mutex_unlock(&s_client->str_mtx);return ready;
}
bool gui_net_client_folder_has_more(const gui_app_t *app){
    if(!app||!s_client||!gui_net_is_client(app))return false;
    net_mutex_lock(&s_client->str_mtx);
    bool more=s_client->folders_ready&&s_client->browse_has_more;
    net_mutex_unlock(&s_client->str_mtx);
    return more;
}
int gui_net_client_folder_count(const gui_app_t *app){
    if(!app||!s_client)return -1;
    net_mutex_lock(&s_client->str_mtx);
    int count=s_client->folders_ready?s_client->folder_count:-1;
    net_mutex_unlock(&s_client->str_mtx);
    return count;
}
bool gui_net_client_folder_name(const gui_app_t *app,int index,char *out,size_t cap){
    if(!app||!s_client||!out||!cap)return false;
    bool ok=false;
    net_mutex_lock(&s_client->str_mtx);
    if(s_client->folders_ready&&index>=0&&index<s_client->folder_count){
        snprintf(out,cap,"%s",s_client->folders[index]);ok=true;
    }
    net_mutex_unlock(&s_client->str_mtx);return ok;
}

void gui_net_client_storage_status(gui_app_t *app,char *out,size_t cap){
    if(!out||!cap)return;
    if(!app||!s_client||!atomic_load(&s_client->connected)){
        snprintf(out,cap,"Remote storage: disconnected");return;
    }
    if(!atomic_load(&s_client->peer_storage_seen)){
        snprintf(out,cap,"Remote storage: checking server...");return;
    }
    if(!atomic_load(&s_client->peer_directory_exists)){
        snprintf(out,cap,"Server folder does not exist");return;
    }
    if(!atomic_load(&s_client->peer_free_known)){
        snprintf(out,cap,"Server folder exists (free space unavailable)");return;
    }
    uint64_t size=atomic_load(&s_client->peer_free_bytes);
    snprintf(out,cap,"Server disk: %.1f GiB free",(double)size/(1024.0*1024.0*1024.0));
}
