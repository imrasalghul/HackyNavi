// SPDX-License-Identifier: MIT
// Adapted from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// smu_cmn.c V1 mailbox, smu_v13_0.c gfx_off_control/init_msg_ctl,
// smu_v13_0_0_ppsmc.h; imu_v11_0.c wait_for_reset_status;
// gfx_v11_0.c wait_for_rlc_autoload_complete; GC11 and MP13 register headers.
// The combined post-ACK cold-start check is additional port validation.
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
#include "hackynavi/smu.hpp"
namespace hackynavi {
namespace smu {
namespace {
bool pollValid(const PollOptions& p) {
    return p.maxPolls && p.maxPolls<=1000000 && p.delayUs && p.delayUs<=1000 &&
        uint64_t(p.maxPolls)*p.delayUs<=10000000;
}
bool ioValid(const Io& io) { return io.read32 && io.write32 && io.delayUs; }
bool layoutValid(const Layout& l) {
    if(!l.valid || l.barBytes<4 || l.message<0x282*4 || l.imuReset<0x40bc*4) return false;
    const uint64_t mp=l.message-0x282*4,gc1=l.imuReset-0x40bc*4;
    if(mp+0x292*4!=l.argument || mp+0x29a*4!=l.response || gc1+0x4e82*4!=l.bootloadStatus)
        return false;
    const uint32_t regs[]={l.message,l.argument,l.response,l.imuReset,l.cpStatus,l.bootloadStatus};
    for(unsigned i=0;i<6;++i) {
        if((regs[i]&3) || regs[i]>l.barBytes-4 || regs[i]==0 || regs[i]==4 || regs[i]==0x18 ||
            (regs[i]>=0x7f000 && regs[i]<0x80000)) return false;
        for(unsigned j=0;j<i;++j) if(regs[i]==regs[j]) return false;
    }
    return true;
}
Status finish(Result& r,Status status) { r.status=status; return status; }
Status decode(uint32_t response) {
    switch(response) {
    case 1: return Status::Ok;
    case 0xff: return Status::CommandFailed;
    case 0xfe: return Status::UnknownCommand;
    case 0xfd: return Status::BadPrerequisite;
    case 0xfc: return Status::Busy;
    default: return Status::UnknownResponse;
    }
}
bool read(const Io& io,uint32_t reg,uint32_t& value,Result& r) {
    ++r.reads; r.lastRegister=reg;
    if(!io.read32(io.context,reg,value)) { finish(r,Status::ReadFailed); return false; }
    r.lastValue=value;
    if(value==UINT32_MAX) { finish(r,Status::DeviceUnavailable); return false; }
    return true;
}
bool write(const Io& io,uint32_t reg,uint32_t value,Result& r) {
    ++r.writes; r.lastRegister=reg; r.lastValue=value; r.mailboxWritesMayHaveOccurred=true;
    if(!io.write32(io.context,reg,value)) { finish(r,Status::WriteFailed); return false; }
    return true;
}
Status response(const Layout& l,const Io& io,const PollOptions& p,Result& r) {
    for(uint32_t i=0;i<p.maxPolls;++i) {
        ++r.polls;
        if(!read(io,l.response,r.response,r)) return r.status;
        if(r.response) return decode(r.response);
        if(i+1<p.maxPolls) io.delayUs(io.context,p.delayUs);
    }
    return Status::ResponseTimeout;
}
Status send(const Layout& l,const Io& io,const PollOptions& p,uint32_t message,uint32_t argument,
            bool first,bool output,uint32_t& result,Result& r) {
    r.message=message; r.argument=argument; result=0;
    if(!first) {
        r.stage=Stage::PreviousResponse;
        const Status status=response(l,io,p,r); if(status!=Status::Ok) return status;
    }
    r.stage=Stage::ClearResponse;
    if(!write(io,l.response,0,r)) return r.status;
    r.stage=Stage::Argument;
    if(!write(io,l.argument,argument,r)) return r.status;
    r.stage=Stage::Message; r.commandMayHaveOccurred=true;
    if(!write(io,l.message,message,r)) return r.status;
    r.stage=Stage::Response;
    const Status status=response(l,io,p,r); if(status!=Status::Ok) return status;
    if(output) {
        r.stage=Stage::Output;
        if(!read(io,l.argument,result,r)) return r.status;
    }
    return Status::Ok;
}
} // namespace
struct Access {
    static Status end(const State& s,Result& r,Status status) {
        r.smuVersion=s.smuVersion_; r.interfaceVersion=s.interfaceVersion_; r.gfxOffDisabled=s.disabled_;
        return finish(r,status);
    }
    static Status fault(State& s,Result& r,Status status) {
        s.phase_=Phase::Faulted; return end(s,r,status);
    }
    static Status init(const Layout& l,const gfxfw::State& fw,const Io& io,const PollOptions& p,
                       State& s,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(s.phase_!=Phase::Fresh || !fw.autoloadComplete()) return end(s,r,Status::InvalidState);
        if(!layoutValid(l)) return end(s,r,Status::UnsupportedLayout);
        if(!ioValid(io) || !pollValid(p)) return end(s,r,Status::InvalidArgument);
        s.layout_=l;
        Status status=send(l,io,p,2,0,true,true,s.smuVersion_,r);
        if(status!=Status::Ok) return fault(s,r,status);
        if(!s.smuVersion_) return fault(s,r,Status::InvalidVersion);
        status=send(l,io,p,3,0,false,true,s.interfaceVersion_,r);
        if(status!=Status::Ok) return fault(s,r,status);
        if(!s.interfaceVersion_) return fault(s,r,Status::InvalidVersion);
        s.phase_=Phase::Ready; r.stage=Stage::Complete; return end(s,r,Status::Ok);
    }
    static Status awake(const Io& io,const PollOptions& p,State& s,Result& r) {
        for(uint32_t i=0;i<p.maxPolls;++i) {
            ++r.polls; r.stage=Stage::ResetRelease;
            if(!read(io,s.layout_.imuReset,r.imuReset,r)) return fault(s,r,r.status);
            // Read ordinary GC registers only after reset releases for HARD,
            // EA, UTCL2, SDMA and GRBM. Each mask has documented RESETB polarity.
            if((r.imuReset&0x1f)==0x1f) {
                r.stage=Stage::Bootload;
                if(!read(io,s.layout_.cpStatus,r.cpStatus,r) ||
                    !read(io,s.layout_.bootloadStatus,r.bootloadStatus,r)) return fault(s,r,r.status);
                if(!r.cpStatus && (r.bootloadStatus&0x80000000U)) {
                    r.stage=Stage::ResetRelease;
                    if(!read(io,s.layout_.imuReset,r.imuReset,r)) return fault(s,r,r.status);
                    if((r.imuReset&0x1f)==0x1f) {
                        s.phase_=Phase::Awake; r.stage=Stage::Complete; return end(s,r,Status::Ok);
                    }
                }
            }
            if(i+1<p.maxPolls) io.delayUs(io.context,p.delayUs);
        }
        return fault(s,r,Status::AwakeTimeout);
    }
    static Status disable(const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(s.phase_!=Phase::Ready) return end(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p)) return end(s,r,Status::InvalidArgument);
        uint32_t unused=0;
        const Status status=send(s.layout_,io,p,0x29,0,false,false,unused,r);
        if(status!=Status::Ok) return fault(s,r,status);
        s.disabled_=true;
        return awake(io,p,s,r);
    }
    static Status verify(const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(s.phase_!=Phase::Awake || !s.disabled_) return end(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p)) return end(s,r,Status::InvalidArgument);
        return awake(io,p,s,r);
    }
    static Status allow(const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{}; r.stage=Stage::Preflight;
        if(s.phase_!=Phase::Awake || !s.disabled_) return end(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p)) return end(s,r,Status::InvalidArgument);
        // Clear the software power permission before an ambiguous message.
        // GFX accesses must not rely on an old Awake state after any failure.
        s.phase_=Phase::Ready; s.disabled_=false;
        uint32_t unused=0;
        const Status status=send(s.layout_,io,p,0x28,0,false,false,unused,r);
        if(status!=Status::Ok) return fault(s,r,status);
        r.stage=Stage::Complete; return end(s,r,Status::Ok);
    }
};
Status buildLayout(const Navi31IpLayout& ips,uint64_t bytes,Layout& out) {
    out=Layout{}; psp::Layout pspLayout={};
    const auto valid=psp::buildLayout(ips,bytes,pspLayout);
    if(valid!=psp::Status::Ok) return valid==psp::Status::OutsideBar ? Status::OutsideBar : Status::UnsupportedLayout;
    const Navi31Block blocks[]={Navi31Block::Mp1,Navi31Block::Mp1,Navi31Block::Mp1,
        Navi31Block::Gc,Navi31Block::Gc,Navi31Block::Gc};
    const uint32_t base[]={0,0,0,1,0,1},raw[]={0x282,0x292,0x29a,0x40bc,0xf40,0x4e82};
    uint32_t regs[6]={};
    for(unsigned i=0;i<6;++i) {
        ResolvedRegister r={};
        if(resolveNavi31Register(ips,blocks[i],base[i],raw[i],bytes,r)!=IpLayoutStatus::Ok)
            return Status::OutsideBar;
        regs[i]=r.mmioByteOffset;
    }
    const Layout l={true,bytes,regs[0],regs[1],regs[2],regs[3],regs[4],regs[5]};
    if(!layoutValid(l)) return Status::OutsideBar;
    out=l; return Status::Ok;
}
Status initialize(const Layout& l,const gfxfw::State& fw,const Io& io,const PollOptions& p,State& s,Result& r) {
    return Access::init(l,fw,io,p,s,r);
}
Status disableGfxOff(const Io& io,const PollOptions& p,State& s,Result& r) { return Access::disable(io,p,s,r); }
Status verifyAwake(const Io& io,const PollOptions& p,State& s,Result& r) { return Access::verify(io,p,s,r); }
Status allowGfxOffAfterQuiescence(const Io& io,const PollOptions& p,State& s,Result& r) {
    return Access::allow(io,p,s,r);
}
const char* statusString(Status status) {
    switch(status) {
#define NAME(x) case Status::x: return #x
    NAME(Ok); NAME(InvalidArgument); NAME(InvalidState); NAME(UnsupportedLayout); NAME(OutsideBar);
    NAME(ReadFailed); NAME(WriteFailed); NAME(DeviceUnavailable); NAME(ResponseTimeout); NAME(CommandFailed);
    NAME(UnknownCommand); NAME(BadPrerequisite); NAME(Busy); NAME(UnknownResponse); NAME(InvalidVersion); NAME(AwakeTimeout);
#undef NAME
    }
    return "Unknown";
}
} // namespace smu
} // namespace hackynavi
