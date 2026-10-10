// SPDX-License-Identifier: MIT
// GC11 legacy queue/PM4 protocol follows pinned Linux gfx_v11_0.c
// cp_gfx_resume/start/enable,ring_set_wptr_gfx,ring_emit_ib_gfx,ring_emit_fence;
// nvd.h and generated GC11.0.0 registers.
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
#include "hackynavi/gfx_queue.hpp"
namespace hackynavi {
namespace gfxqueue {
namespace {
enum Reg { Select,Me,StatusReg,Imu,Mes,Mec,Active,Active1,Delay,Vmid,Control,
    Wp,WpHi,RpAddr,RpAddrHi,PollLo,PollHi,Base,BaseHi,Doorbell,RangeLo,RangeHi,
    MaxContext,DeviceId,Vm0,Bootload,RegCount };
bool pollValid(const PollOptions& p) { return p.maxPolls && p.maxPolls<=1000000 && p.delayUs && p.delayUs<=10 && uint64_t(p.maxPolls)*p.delayUs<=1000000; }
bool ioValid(const Io& io) { return io.read32 && io.write32 && io.syncForDeviceAndHdp && io.syncForCpu && io.delayUs; }
bool range(uint64_t start,uint64_t bytes) { return bytes && start<(UINT64_C(1)<<48) && bytes<=(UINT64_C(1)<<48)-start; }
bool overlaps(uint64_t a,uint64_t an,uint64_t b,uint64_t bn) { return a<=b?b-a<an:a-b<bn; }
bool memoryValid(const Config& c,const Memory& m) {
    if(c.ringWords<2048 || c.ringWords>16384 || (c.ringWords&(c.ringWords-1)) ||
        m.capacityWords<c.ringWords || !m.ring || !m.rptr || !m.wptr || !m.fence)return false;
    const uint64_t gpu[]={c.ringGpu,c.rptrGpu,c.wptrGpu,c.fenceGpu};
    const uintptr_t cpu[]={reinterpret_cast<uintptr_t>(m.ring),reinterpret_cast<uintptr_t>(m.rptr),
        reinterpret_cast<uintptr_t>(m.wptr),reinterpret_cast<uintptr_t>(m.fence)};
    const uint64_t sizes[]={uint64_t(c.ringWords)*4,4,8,4},align[]={256,4,8,4};
    for(unsigned i=0;i<4;++i) {
        if(!range(gpu[i],sizes[i]) || (gpu[i]&(align[i]-1)) || (cpu[i]&((i==2?8:4)-1)) || sizes[i]-1>UINTPTR_MAX-cpu[i])return false;
        for(unsigned j=0;j<i;++j)if(overlaps(gpu[i],sizes[i],gpu[j],sizes[j]) || overlaps(cpu[i],sizes[i],cpu[j],sizes[j]))return false;
    }
    return true;
}
Status resolve(const Navi31IpLayout& ips,uint64_t bar,uint32_t* regs) {
    if(!ips.valid)return Status::UnsupportedLayout;
    const auto& gc=ips.blocks[0];
    if(gc.hardwareId!=11 || gc.major!=11 || gc.minor || gc.revision || gc.instance || gc.dieId)return Status::UnsupportedLayout;
    const uint32_t offsets[]={0x900,0x803,0xf40,0x40bc,0x2807,0x2904,0x1f40,0x1f41,0xf61,0x1df1,
        0x1de1,0x1df4,0x1df5,0x1de3,0x1de4,0x1e8b,0x1e8c,0x1de0,0x1e51,0x1e8d,0x1dfa,0x1dfb,
        0x1e4e,0x1deb,0x1688,0x4e82};
    for(unsigned i=0;i<RegCount;++i) {
        const unsigned base=(i==Select || i==Me || i==Imu || i==Mes || i==Mec || i==Bootload)?1:0;
        ResolvedRegister r={};
        if(resolveNavi31Register(ips,Navi31Block::Gc,base,offsets[i],bar,r)!=IpLayoutStatus::Ok)return Status::OutsideBar;
        regs[i]=r.mmioByteOffset;
        if(regs[i]<0x1c || (regs[i]>=0x7f000 && regs[i]<0x80000))return Status::OutsideBar;
        for(unsigned j=0;j<i;++j)if(regs[i]==regs[j])return Status::UnsupportedLayout;
    }
    return Status::Ok;
}
Status end(Result& r,Status status) { r.status=status; return status; }
bool read(const Io& io,uint32_t reg,uint32_t& value,Result& r) {
    ++r.reads; r.lastRegister=reg;
    if(!io.read32(io.context,reg,value)) { end(r,Status::ReadFailed); return false; }
    r.lastValue=value; if(value==UINT32_MAX) { end(r,Status::DeviceUnavailable); return false; }return true;
}
bool write(const Io& io,uint32_t reg,uint32_t value,Result& r) {
    ++r.writes; r.lastRegister=reg; r.lastValue=value;
    if(!io.write32(io.context,reg,value)) { end(r,Status::WriteFailed); return false; }return true;
}
bool match(const Io& io,uint32_t reg,uint32_t mask,uint32_t expected,Result& r,Status error=Status::ReadbackMismatch) {
    uint32_t value=0; if(!read(io,reg,value,r))return false;
    if((value&mask)!=expected) { end(r,error); return false; }return true;
}
bool store(const Io& io,uint32_t reg,uint32_t value,Result& r) { return write(io,reg,value,r) && match(io,reg,UINT32_MAX,value,r); }
bool rmw(const Io& io,uint32_t reg,uint32_t mask,uint32_t value,Result& r) {
    uint32_t old=0; return read(io,reg,old,r) && write(io,reg,(old&~mask)|(value&mask),r) && match(io,reg,mask,value&mask,r);
}
} // namespace
Status State::submissionCapacity() const {
    if(pending_)return Status::PendingSubmission;
    if(phase_!=Phase::Running)return Status::InvalidState;
    if(sequence_==UINT32_MAX || producer_>UINT32_MAX-2048)return Status::CounterExhausted;
    return Status::Ok;
}
struct Access {
    static Status finish(const State& s,Result& r,Status status) {
        r.retentionRequired=s.retain_; r.producerWords=s.producer_; return end(r,status);
    }
    static Status fault(State& s,Result& r,Status status) { s.phase_=Phase::Faulted; return finish(s,r,status); }
    static bool idle(const Io& io,State& s,const PollOptions& p,Result& r) {
        for(uint32_t i=0;i<p.maxPolls;++i) {
            ++r.polls; uint32_t value=0; if(!read(io,s.regs_[StatusReg],value,r))return false;
            if(!value)return true; if(i+1<p.maxPolls)io.delayUs(io.context,p.delayUs);
        }
        end(r,Status::IdleTimeout); return false;
    }
    static void emit(State& s,uint32_t value) { s.memory_.ring[s.producer_++&(s.config_.ringWords-1)]=value; }
    static bool controlOverlap(const State& s,uint64_t gpu,uint64_t bytes) {
        const uint64_t a[]={s.config_.ringGpu,s.config_.rptrGpu,s.config_.wptrGpu,s.config_.fenceGpu};
        const uint64_t n[]={uint64_t(s.config_.ringWords)*4,4,8,4};
        for(unsigned i=0;i<4;++i)if(overlaps(a[i],n[i],gpu,bytes))return true; return false;
    }
    static Status guard(const Io& io,State& s,Result& r) {
        const auto capacity=s.submissionCapacity();
        if(capacity==Status::PendingSubmission)return finish(s,r,capacity);
        if(s.phase_!=Phase::Running || !ioValid(io))return finish(s,r,Status::InvalidState);
        if(capacity!=Status::Ok)return finish(s,r,capacity);
        if(!io.syncForCpu(io.context))return fault(s,r,Status::SyncFailed);
        r.observedRptr=*s.memory_.rptr;
        if(r.observedRptr!=s.producer_)return fault(s,r,Status::PointerCorrupt);
        if(!match(io,s.regs_[Select],UINT32_MAX,0,r) || !match(io,s.regs_[Active],1,1,r) ||
            !match(io,s.regs_[Active1],1,0,r) || !match(io,s.regs_[Doorbell],0x40000000,0,r) ||
            !match(io,s.regs_[Wp],UINT32_MAX,s.producer_,r) || !match(io,s.regs_[WpHi],UINT32_MAX,0,r))return fault(s,r,r.status);
        return Status::Ok;
    }
    static Status publish(const Io& io,State& s,Result& r) {
        r.stage=Stage::Encode; const uint64_t address=s.config_.fenceGpu;
        emit(s,0xc0064900); // RELEASE_MEM, 7 payload DWORDs, no interrupt.
        emit(s,0x06603514); // SEQ|GL2_WB|GLM_INV|GLM_WB|policy3|event0x14|index5.
        emit(s,0x20000000); emit(s,uint32_t(address)); emit(s,uint32_t(address>>32));
        emit(s,++s.sequence_); emit(s,0); emit(s,0);
        const uint32_t padding=(256-(s.producer_&255))&255;
        if(padding) {
            emit(s,padding==1?0xffff1000U:0xc0001000U|((padding-2)<<16));
            for(uint32_t i=1;i<padding;++i)emit(s,0xffff1000U);
        }
        r.stage=Stage::Publish;
        if(!io.syncForDeviceAndHdp(io.context))return fault(s,r,Status::SyncFailed);
        s.pending_=true; r.submissionMayHaveOccurred=true;
        // Linux's non-doorbell path leaves the host WPTR shadow unchanged.
        if(!write(io,s.regs_[Wp],s.producer_,r) || !write(io,s.regs_[WpHi],0,r))return fault(s,r,r.status);
        return finish(s,r,Status::Ok);
    }
    static Status wait(const Io& io,State& s,const PollOptions& p,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(!s.pending_ || (s.phase_!=Phase::Running && s.phase_!=Phase::Faulted))return finish(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p))return finish(s,r,Status::InvalidArgument);
        r.stage=Stage::FenceWait;
        for(uint32_t i=0;i<p.maxPolls;++i) {
            ++r.polls;
            if(!io.syncForCpu(io.context))return fault(s,r,Status::SyncFailed);
            r.fenceValue=*s.memory_.fence; r.observedRptr=*s.memory_.rptr;
            if(r.fenceValue!=s.sequence_ && r.fenceValue!=s.sequence_-1)return fault(s,r,Status::PointerCorrupt);
            if(r.observedRptr>s.producer_)return fault(s,r,Status::PointerCorrupt);
            if(r.fenceValue==s.sequence_ && r.observedRptr==s.producer_) {
                s.pending_=false; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
            }
            if(i+1<p.maxPolls)io.delayUs(io.context,p.delayUs);
        }
        return fault(s,r,Status::FenceTimeout);
    }
    static Status start(const Navi31IpLayout& ips,uint64_t bar,const gfxinit::State& init,const smu::State& smu,
        const Config& c,const Memory& m,const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{};
        if(s.phase_!=Phase::Fresh || s.startAttempted_ || !init.prepared() || !smu.awake())return finish(s,r,Status::InvalidState);
        if(!memoryValid(c,m) || !ioValid(io) || !pollValid(p))return finish(s,r,Status::InvalidArgument);
        const auto resolved=resolve(ips,bar,s.regs_); if(resolved!=Status::Ok)return finish(s,r,resolved);
        s.startAttempted_=true; r.stage=Stage::Guard;
        if(!match(io,s.regs_[Imu],0x1f,0x1f,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Bootload],0x80000000,0x80000000,r,Status::ActiveEngine) ||
            !match(io,s.regs_[StatusReg],UINT32_MAX,0,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Me],0x14000000,0x14000000,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Mec],0x7c000000,0x40000000,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Mes],0x7c000000,0x40000000,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Active],1,0,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Active1],1,0,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Vm0],1,1,r,Status::ActiveEngine))return fault(s,r,r.status);
        s.config_=c; s.memory_=m; r.stage=Stage::Initialize;
        for(uint32_t i=0;i<c.ringWords;++i)m.ring[i]=0;
        *m.rptr=0; *m.wptr=0; *m.fence=0;
        // Encode only after full bounds checks. These 960DWORDs form the
        // initial real ring submission, not a CPU-executed substitute.
        if(gfxinit::encodeClearState(ips,reinterpret_cast<uint8_t*>(m.ring),size_t(c.ringWords)*4)!=gfxinit::Status::Ok)
            return fault(s,r,Status::UnsupportedLayout);
        if(!io.syncForDeviceAndHdp(io.context))return fault(s,r,Status::SyncFailed);
        r.stage=Stage::Program; s.retain_=true;
        uint32_t log=0; for(uint32_t n=c.ringWords/2;n>1;n>>=1)++log;
        const uint32_t rbControl=0x20000000|log|((log-2)<<8);
        if(!store(io,s.regs_[Delay],0,r) || !store(io,s.regs_[Vmid],0,r) || !store(io,s.regs_[Select],0,r) ||
            !store(io,s.regs_[Control],rbControl,r) || !store(io,s.regs_[Wp],0,r) || !store(io,s.regs_[WpHi],0,r) ||
            !store(io,s.regs_[RpAddr],uint32_t(c.rptrGpu),r) || !store(io,s.regs_[RpAddrHi],uint32_t(c.rptrGpu>>32),r) ||
            !store(io,s.regs_[PollLo],uint32_t(c.wptrGpu),r) || !store(io,s.regs_[PollHi],uint32_t(c.wptrGpu>>32),r))return fault(s,r,r.status);
        io.delayUs(io.context,1000);
        if(!store(io,s.regs_[Control],rbControl,r) || !store(io,s.regs_[Base],uint32_t(c.ringGpu>>8),r) ||
            !store(io,s.regs_[BaseHi],uint32_t(c.ringGpu>>40),r) || !store(io,s.regs_[Active],1,r) ||
            !rmw(io,s.regs_[Doorbell],0x40000000,0,r) || !store(io,s.regs_[RangeLo],0,r) ||
            !store(io,s.regs_[RangeHi],0xffc,r) || !store(io,s.regs_[MaxContext],7,r) ||
            !store(io,s.regs_[DeviceId],1,r))return fault(s,r,r.status);
        r.stage=Stage::Enable;
        if(!rmw(io,s.regs_[Me],0x14000000,0,r) || !idle(io,s,p,r))return fault(s,r,r.status);
        s.phase_=Phase::Running; s.producer_=uint32_t(gfxinit::kClearStateDwords);
        const auto sent=publish(io,s,r); if(sent!=Status::Ok)return sent;
        return wait(io,s,p,r);
    }
    static Status submit(const Io& io,State& s,uint64_t gpu,uint32_t value,bool ib,Result& r,uint8_t vmid=0) {
        r=Result{};
        const uint64_t bytes=ib?uint64_t(value)*4:4;
        if((gpu&3) || !range(gpu,bytes) || (ib && (!value || value>0xfffff)))return finish(s,r,Status::InvalidArgument);
        if(vmid>7 || (!ib && vmid))return finish(s,r,Status::InvalidArgument);
        if(vmid && (gpu<4096 || gpu>=(UINT64_C(1)<<47) || bytes>(UINT64_C(1)<<47)-gpu))
            return finish(s,r,Status::InvalidArgument);
        if(!vmid && controlOverlap(s,gpu,bytes))return finish(s,r,Status::InvalidArgument);
        const auto ready=guard(io,s,r); if(ready!=Status::Ok)return ready;
        // gfx_v11_0_ring_emit_ib_gfx: VMID in bits27:24; graphics has no
        // compute-only INDIRECT_BUFFER_VALID. Ring/writeback stay kernel VMID0.
        if(ib) { emit(s,0xc0023f00); emit(s,uint32_t(gpu)); emit(s,uint32_t(gpu>>32)); emit(s,value|(uint32_t(vmid)<<24)); }
        else { emit(s,0xc0033700); emit(s,0x00100500); emit(s,uint32_t(gpu)); emit(s,uint32_t(gpu>>32)); emit(s,value); }
        if(vmid) {
            // Kernel-owned tail, outside the captured IB: CS_PARTIAL_FLUSH,
            // event index4, then PFP_SYNC_ME before RELEASE_MEM cache writeback.
            emit(s,0xc0004600); emit(s,0x407); emit(s,0xc0004200); emit(s,0);
        }
        return publish(io,s,r);
    }
    static Status stop(const Io& io,State& s,const PollOptions& p,Result& r) {
        r=Result{};
        if(!s.retain_ || (s.phase_!=Phase::Running && s.phase_!=Phase::Faulted))return finish(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p))return finish(s,r,Status::InvalidArgument);
        if(s.pending_) { const auto retired=wait(io,s,p,r); if(retired!=Status::Ok)return retired; }
        r.stage=Stage::Halt;
        if(!store(io,s.regs_[Select],0,r) || !match(io,s.regs_[Active1],1,0,r,Status::ActiveEngine) ||
            !rmw(io,s.regs_[Me],0x14000000,0x14000000,r) || !idle(io,s,p,r) ||
            !store(io,s.regs_[Active],0,r) || !rmw(io,s.regs_[Doorbell],0x40000000,0,r))return fault(s,r,r.status);
        s.retain_=false; s.phase_=Phase::Stopped; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
    static Status checkIdleCompletion(const Io& io,State& s,const PollOptions& p,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(s.pending_ || (s.phase_!=Phase::Running && s.phase_!=Phase::Stopped) || !s.sequence_)
            return finish(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p))return finish(s,r,Status::InvalidArgument);
        const bool stopped=s.phase_==Phase::Stopped;
        r.stage=Stage::Guard;
        if(!match(io,s.regs_[Select],UINT32_MAX,0,r) ||
            !match(io,s.regs_[Me],0x14000000,stopped?0x14000000:0,r) ||
            !match(io,s.regs_[Mec],0x7c000000,0x40000000,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Mes],0x7c000000,0x40000000,r,Status::ActiveEngine) ||
            !match(io,s.regs_[Active],1,stopped?0:1,r) || !match(io,s.regs_[Active1],1,0,r) ||
            !match(io,s.regs_[Vmid],UINT32_MAX,0,r) ||
            !match(io,s.regs_[Doorbell],0x40000000,0,r) ||
            !match(io,s.regs_[Wp],UINT32_MAX,s.producer_,r) || !match(io,s.regs_[WpHi],UINT32_MAX,0,r) ||
            !match(io,s.regs_[Base],UINT32_MAX,uint32_t(s.config_.ringGpu>>8),r) ||
            !match(io,s.regs_[BaseHi],UINT32_MAX,uint32_t(s.config_.ringGpu>>40),r) ||
            !match(io,s.regs_[RpAddr],UINT32_MAX,uint32_t(s.config_.rptrGpu),r) ||
            !match(io,s.regs_[RpAddrHi],UINT32_MAX,uint32_t(s.config_.rptrGpu>>32),r) ||
            !match(io,s.regs_[PollLo],UINT32_MAX,uint32_t(s.config_.wptrGpu),r) ||
            !match(io,s.regs_[PollHi],UINT32_MAX,uint32_t(s.config_.wptrGpu>>32),r) ||
            !idle(io,s,p,r))return fault(s,r,r.status);
        if(!io.syncForCpu(io.context))return fault(s,r,Status::SyncFailed);
        r.fenceValue=*s.memory_.fence; r.observedRptr=*s.memory_.rptr;
        if(r.fenceValue!=s.sequence_ || r.observedRptr!=s.producer_)
            return fault(s,r,Status::PointerCorrupt);
        r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
};
bool State::neverPublished() const {
    return phase_==Phase::Fresh && !startAttempted_ && !producer_ && !sequence_ && !pending_ && !retain_;
}
Status start(const Navi31IpLayout& l,uint64_t bytes,const gfxinit::State& init,const smu::State& smu,
    const Config& c,const Memory& m,const Io& io,const PollOptions& p,State& s,Result& r) { return Access::start(l,bytes,init,smu,c,m,io,p,s,r); }
