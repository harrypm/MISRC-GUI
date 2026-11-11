#include "monitor_local_save_proxy.h"
#include "monitor_local_save_protocol.h"
/* Avoid importing the GUI graphics headers into the portable file proxy. */
#include <stdbool.h>
extern bool gui_monitor_local_save_recording(void);
extern bool gui_monitor_local_save_failed(void);
extern int gui_monitor_local_save_send(unsigned type,unsigned id,uint64_t offset,const void *buf,size_t bytes);
#include "../../common/file_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>

/* Batches four times more FLAC output per LAN acknowledgement. A single
 * transaction must never exceed the wire protocol's 1 MiB payload limit. */
#define MLS_CACHE_SIZE MLS_MAX_PAYLOAD
typedef struct {
    unsigned id;
    uint64_t pos, file_size, buffered_offset;
    size_t buffered;
    uint8_t *cache;
    int bad;
} proxy_t;
static atomic_uintptr_t s_handles[MLS_MAX_FILES];
/* Never recycle a wire ID inside one recording: CLOSE must not erase a
 * previously completed file from the receiver transaction. */
static atomic_uint s_next_id = ATOMIC_VAR_INIT(0);
void mls_proxy_reset_session_ids(void) {
    if (mls_proxy_open_count()==0) atomic_store(&s_next_id,0);
}

static proxy_t *lookup(const FILE *f) {
    for(unsigned i=0;i<MLS_MAX_FILES;i++) {
        uintptr_t v=atomic_load(&s_handles[i]);
        if(v && v==(uintptr_t)f) return (proxy_t *)v;
    }
    return NULL;
}
int mls_proxy_is(const FILE *f){return lookup(f)!=NULL;}
static int flush_proxy(proxy_t *p) {
    if(!p || p->bad)return -1;
    if(!p->buffered)return 0;
    if(gui_monitor_local_save_send(MLS_WRITE,p->id,p->buffered_offset,p->cache,p->buffered)!=0) {
        p->bad=1;return -1;
    }
    p->buffered=0;
    return 0;
}
FILE *mls_proxy_open(const char *path,const char *mode) {
    if(!gui_monitor_local_save_recording())return file_open_utf8(path,mode);
    if(!path || !mode || mode[0]!='w')return NULL;
    const char *name=path;
    for(const char *x=path;*x;x++) if(*x=='/'||*x=='\\')name=x+1;
    size_t n=strlen(name);
    if(!mls_safe_filename(name,n))return NULL;
    proxy_t *p=calloc(1,sizeof(*p));
    if(!p)return NULL;
    p->cache=malloc(MLS_CACHE_SIZE);
    if(!p->cache){free(p);return NULL;}
    /* Each file in a transaction owns a permanent ID even after CLOSE.
     * With the v2 wire format one transaction supports at most 16 files. */
    unsigned id=atomic_fetch_add(&s_next_id,1);
    if(id>=MLS_MAX_FILES){free(p->cache);free(p);return NULL;}
    uintptr_t expected=0;
    if(!atomic_compare_exchange_strong(&s_handles[id],&expected,(uintptr_t)p)){
        free(p->cache);free(p);return NULL;
    }
    p->id=id;
    if(gui_monitor_local_save_send(MLS_OPEN,id,0,name,n)==0)return (FILE *)p;
    atomic_store(&s_handles[id],0);free(p->cache);free(p);return NULL;
}
size_t mls_proxy_write(const void *data,size_t size,size_t count,FILE *f){
    proxy_t *p=lookup(f);
    if(!p)return fwrite(data,size,count,f);
    if(p->bad || (size && count>SIZE_MAX/size) || (!data && size && count))return 0;
    size_t bytes=size*count;
    const uint8_t *src=data;
    size_t remain=bytes;
    while(remain){
        if(p->buffered && p->buffered_offset+p->buffered!=p->pos && flush_proxy(p)!=0)break;
        if(!p->buffered)p->buffered_offset=p->pos;
        size_t avail=MLS_CACHE_SIZE-p->buffered;
        if(!avail){if(flush_proxy(p)!=0)break;continue;}
        size_t take=remain<avail?remain:avail;
        memcpy(p->cache+p->buffered,src,take);
        p->buffered+=take;p->pos+=take;src+=take;remain-=take;
        if(p->pos>p->file_size)p->file_size=p->pos;
    }
    return size ? (bytes-remain)/size : count;
}
int mls_proxy_seek(FILE *f,int64_t offset,int whence){
    proxy_t *p=lookup(f);
    if(!p){
#ifdef _WIN32
        return _fseeki64(f,offset,whence);
#else
        return fseeko(f,(off_t)offset,whence);
#endif
    }
    if(flush_proxy(p)!=0)return -1;
    uint64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?p->pos:whence==SEEK_END?p->file_size:UINT64_MAX;
    const uint64_t magnitude = offset < 0 ? (uint64_t)(-(offset+1)) + 1u : (uint64_t)offset;
    if(base==UINT64_MAX || (offset<0 && magnitude>base) ||
        (offset>=0 && magnitude>(uint64_t)INT64_MAX-base))return -1;
    p->pos=offset<0?base-magnitude:base+magnitude;
    return 0;
}
int64_t mls_proxy_tell(FILE *f){
    proxy_t *p=lookup(f);
    if(p)return p->pos<=INT64_MAX?(int64_t)p->pos:-1;
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}
int mls_proxy_flush(FILE *f){proxy_t *p=lookup(f);return p?flush_proxy(p):fflush(f);}
int mls_proxy_error(FILE *f){proxy_t *p=lookup(f);return p?p->bad:ferror(f);}
void mls_proxy_clearerr(FILE *f){proxy_t *p=lookup(f);if(!p)clearerr(f);}
int mls_proxy_setvbuf(FILE *f,char *buf,int mode,size_t size){return lookup(f)?0:setvbuf(f,buf,mode,size);}
int mls_proxy_close(FILE *f){
    proxy_t *p=lookup(f);
    if(!p)return fclose(f);
    int ok=flush_proxy(p)==0 && !gui_monitor_local_save_failed();
    if(ok && gui_monitor_local_save_send(MLS_CLOSE,p->id,0,NULL,0)!=0)ok=0;
    if(!ok){
        (void)gui_monitor_local_save_send(MLS_ABORT,p->id,0,NULL,0);
    }
    atomic_store(&s_handles[p->id],0);
    free(p->cache);free(p);
    return ok?0:EOF;
}
int mls_proxy_printf(FILE *f,const char *fmt,...){
    va_list ap;va_start(ap,fmt);
    if(!lookup(f)){int n=vfprintf(f,fmt,ap);va_end(ap);return n;}
    va_list cp;va_copy(cp,ap);int n=vsnprintf(NULL,0,fmt,cp);va_end(cp);
    if(n<0 || n>1048576){va_end(ap);return -1;}
    char *s=malloc((size_t)n+1);
    if(!s){va_end(ap);return -1;}
    vsnprintf(s,(size_t)n+1,fmt,ap);va_end(ap);
    size_t wr=mls_proxy_write(s,1,(size_t)n,f);free(s);
    return wr==(size_t)n?n:-1;
}

unsigned mls_proxy_open_count(void) {
    unsigned total=0;
    for(unsigned i=0;i<MLS_MAX_FILES;i++)total+=atomic_load(&s_handles[i])!=0;
    return total;
}
