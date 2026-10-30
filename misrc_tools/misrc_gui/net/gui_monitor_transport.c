/* Independent M-Server/M-Client transport. No full RF /rf or /baseband.
 * MRD5/Zstd-1 carries original Harry waveform/FFT display values only. */
#include "gui_monitor_transport.h"
#include "gui_monitor_session_state.h"
#include "gui_remote_display.h"
#include "gui_remote_fs.h"
#include "gui_monitor_settings.h"
#include "gui_monitor_local_save.h"
#include "../output/gui_record.h"
#include "../ui/gui_popup.h"
#include "../../common/buffer_manager.h"
#include "../../common/file_utils.h"
#include <inttypes.h>
#include "../../common/threading.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
/* The monitor socket layer includes raylib via gui_app.h. Hide the Win32
 * GDI/USER APIs whose symbols (e.g. Rectangle) collide with raylib. Match
 * the guards already used by Harry's original gui_net.c. */
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET mon_sock_t;
#define MON_BAD INVALID_SOCKET
#define mon_close closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int mon_sock_t;
#define MON_BAD (-1)
#define mon_close close
#endif

/* Display preview uses the original float32 waveform/FFT frame (MRD5)
 * and Zstd compression level 1. HTTP control remains MISRC-M/2. */
#define MON_PREVIEW_PERIOD_MS 100u
/* A 400 ms recv timeout caused spurious failures on busy M-Servers/Wi-Fi.
 * It bounds an individual send/recv wait, not the entire HTTP request. */
#define MON_HTTP_TIMEOUT_MS 1500
#define MON_REMOTE_PATH GUI_REMOTE_FS_PATH
#define MON_BROWSER_WIRE 40000u
static atomic_uint g_preview_transfers;
static atomic_uint_fast64_t g_preview_wire_bytes;

static int mon_arg(const char *query,const char *key,int fallback) {
    if(!query||!*query)return fallback;
    size_t klen=strlen(key);
    for(const char *p=query; *p; p++) {
        if((p==query||p[-1]=='&') && strncmp(p,key,klen)==0 && p[klen]=='=') {
            char *end=NULL;
            long value=strtol(p+klen+1,&end,10);
            if(end==p+klen+1 || (*end!='&' && *end!='\0') || value>100000000L || value< -100000000L)
                return fallback;
            return (int)value;
        }
    }
    return fallback;
}
static struct {
    gui_app_t *app;
    uint16_t port;
    atomic_bool active;
} g_server;
static struct {
    gui_app_t *app;
    char host[128];
    uint16_t port;
    thrd_t thread;
    atomic_bool running, stopping, connected;
    atomic_int peer_state;
    atomic_uint peer_rate;
    atomic_uint peer_frames, peer_errors, peer_missed;
    atomic_bool record_stats_valid;
    atomic_uint_fast64_t elapsed_ms,raw[2],compressed[2],samples_a;
    atomic_uint_fast64_t send_queue_bytes,send_queue_capacity;
    atomic_uint rec_fill_pct[2],rec_waits[2],rec_drops[2],rf_fill_pct,aud_fill_pct;
    atomic_bool capture_a,capture_b,use_flac,local_save;
    atomic_bool stream_synced,server_free_valid;
    atomic_uint_fast64_t server_free_bytes;
    atomic_uint clip_pos[2],clip_neg[2];
    atomic_bool reset_clips_pending[2];
    /* 0=none, 1=unreachable, 2=incompatible M-protocol, 3=invalid status. */
    atomic_int last_error;
    atomic_uint status_dropouts,preview_dropouts;
    atomic_bool preview_error;
} g_client;


/* Correlated asynchronous Record response. No filesystem I/O on socket
 * worker; the main thread publishes the outcome after executing the command.
 * The lock protects an atomic request-id + result + escaped message snapshot. */
static atomic_flag g_record_result_lock=ATOMIC_FLAG_INIT;
static void mon_record_lock(void){
    while(atomic_flag_test_and_set_explicit(&g_record_result_lock,memory_order_acquire))
        thrd_sleep_ms(1);
}
static void mon_record_unlock(void){
    atomic_flag_clear_explicit(&g_record_result_lock,memory_order_release);
}
static struct {
    uint64_t next_id,pending_id,completed_id;
    unsigned result_code; /* 0=started/stopped; 1=failed; 2=awaiting confirmation */
    char detail[256]; /* URL-encoded; safe to embed in a JSON string */
} g_record_result;
static atomic_flag g_record_ui_lock=ATOMIC_FLAG_INIT;
static char g_record_ui_message[512];
static bool g_record_ui_pending;
static void mon_record_ui_error(const char *message){
    while(atomic_flag_test_and_set_explicit(&g_record_ui_lock,memory_order_acquire))
        thrd_sleep_ms(1);
    snprintf(g_record_ui_message,sizeof(g_record_ui_message),"%s",message);
    g_record_ui_pending=true;
    atomic_flag_clear_explicit(&g_record_ui_lock,memory_order_release);
}
uint64_t gui_monitor_server_queue_record(gui_app_t *app,int command){
    if(!app || command<1 || command>3)return 0;
    mon_record_lock();
    uint64_t id=++g_record_result.next_id;
    if(!id)id=++g_record_result.next_id;
    g_record_result.pending_id=id;
    atomic_store(&app->net_cmd_monitor_record,command);
    mon_record_unlock();
    return id;
}
int gui_monitor_server_take_record(gui_app_t *app,uint64_t *request_id){
    if(request_id)*request_id=0;
    if(!app)return 0;
    mon_record_lock();
    int command=atomic_exchange(&app->net_cmd_monitor_record,0);
    if(command && request_id)*request_id=g_record_result.pending_id;
    if(command)g_record_result.pending_id=0;
    mon_record_unlock();
    return command;
}
void gui_monitor_server_record_result(uint64_t request_id,unsigned result,const char *detail){
    if(!request_id)return;
    char encoded[256]={0};
    /* Use the existing URL encoding rules (ASCII token, no JSON injection). */
    if(detail){
        static const char hex[]="0123456789ABCDEF";
        size_t n=0;
        for(const unsigned char *v=(const unsigned char *)detail;*v && n+4<sizeof(encoded);v++){
            unsigned char c=*v;
            if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')
                encoded[n++]=(char)c;
            else {encoded[n++]='%';encoded[n++]=hex[c>>4];encoded[n++]=hex[c&15];}
        }
        encoded[n]=0;
    }
    mon_record_lock();
    g_record_result.completed_id=request_id;
    g_record_result.result_code=result;
    snprintf(g_record_result.detail,sizeof(g_record_result.detail),"%s",encoded);
    mon_record_unlock();
}
/* Phase 2I: fully M-only remote recording config and device catalogue.
 * Network workers never read/write app->settings or app->devices directly.
 * Main-thread snapshots are protected by a very short atomic lock. */
static atomic_flag g_cfg_lock=ATOMIC_FLAG_INIT;
static void cfg_lock(void){while(atomic_flag_test_and_set_explicit(&g_cfg_lock,memory_order_acquire))thrd_sleep_ms(1);}
static void cfg_unlock(void){atomic_flag_clear_explicit(&g_cfg_lock,memory_order_release);}
static struct {
    char settings[MON_SETTINGS_WIRE_CAP];
    char devices[8192];
    char pending_key[MON_SETTINGS_KEY_CAP],pending_value[MON_SETTINGS_VALUE_CAP];
    bool pending;
} g_cfg_server;
static struct {
    char wire[MON_SETTINGS_WIRE_CAP];
    bool received,initialized;
    char last[96][MON_SETTINGS_VALUE_CAP];
    bool send_pending,send_sent;
    char send_key[MON_SETTINGS_KEY_CAP],send_value[MON_SETTINGS_VALUE_CAP];
    device_info_t devices[MAX_DEVICES];
    int count,selected;
    /* Restore the client's local device catalogue on leaving M-Client;
     * never allow a remote-only index into the legacy Local path. */
    device_info_t local_devices[MAX_DEVICES];
    int local_count,local_selected;
    bool device_ready,device_seen;
} g_cfg_client;
static void mon_cfg_disconnect(void){
    cfg_lock();g_cfg_client.received=false;g_cfg_client.initialized=false;
    g_cfg_client.send_pending=false;g_cfg_client.send_sent=false;g_cfg_client.device_ready=false;
    g_cfg_client.device_seen=false;cfg_unlock();
}
/* Settings are returned as percent-escaped key=value lines (no arbitrary JSON
 * deserialization or raw structs across different compilers/platforms). */
static bool mon_cfg_pack(const gui_settings_t *s,char *dst,size_t cap);
static void mon_cfg_snapshot(gui_app_t *app);

/* The browser result/request mailboxes are briefly guarded; never hold the
 * spinlock during filesystem operations, networking, or rendering. */
static atomic_flag g_fs_lock=ATOMIC_FLAG_INIT;
static void fs_lock(void){while(atomic_flag_test_and_set_explicit(&g_fs_lock,memory_order_acquire))thrd_sleep_ms(1);}
static void fs_unlock(void){atomic_flag_clear_explicit(&g_fs_lock,memory_order_release);}

/* Staged M-Client local-profile/device restore: gui_monitor_client_stop may
 * run on the net transition worker, but the app->settings / app->devices
 * restores belong to the MAIN thread. Stop stages the restore; the main-thread
 * poller (and gui_net_stop at exit) consumes it. A new client session clears
 * it (reconnecting must not restore the local catalogue mid-session). */
