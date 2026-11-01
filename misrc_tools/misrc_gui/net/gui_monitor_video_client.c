#include "gui_monitor_video_client.h"
#include "gui_monitor_video_codec.h"
#include "gui_monitor_video_wire.h"
#include "gui_monitor_video_options.h"
#include "../visualization/gui_text.h"
#include "../ui/gui_ui.h"
#include "../../common/threading.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
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
typedef SOCKET mvd_socket;
#define MVD_SOCKET_BAD INVALID_SOCKET
#define MVD_CLOSE closesocket
#else
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
typedef int mvd_socket;
#define MVD_SOCKET_BAD (-1)
#define MVD_CLOSE close
#endif
#define MVD_PIXEL_CAP ((size_t)MVD_MAX_WIDTH*MVD_MAX_HEIGHT*4u)
#define MVD_CLIENT_TIMEOUT_MS 1200u
/* Local-only OSD/Stats choices must not tear down a live H.264 stream. */
#define MVD_DSP_MASK (~((3u<<MVD_CTL_OSD)|(3u<<MVD_CTL_STATS)))

typedef struct {
    thrd_t worker;
    atomic_bool stop,active;
    atomic_uint mode;
    atomic_uint controls;
    atomic_int video_http_status; /* 0=connecting, negative=network/response error, positive=HTTP code */
    atomic_uint_fast64_t seen_seconds;
    atomic_flag pixel_busy;
    atomic_uint_fast64_t published;
    uint8_t *pixels;
    unsigned width,height;
    uint64_t generation;
    /* The following are UI-only, never accessed by the network worker. */
    Texture2D texture;
    unsigned texture_width,texture_height;
    uint8_t *scratch;
    uint64_t drawn_generation;
    /* UI-thread only; each decoder remembers its own choices. */
    uint32_t cvbs_settings,rf_settings;
    bool chrome_hidden;
    unsigned popup;
    double last_click;
    unsigned display_frame_count;
    double display_window_started,display_fps;
    atomic_flag stats_busy;
    char stats_text[1024];
} mvd_client_channel;
static struct {
    char host[128];uint16_t port;
    mvd_client_channel channels[2];
    atomic_bool active;
    atomic_bool stats_stop;
    atomic_uint global_budget; /* shared logical-core budget, same as Local */
    bool stats_started;
    thrd_t stats_worker;
} g_mvd_client;

static int mvd_send_full(mvd_socket sock,const char *bytes,size_t n) {
    while(n){
        int k=send(sock,bytes,(int)(n>16384u?16384u:n),0);
        if(k<=0)return -1;
        bytes+=k;n-=(size_t)k;
    }
    return 0;
}
static void mvd_socket_timeout(mvd_socket sock) {
#ifdef _WIN32
    DWORD ms=MVD_CLIENT_TIMEOUT_MS;
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char *)&ms,sizeof(ms));
    setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(const char *)&ms,sizeof(ms));
