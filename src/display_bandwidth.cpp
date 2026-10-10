// SPDX-License-Identifier: MIT
/*
 * DCN3.2 HUBP protocol adapted from Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0: dcn20_hubp.c,
 * dcn21_hubp.c, dcn30_hubp.c, dcn32_hubbub.c, dcn32_fpu.c and
 * dcn_3_2_0_offset.h/sh_mask.h. DML calculations remain in userspace.
 * Copyright 2012-2022 Advanced Micro Devices, Inc.
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "hackynavi/display_bandwidth.hpp"
namespace hackynavi { namespace displaybandwidth { namespace {
struct FieldSpec { uint8_t reg,shift;uint32_t mask; };
// Generated from pinned dcn_3_2_0_offset.h/sh_mask.h. All BASE_IDX2;
// every pipe's generated offset was checked, not assumed from a single stride.
const uint32_t timingOffsets[kTimingRegisterCount]={
    0x66c, // HUBPRET0_HUBPRET_CONTROL
    0x628, // HUBPREQ0_DCN_EXPANSION_MODE
    0x5f1, // HUBP0_DCHUBP_REQ_SIZE_CONFIG
    0x5f2, // HUBP0_DCHUBP_REQ_SIZE_CONFIG_C
    0x643, // HUBPREQ0_BLANK_OFFSET_0
    0x644, // HUBPREQ0_BLANK_OFFSET_1
    0x645, // HUBPREQ0_DST_DIMENSIONS
    0x646, // HUBPREQ0_DST_AFTER_SCALER
    0x65c, // HUBPREQ0_REF_FREQ_TO_PIX_FREQ
    0x64a, // HUBPREQ0_VBLANK_PARAMETERS_1
    0x651, // HUBPREQ0_NOM_PARAMETERS_0
    0x652, // HUBPREQ0_NOM_PARAMETERS_1
    0x655, // HUBPREQ0_NOM_PARAMETERS_4
    0x656, // HUBPREQ0_NOM_PARAMETERS_5
    0x65a, // HUBPREQ0_PER_LINE_DELIVERY
    0x64b, // HUBPREQ0_VBLANK_PARAMETERS_2
    0x653, // HUBPREQ0_NOM_PARAMETERS_2
    0x654, // HUBPREQ0_NOM_PARAMETERS_3
    0x657, // HUBPREQ0_NOM_PARAMETERS_6
    0x658, // HUBPREQ0_NOM_PARAMETERS_7
    0x629, // HUBPREQ0_DCN_TTU_QOS_WM
    0x62b, // HUBPREQ0_DCN_SURF0_TTU_CNTL0
    0x62d, // HUBPREQ0_DCN_SURF1_TTU_CNTL0
    0x62f, // HUBPREQ0_DCN_CUR0_TTU_CNTL0
    0x64f, // HUBPREQ0_FLIP_PARAMETERS_1
    0x633, // HUBPREQ0_DCN_DMDATA_VM_CNTL
    0x647, // HUBPREQ0_PREFETCH_SETTINGS
    0x648, // HUBPREQ0_PREFETCH_SETTINGS_C
    0x649, // HUBPREQ0_VBLANK_PARAMETERS_0
    0x64e, // HUBPREQ0_FLIP_PARAMETERS_0
    0x64c, // HUBPREQ0_VBLANK_PARAMETERS_3
    0x64d, // HUBPREQ0_VBLANK_PARAMETERS_4
    0x650, // HUBPREQ0_FLIP_PARAMETERS_2
    0x659, // HUBPREQ0_PER_LINE_DELIVERY_PRE
    0x62c, // HUBPREQ0_DCN_SURF0_TTU_CNTL1
    0x62e, // HUBPREQ0_DCN_SURF1_TTU_CNTL1
    0x630, // HUBPREQ0_DCN_CUR0_TTU_CNTL1
    0x632, // HUBPREQ0_DCN_CUR1_TTU_CNTL1
    0x62a, // HUBPREQ0_DCN_GLOBAL_TTU_CNTL
};
const FieldSpec fields[kFieldCount]={
    {0,4,0x00007ff0u}, // rq_plane1_base_address / DET_BUF_PLANE1_BASE_ADDRESS
    {1,0,0x00000003u}, // rq_drq_expansion_mode / DRQ_EXPANSION_MODE
    {1,6,0x000000c0u}, // rq_prq_expansion_mode / PRQ_EXPANSION_MODE
    {1,4,0x00000030u}, // rq_mrq_expansion_mode / MRQ_EXPANSION_MODE
    {1,2,0x0000000cu}, // rq_crq_expansion_mode / CRQ_EXPANSION_MODE
    {2,8,0x00000700u}, // rq_rq_regs_l_chunk_size / CHUNK_SIZE
    {2,11,0x00001800u}, // rq_rq_regs_l_min_chunk_size / MIN_CHUNK_SIZE
    {2,16,0x00030000u}, // rq_rq_regs_l_meta_chunk_size / META_CHUNK_SIZE
    {2,18,0x000c0000u}, // rq_rq_regs_l_min_meta_chunk_size / MIN_META_CHUNK_SIZE
    {2,20,0x00700000u}, // rq_rq_regs_l_dpte_group_size / DPTE_GROUP_SIZE
    {2,24,0x07000000u}, // rq_rq_regs_l_mpte_group_size / VM_GROUP_SIZE
    {2,0,0x00000007u}, // rq_rq_regs_l_swath_height / SWATH_HEIGHT
    {2,4,0x00000070u}, // rq_rq_regs_l_pte_row_height_linear / PTE_ROW_HEIGHT_LINEAR
    {3,8,0x00000700u}, // rq_rq_regs_c_chunk_size / CHUNK_SIZE_C
    {3,11,0x00001800u}, // rq_rq_regs_c_min_chunk_size / MIN_CHUNK_SIZE_C
    {3,16,0x00030000u}, // rq_rq_regs_c_meta_chunk_size / META_CHUNK_SIZE_C
    {3,18,0x000c0000u}, // rq_rq_regs_c_min_meta_chunk_size / MIN_META_CHUNK_SIZE_C
    {3,20,0x00700000u}, // rq_rq_regs_c_dpte_group_size / DPTE_GROUP_SIZE_C
    {3,0,0x00000007u}, // rq_rq_regs_c_swath_height / SWATH_HEIGHT_C
    {3,4,0x00000070u}, // rq_rq_regs_c_pte_row_height_linear / PTE_ROW_HEIGHT_LINEAR_C
    {4,0,0x00001fffu}, // dlg_refcyc_h_blank_end / REFCYC_H_BLANK_END
    {4,16,0x7fff0000u}, // dlg_dlg_vblank_end / DLG_V_BLANK_END
    {5,0,0x0003ffffu}, // dlg_min_dst_y_next_start / MIN_DST_Y_NEXT_START
    {6,0,0x001fffffu}, // dlg_refcyc_per_htotal / REFCYC_PER_HTOTAL
    {7,0,0x00001fffu}, // dlg_refcyc_x_after_scaler / REFCYC_X_AFTER_SCALER
    {7,16,0x00070000u}, // dlg_dst_y_after_scaler / DST_Y_AFTER_SCALER
    {8,0,0x001fffffu}, // dlg_ref_freq_to_pix_freq / REF_FREQ_TO_PIX_FREQ
    {9,0,0x007fffffu}, // dlg_refcyc_per_pte_group_vblank_l / REFCYC_PER_PTE_GROUP_VBLANK_L
    {10,0,0x0001ffffu}, // dlg_dst_y_per_pte_row_nom_l / DST_Y_PER_PTE_ROW_NOM_L
    {11,0,0x007fffffu}, // dlg_refcyc_per_pte_group_nom_l / REFCYC_PER_PTE_GROUP_NOM_L
    {12,0,0x0001ffffu}, // dlg_dst_y_per_meta_row_nom_l / DST_Y_PER_META_ROW_NOM_L
    {13,0,0x007fffffu}, // dlg_refcyc_per_meta_chunk_nom_l / REFCYC_PER_META_CHUNK_NOM_L
    {14,0,0x00001fffu}, // dlg_refcyc_per_line_delivery_l / REFCYC_PER_LINE_DELIVERY_L
    {14,16,0x1fff0000u}, // dlg_refcyc_per_line_delivery_c / REFCYC_PER_LINE_DELIVERY_C
    {15,0,0x007fffffu}, // dlg_refcyc_per_pte_group_vblank_c / REFCYC_PER_PTE_GROUP_VBLANK_C
    {16,0,0x0001ffffu}, // dlg_dst_y_per_pte_row_nom_c / DST_Y_PER_PTE_ROW_NOM_C
    {17,0,0x007fffffu}, // dlg_refcyc_per_pte_group_nom_c / REFCYC_PER_PTE_GROUP_NOM_C
    {18,0,0x0001ffffu}, // dlg_dst_y_per_meta_row_nom_c / DST_Y_PER_META_ROW_NOM_C
    {19,0,0x007fffffu}, // dlg_refcyc_per_meta_chunk_nom_c / REFCYC_PER_META_CHUNK_NOM_C
    {20,0,0x00003fffu}, // ttu_qos_level_low_wm / QoS_LEVEL_LOW_WM
    {20,16,0x3fff0000u}, // ttu_qos_level_high_wm / QoS_LEVEL_HIGH_WM
    {21,0,0x007fffffu}, // ttu_refcyc_per_req_delivery_l / REFCYC_PER_REQ_DELIVERY
    {21,24,0x0f000000u}, // ttu_qos_level_fixed_l / QoS_LEVEL_FIXED
    {21,28,0x10000000u}, // ttu_qos_ramp_disable_l / QoS_RAMP_DISABLE
    {22,0,0x007fffffu}, // ttu_refcyc_per_req_delivery_c / REFCYC_PER_REQ_DELIVERY
    {22,24,0x0f000000u}, // ttu_qos_level_fixed_c / QoS_LEVEL_FIXED
    {22,28,0x10000000u}, // ttu_qos_ramp_disable_c / QoS_RAMP_DISABLE
    {23,0,0x007fffffu}, // ttu_refcyc_per_req_delivery_cur0 / REFCYC_PER_REQ_DELIVERY
    {23,24,0x0f000000u}, // ttu_qos_level_fixed_cur0 / QoS_LEVEL_FIXED
    {23,28,0x10000000u}, // ttu_qos_ramp_disable_cur0 / QoS_RAMP_DISABLE
    {24,0,0x007fffffu}, // dlg_refcyc_per_pte_group_flip_l / REFCYC_PER_PTE_GROUP_FLIP_L
    {25,0,0x0000ffffu}, // dlg_refcyc_per_vm_dmdata / REFCYC_PER_VM_DMDATA
    {26,24,0xff000000u}, // dlg_dst_y_prefetch / DST_Y_PREFETCH
    {26,0,0x003fffffu}, // dlg_vratio_prefetch / VRATIO_PREFETCH
    {27,0,0x003fffffu}, // dlg_vratio_prefetch_c / VRATIO_PREFETCH_C
    {28,0,0x0000007fu}, // dlg_dst_y_per_vm_vblank / DST_Y_PER_VM_VBLANK
    {28,8,0x00003f00u}, // dlg_dst_y_per_row_vblank / DST_Y_PER_ROW_VBLANK
    {29,0,0x0000007fu}, // dlg_dst_y_per_vm_flip / DST_Y_PER_VM_FLIP
    {29,8,0x00003f00u}, // dlg_dst_y_per_row_flip / DST_Y_PER_ROW_FLIP
    {30,0,0x007fffffu}, // dlg_refcyc_per_meta_chunk_vblank_l / REFCYC_PER_META_CHUNK_VBLANK_L
    {31,0,0x007fffffu}, // dlg_refcyc_per_meta_chunk_vblank_c / REFCYC_PER_META_CHUNK_VBLANK_C
    {32,0,0x007fffffu}, // dlg_refcyc_per_meta_chunk_flip_l / REFCYC_PER_META_CHUNK_FLIP_L
    {33,0,0x00001fffu}, // dlg_refcyc_per_line_delivery_pre_l / REFCYC_PER_LINE_DELIVERY_PRE_L
    {33,16,0x1fff0000u}, // dlg_refcyc_per_line_delivery_pre_c / REFCYC_PER_LINE_DELIVERY_PRE_C
    {34,0,0x007fffffu}, // ttu_refcyc_per_req_delivery_pre_l / REFCYC_PER_REQ_DELIVERY_PRE
    {35,0,0x007fffffu}, // ttu_refcyc_per_req_delivery_pre_c / REFCYC_PER_REQ_DELIVERY_PRE
    {36,0,0x007fffffu}, // ttu_refcyc_per_req_delivery_pre_cur0 / REFCYC_PER_REQ_DELIVERY_PRE
    {37,0,0x007fffffu}, // ttu_refcyc_per_req_delivery_pre_cur1 / REFCYC_PER_REQ_DELIVERY_PRE
    {38,0,0x00ffffffu}, // ttu_min_ttu_vblank / MIN_TTU_VBLANK
    {38,28,0xf0000000u}, // ttu_qos_level_flip / QoS_LEVEL_FLIP
};
const uint32_t timingMasks[kTimingRegisterCount]={
    0x00007ff0u,
    0x000000ffu,
    0x077f1f77u,
    0x007f1f77u,
    0x7fff1fffu,
    0x0003ffffu,
    0x001fffffu,
    0x00071fffu,
    0x001fffffu,
    0x007fffffu,
    0x0001ffffu,
    0x007fffffu,
    0x0001ffffu,
    0x007fffffu,
    0x1fff1fffu,
    0x007fffffu,
    0x0001ffffu,
    0x007fffffu,
    0x0001ffffu,
    0x007fffffu,
    0x3fff3fffu,
    0x1f7fffffu,
    0x1f7fffffu,
    0x1f7fffffu,
    0x007fffffu,
    0x0000ffffu,
    0xff3fffffu,
    0x003fffffu,
    0x00003f7fu,
    0x00003f7fu,
    0x007fffffu,
    0x007fffffu,
    0x007fffffu,
    0x1fff1fffu,
    0x007fffffu,
    0x007fffffu,
    0x007fffffu,
    0x007fffffu,
    0xf0ffffffu,
};
const uint32_t wmOffsets[9][4]={
    {0x4fe,0x507,0x510,0x519}, // DATA_URGENCY_WATERMARK
    {0x506,0x50f,0x518,0x521}, // FRAC_URG_BW_FLIP
    {0x505,0x50e,0x517,0x520}, // FRAC_URG_BW_NOM
    {0x500,0x509,0x512,0x51b}, // REFCYC_PER_TRIP_TO_MEMORY
    {0x501,0x50a,0x513,0x51c}, // ALLOW_SR_ENTER_WATERMARK
    {0x502,0x50b,0x514,0x51d}, // ALLOW_SR_EXIT_WATERMARK
    {0x503,0x50c,0x515,0x51e}, // UCLK_PSTATE_CHANGE_WATERMARK
    {0x504,0x50d,0x516,0x51f}, // FCLK_PSTATE_CHANGE_WATERMARK
    {0x4ff,0x508,0x511,0x51a}, // USR_RETRAINING_WATERMARK
};
const uint32_t wmMasks[9]={0x3fff,0x3ff,0x3ff,0x3fff,0xffff,0xffff,0xffff,0xffff,0x3fff};
constexpr uint32_t kComp=0x4ba,kDet=0x4bb,kTimer=0x525,kReserved=0x4c4,kDepth=0x4c5;
// Controls are always-on PGFSM/OTG; HUBP reads only follow a stable on state.
const uint32_t pipeGuardOffsets[]={0x80,0x81,0x1b41,0x1b85,0x1b8a,0x1b44,
    0x5f3,0x5f4,0x61a,0x61b,0x678,0x684,0x5f6,0x609,0x642,0x5e5,0x5e7,0x5ea};
uint32_t stride(size_t n){return n<2?2:n<6?0x80:0xdc;}
struct Dmu { unsigned count;bool valid;uint64_t base; };
void collect(const DiscoveryIp& ip,void* ctx){
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(ctx);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=3;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool populate(uint64_t base,uint64_t bar,Layout& out){
    out={};if(bar<4||base>UINT32_MAX/4)return false;
    uint32_t addresses[320]={};size_t count=0;
    auto add=[&](uint32_t offset){
        const uint64_t a=(base+offset)*4;
        if(a>UINT32_MAX||a>bar-4||count>=320)return false;
        for(size_t i=0;i<count;++i)if(addresses[i]==a)return false;
        addresses[count++]=uint32_t(a);return true;
    };
    for(size_t p=0;p<4;++p){
        for(size_t n=0;n<sizeof(pipeGuardOffsets)/sizeof(pipeGuardOffsets[0]);++n)
            if(!add(pipeGuardOffsets[n]+uint32_t(p)*stride(n)))return false;
        if(!add(0x5f8+uint32_t(p)*0xdc))return false;
        for(auto offset:timingOffsets)if(!add(offset+uint32_t(p)*0xdc))return false;
    }
    for(const auto& row:wmOffsets)for(auto offset:row)if(!add(offset))return false;
    const uint32_t globals[]={kComp,kDet,kDet+1,kDet+2,kDet+3,kTimer,kReserved,kDepth};
    for(auto offset:globals)
        if(!add(offset))return false;
    out={true,uint32_t(base),bar};return true;
}
bool validLayout(const Layout& l){Layout actual={};return l.valid&&populate(l.baseDwords,l.barBytes,actual);}
bool validPoll(const Poll& p){return p.iterations&&p.intervalUs&&p.intervalUs<=100&&uint64_t(p.iterations)*p.intervalUs<=100000;}
}
Plan::Plan():valid_(false),identity_{},pipe_(0),watermarkSources_{},timing_{},watermarks_{},detSegments_(0),
    compbufSegments_(0),vready_(0),timerDivider_(0),width_(0),height_(0),dccgReferenceKHz_(0),dchubReferenceKHz_(0){}
Status plan(const Input& in,Plan& out){
    out=Plan();
    if(in.profile!=Profile::SingleLinearRgb32DirectVram||in.fieldCount!=kFieldCount||in.pipe>=4||
        !in.identity.deviceGeneration||!in.identity.modeGeneration||!in.identity.clockGeneration||!in.identity.calculationId||
        !in.width||!in.height||in.width>16383||in.height>16383||
        in.htotal<=in.width||in.htotal>32768||in.vtotal<=in.height||in.vtotal>32768||
        in.vblankEnd>=in.vtotal-in.height||!in.vstartupStart||in.vstartupStart>=in.vtotal||
        !in.vupdateWidth||in.vreadyOffset>32767||in.vupdateWidth>32767||in.vupdateOffset>32767||
        in.timerDivider>15||!in.dccgReferenceKHz||in.dccgReferenceKHz>120000||
        in.dchubReferenceKHz<40000||in.dchubReferenceKHz>60000||
        in.dchubReferenceKHz!=in.dccgReferenceKHz/(in.timerDivider==2?2:1)||
        in.detKbytes<128||in.detKbytes>1152||in.compbufKbytes<128||
        (in.detKbytes&63)||(in.compbufKbytes&63)||uint64_t(in.detKbytes)+in.compbufKbytes>1280)
        return Status::InvalidPlan;
    auto f=[&](Field which){return in.fields[size_t(which)];};
    if(f(Field::rq_plane1_base_address)||f(Field::dlg_dlg_vblank_end)!=in.vblankEnd||
        !f(Field::dlg_refcyc_per_htotal)||!f(Field::dlg_ref_freq_to_pix_freq)||
        !f(Field::dlg_min_dst_y_next_start)||!f(Field::dlg_vratio_prefetch)||
        !f(Field::dlg_refcyc_per_line_delivery_l)||!f(Field::ttu_refcyc_per_req_delivery_l)||
        !f(Field::ttu_min_ttu_vblank)||f(Field::ttu_qos_level_low_wm)>f(Field::ttu_qos_level_high_wm))
        return Status::InvalidPlan;
    const uint64_t syncPixels=uint64_t(in.vreadyOffset)+in.vupdateWidth+in.vupdateOffset;
    if(syncPixels/in.htotal>in.vstartupStart)return Status::InvalidPlan; // no unsigned underflow
    Plan built;
    for(size_t n=0;n<kFieldCount;++n){
        const auto& spec=fields[n];
        if(in.fields[n]>(spec.mask>>spec.shift))return Status::FieldOverflow;
        built.timing_[spec.reg]|=in.fields[n]<<spec.shift;
    }
    // Linux dcn32_fpu converts DML us/fractions to uint32 ns/per-thousand.
    // dcn32_hubbub then truncates ns * integer refclk MHz / 1000. Preserve
    // those units, use wide multiplication, and reject instead of saturating
    // a watermark that the hardware cannot represent.
    for(size_t s=0;s<4;++s){
        const auto& w=in.watermarks[s];
        if(!w.sourceId||!w.urgentNs||!w.urgentLatencyNs||
            w.fracUrgentFlipPerThousand>1000||w.fracUrgentNomPerThousand>1000)
            return Status::InvalidPlan;
        for(size_t previous=0;previous<s;++previous)
            if(in.watermarks[previous].sourceId==w.sourceId)return Status::InvalidPlan;
        built.watermarkSources_[s]=w.sourceId;
        const uint32_t values[]={w.urgentNs,w.fracUrgentFlipPerThousand,w.fracUrgentNomPerThousand,
            w.urgentLatencyNs,w.stutterEnterExitNs,w.stutterExitNs,w.dramChangeNs,w.fabricChangeNs,w.retrainingNs};
        for(size_t n=0;n<9;++n){
            const uint64_t v=(n==1||n==2)?values[n]:uint64_t(values[n])*(in.dchubReferenceKHz/1000)/1000;
            if(v>wmMasks[n]||((n==0||n==3)&&!v))return Status::FieldOverflow;
            built.watermarks_[s][n]=uint32_t(v);
        }
    }
    built.identity_=in.identity;built.pipe_=in.pipe;
    built.detSegments_=in.detKbytes/64;built.compbufSegments_=in.compbufKbytes/64;
    built.vready_=in.vstartupStart-syncPixels/in.htotal<=in.vblankEnd?0x100:0;
    built.timerDivider_=in.timerDivider;built.dccgReferenceKHz_=in.dccgReferenceKHz;
    built.width_=in.width;built.height_=in.height;
    built.dchubReferenceKHz_=in.dchubReferenceKHz;built.valid_=true;out=built;return Status::Ok;
}
Status buildLayout(const uint8_t* bytes,size_t length,uint64_t bar,Layout& out){
    out={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(bytes,length,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.base,bar,out)?Status::Ok:Status::OutsideBar;
}
Controller::Controller():layout_{},io_{},plan_{},phase_(Phase::Closed),touched_(false),powerStatus_{},powerConfig_{},powerValid_{}{}
void Controller::clearAfterVerifiedReset(){layout_={};io_={};plan_=Plan();phase_=Phase::Closed;touched_=false;for(unsigned p=0;p<4;++p){powerStatus_[p]=powerConfig_[p]=0;powerValid_[p]=false;}}
Status Controller::finish(Status s,Result& r){
    r.status=s;r.retentionRequired=touched_;
    if(s!=Status::Ok)phase_=touched_?Phase::Faulted:Phase::Closed;
    return s;
}
Status Controller::read(uint32_t offset,uint32_t& value,Result& r){
    if(!io_.valid(io_.context,plan_.identity_))return Status::Revoked;
    r.lastRegister=(layout_.baseDwords+offset)*4;++r.reads;
    if(!io_.read32(io_.context,r.lastRegister,value))return Status::TransportError;
    r.lastValue=value;return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::power(uint8_t p,uint32_t& state,Result& r){
    auto s=read(0x81+p*2,state,r);if(s!=Status::Ok)return s;
    if(state!=0&&state!=0x10000000&&state!=0x80000000&&state!=0x90000000)return Status::PowerUnavailable;
    uint32_t config=0,after=0;s=read(0x80+p*2,config,r);if(s!=Status::Ok)return s;
    if((state&0x80000000)?config!=0x100:config>1)return Status::PowerUnavailable;
    if(powerValid_[p]&&(state!=powerStatus_[p]||config!=powerConfig_[p]))return Status::PowerUnavailable;
    s=read(0x81+p*2,after,r);if(s!=Status::Ok)return s;
    if(state!=after)return Status::PowerUnavailable;
    s=read(0x80+p*2,after,r);if(s!=Status::Ok)return s;
    if(config!=after)return Status::PowerUnavailable;
    s=read(0x81+p*2,after,r);if(s!=Status::Ok)return s;
    if(state!=after)return Status::PowerUnavailable;
    powerStatus_[p]=state;powerConfig_[p]=config;powerValid_[p]=true;return Status::Ok;
}
Status Controller::guard(Result& r){
    uint32_t states[4]={},value=0;
    for(uint8_t p=0;p<4;++p){
        auto s=power(p,states[p],r);if(s!=Status::Ok)return s;
        // OTG always-on domain checks remain necessary even when HUBP is gated.
        const uint32_t otgOffsets[]={0x1b41,0x1b85,0x1b8a};
        for(uint32_t offset:otgOffsets){
            s=read(offset+p*0x80,value,r);if(s!=Status::Ok)return s;
            const uint32_t mask=offset==0x1b41?0x10001:offset==0x1b85?0x10000:0x101;
            if(value&mask)return offset==0x1b8a?Status::ForeignControl:Status::ActivePipe;
        }
        if(states[p]&0x80000000){if(p==plan_.pipe_)return Status::PowerUnavailable;continue;}
        s=read(0x5f3+p*0xdc,value,r);if(s!=Status::Ok)return s;
        if(value&0x70f00800)return Status::HardwareFault;
        if((value&0xf)!=0xb)return Status::ActivePipe;
    }
    const uint32_t p=plan_.pipe_*0xdc;
    auto s=read(0x5f4+p,value,r);if(s!=Status::Ok)return s;
    if(!(value&1))return Status::ClockUnavailable;
    // Require the plain memory-fetch profile already established by scanout
    // ownership. Disabled VM/DCC/cursor paths can still have timing values.
    const uint32_t offsets[]={0x61a,0x61b,0x678,0x684,0x5f6,0x609,0x642,0x5f3,0x5e5,0x5e7};
    const uint32_t masks[]={0xf7f7f,~0x102u,1,7,7,0xffffffff,0xffffffff,0x1400,0xffffffff,~0xa00u};
    const uint32_t wanted[]={0,0,0,0,0,0,0x19,0,8,0};
    for(size_t n=0;n<sizeof(offsets)/sizeof(offsets[0]);++n){
        s=read(offsets[n]+p,value,r);if(s!=Status::Ok)return s;
        if((value&masks[n])!=wanted[n])return Status::ForeignControl;
    }
    s=read(0x61b+p,value,r);if(s!=Status::Ok)return s;
    if(value&0x100)return Status::ActivePipe;
    s=read(0x5ea+p,value,r);if(s!=Status::Ok)return s;
    if(value!=(plan_.width_|(plan_.height_<<16)))return Status::ForeignControl;
    s=read(0x1b44+plan_.pipe_*0x80,value,r);if(s!=Status::Ok)return s;
    if(value&1)return Status::ForeignControl;
    s=read(kTimer,value,r);if(s!=Status::Ok)return s;
    if((value&0x100f)!=(0x1000|plan_.timerDivider_))return Status::ClockUnavailable;
    for(uint8_t q=0;q<4;++q){uint32_t state=0;s=power(q,state,r);if(s!=Status::Ok)return s;
        if(state!=states[q])return Status::PowerUnavailable;}
    return Status::Ok;
}
Status Controller::update(uint32_t offset,uint32_t mask,uint32_t value,Result& r,uint32_t clear){
    auto s=guard(r);if(s!=Status::Ok)return s;
    uint32_t old=0;s=read(offset,old,r);if(s!=Status::Ok)return s;
    if(value&~mask)return Status::InvalidPlan;
    const uint32_t next=((old&~mask)|value)&~clear;
    if(!io_.valid(io_.context,plan_.identity_))return Status::Revoked;
    // Failed transport may still deliver: retain partial ownership beforehand.
    touched_=r.writeAttempted=true;++r.writes;r.lastRegister=(layout_.baseDwords+offset)*4;r.lastValue=next;
    if(!io_.write32(io_.context,r.lastRegister,next))return Status::TransportError;
    s=read(offset,old,r);if(s!=Status::Ok)return s;
    return (old&mask)==value?Status::Ok:Status::VerifyFailed;
}
Status Controller::waitBuffer(uint32_t offset,uint32_t size,const Poll& poll,Result& r){
    for(uint32_t n=0;n<poll.iterations;++n){
        auto s=guard(r);if(s!=Status::Ok)return s;
        uint32_t value=0;s=read(offset,value,r);if(s!=Status::Ok)return s;++r.polls;
        if(offset==kComp&&(value&0x80000000))return Status::HardwareFault;
        if((value&31)!=size)return Status::VerifyFailed;
        if(((value>>8)&31)==size)return Status::Ok;
        if(n+1<poll.iterations)io_.delayUs(io_.context,poll.intervalUs);
    }
    return Status::Timeout;
}
Status Controller::buffers(const Poll& poll,Result& r){
    uint32_t current[5]={},desired[5]={plan_.compbufSegments_,0,0,0,0};
    desired[1+plan_.pipe_]=plan_.detSegments_;
    uint32_t total=0;
    for(size_t n=0;n<5;++n){
        uint32_t v=0;auto s=read(kComp+uint32_t(n),v,r);if(s!=Status::Ok)return s;
        if(n==0&&(v&0x80000000))return Status::HardwareFault;
        current[n]=v&31;
        if(((v>>8)&31)!=current[n])return Status::BufferState;
        total+=current[n];
    }
    if(total>20)return Status::BufferState;
    // dcn32_init_crb: 8-KiB IP pixel chunk /32 and /128, DET_DEPTH=0x47f.
    // These are pinned DCN3.2 ASIC constants, never inferred board clocks.
    auto s=update(kReserved,0x0fff0fff,0x00400100,r);if(s!=Status::Ok)return s;
    s=update(kDepth,0x07ff0000,0x047f0000,r);if(s!=Status::Ok)return s;
    // Shrink before growing, and wait on SIZE_CURRENT (not requested SIZE).
    // This handles a former multi-pipe allocation without transient CRB overrun.
    for(unsigned pass=0;pass<2;++pass)for(size_t n=0;n<5;++n){
        if((pass==0?desired[n]>=current[n]:desired[n]<=current[n]))continue;
        const uint32_t nextTotal=total-current[n]+desired[n];
        if(nextTotal>20)return Status::BufferState;
        s=update(kComp+uint32_t(n),31,desired[n],r,n==0?0x80080000:0);
        if(s!=Status::Ok)return s;
        s=waitBuffer(kComp+uint32_t(n),desired[n],poll,r);if(s!=Status::Ok)return s;
        total=nextTotal;current[n]=desired[n];
    }
    uint32_t v=0;s=read(kComp,v,r);if(s!=Status::Ok)return s;
    return (v&0x80000000)?Status::HardwareFault:Status::Ok;
}
Status Controller::configure(const Layout& l,const Io& io,const Plan& p,const Poll& poll,Result& r){
    r={};
    if(phase_!=Phase::Closed&&phase_!=Phase::Retired){r.status=Status::InvalidState;r.retentionRequired=touched_;return r.status;}
    if(!validLayout(l)||!p.valid_||!validPoll(poll)||!io.enter||!io.leave||!io.valid||!io.read32||!io.write32||!io.delayUs)
        return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;plan_=p;for(auto& observed:powerValid_)observed=false;
    if(!io_.enter(io_.context,p))return finish(Status::OwnershipRejected,r);
    phase_=Phase::Programming;
    auto s=guard(r);
    if(s==Status::Ok)s=buffers(poll,r);
    const uint32_t pipe=plan_.pipe_*0xdc;
    // hubp2_vready_at_or_After_vsync: disable DV test mode, preserving unrelated
    // chicken bits, then set VREADY without replaying status/W1C fields.
    if(s==Status::Ok)s=update(0x5f8+pipe,0x100,0x100,r);
    if(s==Status::Ok)s=update(0x5f3+pipe,0x100,plan_.vready_,r,0x84000000);
    for(size_t n=0;s==Status::Ok&&n<kTimingRegisterCount;++n)
        s=update(timingOffsets[n]+pipe,timingMasks[n],plan_.timing_[n],r);
    // All requestors are blank/drained and clocks held: lower/raise is safe.
    // This does not grant SMU clock switching, self refresh, or retraining.
    for(size_t set=0;s==Status::Ok&&set<4;++set)for(size_t n=0;s==Status::Ok&&n<9;++n)
        s=update(wmOffsets[n][set],wmMasks[n],plan_.watermarks_[set][n],r);
    if(s==Status::Ok)s=guard(r);
    if(s==Status::Ok)phase_=Phase::Configured;
    io_.leave(io_.context);return finish(s,r);
}
Status Controller::retire(Result& r){
    r={};
    if(!touched_){r.status=Status::InvalidState;return r.status;}
    if(!io_.enter(io_.context,plan_))return finish(Status::OwnershipRejected,r);
    auto s=guard(r);
    for(uint32_t n=0;s==Status::Ok&&n<5;++n){
        uint32_t value=0;s=read(kComp+n,value,r);
        if(s!=Status::Ok)break;
        if(n==0&&(value&0x80000000)){s=Status::HardwareFault;break;}
        if((value&31)!=((value>>8)&31))s=Status::BufferState;
    }
    io_.leave(io_.context);
    if(s==Status::Ok){touched_=false;phase_=Phase::Retired;}
    return finish(s,r);
}
} }
