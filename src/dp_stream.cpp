// SPDX-License-Identifier: MIT
/*
 * DCN3.2 sequence adapted from Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
 * dcn10_stream_encoder.c, dcn20_stream_encoder.c, dcn32_dio_stream_encoder.c,
 * stream_encoder.h and generated dcn_3_2_0_offset.h/sh_mask.h.
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
#include "hackynavi/dp_stream.hpp"
namespace hackynavi { namespace dpstream { namespace {
const uint16_t offsets[]={0x208b,0x2091,0x20b1,0x20b2,0x1f04,0x210b,0x2108,0x2117,
    0x2113,0x210f,0x211d,0x2109,0x210a,0x210e,0x214c,0x214d,0x214e,0x214f,
    0x2110,0x2111,0x2112,0x210c,0x210d,0x212b,0x2153,0x2130,0x215b,0x2161,0x2152,0x2159,0x215a,
    0x212c,0x215e,0x215f,0x2160,0x211e};
const uint16_t timingOffsets[]={0x1b41,0x1b85,0x1b4c,0x1b2a,0x1b2f,
    0x1b2b,0x1b2c,0x1b2d,0x1b36,0x1b37,0x1b38,0x1b44};
static_assert(sizeof(offsets)/sizeof(offsets[0])==kRegisterCount,"register order");
static_assert(sizeof(timingOffsets)/sizeof(timingOffsets[0])==kTimingRegisterCount,"timing register order");
struct Dmu { unsigned count;uint64_t base;bool valid; };
void collect(const DiscoveryIp& ip,void* context){
    if(ip.hardwareId!=271)return;auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&!ip.subRevision&&
        !ip.variant&&!ip.harvest&&ip.baseAddressCount>=3&&ip.baseAddresses[2]<=UINT32_MAX;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool populate(uint64_t base,uint64_t bar,Layout& l){
    l={};if(bar<4||base>UINT32_MAX/4)return false;l.barBytes=bar;
    for(size_t n=0;n<5;++n)for(size_t i=0;i<kRegisterCount;++i){
        const uint64_t a=(base+offsets[i]+n*(i==size_t(Register::PhyMux)?1:0x100))*4;
        if(a>UINT32_MAX||a>bar-4)return false;l.registers[n][i]=uint32_t(a);
    }
    for(size_t n=0;n<4;++n)for(size_t i=0;i<kTimingRegisterCount;++i){
        const uint64_t a=(base+timingOffsets[i]+n*0x80)*4;
        if(a>UINT32_MAX||a>bar-4)return false;l.timing[n][i]=uint32_t(a);
    }
    l.valid=true;return true;
}
bool validLayout(const Layout& l){
    if(!l.valid||l.registers[0][0]%4||l.registers[0][0]/4<offsets[0])return false;
    const uint64_t base=l.registers[0][0]/4-offsets[0];if(l.barBytes<4)return false;
    for(size_t n=0;n<5;++n)for(size_t i=0;i<kRegisterCount;++i){
        const uint64_t a=(base+offsets[i]+n*(i==size_t(Register::PhyMux)?1:0x100))*4;
        if(a>UINT32_MAX||a>l.barBytes-4||l.registers[n][i]!=a)return false;
    }
    for(size_t n=0;n<4;++n)for(size_t i=0;i<kTimingRegisterCount;++i){
        const uint64_t a=(base+timingOffsets[i]+n*0x80)*4;
        if(a>UINT32_MAX||a>l.barBytes-4||l.timing[n][i]!=a)return false;
    }
    return true;
}
bool validPoll(const Poll& p){return p.maxPolls&&p.maxPolls<=2000000&&p.delayUs&&p.delayUs<=100&&uint64_t(p.maxPolls)*p.delayUs<=2000000;}
struct Guard { Register reg;uint32_t mask,value; };
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t bar,Layout& l){
    l={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(bytes,count,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.base,bar,l)?Status::Ok:Status::OutsideBar;
}
Status encode(const Config& c,Values& v){
    v={};if(c.frontend>=5||c.backend>=5||c.pipe>=4||c.hpd>=5||(c.lanes!=1&&c.lanes!=2&&c.lanes!=4)||
        (c.symbolClockKHz!=162000&&c.symbolClockKHz!=270000&&c.symbolClockKHz!=540000&&c.symbolClockKHz!=810000))return Status::InvalidArgument;
    const auto& t=c.timing;
    if(!t.pixelClockKHz||!t.width||!t.height||t.width>=t.hSyncStart||t.hSyncStart>=t.hSyncEnd||t.hSyncEnd>t.hTotal||
        t.height>=t.vSyncStart||t.vSyncStart>=t.vSyncEnd||t.vSyncEnd>t.vTotal||t.hTotal>32768||t.vTotal>32768||
        t.hTotal-t.width<32||t.vTotal-t.height<3||t.hSyncEnd-t.hSyncStart<4)return Status::InvalidTiming;
    // 8 payload bits per lane-symbol; full pixel clock includes blanking.
    // This is a necessary RGB24 capacity bound, not DML/SDP/sink admission.
    if(uint64_t(t.pixelClockKHz)*24>uint64_t(c.symbolClockKHz)*c.lanes*8)return Status::LinkCapacity;
    v.timing[0]=(uint32_t(t.hTotal)<<16)|t.vTotal;
    v.timing[1]=(uint32_t(t.hTotal-t.hSyncStart)<<16)|(t.vTotal-t.vSyncStart);
    v.timing[2]=(uint32_t(t.hSyncEnd-t.hSyncStart)<<16)|(t.vSyncEnd-t.vSyncStart)|
        (t.hPositive?0:0x80000000u)|(t.vPositive?0:0x8000u);
    v.timing[3]=(uint32_t(t.width)<<16)|t.height;v.n=0x8000;
    v.m=uint32_t(uint64_t(v.n)*t.pixelClockKHz/c.symbolClockKHz);
    return v.m&&v.m<=0xffffff?Status::Ok:Status::InvalidTiming;
}
Controller::Controller():layout_{},io_{},config_{},values_{},phase_(Phase::Closed),touched_(false){}
void Controller::clearAfterVerifiedReset(){layout_={};io_={};config_={};values_={};phase_=Phase::Closed;touched_=false;}
Status Controller::finish(Status s,Result& r){r.status=s;if(s!=Status::Ok&&touched_)phase_=Phase::Faulted;return s;}
Status Controller::readAddress(uint32_t a,uint32_t& v,Result& r){
    if(!io_.valid(io_.context))return Status::DeviceUnavailable;
    ++r.reads;r.lastRegister=a;const bool readOk=io_.read32(io_.context,a,v);
    const bool current=io_.valid(io_.context);r.lastValue=v;
    if(!current)return Status::DeviceUnavailable;
    if(!readOk)return Status::TransportError;
    return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::read(uint8_t n,Register reg,uint32_t& v,Result& r){return readAddress(layout_.registers[n][size_t(reg)],v,r);}
Status Controller::update(Register reg,uint32_t mask,uint32_t v,Result& r){
    uint32_t old=0;auto s=read(config_.frontend,reg,old,r);if(s!=Status::Ok)return s;
    if(!io_.valid(io_.context))return Status::DeviceUnavailable;
    ++r.writes;r.writeAttempted=touched_=true;r.lastValue=(old&~mask)|(v&mask);
    const bool writeOk=io_.write32(io_.context,r.lastRegister,r.lastValue);
    if(!io_.valid(io_.context))return Status::DeviceUnavailable;
    if(!writeOk)return Status::TransportError;
    s=read(config_.frontend,reg,old,r);if(s!=Status::Ok)return s;
    return (old&mask)==(v&mask)?Status::Ok:Status::VerifyFailed;
}
Status Controller::delay(uint32_t us){if(!io_.valid(io_.context))return Status::DeviceUnavailable;io_.delayUs(io_.context,us);return io_.valid(io_.context)?Status::Ok:Status::DeviceUnavailable;}
Status Controller::wait(Register reg,uint32_t mask,uint32_t v,const Poll& p,Result& r){
    for(uint32_t n=0;n<p.maxPolls;++n){uint32_t got=0;auto s=read(config_.frontend,reg,got,r);if(s!=Status::Ok)return s;
        ++r.polls;if((got&mask)==v)return Status::Ok;if(n+1<p.maxPolls){s=delay(p.delayUs);if(s!=Status::Ok)return s;}}
    return Status::Timeout;
}
Status Controller::route(bool programmed,Result& r){
    uint32_t v=0;auto s=read(config_.backend,Register::BackendEnable,v,r);if(s!=Status::Ok)return s;if(!(v&1))return Status::ForeignControl;
    s=read(config_.backend,Register::PhyMux,v,r);if(s!=Status::Ok)return s;if(v&3)return Status::ForeignControl;
    for(uint8_t n=0;n<5;++n){s=read(n,Register::Backend,v,r);if(s!=Status::Ok)return s;
        if(n==config_.backend){if((v&0x70077f07)!=((uint32_t(config_.hpd)<<28)|(0x100u<<config_.frontend)))return Status::ForeignControl;}
        else if(v&(0x100u<<config_.frontend))return Status::ForeignControl;
    }
    const Guard backend[]={{Register::Config,3,uint32_t(config_.lanes-1)},{Register::Link,0x20010,0x10},
        {Register::Phy,0x101007f,0},{Register::Framing,0x1103ffff,0x10002000},{Register::Panel,UINT32_MAX,0},
        {Register::Prbs,1,0},{Register::Scrambler,0x103ff11,0x1ff10}};
    for(const auto& g:backend){s=read(config_.backend,g.reg,v,r);if(s!=Status::Ok)return s;if((v&g.mask)!=g.value)return Status::ForeignControl;}
    s=read(config_.frontend,Register::Frontend,v,r);if(s!=Status::Ok)return s;
    if(v&0xd017f000)return Status::ForeignControl;
    if(programmed&&(v&0x177)!=(uint32_t(config_.pipe)*0x11+0x100))return Status::ForeignControl;
    // A second active frontend cannot consume the selected timing source.
    for(uint8_t n=0;n<5;++n)if(n!=config_.frontend){uint32_t fe=0,fifo=0,stream=0;
        s=read(n,Register::Frontend,fe,r);if(s!=Status::Ok)return s;
        if((fe&7)!=config_.pipe)continue;
        s=read(n,Register::Fifo,fifo,r);if(s!=Status::Ok)return s;
        s=read(n,Register::Stream,stream,r);if(s!=Status::Ok)return s;
        if((fifo&1)||(stream&0x10001))return Status::ForeignControl;
    }
    return Status::Ok;
}
Status Controller::controls(Result& r){
    const Guard guards[]={{Register::Secondary,0x1ff11111,0},{Register::Secondary2,0x13333333,0},
        {Register::Secondary1,0x161,0},{Register::Gsp8,0x31df,0},{Register::Gsp9,0x31df,0},{Register::Gsp10,0x31df,0},
        {Register::Metadata,0xf1,0},{Register::Pps,0x31df,0},{Register::Dsc,1,0},
        {Register::DoubleBuffer,0x8101,0},{Register::Vbid,0x3010,0},
        {Register::VidTiming,0xff003010,0}};
    for(const auto& g:guards){uint32_t v=0;const auto s=read(config_.frontend,g.reg,v,r);if(s!=Status::Ok)return s;if((v&g.mask)!=g.value)return Status::ForeignControl;}
    return Status::Ok;
}
Status Controller::timing(Result& r){
    const auto& t=config_.timing;
    const uint32_t hb=t.hTotal-(t.hSyncStart-t.width),vb=t.vTotal-(t.vSyncStart-t.height);
    const uint32_t masks[]={0x313001,0x110,0,0x7fff,0x7fff,
        0x7fff7fff,0x7fff7fff,0x30001,0x7fff7fff,0x7fff7fff,0x101,0x30001};
    const uint32_t expected[]={0x11001,0x100,0,uint32_t(t.hTotal-1),uint32_t(t.vTotal-1),
        hb|((hb-t.width)<<16),uint32_t(t.hSyncEnd-t.hSyncStart)<<16,t.hPositive?0u:1u,
        vb|((vb-t.height)<<16),uint32_t(t.vSyncEnd-t.vSyncStart)<<16,t.vPositive?0u:1u,0};
    for(size_t n=0;n<kTimingRegisterCount;++n)if(masks[n]){uint32_t v=0;auto s=readAddress(layout_.timing[config_.pipe][n],v,r);
        if(s!=Status::Ok)return s;if((v&masks[n])!=expected[n])return Status::ForeignControl;}
    return Status::Ok;
}
Status Controller::verifyProgrammed(Result& r){
    const Guard g[]={{Register::Pixel,0x47000007,0x1000000},{Register::Color,0xff000000,0x20000000},
        {Register::Misc,0xff,0},{Register::SecondaryFraming,1,0},{Register::Fifo,0x180,0},
        {Register::Timing1,UINT32_MAX,values_.timing[0]},{Register::Timing2,UINT32_MAX,values_.timing[1]},
        {Register::Timing3,UINT32_MAX,values_.timing[2]},{Register::Timing4,UINT32_MAX,values_.timing[3]}};
    for(const auto& e:g){uint32_t v=0;auto s=read(config_.frontend,e.reg,v,r);if(s!=Status::Ok)return s;if((v&e.mask)!=e.value)return Status::VerifyFailed;}
    return controls(r);
}
Status Controller::prepare(const Layout& l,const Io& io,const Config& c,Result& r){
    r={};if(phase_!=Phase::Closed&&phase_!=Phase::Stopped)return finish(Status::InvalidState,r);
    Values v={};auto s=encode(c,v);if(s!=Status::Ok)return finish(s,r);
    if(!validLayout(l)||!io.valid||!io.read32||!io.write32||!io.delayUs)return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;config_=c;values_=v;s=route(false,r);if(s!=Status::Ok)return finish(s,r);
    s=controls(r);if(s!=Status::Ok)return finish(s,r);
    const Guard guards[]={{Register::Stream,0x10001,0},{Register::Fifo,0x30100183,0},{Register::Steer,0x110,0}};
    for(const auto& g:guards){uint32_t got=0;s=read(config_.frontend,g.reg,got,r);if(s!=Status::Ok)return finish(s,r);if((got&g.mask)!=g.value)return finish(Status::ForeignControl,r);}
    const Guard program[]={{Register::Frontend,0x177,uint32_t(config_.pipe)*0x11+0x100},
        {Register::Pixel,0x47000007,0x1000000},{Register::Color,0xff000000,0x20000000},
        {Register::Misc,0xff,0},{Register::SecondaryFraming,1,0},{Register::Fifo,0x100,0},
        {Register::Timing1,UINT32_MAX,v.timing[0]},{Register::Timing2,UINT32_MAX,v.timing[1]},
        {Register::Timing3,UINT32_MAX,v.timing[2]},{Register::Timing4,UINT32_MAX,v.timing[3]}};
    for(const auto& g:program){s=update(g.reg,g.mask,g.value,r);if(s!=Status::Ok)return finish(s,r);}
    s=route(true,r);if(s!=Status::Ok)return finish(s,r);phase_=Phase::Prepared;return finish(Status::Ok,r);
}
Status Controller::start(const Poll& options,Result& r){
    const Poll p=options;
    r={};if(phase_!=Phase::Prepared)return finish(Status::InvalidState,r);if(!validPoll(p))return finish(Status::InvalidArgument,r);
    auto s=route(true,r);if(s!=Status::Ok)return finish(s,r);s=verifyProgrammed(r);if(s!=Status::Ok)return finish(s,r);
    s=timing(r);if(s!=Status::Ok)return finish(s,r);
    uint32_t got=0;s=read(config_.frontend,Register::Stream,got,r);if(s!=Status::Ok)return finish(s,r);
    if(got&0x10001)return finish(Status::ForeignControl,r);
    s=read(config_.frontend,Register::Fifo,got,r);if(s!=Status::Ok)return finish(s,r);
    if(got&0x30100183)return finish(Status::ForeignControl,r);
    const Guard mn[]={{Register::VidTiming,0x100,0},{Register::N,0xffffff,values_.n},
        {Register::M,0xffffff,values_.m},{Register::VidTiming,0xd00,0x100}};
    for(const auto& g:mn){s=update(g.reg,g.mask,g.value,r);if(s!=Status::Ok)return finish(s,r);}
    s=update(Register::Stream,1,0,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Stream,0x10000,0,p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Steer,1,1,r);if(s!=Status::Ok)return finish(s,r);s=delay(10);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Steer,1,0,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Frontend,0x1000000,0x1000000,p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,0x7c,7u<<2,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,2,2,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Fifo,0x100000,0x100000,p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,2,0,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Fifo,0x100000,0,p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,1,1,r);if(s!=Status::Ok)return finish(s,r);s=delay(100);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Stream,1,1,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Stream,0x10001,0x10001,p,r);if(s!=Status::Ok)return finish(s,r);r.streamEnabled=true;
    const uint32_t frame=layout_.timing[config_.pipe][size_t(TimingRegister::FrameCount)];
    s=readAddress(frame,r.firstFrame,r);if(s!=Status::Ok)return finish(s,r);r.firstFrame&=0xffffff;
    for(uint32_t n=0;n<p.maxPolls;++n){
        s=read(config_.frontend,Register::Fifo,got,r);if(s!=Status::Ok)return finish(s,r);if((got&0x30100183)!=1)return finish(Status::FifoError,r);
        s=read(config_.frontend,Register::Steer,got,r);if(s!=Status::Ok)return finish(s,r);if(got&0x111)return finish(Status::FifoError,r);
        s=readAddress(frame,r.lastFrame,r);if(s!=Status::Ok)return finish(s,r);r.lastFrame&=0xffffff;++r.polls;
        const uint32_t delta=(r.lastFrame-r.firstFrame)&0xffffff;if(delta>=2&&delta<0x800000){r.framesAdvanced=true;break;}
        if(n+1<p.maxPolls){s=delay(p.delayUs);if(s!=Status::Ok)return finish(s,r);}
    }
    if(!r.framesAdvanced)return finish(Status::Timeout,r);
    s=route(true,r);if(s!=Status::Ok)return finish(s,r);s=timing(r);if(s!=Status::Ok)return finish(s,r);
    s=verifyProgrammed(r);if(s!=Status::Ok)return finish(s,r);
    s=read(config_.frontend,Register::VidTiming,got,r);if(s!=Status::Ok)return finish(s,r);
    if((got&0xd00)!=0x100)return finish(Status::VerifyFailed,r);
    s=read(config_.frontend,Register::Stream,got,r);if(s!=Status::Ok)return finish(s,r);if((got&0x10001)!=0x10001)return finish(Status::VerifyFailed,r);
    phase_=Phase::Running;return finish(Status::Ok,r);
}
Status Controller::stop(const Poll& options,Result& r){
    const Poll p=options;
    r={};if(!touched_)return finish(Status::InvalidState,r);if(!validPoll(p))return finish(Status::InvalidArgument,r);
    // Do not require programmed attributes after a partial prepare, but do
    // require the same exclusive physical link before touching its frontend.
    auto s=route(false,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Stream,0x300,0x200,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Stream,1,0,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Stream,0x10001,0,p,r);if(s!=Status::Ok)return finish(s,r);r.streamStopped=true;
    s=update(Register::Steer,1,1,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,1,0,r);if(s!=Status::Ok)return finish(s,r);r.fifoDisabled=true;
    s=update(Register::VidTiming,0x100,0,r);if(s!=Status::Ok)return finish(s,r);
    // Leave reset asserted; restoring stale timing/packet state is not safe.
    uint32_t got=0;s=read(config_.frontend,Register::Stream,got,r);if(s!=Status::Ok)return finish(s,r);
    if(got&0x10001)return finish(Status::VerifyFailed,r);
    touched_=false;phase_=Phase::Stopped;return finish(Status::Ok,r);
}
const char* statusString(Status s){switch(s){
#define STATUS_CASE(x) case Status::x:return #x
    STATUS_CASE(Ok);STATUS_CASE(InvalidArgument);STATUS_CASE(InvalidTiming);STATUS_CASE(LinkCapacity);
    STATUS_CASE(DiscoveryRejected);STATUS_CASE(UnsupportedDmu);STATUS_CASE(OutsideBar);STATUS_CASE(InvalidState);
    STATUS_CASE(ForeignControl);STATUS_CASE(TransportError);STATUS_CASE(DeviceUnavailable);STATUS_CASE(VerifyFailed);
    STATUS_CASE(Timeout);STATUS_CASE(FifoError);STATUS_CASE(Faulted);
#undef STATUS_CASE
    }return "Unknown";}
} }
