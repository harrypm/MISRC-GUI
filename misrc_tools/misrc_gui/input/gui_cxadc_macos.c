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
#include "clockgen_audio.h"
#include <libusb.h>

#define MAX_NATIVE_DEVICES 16
static const cxadc_client_api *api;
static const cxadc_client_api_v2 *api2;
static const cxadc_audio_api *audio_api;
static cxadc_audio *audio;
static bool clockgen;
static uint32_t native_rate=28636363;
static int capture_devices[2];
static int capture_count;
static void *library;
static cxadc_client_device devices[MAX_NATIVE_DEVICES];
static cxadc_client_settings settings[MAX_NATIVE_DEVICES];
static int selected, device_count;
static cxadc_client *clients[2];
static atomic_bool active;
static unsigned char staging[2][CXADC_CLIENT_CHUNK_BYTES];
static size_t offset[2], available[2];
static bool dma_started[2];
static _Thread_local char last_error[256];
static atomic_bool stream_failed;
bool gui_cxadc_macos_failed(void) { return atomic_load(&stream_failed); }
void gui_cxadc_macos_mark_failed(void) { atomic_store(&stream_failed,true); }
static bool panel;
static char panel_status[256];
static cxadc_client_state final_state[2];

const char *gui_cxadc_macos_error(void) { return last_error; }
bool gui_cxadc_macos_active(void) { return atomic_load(&active); }
bool gui_cxadc_macos_clockgen_active(void) {return clockgen && atomic_load(&active); }
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
    const cxadc_client_api_v2 *(*get2)(void)=(const cxadc_client_api_v2 *(*)(void))dlsym(library,"cxadc_client_get_api_v2");
    if(get2)api2=get2();
    if(api2 && (api2->abi_version!=2 || api2->struct_bytes!=sizeof(*api2)))api2=NULL;
    const cxadc_audio_api *(*get_audio)(void)=(const cxadc_audio_api *(*)(void))dlsym(library,"cxadc_clockgen_audio_get_api_v1");
    if(get_audio)audio_api=get_audio();
    if(audio_api && (audio_api->abi_version!=1 || audio_api->struct_bytes!=sizeof(*audio_api)))audio_api=NULL;
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
int gui_cxadc_macos_get_card(int card,const char *name,int *value) {
    if(card<0 || card>1)return -1;
    int previous=selected;
    if(clockgen)selected=card;
    int rc=gui_cxadc_macos_get(name,value);selected=previous;return rc;
}
int gui_cxadc_macos_set_card(int card,const char *name,int value) {
    if(card<0 || card>1)return -1;
    int previous=selected;
    if(clockgen)selected=card;
    int rc=gui_cxadc_macos_set(name,value);selected=previous;return rc;
}
void gui_cxadc_macos_profile(bool use_clockgen) {
    if(!atomic_load(&active)){clockgen=use_clockgen;native_rate=clockgen?40000000:28636363;}
}
/* ClockGen protocol: pinned wolfre firmware 9763676, UAC2 selector units
 * 0x20/0x21. Descriptor layout is verified before writes. No interface claim,
 * kernel driver detachment, firmware flash or undocumented request. */
