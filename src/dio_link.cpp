// SPDX-License-Identifier: MIT
/*
 * DCN3.2 DIO sequence adapted from Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
 * display/dc/dio/dcn10/dcn10_link_encoder.c,
 * dcn31/dcn31_dio_link_encoder.c, dcn32/dcn32_dio_link_encoder.c,
 * display/dc/resource/dcn32/dcn32_resource.c, dc/inc/hw/link_encoder.h,
 * dcn32/dcn32_dio_stream_encoder.c, dcn10/dcn10_stream_encoder.c,
 * and generated dcn_3_2_0_offset.h/sh_mask.h.
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
#include "hackynavi/dio_link.hpp"
namespace hackynavi { namespace dio {
namespace {
const uint32_t offsets[kRegisterCount]={0x20b1,0x20b2,0x20df,0x1f04,
    0x210b,0x211e,0x2118,0x2108,0x2117,0x211d,0x210f,0x2113,0x210c,
    0x208b,0x2091,0x2094,0x2098,0x209e};
struct Dmu { unsigned count;uint64_t base;bool valid; };
void collect(const DiscoveryIp& ip,void* ctx) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(ctx);++d.count;
    d.valid=!ip.dieId && !ip.instance && ip.major==3 && ip.minor==2 && !ip.revision &&
        !ip.subRevision && !ip.variant && !ip.harvest && ip.baseAddressCount>=3 && ip.baseAddresses[2]<=UINT32_MAX;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool populate(uint64_t base,uint64_t bytes,Layout& out) {
    out=Layout{};if(bytes<4 || base>UINT32_MAX/4)return false;
    for(size_t l=0;l<kLinkCount;++l)for(size_t i=0;i<kRegisterCount;++i) {
        const uint64_t address=(base+offsets[i]+l*(i==size_t(Register::PhyMux)?1:0x100))*4;
        if(address>UINT32_MAX || address>bytes-4)return false;
        out.registers[l][i]=uint32_t(address);
    }
    out.valid=true;out.barBytes=bytes;return true;
}
bool validLayout(const Layout& l) {
    if(!l.valid || (l.registers[0][0]&3) || l.registers[0][0]/4<offsets[0])return false;
    Layout expected{};if(!populate(l.registers[0][0]/4-offsets[0],l.barBytes,expected))return false;
    for(size_t n=0;n<kLinkCount;++n)for(size_t i=0;i<kRegisterCount;++i)if(l.registers[n][i]!=expected.registers[n][i])return false;
    return true;
}
bool validRoute(const dmcub::Route& r) {
    const unsigned id=r.encoderObject&255,n=(r.encoderObject>>8)&15;
    if((r.encoderObject&0xf000)!=0x2000 || n<1 || n>2 || (id!=0x1e && id!=0x20 && id!=0x21))return false;
    const unsigned phy=(id==0x1e?0:id==0x20?2:4)+n-1;
    return phy<5 && phy==r.phy && r.ddc<5 && r.hpd<5 &&
        (r.connectorObject&0xf000)==0x3000 && ((r.connectorObject>>8)&15)>=1 && ((r.connectorObject>>8)&15)<=6 &&
        ((r.connectorObject&255)==0x13 || (r.connectorObject&255)==0x0c);
}
bool hdmi(const dmcub::Route& r) {return (r.connectorObject&255)==0x0c;}
bool pollValid(const dmcub::PollOptions& p) {return p.maxPolls && p.maxPolls<=2000000 && p.delayUs && p.delayUs<=100 && uint64_t(p.maxPolls)*p.delayUs<=2000000;}
bool validClock(bool h,uint32_t c) {return h?c>=10 && c<=600000:c==162000 || c==270000 || c==540000 || c==810000;}
}
Status buildLayout(const uint8_t* b,size_t n,uint64_t bar,Layout& out) {
    out=Layout{};DiscoveryInfo info{};Dmu d{};
    if(parseDiscovery(b,n,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1 || !d.valid || info.baseAddresses64Bit || info.dieCount!=1 || info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.base,bar,out)?Status::Ok:Status::OutsideBar;
}
Status hdmiCharacterClock(const HdmiConfig& cfg,uint32_t& clock) {
    clock=0;
    if(cfg.pixelClockKHz<10 || cfg.pixelClockKHz>600000 ||
        (cfg.bitsPerComponent!=8 && cfg.bitsPerComponent!=10 &&
         cfg.bitsPerComponent!=12 && cfg.bitsPerComponent!=16))return Status::InvalidArgument;
    const uint64_t computed=(uint64_t(cfg.pixelClockKHz)*cfg.bitsPerComponent+7)/8;
    if(computed>600000 || (computed>340000 && !cfg.scramble))return Status::InvalidArgument;
    clock=uint32_t(computed);return Status::Ok;
}
Link::Link():phase_(Phase::Closed),io_{},layout_{},route_{},frontend_(0),lanes_(0),clock_(0),hdmiControl_(0),hdmiConfigured_(false) {}
void Link::clearAfterVerifiedReset() {phase_=Phase::Closed;io_=Io{};layout_=Layout{};route_=dmcub::Route{};frontend_=lanes_=0;clock_=hdmiControl_=0;hdmiConfigured_=false;}
Status Link::fail(Status s) {phase_=Phase::Faulted;return s;}
Status Link::read(uint8_t n,Register reg,uint32_t& value,Result& r) {
    ++r.reads;r.lastRegister=layout_.registers[n][size_t(reg)];
    if(!io_.read32(io_.context,r.lastRegister,value))return fail(Status::TransportError);
    r.lastValue=value;if(value==UINT32_MAX)return fail(Status::DeviceUnavailable);return Status::Ok;
}
Status Link::write(Register reg,uint32_t value,Result& r) {
    ++r.writes;r.lastRegister=layout_.registers[route_.phy][size_t(reg)];r.lastValue=value;
    if(!io_.write32(io_.context,r.lastRegister,value))return fail(Status::TransportError);
    uint32_t observed=0;const auto s=read(route_.phy,reg,observed,r);if(s!=Status::Ok)return s;
    return observed==value?Status::Ok:fail(Status::VerifyFailed);
}
Status Link::update(Register reg,uint32_t mask,uint32_t value,Result& r) {
    return updateAt(route_.phy,reg,mask,value,r);
}
Status Link::updateAt(uint8_t n,Register reg,uint32_t mask,uint32_t value,Result& r) {
    uint32_t old=0;auto s=read(n,reg,old,r);if(s!=Status::Ok)return s;
    ++r.writes;r.lastRegister=layout_.registers[n][size_t(reg)];r.lastValue=(old&~mask)|(value&mask);
    if(!io_.write32(io_.context,r.lastRegister,r.lastValue))return fail(Status::TransportError);
    s=read(n,reg,old,r);if(s!=Status::Ok)return s;
    return (old&mask)==(value&mask)?Status::Ok:fail(Status::VerifyFailed);
}
Status Link::guard(bool enabled,Result& r) {
    uint32_t value=0;auto s=read(route_.phy,Register::BackendEnable,value,r);if(s!=Status::Ok)return s;
    r.phyEnableObserved=(value&1)!=0;
    if(bool(value&1)!=enabled)return fail(Status::ActiveOutput);
    s=read(frontend_,Register::StreamControl,value,r);if(s!=Status::Ok)return s;
    if(value&0x10001)return fail(Status::ActiveOutput);
    if(hdmi(route_)) {
        s=read(frontend_,Register::FifoControl,value,r);if(s!=Status::Ok)return s;
        if(value&1)return fail(Status::ActiveOutput);
        if(hdmiConfigured_) {
            s=read(frontend_,Register::HdmiControl,value,r);if(s!=Status::Ok)return s;
            if((value&0x3100001f)!=hdmiControl_)return fail(Status::ForeignRoute);
            s=read(frontend_,Register::FrontendControl,value,r);if(s!=Status::Ok)return s;
            if(value&0xd0000000)return fail(Status::ForeignRoute);
        }
    }
    s=read(route_.phy,Register::PhyMux,value,r);if(s!=Status::Ok)return s;
    if(value&3)return fail(Status::ForeignRoute);
    const uint32_t mask=0x70077f00,expected=(uint32_t(route_.hpd)<<28)|(hdmi(route_)?0x30000:0)|(0x100U<<frontend_);
    s=read(route_.phy,Register::BackendControl,value,r);if(s!=Status::Ok)return s;
    if((value&mask)!=expected)return fail(Status::ForeignRoute);
    for(uint8_t i=0;i<kLinkCount;++i)if(i!=route_.phy) {
        s=read(i,Register::BackendControl,value,r);if(s!=Status::Ok)return s;
        if(value&(0x100U<<frontend_))return fail(Status::ForeignRoute);
    }
    if(!hdmi(route_)) {
        s=read(route_.phy,Register::PhyControl,value,r);if(s!=Status::Ok)return s;
        if(value&0x30)return fail(Status::ActiveOutput); // inherited FEC enable/ready
        s=read(route_.phy,Register::DpConfig,value,r);if(s!=Status::Ok)return s;
        if((value&3)!=uint32_t(lanes_-1))return fail(Status::ForeignRoute);
    }
    return Status::Ok;
}
Status Link::prepare(const Layout& l,const Io& io,const dmcub::Route& route,uint8_t fe,uint8_t lanes,Result& r) {
    r=Result{};
    if(phase_!=Phase::Closed)return phase_==Phase::Faulted?Status::Faulted:Status::InvalidState;
    if(!validLayout(l) || !io.read32 || !io.write32 || !io.delayUs || !validRoute(route) || fe>=5 ||
        (hdmi(route)?lanes!=4:lanes!=1 && lanes!=2 && lanes!=4))return Status::InvalidArgument;
    layout_=l;io_=io;route_=route;frontend_=fe;lanes_=lanes;
    uint32_t value=0;auto s=read(route.phy,Register::BackendEnable,value,r);
    if(s!=Status::Ok)return s;
    if(value&1) {clearAfterVerifiedReset();return Status::ActiveOutput;}
    s=read(fe,Register::StreamControl,value,r);if(s!=Status::Ok)return s;
    if(value&0x10001) {clearAfterVerifiedReset();return Status::ActiveOutput;}
    if(hdmi(route)) {
        s=read(fe,Register::FifoControl,value,r);if(s!=Status::Ok)return s;
        if(value&1) {clearAfterVerifiedReset();return Status::ActiveOutput;}
    }
    s=read(route.phy,Register::PhyMux,value,r);if(s!=Status::Ok)return s;
    if(value&3) {clearAfterVerifiedReset();return Status::ForeignRoute;}
    if(!hdmi(route)) {
        s=read(route.phy,Register::PhyControl,value,r);if(s!=Status::Ok)return s;
        if(value&0x30) {clearAfterVerifiedReset();return Status::ActiveOutput;}
    }
    for(uint8_t i=0;i<kLinkCount;++i) {
        s=read(i,Register::BackendControl,value,r);if(s!=Status::Ok)return s;
        if(value&(i==route.phy?0x7f00:(0x100U<<fe))) {clearAfterVerifiedReset();return Status::ForeignRoute;}
    }
    // Ownership starts before the first potentially ambiguous MMIO mutation.
    phase_=Phase::Prepared;
    s=update(Register::TmdsControl,1,1,r);if(s!=Status::Ok)return s;
    s=update(Register::PhyMux,3,0,r);if(s!=Status::Ok)return s;
    s=update(Register::BackendControl,0x70000000,uint32_t(route.hpd)<<28,r);if(s!=Status::Ok)return s;
    s=update(Register::BackendControl,0x70000,hdmi(route)?0x30000:0,r);if(s!=Status::Ok)return s;
    s=update(Register::BackendControl,0x7f00,0x100U<<fe,r);if(s!=Status::Ok)return s;
    if(!hdmi(route)) {
        s=write(Register::DpConfig,lanes-1,r);if(s!=Status::Ok)return s;
        s=update(Register::Scrambler,0x10,0x10,r);if(s!=Status::Ok)return s;
    }
    return guard(false,r);
}
Status Link::pollEnable(bool enabled,const dmcub::PollOptions& p,Result& r) {
    for(uint32_t i=0;i<p.maxPolls;++i) {
        uint32_t value=0;++r.polls;auto s=read(route_.phy,Register::BackendEnable,value,r);if(s!=Status::Ok)return s;
        r.phyEnableObserved=(value&1)!=0;
        if(bool(value&1)==enabled)return Status::Ok;
        if(i+1<p.maxPolls)io_.delayUs(io_.context,p.delayUs);
    }
    return fail(Status::EnableTimeout);
}
Status Link::enable(dmcub::Inbox& inbox,uint32_t clock,const dmcub::PollOptions& p,Result& r) {
    r=Result{};
    if(phase_!=Phase::Prepared)return phase_==Phase::Faulted?Status::Faulted:Status::InvalidState;
    if(!inbox.isOpen() || !pollValid(p) || !validClock(hdmi(route_),clock))return Status::InvalidArgument;
    if(hdmi(route_) && (!hdmiConfigured_ || clock!=clock_))return Status::InvalidState;
    auto s=guard(false,r);if(s!=Status::Ok)return s;
    dmcub::Result fr{};r.firmwareStatus=inbox.transmitter(route_,{dmcub::Action::Enable,lanes_,clock},p,fr);
    r.phyCommandSubmitted=fr.submitted;r.firmwareCommands=fr.submitted?1:0;
    if(r.firmwareStatus!=dmcub::Status::Ok)return fail(Status::FirmwareFailed);
    s=pollEnable(true,p,r);if(s!=Status::Ok)return s;
    s=guard(true,r);if(s!=Status::Ok)return s;
    clock_=clock;phase_=Phase::Enabled;return Status::Ok;
}
Status Link::configureHdmi(dmcub::Inbox& inbox,const HdmiConfig& cfg,const dmcub::PollOptions& p,Result& r) {
    r=Result{};
    if(phase_!=Phase::Prepared)return phase_==Phase::Faulted?Status::Faulted:Status::InvalidState;
    uint32_t clock=0;
    if(!hdmi(route_) || !inbox.isOpen() || !pollValid(p) || hdmiCharacterClock(cfg,clock)!=Status::Ok)return Status::InvalidArgument;
    if(hdmiConfigured_)return Status::InvalidState;
    auto s=guard(false,r);if(s!=Status::Ok)return s;
    dmcub::Result fr{};
    r.firmwareStatus=inbox.setupEncoder(route_,{frontend_,4,cfg.bitsPerComponent,cfg.pixelClockKHz},p,fr);
    r.firmwareCommands=fr.submitted?1:0;
    if(r.firmwareStatus!=dmcub::Status::Ok)return fail(Status::FirmwareFailed);
    // A consumed firmware command is not proof that the output is idle.
    s=guard(false,r);if(s!=Status::Ok)return s;
    s=updateAt(frontend_,Register::FrontendControl,0xd0000000,0,r);if(s!=Status::Ok)return s;
    const uint32_t depth=cfg.bitsPerComponent==8?0:cfg.bitsPerComponent==10?1:cfg.bitsPerComponent==12?2:3;
    // Linux's source >=340000 and sink >340000 disagree at the boundary.
    // Follow the sink's >340000 rule at both ends, avoiding a 4:1 mismatch.
    const uint32_t control=0x19|(depth<<28)|(depth?0x1000000:0)|
        (cfg.scramble?2:0)|(clock>340000?4:0);
    s=updateAt(frontend_,Register::HdmiControl,0x3100001f,control,r);if(s!=Status::Ok)return s;
    s=updateAt(frontend_,Register::HdmiGeneralControl,1,1,r);if(s!=Status::Ok)return s;
    // GC carries deep-color packing; audio/AVI packets remain the stream owner's.
    s=updateAt(frontend_,Register::HdmiVbiControl,0x1031,0x31,r);if(s!=Status::Ok)return s;
    s=guard(false,r);if(s!=Status::Ok)return s;
    clock_=clock;hdmiControl_=control;hdmiConfigured_=true;return Status::Ok;
}
Status Link::trainingPattern(Pattern pattern,Result& r) {
    r=Result{};
    if(phase_==Phase::Faulted)return Status::Faulted;
    if(phase_!=Phase::Enabled && phase_!=Phase::TrainingPattern && phase_!=Phase::VideoPattern)return Status::InvalidState;
    if(hdmi(route_) || uint8_t(pattern)>3)return Status::InvalidArgument;
    auto s=guard(true,r);if(s!=Status::Ok)return s;
    s=write(Register::TrainingPattern,uint8_t(pattern),r);if(s!=Status::Ok)return s;
    s=update(Register::LinkControl,0x10,0,r);if(s!=Status::Ok)return s;
    s=update(Register::PhyControl,0x10000,0,r);if(s!=Status::Ok)return s;
    s=update(Register::PrbsControl,1,0,r);if(s!=Status::Ok)return s;
    phase_=Phase::TrainingPattern;return Status::Ok;
}
Status Link::drive(dmcub::Inbox& inbox,uint8_t swing,uint8_t pre,uint8_t post,const dmcub::PollOptions& p,Result& r) {
    r=Result{};
    if(phase_==Phase::Faulted)return Status::Faulted;
    if(phase_!=Phase::Enabled && phase_!=Phase::TrainingPattern)return Status::InvalidState;
    if(hdmi(route_) || !inbox.isOpen() || !pollValid(p) || swing>3 || pre>3 || post>3 || swing+pre>3 || (post && clock_!=540000))return Status::InvalidArgument;
    auto s=guard(true,r);if(s!=Status::Ok)return s;
    for(uint8_t lane=0;lane<lanes_;++lane) {
        dmcub::Result fr{};r.firmwareStatus=inbox.dpDrive(route_,{lanes_,swing,pre,post,clock_},p,fr);
        if(fr.submitted) {++r.firmwareCommands;r.phyCommandSubmitted=true;}
        if(r.firmwareStatus!=dmcub::Status::Ok)return fail(Status::FirmwareFailed);
    }
    return guard(true,r);
}
Status Link::videoPattern(Result& r) {
    r=Result{};
    if(phase_==Phase::Faulted)return Status::Faulted;
    if(phase_!=Phase::TrainingPattern)return Status::InvalidState;
    if(hdmi(route_))return Status::InvalidArgument;
    auto s=guard(true,r);if(s!=Status::Ok)return s;
    s=write(Register::PanelControl,0,r);if(s!=Status::Ok)return s;
    s=update(Register::FramingControl,0x1103ffff,0x10002000,r);if(s!=Status::Ok)return s;
    s=update(Register::Scrambler,0x3ff00,0x1ff00,r);if(s!=Status::Ok)return s;
    s=update(Register::LinkControl,0x10,0x10,r);if(s!=Status::Ok)return s;
    s=update(Register::PhyControl,0x10000,0,r);if(s!=Status::Ok)return s;
    s=update(Register::PrbsControl,1,0,r);if(s!=Status::Ok)return s;
    phase_=Phase::VideoPattern;return Status::Ok;
}
Status Link::disable(dmcub::Inbox& inbox,const dmcub::PollOptions& p,Result& r) {
    r=Result{};
    if(phase_==Phase::Closed || phase_==Phase::Faulted)return phase_==Phase::Faulted?Status::Faulted:Status::InvalidState;
    if(!inbox.isOpen() || !pollValid(p))return Status::InvalidArgument;
    const bool enabled=phase_!=Phase::Prepared;auto s=guard(enabled,r);if(s!=Status::Ok)return s;
    if(enabled) {
        dmcub::Result fr{};r.firmwareStatus=inbox.transmitter(route_,{dmcub::Action::Disable,0,0},p,fr);
        r.phyCommandSubmitted=fr.submitted;r.firmwareCommands=fr.submitted?1:0;
        if(r.firmwareStatus!=dmcub::Status::Ok)return fail(Status::FirmwareFailed);
        s=pollEnable(false,p,r);if(s!=Status::Ok)return s;
        s=guard(false,r);if(s!=Status::Ok)return s;
    }
    if(!hdmi(route_)) {
        s=write(Register::TrainingPattern,0,r);if(s!=Status::Ok)return s;
        s=update(Register::LinkControl,0x10,0,r);if(s!=Status::Ok)return s;
        s=write(Register::PanelControl,0,r);if(s!=Status::Ok)return s;
    } else if(hdmiConfigured_) {
        s=updateAt(frontend_,Register::HdmiGeneralControl,1,1,r);if(s!=Status::Ok)return s;
        s=updateAt(frontend_,Register::HdmiVbiControl,0x1031,0,r);if(s!=Status::Ok)return s;
        s=updateAt(frontend_,Register::HdmiControl,0x3100001f,0x19,r);if(s!=Status::Ok)return s;
    }
    s=update(Register::BackendControl,0x100U<<frontend_,0,r);if(s!=Status::Ok)return s;
    clearAfterVerifiedReset();return Status::Ok;
}
const char* statusString(Status s) {
    switch(s) {
#define S(x) case Status::x:return #x
    S(Ok);S(InvalidArgument);S(DiscoveryRejected);S(UnsupportedDmu);S(OutsideBar);
    S(InvalidState);S(ActiveOutput);S(ForeignRoute);S(TransportError);S(DeviceUnavailable);
    S(VerifyFailed);S(FirmwareFailed);S(EnableTimeout);S(Faulted);
#undef S
    }return "Unknown";
}
} }
