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
} mvd_client_channel;
static struct {
    char host[128];uint16_t port;
    mvd_client_channel channels[2];
    atomic_bool active;
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
          mvd_visible(ch) && atomic_load(&ch->controls)==controls) {
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
            uint32_t ctl=atomic_load(&ch->controls);
            (void)mvd_receive(channel,ch,mvd_ctl_get(ctl,MVD_CTL_MODE,3u),ctl);
        }
        for(int i=0;i<40 && !atomic_load(&ch->stop);i++)thrd_sleep_ms(10);
    }
    return 0;
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
    atomic_store(&g_mvd_client.active,true);
    return 0;
}
void gui_monitor_video_client_stop(void) {
    if(!atomic_exchange(&g_mvd_client.active,false))return;
    for(unsigned c=0;c<2;c++) atomic_store(&g_mvd_client.channels[c].stop,true);
    for(unsigned c=0;c<2;c++){
        mvd_client_channel *ch=&g_mvd_client.channels[c];
        if(atomic_exchange(&ch->active,false))thrd_join(ch->worker,NULL);
        atomic_store(&ch->seen_seconds,0);
        /* Pixel buffers and textures are UI-owned across reconnects;
         * freeing OpenGL resources is only safe on the render thread. */
    }
}
void gui_monitor_video_client_draw(unsigned channel,Rectangle bounds) {
    if(channel>1)return;
    mvd_client_channel *ch=&g_mvd_client.channels[channel];
    atomic_store_explicit(&ch->seen_seconds,(uint64_t)time(NULL),memory_order_release);
    const char *modes[]={"CVBS","Tape Decode","VHS PAL","VHS NTSC"};
    unsigned mode=atomic_load(&ch->mode);
    if(mode>3)mode=0;
    Rectangle selector={bounds.x+bounds.width-132,bounds.y+8,124,27};
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
       CheckCollisionPointRec(gui_ui_get_mouse_position(),selector)) {
        mode=(mode+1)%4;
        atomic_store(&ch->mode,mode);
        ch->drawn_generation=0;
        atomic_store(&ch->video_http_status,0);
    }
    DrawRectangleRec(bounds,(Color){18,18,20,255});
    if(!ch->scratch)ch->scratch=malloc(MVD_PIXEL_CAP);
    if(ch->scratch && !atomic_flag_test_and_set_explicit(&ch->pixel_busy,memory_order_acquire)){
        unsigned w=ch->width,h=ch->height;
        uint64_t generation=ch->generation;
        if(generation && generation!=ch->drawn_generation &&
           w>=2 && h>=2 && w<=MVD_MAX_WIDTH && h<=MVD_MAX_HEIGHT){
            memcpy(ch->scratch,ch->pixels,(size_t)w*h*4);
            ch->drawn_generation=generation;
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
    DrawRectangleRec(selector,(Color){35,43,54,235});
    gui_text_draw(modes[mode],selector.x+8,selector.y+6,14,WHITE);
}
