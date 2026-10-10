// SPDX-License-Identifier: MIT
/*
 * Copyright 2018, 2021 Advanced Micro Devices, Inc.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */
// dccg2_update_dpp_dto / dccg32_set_pixel_rate_div, Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0; DCN3.2 generated register masks.
#include "hackynavi/display_clock.hpp"
namespace hackynavi { namespace displayclock { namespace {
struct Dmu { unsigned count;uint64_t clockBase,pipeBase;bool valid; };
void collect(const DiscoveryIp& ip,void* context){
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&!ip.subRevision&&
        !ip.variant&&!ip.harvest&&ip.baseAddressCount>=3;
    if(d.valid){d.clockBase=ip.baseAddresses[1];d.pipeBase=ip.baseAddresses[2];}
}
bool populate(uint64_t clock,uint64_t pipe,uint64_t bar,Layout& l){
    l={};if(bar<4||clock>UINT32_MAX/4||pipe>UINT32_MAX/4)return false;
    for(size_t n=0;n<4;++n){const uint64_t registers[]={clock+0x6f,clock+0xb6,clock+0x99+n,pipe+0x1b41+n*0x80,pipe+0x5f3+n*0xdc};
        for(size_t r=0;r<kRegisterCount;++r){const uint64_t a=registers[r]*4;if(a>UINT32_MAX||a>bar-4)return false;l.registers[n][r]=uint32_t(a);}}
    l.valid=true;l.barBytes=bar;return true;
}
bool valid(const Layout& l){
    if(!l.valid||(l.registers[0][0]&3)||(l.registers[0][3]&3)||l.registers[0][0]/4<0x6f||l.registers[0][3]/4<0x1b41)return false;
    Layout expected={};if(!populate(l.registers[0][0]/4-0x6f,l.registers[0][3]/4-0x1b41,l.barBytes,expected))return false;
    for(size_t n=0;n<4;++n)for(size_t r=0;r<kRegisterCount;++r)if(l.registers[n][r]!=expected.registers[n][r])return false;
    return true;
}
}
Status plan(const Config& c,Plan& output){
    if(!c.referenceDppClockKHz||!c.requiredDppClockKHz||c.requiredDppClockKHz>c.referenceDppClockKHz||
        (c.k1Divisor!=1&&c.k1Divisor!=2)||(c.k2Divisor!=1&&c.k2Divisor!=2&&c.k2Divisor!=4))return Status::InvalidArgument;
    Plan p={};const uint64_t phase=(uint64_t(c.requiredDppClockKHz)*255+c.referenceDppClockKHz-1)/c.referenceDppClockKHz;
    p.phase=uint8_t(phase);p.dtoParam=0x00ff0000|uint32_t(phase);
    p.pixelDivBits=uint32_t(c.k1Divisor-1)|(uint32_t(c.k2Divisor-1)<<1);
    p.minimumOutputKHz=uint32_t(uint64_t(c.referenceDppClockKHz)*phase/255);output=p;return Status::Ok;
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t bar,Layout& l){
    l={};Dmu d={};DiscoveryInfo info={};
    if(parseDiscovery(bytes,count,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.clockBase,d.pipeBase,bar,l)?Status::Ok:Status::OutsideBar;
}
Controller::Controller():layout_{},io_{},pipe_(0),touched_(false),configured_(false),savedParam_(0),savedDiv_(0){}
void Controller::clearAfterVerifiedReset(){layout_={};io_={};pipe_=0;touched_=configured_=false;savedParam_=savedDiv_=0;}
Status Controller::finish(Status s,Result& r){r.status=s;return s;}
Status Controller::read(Register reg,uint32_t& v,Result& r){
    r.lastRegister=layout_.registers[pipe_][size_t(reg)];++r.reads;
    if(!io_.read32(io_.context,r.lastRegister,v))return Status::TransportError;
    r.lastValue=v;
    return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::update(Register reg,uint32_t mask,uint32_t value,Result& r){
    uint32_t old=0;auto s=read(reg,old,r);if(s!=Status::Ok)return s;
    r.writeAttempted=touched_=true;++r.writes;r.lastValue=(old&~mask)|(value&mask);
    if(!io_.write32(io_.context,r.lastRegister,r.lastValue))return Status::TransportError;
    s=read(reg,old,r);if(s!=Status::Ok)return s;return (old&mask)==(value&mask)?Status::Ok:Status::VerifyFailed;
}
Status Controller::idle(Result& r){
    uint32_t value=0;auto s=read(Register::OtgControl,value,r);if(s!=Status::Ok)return s;
    if(value&0x10001)return Status::ActivePipe;
    s=read(Register::HubpControl,value,r);if(s!=Status::Ok)return s;
    // HUBP_NO_OUTSTANDING_REQ is the aggregate predicate used by upstream
    // hubp2_set_blank_regs. XRQ_NO_OUTSTANDING_REQ is a four-bit field;
    // requiring its low bit would invent a per-requestor completion rule.
    return (value&0xf)==0xb?Status::Ok:Status::ActivePipe;
}
Status Controller::configure(const Layout& l,const Io& io,uint8_t pipe,const Config& c,Result& r){
    r={};if(touched_||configured_)return finish(Status::InvalidState,r);
    Plan p={};if(!valid(l)||pipe>=4||!io.read32||!io.write32||plan(c,p)!=Status::Ok)return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;pipe_=pipe;auto s=idle(r);if(s!=Status::Ok)return finish(s,r);
    uint32_t value=0;s=read(Register::DtoControl,value,r);if(s!=Status::Ok)return finish(s,r);
    if(value&(3u<<(pipe*4)))return finish(Status::ForeignControl,r);
    s=read(Register::DtoParam,savedParam_,r);if(s!=Status::Ok)return finish(s,r);
    s=read(Register::PixelDiv,savedDiv_,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::PixelDiv,7u<<(pipe*3),p.pixelDivBits<<(pipe*3),r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::DtoParam,0x00ff00ff,p.dtoParam,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::DtoControl,1u<<(pipe*4),1u<<(pipe*4),r);if(s!=Status::Ok)return finish(s,r);
    configured_=true;return finish(Status::Ok,r);
}
Status Controller::stop(Result& r){
    r={};if(!touched_)return finish(Status::InvalidState,r);auto s=idle(r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::DtoControl,1u<<(pipe_*4),0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::DtoParam,0x00ff00ff,savedParam_,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::PixelDiv,7u<<(pipe_*3),savedDiv_,r);if(s!=Status::Ok)return finish(s,r);
    clearAfterVerifiedReset();return finish(Status::Ok,r);
}
} }

