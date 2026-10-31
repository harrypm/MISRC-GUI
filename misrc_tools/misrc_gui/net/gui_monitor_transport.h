/* Versioned low-bandwidth monitor endpoints on the shared RF/monitor Server.
 * The monitor client still uses a separate protocol, never raw RF ingest. */
#ifndef GUI_MONITOR_TRANSPORT_H
#define GUI_MONITOR_TRANSPORT_H
#include "../core/gui_app.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* M-Server/M-Client display preview: original phosphor renderer data,
 * Zstd-1 on-wire, <=10 fps. Encoding and decoding: gui_remote_display.h. */
int gui_monitor_server_start(gui_app_t *app, uint16_t port);
/* The caller owns the accepted socket unless this returns true: Local Save
 * then takes ownership and closes it when the receiver shuts down. */
bool gui_monitor_server_handle_request(uintptr_t socket_handle,
                                       gui_app_t *app, const char *request_headers);
void gui_monitor_server_stop(void);
/* Record requests are accepted by the socket worker and executed later on
 * the server main thread. The request id correlates the later result with
 * the requesting M-Client; status results are kept per server session. */
uint64_t gui_monitor_server_queue_record(gui_app_t *app, int command);
int gui_monitor_server_take_record(gui_app_t *app, uint64_t *request_id);
void gui_monitor_server_record_result(uint64_t request_id, unsigned result,
                                      const char *detail);
bool gui_monitor_server_active(void);
uint16_t gui_monitor_server_port(void);
int gui_monitor_client_start(gui_app_t *app, const char *host, uint16_t port);
void gui_monitor_client_stop(void);
/* Consume a stop's staged local-profile/device restore on the MAIN thread
 * (gui_monitor_client_stop may run on the net transition worker; app->settings
 * and app->devices are main-thread-owned). Returns true when a restore was
 * pending and applied. Also applied synchronously by gui_net_stop at exit. */
bool gui_monitor_client_take_pending_restore(gui_app_t *app);
bool gui_monitor_client_running(void);
bool gui_monitor_client_matches(const char *host, uint16_t port);
bool gui_monitor_client_connected(void);
int gui_monitor_client_peer_state(void);
uint32_t gui_monitor_client_peer_rate(void);
uint32_t gui_monitor_client_peer_frames(void);
uint32_t gui_monitor_client_peer_errors(void);
uint32_t gui_monitor_client_peer_missed(void);
/* Snapshot from the server, not local M-Client capture counters. All
 * fields are delivered with /monitor/status, no RF data transfer. */
typedef struct gui_monitor_record_stats {
    uint64_t elapsed_ms,raw[2],compressed[2],samples_a;
    uint64_t send_queue_bytes,send_queue_capacity;
    unsigned rec_fill_pct[2],rec_waits[2],rec_drops[2],rf_fill_pct,aud_fill_pct;
    bool capture_a,capture_b,use_flac,local_save;
    /* Display values from the server (all optional outside verified M/2). */
    bool stream_synced,server_free_valid;
    uint64_t server_free_bytes;
    uint32_t clip_pos[2],clip_neg[2];
} gui_monitor_record_stats_t;
bool gui_monitor_client_record_stats(gui_monitor_record_stats_t *out);
/* Queue a remote, per-channel clip counter reset on the M-Client worker. */
void gui_monitor_client_reset_clips(int channel);
/* 0=none/connecting, 1=unreachable, 2=legacy MISRC-M/1, 3=unknown response. */
int gui_monitor_client_last_error(void);
/* Per-session request error counters, independent of server capture errors.
 * Established monitor sessions are retained through request dropouts. */
uint32_t gui_monitor_client_status_dropouts(void);
uint32_t gui_monitor_client_preview_dropouts(void);
bool gui_monitor_client_preview_error(void);
/* Read-only server filesystem navigation and server-owned Output folder.
 * Available only when connected via MISRC-M/2; no local path mixing. */
void gui_monitor_client_browse_folders(const char *path, unsigned page);
/* Returns -1 while pending; otherwise number of subfolders (0..12). */
int gui_monitor_client_folder_count(void);
bool gui_monitor_client_folder_state(char *path,size_t cap,char *parent,size_t pcap,
                                    bool *computer,bool *recovered,bool *failed,bool *more);
bool gui_monitor_client_folder_name(unsigned index,char *name,size_t cap);
void gui_monitor_client_select_output(const char *path);
bool gui_monitor_client_output_path(char *out,size_t cap);
void gui_monitor_server_poll_settings(gui_app_t *app);
/* Main-thread-only: mirror M-Server recording config and device list. */
void gui_monitor_client_sync_main(gui_app_t *app);
#endif
