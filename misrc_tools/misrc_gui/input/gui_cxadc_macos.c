/* SPDX-License-Identifier: GPL-2.0-or-later
 * Optional cxadc-macos C ABI adapter; no Linux device-node emulation, subprocess,
 * socket server, PCI register definitions or DMA code in this backend.
 */
#include "gui_cxadc_macos.h"
#include "../core/gui_app.h"
#include "../output/gui_record.h"
#include "../ui/gui_ui.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define MAX_NATIVE_DEVICES 16
static const cxadc_client_api *api;
static void *library;
static cxadc_client_device devices[MAX_NATIVE_DEVICES];
static cxadc_client_settings settings[MAX_NATIVE_DEVICES];
static int selected, device_count;
static cxadc_client *client;
static atomic_bool active;
static unsigned char staging[CXADC_CLIENT_CHUNK_BYTES];
static size_t offset, available;
static bool dma_started;
static _Thread_local char last_error[256];
static atomic_bool stream_failed;
bool gui_cxadc_macos_failed(void) { return atomic_load(&stream_failed); }
void gui_cxadc_macos_mark_failed(void) { atomic_store(&stream_failed,true); }
static bool panel;
static char panel_status[256];
static cxadc_client_state final_state;

const char *gui_cxadc_macos_error(void) { return last_error; }
bool gui_cxadc_macos_active(void) { return atomic_load(&active); }
static int failed(void) {
    snprintf(last_error,sizeof(last_error),"%s",api ? api->error() : "Install libcxadc-client.dylib beside misrc_gui (or set CXADC_CLIENT_LIBRARY)");
    fprintf(stderr,"[CXADC macOS] %s\n",last_error); return -1;
}
static bool load_api(void) {
    if(api)return true;
    const char *override=getenv("CXADC_CLIENT_LIBRARY");
    if(override && override[0])library=dlopen(override,RTLD_NOW|RTLD_LOCAL);
    else {
        char path[4096]; uint32_t length=sizeof(path);
        if(_NSGetExecutablePath(path,&length)==0) {
            char *slash=strrchr(path,'/');
            if(slash){snprintf(slash+1,sizeof(path)-(size_t)(slash+1-path),"libcxadc-client.dylib");library=dlopen(path,RTLD_NOW|RTLD_LOCAL);}
        }
    }
    if(!library){failed();return false;}
    const cxadc_client_api *(*get_api)(void)=(const cxadc_client_api *(*)(void))dlsym(library,"cxadc_client_get_api_v1");
    if(get_api)api=get_api();
    if(!api || api->abi_version!=CXADC_CLIENT_ABI || api->struct_bytes!=sizeof(*api)) {
        api=NULL;snprintf(last_error,sizeof(last_error),"Incompatible cxadc-macos client ABI (need v1)");return false;
    }
    return true;
}
int gui_cxadc_macos_detect(void) {
    if(atomic_load(&active))return device_count;
    if(!load_api())return 0;
    size_t count=0;
    cxadc_client_device previous[MAX_NATIVE_DEVICES];
    cxadc_client_settings old_settings[MAX_NATIVE_DEVICES];
    int previous_count=device_count;
    memcpy(previous,devices,sizeof(previous));memcpy(old_settings,settings,sizeof(old_settings));
    if(api->enumerate(devices,MAX_NATIVE_DEVICES,&count)){failed();return 0;}
    device_count=(int)(count>MAX_NATIVE_DEVICES?MAX_NATIVE_DEVICES:count);
    for(int i=0;i<device_count;i++){
        settings[i]=(cxadc_client_settings){1,0,0,8,1};
        bool seen=false;
        for(int j=0;j<previous_count;j++)if(previous[j].registry_id==devices[i].registry_id){settings[i]=old_settings[j];seen=true;}
        if(!seen && devices[i].driver_attached){
            cxadc_client *c=NULL;cxadc_client_state state;
            if(!api->open(devices[i].registry_id,&c)){
                if(!api->status(c,&state))settings[i]=state.settings;
                api->close(c);
            }
        }
    }
    if(selected>=device_count)selected=0;
    return device_count;
}
const char *gui_cxadc_macos_name(int index) { return index>=0 && index<device_count ? devices[index].name : "CXADC macOS unavailable"; }
uint64_t gui_cxadc_macos_registry_id(int index) { return index>=0 && index<device_count ? devices[index].registry_id : 0; }
void gui_cxadc_macos_select(int index) { if(!atomic_load(&active) && index>=0 && index<device_count)selected=index; }
static uint32_t *parameter(cxadc_client_settings *s,const char *name) {
    if(!strcmp(name,"vmux"))return &s->vmux;
    if(!strcmp(name,"level"))return &s->level;
    if(!strcmp(name,"sixdb"))return &s->sixdb;
    if(!strcmp(name,"center_offset"))return &s->center_offset;
    if(!strcmp(name,"agc_enable"))return &s->agc_enable;
    return NULL;
}
int gui_cxadc_macos_get(const char *name,int *value) {
    if(!value || !load_api() || selected>=device_count)return -1;
    if(!strcmp(name,"tenbit")){*value=0;return 0;}
    uint32_t *p=parameter(&settings[selected],name);
    if(!p)return -1;
    *value=(int)*p;return 0;
}
int gui_cxadc_macos_set(const char *name,int value) {
    if(!strcmp(name,"tenbit"))return value==0?0:-1;
    if(!load_api() || selected>=device_count || atomic_load(&active)) {
        snprintf(last_error,sizeof(last_error),"Stop capture before changing ADC settings");return -1;
    }
    cxadc_client_settings next=settings[selected];uint32_t *p=parameter(&next,name);
    if(!p || value<0)return -1;
    *p=(uint32_t)value;
    cxadc_client *c=NULL;
    if(api->open(devices[selected].registry_id,&c))return failed();
    int rc=api->configure(c,&next);
    if(rc)failed();
    cxadc_client_state state;
    if(!rc && api->status(c,&state)){failed();rc=-1;}
    if(!rc)settings[selected]=state.settings;
    api->close(c);return rc? -1:0;
}
int gui_cxadc_macos_open(int card) {
    if(card!=0 || !load_api() || selected>=device_count)return -1;
    if(api->open(devices[selected].registry_id,&client))return failed();
    int rc=api->configure(client,&settings[selected]);
    /* DMA starts on the RF thread only after GUI buffers/workers are ready. */
    if(rc){failed();api->close(client);client=NULL;return -1;}
    offset=available=0;dma_started=false;atomic_store(&stream_failed,false);atomic_store(&active,true);
    fprintf(stderr,"[CXADC macOS] started %s nominal 28636363 Hz RAW u8 vmux=%u level=%u sixdb=%u offset=%u agc=%u\n",
        devices[selected].name,settings[selected].vmux,settings[selected].level,settings[selected].sixdb,settings[selected].center_offset,settings[selected].agc_enable);
    return 0;
}
int gui_cxadc_macos_read(int card,unsigned char *bytes,size_t length) {
    if(card!=0 || !client || !atomic_load(&active))return -1;
    if(!dma_started){if(api->start(client))return failed();dma_started=true;}
    if(offset==available) {
        offset=available=0;
        if(api->read(client,staging,sizeof(staging),&available))return failed();
        if(!available)return 0;
    }
    size_t count=available-offset;if(count>length)count=length;
    memcpy(bytes,staging+offset,count);offset+=count;return (int)count;
}
void gui_cxadc_macos_close(int card) {
    if(card!=0 || !client)return;
    int rc=api->stop(client);if(rc){gui_cxadc_macos_mark_failed();failed();}
    dma_started=false;
    if(!api->status(client,&final_state))
        fprintf(stderr,"[CXADC macOS] stop=%s delivered=%llu irq=%llu wraps=%llu error=0x%x overruns=%u stop_verified=%u command=0x%x quarantine=%u\n",
            rc?"FAILED":"verified",(unsigned long long)final_state.delivered_bytes,(unsigned long long)final_state.interrupts,
            (unsigned long long)final_state.ring_wraps,final_state.error,final_state.overruns,final_state.stop_verified,
            final_state.command_restored,final_state.quarantined);
    api->close(client);client=NULL;atomic_store(&active,false);offset=available=0;
}
void gui_cxadc_macos_log(gui_app_t *app) {
    if(!client)return;
    cxadc_client_state state;if(api->status(client,&state)){failed();return;}
    char text[512];snprintf(text,sizeof(text),"cxadc-macos: driver=%u API=%u nominal_rate_hz=%u (unmeasured clock) unsigned8 vmux=%u level=%u sixdb=%u center_offset=%u agc=%u delivered=%llu IRQ=%llu wraps=%llu error=0x%x overruns=%u; authoritative_dropped_sample_count=null",
      state.driver_build,state.driver_api,state.nominal_rate_hz,state.settings.vmux,state.settings.level,state.settings.sixdb,
      state.settings.center_offset,state.settings.agc_enable,(unsigned long long)state.delivered_bytes,(unsigned long long)state.interrupts,
      (unsigned long long)state.ring_wraps,state.error,state.overruns);
    gui_record_log_capture_event(app,"INFO",text,GUI_ERROR_CLASS_SYSTEM,0);
}
static Rectangle panel_box(void) {return (Rectangle){(GetScreenWidth()-540)/2.0f,(GetScreenHeight()-360)/2.0f,540,360};}
bool gui_cxadc_macos_ui_handle(gui_app_t *app) {
    bool selected_cx=app->selected_device>=0 && app->selected_device<app->device_count && app->devices[app->selected_device].type==DEVICE_TYPE_CXADC;
    if(selected_cx && IsKeyPressed(KEY_F8)) {
        panel=!panel;
        if(panel) {
            int index=0;sscanf(app->devices[app->selected_device].serial,"CXADC_MACOS_%d",&index);gui_cxadc_macos_select(index);
            if(!app->is_capturing && api && selected<device_count) {
                cxadc_client *c=NULL;cxadc_client_state state;
                if(!api->open(devices[selected].registry_id,&c)){
                    if(!api->status(c,&state))settings[selected]=state.settings;
                    api->close(c);
                }
            }
            snprintf(panel_status,sizeof(panel_status),"Stop capture to apply settings. Stock clock; RAW8 only.");
        }
    }
    if(!panel)return false;
    if(IsKeyPressed(KEY_ESCAPE))panel=false;
    Rectangle box=panel_box();Vector2 mouse=gui_ui_get_mouse_position();
    /* This overlay is drawn after Clay in window coordinates. */
    mouse.x*=gui_ui_get_scale_factor();mouse.y*=gui_ui_get_scale_factor();
    if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if(CheckCollisionPointRec(mouse,(Rectangle){box.x+420,box.y+315,100,30})){panel=false;return true;}
        if(!app->is_capturing) {
            const char *names[]={"vmux","level","sixdb","center_offset","agc_enable"};
            const int maxima[]={3,31,1,255,1};
            for(int row=0;row<5;row++) {
                int delta=0;
                if(CheckCollisionPointRec(mouse,(Rectangle){box.x+310,box.y+55+row*42,40,30}))delta=-1;
                if(CheckCollisionPointRec(mouse,(Rectangle){box.x+460,box.y+55+row*42,40,30}))delta=1;
                if(delta) {
                    int value=0;gui_cxadc_macos_get(names[row],&value);value+=delta;if(value<0)value=0;if(value>maxima[row])value=maxima[row];
                    int rc=gui_cxadc_macos_set(names[row],value);
                    snprintf(panel_status,sizeof(panel_status),"%s",rc?gui_cxadc_macos_error():"Applied and read back from hardware");
                }
            }
        }
    }
    return true; /* modal: underlying UI does not receive these clicks */
}
void gui_cxadc_macos_ui_draw(gui_app_t *app) {
    if(app->selected_device>=0 && app->selected_device<app->device_count && app->devices[app->selected_device].type==DEVICE_TYPE_CXADC)
        DrawText("F8: CXADC hardware settings",20,GetScreenHeight()-48,16,LIGHTGRAY);
    if(!panel)return;
    Rectangle box=panel_box();DrawRectangle(0,0,GetScreenWidth(),GetScreenHeight(),Fade(BLACK,.7f));
    DrawRectangleRec(box,(Color){28,32,40,255});DrawRectangleLinesEx(box,2,GRAY);
    DrawText("macOS CXADC - 28.636363 MSPS / unsigned 8-bit",box.x+16,box.y+15,19,RAYWHITE);
    const char *names[]={"VMUX","ADC level","6 dB boost","Center offset","ADC AGC"};
    const char *parameters[]={"vmux","level","sixdb","center_offset","agc_enable"};
    for(int row=0;row<5;row++) {
        int value=0;gui_cxadc_macos_get(parameters[row],&value);char text[32];snprintf(text,sizeof(text),"%u",value);
        float y=box.y+55+row*42;DrawText(names[row],box.x+20,y+5,19,RAYWHITE);
        DrawRectangleLinesEx((Rectangle){box.x+310,y,40,30},1,GRAY);DrawText("-",box.x+325,y+5,20,app->is_capturing?GRAY:RAYWHITE);
        DrawText(text,box.x+380,y+5,20,RAYWHITE);
        DrawRectangleLinesEx((Rectangle){box.x+460,y,40,30},1,GRAY);DrawText("+",box.x+473,y+5,20,app->is_capturing?GRAY:RAYWHITE);
    }
    DrawText(app->is_capturing?"Capture running: controls locked until stopped.":panel_status,box.x+16,box.y+280,14,LIGHTGRAY);
    DrawRectangleLinesEx((Rectangle){box.x+420,box.y+315,100,30},1,GRAY);DrawText("Close",box.x+440,box.y+321,18,RAYWHITE);
}
