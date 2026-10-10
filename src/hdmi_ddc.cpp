// SPDX-License-Identifier: MIT
/*
 * DCN3.2 I2C protocol adapted from Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
 * display/dc/dce/dce_i2c_hw.c/.h; dc/gpio/hw_gpio.c, hw_ddc.c, ddc_regs.h,
 * dc/gpio/dcn32/hw_factory_dcn32.c; dc/resource/dcn32/dcn32_resource.c;
 * dcn_3_2_0_offset.h/sh_mask.h. EDID transaction addressing: drm_edid.c.
 * Copyright 2012-2022 Advanced Micro Devices, Inc.
 * Copyright (c) 2006 Luc Verhaegen (quirks list)
 * Copyright (c) 2007-2008 Intel Corporation
 * Copyright 2010 Red Hat, Inc.
 * DDC probing originally from FB layer:
 * Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>
 *
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
#include "hackynavi/hdmi_ddc.hpp"
namespace hackynavi { namespace hdmiddc {
namespace {
const uint32_t offsets[kRegisters]={0x1ea3,0x1ea2,0x1e9c,0x1e99,0x1e98,
    0x1e9b,0x1eae,0x1eaf,0x1eb0,0x1eb1,0x1eb2,0x7b,0x1ede,0x1edd,
    0x28d0,0x28d2,0x291d,0x28ff};
constexpr uint32_t kGpioBits=0x11111,kSpeedBits=0xffff0303;
struct Dmu { uint64_t base1,base2;unsigned count;bool valid; };
void collect(const DiscoveryIp& ip,void* ctx) {
    if(ip.hardwareId!=271)return;
    Dmu& d=*static_cast<Dmu*>(ctx);++d.count;
    d.valid=ip.dieId==0 && ip.instance==0 && ip.major==3 && ip.minor==2 &&
        ip.revision==0 && !ip.harvest && !ip.subRevision && !ip.variant &&
        ip.baseAddressCount>=3 && ip.baseAddresses[1]<=UINT32_MAX && ip.baseAddresses[2]<=UINT32_MAX;
    if(d.valid){d.base1=ip.baseAddresses[1];d.base2=ip.baseAddresses[2];}
}
bool populate(uint64_t b1,uint64_t b2,uint64_t bytes,Layout& l) {
    l=Layout{};
    if(bytes<4 || b1>UINT32_MAX/4 || b2>UINT32_MAX/4)return false;
    for(size_t p=0;p<kPorts;++p)for(size_t r=0;r<kRegisters;++r) {
        const uint64_t stride=r<2?p*2:r==2?p:(r==14 || r==15)?p*4:0;
        const uint64_t offset=((r==11?b1:b2)+offsets[r]+stride)*4;
        if(offset>UINT32_MAX || offset>bytes-4)return false;
        l.registers[p][r]=static_cast<uint32_t>(offset);
        // Base1 time register must not alias any other semantic register.
        if(r!=11 && offset==(b1+offsets[11])*4)return false;
    }
    l.valid=true;l.barBytes=bytes;return true;
}
bool validLayout(const Layout& l) {
    if(!l.valid || (l.registers[0][0]&3) || (l.registers[0][11]&3) ||
        l.registers[0][0]/4<offsets[0] || l.registers[0][11]/4<offsets[11])return false;
    Layout expected{};
    if(!populate(l.registers[0][11]/4-offsets[11],l.registers[0][0]/4-offsets[0],l.barBytes,expected))return false;
    for(size_t p=0;p<kPorts;++p)for(size_t r=0;r<kRegisters;++r)
        if(l.registers[p][r]!=expected.registers[p][r])return false;
    return true;
}
void clear(Result& r){r=Result{};}
}
Status calculateSpeed(const Clock& clock,uint32_t timeBase,Speed& out) {
    out=Speed{};
    if(!clock.crystalKHz || clock.crystalKHz>1000000 || !clock.requestedKHz || clock.requestedKHz>400 ||
        timeBase==UINT32_MAX)return Status::InvalidClock;
    const uint32_t base=timeBase&127;
    const uint32_t xtal=(timeBase>>8)&127;
    const uint32_t ref=base?base*1000:(clock.crystalKHz/2)*2;
    const uint32_t input=ref/(xtal?xtal:2);
    const uint32_t prescale=input/clock.requestedKHz;
    if(!prescale || prescale>65535)return Status::InvalidClock;
    out=Speed{timeBase,ref,input,prescale};return Status::Ok;
}
Status buildLayout(const uint8_t* b,size_t n,uint64_t bytes,Layout& out) {
    out=Layout{};DiscoveryInfo info{};Dmu d{};
    if(parseDiscovery(b,n,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(!d.count)return Status::MissingDmu;
    if(d.count!=1 || !d.valid || info.baseAddresses64Bit || info.dieCount!=1 || info.dies[0].id)
        return Status::UnsupportedDmu;
    Layout l{};if(!populate(d.base1,d.base2,bytes,l))return Status::OutsideBar;
    out=l;return Status::Ok;
}
Engine::Engine():phase_(Phase::Closed),io_{},speed_{},port_(0),reg_{},saved_{}{}
void Engine::clearAfterVerifiedReset(){phase_=Phase::Closed;io_=Io{};speed_=Speed{};port_=0;
    for(size_t i=0;i<kRegisters;++i){reg_[i]=0;saved_[i]=0;}}
Status Engine::fail(Status s){phase_=Phase::Faulted;return s;}
Status Engine::read(Register r,uint32_t& v,Result& out){out.lastRegister=reg_[static_cast<size_t>(r)];++out.reads;
    if(!io_.read32(io_.context,out.lastRegister,v))return fail(Status::TransportError);
    return v==UINT32_MAX?fail(Status::DeviceUnavailable):Status::Ok;}
Status Engine::write(Register r,uint32_t v,Result& out){out.lastRegister=reg_[static_cast<size_t>(r)];++out.writes;
    return io_.write32(io_.context,out.lastRegister,v)?Status::Ok:fail(Status::TransportError);}
Status Engine::update(Register r,uint32_t mask,uint32_t value,Result& out,bool verify){
    uint32_t v=0;Status s=read(r,v,out);if(s!=Status::Ok)return s;
    s=write(r,(v&~mask)|(value&mask),out);if(s!=Status::Ok || !verify)return s;
    s=read(r,v,out);if(s!=Status::Ok)return s;
    return (v&mask)==(value&mask)?Status::Ok:fail(Status::VerifyFailed);
}
Status Engine::owned(Result& out){uint32_t v=0;Status s=read(Register::Arbitration,v,out);
    if(s!=Status::Ok)return s;
    // The request strobe can clear when granted (Linux stuck-controller WA).
    return (v&12)==4?Status::Ok:fail(Status::OwnershipLost);}
#define DDC_TRY(x) do{const Status s_=(x);if(s_!=Status::Ok)return s_;}while(0)
Status Engine::open(const Layout& l,const Io& io,uint8_t port,const Clock& clock,Result& out){
    clear(out);
    if(phase_!=Phase::Closed)return phase_==Phase::Faulted?Status::Faulted:Status::Busy;
    if(!validLayout(l) || !io.read32 || !io.write32 || !io.delayUs || port>=5)return Status::InvalidArgument;
    // Validate scalar inputs before read-only preflight.
    if(!clock.crystalKHz || clock.crystalKHz>1000000 || !clock.requestedKHz || clock.requestedKHz>400)
        return Status::InvalidClock;
    io_=io;port_=port;for(size_t r=0;r<kRegisters;++r)reg_[r]=l.registers[port][r];
    uint32_t value=0;
    DDC_TRY(read(Register::Arbitration,value,out));
    if((value&12)!=0 || (value&0x1100000))return Status::Busy;
    saved_[static_cast<size_t>(Register::Arbitration)]=value;
    DDC_TRY(read(Register::HardwareStatus,value,out));if(value&3)return Status::Busy;
    DDC_TRY(read(Register::Status,value,out));if((value&3) || (value&0x40000))return Status::Busy;
    DDC_TRY(read(Register::Control,value,out));if(value&5)return Status::Busy;
    DDC_TRY(read(Register::TimeBase,value,out));
    if(calculateSpeed(clock,value,speed_)!=Status::Ok)return Status::InvalidClock;
    const Register savedRegs[]={Register::Setup,Register::Speed,Register::MemoryPowerControl,
        Register::GpioMask,Register::GpioEnable,Register::GpioAux,Register::PhyAux};
    for(Register r:savedRegs)DDC_TRY(read(r,saved_[static_cast<size_t>(r)],out));
    phase_=Phase::Opening;
    // hw_gpio HARDWARE mode and hw_ddc I2C mode: pulls first, then pad mux.
    DDC_TRY(update(Register::GpioMask,0x101,0,out,true));
    if((saved_[14]&0x1010)!=0x1010){
        DDC_TRY(update(Register::GpioMask,0x1010,0x1010,out,true));
        io_.delayUs(io_.context,3000); // >=2.5ms discharge for dual-mode pads
    }
    if((saved_[14]&0x10000) && (saved_[15]&0x101))io_.delayUs(io_.context,2000);
    DDC_TRY(update(Register::GpioMask,0x10000,0,out,true));
    DDC_TRY(update(Register::GpioAux,1U<<(12+port_),1U<<(12+port_),out,true));
    DDC_TRY(update(Register::PhyAux,3U<<(10+2*port_),1U<<(10+2*port_),out,true));
    DDC_TRY(update(Register::Control,2,0,out,true));
    DDC_TRY(update(Register::MemoryPowerControl,1,0,out,true));
    bool awake=false;
    for(unsigned i=0;i<5;++i){++out.polls;DDC_TRY(read(Register::MemoryPowerStatus,value,out));
        if(!(value&1)){awake=true;break;}io_.delayUs(io_.context,1);}
    if(!awake)return fail(Status::PowerTimeout);
    // Request global I2C register ownership; never claim another software
    // owner's grant and never abort DMCU/HW traffic to steal the controller.
    DDC_TRY(update(Register::Arbitration,0x300000,0x100000,out));DDC_TRY(owned(out));
    const uint32_t speed=(speed_.prescaler<<16)|2U|((clock.requestedKHz>50?2U:1U)<<8);
    DDC_TRY(update(Register::Speed,kSpeedBits,speed,out,true));
    DDC_TRY(update(Register::Control,0x30070d,8U|(static_cast<uint32_t>(port_)<<8),out));
    // DCN's setup limit is3; SEND_RESET_LENGTH is a one-bit field with9
    // written by Linux (9&1 ==1), selecting the source's nine-clock policy.
    // Suspend autonomous EDID detection on this owned port for the command
    // scope (hw_ddc DISABLE_POLLING field); restore its prior state on close.
    DDC_TRY(update(Register::Setup,0xff000054,0x03000044,out,true));
    DDC_TRY(update(Register::Arbitration,0x10,0,out,true));
    DDC_TRY(owned(out));phase_=Phase::Open;return Status::Ok;
}
Status Engine::transfer(const Message* messages,size_t count,Result& out){
    clear(out);
    if(!messages || !count || count>4)return Status::InvalidArgument;
    size_t total=0,writeBytes=0,readBytes=0;
    for(size_t i=0;i<count;++i){const Message& m=messages[i];
        if(m.address>0x7f || m.bytes>=kFifoBytes ||
            (m.read?(!m.bytes || i+1!=count):(m.bytes && !m.data)))return Status::InvalidArgument;
        total+=1+m.bytes;writeBytes+=1+(m.read?0:m.bytes);if(m.read)readBytes=m.bytes;
    }
    if(total>kFifoBytes)return Status::InvalidArgument;
    if(phase_!=Phase::Open)return phase_==Phase::Faulted?Status::Faulted:Status::Closed;
    // Derive the timeout before any command programming. Integer ceiling
    // preserves at least32 actual SCL clocks for each stretch opportunity.
    const uint64_t stretch=(32000ULL*speed_.prescaler+speed_.inputKHz-1)/speed_.inputKHz;
    const uint64_t timeout=stretch*(2+8*total+2*count);
    if(!timeout || timeout>10000000)return Status::InvalidClock;
    out.timeoutUs=static_cast<uint32_t>(timeout);
    DDC_TRY(owned(out));uint32_t value=0;
    DDC_TRY(read(Register::MemoryPowerStatus,value,out));if(value&1)return fail(Status::PowerTimeout);
    DDC_TRY(read(Register::TimeBase,value,out));if(value!=speed_.timeBase)return fail(Status::InvalidClock);
    DDC_TRY(read(Register::Status,value,out));if((value&3) || (value&0x40000))return fail(Status::Busy);
    // Reset stale status before the new command and insist terminal/error bits
    // cleared, preventing an old DONE from validating a dropped GO.
    DDC_TRY(update(Register::Control,8,8,out));
    DDC_TRY(read(Register::Status,value,out));
    if(value&0x4f1f7)return fail(Status::VerifyFailed);
    for(size_t i=0;i<count;++i){const Message& m=messages[i];
        const Register transaction=static_cast<Register>(static_cast<size_t>(Register::Transaction0)+i);
        const uint32_t descriptor=(static_cast<uint32_t>(m.bytes)<<16)|0x1100U|
            (m.read?1U:0)|(i+1==count?0x2000U:0);
        DDC_TRY(update(transaction,0x3ff3101,descriptor,out,true));
        const uint32_t address=(static_cast<uint32_t>(m.address)<<1)|(m.read?1U:0);
        DDC_TRY(write(Register::Data,(i==0?0x80000000U:0)|(address<<8),out));
        if(!m.read)for(size_t j=0;j<m.bytes;++j)
            DDC_TRY(write(Register::Data,static_cast<uint32_t>(m.data[j])<<8,out));
    }
    DDC_TRY(update(Register::Setup,0xffff83,0,out,true));
    const uint32_t controlCount=static_cast<uint32_t>(count-1)<<20;
    DDC_TRY(update(Register::Control,0x30000f,controlCount,out));
    DDC_TRY(owned(out));
    // Linux stretch budget:32 SCL clocks per bit, with start/stop overhead.
    // Derive using the actual rational divider, rounding up microseconds.
    DDC_TRY(read(Register::Control,value,out));out.submitted=true;
    DDC_TRY(write(Register::Control,value|1U,out));
    bool terminal=false;
    for(uint32_t i=0;i<out.timeoutUs;++i){++out.polls;DDC_TRY(read(Register::Status,value,out));out.rawStatus=value;
        if((value&3)==2 || (value&3)==3)return fail(Status::OwnershipLost);
        if(!(value&3) && !(value&0x40000) && (value&0x1f4)){terminal=true;break;}
        io_.delayUs(io_.context,1);
    }
    if(!terminal)return fail(Status::CompletionTimeout);
    out.completed=true;DDC_TRY(owned(out));
    if(value&0x100)return Status::Nack;
    if(value&0x20)return Status::WireTimeout;
    if(value&0x10)return Status::Aborted;
    if(value&0x40)return Status::Interrupted;
    if(value&0x80)return Status::BufferOverflow;
    if(!(value&4))return fail(Status::VerifyFailed);
    if(readBytes){
        // Receive bytes follow ALL outgoing address/data bytes in the single
        // circular buffer. Set read index explicitly, without reading DATA.
        DDC_TRY(write(Register::Data,0x80000001U|(static_cast<uint32_t>(writeBytes)<<16),out));
        for(size_t i=0;i<readBytes;++i){DDC_TRY(read(Register::Data,value,out));out.data[i]=static_cast<uint8_t>(value>>8);}
        out.bytes=static_cast<uint16_t>(readBytes);
    }
    return Status::Ok;
}
Status Engine::readEdidBlock(uint8_t block,Result& out){
    const uint8_t segment=block>>1,offset=static_cast<uint8_t>(block*128U);
    const Message messages[]={{0x30,false,1,&segment},{0x50,false,1,&offset},{0x50,true,128,nullptr}};
    return transfer(messages+(segment?0:1),segment?3:2,out);
}
Status Engine::readScdc(uint8_t offset,uint16_t bytes,Result& out){
    if(!bytes || bytes>256U-offset){clear(out);return Status::InvalidArgument;}
    const Message messages[]={{0x54,false,1,&offset},{0x54,true,bytes,nullptr}};
    return transfer(messages,2,out);
}
Status Engine::writeScdc(uint8_t offset,const uint8_t* data,uint16_t bytes,Result& out){
    if(!data || !bytes || bytes>256U-offset){clear(out);return Status::InvalidArgument;}
    uint8_t payload[257]={};payload[0]=offset;for(size_t i=0;i<bytes;++i)payload[i+1]=data[i];
    const Message message{0x54,false,static_cast<uint16_t>(bytes+1),payload};return transfer(&message,1,out);
}
Status Engine::close(Result& out){
    clear(out);if(phase_==Phase::Closed)return Status::Ok;
    if(phase_!=Phase::Open)return Status::Faulted;
    DDC_TRY(owned(out));uint32_t value=0;
    DDC_TRY(read(Register::Status,value,out));if((value&3) || (value&0x40000))return fail(Status::Busy);
    DDC_TRY(update(Register::Control,8,8,out));
    DDC_TRY(update(Register::Setup,0xffffffd7,saved_[0],out,true));
    DDC_TRY(update(Register::Speed,kSpeedBits,saved_[1],out,true));
    // Restore GPIO while still owning registers; other DDC/AUX actors remain
    // excluded by the caller's lock throughout the complete open/close scope.
    DDC_TRY(update(Register::GpioMask,kGpioBits,saved_[14],out,true));
    DDC_TRY(update(Register::GpioAux,1U<<(12+port_),saved_[16],out,true));
    DDC_TRY(update(Register::PhyAux,3U<<(10+2*port_),saved_[17],out,true));
    DDC_TRY(update(Register::MemoryPowerControl,1,saved_[12],out,true));
    DDC_TRY(update(Register::Arbitration,0x10,saved_[3],out,true));
    DDC_TRY(update(Register::Arbitration,0x300000,0x200000,out));
    DDC_TRY(read(Register::Arbitration,value,out));
    if((value&12)==4){ // source cntl_stuck_hw_workaround, exactly one retry
        DDC_TRY(update(Register::Arbitration,0x300000,0x200000,out));
        DDC_TRY(read(Register::Arbitration,value,out));
    }
    if((value&12)==4 || (value&12)==12 || (value&0x100000))return fail(Status::VerifyFailed);
    clearAfterVerifiedReset();return Status::Ok;
}
#undef DDC_TRY
const char* statusString(Status s){switch(s){
#define DDC_CASE(x) case Status::x:return #x
    DDC_CASE(Ok);DDC_CASE(InvalidArgument);DDC_CASE(DiscoveryRejected);DDC_CASE(MissingDmu);
    DDC_CASE(UnsupportedDmu);DDC_CASE(OutsideBar);DDC_CASE(InvalidClock);DDC_CASE(Closed);
    DDC_CASE(Busy);DDC_CASE(Faulted);DDC_CASE(TransportError);DDC_CASE(DeviceUnavailable);
    DDC_CASE(OwnershipLost);DDC_CASE(PowerTimeout);DDC_CASE(VerifyFailed);DDC_CASE(CompletionTimeout);
    DDC_CASE(Nack);DDC_CASE(WireTimeout);DDC_CASE(Aborted);DDC_CASE(Interrupted);DDC_CASE(BufferOverflow);
#undef DDC_CASE
    }return "Unknown";
}
} }