#else
    struct timeval tv={MVD_CLIENT_TIMEOUT_MS/1000,(MVD_CLIENT_TIMEOUT_MS%1000)*1000};
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
#endif
}
static mvd_socket mvd_connect(const char *host,uint16_t port) {
    char service[12];snprintf(service,sizeof(service),"%u",(unsigned)port);
    struct addrinfo hints={0},*addresses=NULL;
    hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,service,&hints,&addresses)!=0)return MVD_SOCKET_BAD;
    mvd_socket sock=MVD_SOCKET_BAD;
    for(struct addrinfo *p=addresses;p;p=p->ai_next){
        sock=socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(sock==MVD_SOCKET_BAD)continue;
        mvd_socket_timeout(sock);
        if(connect(sock,p->ai_addr,(int)p->ai_addrlen)==0)break;
        MVD_CLOSE(sock);sock=MVD_SOCKET_BAD;
    }
    freeaddrinfo(addresses);
    return sock;
}
static bool mvd_visible(mvd_client_channel *ch) {
    uint64_t last=atomic_load_explicit(&ch->seen_seconds,memory_order_acquire);
    uint64_t now=(uint64_t)time(NULL);
    return last && now>=last && now-last<=2;
}
static int mvd_read_status(mvd_socket sock) {
    char header[2048];size_t used=0;
    while(used+1<sizeof(header)) {
        char b;
        if(recv(sock,&b,1,0)!=1)return -1;
        header[used++]=b;
        if(used>=4 && !memcmp(header+used-4,"\r\n\r\n",4)){
            header[used]=0;
            int status=0;
            return sscanf(header,"HTTP/%*u.%*u %d",&status)==1 ? status:-1;
        }
    }
    return -1;
}
static int mvd_receive(unsigned channel,mvd_client_channel *ch,unsigned mode,uint32_t controls) {
    mvd_socket sock=mvd_connect(g_mvd_client.host,g_mvd_client.port);
    if(sock==MVD_SOCKET_BAD){atomic_store(&ch->video_http_status,-1);return -1;}
    char request[320];
    int n=snprintf(request,sizeof(request),
       "GET /monitor/video?channel=%u&mode=%u&bps=750000&fps=2500&ctl=%u HTTP/1.0\r\n"
       "Connection: close\r\n\r\n",channel,mode,(unsigned)controls);
    if(n<1 || (size_t)n>=sizeof(request) ||
       mvd_send_full(sock,request,(size_t)n)!=0){
        atomic_store(&ch->video_http_status,-1);
        MVD_CLOSE(sock);return -1;
    }
    int http_status=mvd_read_status(sock);
    atomic_store(&ch->video_http_status,http_status);
    if(http_status!=200){
        static const char *hint="check server OpenH264 and version";
        fprintf(stderr,"[M-VIDEO] CH%u HTTP %d; %s\n",channel+1,http_status,hint);
        MVD_CLOSE(sock);return -1;
    }
    gui_mvd_decoder_t *decoder=gui_mvd_decoder_create();
    uint8_t *packet=malloc(MVD_MAX_PAYLOAD);
    uint8_t *output=malloc(MVD_PIXEL_CAP);
    if(!decoder || !packet || !output){
        gui_mvd_decoder_destroy(decoder);free(packet);free(output);MVD_CLOSE(sock);return -1;
    }
    mvd_frame_reader reader;
    mvd_reader_init(&reader,packet,MVD_MAX_PAYLOAD);
    uint8_t buffer[16384];
    unsigned errors=0;
    bool synced=false;
    while(!atomic_load_explicit(&ch->stop,memory_order_acquire) &&
          mvd_visible(ch) && (atomic_load(&ch->controls)&MVD_DSP_MASK)==controls) {
        int bytes=recv(sock,(char *)buffer,sizeof(buffer),0);
        if(bytes==0)break;
        if(bytes<0) {
            /* Brief Wi-Fi timeouts are expected; a missing frame is not a
             * reason to restart the OpenH264 decoder immediately. */
            if(++errors<10)continue;
            break;
        }
        errors=0;
        size_t pos=0;
        while(pos<(size_t)bytes) {
            size_t consumed=0;
            int rc=mvd_reader_feed(&reader,buffer+pos,(size_t)bytes-pos,&consumed);
            pos+=consumed;
            if(rc<0 || !consumed) {pos=(size_t)bytes;errors=10;break;}
            if(rc==0)continue;
            mvd_frame_header h=reader.frame;
            if((h.flags&MVD_FLAG_HEARTBEAT)!=0){
                mvd_reader_reset(&reader);
                continue;
            }
            if((h.flags&MVD_FLAG_KEYFRAME)!=0)synced=true;
            if(synced && h.stream_id==channel+1 && h.payload_bytes<=MVD_MAX_PAYLOAD) {
                unsigned w=0,ht=0;
                int produced=gui_mvd_decoder_frame(decoder,packet,h.payload_bytes,
                                                    output,MVD_PIXEL_CAP,&w,&ht);
                if(produced==1 && w==h.width && ht==h.height &&
                   !atomic_flag_test_and_set_explicit(&ch->pixel_busy,memory_order_acquire)){
                    memcpy(ch->pixels,output,(size_t)w*ht*4u);
                    ch->width=w;ch->height=ht;
                    ch->generation++;
                    atomic_fetch_add(&ch->published,1);
                    atomic_flag_clear_explicit(&ch->pixel_busy,memory_order_release);
                }
                if(produced<0){errors=10;break;}
            }
            mvd_reader_reset(&reader);
        }
        if(errors>=10)break;
    }
    gui_mvd_decoder_destroy(decoder);
    free(packet);free(output);
    MVD_CLOSE(sock);
    return 0;
}
static int mvd_client_thread(void *arg) {
    mvd_client_channel *ch=(mvd_client_channel *)arg;
    unsigned channel=(unsigned)(ch-&g_mvd_client.channels[0]);
    while(!atomic_load_explicit(&ch->stop,memory_order_acquire)) {
        if(mvd_visible(ch)) {
            uint32_t ctl=atomic_load(&ch->controls)&MVD_DSP_MASK;
            (void)mvd_receive(channel,ch,mvd_ctl_get(ctl,MVD_CTL_MODE,3u),ctl);
        }
        for(int i=0;i<40 && !atomic_load(&ch->stop);i++)thrd_sleep_ms(10);
    }
    return 0;
}
/* HTTP poll runs outside the render and the H.264 receiver threads.
 * It fetches real, server-owned DSP metrics once per second, never sampling
 * local RF or decoding analogue video on Monitor Client. */
