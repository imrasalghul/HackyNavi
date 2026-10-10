// SPDX-License-Identifier: MIT
// GC11 PSP initialization adapted from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// gfx_v11_0.c:config_gfx_rs64,constants_init,init_gds_vmid,init_csb,rlc_enable_srm;
// soc21.c:soc21_grbm_select; gc_11_0_0_offset.h/sh_mask.h; clearstate_gfx11.h.
// Retirement combines gfx_v11_0_set_safe_mode/rlc_stop with the generated
// GPM/SRM/FIFO status fields as a conservative terminal port policy.
// Copyright 2020-2021 Advanced Micro Devices, Inc.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
#include "hackynavi/gfx_init.hpp"
namespace hackynavi {
namespace gfxinit {
namespace {
struct cs_extent_def { const unsigned int* extent; uint32_t reg_index,reg_count; };
enum { SECT_NONE,SECT_CONTEXT };
struct cs_section_def { const cs_extent_def* section; unsigned id; };
// Verbatim AMD table from the pin above; its full copyright/license is retained.
#include "gfx11_clearstate_data.hpp"
enum Reg { Select,MeControl,MecControl,PfpPc,PfpPcHi,MePc,MePcHi,MecPc,MecPcHi,
    CpStatus,Bootload,Mes,ImuReset,GfxIsa,MecIsa,Tcp,Grbm,DbRing,ShMem,
    CsbHi,CsbLo,CsbLength,Srm,VmContext, Gds=VmContext+16, Count=Gds+15*4 };
struct Registers { uint32_t r[Count]; };
enum RetireReg { RlcControl,SafeMode,GpmStatus,SrmStatus,SrmCommandStatus,RlcStatus,Utcl1Status,
    GrbmStatus2,RbActive,RbActive1,RetireCount };
struct RetireRegisters { uint32_t r[RetireCount]; };
bool gcValid(const Navi31IpLayout& ip) {
    const auto& gc=ip.blocks[static_cast<size_t>(Navi31Block::Gc)];
    return ip.valid && gc.hardwareId==11 && gc.major==11 && !gc.minor && !gc.revision &&
        !gc.instance && !gc.dieId && gc.baseCount>=2 && gc.baseCount<=kNavi31MaxBases;
}
void put(uint8_t* p,uint32_t value) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i)); }
uint32_t packet(uint32_t op,uint32_t count) { return 0xc0000000U|(count<<16)|(op<<8); }
Status resolve(const Navi31IpLayout& ip,uint64_t bytes,Registers& out) {
    if(!gcValid(ip))return Status::UnsupportedLayout;
    const uint32_t offsets[]={0x900,0x803,0x2904,0x1e44,0x1e59,0x1e45,0x1e79,0x2900,0x2938,
        0xf40,0x4e82,0x2807,0x40bc,0x2a00,0x2903,0x19a2,0xda0,0x13bb,0x9e4,0x988,0x987,0x989,0x4c80};
    const uint32_t bases[]={1,1,1,0,0,0,0,1,1,0,1,1,1,1,1,1,0,0,1,1,1,1,1};
    for(unsigned i=0;i<Count;++i) {
        uint32_t off=0,base=0;
        if(i<VmContext) { off=offsets[i]; base=bases[i]; }
        else if(i<Gds)off=0x1688+i-VmContext;
        else {
            const unsigned vmid=1+(i-Gds)/4,which=(i-Gds)%4;
            off=which<2?0x20a0+2*vmid+which:which==2?0x20c0+vmid:0x20d0+vmid;
        }
        ResolvedRegister r={};
        if(resolveNavi31Register(ip,Navi31Block::Gc,base,off,bytes,r)!=IpLayoutStatus::Ok)return Status::OutsideBar;
        if(r.mmioByteOffset<0x1c || (r.mmioByteOffset>=0x7f000 && r.mmioByteOffset<0x80000))return Status::OutsideBar;
        out.r[i]=r.mmioByteOffset;
        for(unsigned j=0;j<i;++j)if(out.r[j]==out.r[i])return Status::UnsupportedLayout;
    }
    return Status::Ok;
}
Status finish(Result& r,Status status) { r.status=status; return status; }
Status resolveRetirement(const Navi31IpLayout& ip,uint64_t bytes,RetireRegisters& out) {
    const uint32_t offsets[]={0x4c00,0x980,0x4e6b,0x4c9b,0x4c88,0x4c04,0x4cb6,0xda2,0x1f40,0x1f41};
    for(unsigned i=0;i<RetireCount;++i) {
        ResolvedRegister r={};
        if(resolveNavi31Register(ip,Navi31Block::Gc,i>=GrbmStatus2?0:1,offsets[i],bytes,r)!=IpLayoutStatus::Ok)
            return Status::OutsideBar;
        if(r.mmioByteOffset<0x1c || (r.mmioByteOffset>=0x7f000 && r.mmioByteOffset<0x80000))return Status::OutsideBar;
        out.r[i]=r.mmioByteOffset;
        for(unsigned j=0;j<i;++j)if(out.r[j]==out.r[i])return Status::UnsupportedLayout;
    }
    return Status::Ok;
}
bool read(const Io& io,uint32_t reg,uint32_t& value,Result& r) {
    r.lastRegister=reg; ++r.reads;
    if(!io.read32(io.context,reg,value)) { finish(r,Status::ReadFailed); return false; }
    r.lastValue=value;
    if(value==UINT32_MAX) { finish(r,Status::DeviceUnavailable); return false; }
    return true;
}
bool write(const Io& io,uint32_t reg,uint32_t value,Result& r) {
    r.lastRegister=reg; r.lastValue=value; ++r.writes; r.registerWritesMayHaveOccurred=true;
    if(!io.write32(io.context,reg,value)) { finish(r,Status::WriteFailed); return false; }
    return true;
}
bool matches(const Io& io,uint32_t reg,uint32_t mask,uint32_t expected,Result& r,Status failure=Status::ReadbackMismatch) {
    uint32_t value=0;
    if(!read(io,reg,value,r))return false;
    if((value&mask)!=expected) { finish(r,failure); return false; }
    return true;
}
bool store(const Io& io,uint32_t reg,uint32_t value,Result& r) {
    return write(io,reg,value,r) && matches(io,reg,UINT32_MAX,value,r);
}
bool rmw(const Io& io,uint32_t reg,uint32_t mask,uint32_t value,Result& r) {
    uint32_t old=0;
    return read(io,reg,old,r) && write(io,reg,(old&~mask)|(value&mask),r) && matches(io,reg,mask,value&mask,r);
}
bool guards(const Registers& regs,const Io& io,Result& r) {
    if(!matches(io,regs.r[CpStatus],UINT32_MAX,0,r,Status::ActiveEngine) ||
        !matches(io,regs.r[Bootload],0x80000000,0x80000000,r,Status::ActiveEngine) ||
        !matches(io,regs.r[Mes],0x7c000000,0x40000000,r,Status::ActiveEngine) ||
        !matches(io,regs.r[ImuReset],0x1f,0x1f,r,Status::ActiveEngine) ||
        !matches(io,regs.r[GfxIsa],1,1,r,Status::UnsupportedArchitecture) ||
        !matches(io,regs.r[MecIsa],1,1,r,Status::UnsupportedArchitecture) ||
        !matches(io,regs.r[MecControl],0x3c000000,0,r,Status::ActiveEngine))return false;
    for(unsigned i=0;i<16;++i)
        if(!matches(io,regs.r[VmContext+i],1,i?0:1,r,Status::ActiveEngine))return false;
    return true;
}
bool retirementGuards(const Registers& regs,const RetireRegisters& extra,const Io& io,Result& r) {
    return guards(regs,io,r) && matches(io,regs.r[Select],UINT32_MAX,0,r,Status::ActiveEngine) &&
        matches(io,regs.r[MeControl],0x143c0000,0x14000000,r,Status::ActiveEngine) &&
        matches(io,regs.r[MecControl],0x7c0f0000,0x40000000,r,Status::ActiveEngine) &&
        matches(io,extra.r[RbActive],1,0,r,Status::ActiveEngine) &&
        matches(io,extra.r[RbActive1],1,0,r,Status::ActiveEngine);
}
bool waitMasked(const Io& io,const RetirementIo& retirement,const RetirementPollOptions& p,
    uint32_t reg,uint32_t mask,uint32_t expected,Result& r) {
    for(uint32_t i=0;i<p.iterations;++i) {
        uint32_t value=0; ++r.polls;
        if(!read(io,reg,value,r))return false;
        if((value&mask)==expected)return true;
        if(i+1<p.iterations)retirement.delayUs(retirement.context,p.intervalUs);
    }
    finish(r,Status::QuiesceTimeout); return false;
}
// GPM's activity fields: RLC_BUSY, context/gfx/compute transitions, register
// save/restore and WGP/pipeline power transitions. The remaining bits report
// steady power/clock state. PG_ERROR_STATUS is also required clear.
constexpr uint32_t kGpmActivityAndError=0xff01ffe1U;
bool drainRlc(const Io& io,const RetirementIo& retirement,const RetirementPollOptions& p,
    const Registers& regs,const RetireRegisters& extra,Result& r) {
    for(uint32_t i=0;i<p.iterations;++i) {
        uint32_t gpm=0,srm=0,fifo=0,cp=0,rlc=0,utcl=0,grbm=0; ++r.polls;
        if(!read(io,extra.r[GpmStatus],gpm,r) || !read(io,extra.r[SrmStatus],srm,r) ||
            !read(io,extra.r[SrmCommandStatus],fifo,r) || !read(io,regs.r[CpStatus],cp,r) ||
            !read(io,extra.r[RlcStatus],rlc,r) || !read(io,extra.r[Utcl1Status],utcl,r) ||
            !read(io,extra.r[GrbmStatus2],grbm,r))return false;
        // GPM software activity alone is not a memory drain: require all
        // hardware RLC/MC/thread busy, translation busy/stall, and GRBM RLC
        // pending-request signals clear as well. Do not mask these with SRM.
        if(!(gpm&kGpmActivityAndError) && !(srm&3) && (fifo&3)==1 && !cp &&
            !(rlc&0xff) && !(utcl&0x1ef) && !(grbm&0x04004000))return true;
        if(i+1<p.iterations)retirement.delayUs(retirement.context,p.intervalUs);
    }
    finish(r,Status::QuiesceTimeout); return false;
}
} // namespace