static atomic_bool g_monitor_restore_pending=ATOMIC_VAR_INIT(false);
static struct {
    char requested[MON_REMOTE_PATH];unsigned page,request_seq,seen_seq;
    char selection[MON_REMOTE_PATH];bool select_pending;
    gui_remote_fs_result_t result;
    bool ready,failed;
    char server_output[MON_REMOTE_PATH];bool output_valid;
} g_fs_client;
static struct {
    char pending[MON_REMOTE_PATH];bool pending_valid;
    char current[MON_REMOTE_PATH];
} g_fs_server;
static bool mon_url_encode(const char *in,char *out,size_t cap){
    static const char hex[]="0123456789ABCDEF";
    size_t n=0;
    if(!in)return false;
    for(const unsigned char *p=(const unsigned char*)in;*p;p++){
        if(n+4>=cap)return false;
        /* Always escape reserved syntax and non-ASCII bytes. */
        if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||
           (*p>='0'&&*p<='9')||*p=='-'||*p=='_'||*p=='.')out[n++]=(char)*p;
        else{out[n++]='%';out[n++]=hex[*p>>4];out[n++]=hex[*p&15];}
    }
    out[n]=0;return true;
}
static int mon_hex(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static bool mon_url_decode(const char *in,size_t len,char *out,size_t cap){
    size_t n=0;
    for(size_t i=0;i<len;i++){
        unsigned char ch=(unsigned char)in[i];
        if(ch=='%'){
            if(i+2>=len)return false;
            int a=mon_hex(in[++i]),b=mon_hex(in[++i]);
            if(a<0||b<0)return false;
            ch=(unsigned char)((a<<4)|b);
        }
        if(!ch||ch<32||ch==127||n+1>=cap)return false;
        out[n++]=(char)ch;
    }
    out[n]=0;return true;
}
static bool mon_query_path(const char *query,char *out,size_t cap){
    if(!query)return false;
    const char *p=query;
    while(*p){
        const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        if(end-p>=3 && !strncmp(p,"at=",3))return mon_url_decode(p+3,(size_t)(end-p-3),out,cap);
        p=*end?end+1:end;
    }
    return false;
}
static bool mon_query_named(const char *query,const char *key,char *out,size_t cap){
    if(!query||!key)return false;
    size_t klen=strlen(key);const char *p=query;
    while(*p){const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        if((size_t)(end-p)>klen&&strncmp(p,key,klen)==0&&p[klen]=='=')
            return mon_url_decode(p+klen+1,(size_t)(end-p-klen-1),out,cap);
        p=*end?end+1:end;
    }
    return false;
}
/* Reuse the original remote browser's on-disk path semantics; only encode
 * server directory names and paths, never file contents. */
static bool mon_write_browse(char *body,size_t cap,const gui_remote_fs_result_t *r){
    char enc[MON_REMOTE_PATH*3+1];size_t n=0;
#define APPEND(...) do {int m=snprintf(body+n,cap-n,__VA_ARGS__);if(m<0||(size_t)m>=cap-n)return false;n+=(size_t)m;} while(0)
    if(!mon_url_encode(r->path,enc,sizeof(enc)))return false;
    APPEND("path=%s\n",enc);
    if(!mon_url_encode(r->parent,enc,sizeof(enc)))return false;
    APPEND("parent=%s\n",enc);
    APPEND("computer=%d\nrecovered=%d\nmore=%d\ncount=%u\n",r->computer,r->recovered,r->has_more,r->count);
    for(unsigned i=0;i<r->count;i++){
        if(!mon_url_encode(r->names[i],enc,sizeof(enc)))return false;
        APPEND("item=%s\n",enc);
    }
#undef APPEND
    return true;
}
static bool mon_parse_browse(char *body,gui_remote_fs_result_t *r){
    memset(r,0,sizeof(*r));bool has_path=false,has_parent=false,has_count=false;
    for(char *line=body;line&&*line;){
        char *end=strchr(line,'\n');if(end)*end=0;
        char *eq=strchr(line,'=');if(!eq)return false;
        *eq++=0;
        if(!strcmp(line,"path")){has_path=mon_url_decode(eq,strlen(eq),r->path,sizeof(r->path));if(!has_path)return false;}
        else if(!strcmp(line,"parent")){has_parent=mon_url_decode(eq,strlen(eq),r->parent,sizeof(r->parent));if(!has_parent)return false;}
        else if(!strcmp(line,"computer"))r->computer=atoi(eq)!=0;
        else if(!strcmp(line,"recovered"))r->recovered=atoi(eq)!=0;
        else if(!strcmp(line,"more"))r->has_more=atoi(eq)!=0;
        else if(!strcmp(line,"count")){r->count=(unsigned)atoi(eq);if(r->count>GUI_REMOTE_FS_PAGE)return false;has_count=true;}
        else if(!strcmp(line,"item")){
            unsigned i=r->page++;if(i>=GUI_REMOTE_FS_PAGE||
               !mon_url_decode(eq,strlen(eq),r->names[i],sizeof(r->names[i])))return false;
        }
        line=end?end+1:NULL;
    }
    if(!has_path||!has_parent||!has_count||r->page!=r->count)return false;
    r->page=0;return true;
}

static bool mon_cfg_pack(const gui_settings_t *s,char *dst,size_t cap){
    size_t used=0;dst[0]=0;
    for(size_t i=0;i<gui_monitor_settings_count();i++){
        char val[MON_SETTINGS_VALUE_CAP],encoded[MON_SETTINGS_VALUE_CAP*3+1];
        if(!gui_monitor_settings_get(s,i,val,sizeof(val))||!mon_url_encode(val,encoded,sizeof(encoded)))return false;
        int n=snprintf(dst+used,cap-used,"%s=%s\n",gui_monitor_settings_key(i),encoded);
        if(n<0||(size_t)n>=cap-used)return false;
        used+=(size_t)n;
    }
    return true;
}
static void mon_cfg_snapshot(gui_app_t *app){
    char settings[MON_SETTINGS_WIRE_CAP]={0},devices[8192]={0};
    if(!mon_cfg_pack(&app->settings,settings,sizeof(settings)))return;
    size_t used=0;
    int n=snprintf(devices,sizeof(devices),"selected=%d\ncount=%d\n",app->selected_device,app->device_count);
    if(n<0||(size_t)n>=sizeof(devices))return;
    used=(size_t)n;
    for(int i=0;i<app->device_count&&i<MAX_DEVICES;i++){
        char name[64*3+1],serial[64*3+1];
        if(!mon_url_encode(app->devices[i].name,name,sizeof(name))||
           !mon_url_encode(app->devices[i].serial,serial,sizeof(serial)))return;
        n=snprintf(devices+used,sizeof(devices)-used,"device=%d,%d,%s,%s\n",
                   (int)app->devices[i].type,app->devices[i].index,name,serial);
        if(n<0||(size_t)n>=sizeof(devices)-used)return;
        used+=(size_t)n;
    }
    cfg_lock();
    memcpy(g_cfg_server.settings,settings,strlen(settings)+1);
    memcpy(g_cfg_server.devices,devices,strlen(devices)+1);
    cfg_unlock();
}
/* Main-thread mirror; preserving every non-recording client setting. */
void gui_monitor_client_sync_main(gui_app_t *app){
    if(!app||app->settings.net_mode!=4)return;
    /* Popups are main-thread UI objects. Never open one from the network worker.
     * Hold the mailbox if another popup (e.g. overwrite confirmation) is open. */
    if(!gui_popup_is_open()){
        char message[512]={0};
        while(atomic_flag_test_and_set_explicit(&g_record_ui_lock,memory_order_acquire))
            thrd_sleep_ms(1);
        if(g_record_ui_pending){
            snprintf(message,sizeof(message),"%s",g_record_ui_message);
            g_record_ui_pending=false;
        }
        atomic_flag_clear_explicit(&g_record_ui_lock,memory_order_release);
        if(message[0]){
            gui_app_set_status(app,"Recording failed. Check settings!");
            gui_popup_info("Remote recording failed",message);
        }
    }
    char received[MON_SETTINGS_WIRE_CAP]={0};bool first=false,apply=false;
    cfg_lock();
    if(g_cfg_client.received && !g_cfg_client.send_pending){
        snprintf(received,sizeof(received),"%s",g_cfg_client.wire);
        first=!g_cfg_client.initialized;
        apply=true;
        g_cfg_client.received=false;
        g_cfg_client.initialized=true;
    }
    if(g_cfg_client.device_ready){
        int n=g_cfg_client.count;
        if(n>=0&&n<=MAX_DEVICES){
            memcpy(app->devices,g_cfg_client.devices,(size_t)n*sizeof(device_info_t));
            app->device_count=n;
            app->selected_device=g_cfg_client.selected>=0&&g_cfg_client.selected<n?g_cfg_client.selected:-1;
        }
        g_cfg_client.device_ready=false;
    }
    cfg_unlock();
    if(apply){
        for(char *line=received;*line;){
            char *end=strchr(line,'\n');if(end)*end=0;
            char *eq=strchr(line,'=');
            if(eq){*eq++=0;char decoded[MON_SETTINGS_VALUE_CAP];
                if(mon_url_decode(eq,strlen(eq),decoded,sizeof(decoded))){
                    for(size_t i=0;i<gui_monitor_settings_count();i++)if(!strcmp(line,gui_monitor_settings_key(i))){
                        /* First sync adopts server state. Later server edits are
                         * mirrored only if client has no unsent local edit. */
                        char local[MON_SETTINGS_VALUE_CAP]={0};
                        bool has_local=gui_monitor_settings_get(&app->settings,i,local,sizeof(local));
                        if(first||!has_local||!strcmp(local,g_cfg_client.last[i]))
                            (void)gui_monitor_settings_set(&app->settings,line,decoded);
                        snprintf(g_cfg_client.last[i],sizeof(g_cfg_client.last[i]),"%s",decoded);
                        break;
                    }
                }
            }
            line=end?end+1:line+strlen(line);
        }
    }
    if(!g_cfg_client.initialized||!gui_monitor_client_connected())return;
    cfg_lock();bool busy=g_cfg_client.send_pending;cfg_unlock();
    if(busy)return;
    /* One update per tick; queued in a single slot, never overwritten.
     * A failed network command is retried by the worker, not lost. */
    for(size_t i=0;i<gui_monitor_settings_count();i++){
        char value[MON_SETTINGS_VALUE_CAP];
        if(!gui_monitor_settings_get(&app->settings,i,value,sizeof(value)))continue;
        if(strcmp(value,g_cfg_client.last[i])==0)continue;
        cfg_lock();
        if(!g_cfg_client.send_pending){
            snprintf(g_cfg_client.send_key,sizeof(g_cfg_client.send_key),"%s",gui_monitor_settings_key(i));
            snprintf(g_cfg_client.send_value,sizeof(g_cfg_client.send_value),"%s",value);
            g_cfg_client.send_pending=true;
            g_cfg_client.send_sent=false;
            snprintf(g_cfg_client.last[i],sizeof(g_cfg_client.last[i]),"%s",value);
        }
        cfg_unlock();break;
    }
}
static int mon_get(const char *host,uint16_t port,const char *path,char *dst,size_t cap);
static void mon_cfg_worker(void){
    char k[MON_SETTINGS_KEY_CAP]={0},v[MON_SETTINGS_VALUE_CAP]={0};bool send=false;
    cfg_lock();
    if(g_cfg_client.send_pending){snprintf(k,sizeof(k),"%s",g_cfg_client.send_key);
        snprintf(v,sizeof(v),"%s",g_cfg_client.send_value);send=!g_cfg_client.send_sent;}
    cfg_unlock();
    if(send){
        char ke[MON_SETTINGS_KEY_CAP*3+1],ve[MON_SETTINGS_VALUE_CAP*3+1];
        char url[MON_SETTINGS_VALUE_CAP*3+MON_SETTINGS_KEY_CAP*3+96],answer[256];
        if(mon_url_encode(k,ke,sizeof(ke))&&mon_url_encode(v,ve,sizeof(ve))){
            snprintf(url,sizeof(url),"/monitor/settings/set?key=%s&value=%s",ke,ve);
            if(mon_get(g_client.host,g_client.port,url,answer,sizeof(answer))==0){
                cfg_lock();
                if(g_cfg_client.send_pending&&!strcmp(g_cfg_client.send_key,k)&&!strcmp(g_cfg_client.send_value,v))
                    g_cfg_client.send_sent=true;
                cfg_unlock();
            }
        }
    }
    static unsigned config_poll=0;
    if((config_poll++%8)==0){
        char *response=malloc(MON_SETTINGS_WIRE_CAP);
        if(response && mon_get(g_client.host,g_client.port,"/monitor/settings",response,MON_SETTINGS_WIRE_CAP)==0){
            cfg_lock();
            if(g_cfg_client.send_pending&&g_cfg_client.send_sent){
                /* The remote value must echo after server main-thread apply.
                 * Do not revert the local UI to a stale pre-ACK snapshot. */
                char encoded[MON_SETTINGS_VALUE_CAP*3+1],match[MON_SETTINGS_VALUE_CAP*3+MON_SETTINGS_KEY_CAP+8];
                if(mon_url_encode(g_cfg_client.send_value,encoded,sizeof(encoded))){
                    snprintf(match,sizeof(match),"%s=%s\n",g_cfg_client.send_key,encoded);
                    if(strstr(response,match))g_cfg_client.send_pending=false;
                }
            }
            if(!g_cfg_client.send_pending){snprintf(g_cfg_client.wire,sizeof(g_cfg_client.wire),"%s",response);
                g_cfg_client.received=true;}
            cfg_unlock();
        }
        free(response);
    }
    static unsigned device_poll=0;
    if((device_poll++%10)==0){
        char response[8192];
        if(mon_get(g_client.host,g_client.port,"/monitor/devices",response,sizeof(response))==0){
            device_info_t dev[MAX_DEVICES]={0};int count=-1,selected=-1,seen=0;
            for(char *line=response;line&&*line;){
                char *end=strchr(line,'\n');if(end)*end=0;
                if(!strncmp(line,"selected=",9))selected=atoi(line+9);
                else if(!strncmp(line,"count=",6))count=atoi(line+6);
                else if(!strncmp(line,"device=",7)&&seen<MAX_DEVICES){
                    char *p=line+7,*comma=strchr(p,',');if(!comma)goto nextdev;*comma++=0;
                    int type=atoi(p);p=comma;comma=strchr(p,',');if(!comma)goto nextdev;*comma++=0;
                    int index=atoi(p);p=comma;comma=strchr(p,',');if(!comma)goto nextdev;*comma++=0;
                    /* A device index of -1 is valid for the synthetic Test Signal,
                     * Playback and some OS-backed capture entries. Rejecting
                     * it would invalidate the entire remote catalogue and
                     * leave the M-Client showing its own local device. */
                    if(type<0||type>100||index<-1||index>MAX_DEVICES)goto nextdev;
                    if(!mon_url_decode(p,strlen(p),dev[seen].name,sizeof(dev[seen].name))||
                       !mon_url_decode(comma,strlen(comma),dev[seen].serial,sizeof(dev[seen].serial)))goto nextdev;
                    dev[seen].type=(device_type_t)type;dev[seen].index=index;seen++;
                }
                nextdev:line=end?end+1:NULL;
            }
            if(count==seen&&count>=0&&count<=MAX_DEVICES){
                cfg_lock();memcpy(g_cfg_client.devices,dev,(size_t)seen*sizeof(device_info_t));
                g_cfg_client.count=seen;g_cfg_client.selected=selected;g_cfg_client.device_ready=true;cfg_unlock();
            }
        }
    }
}

/* Parse only numeric fields emitted by our own versioned status endpoint. */
static bool mon_json_u64(const char *body,const char *name,uint64_t *result) {
    char key[64];
    snprintf(key,sizeof(key),"\"%s\":",name);
    const char *p=strstr(body,key);
    if (!p) return false;
    p+=strlen(key);
    if (*p<'0'||*p>'9')return false;
    char *end=NULL;
    unsigned long long v=strtoull(p,&end,10);
    if (end==p || (*end!=',' && *end!='}' && *end!='\n'))return false;
    *result=(uint64_t)v;
    return true;
}
static unsigned mon_record_buffer_pct(gui_app_t *app,buffer_id_t id) {
    size_t cap=app->buffers.buffers[id].buffer_size;
    if (!cap || !app->is_recording) return 0;
    size_t fill=bufmgr_fill_level(&app->buffers,id);
    uint64_t pct=(uint64_t)(fill / 1024u)*100u/(cap/1024u ? cap/1024u:1u);
    return pct>100?100:(unsigned)pct;
}

static unsigned mon_capture_buffer_pct(gui_app_t *app, buffer_id_t id) {
    if (!app->is_capturing) return 0;
    size_t cap=app->buffers.buffers[id].buffer_size;
    if (!cap) return 0;
    size_t fill=bufmgr_fill_level(&app->buffers,id);
    uint64_t pct=(uint64_t)(fill/1024u)*100u/(cap/1024u?cap/1024u:1u);
    return pct>100?100:(unsigned)pct;
}

static void mon_timeouts(mon_sock_t s, int ms) {
#ifdef _WIN32
    DWORD t=(DWORD)ms;
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&t,sizeof(t));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&t,sizeof(t));
#else
    struct timeval tv={ ms/1000, (ms%1000)*1000 };
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
#endif
}
static int mon_send(mon_sock_t s, const char *data, size_t n) {
    while(n) {
#ifdef _WIN32
        int k=send(s,data,(int)n,0);
#else
        ssize_t k=send(s,data,n,0);
#endif
        if(k<=0)return -1;
        data+=(size_t)k;n-=(size_t)k;
    }
    return 0;
}
static void mon_answer(mon_sock_t s, int code, const char *type, const char *body) {
    char hdr[320];
    size_t n=strlen(body);
    int hn=snprintf(hdr,sizeof(hdr),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        code,code==200?"OK":code==404?"Not Found":"Bad Request",type,n);
    if(hn>0 && (size_t)hn<sizeof(hdr)) {
        (void)mon_send(s,hdr,(size_t)hn);
        (void)mon_send(s,body,n);
    }
}
static void mon_answer_preview(mon_sock_t fd,const char *query){
    gui_rd_config_t cfg[2];
    for(int c=0;c<2;c++){
        cfg[c]=(gui_rd_config_t){32.f,GUI_RD_WIDTH,0,0,0,c};
        cfg[c].zoom=(float)mon_arg(query,c?"zb":"za",32000)/1000.0f;
        cfg[c].width=(uint32_t)mon_arg(query,c?"wb":"wa",GUI_RD_WIDTH);
        cfg[c].trigger_enabled=mon_arg(query,c?"tb":"ta",0);
        cfg[c].trigger_level=mon_arg(query,c?"lb":"la",0);
        cfg[c].trigger_mode=mon_arg(query,c?"mb":"ma",0);
        cfg[c].trigger_source=mon_arg(query,c?"sb":"sa",c);
    }
    void *wire=NULL;size_t bytes=0,raw=0;
    if(!gui_rd_server_build(cfg,&wire,&bytes,&raw)){
        mon_answer(fd,404,"text/plain","no current preview\n");return;
    }
    char hdr[320];
    int hn=snprintf(hdr,sizeof(hdr),
        "HTTP/1.0 200 OK\r\nContent-Type: application/zstd\r\n"
        "Cache-Control: no-store\r\nContent-Length: %zu\r\n"
        "X-Remote-Raw-Bytes: %zu\r\nConnection: close\r\n\r\n",bytes,raw);
    if(hn>0&&(size_t)hn<sizeof(hdr)&&mon_send(fd,hdr,(size_t)hn)==0)
        (void)mon_send(fd,(const char*)wire,bytes);
    unsigned n=atomic_fetch_add(&g_preview_transfers,1)+1;
    atomic_fetch_add(&g_preview_wire_bytes,bytes);
    if(n%100==0) {
        uint64_t total=atomic_exchange(&g_preview_wire_bytes,0);
        fprintf(stderr,"[M-SERVER] last 100 frames: compressed %.1f KiB/frame, ratio %.1f%% (Zstd-1)\n",
               (double)total/100.0/1024.0,100.0*(double)total/(100.0*(double)raw));
    }
    free(wire);
}
/* Called by the ONE HTTP listener in gui_net.c after it has read the full
 * request headers. Return true only when Local Save takes socket ownership. */
