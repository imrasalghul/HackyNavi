// SPDX-License-Identifier: MIT
// Named GFX11 state from Mesa35b085c4c067f198ce0b6437d94678e47b9c645f.
// Full field/source contract: docs/gfx11-graphics-draw-contract.md.
// This builds CPU bytes only. Native capture/retention/topology are mandatory.
// Source-derived portions: Copyright 2012 Advanced Micro Devices, Inc.
// Copyright 2024 Advanced Micro Devices, Inc. Copyright 2026 Valve Corporation.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include "hackynavi/gfx11_graphics_draw.hpp"
#include "hackynavi/gfx11_graphics_barrier.hpp"
namespace hackynavi { namespace gfx11draw { namespace {
constexpr uint64_t kVaEnd=UINT64_C(1)<<47;
// Exact GFX11 byte register addresses; names disambiguate generation aliases.
enum Register : uint32_t {
    SPI_SHADER_PGM_RSRC4_PS=0x00b004,
    SPI_SHADER_PGM_RSRC3_PS=0x00b01c,
    SPI_SHADER_PGM_LO_PS=0x00b020,
    SPI_SHADER_PGM_HI_PS=0x00b024,
    SPI_SHADER_PGM_RSRC1_PS=0x00b028,
    SPI_SHADER_PGM_RSRC2_PS=0x00b02c,
    SPI_SHADER_USER_DATA_PS_0=0x00b030,
    SPI_SHADER_REQ_CTRL_PS=0x00b0c0,
    SPI_SHADER_USER_ACCUM_PS_0=0x00b0c8,
    SPI_SHADER_USER_ACCUM_PS_1=0x00b0cc,
    SPI_SHADER_USER_ACCUM_PS_2=0x00b0d0,
    SPI_SHADER_USER_ACCUM_PS_3=0x00b0d4,
    SPI_SHADER_PGM_RSRC4_GS=0x00b204,
    SPI_SHADER_PGM_RSRC3_GS=0x00b21c,
    SPI_SHADER_PGM_RSRC1_GS=0x00b228,
    SPI_SHADER_PGM_RSRC2_GS=0x00b22c,
    SPI_SHADER_USER_DATA_GS_0=0x00b230,
    SPI_SHADER_USER_ACCUM_ESGS_0=0x00b2c8,
    SPI_SHADER_USER_ACCUM_ESGS_1=0x00b2cc,
    SPI_SHADER_USER_ACCUM_ESGS_2=0x00b2d0,
    SPI_SHADER_USER_ACCUM_ESGS_3=0x00b2d4,
    SPI_SHADER_PGM_LO_ES=0x00b320,
    SPI_SHADER_PGM_HI_ES=0x00b324,
    SPI_SHADER_PGM_RSRC3_HS=0x00b41c,
    SPI_SHADER_USER_ACCUM_LSHS_0=0x00b4c8,
    SPI_SHADER_USER_ACCUM_LSHS_1=0x00b4cc,
    SPI_SHADER_USER_ACCUM_LSHS_2=0x00b4d0,
    SPI_SHADER_USER_ACCUM_LSHS_3=0x00b4d4,
    SPI_SHADER_PGM_HI_LS=0x00b524,
    DB_RENDER_CONTROL=0x028000,
    DB_COUNT_CONTROL=0x028004,
    DB_RENDER_OVERRIDE=0x02800c,
    DB_RENDER_OVERRIDE2=0x028010,
    PA_SC_SCREEN_SCISSOR_TL=0x028030,
    PA_SC_SCREEN_SCISSOR_BR=0x028034,
    DB_Z_INFO=0x028040,
    DB_STENCIL_INFO=0x028044,
    DB_RMI_L2_CACHE_CONTROL=0x02807c,
    TA_BC_BASE_ADDR=0x028080,
    TA_BC_BASE_ADDR_HI=0x028084,
    PA_SC_WINDOW_OFFSET=0x028200,
    PA_SC_WINDOW_SCISSOR_TL=0x028204,
    PA_SC_WINDOW_SCISSOR_BR=0x028208,
    PA_SC_CLIPRECT_RULE=0x02820c,
    PA_SC_EDGERULE=0x028230,
    PA_SU_HARDWARE_SCREEN_OFFSET=0x028234,
    CB_TARGET_MASK=0x028238,
    CB_SHADER_MASK=0x02823c,
    PA_SC_GENERIC_SCISSOR_TL=0x028240,
    PA_SC_GENERIC_SCISSOR_BR=0x028244,
    PA_SC_VPORT_SCISSOR_0_TL=0x028250,
    PA_SC_VPORT_SCISSOR_0_BR=0x028254,
    PA_SC_VPORT_ZMIN_0=0x0282d0,
    PA_SC_VPORT_ZMAX_0=0x0282d4,
    PA_SC_TILE_STEERING_OVERRIDE=0x02835c,
    PA_SC_VRS_OVERRIDE_CNTL=0x0283d0,
    PA_SC_VRS_RATE_CACHE_CNTL=0x0283e4,
    CB_RMI_GL2_CACHE_CONTROL=0x028410,
    CB_FDCC_CONTROL=0x028424,
    CB_COVERAGE_OUT_CONTROL=0x028428,
    DB_STENCIL_CONTROL=0x02842c,
    PA_CL_VPORT_XSCALE=0x02843c,
    SPI_VS_OUT_CONFIG=0x0286c4,
    SPI_PS_INPUT_ENA=0x0286cc,
    SPI_PS_INPUT_ADDR=0x0286d0,
    SPI_INTERP_CONTROL_0=0x0286d4,
    SPI_PS_IN_CONTROL=0x0286d8,
    SPI_BARYC_SSAA_CNTL=0x0286dc,
    SPI_BARYC_CNTL=0x0286e0,
    SPI_SHADER_IDX_FORMAT=0x028708,
    SPI_SHADER_POS_FORMAT=0x02870c,
    SPI_SHADER_Z_FORMAT=0x028710,
    SPI_SHADER_COL_FORMAT=0x028714,
    SX_PS_DOWNCONVERT_CONTROL=0x028750,
    SX_PS_DOWNCONVERT=0x028754,
    SX_BLEND_OPT_EPSILON=0x028758,
    SX_BLEND_OPT_CONTROL=0x02875c,
    SX_MRT0_BLEND_OPT=0x028760,
    CB_BLEND0_CONTROL=0x028780,
    PA_CL_POINT_X_RAD=0x0287d4,
    PA_CL_POINT_Y_RAD=0x0287d8,
    PA_CL_POINT_SIZE=0x0287dc,
    PA_CL_POINT_CULL_RAD=0x0287e0,
    GE_MAX_OUTPUT_PER_SUBGROUP=0x0287fc,
    DB_DEPTH_CONTROL=0x028800,
    DB_EQAA=0x028804,
    CB_COLOR_CONTROL=0x028808,
    DB_SHADER_CONTROL=0x02880c,
    PA_CL_CLIP_CNTL=0x028810,
    PA_SU_SC_MODE_CNTL=0x028814,
    PA_CL_VTE_CNTL=0x028818,
    PA_CL_VS_OUT_CNTL=0x02881c,
    PA_CL_NANINF_CNTL=0x028820,
    PA_SU_LINE_STIPPLE_CNTL=0x028824,
    PA_SU_LINE_STIPPLE_SCALE=0x028828,
    PA_SU_PRIM_FILTER_CNTL=0x02882c,
    PA_SU_SMALL_PRIM_FILTER_CNTL=0x028830,
    PA_CL_NGG_CNTL=0x028838,
    PA_SU_OVER_RASTERIZATION_CNTL=0x02883c,
    PA_STEREO_CNTL=0x028840,
    PA_CL_VRS_CNTL=0x028848,
    PA_SU_POINT_SIZE=0x028a00,
    PA_SU_POINT_MINMAX=0x028a04,
    PA_SU_LINE_CNTL=0x028a08,
    PA_SC_LINE_STIPPLE=0x028a0c,
    VGT_HOS_MAX_TESS_LEVEL=0x028a18,
    VGT_HOS_MIN_TESS_LEVEL=0x028a1c,
    PA_SC_MODE_CNTL_0=0x028a48,
    PA_SC_MODE_CNTL_1=0x028a4c,
    VGT_ENHANCE=0x028a50,
    VGT_PRIMITIVEID_EN=0x028a84,
    VGT_PRIMITIVEID_RESET=0x028a8c,
    VGT_DRAW_PAYLOAD_CNTL=0x028a98,
    VGT_ESGS_RING_ITEMSIZE=0x028aac,
    VGT_REUSE_OFF=0x028ab4,
    GE_NGG_SUBGRP_CNTL=0x028b4c,
    VGT_TESS_DISTRIBUTION=0x028b50,
    VGT_SHADER_STAGES_EN=0x028b54,
    DB_ALPHA_TO_MASK=0x028b70,
    VGT_GS_INSTANCE_CNT=0x028b90,
    PA_SC_CENTROID_PRIORITY_0=0x028bd4,
    PA_SC_CENTROID_PRIORITY_1=0x028bd8,
    PA_SC_LINE_CNTL=0x028bdc,
    PA_SC_AA_CONFIG=0x028be0,
    PA_SU_VTX_CNTL=0x028be4,
    PA_CL_GB_VERT_CLIP_ADJ=0x028be8,
    PA_CL_GB_VERT_DISC_ADJ=0x028bec,
    PA_CL_GB_HORZ_CLIP_ADJ=0x028bf0,
    PA_CL_GB_HORZ_DISC_ADJ=0x028bf4,
    PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0=0x028bf8,
    PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0=0x028c08,
    PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0=0x028c18,
    PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0=0x028c28,
    PA_SC_AA_MASK_X0Y0_X1Y0=0x028c38,
    PA_SC_AA_MASK_X0Y1_X1Y1=0x028c3c,
    PA_SC_SHADER_CONTROL=0x028c40,
    PA_SC_BINNER_CNTL_0=0x028c44,
    PA_SC_BINNER_CNTL_1=0x028c48,
    PA_SC_CONSERVATIVE_RASTERIZATION_CNTL=0x028c4c,
    PA_SC_NGG_MODE_CNTL=0x028c50,
    PA_SC_BINNER_CNTL_2=0x028c54,
    CB_COLOR0_BASE=0x028c60,
    CB_COLOR0_VIEW=0x028c6c,
    CB_COLOR0_INFO=0x028c70,
    CB_COLOR0_DCC_BASE=0x028c94,
    CB_COLOR0_BASE_EXT=0x028e40,
    CB_COLOR0_DCC_BASE_EXT=0x028ea0,
    CB_COLOR0_ATTRIB2=0x028ec0,
    CB_COLOR0_ATTRIB3=0x028ee0,
    VGT_PRIMITIVE_TYPE=0x030908,
    GE_MIN_VTX_INDX=0x030924,
    GE_INDX_OFFSET=0x030928,
    GE_MULTI_PRIM_IB_RESET_EN=0x03092c,
    GE_MAX_VTX_INDX=0x030964,
    VGT_INSTANCE_BASE_ID=0x030968,
    GE_CNTL=0x03096c,
    GE_STEREO_CNTL=0x03097c,
    GE_PC_ALLOC=0x030980,
    GE_USER_VGPR_EN=0x030988,
    GE_VRS_RATE=0x03098c,
    VGT_GS_OUT_PRIM_TYPE=0x030998,
    PA_SU_LINE_STIPPLE_VALUE=0x030a00,
    PA_SC_LINE_STIPPLE_STATE=0x030a04,
    SPI_GS_THROTTLE_CNTL1=0x031110,
};
bool pointer(const void* p,size_t n,size_t alignment=1) {
    return p&&n&&!(reinterpret_cast<uintptr_t>(p)%alignment)&&n-1<=UINTPTR_MAX-reinterpret_cast<uintptr_t>(p);
}
bool overlaps(const void* a,size_t an,const void* b,size_t bn) {
    const uintptr_t av=reinterpret_cast<uintptr_t>(a),bv=reinterpret_cast<uintptr_t>(b);
    return av<=bv?bv-av<an:av-bv<bn;
}
bool inputAlias(const Request& r,const void* p,size_t bytes) {
    return overlaps(p,bytes,&r,sizeof(r))||overlaps(p,bytes,r.image.image,r.image.imageBytes);
}
Status binding(const Request& r,const gfxisolated::Binding& b,uint32_t access,uint64_t exact=0) {
    if(b.owner!=r.owner||b.epoch!=r.epoch)return Status::ForeignContext;
    if(!b.id||!b.generation||!b.bytes||b.gpu<4096||b.gpu>=kVaEnd||b.bytes>kVaEnd-b.gpu||
       ((b.gpu|b.bytes|b.bufferOffset|b.allocationBytes)&4095)||
       b.bufferOffset>b.allocationBytes||b.bytes>b.allocationBytes-b.bufferOffset||
       (exact&&(b.bytes!=exact||b.bufferOffset||b.allocationBytes!=exact)))return Status::InvalidBinding;
    return b.access==access?Status::Ok:Status::Permission;
}
const gfxisolated::Binding& bound(const Request& r,unsigned i) {
    return i==0?r.code:i==1?r.target:i==2?r.attributes:r.indirect;
}
uint32_t packet(uint32_t opcode,uint32_t count) {return 0xc0000000U|(count<<16)|(opcode<<8);}
struct Stream {
    uint32_t* output;const uint32_t* compare;size_t compareWords,count;bool mismatch;
    void add(uint32_t value) {
        if(count>=kMaxCommandDwords)mismatch=true;
        else {
            if(output)output[count]=value;
            if(compare&&(count>=compareWords||compare[count]!=value))mismatch=true;
        }
        ++count;
    }
    void reg(uint32_t address,uint32_t value) {
        const uint32_t base=address>=0x30000?0x30000:address>=0x28000?0x28000:0xb000;
        add(packet(base==0x30000?0x79:base==0x28000?0x69:0x76,1));add((address-base)/4);add(value);
    }
    void seq(uint32_t address,uint32_t registerCount) {
        const uint32_t base=address>=0x30000?0x30000:address>=0x28000?0x28000:0xb000;
        add(packet(base==0x30000?0x79:base==0x28000?0x69:0x76,registerCount));add((address-base)/4);
    }
    void pws(uint32_t event,uint32_t acquire) {
        add(packet(0x49,6));add(event);for(unsigned i=0;i<6;++i)add(0);
        add(packet(0x58,6));add(acquire);add(UINT32_MAX);add(0x01ffffff);
        add(0);add(0);add(0x80000000);add(0);
    }
};
// Exact binary32 bits of n/2 for the admitted integer dimensions1..16384.
uint32_t halfBits(uint32_t n) {
    unsigned e=0;for(uint32_t v=n;v>1;v>>=1)++e;
    return ((e+126)<<23)|((n-(1u<<e))<<(23-e));
}
void commonPreamble(const Request& r,const Workspace& w,Stream& s) {
    // ac_cmdbuf.c gfx10_init_graphics_preamble_state, exact GFX11 branch.
    s.reg(SPI_SHADER_PGM_RSRC3_PS,w.psCuMask|0x007f0000U);
    s.reg(SPI_SHADER_REQ_CTRL_PS,0x00000007U);
    s.reg(SPI_SHADER_USER_ACCUM_PS_0,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_PS_1,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_PS_2,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_PS_3,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_ESGS_0,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_ESGS_1,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_ESGS_2,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_ESGS_3,0x00000000U);
    s.reg(SPI_SHADER_PGM_HI_ES,w.address32Hi>>8);
    s.reg(SPI_SHADER_PGM_RSRC3_HS,0xffff003fU);
    s.reg(SPI_SHADER_USER_ACCUM_LSHS_0,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_LSHS_1,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_LSHS_2,0x00000000U);
    s.reg(SPI_SHADER_USER_ACCUM_LSHS_3,0x00000000U);
    s.reg(SPI_SHADER_PGM_HI_LS,w.address32Hi>>8);
    s.reg(PA_SC_SCREEN_SCISSOR_TL,0x00000000U);
    s.reg(PA_SC_GENERIC_SCISSOR_TL,0x80000000U);
    s.reg(PA_SC_GENERIC_SCISSOR_BR,0x40004000U);
    s.reg(PA_SC_TILE_STEERING_OVERRIDE,0x00000000U);
    s.reg(PA_SC_VRS_RATE_CACHE_CNTL,0x00000000U);
    s.reg(CB_COVERAGE_OUT_CONTROL,0x00000000U);
    s.reg(SPI_BARYC_SSAA_CNTL,0x00000000U);
    s.reg(PA_CL_POINT_X_RAD,0x00000000U);
    s.reg(PA_CL_POINT_Y_RAD,0x00000000U);
    s.reg(PA_CL_POINT_SIZE,0x00000000U);
    s.reg(PA_CL_POINT_CULL_RAD,0x00000000U);
    s.reg(PA_CL_NANINF_CNTL,0x00000000U);
    s.reg(PA_SU_LINE_STIPPLE_CNTL,0x00000000U);
    s.reg(PA_SU_OVER_RASTERIZATION_CNTL,0x00000000U);
    s.reg(PA_STEREO_CNTL,0x00000000U);
    s.reg(VGT_ENHANCE,0x00000000U);
    s.reg(VGT_PRIMITIVEID_RESET,0x00000000U);
    s.reg(VGT_REUSE_OFF,0x00000000U);
    s.reg(PA_SC_SHADER_CONTROL,0x00000000U);
    s.reg(DB_RMI_L2_CACHE_CONTROL,0x002a0055U);
    s.reg(TA_BC_BASE_ADDR,0x00000000U);
    s.reg(TA_BC_BASE_ADDR_HI,0x00000000U);
    s.reg(CB_RMI_GL2_CACHE_CONTROL,r.topology.maxRenderBackends<=4?0x00880004U:0x00aa0015U);
    s.reg(SX_PS_DOWNCONVERT_CONTROL,0x000000ffU);
    s.reg(PA_SU_SMALL_PRIM_FILTER_CNTL,0x00000001U);
    s.reg(VGT_HOS_MAX_TESS_LEVEL,0x42800000U);
    s.reg(VGT_HOS_MIN_TESS_LEVEL,0x00000000U);
    s.reg(VGT_ESGS_RING_ITEMSIZE,0x00000001U);
    s.reg(VGT_TESS_DISTRIBUTION,0xd8808080U);
    s.reg(PA_SC_BINNER_CNTL_1,0x03ff0010U);
    s.reg(PA_SC_BINNER_CNTL_2,0x00000000U);
    s.reg(PA_SC_NGG_MODE_CNTL,0x00000010U);
    s.reg(GE_MIN_VTX_INDX,0x00000000U);
    s.reg(GE_INDX_OFFSET,0x00000000U);
    s.reg(GE_MULTI_PRIM_IB_RESET_EN,0x00000004U);
    s.reg(GE_MAX_VTX_INDX,0xffffffffU);
    s.reg(VGT_INSTANCE_BASE_ID,0x00000000U);
    s.reg(GE_STEREO_CNTL,0x00000000U);
    s.reg(GE_USER_VGPR_EN,0x00000000U);
    s.reg(PA_SU_LINE_STIPPLE_VALUE,0x00000000U);
    s.reg(PA_SC_LINE_STIPPLE_STATE,0x00000000U);
    // PIXEL_PIPE_STAT_CONTROL: counter0, stride2, all maximum RB instances.
    const uint64_t rb=(UINT64_C(1)<<r.topology.maxRenderBackends)-1;
    s.add(packet(0x46,2));s.add(0x138);s.add(0x400U|uint32_t(rb<<11));s.add(uint32_t(rb>>21));
}
void fixedState(const Request& r,const Workspace& w,Stream& s) {
    // Named no-resource RADV shader/raster/sampling/CB/DB specializations.
    s.reg(SPI_VS_OUT_CONFIG,0x00000080U);
    s.reg(SPI_SHADER_PGM_RSRC3_GS,w.nggCuMask|0x003f0000U);
    s.reg(SPI_SHADER_PGM_RSRC4_GS,1U|(w.lateAlloc<<16)|(w.vertexPrefetch<<23));
    s.reg(GE_PC_ALLOC,w.lateAlloc?0x1ffU:0x7feU);
    s.reg(SPI_SHADER_IDX_FORMAT,0x00000001U);
    s.reg(SPI_SHADER_POS_FORMAT,0x00000004U);
    s.reg(PA_CL_VS_OUT_CNTL,0x00000000U);
    s.reg(VGT_PRIMITIVEID_EN,0x00000000U);
    s.reg(GE_MAX_OUTPUT_PER_SUBGROUP,0x00000080U);
    s.reg(GE_NGG_SUBGRP_CNTL,0x00000001U);
    s.reg(VGT_GS_INSTANCE_CNT,0x00000004U);
    s.reg(GE_CNTL,0x1f810080U);
    s.reg(DB_SHADER_CONTROL,0x00000010U);
    s.reg(SPI_SHADER_PGM_RSRC4_PS,w.fragmentPrefetch<<16);
    s.reg(PA_SC_MODE_CNTL_1,(r.topology.color.gb_addr_config&7)==1?0x760201acU:0x760201bcU);
    s.reg(PA_SC_BINNER_CNTL_0,0x10040123U);
    s.reg(CB_COLOR_CONTROL,0x00cc0010U);
    s.reg(DB_EQAA,0x00130000U);
    s.reg(PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,0x00100000U);
    s.reg(PA_SC_MODE_CNTL_0,0x00000022U);
    s.reg(SX_MRT0_BLEND_OPT,0x06000600U);
    s.reg(PA_SC_EDGERULE,0xaaaaaaaaU);
    s.reg(PA_CL_NGG_CNTL,0x00000078U);
    s.reg(PA_SU_POINT_SIZE,0x00080008U);
    s.reg(PA_SU_POINT_MINMAX,0xffff0000U);
    s.reg(PA_SU_SMALL_PRIM_FILTER_CNTL,0x00000041U);
    s.reg(SPI_INTERP_CONTROL_0,0x0000086bU);
    s.reg(PA_SU_VTX_CNTL,0x0000002dU);
    s.reg(PA_CL_VTE_CNTL,0x0000043fU);
    s.reg(PA_SU_LINE_STIPPLE_SCALE,0x3f800000U);
    s.reg(CB_FDCC_CONTROL,0x00000200U);
    s.reg(PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0,0x00000000U);
    s.reg(PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0,0x00000000U);
    s.reg(PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0,0x00000000U);
    s.reg(PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0,0x00000000U);
    s.reg(PA_SC_CENTROID_PRIORITY_0,0x00000000U);
    s.reg(PA_SC_CENTROID_PRIORITY_1,0x00000000U);
    s.reg(PA_SU_PRIM_FILTER_CNTL,0xc0000000U);
    s.reg(DB_ALPHA_TO_MASK,0x00018700U);
    s.reg(PA_SC_AA_MASK_X0Y0_X1Y0,0xffffffffU);
    s.reg(PA_SC_AA_MASK_X0Y1_X1Y1,0xffffffffU);
    s.reg(PA_SC_AA_CONFIG,0x00000000U);
    s.reg(SPI_BARYC_CNTL,0x00000000U);
    s.reg(PA_SU_SC_MODE_CNTL,0x00000240U);
    s.reg(PA_CL_CLIP_CNTL,0x01080000U);
    s.reg(PA_SU_LINE_CNTL,0x00000008U);
    s.reg(PA_SC_LINE_STIPPLE,0x4000ffffU);
    s.reg(PA_SC_LINE_CNTL,0x00000000U);
    s.reg(DB_Z_INFO,0x00000000U);
    s.reg(DB_STENCIL_INFO,0x00000000U);
    s.reg(DB_RENDER_CONTROL,0x00000000U);
    s.reg(DB_COUNT_CONTROL,0x00000000U);
    s.reg(DB_RENDER_OVERRIDE2,0x08000000U);
    s.reg(DB_RENDER_OVERRIDE,0x00000000U);
    s.reg(DB_DEPTH_CONTROL,0x00000000U);
    s.reg(DB_STENCIL_CONTROL,0x00000000U);
    s.reg(PA_SC_CLIPRECT_RULE,0x0000ffffU);
    s.reg(GE_VRS_RATE,0x00000000U);
    s.reg(PA_CL_VRS_CNTL,0x00000000U);
    s.reg(PA_SC_VRS_OVERRIDE_CNTL,0x00000000U);
    s.reg(VGT_DRAW_PAYLOAD_CNTL,0x00000040U);
    s.reg(VGT_PRIMITIVE_TYPE,0x00000004U);
    s.reg(VGT_GS_OUT_PRIM_TYPE,0x00000002U);
    s.reg(SX_PS_DOWNCONVERT,0x00000001U);
    s.reg(SX_BLEND_OPT_EPSILON,0x00000000U);
    s.reg(SX_BLEND_OPT_CONTROL,0x00000000U);
}
void commands(const Request& r,const Workspace& w,Stream& s,const uint32_t* descriptor=nullptr,
              const uint32_t* vertexDescriptor=nullptr) {
    s.add(packet(0x28,1));s.add(0x80000000);s.add(0x80000000); // CONTEXT_CONTROL
    // Real bottom-of-pipe idle, no memory destination. Adjacent PWS pair and
    // all four ring writes; no caller can insert a producer in this private IB.
    s.pws(0x80000528,0x00022800);
    s.seq(SPI_GS_THROTTLE_CNTL1,4);s.add(0x12355123);s.add(0x1544d);
    s.add(uint32_t(r.attributes.gpu>>16));s.add(0x00020015); //22*64KiB/SE,L1 policy1,BIG_PAGE0
    commonPreamble(r,w,s);
    for(unsigned i=0;i<16;++i) {
        s.reg(PA_SC_VPORT_ZMIN_0+i*8,0);s.reg(PA_SC_VPORT_ZMAX_0+i*8,0x3f800000);
    }
    fixedState(r,w,s);
    // CPU upload/HDP sync belongs to the owner. This real full-range PFP
    // ACQUIRE invalidates instruction/scalar/vector/L1 and writes back+invalidates
    // L2/metadata before any shader instruction reads (ac_barrier.c).
    s.add(packet(0x58,6));s.add(0);s.add(UINT32_MAX);s.add(0x00ffffff);
    s.add(0);s.add(0);s.add(10);s.add(0xc3b1);
    s.reg(SPI_SHADER_PGM_LO_ES,uint32_t(r.code.gpu>>8));
    s.reg(SPI_SHADER_PGM_RSRC1_GS,w.program.vertex.rsrc1);
    s.reg(SPI_SHADER_PGM_RSRC2_GS,w.program.vertex.rsrc2);
    if(vertexDescriptor) {
        s.seq(SPI_SHADER_USER_DATA_GS_0,5);
        for(unsigned i=0;i<4;++i)s.add(vertexDescriptor[i]);
        s.add(0); // baseVertex0 is s12 after the raw48 descriptor in s8..s11
    } else s.reg(SPI_SHADER_USER_DATA_GS_0,0); // resource-free baseVertex s8
    const uint64_t ps=r.code.gpu+w.program.image.fragmentOffset;
    s.reg(SPI_SHADER_PGM_LO_PS,uint32_t(ps>>8));s.reg(SPI_SHADER_PGM_HI_PS,uint32_t(ps>>40));
    s.reg(SPI_SHADER_PGM_RSRC1_PS,w.program.fragment.rsrc1);
    s.reg(SPI_SHADER_PGM_RSRC2_PS,w.program.fragment.rsrc2);
    if(descriptor) {
        s.seq(SPI_SHADER_USER_DATA_PS_0,4);
        for(unsigned i=0;i<4;++i)s.add(descriptor[i]);
    }
    s.reg(VGT_SHADER_STAGES_EN,w.program.vgtShaderStagesEn);
    s.reg(SPI_PS_INPUT_ENA,w.program.spiPsInputEna);s.reg(SPI_PS_INPUT_ADDR,w.program.spiPsInputAddr);
    s.reg(SPI_PS_IN_CONTROL,w.program.spiPsInControl);
    s.reg(SPI_SHADER_Z_FORMAT,w.program.spiShaderZFormat);
    s.reg(SPI_SHADER_COL_FORMAT,w.program.spiShaderColFormat);
    s.reg(CB_SHADER_MASK,w.program.cbShaderMask);s.reg(CB_TARGET_MASK,15);
    for(unsigned i=0;i<8;++i) {
        s.reg(CB_BLEND0_CONTROL+i*4,0);s.reg(SX_MRT0_BLEND_OPT+i*4,0x06000600);
    }
    const HNColorLayout& c=r.targetLayout;
    // Exact GFX11 VIEW..FDCC sequence, followed by separate address/attribute
    // writes (radv_gfx11_emit_fb_color_state). GFX10 pitch/slice/CMASK/FMASK
    // slots are absent on GFX11 and must never be written, even with zero.
    s.seq(CB_COLOR0_VIEW,4);s.add(c.cb_color_view);
    s.add(c.cb_color_info);s.add(c.cb_color_attrib);s.add(c.cb_dcc_control);
    s.reg(CB_COLOR0_BASE,uint32_t(r.target.gpu>>8));s.reg(CB_COLOR0_BASE_EXT,uint32_t(r.target.gpu>>40));
    s.reg(CB_COLOR0_DCC_BASE,0);s.reg(CB_COLOR0_DCC_BASE_EXT,0);
    s.reg(CB_COLOR0_ATTRIB2,c.cb_color_attrib2);s.reg(CB_COLOR0_ATTRIB3,c.cb_color_attrib3);
    for(unsigned i=1;i<8;++i)s.reg(CB_COLOR0_INFO+i*0x3c,0); //COLOR_INVALID
    // Metal clip Y=+1 is the top edge. Actual AIR/SPIR-V retains that Y,
    // so negate the hardware scale exactly once; offset remains +height/2.
    // All dimensions<=16384 fit signed16.8 with screen offset zero.
    s.seq(PA_CL_VPORT_XSCALE,6);s.add(halfBits(c.width));s.add(halfBits(c.width));
    s.add(halfBits(c.height)|0x80000000u);s.add(halfBits(c.height));s.add(0x3f800000);s.add(0);
    const uint32_t br=c.width|(c.height<<16);
    s.reg(PA_SC_SCREEN_SCISSOR_BR,br);s.reg(PA_SC_WINDOW_OFFSET,0);
    s.reg(PA_SC_WINDOW_SCISSOR_TL,0);s.reg(PA_SC_WINDOW_SCISSOR_BR,br);
    s.reg(PA_SC_VPORT_SCISSOR_0_TL,0x80000000);s.reg(PA_SC_VPORT_SCISSOR_0_BR,br);
    // Conservative clip/discard boundary1, rather than Mesa's larger optional
    // guardband optimization. ac_guardband.h specifies these clip-space bounds;
    // geometry is clipped exactly at the viewport and cannot overflow rasterizer.
    s.reg(PA_SU_HARDWARE_SCREEN_OFFSET,0);
    s.reg(PA_CL_GB_VERT_CLIP_ADJ,0x3f800000);s.reg(PA_CL_GB_VERT_DISC_ADJ,0x3f800000);
    s.reg(PA_CL_GB_HORZ_CLIP_ADJ,0x3f800000);s.reg(PA_CL_GB_HORZ_DISC_ADJ,0x3f800000);
    s.add(packet(0x2f,0));s.add(1); //NUM_INSTANCES
    s.add(packet(0x2d,1));s.add(3);s.add(2); //DRAW_INDEX_AUTO, AUTO_INDEX
    uint32_t completion[gfx11graphicsbarrier::kDwords]={};
    if(gfx11graphicsbarrier::encode(completion,gfx11graphicsbarrier::kDwords)!=gfx11graphicsbarrier::Status::Ok)
        s.mismatch=true;
    for(size_t i=0;i<gfx11graphicsbarrier::kDwords;++i)s.add(completion[i]);
    while(s.count<kMaxCommandDwords&&(s.count&7))s.add(0xffff1000); //type3 NOP pad
}
} // namespace
static Status prepareProgram(const Request& r,Workspace& w,bool buffered,bool vertexBuffered=false) {
    if(!pointer(&r,sizeof(r),alignof(Request))||!pointer(&w,sizeof(w),alignof(Workspace))||
       !pointer(r.image.image,r.image.imageBytes)||inputAlias(r,&w,sizeof(w)))return Status::InvalidArgument;
    w={};
    if(!r.owner||!r.epoch||r.vmid<1||r.vmid>7)return Status::InvalidArgument;
    const Topology& t=r.topology;
    // Numeric consistency only. Actual same-owner GC/harvest capture is mandatory.
    if(!t.maxShaderEngines||t.maxShaderEngines>6||t.maxRenderBackends!=t.maxShaderEngines*4||
       t.minGoodCuPerSa<2||t.minGoodCuPerSa>16||(t.minGoodCuPerSa&1))return Status::UnsupportedTopology;
    const HNColorRequest dimensions={r.targetLayout.width,r.targetLayout.height};
    if(hn_color_target_validate(&t.color,&dimensions,&r.targetLayout)!=HN_COLOR_OK)return Status::InvalidTarget;
    const uint64_t attributeBytes=uint64_t(kAttributeBytesPerSe)*t.maxShaderEngines;
    Status st=binding(r,r.code,gfxisolated::Read|gfxisolated::Execute,kCodeBytes);
    if(st!=Status::Ok)return st;
    st=binding(r,r.target,gfxisolated::Read|gfxisolated::Write);if(st!=Status::Ok)return st;
    st=binding(r,r.attributes,gfxisolated::Read|gfxisolated::Write,attributeBytes);if(st!=Status::Ok)return st;
    st=binding(r,r.indirect,gfxisolated::Read|gfxisolated::Execute,kIndirectBytes);if(st!=Status::Ok)return st;
    if(r.targetLayout.total_bytes>r.target.bytes)return Status::InvalidTarget;
    if((r.attributes.gpu&65535)||r.code.gpu>>32!=r.attributes.gpu>>32||
       (r.code.gpu+r.code.bytes-1)>>32!=r.code.gpu>>32||
       (r.attributes.gpu+r.attributes.bytes-1)>>32!=r.attributes.gpu>>32)return Status::InvalidBinding;
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j) {
        const auto& a=bound(r,i);const auto& b=bound(r,j);
        if(a.id==b.id)return Status::AliasedStorage;
        if(a.gpu<b.gpu+b.bytes&&b.gpu<a.gpu+a.bytes)return Status::Overlap;
    }
    if(r.image.imageBytes!=kCodeBytes||(vertexBuffered ?
        (buffered?gfx11graphics::validateVertexFragmentBuffers(r.image,w.program):
                  gfx11graphics::validateVertexBuffer(r.image,w.program)) :
        (buffered?gfx11graphics::validateFragmentBuffer(r.image,w.program):
                  gfx11graphics::validate(r.image,w.program)))!=gfx11graphics::Status::Ok)
        return Status::InvalidShader;
    w.address32Hi=uint32_t(r.code.gpu>>32);
    w.psCuMask=(1u<<t.minGoodCuPerSa)-1;
    w.lateAlloc=t.minGoodCuPerSa<=2?0:63;w.nggCuMask=w.lateAlloc?0xfffd:0xffff;
    w.vertexPrefetch=(w.program.vertex.executableBytes+192+127)/128;
    w.fragmentPrefetch=(w.program.fragment.executableBytes+192+127)/128;
    if(uint64_t(w.vertexPrefetch)*128>r.code.bytes||
       w.program.image.fragmentOffset+uint64_t(w.fragmentPrefetch)*128>r.code.bytes)return Status::InvalidShader;
    return Status::Ok;
}
static Status countCommands(const Request& r,Workspace& w,const uint32_t* descriptor=nullptr,
                            const uint32_t* vertexDescriptor=nullptr) {
    Stream count={nullptr,nullptr,0,0,false};commands(r,w,count,descriptor,vertexDescriptor);
    if(count.mismatch||count.count>kMaxCommandDwords)return Status::BufferTooSmall;
    w.commandWords=count.count;return Status::Ok;
}
Status prepare(const Request& r,Workspace& w) {
    const Status st=prepareProgram(r,w,false);return st==Status::Ok?countCommands(r,w):st;
}
Status encode(const Request& r,Workspace& w,uint32_t* output,size_t capacityWords,size_t& words) {
    // Validate all publication aliases BEFORE prepare can modify workspace.
    if(!pointer(&words,sizeof(words),alignof(size_t))||inputAlias(r,&words,sizeof(words))||
       overlaps(&words,sizeof(words),&w,sizeof(w))||!pointer(output,kIndirectBytes,alignof(uint32_t))||
       inputAlias(r,output,kIndirectBytes)||overlaps(output,kIndirectBytes,&w,sizeof(w))||
       overlaps(output,kIndirectBytes,&words,sizeof(words)))return Status::InvalidArgument;
    const Status st=prepare(r,w);if(st!=Status::Ok)return st;
    if(capacityWords<w.commandWords)return Status::BufferTooSmall;
    Stream write={output,nullptr,0,0,false};commands(r,w,write);
    words=w.commandWords;return Status::Ok;
}
Status validate(const Request& r,Workspace& w,const uint32_t* input,size_t exactWords) {
    if(!exactWords||exactWords>kMaxCommandDwords||!pointer(input,exactWords*4,alignof(uint32_t))||
       inputAlias(r,input,exactWords*4)||overlaps(input,exactWords*4,&w,sizeof(w)))return Status::InvalidArgument;
    const Status st=prepare(r,w);if(st!=Status::Ok)return st;
    if(exactWords!=w.commandWords)return Status::InvalidStream;
    Stream compare={nullptr,input,exactWords,0,false};commands(r,w,compare);
    return compare.mismatch?Status::InvalidStream:Status::Ok;
}
static bool bufferedAlias(const BufferedRequest& r,const void* p,size_t bytes) {
    return overlaps(p,bytes,&r,sizeof(r))||overlaps(p,bytes,r.draw.image.image,r.draw.image.imageBytes);
}
Status prepare(const BufferedRequest& r,BufferedWorkspace& w) {
    if(!pointer(&r,sizeof(r),alignof(BufferedRequest))||!pointer(&w,sizeof(w),alignof(BufferedWorkspace))||
       !pointer(r.draw.image.image,r.draw.image.imageBytes)||bufferedAlias(r,&w,sizeof(w)))
        return Status::InvalidArgument;
    w={};
    Status st=prepareProgram(r.draw,w.draw,true);if(st!=Status::Ok)return st;
    st=binding(r.draw,r.fragment,gfxisolated::Read);if(st!=Status::Ok)return st;
    if((r.offset&15)||r.offset>r.fragment.bytes||r.fragment.bytes-r.offset<16)
        return Status::InvalidBinding;
    for(unsigned i=0;i<4;++i) {
        const auto& b=bound(r.draw,i);
        if(r.fragment.id==b.id)return Status::AliasedStorage;
        if(r.fragment.gpu<b.gpu+b.bytes&&b.gpu<r.fragment.gpu+r.fragment.bytes)return Status::Overlap;
    }
    const uint64_t address=r.fragment.gpu+r.offset;
    // ac_build_raw_buffer_descriptor(GFX11,false,address,16): stride0,
    // XYZW swizzle, R32_FLOAT raw format, OOB_SELECT_RAW includes payload.
    // Native VA ceiling is stricter than the descriptor's 48-bit address.
    w.descriptor[0]=uint32_t(address);w.descriptor[1]=uint32_t(address>>32);
    w.descriptor[2]=16;w.descriptor[3]=0x30016fac;
    return countCommands(r.draw,w.draw,w.descriptor);
}
Status encode(const BufferedRequest& r,BufferedWorkspace& w,uint32_t* output,size_t capacityWords,size_t& words) {
    if(!pointer(&words,sizeof(words),alignof(size_t))||bufferedAlias(r,&words,sizeof(words))||
       overlaps(&words,sizeof(words),&w,sizeof(w))||!pointer(output,kIndirectBytes,alignof(uint32_t))||
       bufferedAlias(r,output,kIndirectBytes)||overlaps(output,kIndirectBytes,&w,sizeof(w))||
       overlaps(output,kIndirectBytes,&words,sizeof(words)))return Status::InvalidArgument;
    const Status st=prepare(r,w);if(st!=Status::Ok)return st;
    if(capacityWords<w.draw.commandWords)return Status::BufferTooSmall;
    Stream write={output,nullptr,0,0,false};commands(r.draw,w.draw,write,w.descriptor);
    words=w.draw.commandWords;return Status::Ok;
}
Status validate(const BufferedRequest& r,BufferedWorkspace& w,const uint32_t* input,size_t exactWords) {
    if(!exactWords||exactWords>kMaxCommandDwords||!pointer(input,exactWords*4,alignof(uint32_t))||
       bufferedAlias(r,input,exactWords*4)||overlaps(input,exactWords*4,&w,sizeof(w)))return Status::InvalidArgument;
    const Status st=prepare(r,w);if(st!=Status::Ok)return st;
    if(exactWords!=w.draw.commandWords)return Status::InvalidStream;
    Stream compare={nullptr,input,exactWords,0,false};commands(r.draw,w.draw,compare,w.descriptor);
    return compare.mismatch?Status::InvalidStream:Status::Ok;
}
static Status resourceDescriptor(const Request& r,const gfxisolated::Binding& resource,
                                 uint64_t offset,uint32_t bytes,uint32_t (&descriptor)[4]) {
    Status st=binding(r,resource,gfxisolated::Read);if(st!=Status::Ok)return st;
    if((offset&15)||offset>resource.bytes||resource.bytes-offset<bytes)return Status::InvalidBinding;
    for(unsigned i=0;i<4;++i) {
        const auto& b=bound(r,i);
        if(resource.id==b.id)return Status::AliasedStorage;
        if(resource.gpu<b.gpu+b.bytes&&b.gpu<resource.gpu+resource.bytes)return Status::Overlap;
    }
    const uint64_t address=resource.gpu+offset;
    descriptor[0]=uint32_t(address);descriptor[1]=uint32_t(address>>32);
    descriptor[2]=bytes;descriptor[3]=0x30016fac;return Status::Ok;
}
template<class R> static bool vertexAlias(const R& r,const void* p,size_t bytes) {
    return overlaps(p,bytes,&r,sizeof(r))||overlaps(p,bytes,r.draw.image.image,r.draw.image.imageBytes);
}
template<class R> static Status prepareVertex(const R& r,VertexBufferedWorkspace& w,uint64_t offset,
                                              const gfxisolated::Binding* fragment,uint64_t fragmentOffset) {
    if(!pointer(&r,sizeof(r),alignof(R))||!pointer(&w,sizeof(w),alignof(VertexBufferedWorkspace))||
       !pointer(r.draw.image.image,r.draw.image.imageBytes)||vertexAlias(r,&w,sizeof(w)))return Status::InvalidArgument;
    w={};
    Status st=prepareProgram(r.draw,w.draw,fragment!=nullptr,true);if(st!=Status::Ok)return st;
    st=resourceDescriptor(r.draw,r.vertex,offset,48,w.vertexDescriptor);if(st!=Status::Ok)return st;
    if(fragment) {
        st=resourceDescriptor(r.draw,*fragment,fragmentOffset,16,w.fragmentDescriptor);if(st!=Status::Ok)return st;
        if(r.vertex.id==fragment->id)return Status::AliasedStorage;
        if(r.vertex.gpu<fragment->gpu+fragment->bytes&&fragment->gpu<r.vertex.gpu+r.vertex.bytes)
            return Status::Overlap;
    }
    return countCommands(r.draw,w.draw,fragment?w.fragmentDescriptor:nullptr,w.vertexDescriptor);
}
Status prepare(const VertexBufferedRequest& r,VertexBufferedWorkspace& w) {
    return prepareVertex(r,w,r.offset,nullptr,0);
}
Status prepare(const VertexFragmentBufferedRequest& r,VertexBufferedWorkspace& w) {
    return prepareVertex(r,w,r.vertexOffset,&r.fragment,r.fragmentOffset);
}
template<class R> static Status encodeVertex(const R& r,VertexBufferedWorkspace& w,uint32_t* output,
                                             size_t capacityWords,size_t& words,bool fragment) {
    if(!pointer(&words,sizeof(words),alignof(size_t))||vertexAlias(r,&words,sizeof(words))||
       overlaps(&words,sizeof(words),&w,sizeof(w))||!pointer(output,kIndirectBytes,alignof(uint32_t))||
       vertexAlias(r,output,kIndirectBytes)||overlaps(output,kIndirectBytes,&w,sizeof(w))||
       overlaps(output,kIndirectBytes,&words,sizeof(words)))return Status::InvalidArgument;
    const Status st=prepare(r,w);if(st!=Status::Ok)return st;
    if(capacityWords<w.draw.commandWords)return Status::BufferTooSmall;
    Stream write={output,nullptr,0,0,false};
    commands(r.draw,w.draw,write,fragment?w.fragmentDescriptor:nullptr,w.vertexDescriptor);
    words=w.draw.commandWords;return Status::Ok;
}
template<class R> static Status validateVertex(const R& r,VertexBufferedWorkspace& w,const uint32_t* input,
                                               size_t exactWords,bool fragment) {
    if(!exactWords||exactWords>kMaxCommandDwords||!pointer(input,exactWords*4,alignof(uint32_t))||
       vertexAlias(r,input,exactWords*4)||overlaps(input,exactWords*4,&w,sizeof(w)))return Status::InvalidArgument;
    const Status st=prepare(r,w);if(st!=Status::Ok)return st;
    if(exactWords!=w.draw.commandWords)return Status::InvalidStream;
    Stream compare={nullptr,input,exactWords,0,false};
    commands(r.draw,w.draw,compare,fragment?w.fragmentDescriptor:nullptr,w.vertexDescriptor);
    return compare.mismatch?Status::InvalidStream:Status::Ok;
}
Status encode(const VertexBufferedRequest& r,VertexBufferedWorkspace& w,uint32_t* output,size_t capacityWords,size_t& words) {
    return encodeVertex(r,w,output,capacityWords,words,false);
}
Status encode(const VertexFragmentBufferedRequest& r,VertexBufferedWorkspace& w,uint32_t* output,size_t capacityWords,size_t& words) {
    return encodeVertex(r,w,output,capacityWords,words,true);
}
Status validate(const VertexBufferedRequest& r,VertexBufferedWorkspace& w,const uint32_t* input,size_t exactWords) {
    return validateVertex(r,w,input,exactWords,false);
}
Status validate(const VertexFragmentBufferedRequest& r,VertexBufferedWorkspace& w,const uint32_t* input,size_t exactWords) {
    return validateVertex(r,w,input,exactWords,true);
}
const char* statusString(Status s) {
    switch(s) {
    case Status::Ok:return "Ok";case Status::InvalidArgument:return "InvalidArgument";
    case Status::UnsupportedTopology:return "UnsupportedTopology";case Status::ForeignContext:return "ForeignContext";
    case Status::InvalidBinding:return "InvalidBinding";case Status::Permission:return "Permission";
    case Status::AliasedStorage:return "AliasedStorage";case Status::Overlap:return "Overlap";
    case Status::InvalidShader:return "InvalidShader";case Status::InvalidTarget:return "InvalidTarget";
    case Status::BufferTooSmall:return "BufferTooSmall";case Status::InvalidStream:return "InvalidStream";
    }
    return "Unknown";
}
} }
