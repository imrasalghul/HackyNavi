// SPDX-License-Identifier: MIT
/*
 * Copyright 2012-2022 Advanced Micro Devices, Inc.
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
// Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// hwss/dcn32/dcn32_init.c and dcn32_hwseq.c (DOMAIN0..3); hwss/dcn20/
// dcn20_hwseq.c:power_on_plane_resources/plane_atomic_disable (request and
// clock ordering); hwss/dcn10/dcn10_hwseq.c:plane_atomic_power_down;
// hubp/dcn32/dcn32_hubp.c:phantom_hubp_post_enable (gated reads return zero);
// mpc/dcn10/dcn10_mpc.c and generated DCN3.2 offsets/masks.
#include "hackynavi/display_power.hpp"
namespace hackynavi { namespace displaypower { namespace {
struct Dmu { unsigned count;bool valid;uint64_t clock,pipe,mpc; };
void collect(const DiscoveryIp& ip,void* context){
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid){d.clock=ip.baseAddresses[1];d.pipe=ip.baseAddresses[2];d.mpc=ip.baseAddresses[3];}
}
bool shared(size_t reg){return reg==size_t(Register::RequestControl)||reg==size_t(Register::DtoControl);}
bool populate(uint64_t clock,uint64_t pipe,uint64_t mpc,uint64_t bar,Layout& out){
    out={};if(bar<4||clock>UINT32_MAX/4||pipe>UINT32_MAX/4||mpc>UINT32_MAX/4)return false;
    Layout l={};l.barBytes=bar;
    for(size_t n=0;n<kPipeCount;++n){
        const uint64_t regs[]={pipe+0x80+n*2,pipe+0x81+n*2,pipe+0x95,clock+0xb6,
            pipe+0x1b41+n*0x80,pipe+0x1b85+n*0x80,pipe+0x5f3+n*0xdc,
            pipe+0x5f4+n*0xdc,pipe+0xcc5+n*0x16b,pipe+0x678+n*0xdc,pipe+0x684+n*0xdc,
            mpc+n*0x15,mpc+1+n*0x15,mpc+2+n*0x15,mpc+0x0e + n*0x15,mpc+0x3d8+n*4,mpc+5+n*0x15};
        for(size_t r=0;r<kRegisterCount;++r){
            const uint64_t a=regs[r]*4;if(a>UINT32_MAX||a>bar-4)return false;
            l.registers[n][r]=uint32_t(a);
            for(size_t p=0;p<=n;++p)for(size_t j=0;j<(p==n?r:kRegisterCount);++j)
                if(l.registers[p][j]==a&&!(p!=n&&j==r&&shared(r)))return false;
        }
    }
    l.valid=true;out=l;return true;
}
bool valid(const Layout& l){
    const auto cfg=size_t(Register::PowerConfig),dto=size_t(Register::DtoControl),top=size_t(Register::MpcTop);
    if(!l.valid||(l.registers[0][cfg]&3)||(l.registers[0][dto]&3)||(l.registers[0][top]&3)||
        l.registers[0][cfg]/4<0x80||l.registers[0][dto]/4<0xb6)return false;
    Layout e={};if(!populate(l.registers[0][dto]/4-0xb6,l.registers[0][cfg]/4-0x80,
        l.registers[0][top]/4,l.barBytes,e))return false;
    for(size_t p=0;p<kPipeCount;++p)for(size_t r=0;r<kRegisterCount;++r)
        if(l.registers[p][r]!=e.registers[p][r])return false;
    return true;
}
bool validPoll(const Poll& p){return p.iterations&&p.intervalUs&&p.intervalUs<=1000&&
    uint64_t(p.iterations)*p.intervalUs<=100000;}
bool currentOn(uint32_t status){return status==0 || status==0x10000000;}
bool settled(uint32_t cfg,uint32_t status){return (currentOn(status)&&(cfg==0||cfg==1))||
    ((status==0x80000000||status==0x90000000)&&cfg==0x100);}
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t bar,Layout& out){
    out={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(bytes,count,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.clock,d.pipe,d.mpc,bar,out)?Status::Ok:Status::OutsideBar;
}
Controller::Controller():layout_{},io_{},pipe_(0),phase_(Phase::Fresh),owned_(false),touched_(false){}
void Controller::clearAfterVerifiedReset(){layout_={};io_={};pipe_=0;phase_=Phase::Fresh;owned_=touched_=false;}
Status Controller::finish(Status s,Result& r){r.status=s;r.retentionRequired=requiresHardwareRetirement();return s;}
Status Controller::fail(Status s,Result& r){if(owned_||touched_)phase_=Phase::Faulted;return finish(s,r);}
Status Controller::read(uint8_t pipe,Register reg,uint32_t& v,Result& r){
    r.lastRegister=layout_.registers[pipe][size_t(reg)];++r.reads;
    if(!io_.read32(io_.context,r.lastRegister,v))return Status::TransportError;
    r.lastValue=v;return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::write(Register reg,uint32_t value,Result& r){
    r.lastRegister=layout_.registers[pipe_][size_t(reg)];r.lastValue=value;++r.writes;
    touched_=r.writeAttempted=true;
    return io_.write32(io_.context,r.lastRegister,value)?Status::Ok:Status::TransportError;
}
Status Controller::observePower(uint32_t& cfg,uint32_t& power,Result& r){
    uint32_t after=0;auto s=read(pipe_,Register::PowerStatus,power,r);if(s!=Status::Ok)return s;
    s=read(pipe_,Register::PowerConfig,cfg,r);if(s!=Status::Ok)return s;
    s=read(pipe_,Register::PowerStatus,after,r);if(s!=Status::Ok)return s;
    if(power!=after)return Status::PowerChanged;
    return settled(cfg,power)?Status::Ok:Status::UnsettledPower;
}
Status Controller::samePower(uint32_t cfg,uint32_t power,Result& r){
    uint32_t nowCfg=0,nowPower=0;auto s=observePower(nowCfg,nowPower,r);if(s!=Status::Ok)return s;
    return cfg==nowCfg&&power==nowPower?Status::Ok:Status::PowerChanged;
}
Status Controller::idle(bool hubpOn,Result& r){
    uint32_t v=0;
    const Register regs[]={Register::OtgControl,Register::OtgClock,Register::DtoControl,
        Register::MpcTop,Register::MpcBottom,Register::MpcOpp,Register::MpcStatus,Register::MpcOutput,Register::MpcLock};
    const uint32_t masks[]={0x10001,0x10000,3u<<(pipe_*4),0xf,0xf,0xf,3,0xf,0x7f};
    const uint32_t wanted[]={0,0,0,0xf,0xf,0xf,1,0xf,0xf};
    const Status failures[]={Status::ActiveTiming,Status::ActiveTiming,Status::ActiveClock,
        Status::ConnectedCombiner,Status::ConnectedCombiner,Status::ConnectedCombiner,Status::BusyCombiner,
        Status::ConnectedCombiner,Status::ForeignControl};
    for(size_t i=0;i<sizeof(regs)/sizeof(regs[0]);++i){
        const auto s=read(pipe_,regs[i],v,r);if(s!=Status::Ok)return s;
        if((v&masks[i])!=wanted[i])return failures[i];
    }
    // Other active routes are allowed, but none may reference this DPP/MPCC.
    const Register selectors[]={Register::MpcTop,Register::MpcBottom,Register::MpcOutput};
    for(uint8_t p=0;p<kPipeCount;++p)if(p!=pipe_){
        for(Register reg:selectors){
            const auto s=read(p,reg,v,r);if(s!=Status::Ok)return s;
            const uint32_t sel=v&0xf;if(sel==pipe_)return Status::ConnectedCombiner;
            if(sel>=kPipeCount&&sel!=0xf)return Status::ForeignControl;
        }
        const auto s=read(p,Register::MpcLock,v,r);if(s!=Status::Ok)return s;
        if(v&0x70)return Status::ForeignControl;
    }
    if(hubpOn){
        // Do not assume DPP register accessibility while HUBP is gated merely
        // because DCN3.2 has no separate DPP PG callback.
        const Register hr[]={Register::HubpControl,Register::HubpClock,Register::DppControl,
            Register::CursorControl,Register::DmdataControl};
        const uint32_t hm[]={0xf,1,0x10,1,7},hv[]={0xb,0,0,0,0};
        for(size_t i=0;i<5;++i){const auto s=read(pipe_,hr[i],v,r);if(s!=Status::Ok)return s;
            if((v&hm[i])!=hv[i])return i==1||i==2?Status::ActiveClock:Status::OutstandingFetch;}
    }
    return Status::Ok;
}
Status Controller::request(bool original,bool enable,Result& r){
    uint32_t value=0;auto s=read(pipe_,Register::RequestControl,value,r);if(s!=Status::Ok)return s;
    if(bool(value&1)!=original)return Status::ForeignControl;
    const uint32_t wanted=(value&~1u)|uint32_t(enable);
    if(wanted!=value){s=write(Register::RequestControl,wanted,r);if(s!=Status::Ok)return s;}
    s=read(pipe_,Register::RequestControl,value,r);if(s!=Status::Ok)return s;
    return value==wanted?Status::Ok:Status::VerifyFailed;
}
Status Controller::transition(bool on,uint32_t cfg,uint32_t power,const Poll& poll,Result& r){
    uint32_t value=0;auto s=read(pipe_,Register::RequestControl,value,r);if(s!=Status::Ok)return s;
    const bool originalRequest=bool(value&1);
    r.stage=Stage::EnableRequest;s=request(originalRequest,true,r);if(s!=Status::Ok)return s;
    s=samePower(cfg,power,r);if(s!=Status::Ok)return s;
    s=idle(currentOn(power),r);if(s!=Status::Ok)return s;
    r.stage=Stage::GateCommand;
    s=read(pipe_,Register::RequestControl,value,r);if(s!=Status::Ok)return s;
    if(!(value&1))return Status::ForeignControl;
    const uint32_t wantedCfg=on?0:0x100;
    // REG_SET in dcn32_hubp_pg_control clears FORCEON and sets only GATE.
    s=write(Register::PowerConfig,wantedCfg,r);if(s!=Status::Ok)return s;
    r.stage=Stage::WaitPower;bool done=false;uint32_t completedPower=0;
    for(uint32_t i=0;i<poll.iterations;++i){
        ++r.polls;s=read(pipe_,Register::RequestControl,value,r);if(s!=Status::Ok)return s;
        if(!(value&1))return Status::ForeignControl;
        s=read(pipe_,Register::PowerConfig,value,r);if(s!=Status::Ok)return s;
        if(value!=wantedCfg)return Status::VerifyFailed;
        s=read(pipe_,Register::PowerStatus,value,r);if(s!=Status::Ok)return s;
        if(value&~0xd0000000u)return Status::UnsettledPower;
        // Linux polls only the current PGFSM field (ON=0/OFF=2). Desired
        // state bit28 may remain set on an ON domain. Capture the actual raw
        // completion value so subsequent bracketing still detects changes.
        if(settled(wantedCfg,value) && currentOn(value)==on){completedPower=value;done=true;break;}
        if(i+1<poll.iterations)io_.delayUs(io_.context,poll.intervalUs);
    }
    if(!done)return Status::Timeout;
    s=samePower(wantedCfg,completedPower,r);if(s!=Status::Ok)return s;
    if(on){
        // A powered-off read could have returned zero regardless of the live
        // value. Verify actual stopped clocks only after the domain is on.
        s=read(pipe_,Register::DppControl,value,r);if(s!=Status::Ok)return s;
        if(value&0x10)return Status::ActiveClock;
        s=read(pipe_,Register::HubpClock,value,r);if(s!=Status::Ok)return s;
        if(value&1)return Status::ActiveClock;
    }
    r.stage=Stage::RestoreRequest;s=request(true,originalRequest,r);if(s!=Status::Ok)return s;
    r.stage=Stage::FinalVerify;s=samePower(wantedCfg,completedPower,r);if(s!=Status::Ok)return s;
    r.powerVerified=true;r.powered=on;return Status::Ok;
}
Status Controller::powerOn(const Layout& l,const Io& io,uint8_t pipe,const Poll& poll,Result& r){
    r={};if(phase_!=Phase::Fresh&&phase_!=Phase::Off)return finish(Status::InvalidState,r);
    if(!valid(l)||pipe>=kPipeCount||!io.read32||!io.write32||!io.delayUs||!validPoll(poll))
        return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;pipe_=pipe;r.stage=Stage::Admission;
    uint32_t cfg=0,power=0;auto s=observePower(cfg,power,r);if(s!=Status::Ok)return fail(s,r);
    s=idle(currentOn(power),r);if(s!=Status::Ok)return fail(s,r);
    s=samePower(cfg,power,r);if(s!=Status::Ok)return fail(s,r);
    if(!currentOn(power)){s=transition(true,cfg,power,poll,r);if(s!=Status::Ok)return fail(s,r);}
    else{r.stage=Stage::FinalVerify;r.powerVerified=true;r.powered=true;}
    owned_=true;phase_=Phase::Powered;return finish(Status::Ok,r);
}
Status Controller::powerOff(const Poll& poll,Result& r){
    r={};if(phase_!=Phase::Powered)return finish(Status::InvalidState,r);
    if(!validPoll(poll))return finish(Status::InvalidArgument,r);
    r.stage=Stage::Admission;uint32_t cfg=0,power=0;auto s=observePower(cfg,power,r);
    if(s!=Status::Ok)return fail(s,r);
    if(!currentOn(power))return fail(Status::PowerChanged,r);
    s=idle(true,r);
    if(s!=Status::Ok){
        if(s==Status::TransportError||s==Status::DeviceUnavailable)return fail(s,r);
        return finish(s,r); // Caller can finish its ordinary stop sequence.
    }
    s=samePower(cfg,power,r);if(s!=Status::Ok)return fail(s,r);
    s=transition(false,cfg,power,poll,r);if(s!=Status::Ok)return fail(s,r);
    owned_=touched_=false;phase_=Phase::Off;return finish(Status::Ok,r);
}
} }