bool gui_monitor_server_handle_request(uintptr_t socket_handle,
                                       gui_app_t *app, const char *request_line) {
    mon_sock_t fd = (mon_sock_t)socket_handle;
    if (!gui_monitor_server_active() || !app || !request_line) return false;
    const char *req = request_line;
    char path[MON_REMOTE_PATH*3+300];
    if(sscanf(req,"GET %12587s",path)!=1) {
        mon_answer(fd,400,"text/plain","invalid request\n");return false;
    }
    /* All control endpoints are on a dedicated, versioned /monitor path. */
    char *query=strchr(path,'?');
    if(query){*query++='\0';}
    /* Persistent receiver is client-initiated. Its socket is owned by
     * the M-local transfer worker, never by the accept loop. */
    if(strcmp(path,"/monitor/local-save/attach")==0){
        if(gui_monitor_local_save_server_attach((uintptr_t)fd)==0)return true;
        mon_answer(fd,409,"text/plain","receiver already active\n");return false;
    }
    if(strcmp(path,"/monitor/fs/browse")==0){
        char requested[MON_REMOTE_PATH];
        int page=mon_arg(query,"page",0);
        if(page<0||page>100000||!mon_query_path(query,requested,sizeof(requested))){
            mon_answer(fd,400,"text/plain","invalid server path\n");return false;
        }
        gui_remote_fs_result_t *r=malloc(sizeof(*r));
        char *body=malloc(MON_BROWSER_WIRE);
        if(!r||!body){free(r);free(body);mon_answer(fd,400,"text/plain","out of memory\n");return false;}
        bool ok=gui_remote_fs_browse(requested,(unsigned)page,r)&&
                mon_write_browse(body,MON_BROWSER_WIRE,r);
        mon_answer(fd,ok?200:400,"text/plain",ok?body:"server folder listing failed\n");
        free(r);free(body);return false;
    }
    if(strcmp(path,"/monitor/fs/output")==0){
        char current[MON_REMOTE_PATH];
        fs_lock();snprintf(current,sizeof(current),"%s",g_fs_server.current);fs_unlock();
        char *body=malloc(MON_REMOTE_PATH*3+32);
        if(!body){mon_answer(fd,400,"text/plain","out of memory\n");return false;}
        bool ok=mon_url_encode(current,body,MON_REMOTE_PATH*3+32);
        mon_answer(fd,ok?200:400,"text/plain",ok?body:"invalid output\n");
        free(body);return false;
    }
    if(strcmp(path,"/monitor/fs/select")==0){
        char requested[MON_REMOTE_PATH];
        if(!mon_query_path(query,requested,sizeof(requested))||!*requested){
            mon_answer(fd,400,"text/plain","invalid selection\n");return false;
        }
        gui_remote_fs_result_t *r=malloc(sizeof(*r));
        if(!r){mon_answer(fd,400,"text/plain","out of memory\n");return false;}
        bool ok=gui_remote_fs_browse(requested,0,r)&&!r->recovered&&!r->computer&&
                !strcmp(r->path,requested);
        free(r);
        if(!ok){mon_answer(fd,400,"text/plain","folder does not exist\n");return false;}
        fs_lock();snprintf(g_fs_server.pending,sizeof(g_fs_server.pending),"%s",requested);
        g_fs_server.pending_valid=true;fs_unlock();
        mon_answer(fd,200,"text/plain","server selection queued\n");return false;
    }
    if(strcmp(path,"/monitor/settings")==0 || strcmp(path,"/monitor/devices")==0){
        char *body=malloc(MON_SETTINGS_WIRE_CAP);
        if(!body){mon_answer(fd,400,"text/plain","out of memory\n");return false;}
        cfg_lock();snprintf(body,MON_SETTINGS_WIRE_CAP,"%s",
            !strcmp(path,"/monitor/settings")?g_cfg_server.settings:g_cfg_server.devices);cfg_unlock();
        mon_answer(fd,200,"text/plain",body);free(body);return false;
    }
    if(strcmp(path,"/monitor/settings/set")==0){
        char key[MON_SETTINGS_KEY_CAP],value[MON_SETTINGS_VALUE_CAP];
        /* Strictly whitelist names and typed values before even staging. */
        gui_settings_t probe={0};
        bool valid=query&&mon_query_named(query,"key",key,sizeof(key))&&
                   mon_query_named(query,"value",value,sizeof(value))&&
                   gui_monitor_settings_set(&probe,key,value);
        if(!valid||app->is_recording){mon_answer(fd,409,"text/plain","setting invalid or recording active\n");return false;}
        cfg_lock();bool busy=g_cfg_server.pending;
        if(!busy){snprintf(g_cfg_server.pending_key,sizeof(g_cfg_server.pending_key),"%s",key);
            snprintf(g_cfg_server.pending_value,sizeof(g_cfg_server.pending_value),"%s",value);
            g_cfg_server.pending=true;}
        cfg_unlock();
        mon_answer(fd,busy?409:200,"text/plain",busy?"another setting pending\n":"setting queued\n");return false;
    }
    if(strcmp(path,"/monitor/device")==0){
        int idx=mon_arg(query,"index",-1);
        if(idx<0||idx>=app->device_count||app->is_recording){
            mon_answer(fd,409,"text/plain","invalid device or recording active\n");return false;
        }
        atomic_store(&app->net_cmd_monitor_device_index,idx);
        atomic_store(&app->net_cmd_monitor_select_device,true);
        mon_answer(fd,200,"text/plain","device queued\n");return false;
    }
    if(strcmp(path,"/monitor/reset-clips")==0){
        int channel=mon_arg(query,"channel",-1);
        if(channel!=0 && channel!=1){
            mon_answer(fd,400,"text/plain","invalid channel\n");return false;
        }
        /* Atomic counters are owned by the capture/extraction pipeline. The
         * socket worker must not alter GUI-only VU hold state. */
        if(channel==0){
            atomic_store(&app->clip_count_a_pos,0);
            atomic_store(&app->clip_count_a_neg,0);
        }else{
            atomic_store(&app->clip_count_b_pos,0);
            atomic_store(&app->clip_count_b_neg,0);
        }
        mon_answer(fd,200,"text/plain","clip counters cleared\n");return false;
    }
    if(strcmp(path,"/monitor/preview")==0){mon_answer_preview(fd,query);return false;}
    if(strcmp(path,"/monitor/hello")==0 || strcmp(path,"/monitor/status")==0) {
        char body[1800];
        unsigned state=app->is_recording?2u:app->is_capturing?1u:0u;
        uint64_t elapsed_ms=0;
        if (app->recording_start_time>0) {
            double now=(double)get_time_us()/1000000.0;
            double elapsed=app->is_recording ? now-app->recording_start_time : app->last_recording_duration_s;
            if (elapsed>0)elapsed_ms=(uint64_t)(elapsed*1000.0);
        }
        size_t queued=0,capacity=0;
        if (gui_monitor_local_save_recording())
            gui_monitor_local_save_queue_stats(&queued,&capacity);
        unsigned ra=mon_record_buffer_pct(app,BUF_RECORD_A);
        unsigned rb=mon_record_buffer_pct(app,BUF_RECORD_B);
        unsigned rf=mon_capture_buffer_pct(app,BUF_CAPTURE_RF);
        unsigned aud=mon_capture_buffer_pct(app,BUF_CAPTURE_AUDIO);
        /* Avoid statvfs / Windows drive I/O on every ~100ms status poll.
         * The server socket worker is single-threaded; cache for 2 seconds. */
        static uint64_t free_checked_us=0,free_cached_bytes=0;
        static bool free_cached_valid=false;
        uint64_t now_us=get_time_us();
        if(!free_checked_us || now_us<free_checked_us || now_us-free_checked_us>=2000000ULL){
            free_checked_us=now_us;
            free_cached_valid=gui_record_get_output_free_space_bytes(app,&free_cached_bytes);
        }

        uint64_t record_result_id=0;
        unsigned record_result_code=0;
        char record_result_detail[256];
        mon_record_lock();
        record_result_id=g_record_result.completed_id;
        record_result_code=g_record_result.result_code;
        snprintf(record_result_detail,sizeof(record_result_detail),"%s",g_record_result.detail);
        mon_record_unlock();
        snprintf(body,sizeof(body),
            "{\"protocol\":\"MISRC-M/2\",\"state\":%u,\"sample_rate\":%u,\"frames\":%u,\"errors\":%u,\"missed\":%u,"
            "\"record_elapsed_ms\":%" PRIu64 ",\"raw_a\":%" PRIu64 ",\"raw_b\":%" PRIu64 ","
            "\"compressed_a\":%" PRIu64 ",\"compressed_b\":%" PRIu64 ",\"samples_a\":%" PRIu64 ","
            "\"rec_a_pct\":%u,\"rec_b_pct\":%u,\"rf_pct\":%u,\"aud_pct\":%u,\"rec_a_waits\":%u,\"rec_b_waits\":%u,"
            "\"rec_a_drops\":%u,\"rec_b_drops\":%u,"
            "\"local_queue_bytes\":%" PRIu64 ",\"local_queue_capacity\":%" PRIu64 ","
            "\"capture_a\":%u,\"capture_b\":%u,\"use_flac\":%u,\"local_save\":%u,"
            "\"stream_synced\":%u,\"server_free_valid\":%u,\"server_free_bytes\":%" PRIu64 ","
            "\"clip_a_pos\":%u,\"clip_a_neg\":%u,\"clip_b_pos\":%u,\"clip_b_neg\":%u,\"record_result_id\":%" PRIu64 ",\"record_result_code\":%u,\"record_result_detail\":\"%s\"}\n",
            state,(unsigned)atomic_load(&app->sample_rate),
            (unsigned)atomic_load(&app->frame_count),
            (unsigned)atomic_load(&app->error_count),
            (unsigned)atomic_load(&app->missed_frame_count),
            elapsed_ms,(uint64_t)atomic_load(&app->recording_raw_a),(uint64_t)atomic_load(&app->recording_raw_b),
            (uint64_t)atomic_load(&app->recording_compressed_a),(uint64_t)atomic_load(&app->recording_compressed_b),
            (uint64_t)atomic_load(&app->samples_a),ra,rb,rf,aud,
            (unsigned)atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits),
            (unsigned)atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits),
            (unsigned)atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops),
            (unsigned)atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops),
            (uint64_t)queued,(uint64_t)capacity,
            app->settings.capture_a?1u:0u,app->settings.capture_b?1u:0u,
            app->settings.use_flac?1u:0u,gui_monitor_local_save_recording()?1u:0u,
            atomic_load(&app->stream_synced)?1u:0u,free_cached_valid?1u:0u,free_cached_bytes,
            (unsigned)atomic_load(&app->clip_count_a_pos),(unsigned)atomic_load(&app->clip_count_a_neg),
            (unsigned)atomic_load(&app->clip_count_b_pos),(unsigned)atomic_load(&app->clip_count_b_neg),
            record_result_id,record_result_code,record_result_detail);
        mon_answer(fd,200,"application/json",body);return false;
    }
    if(strcmp(path,"/monitor/start")==0) {
        atomic_store(&app->net_cmd_start,true);
        mon_answer(fd,200,"text/plain","start queued\n");return false;
    }
    if(strcmp(path,"/monitor/stop")==0) {
        atomic_store(&app->net_cmd_stop,true);
        mon_answer(fd,200,"text/plain","stop queued\n");return false;
    }
    if(strcmp(path,"/monitor/record")==0 && query &&
       (!strcmp(query,"on=1")||!strcmp(query,"on=0")||!strcmp(query,"on=1&local=1"))) {
        bool on=query[3]=='1';
        if(on && !strcmp(query,"on=1&local=1") && !gui_monitor_local_save_server_ready()) {
            mon_answer(fd,409,"text/plain","no Monitor & Remote local receiver\n");return false;
        }
        uint64_t id=gui_monitor_server_queue_record(app,
                    on ? (!strcmp(query,"on=1&local=1") ? 2 : 1) : 3);
        char body[80];
        snprintf(body,sizeof(body),"record queued %" PRIu64 "\n",id);
        mon_answer(fd,200,"text/plain",body);return false;
    }
    mon_answer(fd,404,"text/plain","monitor endpoint not found\n");
    return false;
}
/* Shared-server initialization only. The legacy Server owns the TCP
 * listener and broadcasts both discovery protocol identifiers on UDP 8091. */
