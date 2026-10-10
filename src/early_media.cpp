// SPDX-License-Identifier: MIT
// Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0 sources:
// smu_v13_0.c check_fw_status/set_vcn_enable/set_jpeg_enable/init_msg_ctl;
// smu_cmn.c V1 mailbox; smu_v13_0.h; smu_v13_0_0_ppsmc.h;
// vcn_v4_0_start and jpeg_v4_0_start power the enclosing domain before PGFSM.
// Cold pre-PSP sequencing, retained-owner guards and deadline enforcement are
// port policy, not a claim that Linux activates media before its PSP loader.
// Copyright 2020-2022 Advanced Micro Devices, Inc.
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
#include "hackynavi/early_media.hpp"
namespace hackynavi { namespace earlymedia {
namespace {
bool bindingValid(const Binding& b) { return b.ownerInstance && b.powerGeneration && b.resetGeneration; }
bool limitsValid(const Limits& l) {
    return l.totalTimeoutUs && l.totalTimeoutUs<=10000000 && l.responseTimeoutUs &&
        l.responseTimeoutUs<=l.totalTimeoutUs && l.maxPolls && l.maxPolls<=1000000 &&
        l.pollDelayUs && l.pollDelayUs<=1000;
}
bool ioValid(const Io& i) { return i.context && i.enter && i.leave && i.valid && i.holdOwner &&
    i.read32 && i.write32 && i.readSmn32 && i.nowUs && i.delayUs; }
Status decode(uint32_t v) {
    switch(v) {case 1:return Status::Ok;case 0xff:return Status::CommandFailed;
    case 0xfe:return Status::UnknownCommand;case 0xfd:return Status::BadPrerequisite;
    case 0xfc:return Status::Busy;default:return Status::UnknownResponse;}
}
struct Gate { const Io& io; ~Gate(){io.leave(io.context);} };
}
struct Access {
    struct Run {
        const Io& io; const Limits& limits; State& state; Result& result;
        uint64_t last,deadline; bool timed;
        Run(const Io& i,const Limits& l,State& s,Result& r):io(i),limits(l),state(s),result(r),last(0),deadline(0),timed(false) {}
        bool fail(Status s){result.status=s;return false;}
        bool check() {
            if(!io.valid(io.context,state.binding_))return fail(Status::OwnerRevoked);
            uint64_t now=0;
            if(!io.nowUs(io.context,now))return fail(Status::ClockFailed);
            if(timed && now<last)return fail(Status::ClockRegressed);
            last=now;
            if(!timed) {
                if(now>UINT64_MAX-limits.totalTimeoutUs)return fail(Status::ClockFailed);
                deadline=now+limits.totalTimeoutUs;timed=true;
            }
            return now<deadline || fail(Status::DeadlineExpired);
        }
        bool read(uint32_t address,uint32_t& value,bool reportAllOnes=false) {
            if(!check())return false;
            ++result.reads;result.lastRegister=address;
            if(!io.read32(io.context,address,value))return fail(Status::ReadFailed);
            result.lastValue=value;
            if(value==UINT32_MAX) {fail(Status::DeviceUnavailable);return reportAllOnes;}
            return check();
        }
        bool write(uint32_t address,uint32_t value,uint8_t domain=0) {
            if(!check())return false;
            ++result.writes;result.lastRegister=address;result.lastValue=value;
            state.touched_=true;state.attempted_|=domain;
            if(!io.write32(io.context,address,value))return fail(Status::WriteFailed);
            return check();
        }
        // The existing cold/posted checkers also reject all-ones. Let them
        // record the actual value and register rather than mislabel it as a
        // callback transport failure; the outer status still fails closed.
        static bool auxRead(void* c,uint32_t a,uint32_t& v) {return static_cast<Run*>(c)->read(a,v,true);}
        bool flags() {
            result.stage=Stage::FirmwareFlags;
            if(!check())return false;
            ++result.smnReads;result.lastRegister=state.layout_.regs_.firmwareFlagsSmn;
            state.indirect_=true;
            if(!io.readSmn32(io.context,result.lastRegister,result.firmwareFlags))return fail(Status::ReadFailed);
            result.lastValue=result.firmwareFlags;
            if(result.firmwareFlags==UINT32_MAX)return fail(Status::DeviceUnavailable);
            if(!check())return false;
            return (result.firmwareFlags&1)!=0 || fail(Status::FirmwareNotRunning);
        }
        bool posted() {
            result.stage=Stage::PostedState;
            const auto status=bootstrap::checkPostedColdState(state.layout_.ips_,state.layout_.barBytes_,
                state.layout_.scratchDwords_,{this,auxRead,nullptr,nullptr},result.posted);
            if(result.status!=Status::Ok)return false;
            return status==bootstrap::Status::Ok || fail(Status::PostedStateRejected);
        }
        bool response() {
            if(!check())return false;
            const uint64_t start=last;
            for(uint32_t p=0;p<limits.maxPolls;++p) {
                if(!check())return false;
                if(last-start>=limits.responseTimeoutUs)return fail(Status::ResponseTimeout);
                ++result.polls;
                if(!read(state.layout_.regs_.response,result.response))return false;
                if(last-start>=limits.responseTimeoutUs)return fail(Status::ResponseTimeout);
                if(result.response) {const auto s=decode(result.response);return s==Status::Ok || fail(s);}
                if(p+1<limits.maxPolls) {
                    if(!check())return false;
                    if(last-start>=limits.responseTimeoutUs)return fail(Status::ResponseTimeout);
                    const uint64_t responseRemaining=limits.responseTimeoutUs-(last-start);
                    const uint64_t remaining=responseRemaining<deadline-last?responseRemaining:deadline-last;
                    const uint32_t delay=remaining<limits.pollDelayUs?static_cast<uint32_t>(remaining):limits.pollDelayUs;
                    if(!delay)return fail(Status::ResponseTimeout);
                    io.delayUs(io.context,delay);
                }
            }
            return fail(Status::ResponseTimeout);
        }
        bool command(uint32_t message,uint32_t argument,bool first,uint32_t* output,uint8_t domain=0) {
            result.message=message;result.argument=argument;
            if(!first) {result.stage=Stage::PreviousResponse;if(!response())return false;}
            result.stage=Stage::ClearResponse;if(!write(state.layout_.regs_.response,0))return false;
            result.stage=Stage::Argument;if(!write(state.layout_.regs_.argument,argument))return false;
            result.stage=Stage::Message;if(!write(state.layout_.regs_.message,message,domain))return false;
            result.stage=Stage::Response;if(!response())return false;
            state.acknowledged_|=domain;
            if(output){result.stage=Stage::Output;if(!read(state.layout_.regs_.argument,*output))return false;
                if(!*output)return fail(Status::InvalidVersion);}
            return true;
        }
        bool auxiliary() {
            result.stage=Stage::Auxiliary;
            const auto status=coldaux::checkStopped(state.layout_.aux_,{this,auxRead},result.auxiliary);
            if(result.status!=Status::Ok)return false;
            return status==coldaux::Status::Ok || fail(Status::AuxiliaryNotStopped);
        }
    };
    static Status end(State& s,Result& r,Status status) {
        r.status=status;r.ownerHeld=s.ownerHeld_;r.mailboxTouched=s.touched_;r.indirectReadAttempted=s.indirect_;
        r.attemptedDomains=s.attempted_;r.acknowledgedDomains=s.acknowledged_;
        r.smuVersion=s.smuVersion_;r.interfaceVersion=s.interfaceVersion_;
        return status;
    }
    static Status fault(State& s,Result& r) {s.phase_=Phase::Faulted;return end(s,r,r.status);}
    static Status layout(const uint8_t* data,size_t count,uint64_t bar,uint32_t scratch,Layout& out) {
        out=Layout{};Layout l;IpLayoutDiagnostic diagnostic={};
        if(buildNavi31IpLayout(data,count,l.ips_,diagnostic)!=IpLayoutStatus::Ok ||
            coldaux::buildLayout(data,count,bar,l.aux_)!=coldaux::Status::Ok)return Status::UnsupportedLayout;
        // BIOS scratch7 is resolved exactly as the posted-state checker does.
        if(!scratch || uint64_t(scratch)+7>UINT32_MAX/4)return Status::OutsideBar;
        const uint32_t s=(scratch+7)*4;
        if(bar<4 || s>bar-4 || s<0x1c || (s>=0x7f000 && s<0x80000))return Status::OutsideBar;
        uint32_t regs[3]={};const uint32_t raw[3]={0x282,0x292,0x29a};
        for(unsigned i=0;i<3;++i) {
            ResolvedRegister r={};
            if(resolveNavi31Register(l.ips_,Navi31Block::Mp1,0,raw[i],bar,r)!=IpLayoutStatus::Ok)
                return Status::OutsideBar;
            regs[i]=r.mmioByteOffset;
            if(regs[i]<0x1c || (regs[i]>=0x7f000 && regs[i]<0x80000) || regs[i]==s || regs[i]==l.aux_.sos)
                return Status::OutsideBar;
        }
        if(s==l.aux_.sos)return Status::OutsideBar;
        // No mailbox or BIOS scratch address may alias the read-only media
        // observation window, including adversarial discovery base values.
        uint32_t observed[1+4+2*coldaux::kVcnRegisters+coldaux::kJpegRegisters+coldaux::kLsdmaRegisters]={};
        size_t n=0;observed[n++]=l.aux_.sos;
        for(unsigned v=0;v<2;++v) {
            observed[n++]=l.aux_.vcnPower[v];observed[n++]=l.aux_.vcnPowerConfig[v];
            for(uint32_t a:l.aux_.vcn[v])observed[n++]=a;
        }
        for(uint32_t a:l.aux_.jpeg)observed[n++]=a;
        for(uint32_t a:l.aux_.lsdma)observed[n++]=a;
        for(size_t i=0;i<n;++i) {
            if(s==observed[i])return Status::OutsideBar;
            for(uint32_t a:regs)if(a==observed[i])return Status::OutsideBar;
        }
        l.regs_={regs[0],regs[1],regs[2],0x03b10024};l.scratchDwords_=scratch;l.barBytes_=bar;l.valid_=true;
        out=l;return Status::Ok;
    }
    static Status prepare(const Layout& l,const Binding& binding,const Io& io,const Limits& limits,State& s,Result& r) {
        r=Result{};r.stage=Stage::Preflight;
        if(s.phase_!=Phase::Fresh)return end(s,r,Status::InvalidState);
        if(!l.valid_)return end(s,r,Status::UnsupportedLayout);
        if(!bindingValid(binding) || !ioValid(io) || !limitsValid(limits))return end(s,r,Status::InvalidArgument);
        r.stage=Stage::Owner;
        if(!io.enter(io.context,binding,Operation::Prepare))return end(s,r,Status::OwnerUnavailable);
        Gate gate={io};s.phase_=Phase::Preparing;s.layout_=l;s.binding_=binding;s.context_=io.context;
        Run run(io,limits,s,r);
        if(!run.check() || !run.posted())return fault(s,r);
        // Retain before SMN selector access: even a failing callback can have
        // changed the selector. This is a real native hold, not a readiness bit.
        if(!io.holdOwner(io.context)){r.status=Status::OwnerHoldFailed;return fault(s,r);}
        s.ownerHeld_=true;
        if(!run.flags() || !run.command(2,0,true,&s.smuVersion_) ||
            !run.command(3,0,false,&s.interfaceVersion_))return fault(s,r);
        for(unsigned d=0;d<3;++d) {
            if(!run.posted() || !run.flags() ||
                !run.command(d==2?0x2c:0x2a,d==1?0x10000:0,false,nullptr,static_cast<uint8_t>(1U<<d)))
                return fault(s,r);
        }
        if(!run.flags() || !run.auxiliary() || !run.check())return fault(s,r);
        s.phase_=Phase::Stopped;r.stage=Stage::Complete;return end(s,r,Status::Ok);
    }
    static Status verify(const Io& io,const Limits& limits,State& s,Result& r) {
        r=Result{};r.stage=Stage::Preflight;
        if(s.phase_!=Phase::Stopped || !s.ownerHeld_ || s.acknowledged_!=7)return end(s,r,Status::InvalidState);
        if(!ioValid(io) || !limitsValid(limits) || io.context!=s.context_)return end(s,r,Status::InvalidArgument);
        r.stage=Stage::Owner;
        if(!io.enter(io.context,s.binding_,Operation::Verify)) {
            r.status=Status::OwnerUnavailable;return fault(s,r);
        }
        Gate gate={io};Run run(io,limits,s,r);
        if(!run.check() || !run.flags() || !run.auxiliary() || !run.check())return fault(s,r);
        r.stage=Stage::Complete;return end(s,r,Status::Ok);
    }
};
Status buildLayout(const uint8_t* d,size_t n,uint64_t b,uint32_t s,Layout& l){return Access::layout(d,n,b,s,l);}
Status prepare(const Layout& l,const Binding& b,const Io& io,const Limits& p,State& s,Result& r){return Access::prepare(l,b,io,p,s,r);}
Status verify(const Io& io,const Limits& p,State& s,Result& r){return Access::verify(io,p,s,r);}
const char* statusString(Status s){switch(s){
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(UnsupportedLayout);NAME(OutsideBar);
    NAME(OwnerUnavailable);NAME(OwnerRevoked);NAME(OwnerHoldFailed);NAME(ClockFailed);NAME(ClockRegressed);
    NAME(DeadlineExpired);NAME(ReadFailed);NAME(WriteFailed);NAME(DeviceUnavailable);NAME(FirmwareNotRunning);
    NAME(PostedStateRejected);NAME(ResponseTimeout);NAME(CommandFailed);NAME(UnknownCommand);
    NAME(BadPrerequisite);NAME(Busy);NAME(UnknownResponse);NAME(InvalidVersion);NAME(AuxiliaryNotStopped);
#undef NAME
    }return "Unknown";}
} }
