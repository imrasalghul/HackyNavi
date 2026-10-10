// SPDX-License-Identifier: MIT
// PSP13 KM transport/ABI adapted from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// psp_v13_0.c:380-524; amdgpu_psp.c:728-972,1130-1203,3900-3967;
// psp_gfx_if.h; mp_13_0_0_offset.h. Ownership and fail-stop checks are port policy.
// Copyright 2016-2017, 2020-2021 Advanced Micro Devices, Inc.
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
#include "hackynavi/psp_ring.hpp"

namespace hackynavi {
namespace pspring {
namespace {
constexpr uint32_t kRingDwords=1024, kFrameDwords=16, kResponse=864;
constexpr uint32_t kMaximumTmr=64U*1024U*1024U;
bool same(VramAllocationHandle a,VramAllocationHandle b) {
    return a.owner==b.owner && a.serial==b.serial && a.slot==b.slot;
}
bool interval(const void* p,size_t n) {
    return p && n && n-1<=UINTPTR_MAX-reinterpret_cast<uintptr_t>(p);
}
bool overlaps(const void* a,size_t an,const void* b,size_t bn) {
    const auto av=reinterpret_cast<uintptr_t>(a),bv=reinterpret_cast<uintptr_t>(b);
    return av<=bv ? bv-av<an : av-bv<bn;
}
void zero(uint8_t* p,size_t n) { for(size_t i=0;i<n;++i) p[i]=0; }
void put(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
uint32_t get(const uint8_t* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
void address(uint8_t* p,uint64_t a) { put(p,uint32_t(a)); put(p+4,uint32_t(a>>32)); }
bool pollValid(const PollOptions& p) {
    return p.maxPolls && p.maxPolls<=1000000 && p.delayUs && p.delayUs<=1000 &&
        uint64_t(p.maxPolls)*p.delayUs<=10000000;
}
bool ioValid(const Io& io) {
    return io.read32 && io.write32 && io.flushHdp && io.beforeReadback && io.delayUs && io.delayMs;
}
bool layoutValid(const Layout& l) {
    if(!l.valid || l.control<0x80*4 || l.barBytes<0x1c) return false;
    const uint64_t base=l.control-0x80*4;
    const uint32_t raw[]={0x80,0x83,0x84,0x85,0x86,0x87,0x91};
    const uint32_t regs[]={l.control,l.wptr,l.rptr,l.ringLo,l.ringHi,l.ringSize,l.heartbeat};
    for(unsigned i=0;i<7;++i) {
        const uint64_t expected=base+uint64_t(raw[i])*4;
        if(expected>UINT32_MAX || expected!=regs[i] || (regs[i]&3) ||
            regs[i]>l.barBytes-4 || regs[i]==0 || regs[i]==4 || regs[i]==0x18 ||
            (regs[i]>=0x7f000 && regs[i]<0x80000)) return false;
    }
    return true;
}
bool pointerValid(uint32_t p) { return p<kRingDwords && !(p&(kFrameDwords-1)); }
bool tmrSizeValid(uint32_t n) { return n && !(n&4095) && n<=kMaximumTmr; }
bool firmwareTypeValid(uint32_t type) {
    return (type>=1 && type<=58) || type==62 || (type>=65 && type<=102) ||
        type==128 || type==129;
}
Status end(Result& r,Status status) { r.status=status; return status; }
bool read(const Io& io,uint32_t reg,uint32_t& value,Result& r) {
    r.lastRegister=reg; ++r.reads;
    if(!io.read32(io.context,reg,value)) { end(r,Status::ReadFailed); return false; }
    r.lastValue=value;
    if(value==UINT32_MAX) { end(r,Status::DeviceUnavailable); return false; }
    return true;
}
bool write(const Io& io,uint32_t reg,uint32_t value,Result& r) {
    r.lastRegister=reg; r.lastValue=value; ++r.writes;
    r.mailboxWritesMayHaveOccurred=true;
    if(!io.write32(io.context,reg,value)) { end(r,Status::WriteFailed); return false; }
    return true;
}
Status mailbox(const Layout& l,const Io& io,const PollOptions& p,Result& r) {
    r.stage=Stage::MailboxWait;
    for(uint32_t i=0;i<p.maxPolls;++i) {
        uint32_t v=0; ++r.polls;
        if(!read(io,l.control,v,r)) return r.status;
        if(v&0x80000000U) return (v&0xffffU) ? Status::MailboxRejected : Status::Ok;
        if(i+1<p.maxPolls) io.delayUs(io.context,p.delayUs);
    }
    return Status::MailboxTimeout;
}
} // namespace

State::State():phase_(Phase::Fresh),layout_{},allocator_(nullptr),buffers_{},mc_{},
    wptr_(0),sequence_(0),pendingId_(0),requiredTmr_(0),tmr_{},pendingInput_{},
    pending_(false),ringRetain_(false),tmrRetain_(false),tmrReady_(false) {}

struct Access {
    static void retention(const State& s,Result& r) {
        r.ringRetentionRequired=s.ringRetain_; r.tmrRetentionRequired=s.tmrRetain_;
        r.fenceSequence=s.sequence_;
    }
    static Status finish(const State& s,Result& r,Status status) {
        retention(s,r); return end(r,status);
    }
    static Status fault(State& s,Result& r,Status status) {
        s.phase_=Phase::Faulted; return finish(s,r,status);
    }
    static bool allocation(const VramAllocator& a,VramAllocationHandle h,uint64_t minimum,
                           uint64_t& mc,uint64_t& physical,VramRange& range) {
        return a.lookup(h,range)==VramAllocatorStatus::Ok && range.bytes>=minimum &&
            a.addresses(h,mc,physical)==VramAllocatorStatus::Ok && !(mc&4095) && !(physical&4095);
    }
    static bool bindings(const State& s) {
        const VramAllocationHandle h[]={s.buffers_.ring,s.buffers_.command,s.buffers_.fence};
        for(unsigned i=0;i<3;++i) {
            uint64_t mc=0,pa=0; VramRange range={};
            if(!allocation(*s.allocator_,h[i],4096,mc,pa,range) || mc!=s.mc_[i]) return false;
        }
        if(s.tmrRetain_) {
            uint64_t mc=0,pa=0; VramRange range={};
            if(!allocation(*s.allocator_,s.tmr_,s.requiredTmr_,mc,pa,range)) return false;
        }
        if(s.pendingInput_.owner) {
            VramRange range={};
            if(s.allocator_->lookup(s.pendingInput_,range)!=VramAllocatorStatus::Ok) return false;
        }
        return true;
    }
    static VramTransferIo transport(const State& s,const Io& io) {
        return {io.context,s.layout_.barBytes,io.read32,io.write32};
    }
    static bool upload(State& s,const Io& io,VramAllocationHandle h,uint64_t offset,
                       size_t bytes,Result& r) {
        if(uploadVram(*s.allocator_,h,offset,s.buffers_.scratch,bytes,transport(s,io),r.transfer)!=
            VramTransferStatus::Ok) { end(r,Status::TransferFailed); return false; }
        return true;
    }
    static bool download(State& s,const Io& io,VramAllocationHandle h,size_t bytes,Result& r) {
        if(!io.beforeReadback(io.context)) { end(r,Status::VisibilityFailed); return false; }
        if(readVram(*s.allocator_,h,0,s.buffers_.scratch,bytes,transport(s,io),r.transfer)!=
            VramTransferStatus::Ok) { end(r,Status::TransferFailed); return false; }
        return true;
    }
    static Status create(const Layout& l,const psp::State& sos,const VramAllocator& a,
        const Buffers& b,const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(s.phase_!=Phase::Fresh) return finish(s,r,Status::InvalidState);
        if(sos.phase()!=psp::Phase::Loaded) return finish(s,r,Status::SosUnavailable);
        if(!layoutValid(l)) return finish(s,r,Status::UnsupportedLayout);
        if(!ioValid(io) || !pollValid(p) || b.scratchBytes<kScratchBytes ||
            !interval(b.scratch,b.scratchBytes)) return finish(s,r,Status::InvalidArgument);
        if(overlaps(b.scratch,b.scratchBytes,&s,sizeof(s)) || overlaps(b.scratch,b.scratchBytes,&r,sizeof(r)) ||
            overlaps(b.scratch,b.scratchBytes,&a,sizeof(a)) || overlaps(b.scratch,b.scratchBytes,&b,sizeof(b)) ||
            overlaps(b.scratch,b.scratchBytes,&l,sizeof(l)) || overlaps(b.scratch,b.scratchBytes,&io,sizeof(io)) ||
            overlaps(b.scratch,b.scratchBytes,&sos,sizeof(sos))) return finish(s,r,Status::InvalidArgument);
        const VramAllocationHandle h[]={b.ring,b.command,b.fence};
        uint64_t mc[3]={};
        for(unsigned i=0;i<3;++i) {
            uint64_t physical=0; VramRange range={};
            if(!allocation(a,h[i],4096,mc[i],physical,range)) return finish(s,r,Status::InvalidAllocation);
            for(unsigned j=0;j<i;++j) if(same(h[i],h[j])) return finish(s,r,Status::AliasedAllocation);
        }
        s.layout_=l; s.allocator_=&a; s.buffers_=b;
        for(unsigned i=0;i<3;++i) s.mc_[i]=mc[i];
        r.stage=Stage::Guard;
        uint32_t heartbeat=0;
        if(!read(io,l.heartbeat,heartbeat,r)) return fault(s,r,r.status);
        if(!heartbeat) return fault(s,r,Status::SosUnavailable);
        const Status ready=mailbox(l,io,p,r);
        if(ready!=Status::Ok) return fault(s,r,ready);
        r.stage=Stage::Initialize;
        zero(b.scratch,kRingBytes);
        for(unsigned i=0;i<3;++i)
            if(!upload(s,io,h[i],0,4096,r)) return fault(s,r,r.status);
        r.stage=Stage::Publish;
        if(!io.flushHdp(io.context)) return fault(s,r,Status::VisibilityFailed);
        r.stage=Stage::Create;
        // Pin all bindings before even an ambiguous first address write.
        s.ringRetain_=true;
        if(!write(io,l.ringLo,uint32_t(mc[0]),r) || !write(io,l.ringHi,uint32_t(mc[0]>>32),r) ||
            !write(io,l.ringSize,kRingBytes,r)) return fault(s,r,r.status);
        const bool issued=write(io,l.control,0x20000,r);
        io.delayMs(io.context,20); // Required even if the write reached hardware but reported failure.
        if(!issued) return fault(s,r,r.status);
        const Status created=mailbox(l,io,p,r);
        if(created!=Status::Ok) return fault(s,r,created);
        uint32_t wp=0,rp=0;
        if(!read(io,l.wptr,wp,r) || !read(io,l.rptr,rp,r)) return fault(s,r,r.status);
        if(wp || rp) return fault(s,r,Status::PointerCorrupt);
        s.phase_=Phase::Running; r.stage=Stage::Complete;
        return finish(s,r,Status::Ok);
    }
    static Status begin(State& s,const Io& io,uint32_t id,Result& r) {
        r=Result{}; r.stage=Stage::Preflight; r.commandId=id;
        if(s.pending_) return finish(s,r,Status::PendingCommand);
        if(s.phase_!=Phase::Running) return finish(s,r,Status::InvalidState);
        if(!ioValid(io)) return finish(s,r,Status::InvalidArgument);
        if(!bindings(s)) return fault(s,r,Status::InvalidAllocation);
        if(s.sequence_==UINT32_MAX) return fault(s,r,Status::CounterExhausted);
        zero(s.buffers_.scratch,kCommandBytes); put(s.buffers_.scratch+8,id);
        return Status::Ok;
    }
    static Status input(const State& s,VramAllocationHandle h,uint64_t offset,uint32_t bytes,
                        uint64_t& mc,Result& r) {
        if(!bytes || (offset&4095)) return finish(s,r,Status::InvalidArgument);
        VramRange range={}; uint64_t base=0,physical=0;
        if(!allocation(*s.allocator_,h,4096,base,physical,range) || offset>range.bytes ||
            bytes>range.bytes-offset || offset>UINT64_MAX-base) return finish(s,r,Status::InvalidAllocation);
        if(same(h,s.buffers_.ring) || same(h,s.buffers_.command) || same(h,s.buffers_.fence) ||
            (s.tmrRetain_ && same(h,s.tmr_))) return finish(s,r,Status::AliasedAllocation);
        mc=base+offset; return Status::Ok;
    }
    static Status submit(State& s,const Io& io,uint32_t id,VramAllocationHandle source,Result& r) {
        r.stage=Stage::Guard;
        uint32_t heartbeat=0,wp=0,rp=0;
        if(!read(io,s.layout_.heartbeat,heartbeat,r) || !read(io,s.layout_.wptr,wp,r) ||
            !read(io,s.layout_.rptr,rp,r)) return fault(s,r,r.status);
        if(!heartbeat) return fault(s,r,Status::SosUnavailable);
        if(!pointerValid(wp) || !pointerValid(rp) || wp!=s.wptr_ || rp!=wp)
            return fault(s,r,Status::PointerCorrupt);
        r.stage=Stage::UploadCommand;
        if(!upload(s,io,s.buffers_.command,0,kCommandBytes,r)) return fault(s,r,r.status);
        r.stage=Stage::UploadFrame;
        zero(s.buffers_.scratch,64);
        address(s.buffers_.scratch,s.mc_[1]); address(s.buffers_.scratch+12,s.mc_[2]);
        put(s.buffers_.scratch+20,s.sequence_+1);
        if(!upload(s,io,s.buffers_.ring,uint64_t(wp)*4,64,r)) return fault(s,r,r.status);
        r.stage=Stage::Publish;
        if(!io.flushHdp(io.context)) return fault(s,r,Status::VisibilityFailed);
        // PSP can execute immediately during the wptr write, including a false
        // callback result. Install pending ownership before issuing that write.
        ++s.sequence_; s.pendingId_=id; s.pending_=true; s.pendingInput_=source;
        if(id==5) { s.tmr_=source; s.tmrRetain_=true; }
        s.wptr_=(wp+kFrameDwords)%kRingDwords;
        r.stage=Stage::Submit; r.submissionMayHaveOccurred=true;
        if(!write(io,s.layout_.wptr,s.wptr_,r)) return fault(s,r,r.status);
        return finish(s,r,Status::Ok);
    }
    static Status simple(const Io& io,State& s,uint32_t id,Result& r) {
        Status status=begin(s,io,id,r); if(status!=Status::Ok) return status;
        if((id==7 || id==0x21) && !s.tmrReady_) return finish(s,r,Status::InvalidState);
        if(id==0x22) put(s.buffers_.scratch+28,2);
        return submit(s,io,id,{},r);
    }
    static Status load(const Io& io,State& s,VramAllocationHandle h,uint64_t offset,
        uint32_t bytes,uint32_t type,uint32_t id,Result& r) {
        Status status=begin(s,io,id,r); if(status!=Status::Ok) return status;
        if(id==0x20 && (s.tmrRetain_ || s.requiredTmr_)) return finish(s,r,Status::InvalidState);
        if(id==6 && !s.tmrReady_) return finish(s,r,Status::InvalidState);
        if(id==6 && !firmwareTypeValid(type)) return finish(s,r,Status::InvalidArgument);
        uint64_t mc=0; status=input(s,h,offset,bytes,mc,r); if(status!=Status::Ok) return status;
        address(s.buffers_.scratch+28,mc); put(s.buffers_.scratch+36,bytes);
        if(id==6) put(s.buffers_.scratch+40,type);
        return submit(s,io,id,h,r);
    }
    static Status setup(const Io& io,State& s,VramAllocationHandle h,Result& r) {
        Status status=begin(s,io,5,r); if(status!=Status::Ok) return status;
        if(!tmrSizeValid(s.requiredTmr_) || s.tmrRetain_) return finish(s,r,Status::InvalidState);
        uint64_t mc=0,physical=0; VramRange range={};
        if(!allocation(*s.allocator_,h,s.requiredTmr_,mc,physical,range) || range.bytes>kMaximumTmr ||
            (mc&0xfffff) || (physical&0xfffff)) return finish(s,r,Status::InvalidAllocation);
        if(same(h,s.buffers_.ring) || same(h,s.buffers_.command) || same(h,s.buffers_.fence))
            return finish(s,r,Status::AliasedAllocation);
        address(s.buffers_.scratch+28,mc); put(s.buffers_.scratch+36,uint32_t(range.bytes));
        put(s.buffers_.scratch+40,2); address(s.buffers_.scratch+44,physical);
        return submit(s,io,5,h,r);
    }
    static Status wait(const Io& io,State& s,const PollOptions& p,Result& r) {
        r=Result{}; r.stage=Stage::Preflight; r.commandId=s.pendingId_;
        if(!s.pending_ || (s.phase_!=Phase::Running && s.phase_!=Phase::Faulted))
            return finish(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p)) return finish(s,r,Status::InvalidArgument);
        if(!bindings(s)) return fault(s,r,Status::InvalidAllocation);
        r.stage=Stage::FenceWait;
        bool complete=false;
        for(uint32_t i=0;i<p.maxPolls;++i) {
            ++r.polls;
            if(!download(s,io,s.buffers_.fence,4,r)) return fault(s,r,r.status);
            r.observedFence=get(s.buffers_.scratch);
            if(r.observedFence!=s.sequence_ && r.observedFence!=s.sequence_-1)
                return fault(s,r,Status::FenceCorrupt);
            uint32_t rp=0,wp=0;
            if(!read(io,s.layout_.wptr,wp,r) || !read(io,s.layout_.rptr,rp,r)) return fault(s,r,r.status);
            const uint32_t previous=(s.wptr_+kRingDwords-kFrameDwords)%kRingDwords;
            if(wp!=s.wptr_ || !pointerValid(rp) || (rp!=s.wptr_ && rp!=previous))
                return fault(s,r,Status::PointerCorrupt);
            if(r.observedFence==s.sequence_ && rp==s.wptr_) { complete=true; break; }
            if(i+1<p.maxPolls) io.delayUs(io.context,p.delayUs);
        }
        if(!complete) return fault(s,r,Status::FenceTimeout);
        r.stage=Stage::ReadResponse;
        if(!download(s,io,s.buffers_.command,kCommandBytes,r)) return fault(s,r,r.status);
        if(get(s.buffers_.scratch+8)!=s.pendingId_) return fault(s,r,Status::ResponseCorrupt);
        r.responseStatus=get(s.buffers_.scratch+kResponse);
        if(r.responseStatus) {
            s.pending_=false; s.pendingInput_={};
            return fault(s,r,Status::CommandRejected);
        }
        const uint32_t id=s.pendingId_;
        if(id==0x20) {
            r.tmrBytes=get(s.buffers_.scratch+kResponse+16);
            if(!tmrSizeValid(r.tmrBytes)) return fault(s,r,Status::InvalidTmrSize);
            s.requiredTmr_=r.tmrBytes;
        } else if(id==5) s.tmrReady_=true;
        else if(id==7) { s.tmrReady_=false; s.tmrRetain_=false; s.tmr_={}; }
        else if(id==0x22) r.bootConfig=get(s.buffers_.scratch+kResponse+64);
        else if(id==6) {
            r.firmwareAddress=uint64_t(get(s.buffers_.scratch+kResponse+8)) |
                (uint64_t(get(s.buffers_.scratch+kResponse+12))<<32);
            uint64_t mc=0,physical=0; VramRange range={};
            if(!allocation(*s.allocator_,s.tmr_,s.requiredTmr_,mc,physical,range))
                return fault(s,r,Status::InvalidAllocation);
            if(r.firmwareAddress && (r.firmwareAddress<mc || r.firmwareAddress-mc>=range.bytes))
                return fault(s,r,Status::ResponseCorrupt);
        }
        s.pending_=false; s.pendingInput_={}; r.stage=Stage::Complete;
        // Observation can retire the pending command after an earlier timeout
        // or ambiguous submit, but never reopens a faulted submission stream.
        return finish(s,r,Status::Ok);
    }
    static Status stop(const Io& io,State& s,const PollOptions& p,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(s.phase_==Phase::Stopped || s.phase_==Phase::Fresh || !s.ringRetain_)
            return finish(s,r,Status::InvalidState);
        if(s.pending_) return finish(s,r,Status::PendingCommand);
        if(!ioValid(io) || !pollValid(p)) return finish(s,r,Status::InvalidArgument);
        if(!bindings(s)) return fault(s,r,Status::InvalidAllocation);
        r.stage=Stage::Stop;
        const bool issued=write(io,s.layout_.control,0x30000,r);
        io.delayMs(io.context,20);
        if(!issued) return fault(s,r,r.status);
        const Status status=mailbox(s.layout_,io,p,r);
        if(status!=Status::Ok) return fault(s,r,status);
        s.ringRetain_=false; s.phase_=Phase::Stopped; r.stage=Stage::Complete;
        return finish(s,r,Status::Ok);
    }
};

Status buildLayout(const Navi31IpLayout& ips,uint64_t bytes,Layout& out) {
    out=Layout{};
    psp::Layout cold={}; const auto status=psp::buildLayout(ips,bytes,cold);
    if(status!=psp::Status::Ok)
        return status==psp::Status::OutsideBar ? Status::OutsideBar : Status::UnsupportedLayout;
    const uint32_t raw[]={0x80,0x83,0x84,0x85,0x86,0x87,0x91}; uint32_t resolved[7]={};
    for(unsigned i=0;i<7;++i) {
        ResolvedRegister r={};
        if(resolveNavi31Register(ips,Navi31Block::Mp0,0,raw[i],bytes,r)!=IpLayoutStatus::Ok)
            return Status::OutsideBar;
        resolved[i]=r.mmioByteOffset;
    }
    const Layout l={true,bytes,resolved[0],resolved[1],resolved[2],resolved[3],resolved[4],resolved[5],resolved[6]};
    if(!layoutValid(l)) return Status::OutsideBar;
    out=l; return Status::Ok;
}
Status create(const Layout& l,const psp::State& sos,const VramAllocator& a,const Buffers& b,
    const Io& io,const PollOptions& p,State& s,Result& r) { return Access::create(l,sos,a,b,io,p,s,r); }
Status validateInputAllocation(const State& s,VramAllocationHandle h,uint64_t off,uint32_t bytes) {
    if(s.pending()) return Status::PendingCommand;
    if(s.phase()!=Phase::Running) return Status::InvalidState;
    if(!Access::bindings(s)) return Status::InvalidAllocation;
    uint64_t mc=0; Result r={}; return Access::input(s,h,off,bytes,mc,r);
}
Status submitBootConfig(const Io& io,State& s,Result& r) { return Access::simple(io,s,0x22,r); }
Status submitLoadToc(const Io& io,State& s,VramAllocationHandle h,uint64_t off,uint32_t bytes,Result& r) {
    return Access::load(io,s,h,off,bytes,0,0x20,r);
}
Status submitSetupTmr(const Io& io,State& s,VramAllocationHandle h,Result& r) { return Access::setup(io,s,h,r); }
Status submitLoadIp(const Io& io,State& s,VramAllocationHandle h,uint64_t off,uint32_t bytes,uint32_t type,Result& r) {
    return Access::load(io,s,h,off,bytes,type,6,r);
}
Status submitAutoloadRlc(const Io& io,State& s,Result& r) { return Access::simple(io,s,0x21,r); }
Status submitDestroyTmr(const Io& io,State& s,Result& r) { return Access::simple(io,s,7,r); }
Status wait(const Io& io,State& s,const PollOptions& p,Result& r) { return Access::wait(io,s,p,r); }
Status stop(const Io& io,State& s,const PollOptions& p,Result& r) { return Access::stop(io,s,p,r); }
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x: return #x
    NAME(Ok); NAME(InvalidArgument); NAME(InvalidState); NAME(UnsupportedLayout); NAME(OutsideBar);
    NAME(InvalidAllocation); NAME(AliasedAllocation); NAME(TransferFailed); NAME(VisibilityFailed);
    NAME(ReadFailed); NAME(WriteFailed); NAME(DeviceUnavailable); NAME(SosUnavailable); NAME(MailboxTimeout);
    NAME(MailboxRejected); NAME(PointerCorrupt); NAME(PendingCommand); NAME(CounterExhausted);
    NAME(FenceTimeout); NAME(FenceCorrupt); NAME(CommandRejected); NAME(ResponseCorrupt); NAME(InvalidTmrSize);
#undef NAME
    }
    return "Unknown";
}
} // namespace pspring
} // namespace hackynavi