int gui_monitor_server_start(gui_app_t *app,uint16_t port) {
    if(!app||!port)return -1;
    if(atomic_load(&g_server.active))
        return g_server.app==app&&g_server.port==port?0:-1;
    mon_record_lock();
    g_record_result.pending_id=0;
    g_record_result.completed_id=0;
    g_record_result.result_code=0;
    g_record_result.detail[0]=0;
    mon_record_unlock();
    gui_monitor_local_save_server_init();
    memset(&g_cfg_server,0,sizeof(g_cfg_server));
    mon_cfg_snapshot(app);
    g_server.app=app;g_server.port=port;
    fs_lock();g_fs_server.pending_valid=false;
    snprintf(g_fs_server.current,sizeof(g_fs_server.current),"%s",app->settings.output_path);
    fs_unlock();
    atomic_store(&g_server.active,true);
    fprintf(stderr,"[M-SERVER] Monitor MISRC-M/2 endpoints enabled on shared TCP %u (Zstd-1 preview + control)\n",(unsigned)port);
    return 0;
}
void gui_monitor_server_stop(void){
    if(!atomic_load(&g_server.active))return;
    atomic_store(&g_server.active,false);
    gui_monitor_local_save_server_shutdown();
    g_server.app=NULL;g_server.port=0;
}
bool gui_monitor_server_active(void){return atomic_load(&g_server.active);}
uint16_t gui_monitor_server_port(void){return g_server.port;}

