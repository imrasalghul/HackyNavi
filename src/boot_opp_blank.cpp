// SPDX-License-Identifier: MIT
/*
 * Copyright 2012-15 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
// Source: pinned Linux dcn32_init_blank -> opp2_set_disp_pattern_generator,
// opp2_dpg_set_blank_color, optc1_get_otg_active_size, opp2_dpg_is_blanked.
// This is a one-shot port policy, not an adoption of a cold OPP controller.
#include "hackynavi/boot_opp_blank.hpp"
namespace hackynavi { namespace bootopp { namespace {
struct Dmu { unsigned count;bool valid;uint64_t base; };
void collect(const DiscoveryIp& ip,void* p) {
    if(ip.hardwareId!=271)return;auto& d=*static_cast<Dmu*>(p);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid)d.base=ip.baseAddresses[2];
}
const uint16_t offsets[kRegisters]={0x1854,0x1856,0x185a,0x1859,0x1858,0x1857,
    0x185b,0x188c,0x1840,0x1b2b,0x1b36,0x1b41,0x1b85};
bool timing(Register r){return size_t(r)>=size_t(Register::Horizontal);}
bool writable(Register r){return size_t(r)<size_t(Register::Pending);}
bool sameBackend(const bootbackend::Capture& a,const bootbackend::Capture& b) {
    if(!a.valid||!b.valid||a.path!=b.path||a.backend!=b.backend||a.frontendIndex!=b.frontendIndex||
        a.timingIndex!=b.timingIndex||a.opp!=b.opp||a.mpcc!=b.mpcc||a.dpp!=b.dpp)return false;
    for(size_t n=0;n<bootbackend::kLinks;++n) {
        for(size_t r=0;r<bootbackend::kLinkRegisters;++r)if(a.link[n][r]!=b.link[n][r])return false;
        for(size_t r=0;r<bootbackend::kFrontendRegisters;++r)if(a.frontend[n][r]!=b.frontend[n][r])return false;
    }
    for(size_t n=0;n<bootbackend::kPipes;++n) {
        for(size_t r=0;r<bootbackend::kTimingRegisters;++r)if(a.timing[n][r]!=b.timing[n][r])return false;
        for(size_t r=0;r<bootbackend::kCombinerRegisters;++r)if(a.combiner[n][r]!=b.combiner[n][r])return false;
    }
    return true;
}
}
Controller::Controller():phase_(Phase::Fresh),touched_(false),io_{},addresses_{},expected_{},before_{},after_{} {}
Status Controller::finish(Status s,Result& r) {
    r.status=s;r.writesMayHaveOccurred=touched_;r.blanked=phase_==Phase::Blanked;
    if(s!=Status::Ok&&phase_==Phase::Blanking)phase_=Phase::Faulted;
    return s;
}
bool Controller::backendValid(void* p) {
    auto& s=*static_cast<Controller*>(p);return s.io_.backend.valid(s.io_.backend.context);
}
bool Controller::backendCanRead(void* p,bootbackend::Bank bank,uint8_t n,uint32_t offset) {
    auto& s=*static_cast<Controller*>(p);
    // A malformed discovery must not turn an OPP write into a backend write.
    for(size_t i=0;i<4;++i)for(size_t r=0;r<size_t(Register::Pending);++r)
        if(s.addresses_[i][r]==offset)return false;
    return s.io_.backend.canRead(s.io_.backend.context,bank,n,offset);
}
bool Controller::backendRead(void* p,uint32_t offset,uint32_t& v) {
    auto& s=*static_cast<Controller*>(p);return s.io_.backend.read32(s.io_.backend.context,offset,v);
}
Status Controller::read(Register reg,uint32_t& v,Result& r) {
    const uint8_t n=timing(reg)?before_.timingIndex:before_.opp;
    r.lastRegister=addresses_[n][size_t(reg)];r.lastValue=0;r.lastReadSucceeded=false;
    if(!backendValid(this)||!io_.canAccess(io_.backend.context,reg,n,r.lastRegister,false)||!backendValid(this))return Status::AccessDenied;
    ++r.reads;
    if(!io_.backend.read32(io_.backend.context,r.lastRegister,v))return Status::ReadFailed;
    r.lastValue=v;r.lastReadSucceeded=true;
    if(!backendValid(this)||!io_.canAccess(io_.backend.context,reg,n,r.lastRegister,false)||!backendValid(this))return Status::AccessDenied;
    return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::guard(Result& r) {
    // Recheck clocks before touching OPP shadow registers, including after a
    // sleep or prior write. No register access enters a HUBP/DSC power window.
    const Register first[]={Register::TimingControl,Register::TimingClock,Register::Clock};
    for(auto reg:first){uint32_t v=0;const auto s=read(reg,v,r);if(s!=Status::Ok)return s;
        if(v!=expected_[size_t(reg)])return Status::Changed;}
    for(size_t i=0;i<kRegisters;++i) {
        const auto reg=Register(i);if(reg==Register::Pending||reg==Register::Clock||reg==Register::TimingClock||reg==Register::TimingControl)continue;
        uint32_t v=0;const auto s=read(reg,v,r);if(s!=Status::Ok)return s;
        if(v!=expected_[i])return Status::Changed;
    }
    return Status::Ok;
}
Status Controller::write(Register reg,uint32_t value,Result& r) {
    auto s=guard(r);if(s!=Status::Ok)return s;
    const uint8_t n=before_.opp;const auto a=addresses_[n][size_t(reg)];
    if(!writable(reg)||!backendValid(this)||!io_.canAccess(io_.backend.context,reg,n,a,true)||!backendValid(this))return Status::AccessDenied;
    r.lastRegister=a;r.lastValue=value;r.lastReadSucceeded=false;
    // Journal before the transport can deliver the write, including failures.
    touched_=true;++r.writes;
    if(!io_.write32(io_.backend.context,a,value))return Status::WriteFailed;
    if(!backendValid(this)||!io_.canAccess(io_.backend.context,reg,n,a,true)||!backendValid(this))return Status::AccessDenied;
    uint32_t actual=0;s=read(reg,actual,r);if(s!=Status::Ok)return s;
    if(actual!=value)return Status::ReadbackMismatch;
    expected_[size_t(reg)]=value;return Status::Ok;
}
Status Controller::blank(const uint8_t* disc,size_t discBytes,uint64_t bar,
    const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io& io,const Poll& poll,Result& r) {
    r={};r.opp=r.timing=0xff;
    if(phase_!=Phase::Fresh)return finish(Status::InvalidState,r);
    if(!disc||!rom||!io.backend.valid||!io.backend.canRead||!io.backend.read32||!io.canAccess||!io.write32||!io.delayUs||
        !poll.iterations||!poll.intervalUs||uint64_t(poll.iterations)*poll.intervalUs>100000)return finish(Status::InvalidArgument,r);
    phase_=Phase::Blanking;io_=io;DiscoveryInfo info{};Dmu d{};
    if(parseDiscovery(disc,discBytes,info,collect,&d)!=DiscoveryStatus::Ok||d.count!=1||!d.valid||
        info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return finish(Status::DiscoveryRejected,r);
    if(bar<4||d.base>UINT32_MAX/4)return finish(Status::OutsideBar,r);
    for(size_t n=0;n<4;++n)for(size_t i=0;i<kRegisters;++i) {
        const auto a=(d.base+offsets[i]+n*(timing(Register(i))?0x80u:0x5au))*4;
        if(a>UINT32_MAX||a>bar-4)return finish(Status::OutsideBar,r);addresses_[n][i]=uint32_t(a);
    }
    const bootbackend::Io capturedIo={this,backendValid,backendCanRead,backendRead};
    if(bootbackend::capture(disc,discBytes,bar,rom,romBytes,imageOffset,capturedIo,before_,r.backend)!=bootbackend::Status::Ok)
        return finish(Status::CaptureRejected,r);
    r.opp=before_.opp;r.timing=before_.timingIndex;
    const Register first[]={Register::TimingControl,Register::TimingClock,Register::Clock};
    for(auto reg:first){const auto s=read(reg,expected_[size_t(reg)],r);if(s!=Status::Ok)return finish(s,r);}
    if(expected_[size_t(Register::TimingControl)]!=before_.timing[r.timing][size_t(bootbackend::TimingRegister::Control)]||
        expected_[size_t(Register::TimingClock)]!=before_.timing[r.timing][size_t(bootbackend::TimingRegister::Clock)]||
        expected_[size_t(Register::Clock)]!=3)return finish(Status::UnsupportedState,r);
    for(size_t i=0;i<kRegisters;++i){const auto reg=Register(i);
        if(reg==Register::Clock||reg==Register::TimingControl||reg==Register::TimingClock)continue;
        const auto s=read(reg,expected_[i],r);if(s!=Status::Ok)return finish(s,r);}
    // Admit an ordinary video source, not somebody else's pending/test pattern.
    const auto control=expected_[size_t(Register::Control)],fmt=expected_[size_t(Register::Format)];
    if((control&~0x01ff3171u)||(control&1u)||expected_[size_t(Register::Pending)]||
        (fmt&0x013f0001u))return finish(Status::UnsupportedState,r);
    const uint32_t h=expected_[size_t(Register::Horizontal)],v=expected_[size_t(Register::Vertical)];
    if((h|v)&0x80008000u||(h&0x7fff)<=(h>>16)||(v&0x7fff)<=(v>>16))return finish(Status::UnsupportedState,r);
    r.width=(h&0x7fff)-(h>>16);r.height=(v&0x7fff)-(v>>16);
    if(r.width>0x3fff||r.height>0x3fff)return finish(Status::UnsupportedState,r);
    auto s=guard(r);if(s!=Status::Ok)return finish(s,r);
    const auto size=(r.width<<16)|r.height;
    // The sourced helper programs dimensions/offset before the solid-color
    // branch, writes both RGB color banks, enables horizontal bars, then writes
    // dimensions again. Equal colors make the pattern uniformly black.
    const Register sequence[]={Register::Dimensions,Register::Offset,Register::Blue,
        Register::Green,Register::Red,Register::Control,Register::Dimensions};
    const uint32_t values[]={size,0,0,0,0,(control&~0x71u)|0x41u,size};
    for(size_t i=0;i<sizeof(sequence)/sizeof(sequence[0]);++i){s=write(sequence[i],values[i],r);if(s!=Status::Ok)return finish(s,r);}
    bool settled=false;
    for(uint32_t i=0;i<poll.iterations;++i) {
        s=guard(r);if(s!=Status::Ok)return finish(s,r);uint32_t pending=0;
        s=read(Register::Pending,pending,r);if(s!=Status::Ok)return finish(s,r);++r.polls;
        if(pending&~1u)return finish(Status::UnsupportedState,r);
        if(!pending){settled=true;break;}
        if(i+1<poll.iterations){if(!backendValid(this))return finish(Status::AccessDenied,r);
            io.delayUs(io.backend.context,poll.intervalUs);if(!backendValid(this))return finish(Status::AccessDenied,r);}
    }
    if(!settled)return finish(Status::Timeout,r);
    if(bootbackend::capture(disc,discBytes,bar,rom,romBytes,imageOffset,capturedIo,after_,r.backend)!=bootbackend::Status::Ok)
        return finish(Status::CaptureRejected,r);
    if(!sameBackend(before_,after_))return finish(Status::Changed,r);
    s=guard(r);if(s!=Status::Ok)return finish(s,r);
    uint32_t pending=0;s=read(Register::Pending,pending,r);if(s!=Status::Ok)return finish(s,r);
    if(pending)return finish(Status::Changed,r);
    phase_=Phase::Blanked;return finish(Status::Ok,r);
}
const char* statusString(Status s) {
#define S(x) case Status::x:return #x
    switch(s){S(Ok);S(InvalidArgument);S(InvalidState);S(DiscoveryRejected);S(OutsideBar);
        S(CaptureRejected);S(AccessDenied);S(ReadFailed);S(WriteFailed);S(DeviceUnavailable);
        S(UnsupportedState);S(Changed);S(ReadbackMismatch);S(Timeout);}return "Unknown";
#undef S
}
} }