static void mvd_poll_one(unsigned channel) {
    mvd_client_channel *ch=&g_mvd_client.channels[channel];
    if(!mvd_visible(ch))return;
    uint32_t ctl=atomic_load(&ch->controls);
    bool rf=mvd_ctl_get(ctl,MVD_CTL_MODE,3u)>=2;
    unsigned detail=rf?(2u-mvd_ctl_get(ctl,MVD_CTL_STATS,3u))
                      :mvd_ctl_get(ctl,MVD_CTL_OSD,3u);
    if(detail==0){
        if(!atomic_flag_test_and_set_explicit(&ch->stats_busy,memory_order_acquire)){
            ch->stats_text[0]=0;
            atomic_flag_clear_explicit(&ch->stats_busy,memory_order_release);
        }
        return;
    }
    mvd_socket sock=mvd_connect(g_mvd_client.host,g_mvd_client.port);
    if(sock==MVD_SOCKET_BAD)return;
    char request[180];
    int n=snprintf(request,sizeof(request),
        "GET /monitor/video/osd?channel=%u&detail=%u HTTP/1.0\r\n"
        "Connection: close\r\n\r\n",channel,detail);
    if(n<=0 || (size_t)n>=sizeof(request) ||
       mvd_send_full(sock,request,(size_t)n)!=0){MVD_CLOSE(sock);return;}
    char response[1536];
    size_t used=0;
    while(used+1<sizeof(response) && !atomic_load(&g_mvd_client.stats_stop)){
        int count=recv(sock,response+used,(int)(sizeof(response)-used-1),0);
        if(count<=0)break;
        used+=(size_t)count;
    }
    MVD_CLOSE(sock);
    response[used]=0;
    if(strncmp(response,"HTTP/1.0 200 ",13) &&
       strncmp(response,"HTTP/1.1 200 ",13))return;
    char *body=strstr(response,"\r\n\r\n");
    if(!body)return;
    body+=4;
    if(!atomic_flag_test_and_set_explicit(&ch->stats_busy,memory_order_acquire)){
        snprintf(ch->stats_text,sizeof(ch->stats_text),"%s",body);
        atomic_flag_clear_explicit(&ch->stats_busy,memory_order_release);
    }
}
static int mvd_stats_thread(void *argument) {
    (void)argument;
    while(!atomic_load(&g_mvd_client.stats_stop)) {
        if(atomic_load(&g_mvd_client.active))
            for(unsigned channel=0;channel<2;channel++)mvd_poll_one(channel);
        for(unsigned i=0;i<10 && !atomic_load(&g_mvd_client.stats_stop);i++)
            thrd_sleep_ms(100);
    }
    return 0;
}

