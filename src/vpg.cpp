// SPDX-License-Identifier: MIT
/*
 * Copyright 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
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


// DCN3.2 protocol: Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0.
#include "hackynavi/vpg.hpp"
namespace hackynavi { namespace vpg { namespace {
const uint32_t offsets[kRegisterCount]={0x2068,0x2069,0x206a,0x206b,0x206c,0x206d,0x20b0};
struct Dmu { uint32_t count;uint64_t base;bool valid; };
void collect(const DiscoveryIp& ip,void* context) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=3&&ip.baseAddresses[2]<=UINT32_MAX;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool populate(uint64_t base,uint64_t bar,Layout& l) {
    l={};if(bar<4||base>UINT32_MAX/4)return false;
    for(size_t i=0;i<kInstanceCount;++i)for(size_t r=0;r<kRegisterCount;++r){
        const uint64_t address=(base+offsets[r]+i*0x100)*4;
        if(address>UINT32_MAX||address>bar-4)return false;
        l.registers[i][r]=uint32_t(address);
    }
    l.valid=true;l.barBytes=bar;return true;
}
bool valid(const Layout& l) {
    if(!l.valid||(l.registers[0][0]&3)||l.registers[0][0]/4<offsets[0])return false;
    Layout expected={};if(!populate(l.registers[0][0]/4-offsets[0],l.barBytes,expected))return false;
    for(size_t i=0;i<kInstanceCount;++i)for(size_t r=0;r<kRegisterCount;++r)
        if(l.registers[i][r]!=expected.registers[i][r])return false;
    return true;
}
bool valid(const Poll& p) { return p.maxPolls&&p.maxPolls<=2000000&&p.delayUs&&p.delayUs<=100&&uint64_t(p.maxPolls)*p.delayUs<=2000000; }
uint32_t word(const uint8_t* b){return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);}
} // namespace
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t bar,Layout& l) {
    l={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(bytes,count,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.base,bar,l)?Status::Ok:Status::OutsideBar;
}
Engine::Engine():layout_{},io_{},instance_(0),phase_(Phase::Closed),touched_(false),savedPower_(0) {}
void Engine::clearAfterVerifiedReset(){layout_={};io_={};instance_=0;phase_=Phase::Closed;touched_=false;savedPower_=0;}
Status Engine::finish(Status s,Result& r){r.status=s;if(s!=Status::Ok&&touched_)phase_=Phase::Faulted;return s;}
Status Engine::read(Register reg,uint32_t& value,Result& r){
    r.lastRegister=layout_.registers[instance_][size_t(reg)];++r.reads;
    if(!io_.read32(io_.context,r.lastRegister,value))return Status::TransportError;
    r.lastValue=value;return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Engine::write(Register reg,uint32_t value,Result& r){
    r.lastRegister=layout_.registers[instance_][size_t(reg)];r.lastValue=value;
    ++r.writes;r.writeAttempted=touched_=true;
    return io_.write32(io_.context,r.lastRegister,value)?Status::Ok:Status::TransportError;
}
Status Engine::update(Register reg,uint32_t mask,uint32_t value,Result& r){
    uint32_t observed=0;auto s=read(reg,observed,r);if(s!=Status::Ok)return s;
    s=write(reg,(observed&~mask)|(value&mask),r);if(s!=Status::Ok)return s;
    s=read(reg,observed,r);if(s!=Status::Ok)return s;
    return (observed&mask)==(value&mask)?Status::Ok:Status::VerifyFailed;
}
Status Engine::wait(Register reg,uint32_t mask,uint32_t value,const Poll& p,Status timeout,Result& r){
    for(uint32_t n=0;n<p.maxPolls;++n){uint32_t observed=0;auto s=read(reg,observed,r);if(s!=Status::Ok)return s;
        ++r.polls;if((observed&mask)==value)return Status::Ok;if(n+1<p.maxPolls)io_.delayUs(io_.context,p.delayUs);}
    return timeout;
}
Status Engine::idle(const Poll& p,Result& r){
    uint32_t v=0;auto s=read(Register::Clock,v,r);if(s!=Status::Ok)return s;
    if((v&0x101)!=0x101)return Status::ForeignControl;
    s=wait(Register::Status,3,0,p,Status::UpdateTimeout,r);if(s!=Status::Ok)return s;
    s=wait(Register::FrameUpdate,0x7fff7fff,0,p,Status::UpdateTimeout,r);if(s!=Status::Ok)return s;
    return wait(Register::ImmediateUpdate,0x7fff7fff,0,p,Status::UpdateTimeout,r);
}
Status Engine::open(const Layout& l,const Io& io,uint8_t instance,const Poll& p,Result& r){
    r={};if(phase_!=Phase::Closed)return finish(phase_==Phase::Faulted?Status::Faulted:Status::InvalidState,r);
    if(!valid(l)||instance>=kInstanceCount||!io.read32||!io.write32||!io.delayUs||!valid(p))return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;instance_=instance;
    auto s=idle(p,r);if(s!=Status::Ok)return finish(s,r);
    uint32_t power=0;s=read(Register::MemoryPower,power,r);if(s!=Status::Ok)return finish(s,r);
    // A forced sleep belongs to another power actor, not a normal gated SRAM.
    if(power&0x10)return finish(Status::ForeignControl,r);savedPower_=power&0x11;
    s=update(Register::MemoryPower,0x11,1,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::MemoryPower,0x100,0,p,Status::PowerTimeout,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Open;return finish(Status::Ok,r);
}
Status Engine::upload(uint8_t packet,const Packet& bytes,const Poll& p,Result& r){
    r={};if(phase_!=Phase::Open)return finish(phase_==Phase::Faulted?Status::Faulted:Status::InvalidState,r);
    if(packet>=kPacketCount||!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    auto s=idle(p,r);if(s!=Status::Ok)return finish(s,r);
    uint32_t power=0;s=read(Register::MemoryPower,power,r);if(s!=Status::Ok)return finish(s,r);
    if((power&0x111)!=1)return finish(Status::ForeignControl,r);
    // Conflict clear and update requests are strobes. Do not require a
    // request bit to remain set, or read the auto-increment SRAM data window.
    s=write(Register::Status,0x10,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Access,0xff,uint32_t(packet)*kPacketWords,r);if(s!=Status::Ok)return finish(s,r);
    s=write(Register::Data,word(bytes.header),r);if(s!=Status::Ok)return finish(s,r);++r.packetWords;
    for(size_t n=0;n<8;++n){s=write(Register::Data,word(bytes.payload+4*n),r);if(s!=Status::Ok)return finish(s,r);++r.packetWords;}
    uint32_t status=0;s=read(Register::Status,status,r);if(s!=Status::Ok)return finish(s,r);
    if(status&3)return finish(Status::VerifyFailed,r);
    r.updateSubmitted=true;s=write(Register::ImmediateUpdate,1u<<packet,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::ImmediateUpdate,(1u<<packet)|(1u<<(16+packet)),0,p,Status::UpdateTimeout,r);if(s!=Status::Ok)return finish(s,r);
    s=read(Register::Status,status,r);if(s!=Status::Ok)return finish(s,r);
    if(status&3)return finish(Status::VerifyFailed,r);
    r.updateCompleted=true;return finish(Status::Ok,r);
}
Status Engine::close(const Poll& p,Result& r){
    r={};if(phase_!=Phase::Open)return finish(phase_==Phase::Faulted?Status::Faulted:Status::InvalidState,r);
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    auto s=idle(p,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::MemoryPower,0x11,savedPower_,r);if(s!=Status::Ok)return finish(s,r);
    clearAfterVerifiedReset();return finish(Status::Ok,r);
}
const char* statusString(Status s){switch(s){
    case Status::Ok:return "ok";case Status::InvalidArgument:return "invalid-argument";case Status::InvalidState:return "invalid-state";
    case Status::DiscoveryRejected:return "discovery-rejected";case Status::UnsupportedDmu:return "unsupported-dmu";case Status::OutsideBar:return "outside-bar";
    case Status::TransportError:return "transport-error";case Status::DeviceUnavailable:return "device-unavailable";case Status::ForeignControl:return "foreign-control";
    case Status::VerifyFailed:return "verify-failed";case Status::PowerTimeout:return "power-timeout";case Status::UpdateTimeout:return "update-timeout";
    case Status::Faulted:return "faulted";}return "unknown";}
} }