static int clockgen_rates(bool configure, uint32_t rates[2]) {
    libusb_context *context=NULL;
    if(libusb_init(&context))return -1;
    libusb_device **list=NULL;ssize_t count=libusb_get_device_list(context,&list);
    libusb_device *device=NULL;int matches=0;
    for(ssize_t i=0;i<count;i++) {
        struct libusb_device_descriptor d;
        if(!libusb_get_device_descriptor(list[i],&d) && d.idVendor==0x1209 && d.idProduct==0x0001){device=list[i];matches++;}
    }
    libusb_device_handle *handle=NULL;int rc=-1;
    if(matches!=1 || libusb_open(device,&handle))goto done;
    unsigned char desc[4096];int n=libusb_control_transfer(handle,0x80,6,0x0200,0,desc,sizeof(desc),1000);
    int mask=0;
    for(int i=0;i+2<=n;) {
        int size=desc[i];if(size<2 || i+size>n)goto done;
        if(size==11 && desc[i+1]==0x24 && desc[i+2]==5 && desc[i+4]==4 &&
           desc[i+5]==0x10 && desc[i+6]==0x11 && desc[i+7]==0x12 && desc[i+8]==0x13) {
            if(desc[i+3]==0x20)mask|=1;if(desc[i+3]==0x21)mask|=2;
        }
        i+=size;
    }
    if(mask!=3)goto done;
    static const uint32_t hz[]={0,20000000,28636360,40000000,50000000};
    for(int i=0;i<2;i++) {
        unsigned char selection=0;
        if(libusb_control_transfer(handle,0xa1,1,0x100,(0x20+i)<<8,&selection,1,1000)!=1 || selection<1 || selection>4)goto done;
        if(configure && selection!=3) {
            selection=3;
            if(libusb_control_transfer(handle,0x21,1,0x100,(0x20+i)<<8,&selection,1,1000)!=1)goto done;
            if(libusb_control_transfer(handle,0xa1,1,0x100,(0x20+i)<<8,&selection,1,1000)!=1 || selection!=3)goto done;
        }
        rates[i]=hz[selection];
    }
    rc=0;
done:
    if(handle)libusb_close(handle);
    if(list)libusb_free_device_list(list,1);
    libusb_exit(context);return rc;
}
bool gui_cxadc_macos_clockgen_available(void) {
    uint32_t rates[2];return load_api() && api2 && audio_api && audio_api->detect()==1 && clockgen_rates(false,rates)==0;
}
uint32_t gui_cxadc_macos_rate(void) {return native_rate;}
int gui_cxadc_macos_prepare(int count, bool use_clockgen) {
    if(!load_api() || atomic_load(&active) || count<1 || count>2 || device_count<count)return -1;
    if(use_clockgen && (!api2 || !audio_api)){snprintf(last_error,sizeof(last_error),"Update libcxadc-client.dylib for ClockGen support");return -1;}
    if(!use_clockgen) {
        uint32_t rates[2];
        if(!clockgen_rates(false,rates) && (rates[0]!=28636360 || rates[1]!=28636360)) {
            snprintf(last_error,sizeof(last_error),"External ClockGen is not at stock rate; select the CXADC Clockgen profile");return -1;
        }
    }
    if(use_clockgen) {
        /* Do not change the shared clock while any enumerated card runs. */
        for(int i=0;i<device_count;i++) {
            cxadc_client *c=NULL;cxadc_client_state state;
            if(api->open(devices[i].registry_id,&c))return failed();
            int rc=api->status(c,&state);api->close(c);
            if(rc)return failed();
            if(state.running){snprintf(last_error,sizeof(last_error),"Stop other CXADC captures before configuring ClockGen");return -1;}
        }
        uint32_t rates[2];
        if(clockgen_rates(true,rates) || rates[0]!=40000000 || rates[1]!=40000000) {
            snprintf(last_error,sizeof(last_error),"ClockGen USB configuration/readback failed; check BOTH outputs before capture");return -1;
        }
    }
    clockgen=use_clockgen;native_rate=clockgen?40000000:28636363;capture_count=count;
    for(int i=0;i<count;i++)capture_devices[i]=clockgen?i:selected;
    return 0;
}
int gui_cxadc_macos_open(int card) {
    if(card<0 || card>=capture_count || !load_api() || clients[card])return -1;
    int index=capture_devices[card];
    if(api->open(devices[index].registry_id,&clients[card]))return failed();
    int rc=api->configure(clients[card],&settings[index]);
    if(!rc && api2)rc=api2->set_nominal_rate(clients[card],native_rate);
    if(rc){failed();api->close(clients[card]);clients[card]=NULL;return -1;}
    offset[card]=available[card]=0;dma_started[card]=false;
    atomic_store(&stream_failed,false);atomic_store(&active,true);
    fprintf(stderr,"[CXADC macOS] card %d: %s nominal %u Hz RAW u8 vmux=%u level=%u sixdb=%u offset=%u agc=%u\n",
        card,devices[index].name,native_rate,settings[index].vmux,settings[index].level,settings[index].sixdb,settings[index].center_offset,settings[index].agc_enable);
    return card;
}
int gui_cxadc_macos_start_cards(void) {
    int rc=capture_count==2?api2->start_pair(clients[0],clients[1]):api->start(clients[0]);
    if(rc)return failed();
    for(int i=0;i<capture_count;i++)dma_started[i]=true;
    return 0;
}
int gui_cxadc_macos_read(int card,unsigned char *bytes,size_t length) {
    if(card<0 || card>=capture_count || !clients[card] || !atomic_load(&active))return -1;
    if(!dma_started[card]){if(api->start(clients[card]))return failed();dma_started[card]=true;}
    if(offset[card]==available[card]) {
        offset[card]=available[card]=0;
        if(api->read(clients[card],staging[card],sizeof(staging[card]),&available[card]))return failed();
        if(!available[card])return 0;
    }
    size_t count=available[card]-offset[card];if(count>length)count=length;
    memcpy(bytes,staging[card]+offset[card],count);offset[card]+=count;return (int)count;
}
void gui_cxadc_macos_close(int card) {
    if(card<0 || card>=2 || !clients[card])return;
    int rc=api->stop(clients[card]);if(rc){gui_cxadc_macos_mark_failed();failed();}
    dma_started[card]=false;
    if(!api->status(clients[card],&final_state[card]))
        fprintf(stderr,"[CXADC macOS] card=%d stop=%s delivered=%llu irq=%llu wraps=%llu error=0x%x overruns=%u stop_verified=%u command=0x%x quarantine=%u\n",
            card,rc?"FAILED":"verified",(unsigned long long)final_state[card].delivered_bytes,(unsigned long long)final_state[card].interrupts,
            (unsigned long long)final_state[card].ring_wraps,final_state[card].error,final_state[card].overruns,final_state[card].stop_verified,
            final_state[card].command_restored,final_state[card].quarantined);
    api->close(clients[card]);clients[card]=NULL;
    atomic_store(&active,clients[0]!=NULL || clients[1]!=NULL);offset[card]=available[card]=0;
}
void gui_cxadc_macos_log(gui_app_t *app) {
    for(int i=0;i<capture_count;i++) {
        if(!clients[i])continue;
        cxadc_client_state state;if(api->status(clients[i],&state)){failed();continue;}
        cxadc_client_timing timing={0};if(api2)api2->timing(clients[i],&timing);
        char text[768];snprintf(text,sizeof(text),"cxadc-macos: channel=%c registry_id=%llx driver=%u API=%u nominal_rate_hz=%u unmeasured-clock unsigned8 vmux=%u level=%u sixdb=%u center_offset=%u agc=%u delivered=%llu IRQ=%llu wraps=%llu error=0x%x overruns=%u started_monotonic_ns=%llu; authoritative_dropped_sample_count=null; sample_start_alignment=uncalibrated",
          'A'+i,(unsigned long long)devices[capture_devices[i]].registry_id,state.driver_build,state.driver_api,state.nominal_rate_hz,state.settings.vmux,state.settings.level,state.settings.sixdb,
          state.settings.center_offset,state.settings.agc_enable,(unsigned long long)state.delivered_bytes,(unsigned long long)state.interrupts,
          (unsigned long long)state.ring_wraps,state.error,state.overruns,(unsigned long long)timing.started_monotonic_ns);
        gui_record_log_capture_event(app,"INFO",text,GUI_ERROR_CLASS_SYSTEM,0);
    }
    if(audio && audio_api) {
        cxadc_audio_state state;
        if(!audio_api->status(audio,&state)) {
            char text[512];snprintf(text,sizeof(text),"ClockGen CoreAudio: rate_hz=%u channels=3 first_input_host_ns=%llu captured_frames=%llu delivered_frames=%llu timestamp_gaps=%llu queue_overruns=%llu error=%u; physical_latency=uncalibrated",
                state.rate_hz,(unsigned long long)state.first_host_ns,(unsigned long long)state.captured_frames,(unsigned long long)state.delivered_frames,
                (unsigned long long)state.timestamp_gaps,(unsigned long long)state.queue_overruns,state.error);
            gui_record_log_capture_event(app,"INFO",text,GUI_ERROR_CLASS_SYSTEM,0);
        }
    }
}
int gui_cxadc_macos_audio_open(uint32_t *rate) {
    if(!clockgen)return -1;
    if(!audio_api || audio_api->open(0,&audio)){snprintf(last_error,sizeof(last_error),"%s",audio_api?audio_api->error():"Update cxadc-macos audio client");return -1;}
    cxadc_audio_state s;audio_api->status(audio,&s);*rate=s.rate_hz;return 0;
}
int gui_cxadc_macos_audio_start(void) {
    if(!audio || audio_api->start(audio)){snprintf(last_error,sizeof(last_error),"%s",audio_api?audio_api->error():"ClockGen audio unavailable");return -1;}return 0;
}
int gui_cxadc_macos_audio_read(void *bytes,size_t capacity,size_t *frames) {
    uint64_t host=0;
    if(!audio || audio_api->read(audio,bytes,capacity,frames,&host)){snprintf(last_error,sizeof(last_error),"%s",audio_api?audio_api->error():"ClockGen audio unavailable");return -1;}return 0;
}
void gui_cxadc_macos_audio_close(void) {
    if(!audio)return;
    int rc=audio_api->stop(audio);cxadc_audio_state s;audio_api->status(audio,&s);
    fprintf(stderr,"[ClockGen CoreAudio] stop=%u frames=%llu gaps=%llu overruns=%llu error=%u\n",s.stop_verified,
        (unsigned long long)s.delivered_frames,(unsigned long long)s.timestamp_gaps,(unsigned long long)s.queue_overruns,s.error);
    if(rc){snprintf(last_error,sizeof(last_error),"%s",audio_api->error());gui_cxadc_macos_mark_failed();}
    audio_api->close(audio);audio=NULL;
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
            snprintf(panel_status,sizeof(panel_status),"Stop capture to apply settings. RAW8; select a card entry to configure it before ClockGen capture.");
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
    char heading[128];snprintf(heading,sizeof(heading),"macOS CXADC - %.6f MSPS / unsigned 8-bit",native_rate/1000000.0);
    DrawText(heading,box.x+16,box.y+15,19,RAYWHITE);
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
