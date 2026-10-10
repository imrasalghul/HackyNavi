// SPDX-License-Identifier: MIT
/*
 * Copyright 2020-2022 Advanced Micro Devices, Inc.
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
// Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// dcn30_hpo_frl_link_encoder.c/.h, dcn32_resource.c, generated
// dcn_3_2_0_offset.h/_sh_mask.h. DCN3.2 only; no IPSv2 memory-power recipe.
#include "hackynavi/hpo_frl.hpp"
namespace hackynavi { namespace hpofrl { namespace {
const uint32_t offsets[kRegisters]={0x95b,0x95c,0x965,0x966,0x968,0x8d3,0x8d6,0x8df};
struct Dmu{unsigned count;bool valid;uint64_t base;};
void collect(const DiscoveryIp& ip,void* ctx){if(ip.hardwareId!=271)return;auto& d=*static_cast<Dmu*>(ctx);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&!ip.subRevision&&
        !ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid)d.base=ip.baseAddresses[3];}
bool populate(uint64_t base,uint64_t bar,Layout& out){out={};if(bar<4||base>UINT32_MAX/4)return false;
    Layout l={};l.barBytes=bar;
    for(size_t n=0;n<kRegisters;++n){const auto a=(base+offsets[n])*4;
        if(a>UINT32_MAX||a>bar-4)return false;l.registers[n]=uint32_t(a);
        for(size_t j=0;j<n;++j)if(l.registers[j]==a)return false;}
    l.valid=true;out=l;return true;}
bool valid(const Layout& l){if(!l.valid||(l.registers[0]&3)||l.registers[0]/4<offsets[0])return false;
    Layout expected={};if(!populate(l.registers[0]/4-offsets[0],l.barBytes,expected))return false;
    for(size_t n=0;n<kRegisters;++n)if(expected.registers[n]!=l.registers[n])return false;return true;}
bool valid(const Poll& p){return p.maxPolls&&p.maxPolls<=100000&&p.delayUs&&p.delayUs<=1000&&
    uint64_t(p.maxPolls)*p.delayUs<=100000;}
}
Status buildLayout(const uint8_t* data,size_t bytes,uint64_t bar,Layout& out){out={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(data,bytes,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.base,bar,out)?Status::Ok:Status::OutsideBar;}
Link::Link():layout_{},io_{},phase_(Phase::Closed),lanes_(0),touched_(false){}
void Link::clearAfterVerifiedReset(){layout_={};io_={};phase_=Phase::Closed;lanes_=0;touched_=false;}
Status Link::finish(Status s,Result& r){if(s!=Status::Ok&&touched_)phase_=Phase::Faulted;
    r.status=s;r.retentionRequired=touched_;return s;}
Status Link::read(Register reg,uint32_t& v,Result& r){if(!io_.valid(io_.context))return Status::Revoked;
    r.lastRegister=layout_.registers[size_t(reg)];++r.reads;
    if(!io_.read32(io_.context,r.lastRegister,v))return Status::TransportError;
    r.lastValue=v;if(!io_.valid(io_.context))return Status::Revoked;
    return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;}
Status Link::update(Register reg,uint32_t mask,uint32_t value,Result& r){uint32_t v=0;auto s=read(reg,v,r);if(s!=Status::Ok)return s;
    v=(v&~mask)|(value&mask);
    // CONFIG2 has a reset strobe and sticky status fields. Never replay them
    // while modifying RC_COMPRESS_DISABLE; preserve unrelated/reserved bits.
    if(reg==Register::Config2)v&=~0x70000000u;
    if(!io_.valid(io_.context))return Status::Revoked;
    r.lastRegister=layout_.registers[size_t(reg)];r.lastValue=v;++r.writes;touched_=r.writeAttempted=true;
    if(!io_.write32(io_.context,r.lastRegister,v))return Status::TransportError;
    s=read(reg,v,r);if(s!=Status::Ok)return s;return (v&mask)==(value&mask)?Status::Ok:Status::VerifyFailed;}
Status Link::streamIdle(Result& r){uint32_t v=0;auto s=read(Register::StreamClock,v,r);if(s!=Status::Ok)return s;
    if(!(v&1))return Status::Ok; // Native owner separately proves gated stream retirement.
    if((v&0x1110)!=0x1110)return Status::ForeignControl;
    s=read(Register::StreamFifo,v,r);if(s!=Status::Ok)return s;
    if(v&0x31100011)return Status::StreamActive; // enable/reset/reset-done/video-active/error
    s=read(Register::Tribyte,v,r);if(s!=Status::Ok)return s;
    return (v&0x111)?Status::StreamActive:Status::Ok;}
Status Link::clockWait(bool on,const Poll& p,Result& r){
    for(uint32_t n=0;n<p.maxPolls;++n){uint32_t v=0;auto s=read(Register::Clock,v,r);if(s!=Status::Ok)return s;
        ++r.polls;if((v&2)==(on?2u:0u))return Status::Ok;
        if(n+1<p.maxPolls){if(!io_.valid(io_.context))return Status::Revoked;io_.delayUs(io_.context,p.delayUs);}}
    return Status::ClockTimeout;}
Status Link::guard(Result& r){auto s=streamIdle(r);if(s!=Status::Ok)return s;
    uint32_t v=0;s=read(Register::Memory,v,r);if(s!=Status::Ok)return s;
    if(v&0x36)return Status::MemoryAsleep;
    s=read(Register::Clock,v,r);if(s!=Status::Ok)return s;if((v&3)!=3)return Status::ForeignControl;
    s=read(Register::Control,v,r);if(s!=Status::Ok)return s;if((v&0x11)!=1)return Status::ForeignControl;
    s=read(Register::Config,v,r);if(s!=Status::Ok)return s;
    const uint32_t expected=(lanes_==4?1u:0u)|(phase_==Phase::Training?2u:0u);
    if((v&7)!=expected)return Status::ForeignControl;
    return Status::Ok;}
Status Link::prepare(const Layout& l,const Io& io,uint8_t lanes,const Poll& p,Result& r){r={};
    if(phase_!=Phase::Closed||touched_)return finish(Status::InvalidState,r);
    if(!valid(l)||!valid(p)||(lanes!=3&&lanes!=4)||!io.valid||!io.read32||!io.write32||!io.delayUs)return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;lanes_=lanes;auto s=streamIdle(r);if(s!=Status::Ok)return finish(s,r);
    uint32_t v=0;s=read(Register::Memory,v,r);if(s!=Status::Ok)return finish(s,r);
    if(v&0x36)return finish(Status::MemoryAsleep,r);
    s=read(Register::Clock,v,r);if(s!=Status::Ok)return finish(s,r);
    if(v&3)return finish(Status::ForeignControl,r);
    s=read(Register::Control,v,r);if(s!=Status::Ok)return finish(s,r);
    if(v&0x11)return finish(Status::ForeignControl,r);
    s=update(Register::Clock,1,1,r);if(s!=Status::Ok)return finish(s,r);
    s=clockWait(true,p,r);if(s!=Status::Ok)return finish(s,r);
    // Enable scrambler and LTP1 on every lane before the link can emit.
    s=update(Register::Config,0xffff0007,(lanes==4?1u:0u)|2,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Control,0x11,0x10,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Control,0x10,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Control,1,1,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Training;return finish(Status::Ok,r);}
Status Link::patterns(const uint8_t values[4],Result& r){r={};if(phase_!=Phase::Training)return finish(Status::InvalidState,r);
    if(!values)return finish(Status::InvalidArgument,r);
    uint32_t mask=2,value=2;
    for(unsigned n=0;n<4;++n){if(values[n]!=255&&values[n]>7)return finish(Status::InvalidArgument,r);
        if(n>=lanes_&&values[n]!=255)return finish(Status::InvalidArgument,r);
        if(values[n]!=255){mask|=15u<<(16+4*n);value|=uint32_t(values[n])<<(16+4*n);}}
    auto s=guard(r);if(s==Status::Ok)s=update(Register::Config,mask,value,r);return finish(s,r);}
Status Link::packetMode(Result& r){r={};if(phase_!=Phase::Training)return finish(Status::InvalidState,r);
    auto s=guard(r);if(s==Status::Ok)s=update(Register::Config,2,0,r);
    if(s==Status::Ok)phase_=Phase::Packets;return finish(s,r);}
Status Link::disable(const Poll& p,Result& r){r={};if(!touched_){phase_=Phase::Closed;return finish(Status::Ok,r);}
    if(!valid(p))return finish(Status::InvalidArgument,r);auto s=streamIdle(r);if(s!=Status::Ok)return finish(s,r);
    uint32_t clock=0;s=read(Register::Clock,clock,r);if(s!=Status::Ok)return finish(s,r);
    // Configuration is not read while its link clock is off. A failed enable
    // may leave it off; CONTROL/CLOCK are the retained always-accessible controls.
    if((clock&3)==3){s=update(Register::Config,0xffff0002,2,r);if(s!=Status::Ok)return finish(s,r);
        s=update(Register::Config2,0x02000000,0,r);if(s!=Status::Ok)return finish(s,r);}
    s=update(Register::Control,0x11,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Clock,1,0,r);if(s!=Status::Ok)return finish(s,r);
    s=clockWait(false,p,r);if(s!=Status::Ok)return finish(s,r);
    touched_=false;phase_=Phase::Closed;return finish(Status::Ok,r);}
const char* statusString(Status s){switch(s){
#define S(x) case Status::x:return #x
    S(Ok);S(InvalidArgument);S(InvalidState);S(DiscoveryRejected);S(UnsupportedDmu);S(OutsideBar);S(Revoked);
    S(TransportError);S(DeviceUnavailable);S(ForeignControl);S(StreamActive);S(MemoryAsleep);S(VerifyFailed);
    S(ClockTimeout);S(Faulted);
#undef S
    }return "Unknown";}
} }
