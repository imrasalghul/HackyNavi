// SPDX-License-Identifier: MIT
/*
 * DCN3.2 single-pipe timing sequence adapted from Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
 * display/dc/optc/dcn10/dcn10_optc.c, dcn30/dcn30_optc.c,
 * dcn32/dcn32_optc.c and generated dcn_3_2_0_offset.h/sh_mask.h.
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
#include "hackynavi/otg.hpp"
namespace hackynavi { namespace otg { namespace {
const uint32_t offsets[kRegisterCount]={
    0x1b2a,0x1b2b,0x1b2c,0x1b2d,0x1b2f,0x1b36,0x1b37,0x1b38,
    0x1b30,0x1b31,0x1b33,0x1b44,0x1b41,0x1b85,0x1b2e,
    0x1b86,0x1b87,0x1b88,0x1b4c,0x1b8a,0x1b91,0x1b8b,0x1b54,
    0x1b82,0x1b5b,0x1b63,0x1b65,0x1b67,0x52e,0x1acf,0x1aca,
    0x1acb,0x1ad0,0x1acc
};
constexpr uint32_t kPair=0x7fff7fff,kMaster=0x10001,kClockOn=0x100,kBusy=0x10000;
constexpr uint32_t kSourceMask=0xffff0303,kPending=0x6f1;
struct Dmu { uint32_t count;uint64_t base;bool valid; };
void collect(const DiscoveryIp& ip,void* context) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId && !ip.instance && ip.major==3 && ip.minor==2 && !ip.revision &&
        !ip.subRevision && !ip.variant && !ip.harvest && ip.baseAddressCount>=3 && ip.baseAddresses[2]<=UINT32_MAX;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool populate(uint64_t base,uint64_t bytes,Layout& out) {
    out={};if(bytes<4 || base>UINT32_MAX/4)return false;
    for(size_t pipe=0;pipe<kPipeCount;++pipe)for(size_t reg=0;reg<kRegisterCount;++reg) {
        const uint64_t stride=reg<size_t(Register::Vtg)?0x80u:reg==size_t(Register::Vtg)?1u:0x10u;
        const uint64_t address=(base+offsets[reg]+pipe*stride)*4;
        if(address>UINT32_MAX || address>bytes-4)return false;
        out.registers[pipe][reg]=uint32_t(address);
    }
    out.valid=true;out.barBytes=bytes;return true;
}
bool validLayout(const Layout& l) {
    if(!l.valid || (l.registers[0][0]&3) || l.registers[0][0]/4<offsets[0])return false;
    Layout expected={};if(!populate(l.registers[0][0]/4-offsets[0],l.barBytes,expected))return false;
    for(size_t p=0;p<kPipeCount;++p)for(size_t r=0;r<kRegisterCount;++r)
        if(l.registers[p][r]!=expected.registers[p][r])return false;
    return true;
}
bool validPoll(const Poll& p) { return p.maxPolls && p.maxPolls<=2000000 && p.delayUs && p.delayUs<=100 && uint64_t(p.maxPolls)*p.delayUs<=2000000; }
struct Value { Register reg;uint32_t mask,value; };
size_t timingEntries(const TimingValues& t,Value* v) {
    const Value all[]={
        {Register::HTotal,0x7fff,t.hTotal},{Register::HSync,kPair,t.hSync},
        {Register::HBlank,kPair,t.hBlank},{Register::HSyncControl,1,t.hPolarity},
        {Register::VTotal,0x7fff,t.vTotal},{Register::VTotalMax,0x7fff,t.vTotal},
        {Register::VTotalMin,0x7fff,t.vTotal},{Register::VSync,kPair,t.vSync},
        {Register::VBlank,kPair,t.vBlank},{Register::VSyncControl,1,t.vPolarity},
        {Register::Control,0x3000,t.startPoint},{Register::VStartup,0x3ff,t.vStartup},
        {Register::VUpdate,0x3ffffff,t.vUpdate},{Register::VReady,0xffff,t.vReady},
        {Register::Vtg,kPair,t.vtg}
    };
    for(size_t i=0;i<sizeof(all)/sizeof(all[0]);++i)v[i]=all[i];
    return sizeof(all)/sizeof(all[0]);
}
} // namespace
Status buildLayout(const uint8_t* bytes,size_t length,uint64_t bar,Layout& out) {
    out={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(bytes,length,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1 || !d.valid || info.baseAddresses64Bit || info.dieCount!=1 || info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.base,bar,out)?Status::Ok:Status::OutsideBar;
}
Status encode(const edid::Timing& t,const GlobalSync& s,Signal signal,TimingValues& out) {
    out={};
    if(signal!=Signal::Hdmi && signal!=Signal::DisplayPort)return Status::InvalidArgument;
    if(!t.pixelClockKHz || !t.width || !t.height || t.width>=t.hSyncStart || t.hSyncStart>=t.hSyncEnd ||
        t.hSyncEnd>t.hTotal || t.height>=t.vSyncStart || t.vSyncStart>=t.vSyncEnd || t.vSyncEnd>t.vTotal ||
        t.hTotal>32768 || t.vTotal>32768 || t.hTotal-t.width<32 || t.vTotal-t.height<3 ||
        t.hSyncEnd-t.hSyncStart<4)return Status::InvalidTiming;
    if(!s.vstartup || s.vstartup>0x3ff || s.vstartup>t.vTotal || s.vupdateWidth>0x3ff)return Status::InvalidSync;
    const uint32_t hBlankStart=uint32_t(t.hTotal)-(t.hSyncStart-t.width);
    const uint32_t vBlankStart=uint32_t(t.vTotal)-(t.vSyncStart-t.height);
    const uint32_t hBlankEnd=hBlankStart-t.width,vBlankEnd=vBlankStart-t.height;
    const uint32_t fp2=s.vstartup>vBlankEnd+1?s.vstartup-vBlankEnd-1:0;
    out.hTotal=uint32_t(t.hTotal)-1;out.hBlank=hBlankStart|(hBlankEnd<<16);
    out.hSync=uint32_t(t.hSyncEnd-t.hSyncStart)<<16;out.hPolarity=t.hPositive?0u:1u;
    out.vTotal=uint32_t(t.vTotal)-1;out.vBlank=vBlankStart|(vBlankEnd<<16);
    out.vSync=uint32_t(t.vSyncEnd-t.vSyncStart)<<16;out.vPolarity=t.vPositive?0u:1u;
    out.vtg=fp2|(vBlankStart<<16);out.startPoint=signal==Signal::DisplayPort?0x1000u:0u;
    out.vStartup=s.vstartup;out.vUpdate=uint32_t(s.vupdateOffset)|(uint32_t(s.vupdateWidth)<<16);out.vReady=s.vreadyOffset;
    return Status::Ok;
}
Controller::Controller():layout_{},io_{},phase_(Phase::Closed),pipe_(0),touched_(false),timing_{},signal_(Signal::Hdmi) {}
void Controller::clearAfterVerifiedReset() { layout_={};io_={};phase_=Phase::Closed;pipe_=0;touched_=false;timing_={};signal_=Signal::Hdmi; }
Status Controller::finish(Status s,Result& r) {
    r.status=s;
    if(s!=Status::Ok) {
        if(touched_)phase_=Phase::Faulted;
        else clearAfterVerifiedReset();
    }
    return s;
}
Status Controller::read(uint8_t pipe,Register reg,uint32_t& value,Result& r) {
    r.lastRegister=layout_.registers[pipe][size_t(reg)];++r.reads;
    if(!io_.read32(io_.context,r.lastRegister,value))return Status::TransportError;
    r.lastValue=value;return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::update(Register reg,uint32_t mask,uint32_t value,Result& r) {
    uint32_t old=0;auto s=read(pipe_,reg,old,r);if(s!=Status::Ok)return s;
    r.lastValue=(old&~mask)|(value&mask);r.writeAttempted=touched_=true;++r.writes;
    if(!io_.write32(io_.context,r.lastRegister,r.lastValue))return Status::TransportError;
    s=read(pipe_,reg,old,r);if(s!=Status::Ok)return s;
    return (old&mask)==(value&mask)?Status::Ok:Status::VerifyFailed;
}
Status Controller::wait(Register reg,uint32_t mask,uint32_t value,const Poll& p,Status timeout,Result& r) {
    for(uint32_t i=0;i<p.maxPolls;++i) {
        uint32_t observed=0;const auto s=read(pipe_,reg,observed,r);if(s!=Status::Ok)return s;
        ++r.polls;if((observed&mask)==value)return Status::Ok;
        if(i+1<p.maxPolls)io_.delayUs(io_.context,p.delayUs);
    }
    return timeout;
}
Status Controller::admit(Result& r) {
    uint32_t value=0;auto s=read(pipe_,Register::Control,value,r);if(s!=Status::Ok)return s;
    if(value&kMaster)return Status::ActivePipe;
    if(value&0x00300000)return Status::ForeignControl;
    s=read(pipe_,Register::Clock,value,r);if(s!=Status::Ok)return s;
    if(value&kBusy)return Status::ActivePipe;if(value&0x10)return Status::ForeignControl;
    const Value guards[]={
        {Register::Vtg,0x80000000,0},{Register::UpdateLock,0x101,0},
        {Register::GlobalControl,0x40070400,0},{Register::Gsl,0x8000000f,0},
        {Register::Stereo,0x01000000,0},{Register::Structure3d,0x1101,0},
        {Register::Interlace,0x30001,0},{Register::DataFormat,0x30,0},
        {Register::InputGlobal,0x80000101,0},{Register::DoubleBuffer,kPending,0},
        {Register::Interrupt0,0x100,0},{Register::Interrupt1,0x100,0},{Register::Interrupt2,0x100,0},
        {Register::HSyncControl,0x30000,0},{Register::VSyncControl,0x100,0}
    };
    for(const auto& g:guards) { s=read(pipe_,g.reg,value,r);if(s!=Status::Ok)return s;if(value&g.mask)return Status::ForeignControl; }
    // No other running OTG may consume this OPP, including any of its ODM
    // segments. Stale routing on another disabled OTG does not fetch pixels.
    for(uint8_t pipe=0;pipe<kPipeCount;++pipe)if(pipe!=pipe_) {
        s=read(pipe,Register::Control,value,r);if(s!=Status::Ok)return s;
        if(value&kMaster) {
            s=read(pipe,Register::DataSource,value,r);if(s!=Status::Ok)return s;
            for(unsigned segment=0;segment<4;++segment)if(((value>>(16+segment*4))&15)==pipe_)return Status::ForeignControl;
        }
    }
    return Status::Ok;
}
Status Controller::clearUnderflow(Result& r) {
    uint32_t value=0;auto s=read(pipe_,Register::InputGlobal,value,r);if(s!=Status::Ok)return s;
    r.underflowObserved=r.underflowObserved || (value&0x2400)!=0;
    r.lastValue=value|0x1000;r.writeAttempted=touched_=true;++r.writes;
    if(!io_.write32(io_.context,r.lastRegister,r.lastValue))return Status::TransportError;
    // UNDERFLOW_CLEAR is a strobe; do not require it to read back as one.
    s=read(pipe_,Register::InputGlobal,value,r);if(s!=Status::Ok)return s;
    return value&0x400?Status::VerifyFailed:Status::Ok;
}
Status Controller::configure(const Layout& l,const Io& io,uint8_t pipe,const edid::Timing& timing,
    const GlobalSync& sync,Signal signal,const Poll& poll,Result& r) {
    r={};
    if(phase_!=Phase::Closed && phase_!=Phase::Stopped) {r.status=phase_==Phase::Faulted?Status::Faulted:Status::InvalidState;return r.status;}
    if(!validLayout(l) || pipe>=kPipeCount || !io.read32 || !io.write32 || !io.delayUs || !validPoll(poll))return finish(Status::InvalidArgument,r);
    TimingValues encoded={};auto s=encode(timing,sync,signal,encoded);if(s!=Status::Ok)return finish(s,r);
    layout_=l;io_=io;pipe_=pipe;timing_=encoded;signal_=signal;
    s=admit(r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Programming;
    s=update(Register::InputClock,3,3,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::InputClock,4,4,poll,Status::ClockTimeout,r);if(s!=Status::Ok)return finish(s,r);r.inputClockOn=true;
    s=update(Register::Clock,3,3,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Clock,kClockOn,kClockOn,poll,Status::ClockTimeout,r);if(s!=Status::Ok)return finish(s,r);r.otgClockOn=true;
    s=clearUnderflow(r);if(s!=Status::Ok)return finish(s,r);
    // optc32_set_odm_bypass: own OPP, no other segments or ODM memory.
    const Value initial[]={
        {Register::DataSource,kSourceMask,0xfff00000u|(uint32_t(pipe_)<<16)},
        {Register::Memory,0xffff,0},{Register::HTiming,0x103,0},{Register::DataFormat,0x33,0},
        {Register::VTotalControl,0xffffff3f,0},{Register::Interlace,1,0},
        {Register::Vtg,0x80000000,0},{Register::DoubleBuffer,0x03000000,0x02000000}
    };
    for(const auto& v:initial) {s=update(v.reg,v.mask,v.value,r);if(s!=Status::Ok)return finish(s,r);}
    Value values[15]={};const size_t count=timingEntries(timing_,values);
    for(size_t i=0;i<count;++i) {s=update(values[i].reg,values[i].mask,values[i].value,r);if(s!=Status::Ok)return finish(s,r);}
    s=verifyProgrammed(r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Programmed;return finish(Status::Ok,r);
}
Status Controller::verifyProgrammed(Result& r) {
    uint32_t value=0;auto s=read(pipe_,Register::Control,value,r);if(s!=Status::Ok)return s;
    if(value&kMaster)return Status::ActivePipe;
    const Value guards[]={
        {Register::InputClock,7,7},{Register::Clock,0x10113,0x103},
        {Register::DataSource,kSourceMask,0xfff00000u|(uint32_t(pipe_)<<16)},
        {Register::DataFormat,0x33,0},{Register::Memory,0xffff,0},
        {Register::HTiming,0x30103,0},{Register::Interlace,1,0},
        {Register::Vtg,0x80000000,0},{Register::UpdateLock,0x101,0},
        {Register::VTotalControl,0xffffff3f,0}
    };
    for(const auto& g:guards) {
        s=read(pipe_,g.reg,value,r);if(s!=Status::Ok)return s;
        if((value&g.mask)!=g.value)return Status::VerifyFailed;
    }
    Value values[15]={};const size_t count=timingEntries(timing_,values);
    for(size_t i=0;i<count;++i) {
        s=read(pipe_,values[i].reg,value,r);if(s!=Status::Ok)return s;
        if((value&values[i].mask)!=values[i].value)return Status::VerifyFailed;
    }
    return Status::Ok;
}
Status Controller::enable(const Poll& poll,Result& r) {
    r={};if(phase_!=Phase::Programmed) {r.status=phase_==Phase::Faulted?Status::Faulted:Status::InvalidState;return r.status;}
    if(!validPoll(poll))return finish(Status::InvalidArgument,r);
    auto s=verifyProgrammed(r);if(s!=Status::Ok)return finish(s,r);
    s=read(pipe_,Register::FrameCount,r.firstFrame,r);if(s!=Status::Ok)return finish(s,r);r.firstFrame&=0xffffff;
    phase_=Phase::Enabling;
    s=update(Register::DataSource,0xf0000,uint32_t(pipe_)<<16,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Vtg,0x80000000,0x80000000,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Control,0x301,0x201,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Control,kMaster,kMaster,poll,Status::EnableTimeout,r);if(s!=Status::Ok)return finish(s,r);r.currentEnabled=true;
    s=wait(Register::InputClock,4,4,poll,Status::ClockTimeout,r);if(s!=Status::Ok)return finish(s,r);r.inputClockOn=true;
    s=wait(Register::Clock,kClockOn,kClockOn,poll,Status::ClockTimeout,r);if(s!=Status::Ok)return finish(s,r);r.otgClockOn=true;
    for(uint32_t i=0;i<poll.maxPolls;++i) {
        s=read(pipe_,Register::FrameCount,r.lastFrame,r);if(s!=Status::Ok)return finish(s,r);r.lastFrame&=0xffffff;++r.polls;
        if(r.lastFrame!=r.firstFrame) {r.frameAdvanced=true;break;}
        if(i+1<poll.maxPolls)io_.delayUs(io_.context,poll.delayUs);
    }
    if(!r.frameAdvanced)return finish(Status::FrameTimeout,r);
    uint32_t value=0;s=read(pipe_,Register::Control,value,r);if(s!=Status::Ok)return finish(s,r);
    if((value&kMaster)!=kMaster)return finish(Status::VerifyFailed,r);
    s=read(pipe_,Register::InputGlobal,value,r);if(s!=Status::Ok)return finish(s,r);
    if(value&0x2400) {r.underflowObserved=true;return finish(Status::Underflow,r);}
    phase_=Phase::Running;return finish(Status::Ok,r);
}
Status Controller::disable(const Poll& poll,Result& r) {
    r={};if(!touched_ || (phase_!=Phase::Programmed && phase_!=Phase::Running && phase_!=Phase::Faulted)) {r.status=Status::InvalidState;return r.status;}
    if(!validPoll(poll))return finish(Status::InvalidArgument,r);
    phase_=Phase::Disabling;
    auto s=update(Register::DataSource,kSourceMask,0xffff0000,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Memory,0xffff,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Control,1,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Vtg,0x80000000,0,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Control,kMaster,0,poll,Status::DisableTimeout,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Clock,kBusy,0,poll,Status::DisableTimeout,r);if(s!=Status::Ok)return finish(s,r);r.busyCleared=true;
    s=clearUnderflow(r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Clock,3,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::InputClock,3,0,r);if(s!=Status::Ok)return finish(s,r);r.clockControlsDisabled=true;
    phase_=Phase::Stopped;touched_=false;return finish(Status::Ok,r);
}
const char* statusString(Status s) {
    switch(s) {
    case Status::Ok:return "ok";case Status::InvalidArgument:return "invalid-argument";case Status::DiscoveryRejected:return "discovery-rejected";
    case Status::UnsupportedDmu:return "unsupported-dmu";case Status::OutsideBar:return "outside-bar";case Status::InvalidTiming:return "invalid-timing";
    case Status::InvalidSync:return "invalid-sync";case Status::InvalidState:return "invalid-state";case Status::ActivePipe:return "active-pipe";
    case Status::ForeignControl:return "foreign-control";case Status::TransportError:return "transport-error";case Status::DeviceUnavailable:return "device-unavailable";
    case Status::VerifyFailed:return "verify-failed";case Status::ClockTimeout:return "clock-timeout";case Status::EnableTimeout:return "enable-timeout";
    case Status::FrameTimeout:return "frame-timeout";case Status::DisableTimeout:return "disable-timeout";case Status::Underflow:return "underflow";
    case Status::Faulted:return "faulted";
    }
    return "unknown";
}
} }