/* Small, bounded, one-request HTTP client: accepts ONLY the MISRC-M/2 marker.
 * Never opens a raw RF/audio endpoint. Client-side commands remain queued
 * on gui_app_t until a verified M-Server answers its status endpoint. */
static int mon_get(const char *host,uint16_t port,const char *path,char *dst,size_t cap){
    if(!dst||cap<2||!host||!*host)return -1;
    dst[0]=0;
    char service[8];snprintf(service,sizeof(service),"%u",(unsigned)port);
    struct addrinfo hints={0},*addr=NULL;
    hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,service,&hints,&addr)!=0)return -1;
    mon_sock_t sock=MON_BAD;
    for(struct addrinfo *p=addr;p;p=p->ai_next){
        sock=socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(sock==MON_BAD)continue;
        mon_timeouts(sock,MON_HTTP_TIMEOUT_MS);
        if(connect(sock,p->ai_addr,(int)p->ai_addrlen)==0)break;
        mon_close(sock);sock=MON_BAD;
    }
    freeaddrinfo(addr);
    if(sock==MON_BAD)return -1;
    char get[MON_REMOTE_PATH*3+300];
    int gn=snprintf(get,sizeof(get),"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",path,host);
    if(gn<=0||(size_t)gn>=sizeof(get)||mon_send(sock,get,(size_t)gn)!=0){mon_close(sock);return -1;}
    size_t used=0;
    for(;;){
#ifdef _WIN32
        int n=recv(sock,dst+used,(int)(cap-used-1),0);
#else
        ssize_t n=recv(sock,dst+used,cap-used-1,0);
#endif
        if(n<=0)break;
        used+=(size_t)n;
        if(used>=cap-1)break;
    }
    mon_close(sock);dst[used]=0;
    if(strncmp(dst,"HTTP/1.0 200 ",13)!=0 && strncmp(dst,"HTTP/1.1 200 ",13)!=0)return -1;
    char *body=strstr(dst,"\r\n\r\n");
    if(!body)return -1;
    body+=4;
    memmove(dst,body,strlen(body)+1);
    return 0;
}
/* Strict bounded binary transfer, never treated as a C-string.
 * Read Content-Length and reject partial/oversized/stale frames. */