Status encodeClearState(const Navi31IpLayout& ip,uint8_t* bytes,size_t size) {
    if(!bytes || size<kClearStateBytes)return Status::InvalidArgument;
    if(!gcValid(ip))return Status::UnsupportedLayout;
    ResolvedRegister tile={};
    if(resolveNavi31Register(ip,Navi31Block::Gc,1,0xd7,UINT64_MAX,tile)!=IpLayoutStatus::Ok ||
        tile.registerDword<0xa000 || tile.registerDword>=0xa400)return Status::UnsupportedLayout;
    size_t count=12;
    for(const cs_section_def* s=gfx11_cs_data;s->section;++s) {
        if(s->id!=SECT_CONTEXT)return Status::UnsupportedLayout;
        for(const cs_extent_def* e=s->section;e->extent;++e)count+=2+e->reg_count;
    }
    if(count!=kClearStateDwords)return Status::UnsupportedLayout;
    size_t cursor=0;
#define EMIT(value) do { put(bytes+4*cursor++,value); } while(0)
    EMIT(packet(0x4a,0)); EMIT(0x20000000); EMIT(packet(0x28,1)); EMIT(0x80000000); EMIT(0x80000000);
    for(const cs_section_def* s=gfx11_cs_data;s->section;++s) {
        for(const cs_extent_def* e=s->section;e->extent;++e) {
            EMIT(packet(0x69,e->reg_count)); EMIT(e->reg_index-0xa000);
            for(uint32_t i=0;i<e->reg_count;++i)EMIT(e->extent[i]);
        }
    }
    EMIT(packet(0x69,1)); EMIT(tile.registerDword-0xa000); EMIT(0);
    EMIT(packet(0x4a,0)); EMIT(0x30000000); EMIT(packet(0x12,0)); EMIT(0);
#undef EMIT
    return Status::Ok;
}
struct Access {
    static Status finish(State& s,Result& r,Status status) {
        r.clearStateRetentionRequired=s.retained_; return gfxinit::finish(r,status);
    }
    static Status prepare(const Navi31IpLayout& ip,const gfxfw::Files& files,const gfxfw::State& firmware,
        const smu::State& smu,const VramAllocator& allocator,VramAllocationHandle csb,uint8_t* scratch,size_t scratchBytes,
        const VramTransferIo& transfer,const Io& io,const HdpIo& hdpIo,HdpState& hdp,State& s,Result& r) {
        r=Result{};
        if(s.started_ || !firmware.autoloadComplete() || !smu.awake() || !hdp.configured())return finish(s,r,Status::InvalidState);
        if(!scratch || scratchBytes<4096 || !transfer.read32 || !transfer.write32 || !io.read32 || !io.write32 ||
            !hdpIo.read32 || !hdpIo.write32)return finish(s,r,Status::InvalidArgument);
        Registers regs={}; const auto resolved=resolve(ip,transfer.barBytes,regs);
        if(resolved!=Status::Ok)return finish(s,r,resolved);
        GfxFirmwareV2 fw[3]={}; const gfxfw::View views[]={files.pfp,files.me,files.mec};
        for(unsigned i=0;i<3;++i) {
            if(parseGfxFirmwareV2(views[i].bytes,views[i].size,fw[i])!=hackynavi::Status::Ok ||
                fw[i].header.ipMajor!=11 || fw[i].header.ipMinor || (fw[i].startAddress&3))return finish(s,r,Status::FirmwareRejected);
        }
        VramRange range={}; uint64_t mc=0,physical=0;
        if(allocator.lookup(csb,range)!=VramAllocatorStatus::Ok ||
            allocator.addresses(csb,mc,physical)!=VramAllocatorStatus::Ok || (mc&4095))return finish(s,r,Status::InvalidAllocation);
        if(range.bytes<4096)return finish(s,r,Status::OutsideAllocation);
        const auto encoded=encodeClearState(ip,scratch,scratchBytes);
        if(encoded!=Status::Ok)return finish(s,r,encoded);
        for(size_t i=kClearStateBytes;i<4096;++i)scratch[i]=0;
        s.started_=true; r.stage=Stage::Guard;
        if(!guards(regs,io,r))return finish(s,r,r.status);
        r.stage=Stage::UploadClearState;
        if(uploadVram(allocator,csb,0,scratch,4096,transfer,r.transfer)!=VramTransferStatus::Ok)return finish(s,r,Status::TransferFailed);
        r.stage=Stage::PublishClearState; r.hdpStatus=flushHdp(hdpIo,hdp,r.hdp);
        if(r.hdpStatus!=HdpStatus::Ok)return finish(s,r,Status::VisibilityFailed);
        r.stage=Stage::Halt;
        if(!rmw(io,regs.r[MeControl],0x14000000,0x14000000,r) ||
            !rmw(io,regs.r[MecControl],0x40000000,0x40000000,r))return finish(s,r,r.status);
        const Reg lo[]={PfpPc,MePc,MecPc},hi[]={PfpPcHi,MePcHi,MecPcHi};
        const uint32_t resetMasks[]={0x000c0000,0x00300000,0x000f0000};
        for(unsigned engine=0;engine<3;++engine) {
            r.stage=Stage::ProgramCounter;
            const uint64_t pc=fw[engine].startAddress>>2;
            for(unsigned pipe=0;pipe<(engine==2?4U:2U);++pipe) {
                if(!store(io,regs.r[Select],pipe|(engine==2?4U:0U),r) ||
                    !store(io,regs.r[lo[engine]],uint32_t(pc),r) || !store(io,regs.r[hi[engine]],uint32_t(pc>>32),r))
                    return finish(s,r,r.status);
            }
            if(!store(io,regs.r[Select],0,r))return finish(s,r,r.status);
            r.stage=Stage::PipeReset; const uint32_t reg=regs.r[engine==2?MecControl:MeControl];
            if(!rmw(io,reg,resetMasks[engine],resetMasks[engine],r) || !rmw(io,reg,resetMasks[engine],0,r))return finish(s,r,r.status);
        }
        r.stage=Stage::Golden;
        if(!rmw(io,regs.r[Tcp],0x20000000,0x20000000,r))return finish(s,r,r.status);
        r.stage=Stage::Constants;
        if(!rmw(io,regs.r[Grbm],0xff,0xff,r) ||
            !rmw(io,regs.r[DbRing],3,1,r) || // One active graphics pipe: counters go to pipe0.
            !store(io,regs.r[Select],0,r) || !store(io,regs.r[ShMem],0xc00c,r))return finish(s,r,r.status);
        for(unsigned i=Gds;i<Count;++i)if(!store(io,regs.r[i],0,r))return finish(s,r,r.status);
        r.stage=Stage::ClearStatePointer; s.retained_=true; s.csb_=csb;
        if(!store(io,regs.r[CsbHi],uint32_t(mc>>32),r) || !store(io,regs.r[CsbLo],uint32_t(mc),r) ||
            !store(io,regs.r[CsbLength],uint32_t(kClearStateDwords),r))return finish(s,r,r.status);
        r.stage=Stage::Srm;
        if(!rmw(io,regs.r[Srm],3,3,r))return finish(s,r,r.status);
        s.ready_=true; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
    static Status retire(const Navi31IpLayout& ip,uint64_t barBytes,const smu::State& smu,
        const VramAllocator& allocator,const RetirementIo& retirement,const RetirementPollOptions& p,
        State& s,Result& r) {
        r=Result{};
        if(!s.ready_ || !s.retained_ || s.retirementStarted_ || !smu.awake())return finish(s,r,Status::InvalidState);
        if(!retirement.read32 || !retirement.write32 || !retirement.delayUs || !p.iterations ||
            !p.intervalUs || p.iterations>1000000 || p.intervalUs>1000 ||
            uint64_t(p.iterations)*p.intervalUs>10000000)return finish(s,r,Status::InvalidArgument);
        Registers regs={}; RetireRegisters extra={};
        Status resolved=resolve(ip,barBytes,regs);
        if(resolved!=Status::Ok)return finish(s,r,resolved);
        resolved=resolveRetirement(ip,barBytes,extra);
        if(resolved!=Status::Ok)return finish(s,r,resolved);
        for(unsigned i=0;i<RetireCount;++i)for(unsigned j=0;j<Count;++j)
            if(extra.r[i]==regs.r[j])return finish(s,r,Status::UnsupportedLayout);
        VramRange range={}; uint64_t mc=0,physical=0;
        if(allocator.lookup(s.csb_,range)!=VramAllocatorStatus::Ok || range.bytes<4096 ||
            allocator.addresses(s.csb_,mc,physical)!=VramAllocatorStatus::Ok || (mc&4095))
            return finish(s,r,Status::InvalidAllocation);
        const Io io={retirement.context,retirement.read32,retirement.write32};
        // Once hardware inspection begins, this is terminal even if a read
        // fails. A copied/stale CPU State never authorizes a second attempt.
        s.retirementStarted_=true; s.ready_=false; r.stage=Stage::RetireGuard;
        if(!retirementGuards(regs,extra,io,r) ||
            !matches(io,regs.r[CsbHi],UINT32_MAX,uint32_t(mc>>32),r) ||
            !matches(io,regs.r[CsbLo],UINT32_MAX,uint32_t(mc),r) ||
            !matches(io,regs.r[CsbLength],UINT32_MAX,kClearStateDwords,r))return finish(s,r,r.status);
        uint32_t enabled=0;
        if(!read(io,extra.r[RlcControl],enabled,r))return finish(s,r,r.status);
        if(enabled&1) {
            r.stage=Stage::EnterSafeMode;
            // Do not overwrite another RLC command. Linux enters safe mode
            // with CMD=1,MESSAGE=1 and completion is CMD clearing in hardware.
            if(!matches(io,extra.r[SafeMode],1,0,r,Status::ActiveEngine) ||
                !write(io,extra.r[SafeMode],3,r) ||
                !waitMasked(io,retirement,p,extra.r[SafeMode],1,0,r))return finish(s,r,r.status);
        }
        r.stage=Stage::DrainRlc;
        if(!rmw(io,extra.r[RlcControl],1,0,r) || !drainRlc(io,retirement,p,regs,extra,r))return finish(s,r,r.status);
        r.stage=Stage::DisableSrm;
        if(!rmw(io,regs.r[Srm],3,0,r) || !drainRlc(io,retirement,p,regs,extra,r) ||
            !retirementGuards(regs,extra,io,r))return finish(s,r,r.status);
        r.stage=Stage::RetireClearState;
        // Length first: no live descriptor can name a partially cleared
        // address. All processors/engines remain stopped through retirement.
        if(!store(io,regs.r[CsbLength],0,r) || !store(io,regs.r[CsbLo],0,r) || !store(io,regs.r[CsbHi],0,r) ||
            !matches(io,extra.r[RlcControl],1,0,r) || !matches(io,regs.r[Srm],3,0,r) ||
            !drainRlc(io,retirement,p,regs,extra,r) || !retirementGuards(regs,extra,io,r))return finish(s,r,r.status);
        s.retained_=false; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
};
Status prepare(const Navi31IpLayout& ip,const gfxfw::Files& files,const gfxfw::State& firmware,
    const smu::State& smu,const VramAllocator& allocator,VramAllocationHandle csb,uint8_t* scratch,size_t scratchBytes,
    const VramTransferIo& transfer,const Io& io,const HdpIo& hdpIo,HdpState& hdp,State& state,Result& result) {
    return Access::prepare(ip,files,firmware,smu,allocator,csb,scratch,scratchBytes,transfer,io,hdpIo,hdp,state,result);
}
Status retireClearState(const Navi31IpLayout& ip,uint64_t barBytes,const smu::State& smu,
    const VramAllocator& allocator,const RetirementIo& io,const RetirementPollOptions& p,State& state,Result& result) {
    return Access::retire(ip,barBytes,smu,allocator,io,p,state,result);
}
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(UnsupportedLayout);NAME(OutsideBar);
    NAME(FirmwareRejected);NAME(InvalidAllocation);NAME(OutsideAllocation);NAME(ActiveEngine);
    NAME(UnsupportedArchitecture);NAME(ReadFailed);NAME(WriteFailed);NAME(DeviceUnavailable);
    NAME(ReadbackMismatch);NAME(TransferFailed);NAME(VisibilityFailed);NAME(QuiesceTimeout);
#undef NAME
    }
    return "Unknown";
}
} // namespace gfxinit
} // namespace hackynavi
