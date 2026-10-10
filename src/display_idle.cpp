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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

// DCN3.2 generated register definitions; disconnected MPC selectors and idle
// checks follow Linux dcn10_mpc.c (mpc1_mpc_init / mpc1_assert_mpcc_idle_before_connect),
// revision 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0. Read-only admission, no reset.
// dcn32_hubp.c hubp32_phantom_hubp_post_enable: gated HUBP control may read
// zero, so its live drain bits cannot establish idleness. dcn32_hwseq.c uses
// DOMAIN0..3 PGFSM 0 for on and 2 for off. Bit28 is desired state, not the
// current PGFSM field; retain it for raw stability, not ON/OFF classification.
#include "hackynavi/display_idle.hpp"
namespace hackynavi { namespace displayidle { namespace {
struct Dmu { unsigned count;bool valid;uint64_t pipe,mpc; };
void collect(const DiscoveryIp& ip,void* context){
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid){d.pipe=ip.baseAddresses[2];d.mpc=ip.baseAddresses[3];}
}
bool populate(uint64_t pipe,uint64_t mpc,uint64_t bar,Layout& l){
    l={};if(bar<4||pipe>UINT32_MAX/4||mpc>UINT32_MAX/4)return false;
    for(size_t n=0;n<kPipeCount;++n){
        const uint64_t regs[]={pipe+0x1b41+n*0x80,pipe+0x1b85+n*0x80,pipe+0x5f3+n*0xdc,
            mpc+n*0x15,mpc+1+n*0x15,mpc+2+n*0x15,mpc+0x0e + n*0x15,mpc+0x3d8+n*4,mpc+5+n*0x15,
            pipe+0x80+n*2,pipe+0x81+n*2};
        for(size_t r=0;r<kRegisterCount;++r){const uint64_t a=regs[r]*4;
            if(a>UINT32_MAX||a>bar-4)return false;
            l.registers[n][r]=uint32_t(a);}
    }
    // These controls are physically distinct. A forged discovery base must
    // not let one compatible register value satisfy two independent checks.
    for(size_t p=0;p<kPipeCount;++p)for(size_t r=0;r<kRegisterCount;++r)
        for(size_t q=0;q<=p;++q)for(size_t s=0;s<kRegisterCount;++s){
            if(q==p&&s>=r)break;
            if(l.registers[p][r]==l.registers[q][s]){l={};return false;}
        }
    l.valid=true;l.barBytes=bar;return true;
}
bool valid(const Layout& l){
    if(!l.valid||(l.registers[0][0]&3)||(l.registers[0][3]&3)||l.registers[0][0]/4<0x1b41)return false;
    Layout expected={};if(!populate(l.registers[0][0]/4-0x1b41,l.registers[0][3]/4,l.barBytes,expected))return false;
    for(size_t n=0;n<kPipeCount;++n)for(size_t r=0;r<kRegisterCount;++r)
        if(l.registers[n][r]!=expected.registers[n][r])return false;
    return true;
}
Status finish(Status s,Result& r){r.status=s;return s;}
Status read(const Layout& l,const Io& io,size_t pipe,Register reg,uint32_t& value,Result& out){
    out.pipe=uint8_t(pipe);out.reg=reg;out.lastRegister=l.registers[pipe][size_t(reg)];++out.reads;
    if(!io.read32(io.context,out.lastRegister,out.lastValue))return Status::ReadFailed;
    if(out.lastValue==UINT32_MAX)return Status::DeviceUnavailable;
    value=out.lastValue;return Status::Ok;
}
Status power(const Layout& l,const Io& io,size_t pipe,uint32_t& state,uint32_t& config,Result& out){
    auto s=read(l,io,pipe,Register::HubpPowerStatus,state,out);if(s!=Status::Ok)return s;
    // Refuse reserved bits as well as intermediate PGFSM states. The off
    // request must agree with the settled state; FORCEON+GATE is ambiguous.
    if(state!=0 && state!=0x10000000 && state!=0x80000000 && state!=0x90000000)return Status::PowerUnavailable;
    s=read(l,io,pipe,Register::HubpPowerConfig,config,out);if(s!=Status::Ok)return s;
    if((state&0xc0000000)==0x80000000?(config!=0x100):(config>1))return Status::PowerUnavailable;
    uint32_t after=0;s=read(l,io,pipe,Register::HubpPowerStatus,after,out);
    if(s!=Status::Ok)return s;
    return after==state?Status::Ok:Status::PowerChanged;
}
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t bar,Layout& out){
    out={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(bytes,count,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.pipe,d.mpc,bar,out)?Status::Ok:Status::OutsideBar;
}
Status verify(const Layout& l,const Io& io,Result& out){
    out={};if(!valid(l)||!io.read32)return finish(Status::InvalidArgument,out);
    // Use the aggregate HUBP drain indication, not a guessed value for its
    // four-bit XRQ field. Require soft reset clear throughout admission.
    const uint32_t masks[]={0x10001,0x10000,0xf,0xf,0xf,0xf,3,0xf,0x7f};
    const uint32_t wanted[]={0,0,0xb,0xf,0xf,0xf,1,0xf,0xf};
    const Status failures[]={Status::ActiveTiming,Status::ActiveTiming,Status::OutstandingFetch,
        Status::ConnectedCombiner,Status::ConnectedCombiner,Status::ConnectedCombiner,Status::BusyCombiner,
        Status::ConnectedCombiner,Status::ForeignLock};
    uint32_t states[kPipeCount]={},configs[kPipeCount]={};
    for(size_t n=0;n<kPipeCount;++n){
        auto status=power(l,io,n,states[n],configs[n],out);if(status!=Status::Ok)return finish(status,out);
        const bool off=(states[n]&0xc0000000)==0x80000000;
        if(off)out.poweredOffMask|=uint8_t(1u<<n);
        for(size_t r=0;r<size_t(Register::HubpPowerConfig);++r){
            if(Register(r)==Register::HubpControl && off)continue;
            uint32_t value=0;status=read(l,io,n,Register(r),value,out);
            if(status!=Status::Ok)return finish(status,out);
            if((value&masks[r])!=wanted[r])return finish(failures[r],out);
        }
        uint32_t state=0,config=0;status=power(l,io,n,state,config,out);
        if(status!=Status::Ok)return finish(status,out);
        if(state!=states[n]||config!=configs[n])return finish(Status::PowerChanged,out);
    }
    // Recheck earlier domains after later pipe reads. This detects observed
    // changes, but does not substitute for caller-held firmware/PM exclusion.
    for(size_t n=0;n<kPipeCount;++n){
        uint32_t state=0,config=0;const auto status=power(l,io,n,state,config,out);
        if(status!=Status::Ok)return finish(status,out);
        if(state!=states[n]||config!=configs[n])return finish(Status::PowerChanged,out);
    }
    return finish(Status::Ok,out);
}
} }