static int mon_get_preview(const char *host,uint16_t port){
    char service[8];snprintf(service,sizeof(service),"%u",(unsigned)port);
    struct addrinfo hints={0},*addr=NULL;
    hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,service,&hints,&addr)!=0)return -1;
    mon_sock_t sock=MON_BAD;
    for(struct addrinfo *p=addr;p;p=p->ai_next){
        sock=socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(sock==MON_BAD)continue;
        mon_timeouts(sock,MON_HTTP_TIMEOUT_MS);
        if(connect(sock,p->ai_addr,(int)p->ai_addrlen)==0)break;
        mon_close(sock);sock=MON_BAD;
    }
    freeaddrinfo(addr);
    if(sock==MON_BAD)return -1;
    gui_rd_config_t ca=gui_rd_get_request(0),cb=gui_rd_get_request(1);
    char req[620];
    int q=snprintf(req,sizeof(req),
        "GET /monitor/preview?za=%d&zb=%d&wa=%u&wb=%u&ta=%d&tb=%d&la=%d&lb=%d&ma=%d&mb=%d&sa=%d&sb=%d HTTP/1.0\r\nConnection: close\r\n\r\n",
        (int)(ca.zoom*1000.f),(int)(cb.zoom*1000.f),ca.width,cb.width,
        ca.trigger_enabled,cb.trigger_enabled,ca.trigger_level,cb.trigger_level,
        ca.trigger_mode,cb.trigger_mode,ca.trigger_source,cb.trigger_source);
    if(q<=0||(size_t)q>=sizeof(req)||mon_send(sock,req,(size_t)q)!=0){mon_close(sock);return -1;}
    const size_t cap=gui_rd_wire_capacity()+1024u;
    uint8_t *response=malloc(cap);
    if(!response){mon_close(sock);return -1;}
    size_t used=0;
    while(used<cap){
#ifdef _WIN32
        int n=recv(sock,(char*)response+used,(int)(cap-used),0);
#else
        ssize_t n=recv(sock,response+used,cap-used,0);
#endif
        if(n<=0)break;
        used+=(size_t)n;
    }
    mon_close(sock);
    int ok=-1;
    if(used>30 && memcmp(response,"HTTP/1.0 200 ",13)==0){
        size_t header=0;
        for(size_t i=0;i+3<used&&i<900;i++)
            if(!memcmp(response+i,"\r\n\r\n",4)){header=i+4;break;}
        if(header){
            size_t wire_size=used-header;
            /* Strict compressed frame length + format validation in codec. */
            if(wire_size && wire_size<=gui_rd_wire_capacity() &&
               gui_rd_client_accept(response+header,wire_size))ok=0;
        }
    }
    free(response);
    return ok;
}
static void mon_worker_folders(void){
    char request[MON_REMOTE_PATH]={0},selection[MON_REMOTE_PATH]={0};
    unsigned page=0,seq=0;bool browse=false,select=false;
    fs_lock();
    if(g_fs_client.request_seq!=g_fs_client.seen_seq){
        seq=g_fs_client.request_seq;page=g_fs_client.page;
        snprintf(request,sizeof(request),"%s",g_fs_client.requested);
        g_fs_client.seen_seq=seq;browse=true;
    }
    if(g_fs_client.select_pending){
        snprintf(selection,sizeof(selection),"%s",g_fs_client.selection);
        g_fs_client.select_pending=false;select=true;
    }
    fs_unlock();
    if(browse){
        char encoded[MON_REMOTE_PATH*3+1],url[MON_REMOTE_PATH*3+80];
        char *response=malloc(MON_BROWSER_WIRE);
        gui_remote_fs_result_t *r=calloc(1,sizeof(*r));
        bool ok=response&&r&&mon_url_encode(request,encoded,sizeof(encoded));
        if(ok){
            snprintf(url,sizeof(url),"/monitor/fs/browse?at=%s&page=%u",encoded,page);
            ok=mon_get(g_client.host,g_client.port,url,response,MON_BROWSER_WIRE)==0&&
               mon_parse_browse(response,r);
        }
        if(r)r->page=page;
        fs_lock();
        if(seq==g_fs_client.request_seq){
            if(ok)g_fs_client.result=*r;
            else memset(&g_fs_client.result,0,sizeof(g_fs_client.result));
            g_fs_client.failed=!ok;g_fs_client.ready=true;
        }
        fs_unlock();free(response);free(r);
    }
    if(select){
        char encoded[MON_REMOTE_PATH*3+1],url[MON_REMOTE_PATH*3+80],response[128];
        if(mon_url_encode(selection,encoded,sizeof(encoded))){
            snprintf(url,sizeof(url),"/monitor/fs/select?at=%s",encoded);
            if(mon_get(g_client.host,g_client.port,url,response,sizeof(response))!=0)
                fprintf(stderr,"[M-CLIENT] Server rejected Output folder: %s\n",selection);
        }
    }
}
static void mon_worker_output(void){
    char *body=malloc(MON_REMOTE_PATH*3+32);
    char path[MON_REMOTE_PATH];
    if(body&&mon_get(g_client.host,g_client.port,"/monitor/fs/output",body,MON_REMOTE_PATH*3+32)==0&&
       mon_url_decode(body,strlen(body),path,sizeof(path))){
        fs_lock();snprintf(g_fs_client.server_output,sizeof(g_fs_client.server_output),"%s",path);
        g_fs_client.output_valid=true;fs_unlock();
    }
    free(body);
}
static int mon_client_thread(void *ctx){
    (void)ctx;
    gui_monitor_session_state_t session={0};
    uint64_t expected_record_id=0;
    while(!atomic_load(&g_client.stopping)){
        char response[4096];
        int status_rc=mon_get(g_client.host,g_client.port,"/monitor/status",response,sizeof(response));
        uint64_t status_state=0;
        bool verified=status_rc==0 &&
            strstr(response,"\"protocol\":\"MISRC-M/2\"")!=NULL &&
            mon_json_u64(response,"state",&status_state) && status_state<=2;
        if(verified){
            if(gui_monitor_session_status_ok(&session)){
                fprintf(stderr,"[M-CLIENT] Status recovered after network dropout; session preserved\n");
                gui_app_set_status(g_client.app,"Monitor network recovered; session retained");
            }
            atomic_store(&g_client.last_error,0);
            if(expected_record_id){
                uint64_t result_id=0,result_code=0;
                if(mon_json_u64(response,"record_result_id",&result_id) &&
                   mon_json_u64(response,"record_result_code",&result_code) &&
                   result_id==expected_record_id){
                    if(result_code==1){
                        char reason[256]={0};
                        const char *key="\"record_result_detail\":\"";
                        const char *start=strstr(response,key);
                        if(start){
                            start+=strlen(key);
                            const char *end=strchr(start,'"');
                            if(end && (size_t)(end-start)<sizeof(reason))
                                (void)mon_url_decode(start,(size_t)(end-start),reason,sizeof(reason));
                        }
                        char msg[512];
                        snprintf(msg,sizeof(msg),"Recording failed on server: %s",
                                 reason[0]?reason:"unknown server error");
                        fprintf(stderr,"[M-CLIENT] %s\n",msg);
                        mon_record_ui_error(msg);
                    }else if(result_code==2){
                        gui_app_set_status(g_client.app,"Recording is awaiting confirmation on server");
                    }
                    expected_record_id=0;
                }
            }
            int state=(int)status_state;unsigned rate=0,frames=0,errors=0,missed=0;
            char *p=NULL;
            p=strstr(response,"\"sample_rate\":");
            if(p)rate=(unsigned)strtoul(p+14,NULL,10);
            p=strstr(response,"\"frames\":");
            if(p)frames=(unsigned)strtoul(p+9,NULL,10);
            p=strstr(response,"\"errors\":");
            if(p)errors=(unsigned)strtoul(p+9,NULL,10);
            p=strstr(response,"\"missed\":");
            if(p)missed=(unsigned)strtoul(p+9,NULL,10);
            if(state<0||state>2)state=0;
            atomic_store(&g_client.peer_state,state);
            atomic_store(&g_client.peer_rate,rate);
            atomic_store(&g_client.peer_frames,frames);
            atomic_store(&g_client.peer_errors,errors);
            atomic_store(&g_client.peer_missed,missed);
            uint64_t values[27];
            static const char *const names[]={
                "record_elapsed_ms","raw_a","raw_b","compressed_a","compressed_b", "samples_a",
                "rec_a_pct","rec_b_pct","rf_pct","aud_pct","rec_a_waits","rec_b_waits","rec_a_drops","rec_b_drops",
                "local_queue_bytes","local_queue_capacity","capture_a","capture_b","use_flac","local_save",
                "stream_synced","server_free_valid","server_free_bytes","clip_a_pos","clip_a_neg","clip_b_pos","clip_b_neg"};
            bool valid=true;
            for(unsigned i=0;i<27;i++) if(!mon_json_u64(response,names[i],&values[i])){valid=false;break;}
            if(valid){
                atomic_store(&g_client.elapsed_ms,values[0]);
                atomic_store(&g_client.raw[0],values[1]);atomic_store(&g_client.raw[1],values[2]);
                atomic_store(&g_client.compressed[0],values[3]);atomic_store(&g_client.compressed[1],values[4]);
                atomic_store(&g_client.samples_a,values[5]);
                for(int ch=0;ch<2;ch++){
                    atomic_store(&g_client.rec_fill_pct[ch],values[6+ch]>100?100:(unsigned)values[6+ch]);
                    atomic_store(&g_client.rec_waits[ch],(unsigned)values[10+ch]);
                    atomic_store(&g_client.rec_drops[ch],(unsigned)values[12+ch]);
                }
                atomic_store(&g_client.rf_fill_pct,(unsigned)values[8]);
                atomic_store(&g_client.aud_fill_pct,(unsigned)values[9]);
                atomic_store(&g_client.send_queue_bytes,values[14]);
                atomic_store(&g_client.send_queue_capacity,values[15]);
                atomic_store(&g_client.capture_a,values[16]!=0);atomic_store(&g_client.capture_b,values[17]!=0);
                atomic_store(&g_client.use_flac,values[18]!=0);atomic_store(&g_client.local_save,values[19]!=0);
                atomic_store(&g_client.stream_synced,values[20]!=0);
                atomic_store(&g_client.server_free_valid,values[21]!=0);
                atomic_store(&g_client.server_free_bytes,values[22]);
                atomic_store(&g_client.clip_pos[0],(unsigned)values[23]);
                atomic_store(&g_client.clip_neg[0],(unsigned)values[24]);
                atomic_store(&g_client.clip_pos[1],(unsigned)values[25]);
                atomic_store(&g_client.clip_neg[1],(unsigned)values[26]);
            }
            atomic_store(&g_client.record_stats_valid,valid);
            atomic_store(&g_client.connected,true);
            atomic_store(&g_client.app->net_connected,true);
            /* Skipping the preview fetch when stopping halves the
             * gui_monitor_client_stop join wait (a mode switch stopping the
             * monitor thread does not wait out another preview transfer). */
            if(state>0 && !atomic_load(&g_client.stopping)){
                if(mon_get_preview(g_client.host,g_client.port)!=0){
                    /* An individual preview failure is not a user Disconnect.
                     * Retain the last complete frame and expose its staleness. */
                    if(gui_monitor_session_preview_failed(&session)){
                        fprintf(stderr,"[M-CLIENT] Preview request failed; last valid frame retained\n");
                        /* The UI reads g_client.preview_error for the live warning. */
                    }
                }else if(gui_monitor_session_preview_ok(&session)){
                    fprintf(stderr,"[M-CLIENT] Preview stream recovered\n");
                }
                atomic_store(&g_client.preview_error,session.preview_failed);
                atomic_store(&g_client.preview_dropouts,session.preview_errors);
            }else if(state<=0){
                gui_rd_client_clear();  /* Server explicitly reports idle. */
                session.preview_failed=false;
                atomic_store(&g_client.preview_error,false);
            }
            /* M-Client's server-owned filesystem browser runs only on this
             * network worker. GUI remains responsive on slow NAS mounts. */
            mon_worker_folders();
            mon_cfg_worker();
            gui_app_t *app=g_client.app;
            if(atomic_exchange(&app->net_cmd_select_device,false)){
                int idx=atomic_load(&app->net_cmd_device_index);
                char device_url[96];snprintf(device_url,sizeof(device_url),"/monitor/device?index=%d",idx);
                if(mon_get(g_client.host,g_client.port,device_url,response,sizeof(response))!=0)
                    gui_app_set_status(app,"Server device selection refused (stop recording first)");
            }
            static unsigned output_tick=0;
            if((output_tick++%10)==0)mon_worker_output();
            /* Forward only to a verified monitor peer; never to an RF Server. */
            /* Apply user clicks on the network worker; never block the GUI
             * waiting for the Pi (or a failing Wi-Fi connection). */
            for(int ch=0;ch<2;ch++){
                if(atomic_exchange(&g_client.reset_clips_pending[ch],false)){
                    char reset_path[64];
                    snprintf(reset_path,sizeof(reset_path),"/monitor/reset-clips?channel=%d",ch);
                    if(mon_get(g_client.host,g_client.port,reset_path,response,sizeof(response))!=0)
                        gui_app_set_status(app,"Server clip reset failed");
                }
            }
            if(atomic_exchange(&app->net_cmd_start,false))
                (void)mon_get(g_client.host,g_client.port,"/monitor/start",response,sizeof(response));
            if(atomic_exchange(&app->net_cmd_stop,false))
                (void)mon_get(g_client.host,g_client.port,"/monitor/stop",response,sizeof(response));
            if(atomic_exchange(&app->net_cmd_record_on,false)){
                bool local=app->settings.monitor_local_save_enabled;
                if(local){
                    if(gui_monitor_local_save_client_start(g_client.host,g_client.port,
                           app->settings.monitor_local_save_path)!=0){
                        gui_app_set_status(app,"Local Save unavailable: check client destination and server");
                        fprintf(stderr,"[M-LOCAL] Receiver start failed; recording NOT started\n");
                    }else if(mon_get(g_client.host,g_client.port,"/monitor/record?on=1&local=1",
                                    response,sizeof(response))!=0){
                        mon_record_ui_error("Recording failed: Local Save request rejected by server");
                        gui_monitor_local_save_client_stop();
                    }else if(sscanf(response,"record queued %" SCNu64,&expected_record_id)!=1){
                        gui_app_set_status(app,"Older server: recording requested; no result confirmation available");
                    }
                }else{
                    if(mon_get(g_client.host,g_client.port,"/monitor/record?on=1",response,sizeof(response))!=0){
                        mon_record_ui_error("Recording failed: server request could not be delivered");
                    }else if(sscanf(response,"record queued %" SCNu64,&expected_record_id)!=1){
                        gui_app_set_status(app,"Older server: recording requested; no result confirmation available");
                    }
                }
            }
            if(atomic_exchange(&app->net_cmd_record_off,false))
                (void)mon_get(g_client.host,g_client.port,"/monitor/record?on=0",response,sizeof(response));
        }else{
            int error_reason=1; /* Network, socket, or HTTP error. */
            if(status_rc==0)
                error_reason=strstr(response,"\"protocol\":\"MISRC-M/1\"") ? 2 : 3;
            bool first=gui_monitor_session_status_failed(&session);
            atomic_store(&g_client.status_dropouts,session.status_errors);
            atomic_store(&g_client.last_error,error_reason);
            if(first || session.consecutive_status_errors==10 ||
               (session.consecutive_status_errors%100)==0){
                fprintf(stderr,"[M-CLIENT] Status error %d (%u consecutive, %u total); %s\n",
                    error_reason,session.consecutive_status_errors,session.status_errors,
                    session.established?"established session retained":"still waiting for first connection");
                if(first && session.established)
                    gui_app_set_status(g_client.app,"Monitor NETWORK ERROR: session retained; retrying");
            }
            /* DO NOT clear session, remote display, peer counters, synchronized
             * settings, local-save receiver or the remote device catalogue here.
             * Transient request errors must not cause Connect/Disconnect loops.
             * The explicit stop/restart path below owns all of that teardown. */
            /* Drop commands whose delivery cannot be confirmed. Never silently
             * replay a stale Record/Stop click after network recovery. */
            gui_app_t *app=g_client.app;
            bool pending=atomic_exchange(&app->net_cmd_start,false);
            pending=atomic_exchange(&app->net_cmd_stop,false)||pending;
            pending=atomic_exchange(&app->net_cmd_record_on,false)||pending;
            pending=atomic_exchange(&app->net_cmd_record_off,false)||pending;
            if(pending){
                fprintf(stderr,"[M-CLIENT] Discarded unconfirmed capture/record command during network error\n");
                gui_app_set_status(app,"Monitor network error: command not sent; retry when healthy");
            }
        }
        thrd_sleep_ms(MON_PREVIEW_PERIOD_MS);
    }
    return 0;
}
int gui_monitor_client_start(gui_app_t *app,const char *host,uint16_t port){
    if(!app||!host||!host[0]||!port)return -1;
    gui_monitor_client_stop();
    /* A reconnect cancels any staged restore: the local catalogue must not be
     * restored over a fresh M-Client session that just snapshotted it. */
    atomic_store(&g_monitor_restore_pending,false);
    g_client.app=app;g_client.port=port;
    mon_cfg_disconnect();
    cfg_lock();
    g_cfg_client.local_count=app->device_count;
    if(g_cfg_client.local_count<0)g_cfg_client.local_count=0;
    if(g_cfg_client.local_count>MAX_DEVICES)g_cfg_client.local_count=MAX_DEVICES;
    memcpy(g_cfg_client.local_devices,app->devices,
        (size_t)g_cfg_client.local_count*sizeof(device_info_t));
    g_cfg_client.local_selected=app->selected_device;
    cfg_unlock();
    while(atomic_flag_test_and_set_explicit(&g_record_ui_lock,memory_order_acquire))
        thrd_sleep_ms(1);
    g_record_ui_pending=false;
    g_record_ui_message[0]=0;
    atomic_flag_clear_explicit(&g_record_ui_lock,memory_order_release);
    fs_lock();memset(&g_fs_client,0,sizeof(g_fs_client));fs_unlock();
    snprintf(g_client.host,sizeof(g_client.host),"%s",host);
    atomic_store(&g_client.stopping,false);
    atomic_store(&g_client.connected,false);
    gui_rd_client_clear();
    gui_monitor_local_save_client_stop();
    atomic_store(&g_client.peer_state,0);
    atomic_store(&g_client.peer_frames,0);
    atomic_store(&g_client.peer_errors,0);
    atomic_store(&g_client.peer_missed,0);
    atomic_store(&g_client.last_error,0);
    atomic_store(&g_client.status_dropouts,0);
    atomic_store(&g_client.preview_dropouts,0);
    atomic_store(&g_client.preview_error,false);
    for(int ch=0;ch<2;ch++) atomic_store(&g_client.reset_clips_pending[ch],false);
    if(thrd_create(&g_client.thread,mon_client_thread,NULL)!=thrd_success)return -1;
    atomic_store(&g_client.running,true);
    fprintf(stderr,"[M-CLIENT] Connecting to monitor %s:%u (Zstd-1 original waveform/FFT + status/control)\n",host,(unsigned)port);
    return 0;
}
void gui_monitor_client_stop(void){
    if(!atomic_load(&g_client.running))return;
    atomic_store(&g_client.stopping,true);
    thrd_join(g_client.thread,NULL);
    if(g_client.app){
        /* Neither Pi settings nor its remote device indices may leak into
         * Harry's existing Local / full-RF Client modes. The settings/devices
         * restores are STAGED for the main thread (this may be the net
         * transition worker); the atomic flag clears are thread-safe. */
        atomic_store(&g_monitor_restore_pending,true);
        atomic_store(&g_client.app->net_connected,false);
        atomic_store(&g_client.app->net_cmd_start,false);
        atomic_store(&g_client.app->net_cmd_stop,false);
        atomic_store(&g_client.app->net_cmd_record_on,false);
        atomic_store(&g_client.app->net_cmd_record_off,false);
    }
    atomic_store(&g_client.running,false);
    atomic_store(&g_client.connected,false);
    gui_rd_client_clear();
    atomic_store(&g_client.peer_state,0);
    atomic_store(&g_client.peer_frames,0);
    atomic_store(&g_client.peer_errors,0);
    atomic_store(&g_client.peer_missed,0);
    atomic_store(&g_client.last_error,0);
    atomic_store(&g_client.preview_error,false);
    g_client.app=NULL;
    fs_lock();memset(&g_fs_client,0,sizeof(g_fs_client));fs_unlock();
}
bool gui_monitor_client_running(void){return atomic_load(&g_client.running);}
bool gui_monitor_client_take_pending_restore(gui_app_t *app){
    if(!app)return false;
    if(!atomic_exchange(&g_monitor_restore_pending,false))return false;
    /* MAIN THREAD ONLY (the net pollers and gui_net_stop call this): apply the
     * stop's staged restores - the local recording profile and the local
     * device catalogue saved when the M-Client session began. */
    gui_settings_restore_monitor_profile(&app->settings);
    cfg_lock();
    if(g_cfg_client.local_count>0){
        memcpy(app->devices,g_cfg_client.local_devices,
            (size_t)g_cfg_client.local_count*sizeof(device_info_t));
    }
    app->device_count=g_cfg_client.local_count;
    app->selected_device=g_cfg_client.local_selected;
    cfg_unlock();
    return true;
}
bool gui_monitor_client_connected(void){return atomic_load(&g_client.connected);}
int gui_monitor_client_peer_state(void){return atomic_load(&g_client.peer_state);}
uint32_t gui_monitor_client_peer_rate(void){return atomic_load(&g_client.peer_rate);}
uint32_t gui_monitor_client_peer_frames(void){return atomic_load(&g_client.peer_frames);}
uint32_t gui_monitor_client_peer_errors(void){return atomic_load(&g_client.peer_errors);}
uint32_t gui_monitor_client_peer_missed(void){return atomic_load(&g_client.peer_missed);}
bool gui_monitor_client_record_stats(gui_monitor_record_stats_t *out){
    if(!out)return false;
    memset(out,0,sizeof(*out));
    if(!gui_monitor_client_connected()||!atomic_load(&g_client.record_stats_valid))return false;
    out->elapsed_ms=atomic_load(&g_client.elapsed_ms);
    for(int ch=0;ch<2;ch++){
        out->raw[ch]=atomic_load(&g_client.raw[ch]);
        out->compressed[ch]=atomic_load(&g_client.compressed[ch]);
        out->rec_fill_pct[ch]=atomic_load(&g_client.rec_fill_pct[ch]);
        out->rec_waits[ch]=atomic_load(&g_client.rec_waits[ch]);
        out->rec_drops[ch]=atomic_load(&g_client.rec_drops[ch]);
    }
    out->samples_a=atomic_load(&g_client.samples_a);
    out->rf_fill_pct=atomic_load(&g_client.rf_fill_pct);
    out->aud_fill_pct=atomic_load(&g_client.aud_fill_pct);
    out->send_queue_bytes=atomic_load(&g_client.send_queue_bytes);
    out->send_queue_capacity=atomic_load(&g_client.send_queue_capacity);
    out->capture_a=atomic_load(&g_client.capture_a);
    out->capture_b=atomic_load(&g_client.capture_b);
    out->use_flac=atomic_load(&g_client.use_flac);
    out->local_save=atomic_load(&g_client.local_save);
    out->stream_synced=atomic_load(&g_client.stream_synced);
    out->server_free_valid=atomic_load(&g_client.server_free_valid);
    out->server_free_bytes=atomic_load(&g_client.server_free_bytes);
    for(int ch=0;ch<2;ch++){
        out->clip_pos[ch]=atomic_load(&g_client.clip_pos[ch]);
        out->clip_neg[ch]=atomic_load(&g_client.clip_neg[ch]);
    }
    return true;
}

