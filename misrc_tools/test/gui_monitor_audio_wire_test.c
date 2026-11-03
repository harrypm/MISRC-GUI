#include "../misrc_gui/net/gui_monitor_audio_wire.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t h[MA_WIRE_HEADER]={0};
    uint32_t seq=0;
    size_t n=0;
    assert(ma_wire_pack(h,0x10203040u,88));
    assert(ma_wire_unpack(h,&seq,&n));
    assert(seq==0x10203040u && n==88);
    assert(h[0]=='M' && h[1]=='A' && h[2]=='O' && h[3]=='1');
    assert(h[10]==0 && h[11]==0);
    assert(ma_wire_pack(h,UINT32_MAX,MA_WIRE_MAX_PAYLOAD));
    assert(ma_wire_unpack(h,&seq,&n) && seq==UINT32_MAX && n==1275);
    assert(!ma_wire_pack(h,0,0));
    assert(!ma_wire_pack(h,0,1276));
    h[0]='X';
    assert(!ma_wire_unpack(h,&seq,&n));
    h[0]='M';h[10]=1;
    assert(!ma_wire_unpack(h,&seq,&n));
    puts("Monitor Opus wire protocol tests passed");
    return 0;
}
