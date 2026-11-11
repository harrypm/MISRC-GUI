#ifndef MISRC_MONITOR_LOCAL_SAVE_PROXY_H
#define MISRC_MONITOR_LOCAL_SAVE_PROXY_H
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
/* Capture output API. Real FILE* in normal mode, opaque client-owned file
 * proxy in Local Save mode. Never pass a proxy to the C runtime directly. */
FILE *mls_proxy_open(const char *path,const char *mode);
int mls_proxy_is(const FILE *file);
size_t mls_proxy_write(const void *data,size_t size,size_t count,FILE *file);
int mls_proxy_seek(FILE *file,int64_t offset,int whence);
int64_t mls_proxy_tell(FILE *file);
int mls_proxy_flush(FILE *file);
int mls_proxy_close(FILE *file);
int mls_proxy_error(FILE *file);
void mls_proxy_clearerr(FILE *file);
int mls_proxy_setvbuf(FILE *file,char *buffer,int mode,size_t size);
int mls_proxy_printf(FILE *file,const char *format,...);
unsigned mls_proxy_open_count(void);
void mls_proxy_reset_session_ids(void);
#endif