void gui_monitor_client_reset_clips(int channel){
    if(channel<0||channel>1||!gui_monitor_client_connected())return;
    atomic_store(&g_client.reset_clips_pending[channel],true);
}
bool gui_monitor_client_matches(const char *host,uint16_t port){
    return atomic_load(&g_client.running) && host &&
           strcmp(g_client.host,host)==0 && g_client.port==port;
}
int gui_monitor_client_last_error(void){return atomic_load(&g_client.last_error);}
uint32_t gui_monitor_client_status_dropouts(void){return atomic_load(&g_client.status_dropouts);}
uint32_t gui_monitor_client_preview_dropouts(void){return atomic_load(&g_client.preview_dropouts);}
bool gui_monitor_client_preview_error(void){return atomic_load(&g_client.preview_error);}


void gui_monitor_server_poll_settings(gui_app_t *app){
    if(!app||!gui_monitor_server_active())return;
    if(gui_monitor_local_save_recording() && gui_monitor_local_save_failed() && app->is_recording){
        gui_app_set_status(app,"Local Save failed: client .partial files retained");
        fprintf(stderr,"[M-LOCAL] Receiver disconnected/disk failure: stopping recording\n");
        atomic_store(&app->net_cmd_record_off,true);
    }
    char pending[MON_REMOTE_PATH]={0};bool changed=false;
    fs_lock();
    if(g_fs_server.pending_valid){
        snprintf(pending,sizeof(pending),"%s",g_fs_server.pending);
        g_fs_server.pending_valid=false;changed=true;
    }
    fs_unlock();
    if(changed){
        if(!app->is_recording){
            /* Revalidate on the main thread immediately before applying. */
            gui_remote_fs_result_t *r=malloc(sizeof(*r));
            bool ok=r&&gui_remote_fs_browse(pending,0,r)&&!r->recovered&&
                    !r->computer&&!strcmp(r->path,pending);
            free(r);
            if(ok){
                snprintf(app->settings.output_path,sizeof(app->settings.output_path),"%s",pending);
                gui_settings_save(&app->settings);
                fprintf(stderr,"[M-SERVER] Recording Output folder changed on server: %s\n",pending);
            }else fprintf(stderr,"[M-SERVER] Output folder selection rejected (recording or unavailable)\n");
        }else fprintf(stderr,"[M-SERVER] Output folder change rejected: recording active\n");
    }
    char key[MON_SETTINGS_KEY_CAP]={0},value[MON_SETTINGS_VALUE_CAP]={0};bool config_pending=false;
    cfg_lock();
    if(g_cfg_server.pending){snprintf(key,sizeof(key),"%s",g_cfg_server.pending_key);
        snprintf(value,sizeof(value),"%s",g_cfg_server.pending_value);
        g_cfg_server.pending=false;config_pending=true;}
    cfg_unlock();
    if(config_pending){
        if(!app->is_recording && gui_monitor_settings_set(&app->settings,key,value)){
            gui_settings_save(&app->settings);
            fprintf(stderr,"[M-SERVER] Remote setting applied: %s\n",key);
        }else fprintf(stderr,"[M-SERVER] Remote setting rejected: %s\n",key);
    }
    mon_cfg_snapshot(app);
    fs_lock();snprintf(g_fs_server.current,sizeof(g_fs_server.current),"%s",app->settings.output_path);fs_unlock();
}

