// SPDX-License-Identifier: MIT
// Admission policy based on Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// ih_v6_0.c; osssys_6_0_0_offset/sh_mask.h; dcn30_dwb.c;
// dcn20/dcn32_mmhubbub.c; dcn32_hubp/hubbub/hwseq.c; dcn_3_2_0_offset/sh_mask.h.
// IH Linux is_idle is unimplemented; this code instead checks the generated
// hardware idle/clean fields. DWB disable alone is not treated as a drain.
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
#include "hackynavi/cold_dma.hpp"
namespace hackynavi { namespace colddma {
namespace {
struct Field { uint32_t offset,mask,value; };
const Field ihFields[kIhRegisters]={
    {0xc2,0x00fc73ff,0x000c0047}, // idle input/buffer/MC/self/CAM; no stall/clean pending/PG
    {0x80,0x80020f01,0}, // RB0, WPTR writeback, drain/clear, IRQ producer disabled
    {0x8c,0x80000e01,0}, // RB1 disabled, no drain/clear request
    {0xc1,0x100,0}, // autonomous self-IV WPTR updates off
    {0xef,0x70000,0}, // retry CAM and MM/GC backpressure disabled
    {0x83,0,0},{0x84,0xc0001,0}, // pointer offsets checked separately
    {0x8f,0,0},{0x90,0xc0001,0},
    {0xd4,0x77,0}, // both rings not full/draining/overflowed
    {0xe6,0xee,0}}; // no MMHUB write-response error
const Field wbFields[kWbRegisters]={
    {0x3229,0x00333300,0}, // SRAM force/state awake; do not infer drain through gated RAM
    {0x33f,0xff,0}, // all four MCIF writeback SRAM states awake (VGA independent)
    {0x340,0xc,0}, // no forced SRAM sleep/deep sleep
    {0x323b,1,0},{0x342,4,0}, // counters not held in DWB/WBIF reset
    {0x3228,1,0},{0x322a,0x80000001,0}, // DWB disabled, shadow/current capture disabled
    {0x322f,0x11,0},{0x3239,0x10001,0}, // no locked/pending update or overflow
    {0x272,0x01000f01,0}, // no buffer manager, software locks, address fence
    {0x274,4,0}, // no buffer overrun
    {0x276,0xf,0},{0x278,0xf,0},{0x27a,0xf,0},{0x27c,0xf,0}, // inactive/unlocked/no overflow
    {0x2ad,1,0}, // autonomous MCIF warmup disabled
    {0x336,0x07ffffff,0},{0x337,0x07ffffff,0}}; // actual outstanding requests, both phases
struct Bases { uint64_t ih,dmu,mp0; unsigned seen[3];bool bad; };
void collect(const DiscoveryIp& ip,void* context) {
    auto& b=*static_cast<Bases*>(context);unsigned i=3;
    if(ip.hardwareId==40)i=0;else if(ip.hardwareId==271)i=1;else if(ip.hardwareId==255)i=2;else return;
    ++b.seen[i];
    if(ip.dieId || ip.instance || ip.harvest || ip.subRevision || ip.variant ||
        ip.major!=(i==0?6:i==1?3:13) || ip.minor!=(i==1?2:0) || ip.revision ||
        ip.baseAddressCount<(i==1?3:1)){b.bad=true;return;}
    if(i==0)b.ih=ip.baseAddresses[0];else if(i==1)b.dmu=ip.baseAddresses[2];else b.mp0=ip.baseAddresses[0];
}
bool address(uint64_t base,uint32_t offset,uint64_t bytes,uint32_t& out) {
    if(base>UINT32_MAX/4 || bytes<4)return false;
    const uint64_t value=(base+offset)*4;
    if(value>UINT32_MAX || value>bytes-4 || value<0x1c || (value>=0x7f000 && value<0x80000))return false;
    out=static_cast<uint32_t>(value);return true;
}
constexpr size_t kAddresses=4+kIhRegisters+kWbRegisters+6*kPipes;
void flatten(const Layout& l,uint32_t (&v)[kAddresses]) {
    size_t n=0;v[n++]=l.sos;v[n++]=l.dmubControl;v[n++]=l.dmubReset;v[n++]=l.mall;
    for(uint32_t x:l.ih)v[n++]=x;for(uint32_t x:l.wb)v[n++]=x;
    for(size_t p=0;p<kPipes;++p){v[n++]=l.power[p];v[n++]=l.powerConfig[p];v[n++]=l.mallConfig[p];v[n++]=l.mallStatus[p];
        v[n++]=l.mallSubvp[p];v[n++]=l.vmPageConfig[p];}
}
bool populate(const Bases& b,uint64_t bytes,Layout& out) {
    out=Layout{};out.barBytes=bytes;
    if(!address(b.mp0,0x91,bytes,out.sos) || !address(b.dmu,0x1f6,bytes,out.dmubControl) ||
       !address(b.dmu,0x200,bytes,out.dmubReset) || !address(b.dmu,0x523,bytes,out.mall))return false;
    for(size_t i=0;i<kIhRegisters;++i)if(!address(b.ih,ihFields[i].offset,bytes,out.ih[i]))return false;
    for(size_t i=0;i<kWbRegisters;++i)if(!address(b.dmu,wbFields[i].offset,bytes,out.wb[i]))return false;
    for(uint32_t p=0;p<kPipes;++p)
        if(!address(b.dmu,0x81+2*p,bytes,out.power[p]) || !address(b.dmu,0x80+2*p,bytes,out.powerConfig[p]) ||
           !address(b.dmu,0x5f6+0xdc*p,bytes,out.mallConfig[p]) || !address(b.dmu,0x5ff+0xdc*p,bytes,out.mallStatus[p]) ||
           !address(b.dmu,0x5f7+0xdc*p,bytes,out.mallSubvp[p]) || !address(b.dmu,0x5f5+0xdc*p,bytes,out.vmPageConfig[p]))return false;
    uint32_t all[kAddresses]={};flatten(out,all);
    for(size_t i=0;i<kAddresses;++i)for(size_t j=0;j<i;++j)if(all[i]==all[j])return false;
    out.valid=true;return true;
}
bool valid(const Layout& l) {
    if(!l.valid || (l.sos&3) || l.sos/4<0x91 || (l.dmubControl&3) || l.dmubControl/4<0x1f6 ||
       (l.ih[0]&3) || l.ih[0]/4<0xc2)return false;
    Bases b={};b.mp0=l.sos/4-0x91;b.dmu=l.dmubControl/4-0x1f6;b.ih=l.ih[0]/4-0xc2;
    Layout expected={};if(!populate(b,l.barBytes,expected))return false;
    uint32_t actual[kAddresses]={},wanted[kAddresses]={};flatten(l,actual);flatten(expected,wanted);
    for(size_t i=0;i<kAddresses;++i)if(actual[i]!=wanted[i])return false;
    return true;
}
Status finish(Result& r,Status s){r.status=s;return s;}
bool read(const Io& io,uint32_t reg,uint32_t& value,Result& r) {
    r.lastRegister=reg;++r.reads;
    if(!io.read32(io.context,reg,value)){finish(r,Status::ReadFailed);return false;}
    r.lastValue=value;if(value==UINT32_MAX){finish(r,Status::DeviceUnavailable);return false;}return true;
}
bool match(const Io& io,uint32_t reg,uint32_t mask,uint32_t expected,Status failure,Result& r,uint32_t* sample=nullptr) {
    uint32_t value=0;r.mask=mask;r.expected=expected;
    if(!read(io,reg,value,r))return false;
    if((value&mask)!=expected){finish(r,failure);return false;}if(sample)*sample=value;return true;
}
bool guard(const Layout& l,const Io& io,Result& r) {
    r.pipe=0xff;
    return match(io,l.sos,UINT32_MAX,0,Status::PriorDriverRequiresReset,r) &&
        match(io,l.dmubControl,0x10000,0,Status::FirmwareNotStopped,r) &&
        match(io,l.dmubReset,1,1,Status::FirmwareNotStopped,r);
}
bool power(const Layout& l,const Io& io,size_t pipe,bool& off,Result& r) {
    r.pipe=static_cast<uint8_t>(pipe);uint32_t status=0,config=0,after=0;
    if(!read(io,l.power[pipe],status,r) || !read(io,l.powerConfig[pipe],config,r) || !read(io,l.power[pipe],after,r))return false;
    status&=0xd0000000;after&=0xd0000000;config&=0x101;
    if(status!=after || !((status==0 && !(config&0x100)) || (status==0x90000000 && config==0x100))) {
        finish(r,Status::PowerUnavailable);return false;
    }
    off=status==0x90000000;return true;
}
bool observed(const Io& io,uint32_t reg,const Field& f,Status failure,Result& r,uint32_t& first) {
    uint32_t value=0;if(!match(io,reg,f.mask,f.value,failure,r,&value))return false;
    if(r.pass && first!=value){finish(r,Status::UnstableSnapshot);return false;}first=value;return true;
}
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t barBytes,Layout& out) {
    out=Layout{};Bases b={};DiscoveryInfo info={};
    if(parseDiscovery(bytes,count,info,collect,&b)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(b.bad || info.baseAddresses64Bit || info.dieCount!=1 || info.dies[0].id)return Status::UnsupportedLayout;
    for(unsigned n:b.seen)if(n!=1)return Status::UnsupportedLayout;
    Layout candidate={};if(!populate(b,barBytes,candidate))return Status::OutsideBar;
    out=candidate;return Status::Ok;
}
Status checkStopped(const Layout& l,const Io& io,Result& r) {
    r=Result{};r.pipe=0xff;
    if(!io.read32 || !valid(l))return finish(r,Status::InvalidArgument);
    uint32_t ih[kIhRegisters]={},wb[kWbRegisters]={},mall=0,config[kPipes]={},status[kPipes]={};uint8_t offMask=0;
    uint32_t subvp[kPipes]={},vmPage[kPipes]={};
    for(r.pass=0;r.pass<2;++r.pass) {
        if(!guard(l,io,r))return r.status;
        for(size_t i=0;i<kIhRegisters;++i)
            if(!observed(io,l.ih[i],ihFields[i],Status::IhNotStopped,r,ih[i]))return r.status;
        for(size_t i=5;i<=7;i+=2)if((ih[i]&0x3fffc)!=(ih[i+1]&0x3fffc)) {
            r.lastRegister=l.ih[i+1];r.lastValue=ih[i+1];r.mask=0x3fffc;r.expected=ih[i]&0x3fffc;
            return finish(r,Status::IhNotStopped);
        }
        for(size_t i=0;i<kWbRegisters;++i)
            if(!observed(io,l.wb[i],wbFields[i],i<5?Status::PowerUnavailable:Status::WritebackNotStopped,r,wb[i]))return r.status;
        if(!observed(io,l.mall,{0,0x11,0},Status::MallNotStopped,r,mall))return r.status;
        uint8_t nowOff=0;
        for(size_t p=0;p<kPipes;++p) {
            bool off=false;if(!power(l,io,p,off,r))return r.status;
            if(r.pass && off!=bool(offMask&(1U<<p)))return finish(r,Status::PowerUnavailable);
            if(off){nowOff|=uint8_t(1U<<p);continue;}
            if(!observed(io,l.mallConfig[p],{0,7,0},Status::MallNotStopped,r,config[p]))return r.status;
            if(!power(l,io,p,off,r))return r.status;
            if(off)return finish(r,Status::PowerUnavailable);
            // An armed start-line SubVP trigger can look idle before its line.
            // Check configuration, not only its instantaneous activity status.
            if(!observed(io,l.mallSubvp[p],{0,1,0},Status::MallNotStopped,r,subvp[p]))return r.status;
            if(!power(l,io,p,off,r))return r.status;
            if(off)return finish(r,Status::PowerUnavailable);
            if(!observed(io,l.vmPageConfig[p],{0,0x80,0},Status::MallNotStopped,r,vmPage[p]))return r.status;
            if(!power(l,io,p,off,r))return r.status;
            if(off)return finish(r,Status::PowerUnavailable);
            // PREFETCH_COMPLETE can remain latched after MALL becomes unused.
            if(!observed(io,l.mallStatus[p],{0,0xfffdf,0x80000},Status::MallNotStopped,r,status[p]))return r.status;
            if(!power(l,io,p,off,r))return r.status;
            if(off)return finish(r,Status::PowerUnavailable);
        }
        if(r.pass && nowOff!=offMask)return finish(r,Status::PowerUnavailable);
        offMask=nowOff;
        if(!guard(l,io,r))return r.status;
    }
    for(size_t p=0;p<kPipes;++p){bool off=false;if(!power(l,io,p,off,r))return r.status;
        if(off!=bool(offMask&(1U<<p)))return finish(r,Status::PowerUnavailable);}
    if(!guard(l,io,r))return r.status;
    r.poweredOffMask=offMask;r.pipe=0xff;return finish(r,Status::Ok);
}
const char* statusString(Status s){switch(s){
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(DiscoveryRejected);NAME(UnsupportedLayout);NAME(OutsideBar);
    NAME(ReadFailed);NAME(DeviceUnavailable);NAME(PriorDriverRequiresReset);NAME(FirmwareNotStopped);
    NAME(PowerUnavailable);NAME(IhNotStopped);NAME(WritebackNotStopped);NAME(MallNotStopped);NAME(UnstableSnapshot);
#undef NAME
    }return "Unknown";}
} }
