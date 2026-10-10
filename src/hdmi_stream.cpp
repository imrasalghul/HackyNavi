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
#include "hackynavi/hdmi_stream.hpp"
namespace hackynavi { namespace hdmistream { namespace {
const uint32_t offsets[kRegisterCount]={0x208b,0x2091,0x2094,0x209e,0x209b,0x209c,0x209f,
    0x20a7,0x20b0,0x20b1,0x20b2,0x210c,0x2098,0x2099,0x2097};
bool valid(const vpg::Poll& p){return p.maxPolls&&p.maxPolls<=2000000&&p.delayUs&&p.delayUs<=100&&uint64_t(p.maxPolls)*p.delayUs<=2000000;}
bool addresses(Layout& l){
    if(!l.packets.valid||!l.timing.valid||l.barBytes<4)return false;
    const uint64_t base=l.packets.registers[0][0]/4-0x2068;
    for(size_t n=0;n<5;++n)for(size_t r=0;r<kRegisterCount;++r){
        const uint64_t a=(base+offsets[r]+n*0x100)*4;if(a>UINT32_MAX||a>l.barBytes-4)return false;
        l.registers[n][r]=uint32_t(a);
    }
    return true;
}
bool valid(const Layout& l){
    if(!l.valid||!l.packets.valid||!l.timing.valid||l.packets.barBytes!=l.barBytes||l.timing.barBytes!=l.barBytes||
        l.packets.registers[0][0]/4<0x2068||l.packets.registers[0][0]%4)return false;
    Layout expected=l;if(!addresses(expected))return false;
    for(size_t n=0;n<5;++n)for(size_t r=0;r<kRegisterCount;++r)if(l.registers[n][r]!=expected.registers[n][r])return false;
    const uint32_t base=l.packets.registers[0][0]/4-0x2068;
    for(size_t n=0;n<4;++n){
        if(l.timing.registers[n][size_t(otg::Register::Control)]!=(base+0x1b41+n*0x80)*4||
            l.timing.registers[n][size_t(otg::Register::Clock)]!=(base+0x1b85+n*0x80)*4||
            l.timing.registers[n][size_t(otg::Register::FrameCount)]!=(base+0x1b4c+n*0x80)*4)return false;
    }
    return true;
}
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t bar,Layout& l){
    l={};l.barBytes=bar;
    if(vpg::buildLayout(bytes,count,bar,l.packets)!=vpg::Status::Ok||otg::buildLayout(bytes,count,bar,l.timing)!=otg::Status::Ok)return Status::DiscoveryRejected;
    if(!addresses(l))return Status::OutsideBar;l.valid=true;return Status::Ok;
}
Controller::Controller():layout_{},io_{},config_{},packets_{},phase_(Phase::Closed),touched_(false),savedClock_(0),savedDb_(0){}
void Controller::clearAfterVerifiedReset(){packets_.clearAfterVerifiedReset();layout_={};io_={};config_={};phase_=Phase::Closed;touched_=false;savedClock_=savedDb_=0;}
Status Controller::finish(Status s,Result& r){r.status=s;if(s!=Status::Ok&&touched_)phase_=Phase::Faulted;return s;}
Status Controller::readAddress(uint32_t a,uint32_t& v,Result& r){
    ++r.reads;r.lastRegister=a;if(!io_.read32(io_.context,a,v))return Status::TransportError;
    r.lastValue=v;return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::read(Register reg,uint32_t& v,Result& r){return readAddress(layout_.registers[config_.frontend][size_t(reg)],v,r);}
Status Controller::update(Register reg,uint32_t mask,uint32_t v,Result& r){
    uint32_t old=0;auto s=read(reg,old,r);if(s!=Status::Ok)return s;
    ++r.writes;r.writeAttempted=touched_=true;r.lastValue=(old&~mask)|(v&mask);
    if(!io_.write32(io_.context,r.lastRegister,r.lastValue))return Status::TransportError;
    s=read(reg,old,r);if(s!=Status::Ok)return s;return (old&mask)==(v&mask)?Status::Ok:Status::VerifyFailed;
}
Status Controller::wait(Register reg,uint32_t mask,uint32_t value,const vpg::Poll& p,Result& r){
    for(uint32_t i=0;i<p.maxPolls;++i){uint32_t observed=0;auto s=read(reg,observed,r);if(s!=Status::Ok)return s;
        ++r.polls;if((observed&mask)==value)return Status::Ok;if(i+1<p.maxPolls)io_.delayUs(io_.context,p.delayUs);}
    return Status::Timeout;
}
Status Controller::route(Result& r){
    uint32_t value=0;auto s=read(Register::Frontend,value,r);if(s!=Status::Ok)return s;
    if((value&0xd017f177)!=config_.pipe)return Status::ForeignControl;
    s=readAddress(layout_.registers[config_.backend][size_t(Register::BackendEnable)],value,r);if(s!=Status::Ok)return s;
    if(!(value&1))return Status::ForeignControl;
    for(uint8_t n=0;n<5;++n){s=readAddress(layout_.registers[n][size_t(Register::Backend)],value,r);if(s!=Status::Ok)return s;
        if(n==config_.backend){if((value&0x77f00)!=(0x30000u|(0x100u<<config_.frontend)))return Status::ForeignControl;}
        else if(value&(0x100u<<config_.frontend))return Status::ForeignControl;
    }
    s=read(Register::DpStream,value,r);if(s!=Status::Ok)return s;if(value&0x10001)return Status::ForeignControl;
    s=read(Register::Hdmi,value,r);if(s!=Status::Ok)return s;if((value&0x19)!=0x19)return Status::ForeignControl;
    s=read(Register::Vbi,value,r);if(s!=Status::Ok)return s;if((value&0x1031)!=0x31)return Status::ForeignControl;
    return Status::Ok;
}
Status Controller::prepare(const Layout& l,const vpg::Io& io,const Config& c,const edid::Monitor& sink,const vpg::Poll& p,Result& r){
    r={};if(phase_!=Phase::Closed&&phase_!=Phase::Stopped){r.status=Status::InvalidState;return r.status;}
    if(!valid(l)||!valid(p)||!io.read32||!io.write32||!io.delayUs||c.frontend>=5||c.backend>=5||c.pipe>=4)return finish(Status::InvalidArgument,r);
    vpg::Packet packet={};if(avi::encode(c.infoframe,sink,packet)!=avi::Status::Ok)return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;config_=c;auto s=route(r);if(s!=Status::Ok)return finish(s,r);
    struct Guard{Register reg;uint32_t mask,value;};
    const Guard guards[]={{Register::Fifo,0x181,0},{Register::General,1,1},{Register::Packets,0x33333333,0},
        {Register::ExtraPackets,0x03333333,0},{Register::DoubleBuffer,0x8101,0},
        {Register::Infoframe,0x330,0},{Register::Acr,0x1003,0}};
    for(const auto& g:guards){uint32_t v=0;s=read(g.reg,v,r);if(s!=Status::Ok)return finish(s,r);if((v&g.mask)!=g.value)return finish(Status::ForeignControl,r);}
    uint32_t value=0;s=read(Register::Clock,value,r);if(s!=Status::Ok)return finish(s,r);savedClock_=value&1;
    s=read(Register::DoubleBuffer,value,r);if(s!=Status::Ok)return finish(s,r);savedDb_=value&0x1000;
    s=update(Register::DoubleBuffer,0x1000,0x1000,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Clock,1,1,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Clock,0x100,0x100,p,r);if(s!=Status::Ok)return finish(s,r);
    if(packets_.open(l.packets,io,c.frontend,p,r.packet)!=vpg::Status::Ok)return finish(Status::PacketFailed,r);
    if(packets_.upload(0,packet,p,r.packet)!=vpg::Status::Ok)return finish(Status::PacketFailed,r);
    s=update(Register::Line,0xffff,2,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Prepared;return finish(Status::Ok,r);
}
Status Controller::start(const vpg::Poll& p,Result& r){
    r={};if(phase_!=Phase::Prepared){r.status=Status::InvalidState;return r.status;}if(!valid(p))return finish(Status::InvalidArgument,r);
    auto s=route(r);if(s!=Status::Ok)return finish(s,r);uint32_t value=0;
    const auto* timing=layout_.timing.registers[config_.pipe];
    s=readAddress(timing[size_t(otg::Register::Control)],value,r);if(s!=Status::Ok)return finish(s,r);
    if((value&0x10001)!=0x10001)return finish(Status::ForeignControl,r);
    s=readAddress(timing[size_t(otg::Register::Clock)],value,r);if(s!=Status::Ok)return finish(s,r);
    if(!(value&0x100))return finish(Status::ForeignControl,r);
    s=wait(Register::Frontend,0x1000000,0x1000000,p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Packets,15,3,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,0x7c,7u<<2,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,2,2,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Fifo,0x100000,0x100000,p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,2,0,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::Fifo,0x100000,0,p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Fifo,1,1,r);if(s!=Status::Ok)return finish(s,r);
    s=readAddress(timing[size_t(otg::Register::FrameCount)],r.firstFrame,r);if(s!=Status::Ok)return finish(s,r);r.firstFrame&=0xffffff;
    for(uint32_t n=0;n<p.maxPolls;++n){
        s=read(Register::Fifo,value,r);if(s!=Status::Ok)return finish(s,r);if(value&0x30000000)return finish(Status::FifoError,r);
        s=readAddress(timing[size_t(otg::Register::FrameCount)],r.lastFrame,r);if(s!=Status::Ok)return finish(s,r);r.lastFrame&=0xffffff;++r.polls;
        const uint32_t delta=(r.lastFrame-r.firstFrame)&0xffffff;
        if(delta>=2&&delta<0x800000){r.framesAdvanced=true;break;}
        if(n+1<p.maxPolls)io_.delayUs(io_.context,p.delayUs);
    }
    if(!r.framesAdvanced)return finish(Status::Timeout,r);
    s=route(r);if(s!=Status::Ok)return finish(s,r);
    s=readAddress(timing[size_t(otg::Register::Control)],value,r);if(s!=Status::Ok)return finish(s,r);
    if((value&0x10001)!=0x10001)return finish(Status::VerifyFailed,r);
    s=read(Register::Fifo,value,r);if(s!=Status::Ok)return finish(s,r);
    if((value&0x30100103)!=1)return finish(Status::FifoError,r);
    s=update(Register::General,1,0,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Running;return finish(Status::Ok,r);
}
Status Controller::stop(const vpg::Poll& p,Result& r){
    r={};if(!touched_||!valid(p)){r.status=Status::InvalidState;return r.status;}
    auto s=update(Register::General,1,1,r);if(s!=Status::Ok)return finish(s,r);r.muted=true;
    s=update(Register::Fifo,1,0,r);if(s!=Status::Ok)return finish(s,r);r.fifoDisabled=true;
    s=update(Register::Packets,3,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Line,0xffff,0,r);if(s!=Status::Ok)return finish(s,r);
    if(packets_.phase()!=vpg::Phase::Closed&&packets_.close(p,r.packet)!=vpg::Status::Ok)return finish(Status::PacketFailed,r);
    s=update(Register::DoubleBuffer,0x1000,savedDb_,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Clock,1,savedClock_,r);if(s!=Status::Ok)return finish(s,r);
    touched_=false;phase_=Phase::Stopped;return finish(Status::Ok,r);
}
} }