static void mvd_draw_stats(mvd_client_channel *ch,Rectangle bounds,uint32_t ctl) {
    bool rf=mvd_ctl_get(ctl,MVD_CTL_MODE,3u)>=2;
    unsigned detail=rf?(2u-mvd_ctl_get(ctl,MVD_CTL_STATS,3u)):
                       mvd_ctl_get(ctl,MVD_CTL_OSD,3u);
    if(!detail || ch->chrome_hidden)return;
    char message[1024];
    if(atomic_flag_test_and_set_explicit(&ch->stats_busy,memory_order_acquire))return;
    snprintf(message,sizeof(message),"%s",ch->stats_text);
    atomic_flag_clear_explicit(&ch->stats_busy,memory_order_release);
    if(!message[0])return;
    char frame_info[120];
    snprintf(frame_info,sizeof(frame_info),"Display %.1f fps | H.264 monitor video",
             ch->display_fps);
    char *lines[8]={0};
    unsigned n=0;lines[n++]=frame_info;
    lines[n++]=message;
    for(char *p=message;*p && n<8;p++)if(*p=='\n'){*p=0;lines[n++]=p+1;}
    float line_h=20.f,base=bounds.y+bounds.height-8.f-(float)n*line_h;
    if(base<bounds.y+8.f)base=bounds.y+8.f;
    DrawRectangleRec((Rectangle){bounds.x+6.f,base-3.f,bounds.width-12.f,
        bounds.y+bounds.height-base-5.f},(Color){0,0,0,116});
    for(unsigned i=0;i<n;i++){
        float y=base+(float)i*line_h;
        if(y>bounds.y+bounds.height-20.f)break;
        char *line=lines[i];
        /* Keep long OSD lines inside split panels. */
        while(*line && gui_text_measure_mono(line,14)>bounds.width-24.f){
            size_t len=strlen(line);
            if(!len)break;
            line[len-1]=0;
        }
        gui_text_draw_mono(line,bounds.x+10.f,y,14,(Color){255,230,120,255});
    }
}

int gui_monitor_video_client_start(const char *host,uint16_t port) {
    if(!host || !*host || !port || !gui_mvd_codec_available())return -1;
    if(atomic_load(&g_mvd_client.active))gui_monitor_video_client_stop();
    snprintf(g_mvd_client.host,sizeof(g_mvd_client.host),"%s",host);
    g_mvd_client.port=port;
    for(unsigned i=0;i<2;i++){
        mvd_client_channel *ch=&g_mvd_client.channels[i];
        if(!ch->pixels)ch->pixels=malloc(MVD_PIXEL_CAP);
        if(!ch->pixels)return -1;
        atomic_flag_clear(&ch->pixel_busy);
        atomic_store(&ch->mode,0);
        atomic_store(&ch->controls,mvd_ctl_default(0));
        ch->cvbs_settings=mvd_ctl_default(0);
        ch->rf_settings=mvd_ctl_default(2);
        ch->chrome_hidden=false;ch->last_click=0;ch->popup=0;
        ch->display_frame_count=0;ch->display_window_started=0;ch->display_fps=0;
        atomic_flag_clear(&ch->stats_busy);
        ch->stats_text[0]=0;
        atomic_store(&ch->video_http_status,0);
        atomic_store(&ch->seen_seconds,0);
        atomic_store(&ch->stop,false);
        if(thrd_create(&ch->worker,mvd_client_thread,ch)!=thrd_success){
            for(unsigned j=0;j<i;j++){
                atomic_store(&g_mvd_client.channels[j].stop,true);
                thrd_join(g_mvd_client.channels[j].worker,NULL);
            }
            return -1;
        }
        atomic_store(&ch->active,true);
    }
    atomic_store(&g_mvd_client.global_budget,2);
    atomic_store(&g_mvd_client.active,true);
    atomic_store(&g_mvd_client.stats_stop,false);
    g_mvd_client.stats_started=(thrd_create(&g_mvd_client.stats_worker,mvd_stats_thread,NULL)==thrd_success);
    return 0;
}
void gui_monitor_video_client_stop(void) {
    if(!atomic_exchange(&g_mvd_client.active,false))return;
    atomic_store(&g_mvd_client.stats_stop,true);
    for(unsigned c=0;c<2;c++) atomic_store(&g_mvd_client.channels[c].stop,true);
    if(g_mvd_client.stats_started){
        thrd_join(g_mvd_client.stats_worker,NULL);
        g_mvd_client.stats_started=false;
    }
    for(unsigned c=0;c<2;c++){
        mvd_client_channel *ch=&g_mvd_client.channels[c];
        if(atomic_exchange(&ch->active,false))thrd_join(ch->worker,NULL);
        atomic_store(&ch->seen_seconds,0);
        /* Pixel buffers and textures are UI-owned across reconnects;
         * freeing OpenGL resources is only safe on the render thread. */
    }
}
/* Monitor owns only UI chrome and H.264 display. Every setting that affects
 * DSP is sent as an explicit, validated server control word. Do not add a
 * separate local CVBS/VHS decode path. */
