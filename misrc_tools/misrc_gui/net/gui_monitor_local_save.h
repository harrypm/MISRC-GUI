#ifndef GUI_MONITOR_LOCAL_SAVE_H
#define GUI_MONITOR_LOCAL_SAVE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Only MISRC M-Server/M-Client; never legacy /rf or /baseband. */
void gui_monitor_local_save_server_init(void);
int gui_monitor_local_save_server_attach(uintptr_t socket_handle);
void gui_monitor_local_save_server_shutdown(void);
bool gui_monitor_local_save_server_ready(void);
int gui_monitor_local_save_client_start(const char *host,uint16_t port,const char *directory);
void gui_monitor_local_save_client_stop(void);
bool gui_monitor_local_save_client_ready(void);
bool gui_monitor_local_save_client_error(void);
bool gui_monitor_local_save_client_take_notice(char *notice,size_t capacity);
bool gui_monitor_local_save_recording(void);
bool gui_monitor_local_save_failed(void);
bool gui_monitor_local_save_commit_unconfirmed(void);
int gui_monitor_local_save_begin(void);
int gui_monitor_local_save_send(unsigned type,unsigned id,uint64_t offset,const void *data,size_t bytes);
bool gui_monitor_local_save_end(bool commit);
void gui_monitor_local_save_diagnostics(char *buf,size_t cap);
/* Server-side bounded send queue statistics; lock only for a snapshot. */
void gui_monitor_local_save_queue_stats(size_t *used,size_t *capacity);
#endif