Status submitIndirect(const Io& io,State& s,uint64_t gpu,uint32_t words,Result& r) { return Access::submit(io,s,gpu,words,true,r); }
Status submitIsolatedIndirect(const Io& io,State& s,uint8_t vmid,uint64_t gpu,uint32_t words,Result& r) {
    if(vmid<1 || vmid>7) { r=Result{};return Access::finish(s,r,Status::InvalidArgument); }
    return Access::submit(io,s,gpu,words,true,r,vmid);
}
Status submitWrite(const Io& io,State& s,uint64_t gpu,uint32_t value,Result& r) { return Access::submit(io,s,gpu,value,false,r); }
Status waitFence(const Io& io,State& s,const PollOptions& p,Result& r) { return Access::wait(io,s,p,r); }
Status checkIdleCompletion(const Io& io,State& s,const PollOptions& p,Result& r) { return Access::checkIdleCompletion(io,s,p,r); }
Status stop(const Io& io,State& s,const PollOptions& p,Result& r) { return Access::stop(io,s,p,r); }
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(UnsupportedLayout);NAME(OutsideBar);NAME(ActiveEngine);
    NAME(ReadFailed);NAME(WriteFailed);NAME(DeviceUnavailable);NAME(ReadbackMismatch);NAME(SyncFailed);
    NAME(PointerCorrupt);NAME(PendingSubmission);NAME(CounterExhausted);NAME(FenceTimeout);NAME(IdleTimeout);
#undef NAME
    }
    return "Unknown";
}
} // namespace gfxqueue
} // namespace hackynavi
