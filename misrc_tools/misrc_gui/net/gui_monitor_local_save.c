/* MISRC-M only: client-owned files, never full RF monitoring. */
#include "gui_monitor_local_save.h"
#include "monitor_local_save_protocol.h"
#include "monitor_local_save_files.h"
#include "monitor_local_save_proxy.h"
#include "../../common/threading.h"
#include "../../common/file_utils.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#ifdef _WIN32
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
#include <windows.h>
typedef SOCKET net_sock_t;
#define NET_INVALID_SOCKET INVALID_SOCKET
#define net_close(s) closesocket(s)
typedef CRITICAL_SECTION net_mutex_t;
typedef CONDITION_VARIABLE net_cond_t;
#define net_mutex_init(m) InitializeCriticalSection(m)
#define net_mutex_lock(m) EnterCriticalSection(m)
#define net_mutex_unlock(m) LeaveCriticalSection(m)
#define net_cond_init(c) InitializeConditionVariable(c)
#define net_cond_broadcast(c) WakeAllConditionVariable(c)
#define mls_cond_wait(c,m) ((void)SleepConditionVariableCS(c,m,INFINITE))
#else
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
typedef int net_sock_t;
#define NET_INVALID_SOCKET (-1)
#define net_close(s) close(s)
typedef pthread_mutex_t net_mutex_t;
typedef pthread_cond_t net_cond_t;
#define net_mutex_init(m) ((void)pthread_mutex_init(m,NULL))
#define net_mutex_lock(m) ((void)pthread_mutex_lock(m))
#define net_mutex_unlock(m) ((void)pthread_mutex_unlock(m))
#define net_cond_init(c) ((void)pthread_cond_init(c,NULL))
#define net_cond_broadcast(c) ((void)pthread_cond_broadcast(c))
#define mls_cond_wait(c,m) ((void)pthread_cond_wait(c,m))
#endif
static bool net_sock_valid(net_sock_t fd){return fd!=NET_INVALID_SOCKET;}
static void net_set_timeouts(net_sock_t fd,int rcv_ms,int snd_ms){
#ifdef _WIN32
 DWORD r=(DWORD)rcv_ms,s=(DWORD)snd_ms;
 setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char*)&r,sizeof(r));
 setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,(const char*)&s,sizeof(s));
#else
 struct timeval tv={rcv_ms/1000,(rcv_ms%1000)*1000};
 setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
 tv.tv_sec=snd_ms/1000;tv.tv_usec=(snd_ms%1000)*1000;
 setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
#endif
}
static void mls_enable_tcp_nodelay(net_sock_t fd){int on=1;
 setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(const char*)&on,sizeof(on));}
