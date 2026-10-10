// SPDX-License-Identifier: MIT
// Register/packet protocols adapted from Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu/sdma_v6_0.c:gfx_resume_instance,gfx_stop,enable,ring_set_wptr,
// ring_emit_ib,ring_emit_mem_sync,ring_emit_fence,ring_funcs;
// amdgpu/sdma_common.h (default read policy2/write policy3);
// amdgpu/amdgpu_ring.c:amdgpu_ring_commit;
// amdgpu/amdgpu_sdma.c:amdgpu_sdma_get_csa_mc_addr (VMID0 returns0);
// amdgpu/sdma_v6_0_0_pkt_open.h and gc/gc_11_0_0_{offset,sh_mask}.h.
// Ownership validation, bounded single-flight execution, failures and readback
// are port policy; no firmware load, interrupt, or scheduler is simulated.
// Copyright 2020-2021 Advanced Micro Devices, Inc.
//
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
#include "hackynavi/sdma_queue.hpp"
#include "hackynavi/sdma.hpp"

namespace hackynavi {
namespace sdmaqueue {
namespace {
enum Reg { Sem, Idle, Watchdog, Utcl, Page, F32, Rb, BaseLo, BaseHi,
    RptrLo, RptrHi, WptrLo, WptrHi, RptrAddrHi, RptrAddrLo, Ib,
    Doorbell, DoorbellOffset, WpollHi, WpollLo, Minor, Aql, Preempt,
    Freeze, Context, Others, RegCount = Others+14 };
static_assert(RegCount <= 40,"State register storage");
const uint32_t kOffsets[Others] = {0x22,0x25,0x2e,0x3c,0x3f,0x589a,0x80,
    0x81,0x82,0x83,0x84,0x85,0x86,0x88,0x89,0x8a,0x92,0xab,0xb2,0xb3,
    0xb5,0xb4,0xb0,0x2b,0x91};
bool callbacks(const Io& io) {
    return io.read32 && io.write32 && io.syncForDeviceAndHdp && io.syncForCpu && io.delayUs;
}
bool pollsValid(const PollOptions& p) {
    return p.maxPolls && p.maxPolls <= 1000000 && p.delayUs && p.delayUs <= 10 &&
        uint64_t(p.maxPolls)*p.delayUs <= 1000000;
}
bool range(uint64_t address, uint64_t bytes, uint64_t maximum = 0xffffffffffffULL) {
    return bytes && address <= maximum && bytes-1 <= maximum-address;
}
bool overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a <= b ? b-a < as : a-b < bs;
}
bool configValid(const Config& c, const Memory& m) {
    if (c.engine > 1 || c.ringWords < 256 || c.ringWords > 16384 ||
        (c.ringWords & (c.ringWords-1)) || !c.watchdog100ms || c.watchdog100ms > 255 ||
        !m.ring || !m.rptr || !m.wptr || !m.fence || m.capacityWords < c.ringWords ||
        (c.ringGpuAddress & 255) || (c.rptrGpuAddress & 7) ||
        (c.wptrGpuAddress & 7) || (c.fenceGpuAddress & 3)) return false;
    const uint64_t gpu[4] = {c.ringGpuAddress,c.rptrGpuAddress,c.wptrGpuAddress,c.fenceGpuAddress};
    const uint64_t cpu[4] = {reinterpret_cast<uintptr_t>(m.ring),reinterpret_cast<uintptr_t>(m.rptr),
        reinterpret_cast<uintptr_t>(m.wptr),reinterpret_cast<uintptr_t>(m.fence)};
    const uint64_t bytes[4] = {uint64_t(c.ringWords)*4,8,8,4};
    const uint32_t alignment[4] = {4,8,8,4};
    for (unsigned i = 0; i < 4; ++i) {
        if (!range(gpu[i],bytes[i]) || !range(cpu[i],bytes[i],UINTPTR_MAX) ||
            (cpu[i] & (alignment[i]-1))) return false;
        for (unsigned j = 0; j < i; ++j)
            if (overlap(gpu[i],bytes[i],gpu[j],bytes[j]) ||
                overlap(cpu[i],bytes[i],cpu[j],bytes[j])) return false;
    }
    return true;
}
bool controlsOverlap(const Config& c, uint64_t address, uint64_t bytes) {
    return overlap(address,bytes,c.ringGpuAddress,uint64_t(c.ringWords)*4) ||
        overlap(address,bytes,c.rptrGpuAddress,8) || overlap(address,bytes,c.wptrGpuAddress,8) ||
        overlap(address,bytes,c.fenceGpuAddress,4);
}
Status resolve(const Navi31IpLayout& l, uint64_t bytes, unsigned engine, uint32_t* regs) {
    if (!l.valid) return Status::UnsupportedLayout;
    const uint16_t ids[6] = {11,34,255,1,42,43};
    const uint8_t majors[6] = {11,3,13,13,6,6};
    for (unsigned i = 0; i < 6; ++i) {
        const auto& b = l.blocks[i];
        if (b.hardwareId != ids[i] || b.major != majors[i] || b.minor || b.revision ||
            b.dieId || b.instance || !b.baseCount || b.baseCount > kNavi31MaxBases ||
            (i == 0 && b.baseCount < 2))
            return Status::UnsupportedLayout;
    }
    for (unsigned i = 0; i < RegCount; ++i) {
        const uint32_t offset = i < Others ? kOffsets[i] :
            uint32_t(0x80+((i-Others)/2+1)*0x58+((i-Others)%2)*0xa);
        ResolvedRegister r = {};
        const auto status = resolveNavi31SdmaRegister(l,engine,offset,bytes,r);
        if (status != IpLayoutStatus::Ok)
            return status == IpLayoutStatus::AddressOverflow || status == IpLayoutStatus::OutsideBar ?
                Status::OutsideBar : Status::UnsupportedLayout;
        regs[i] = r.mmioByteOffset;
        for (unsigned j = 0; j < i; ++j) if (regs[i] == regs[j]) return Status::AliasedRegisters;
    }
    return Status::Ok;
}
} // namespace

struct Access {
    static Status finish(State& s, Result& r, Status status, bool fault = false) {
        if (fault && s.retain_) s.phase_ = Phase::Faulted;
        r.status = status; r.retentionRequired = s.retain_; r.producerBytes = s.producerBytes_;
        return status;
    }
    static bool read(const Io& io, State& s, Reg reg, uint32_t& value, Result& r) {
        r.lastOffset = s.regs_[reg]; ++r.reads;
        if (!io.read32(io.context,r.lastOffset,value)) { finish(s,r,Status::ReadFailed,true); return false; }
        r.lastValue = value;
        if (value == UINT32_MAX) { finish(s,r,Status::DeviceUnavailable,true); return false; }
        return true;
    }
    static bool write(const Io& io, State& s, Reg reg, uint32_t value, Result& r) {
        r.lastOffset = s.regs_[reg]; r.lastValue = value; ++r.writes;
        s.retain_ = r.retentionRequired = r.hardwareMayHaveChanged = true;
        if (!io.write32(io.context,r.lastOffset,value)) { finish(s,r,Status::WriteFailed,true); return false; }
        return true;
    }
    static bool check(const Io& io, State& s, Reg reg, uint32_t wanted, uint32_t mask, Result& r) {
        uint32_t value = 0;
        if (!read(io,s,reg,value,r)) return false;
        if ((value & mask) != (wanted & mask)) { finish(s,r,Status::ReadbackMismatch,true); return false; }
        return true;
    }
    static bool otherQueues(const Io& io, State& s, Result& r) {
        for (unsigned i = Others; i < RegCount; ++i) {
            uint32_t v = 0;
            if (!read(io,s,static_cast<Reg>(i),v,r)) return false;
            if (v & 1) { finish(s,r,Status::OtherQueueActive,true); return false; }
        }
        return true;
    }
    static Status start(const Navi31IpLayout& l, uint64_t barBytes, const Config& c,
                        const Memory& memory, const Io& io, State& s, Result& r) {
        r = Result{}; r.stage = Stage::Preflight;
        if (!callbacks(io) || !configValid(c,memory)) return finish(s,r,Status::InvalidArgument);
        if (s.phase_ != Phase::Empty || s.retain_) return finish(s,r,Status::InvalidState);
        uint32_t regs[40] = {};
        Status status = resolve(l,barBytes,c.engine,regs);
        if (status != Status::Ok) return finish(s,r,status);
        for (unsigned i = 0; i < RegCount; ++i) s.regs_[i] = regs[i];
        s.config_ = c; s.memory_ = memory;
        r.stage = Stage::Guards;
        uint32_t rb=0,ib=0,f32=0,idle=0,aql=0,preempt=0,freeze=0;
        if (!read(io,s,Rb,rb,r) || !read(io,s,Ib,ib,r) || !read(io,s,F32,f32,r) ||
            !read(io,s,Idle,idle,r) || !read(io,s,Aql,aql,r) || !read(io,s,Preempt,preempt,r) ||
            !read(io,s,Freeze,freeze,r)) return r.status;
        if ((rb & 1) || (ib & 1) || !(f32 & 1) || !(idle & 1))
            return finish(s,r,Status::EngineNotStopped);
        // FREEZE also carries PREEMPT, FROZEN and F32_FREEZE. Unhalting F32
        // does not clear these inherited stop modes; reject the whole set.
        if ((aql & 0x70001) || (preempt & 1) || (freeze & 0x71) || (f32 & 0x200))
            return finish(s,r,Status::UnsupportedQueueMode);
        if (!otherQueues(io,s,r)) return r.status;
        uint32_t watchdog=0,utcl=0,page=0,doorbell=0,doorbellOffset=0;
        if (!read(io,s,Watchdog,watchdog,r) || !read(io,s,Utcl,utcl,r) ||
            !read(io,s,Page,page,r) || !read(io,s,Doorbell,doorbell,r) ||
            !read(io,s,DoorbellOffset,doorbellOffset,r)) return r.status;
        unsigned logWords = 0; for (uint32_t n=c.ringWords; n>1; n>>=1) ++logWords;
        const uint32_t initialRb = (rb & ~0x0f003f3fU) | 0x800000 | (logWords<<1);
        const uint32_t enabledRb = initialRb | 0x1801;
        const uint32_t enabledIb = (ib & ~0x000f0011U) | 1;
        watchdog = (watchdog & ~0xffU) | c.watchdog100ms;
        utcl = (utcl & ~0x61fU) | 0x609;
        page = (page & 0x00ff0fff) | 0x0100e000; // READ_NOA2, WRITE_BYPASS3, LLC_NOALLOC.
        doorbell &= ~0x10000000U;
        f32 &= ~0x2001U;
        r.stage = Stage::InitializeMemory;
        for (uint32_t i=0;i<c.ringWords;++i) memory.ring[i]=0;
        *memory.rptr=0; *memory.wptr=0; *memory.fence=0;
        if (!io.syncForDeviceAndHdp(io.context)) return finish(s,r,Status::SyncFailed);
        struct Step { Reg reg; uint32_t value; };
        const Step plan[] = {
            {Sem,0},{Rb,initialRb},{RptrLo,0},{RptrHi,0},{WptrLo,0},{WptrHi,0},
            {WpollLo,uint32_t(c.wptrGpuAddress)},{WpollHi,uint32_t(c.wptrGpuAddress>>32)},
            {RptrAddrHi,uint32_t(c.rptrGpuAddress>>32)},{RptrAddrLo,uint32_t(c.rptrGpuAddress)},
            {BaseLo,uint32_t(c.ringGpuAddress>>8)},{BaseHi,uint32_t(c.ringGpuAddress>>40)},
            {Minor,1},{WptrLo,0},{WptrHi,0},{Doorbell,doorbell},{DoorbellOffset,doorbellOffset},
            {Minor,0},{Watchdog,watchdog},{Utcl,utcl},{Page,page}
        };
        s.phase_=Phase::Faulted; r.stage=Stage::ProgramRing;
        for (const auto& step:plan) if (!write(io,s,step.reg,step.value,r)) return r.status;
        r.stage=Stage::VerifyRing;
        for (const auto& step:plan) {
            if (step.reg==Minor && step.value==1) continue;
            const uint32_t mask = step.reg==Doorbell ? 0x10000000 : UINT32_MAX;
            if (!check(io,s,step.reg,step.value,mask,r)) return r.status;
        }
        r.stage=Stage::Enable;
        if (!write(io,s,F32,f32,r) || !write(io,s,Rb,enabledRb,r) || !write(io,s,Ib,enabledIb,r) ||
            !check(io,s,F32,0,0x2201,r) || !check(io,s,Rb,enabledRb,0x0f003f3f|0x800000,r) ||
            !check(io,s,Ib,enabledIb,0xf0011,r)) return r.status;
        s.phase_=Phase::Running; r.stage=Stage::Complete;
        return finish(s,r,Status::Ok);
    }
    static Status encode(const Io& io, State& s, unsigned kind, uint64_t a, uint64_t b,
                         uint32_t count, uint32_t value, Result& r) {
        r=Result{};r.stage=Stage::Preflight;
        if (!callbacks(io)) return finish(s,r,Status::InvalidArgument);
        if (s.phase_!=Phase::Running) return finish(s,r,Status::InvalidState);
        if (s.pending_) return finish(s,r,Status::PendingSubmission);
        const uint64_t bytes=kind==2 ? uint64_t(count)*4 : count;
        if (!range(a,bytes) || controlsOverlap(s.config_,a,bytes) ||
            (kind==0 && (!range(b,bytes) || controlsOverlap(s.config_,b,bytes))))
            return finish(s,r,Status::InvalidArgument);
        if (kind==2 && ((a&31) || count<8 || count>65536 || (count&7)))
            return finish(s,r,Status::InvalidArgument);
        if (s.fenceSequence_==UINT32_MAX || s.producerBytes_>UINT64_MAX-128)
            return finish(s,r,Status::CounterExhausted);
        uint32_t words[32]={}; sdma6::CommandBuffer buffer={words,32,0};
        r.stage=Stage::Encode;
        if (kind==0 && sdma6::appendCopyLinear(buffer,a,b,count)!=hackynavi::Status::Ok)
            return finish(s,r,Status::InvalidArgument);
        if (kind==1 && sdma6::appendConstantFill(buffer,a,value,count)!=hackynavi::Status::Ok)
            return finish(s,r,Status::InvalidArgument);
        if (kind==2) {
            // Every prior publish is16DW aligned; two NOPs align INDIRECT end.
            words[2]=4; words[3]=uint32_t(a); words[4]=uint32_t(a>>32);
            words[5]=count; words[6]=words[7]=0; buffer.used=8;
        }
        // Linux whole-cache GCR control=0xc3a1; high control bits and VA limits0.
        words[buffer.used++]=17; words[buffer.used++]=0;
        words[buffer.used++]=0xc3a10000; words[buffer.used++]=0; words[buffer.used++]=0;
        const uint32_t sequence=s.fenceSequence_+1;
        if (sdma6::appendFence32(buffer,s.config_.fenceGpuAddress,sequence,false)!=hackynavi::Status::Ok)
            return finish(s,r,Status::InvalidArgument);
        while (buffer.used&15) words[buffer.used++]=0;
        // One outstanding batch and waitFence's exact RPTR check mean all old
        // words are retired. The bounded batch fits even the smallest ring.
        if (!io.syncForCpu(io.context)) return finish(s,r,Status::SyncFailed,true);
        const uint64_t rptr=*s.memory_.rptr;
        r.observedRptrBytes=rptr;
        if (rptr!=s.producerBytes_) return finish(s,r,Status::PointerCorrupt,true);
        for (size_t i=0;i<buffer.used;++i)
            s.memory_.ring[((s.producerBytes_>>2)+i)&(s.config_.ringWords-1)]=words[i];
        r.stage=Stage::Publish;
        if (!io.syncForDeviceAndHdp(io.context)) return finish(s,r,Status::SyncFailed,true);
        s.producerBytes_+=uint64_t(buffer.used)*4; s.fenceSequence_=sequence; s.pending_=true;
        r.submissionMayHaveOccurred=true;
        if (!write(io,s,WptrLo,uint32_t(s.producerBytes_),r) ||
            !write(io,s,WptrHi,uint32_t(s.producerBytes_>>32),r)) return r.status;
        r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
    static Status wait(const Io& io, State& s, const PollOptions& p, Result& r) {
        if (!s.pending_) return finish(s,r,Status::InvalidState);
        r.stage=Stage::FenceWait;
        for (uint32_t i=0;i<p.maxPolls;++i) {
            ++r.polls;
            if (!io.syncForCpu(io.context)) return finish(s,r,Status::SyncFailed,true);
            const uint64_t rptr=*s.memory_.rptr;
            const uint32_t fence=*s.memory_.fence;
            r.observedRptrBytes=rptr; r.fenceValue=fence;
            if ((rptr&3) || rptr<s.lastRptrBytes_ || rptr>s.producerBytes_ || fence>s.fenceSequence_)
                return finish(s,r,Status::PointerCorrupt,true);
            s.lastRptrBytes_=rptr;
            if (fence==s.fenceSequence_ && rptr==s.producerBytes_) {
                s.pending_=false; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
            }
            if (i+1<p.maxPolls) io.delayUs(io.context,p.delayUs);
        }
        return finish(s,r,Status::FenceTimeout,true);
    }
    static Status stop(const Io& io, State& s, const PollOptions& p, Result& r) {
        r=Result{};r.stage=Stage::Preflight;
        if (!callbacks(io)||!pollsValid(p)) return finish(s,r,Status::InvalidArgument);
        if (!s.retain_ || (s.phase_!=Phase::Running && s.phase_!=Phase::Faulted))
            return finish(s,r,Status::InvalidState);
        if (s.pending_ && wait(io,s,p,r)!=Status::Ok) return r.status;
        r.stage=Stage::Drain;
        if (!otherQueues(io,s,r)) return r.status;
        uint32_t rb=0,ib=0,f32=0,doorbell=0;
        if (!read(io,s,Rb,rb,r)||!read(io,s,Ib,ib,r)||!read(io,s,F32,f32,r)||
            !read(io,s,Doorbell,doorbell,r)) return r.status;
        r.stage=Stage::Stop;
        if (!write(io,s,Rb,rb&~1U,r)||!write(io,s,Ib,ib&~1U,r)||
            !write(io,s,Doorbell,doorbell&~0x10000000U,r)||!write(io,s,F32,f32|1,r)) return r.status;
        r.stage=Stage::VerifyStop;
        if (!check(io,s,Rb,0,1,r)||!check(io,s,Ib,0,1,r)||
            !check(io,s,Doorbell,0,0x10000000,r)||!check(io,s,F32,1,1,r)) return r.status;
        for(uint32_t i=0;i<p.maxPolls;++i) {
            uint32_t idle=0; ++r.polls;
            if(!read(io,s,Idle,idle,r)) return r.status;
            if(idle&1) {
                s.retain_=false;s.phase_=Phase::Stopped;r.stage=Stage::Complete;
                return finish(s,r,Status::Ok);
            }
            if(i+1<p.maxPolls) io.delayUs(io.context,p.delayUs);
        }
        return finish(s,r,Status::IdleTimeout,true);
    }
};
void State::clearAfterVerifiedReset() {
    phase_=Phase::Empty;config_=Config{};memory_=Memory{};
    for(unsigned i=0;i<40;++i) regs_[i]=0;
    producerBytes_=lastRptrBytes_=0;fenceSequence_=0;pending_=retain_=false;
}
Status start(const Navi31IpLayout& l,uint64_t b,const Config& c,const Memory& m,
             const Io& io,State& s,Result& r) { return Access::start(l,b,c,m,io,s,r); }
Status submitCopy(const Io& io,State& s,uint64_t a,uint64_t b,uint32_t n,Result& r) {
    return Access::encode(io,s,0,a,b,n,0,r);
}
Status submitFill(const Io& io,State& s,uint64_t a,uint32_t v,uint32_t n,Result& r) {
    return Access::encode(io,s,1,a,0,n,v,r);
}
Status submitIndirect(const Io& io,State& s,uint64_t a,uint32_t n,Result& r) {
    return Access::encode(io,s,2,a,0,n,0,r);
}
Status waitFence(const Io& io,State& s,const PollOptions& p,Result& r) {
    r=Result{};r.stage=Stage::Preflight;
    if(!callbacks(io)||!pollsValid(p)) return Access::finish(s,r,Status::InvalidArgument);
    if(!s.needsRetention()) return Access::finish(s,r,Status::InvalidState);
    return Access::wait(io,s,p,r);
}
Status stop(const Io& io,State& s,const PollOptions& p,Result& r) { return Access::stop(io,s,p,r); }
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(UnsupportedLayout);NAME(OutsideBar);
    NAME(AliasedRegisters);NAME(ReadFailed);NAME(WriteFailed);NAME(DeviceUnavailable);
    NAME(EngineNotStopped);NAME(OtherQueueActive);NAME(UnsupportedQueueMode);
    NAME(ReadbackMismatch);NAME(SyncFailed);NAME(PendingSubmission);NAME(PointerCorrupt);
    NAME(CounterExhausted);NAME(FenceTimeout);NAME(IdleTimeout);
#undef NAME
    }
    return "Unknown";
}
} // namespace sdmaqueue
} // namespace hackynavi