static bool mvd_option_button(Rectangle panel,float *y,const char *label,Vector2 mouse,bool click) {
    float w=(float)gui_text_measure(label,14)+16.f;
    if(w<118.f)w=118.f;
    if(w>panel.width-16.f)w=panel.width-16.f;
    if(w<0)return false;
    Rectangle r={panel.x+panel.width-w-8.f,*y,w,18.f};
    DrawRectangleRounded(r,0.15f,4,(Color){36,46,57,235});
    gui_text_draw(label,r.x+8.f,r.y+2.f,14,WHITE);
    *y+=24.f;
    return click && CheckCollisionPointRec(mouse,r);
}
static bool mvd_dropdown(Rectangle panel,float button_y,const char *label,
                          const char *const *names,unsigned n,Vector2 mouse,
                          bool click,unsigned *value) {
    float w=(float)gui_text_measure(label,14)+16.f;
    if(w<118.f)w=118.f;
    if(w>panel.width-16.f)w=panel.width-16.f;
    if(w<=0.f)return false;
    float y=button_y+18.f;
    if(y+20.f*n>panel.y+panel.height-8.f)y=button_y-20.f*n;
    for(unsigned i=0;i<n;i++) {
        Rectangle rect={panel.x+panel.width-w-8.f,y+20.f*i,w,20.f};
        DrawRectangleRec(rect,i==*value?(Color){65,84,104,248}:(Color){28,37,48,248});
        gui_text_draw(names[i],rect.x+8.f,rect.y+3.f,14,WHITE);
        if(click && CheckCollisionPointRec(mouse,rect)){*value=i;return true;}
    }
    return false;
}
void gui_monitor_video_client_draw(unsigned channel,Rectangle bounds) {
    if(channel>1)return;
    mvd_client_channel *ch=&g_mvd_client.channels[channel];
    atomic_store_explicit(&ch->seen_seconds,(uint64_t)time(NULL),memory_order_release);
    uint32_t ctl=atomic_load_explicit(&ch->controls,memory_order_acquire);
    bool rf=mvd_ctl_get(ctl,MVD_CTL_MODE,3u)>=2;
    DrawRectangleRec(bounds,(Color){18,18,20,255});
    if(!ch->scratch)ch->scratch=malloc(MVD_PIXEL_CAP);
    if(ch->scratch && !atomic_flag_test_and_set_explicit(&ch->pixel_busy,memory_order_acquire)){
        unsigned w=ch->width,h=ch->height;
        uint64_t generation=ch->generation;
        if(generation && generation!=ch->drawn_generation &&
           w>=2 && h>=2 && w<=MVD_MAX_WIDTH && h<=MVD_MAX_HEIGHT){
            memcpy(ch->scratch,ch->pixels,(size_t)w*h*4);
            ch->drawn_generation=generation;
            ch->display_frame_count++;
            atomic_flag_clear_explicit(&ch->pixel_busy,memory_order_release);
            if(ch->texture.id && (w!=ch->texture_width || h!=ch->texture_height)){
                UnloadTexture(ch->texture);memset(&ch->texture,0,sizeof(ch->texture));
            }
            if(!ch->texture.id){
                Image img={ch->scratch,(int)w,(int)h,1,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                ch->texture=LoadTextureFromImage(img);
                ch->texture_width=w;ch->texture_height=h;
            }else UpdateTexture(ch->texture,ch->scratch);
        }else atomic_flag_clear_explicit(&ch->pixel_busy,memory_order_release);
    }
    { double now=GetTime();
      if(ch->display_window_started<=0)ch->display_window_started=now;
      if(now-ch->display_window_started>=1.0){
          ch->display_fps=ch->display_frame_count/(now-ch->display_window_started);
          ch->display_frame_count=0;
          ch->display_window_started=now;
      }
    }
    if(ch->texture.id){
        float scale=bounds.width/(float)ch->texture_width;
        float vertical=bounds.height/(float)ch->texture_height;
        if(vertical<scale)scale=vertical;
        float dw=(float)ch->texture_width*scale,dh=(float)ch->texture_height*scale;
        Rectangle dst={bounds.x+(bounds.width-dw)*0.5f,
                       bounds.y+(bounds.height-dh)*0.5f,dw,dh};
        DrawTexturePro(ch->texture,
           (Rectangle){0,0,(float)ch->texture_width,(float)ch->texture_height},
           dst,(Vector2){0,0},0,WHITE);
    }else{
        int status=atomic_load(&ch->video_http_status);
        const char *message = !gui_mvd_codec_available() ?
            "OpenH264 unavailable in this client build" :
            status==503 ? "Server OpenH264 unavailable: rebuild headless server" :
            status==404 ? "Server lacks /monitor/video: update headless server" :
            status==409 ? "Server video channel busy: disconnect other viewer" :
            status<0 ? "H.264 video connection failed: check server and port" :
            status==200 ? "Connected: waiting for decoded video frames..." :
            "Connecting to H.264 video preview...";
        gui_text_draw(message,bounds.x+12,bounds.y+50,14,LIGHTGRAY);
    }
    Vector2 mouse=gui_ui_get_mouse_position();
    bool click=IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool used=false,changed=false;
    char label[96];float y=bounds.y+8.f;
    static const char *const systems[]={"PAL","NTSC","SECAM"};
    if(!ch->chrome_hidden){
        if(mvd_option_button(bounds,&y,rf?"Decoder: RF Preview":"Decoder: CVBS",mouse,click)){
            if(rf){
                ch->rf_settings=ctl;ctl=ch->cvbs_settings;
            }else{
                ch->cvbs_settings=ctl;ctl=ch->rf_settings;
            }
            rf=!rf;used=changed=true;ch->popup=0;
        }
        if(rf){
            unsigned tape=mvd_ctl_get(ctl,MVD_CTL_TAPE,1u);
            if(mvd_option_button(bounds,&y,tape?"Tape: Betamax":"Tape: VHS",mouse,click)){
                ctl=mvd_ctl_set(ctl,MVD_CTL_TAPE,1u,!tape);
                if(!tape)ctl=mvd_ctl_set(ctl,MVD_CTL_SYSTEM,3u,1);
                used=changed=true;
            }
            unsigned system=mvd_ctl_get(ctl,MVD_CTL_SYSTEM,3u);
            snprintf(label,sizeof(label),tape?"NTSC (fixed)":"System: %s",systems[system]);
            if(mvd_option_button(bounds,&y,label,mouse,click) && !tape){
                ctl=mvd_ctl_set(ctl,MVD_CTL_SYSTEM,3u,(system+1)%3);
                used=changed=true;
            }
            if(!tape && system==2){
                unsigned mesecam=mvd_ctl_get(ctl,MVD_CTL_MESECAM,1u);
                if(mvd_option_button(bounds,&y,mesecam?"MESECAM: On":"MESECAM: Off",mouse,click)){
                    ctl=mvd_ctl_set(ctl,MVD_CTL_MESECAM,1u,!mesecam);
                    used=changed=true;
                }
            }
            if(tape){
                static const char *const beta[]={"Betamax","Betamax Hi-Fi","SuperBeta","ED Beta"};
                unsigned preset=mvd_ctl_get(ctl,MVD_CTL_BETA,3u);
                snprintf(label,sizeof(label),"Preset: %s",beta[preset]);
                if(mvd_option_button(bounds,&y,label,mouse,click)){
                    ctl=mvd_ctl_set(ctl,MVD_CTL_BETA,3u,(preset+1)%4);
                    used=changed=true;
                }
            }
            unsigned full=mvd_ctl_get(ctl,MVD_CTL_FRAME,1u);
            if(mvd_option_button(bounds,&y,full?"Frame: Full":"Frame: Active",mouse,click)){
                ctl=mvd_ctl_set(ctl,MVD_CTL_FRAME,1u,!full);
                used=changed=true;
            }
            unsigned budget=mvd_ctl_get(ctl,MVD_CTL_BUDGET,7u);
            snprintf(label,sizeof(label),"Budget: %.2f core (shared)",mvd_ctl_budget(ctl)/100.0);
            if(mvd_option_button(bounds,&y,label,mouse,click)){
                unsigned next=(budget+1)%6;
                atomic_store(&g_mvd_client.global_budget,next);
                /* A shared budget must be reflected in both channel
                 * requests, never applied twice as independent settings. */
                for(unsigned other=0;other<2;other++){
                    mvd_client_channel *peer=&g_mvd_client.channels[other];
                    uint32_t setting=atomic_load(&peer->controls);
                    setting=mvd_ctl_set(setting,MVD_CTL_BUDGET,7u,next);
                    atomic_store_explicit(&peer->controls,setting,memory_order_release);
                }
                ctl=mvd_ctl_set(ctl,MVD_CTL_BUDGET,7u,next);
                used=changed=true;
            }
            static const char *const stats[]={"All","Basic","Off"};
            unsigned view=mvd_ctl_get(ctl,MVD_CTL_STATS,3u);
            snprintf(label,sizeof(label),"Stats: %s",stats[view]);
            float menu_y=y;
            if(mvd_option_button(bounds,&y,label,mouse,click)){
                ch->popup=ch->popup==2?0:2;used=true;
            }
            if(ch->popup==2 && mvd_dropdown(bounds,menu_y,label,stats,3,mouse,click,&view)){
                ctl=mvd_ctl_set(ctl,MVD_CTL_STATS,3u,view);
                ch->popup=0;used=changed=true;
            }
        }else{
            static const char *const osd[]={"Off","Minimal","Stats"};
            unsigned value=mvd_ctl_get(ctl,MVD_CTL_OSD,3u);
            snprintf(label,sizeof(label),"OSD: %s",osd[value]);
            if(mvd_option_button(bounds,&y,label,mouse,click)){
                ctl=mvd_ctl_set(ctl,MVD_CTL_OSD,3u,(value+1)%3);
                used=changed=true;
            }
            static const unsigned bits[]={MVD_CTL_FRAME,MVD_CTL_LEVELS,MVD_CTL_SYNC,MVD_CTL_COLOUR};
            static const char *const keys[]={"Frame","Levels","Sync","Colour"};
            static const char *const low[]={"Active","Auto","Auto","Off"};
            static const char *const high[]={"Full","Fixed","Fixed","Auto"};
            for(unsigned i=0;i<4;i++){
                unsigned val=mvd_ctl_get(ctl,bits[i],1u);
                snprintf(label,sizeof(label),"%s: %s",keys[i],val?high[i]:low[i]);
                if(mvd_option_button(bounds,&y,label,mouse,click)){
                    ctl=mvd_ctl_set(ctl,bits[i],1u,!val);
                    used=changed=true;
                }
            }
            unsigned system=mvd_ctl_get(ctl,MVD_CTL_SYSTEM,3u);
            float menu_y=y;
            if(mvd_option_button(bounds,&y,systems[system],mouse,click)){
                ch->popup=ch->popup==1?0:1;used=true;
            }
            if(ch->popup==1 && mvd_dropdown(bounds,menu_y,systems[system],systems,3,mouse,click,&system)){
                ctl=mvd_ctl_set(ctl,MVD_CTL_SYSTEM,3u,system);
                ch->popup=0;used=changed=true;
            }
        }
    }
    if(click && !used && CheckCollisionPointRec(mouse,bounds)){
        double now=GetTime();
        if(ch->last_click>0 && now-ch->last_click<0.36){
            ch->chrome_hidden=!ch->chrome_hidden;ch->last_click=0;ch->popup=0;
        }else ch->last_click=now;
    }else if(used)ch->last_click=0;
    mvd_draw_stats(ch,bounds,ctl);
    if(changed){
        ctl=mvd_ctl_set(ctl,MVD_CTL_BUDGET,7u,
                        atomic_load(&g_mvd_client.global_budget));
    }
    if(changed && mvd_ctl_valid(ctl,mvd_ctl_get(ctl,MVD_CTL_MODE,3u))){
        atomic_store_explicit(&ch->controls,ctl,memory_order_release);
        atomic_store(&ch->mode,mvd_ctl_get(ctl,MVD_CTL_MODE,3u));
        if(rf)ch->rf_settings=ctl;else ch->cvbs_settings=ctl;
        atomic_store(&ch->video_http_status,0);
    }
}
