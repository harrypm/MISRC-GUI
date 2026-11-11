#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production native staging reader with independent fake clients.

No hardware, window, CoreAudio, or DMA. Covers bytes/order, partial reads,
not-ready, failure propagation, and cross-card state isolation.
"""
import pathlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
source=(root/'misrc_gui/input/gui_cxadc_macos.c').read_text()
start=source.index('int gui_cxadc_macos_read(');opening=source.index('{',start);end=opening+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
function=source[start:end]
fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#define CXADC_CLIENT_CHUNK_BYTES 262144
static int capture_count=2;
static int first=0,second=1;
static int *clients[2]={&first,&second};
static atomic_bool active=true;
static bool dma_started[2]={false,false};
static unsigned char staging[2][CXADC_CLIENT_CHUNK_BYTES];
static size_t offset[2],available[2];
static size_t position[2];
static int stall=-1,broken=-1;
static int fake_start(int *c){(void)c;return 0;}
static unsigned char value(int card,size_t i){return (unsigned char)((i+card*73)%256);}
static int fake_read(int *c,void *out,size_t capacity,size_t *bytes){
    assert(capacity==CXADC_CLIENT_CHUNK_BYTES);
    if(*c==broken)return -1;
    if(*c==stall){*bytes=0;return 0;}
    for(size_t i=0;i<capacity;i++)((unsigned char*)out)[i]=value(*c,position[*c]+i);
    position[*c]+=capacity;*bytes=capacity;return 0;
}
static struct {int (*start)(int*);int (*read)(int*,void*,size_t,size_t*);} table={fake_start,fake_read},*api=&table;
static int failed(void){return -1;}
'''
checks=r'''
int main(void){
 unsigned char out[65536];size_t consumed[2]={0,0};
 for(int n=0;n<40;n++)for(int c=0;c<2;c++) {
  size_t request=(n%3==0)?137:sizeof(out);
  int count=gui_cxadc_macos_read(c,out,request);assert(count>0 && count<=(int)request);
  for(int i=0;i<count;i++)assert(out[i]==value(c,consumed[c]+i));consumed[c]+=count;
 }
 assert(gui_cxadc_macos_read(-1,out,sizeof(out))==-1);
 assert(gui_cxadc_macos_read(2,out,sizeof(out))==-1);
 offset[1]=available[1]=0;stall=1;assert(gui_cxadc_macos_read(1,out,sizeof(out))==0);
 int count=gui_cxadc_macos_read(0,out,sizeof(out));assert(count>0);
 for(int i=0;i<count;i++)assert(out[i]==value(0,consumed[0]+i));
 stall=-1;broken=1;assert(gui_cxadc_macos_read(1,out,sizeof(out))==-1);
 active=false;assert(gui_cxadc_macos_read(0,out,sizeof(out))==-1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='cxadc-stream-') as directory:
 p=pathlib.Path(directory);(p/'test.c').write_text(fixture+function+checks)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('native dual CXADC staging/order/error checks: PASS')
