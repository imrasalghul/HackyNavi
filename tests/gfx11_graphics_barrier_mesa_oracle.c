// SPDX-License-Identifier: MIT
// Calls the REAL pinned Mesa encoder in the existing amd_common archive.
// This only produces command bytes in host memory. No device/backend exists.
#include "ac_barrier.h"
#include "ac_cmdbuf_base.h"
#include <stdio.h>
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    uint32_t words[64]={0};
    struct ac_cmdbuf commands={.max_dw=64,.reserved_dw=64,.buf=words};
    const struct ac_barrier_state state={
        .flags=AC_BARRIER_SYNC_AND_INV_CB|AC_BARRIER_SYNC_AND_INV_DB|
            AC_BARRIER_INV_L2|AC_BARRIER_PFP_SYNC_ME,
        .pws_acquire_point=AC_PWS_ACQUIRE_POINT_PFP
    };
    bool context_roll=false;
    enum ac_rgp_flush_bits flush=0;
    ac_emit_barrier(&commands,GFX11,AMD_IP_GFX,&state,&context_roll,&flush);
    if(commands.cdw!=16||context_roll)return 3;
    FILE* file=fopen(argv[1],"wb");if(!file)return 4;
    const size_t written=fwrite(words,sizeof(uint32_t),commands.cdw,file);
    return fclose(file)!=0||written!=commands.cdw?5:0;
}