void gui_monitor_client_browse_folders(const char *path,unsigned page){
    fs_lock();snprintf(g_fs_client.requested,sizeof(g_fs_client.requested),"%s",path?path:"");
    g_fs_client.page=page;g_fs_client.request_seq++;
    g_fs_client.ready=false;g_fs_client.failed=false;
    fs_unlock();
}
int gui_monitor_client_folder_count(void){
    fs_lock();int n=g_fs_client.ready?(g_fs_client.failed?-2:(int)g_fs_client.result.count):-1;fs_unlock();return n;
}
bool gui_monitor_client_folder_state(char *path,size_t cap,char *parent,size_t pcap,
                                    bool *computer,bool *recovered,bool *failed,bool *more){
    fs_lock();bool ready=g_fs_client.ready;
    if(ready){
        if(path&&cap)snprintf(path,cap,"%s",g_fs_client.result.path);
        if(parent&&pcap)snprintf(parent,pcap,"%s",g_fs_client.result.parent);
        if(computer)*computer=g_fs_client.result.computer;
        if(recovered)*recovered=g_fs_client.result.recovered;
        if(failed)*failed=g_fs_client.failed;
        if(more)*more=g_fs_client.result.has_more;
    }
    fs_unlock();return ready;
}
bool gui_monitor_client_folder_name(unsigned index,char *name,size_t cap){
    if(!name||!cap)return false;
    fs_lock();bool ok=g_fs_client.ready&&!g_fs_client.failed&&index<g_fs_client.result.count;
    if(ok)snprintf(name,cap,"%s",g_fs_client.result.names[index]);
    fs_unlock();return ok;
}
void gui_monitor_client_select_output(const char *path){
    if(!path||!*path)return;
    fs_lock();snprintf(g_fs_client.selection,sizeof(g_fs_client.selection),"%s",path);
    g_fs_client.select_pending=true;fs_unlock();
}
bool gui_monitor_client_output_path(char *out,size_t cap){
    if(!out||!cap)return false;
    fs_lock();bool ok=g_fs_client.output_valid;
    if(ok)snprintf(out,cap,"%s",g_fs_client.server_output);
    fs_unlock();return ok;
}

/* Called on the capture server's main thread before consuming Record ON. */