static int net_send_all(net_sock_t fd,const void *data,size_t n){
 const char*p=(const char*)data;
 while(n){
#ifdef _WIN32
 int k=send(fd,p,(int)(n>1048576?1048576:n),0);
#else
 ssize_t k=send(fd,p,n>1048576?1048576:n,0);
#endif
 if(k<=0){
#ifndef _WIN32
 if(k<0 && errno==EINTR)continue;
#endif
 return -1;}
 p+=k;n-=(size_t)k;
 }return 0;
}
static int mls_read_headers(net_sock_t fd,char *buf,size_t cap){
 size_t n=0; while(n<cap-1){
#ifdef _WIN32
 int k=recv(fd,buf+n,(int)(cap-1-n),0);
#else
 ssize_t k=recv(fd,buf+n,cap-1-n,0);
#endif
 if(k<=0)return -1; n+=(size_t)k;buf[n]=0;
 if(strstr(buf,"\r\n\r\n"))return (int)n;
 }return -1;
}
static net_mutex_t s_mls_mtx;
static bool s_mls_lock_ready = false;
static net_sock_t s_mls_fd = NET_INVALID_SOCKET;
static atomic_bool s_mls_active = false;
static atomic_bool s_mls_record = false;
static atomic_bool s_mls_failed = false;
static atomic_bool s_mls_commit_unconfirmed = false;
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
bool gui_monitor_local_save_recording(void) { return s_mls_record; }
bool gui_monitor_local_save_failed(void) { return s_mls_failed; }
bool gui_monitor_local_save_commit_unconfirmed(void) { return atomic_load(&s_mls_commit_unconfirmed); }
int gui_monitor_local_save_begin(void) {
    if (!s_mls_lock_ready) return -1;
    net_mutex_lock(&s_mls_mtx);
    int ok = s_mls_active && !s_mls_record && !s_mls_failed && net_sock_valid(s_mls_fd);
    if (ok) {
        mls_proxy_reset_session_ids();
        atomic_store(&s_mls_commit_unconfirmed, false);
        s_mls_write_bytes = 0;
        s_mls_write_frames = 0;
        s_mls_write_seconds = 0.0;
        s_mls_next_report = 32u * 1024u * 1024u;
        s_mls_last_ack_time = ((double)get_time_ms()/1000.0);
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
    double write_started = type == MLS_WRITE ? ((double)get_time_ms()/1000.0) : 0.0;
    bool sent = mls_encode_header(h,&f)==0 && net_send_all(s_mls_fd,h,sizeof(h))==0 &&
                (!bytes || net_send_all(s_mls_fd,buf,bytes)==0);
    bool answered = sent && mls_recv_exact(s_mls_fd,h,sizeof(h))==0 &&
                    mls_decode_header(&ack,h,sizeof(h))==0 && ack.type==MLS_ACK &&
                    ack.file_id==id && ack.length==0 && ack.offset<=1;
    bool ok = answered && ack.offset==0;
    if (type==MLS_COMMIT && sent && !answered)
        atomic_store(&s_mls_commit_unconfirmed, true);
    if(ok) s_mls_last_ack_time = ((double)get_time_ms()/1000.0);
    if(ok && type == MLS_WRITE) {
        double elapsed = ((double)get_time_ms()/1000.0) - write_started;
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
            if(!outstanding)batch_started=((double)get_time_ms()/1000.0);
            result=mls_send_wire_stream(job->id,job->offset,job->payload,job->bytes);
            if(result==0){outstanding++;outstanding_bytes+=job->bytes;}
        }
        /* Limit the unacknowledged window to 8 MiB, and turn all control
         * frames (including CLOSE/COMMIT) into full ordering barriers. */
        if(result==0 && (outstanding>=8 ||
                          (outstanding && job->type!=MLS_WRITE))) {
            result=mls_send_wire_sync(MLS_SYNC,0,0,NULL,0);
            if(result==0){
                double elapsed=((double)get_time_ms()/1000.0)-batch_started;
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
int gui_monitor_local_save_send(unsigned type, unsigned id, uint64_t offset,
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

bool gui_monitor_local_save_end(bool commit) {
    if(!s_mls_lock_ready)return false;
    bool complete = false;
    if(s_mls_record && commit && !s_mls_failed) {
        complete = gui_monitor_local_save_send(MLS_COMMIT,0,0,NULL,0)==0;
        if(!complete)
            fprintf(stderr, "[Local Save] %s\n", gui_monitor_local_save_commit_unconfirmed() ?
                    "COMMIT was sent but the receiver outcome is unconfirmed" :
                    "COMMIT was rejected or could not be sent");
    } else if(s_mls_record && s_mls_active && !s_mls_failed) {
        (void)gui_monitor_local_save_send(MLS_ABORT,0,0,NULL,0);
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
void gui_monitor_local_save_queue_stats(size_t *used,size_t *capacity) {
    if (used) *used=0;
    if (capacity) *capacity=MLS_QUEUE_MAX_BYTES;
    if (!s_mls_q_ready) return;
    net_mutex_lock(&s_mls_q_mtx);
    if (used) *used=s_mls_q_bytes;
    net_mutex_unlock(&s_mls_q_mtx);
}

void gui_monitor_local_save_diagnostics(char *buf,size_t cap) {
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
    double age = last_ack > 0.0 ? ((double)get_time_ms()/1000.0) - last_ack : 0.0;
    if (age < 0.0) age = 0.0;
    snprintf(buf, cap, "queue=%.1f/64MiB peak=%.1fMiB acked=%.1fMiB ack_age=%.1fs failed=%d",
             (double)queued / 1048576.0, (double)peak / 1048576.0,
             (double)acknowledged / 1048576.0, age, failed ? 1 : 0);
}


/* The dedicated socket stays attached while the M-Client polls status and
 * preview over independent short-lived HTTP connections. */
static atomic_bool s_server_shutdown=false;
static bool s_attached_thread=false;
static thrd_t s_attach_thread;
static atomic_bool s_receiver_ready=false,s_receiver_running=false,s_receiver_stop=false;
static bool s_receiver_thread_valid=false;
static thrd_t s_receiver_thread;
static char s_receiver_directory[MLS_PATH_MAX],s_receiver_host[128];
static uint16_t s_receiver_port;
static atomic_bool s_receiver_error=false;
static atomic_uintptr_t s_receiver_socket=0;
static atomic_flag s_receiver_notice_lock=ATOMIC_FLAG_INIT;
static char s_receiver_notice[256];

static void mls_lock_receiver_notice(void) {
 while(atomic_flag_test_and_set_explicit(&s_receiver_notice_lock,memory_order_acquire))
  thrd_sleep_ms(1);
}
static void mls_unlock_receiver_notice(void) {
 atomic_flag_clear_explicit(&s_receiver_notice_lock,memory_order_release);
}
static void mls_note_receiver_publication(const char *path,bool ack_sent) {
 mls_lock_receiver_notice();
 snprintf(s_receiver_notice,sizeof(s_receiver_notice),"Local Save published %.185s%s",
          path,ack_sent ? "" : " (server ACK could not be sent)");
 mls_unlock_receiver_notice();
}
bool gui_monitor_local_save_client_take_notice(char *notice,size_t capacity) {
 if(!notice || !capacity)return false;
 mls_lock_receiver_notice();
 bool available=s_receiver_notice[0]!=0;
 if(available){
  snprintf(notice,capacity,"%s",s_receiver_notice);
  s_receiver_notice[0]=0;
 }
 mls_unlock_receiver_notice();
 return available;
}

static int mls_server_attached(void *unused){
 (void)unused;
 /* While a recording runs only the sender consumes ACK bytes.  When idle,
  * MSG_PEEK detects a disconnected receiver without stealing anything. */
 while(!atomic_load(&s_server_shutdown)){
  net_mutex_lock(&s_mls_mtx);
  bool active=s_mls_active && net_sock_valid(s_mls_fd);
  bool recording=s_mls_record;
  net_sock_t fd=s_mls_fd;
  net_mutex_unlock(&s_mls_mtx);
  if(!active)break;
  if(!recording){
   fd_set set;FD_ZERO(&set);FD_SET(fd,&set);struct timeval tv={0,100000};
   int select_rc=select((int)fd+1,&set,NULL,NULL,&tv);
   if(select_rc>0){char c;int got=recv(fd,&c,1,MSG_PEEK);if(got<=0)break;}
   else if(select_rc<0)break;
  }else thrd_sleep_ms(100);
 }
 net_mutex_lock(&s_mls_mtx);
 net_sock_t fd=s_mls_fd;
 s_mls_fd=NET_INVALID_SOCKET;
 s_mls_active=false;s_mls_failed=true;
 net_mutex_unlock(&s_mls_mtx);
 if(net_sock_valid(fd))net_close(fd);
 return 0;
}
void gui_monitor_local_save_server_init(void){
 if(!s_mls_lock_ready){net_mutex_init(&s_mls_mtx);s_mls_lock_ready=true;}
 atomic_store(&s_server_shutdown,false);
}
int gui_monitor_local_save_server_attach(uintptr_t socket_handle){
 gui_monitor_local_save_server_init();
 if(s_attached_thread){
  net_mutex_lock(&s_mls_mtx);bool busy=s_mls_active||s_mls_record;net_mutex_unlock(&s_mls_mtx);
  if(busy)return -1;
  thrd_join(s_attach_thread,NULL);s_attached_thread=false;
 }
 net_mutex_lock(&s_mls_mtx);
 if(s_mls_active||s_mls_record||atomic_load(&s_server_shutdown)){
  net_mutex_unlock(&s_mls_mtx);return -1;
 }
 s_mls_fd=(net_sock_t)socket_handle;s_mls_active=true;s_mls_failed=false;
 net_set_timeouts(s_mls_fd,15000,15000);mls_enable_tcp_nodelay(s_mls_fd);
 if(net_send_all(s_mls_fd,"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n",strlen("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n"))!=0){
  s_mls_fd=NET_INVALID_SOCKET;s_mls_active=false;
  net_mutex_unlock(&s_mls_mtx);return -1;
 }
 net_mutex_unlock(&s_mls_mtx);
 if(thrd_create(&s_attach_thread,mls_server_attached,NULL)!=thrd_success){
  net_mutex_lock(&s_mls_mtx);s_mls_fd=NET_INVALID_SOCKET;s_mls_active=false;net_mutex_unlock(&s_mls_mtx);
  return -1;
 }
 s_attached_thread=true;
 fprintf(stderr,"[M-LOCAL] Monitor & Remote file receiver attached\n");
 return 0;
}
void gui_monitor_local_save_server_shutdown(void){
 if(!s_mls_lock_ready)return;
 atomic_store(&s_server_shutdown,true);
 net_mutex_lock(&s_mls_mtx);net_sock_t fd=s_mls_fd;net_mutex_unlock(&s_mls_mtx);
 if(net_sock_valid(fd)){
#ifdef _WIN32
 shutdown(fd,SD_BOTH);
#else
 shutdown(fd,SHUT_RDWR);
#endif
 }
 if(s_attached_thread){thrd_join(s_attach_thread,NULL);s_attached_thread=false;}
}

static int mls_client_recv_thread(void *unused){
 (void)unused;
 mls_receiver_t *receiver=malloc(sizeof(*receiver));
 if(!receiver||mls_receiver_init(receiver,s_receiver_directory,false)!=0)goto done;
 char service[8];snprintf(service,sizeof(service),"%u",(unsigned)s_receiver_port);
 struct addrinfo hints={0},*addr=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
 if(getaddrinfo(s_receiver_host,service,&hints,&addr)!=0)goto done;
 net_sock_t fd=NET_INVALID_SOCKET;
 for(struct addrinfo *p=addr;p;p=p->ai_next){
  fd=socket(p->ai_family,p->ai_socktype,p->ai_protocol);
  if(!net_sock_valid(fd))continue;
  net_set_timeouts(fd,5000,5000);
  if(connect(fd,p->ai_addr,(int)p->ai_addrlen)==0)break;
  net_close(fd);fd=NET_INVALID_SOCKET;
 }
 freeaddrinfo(addr);
 if(!net_sock_valid(fd))goto done;
 mls_enable_tcp_nodelay(fd);
 atomic_store(&s_receiver_socket,(uintptr_t)fd);
 if(net_send_all(fd,"GET /monitor/local-save/attach HTTP/1.0\r\n\r\n",strlen("GET /monitor/local-save/attach HTTP/1.0\r\n\r\n"))!=0)goto close_fd;
 char header[1024];
 if(mls_read_headers(fd,header,sizeof(header))<0||strncmp(header,"HTTP/1.0 200",12)!=0)goto close_fd;
 /* No timeout on idle receive. Stop shuts down the socket explicitly. */
 net_set_timeouts(fd,0,15000);
 /* Client stop must be able to shutdown an idle recv without a data race. */
 atomic_store(&s_receiver_ready,true);
 while(!atomic_load(&s_receiver_stop)){
  uint8_t h[MLS_HEADER_SIZE]; mls_frame_t f={0},ack={0};
  if(mls_recv_exact(fd,h,sizeof(h))!=0||mls_decode_header(&f,h,sizeof(h))!=0)break;
  uint8_t *payload=NULL;
  if(f.length){payload=malloc(f.length);if(!payload)break;
   if(mls_recv_exact(fd,payload,f.length)!=0){free(payload);break;}}
  int ok=mls_receiver_apply(receiver,&f,payload);free(payload);
  if(f.type==MLS_WRITE_STREAM && ok==0)continue;
  ack.type=MLS_ACK;ack.file_id=f.file_id;ack.offset=ok==0?0:1;
  bool sent=mls_encode_header(h,&ack)==0 && net_send_all(fd,h,sizeof(h))==0;
  if(ok==0 && f.type==MLS_COMMIT){
   // The files are already published. A socket failure changes only what
   // the server can confirm, so keep and report the known receiver result.
   mls_receiver_finish_commit(receiver);
   fprintf(stderr,"[M-LOCAL] Published %s%s\n", receiver->published_path,
           sent ? "" : " (could not acknowledge the server)");
   mls_note_receiver_publication(receiver->published_path,sent);
   if(!sent || mls_receiver_init(receiver,s_receiver_directory,false)!=0)break;
  }
  if(!sent || ok!=0 || f.type==MLS_ABORT)break;
 }
 mls_receiver_abort(receiver);
close_fd:
 atomic_store(&s_receiver_ready,false);
 atomic_store(&s_receiver_socket,0);
 net_close(fd);
done:
 if(receiver)free(receiver);
 if(!atomic_load(&s_receiver_stop))atomic_store(&s_receiver_error,true);
 atomic_store(&s_receiver_ready,false);atomic_store(&s_receiver_running,false);
 return 0;
}
int gui_monitor_local_save_client_start(const char *host,uint16_t port,const char *directory){
 if(!host||!*host||!port||!directory||!*directory)return -1;
 if(s_receiver_thread_valid){
  if(atomic_load(&s_receiver_ready))return 0;
  if(atomic_load(&s_receiver_running))return -1;
  thrd_join(s_receiver_thread,NULL);s_receiver_thread_valid=false;
 }
 if(strlen(host)>=sizeof(s_receiver_host)||strlen(directory)>=sizeof(s_receiver_directory))return -1;
 snprintf(s_receiver_host,sizeof(s_receiver_host),"%s",host);
 snprintf(s_receiver_directory,sizeof(s_receiver_directory),"%s",directory);
 s_receiver_port=port;
 atomic_store(&s_receiver_stop,false);atomic_store(&s_receiver_ready,false);
 atomic_store(&s_receiver_error,false);atomic_store(&s_receiver_running,true);
 if(thrd_create(&s_receiver_thread,mls_client_recv_thread,NULL)!=thrd_success){
  atomic_store(&s_receiver_running,false);return -1;
 }
 s_receiver_thread_valid=true;
 for(int i=0;i<80;i++){
  if(atomic_load(&s_receiver_ready))return 0;
  if(!atomic_load(&s_receiver_running))break;
  thrd_sleep_ms(40);
 }
 gui_monitor_local_save_client_stop();return -1;
}
void gui_monitor_local_save_client_stop(void){
 if(!s_receiver_thread_valid)return;
 atomic_store(&s_receiver_stop,true);
 uintptr_t raw=atomic_load(&s_receiver_socket);
 if(raw){
#ifdef _WIN32
  shutdown((net_sock_t)raw,SD_BOTH);
#else
  shutdown((net_sock_t)raw,SHUT_RDWR);
#endif
 }
 thrd_join(s_receiver_thread,NULL);s_receiver_thread_valid=false;
 atomic_store(&s_receiver_ready,false);
}
bool gui_monitor_local_save_client_ready(void){return atomic_load(&s_receiver_ready);}
bool gui_monitor_local_save_client_error(void){return atomic_load(&s_receiver_error);}

bool gui_monitor_local_save_server_ready(void){
 if(!s_mls_lock_ready)return false;
 net_mutex_lock(&s_mls_mtx);
 bool ready=s_mls_active && !s_mls_record && !s_mls_failed;
 net_mutex_unlock(&s_mls_mtx);return ready;
}
