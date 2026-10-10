// SPDX-License-Identifier: MIT
/*
 * Copyright 2018, 2020, 2021 Advanced Micro Devices, Inc.
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
// Pinned Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// dcn32_dpp.c caps, dcn30_dpp.c dpp3_cnv_setup, dcn10_dpp_dscl.c,
// dcn10_mpc.c insert/remove, dcn20_mpc.c blending, dcn30_mpc.c denorm,
// dcn32_mpc.c movable CM bypass, dcn20_opp.c -> dcn10_opp.c FMT.
// HUBP clock/init: dcn20_hwseq.c enable_plane, dcn20_hubp.c hubp2_clk_cntl,
// dcn30_hubp.c hubp3_init (dcn32_hubp.c function table).
// Addresses/masks: dcn_3_2_0_offset.h and dcn_3_2_0_sh_mask.h.
#include "hackynavi/pixel_pipeline.hpp"
namespace hackynavi { namespace pixelpipeline { namespace {
struct Dmu { unsigned count;bool valid;uint64_t pipe,mpc; };
void collect(const DiscoveryIp& ip,void* context){
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid){d.pipe=ip.baseAddresses[2];d.mpc=ip.baseAddresses[3];}
}
struct Address { uint16_t offset,stride;bool mpc; };
// All offsets are DWORDs. DPP stride 0x16b; MPCC 0x15; OPP/FMT 0x5a.
const Address addresses[kRegisterCount]={
    {0x81,2,false},{0x80,2,false},{0x5f3,0xdc,false},{0x5f4,0xdc,false},
    {0x5f9,0xdc,false},{0x1b41,0x80,false},
    {0x1b8a,0x80,false},{0xcc5,0x16b,false},{0xcd0,0x16b,false},
    {0xccf,0x16b,false},{0xced,0x16b,false},{0xcde,0x16b,false},
    {0xcee,0x16b,false},{0xcd7,0x16b,false},{0xcf1,0x16b,false},
    {0xd20,0x16b,false},{0xd0c,0x16b,false},{0xcfd,0x16b,false},
    {0xd11,0x16b,false},{0xd12,0x16b,false},{0xd13,0x16b,false},
    {0xcfb,0x16b,false},{0xd14,0x16b,false},{0xd15,0x16b,false},
    {0xd17,0x16b,false},{0xd18,0x16b,false},
    {0,0x15,true},{1,0x15,true},{2,0x15,true},{3,0x15,true},
    {4,0x15,true},{5,0x15,true},{6,0x15,true},{7,0x15,true},{8,0x15,true},
    {0xe,0x15,true},{0xa8,0x5e,true},{0x453,0x90,true},{0x48a,0x90,true},
    {0x3d8,4,true},{0x3d9,4,true},{0x3da,4,true},{0x3db,4,true},
    {0x3f1,0xd,true},{0x3c6,0,true},
    {0x188c,0x5a,false},{0x1840,0x5a,false},{0x1841,0x5a,false},
    {0x1845,0x5a,false},{0x183f,0x5a,false},{0x1846,0x5a,false},
    {0x1884,0x5a,false},{0x1854,0x5a,false}
};
bool populate(uint64_t pipe,uint64_t mpc,uint64_t bar,Layout& l){
    l={};if(bar<4||pipe>UINT32_MAX/4||mpc>UINT32_MAX/4)return false;
    for(size_t n=0;n<4;++n)for(size_t i=0;i<kRegisterCount;++i){
        const auto& a=addresses[i];const uint64_t v=((a.mpc?mpc:pipe)+a.offset+n*a.stride)*4;
        if(v>UINT32_MAX||v>bar-4)return false;l.registers[n][i]=uint32_t(v);
    }
    l.valid=true;l.barBytes=bar;return true;
}
bool valid(const Layout& l){
    if(!l.valid||(l.registers[0][0]&3)||(l.registers[0][size_t(Register::Top)]&3)||l.registers[0][0]/4<0x81)return false;
    Layout e={};if(!populate(l.registers[0][0]/4-0x81,l.registers[0][size_t(Register::Top)]/4,l.barBytes,e))return false;
    for(size_t n=0;n<4;++n)for(size_t r=0;r<kRegisterCount;++r)if(e.registers[n][r]!=l.registers[n][r])return false;
    return true;
}
bool validPoll(const Poll& p){return p.iterations&&p.iterations<=100000&&p.intervalUs&&
    uint64_t(p.iterations)*p.intervalUs<=100000;}
}
Status plan(const Config& c,Plan& out){
    if(!c.width||c.width>16383||!c.height||c.height>16383||
        (c.outputBpc!=8&&c.outputBpc!=10)||uint32_t(c.format)>uint32_t(Format::Abgr2101010))return Status::InvalidArgument;
    Plan p={};p.size=c.width|(c.height<<16);
    p.surfaceFormat=(c.format==Format::Argb8888||c.format==Format::Abgr8888)?8:10;
    // DPP3 crossbar R=0,G=1,B=2; HUBP performs the ARGB/ABGR swap.
    // Alpha is ignored by this opaque plane; expansion is EXPANSION_MODE_ZERO.
    p.formatControl=0x24000000;
    // Unity scaling needs one line. dcn32 has 970/1290/3430 entries,
    // six pixels per entry; choose first fitting LB config, as Linux does.
    const uint32_t line=(c.width+5)/6;
    p.lbMemory=0x1f00|(line<=970?1u:line<=1290?2u:0u);
    p.denorm=c.outputBpc==8?2:4;p.expansion=c.outputBpc==8?0x11:1;
    out=p;return Status::Ok;
}
Status buildLayout(const uint8_t* b,size_t bytes,uint64_t bar,Layout& l){
    l={};Dmu d={};DiscoveryInfo info={};
    if(parseDiscovery(b,bytes,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.pipe,d.mpc,bar,l)?Status::Ok:Status::OutsideBar;
}
Controller::Controller():layout_{},io_{},pipe_(0),touched_(false),phase_(Phase::Closed),powerStatus_(0),powerConfig_(0),powerValid_(false){}
void Controller::clearAfterVerifiedReset(){layout_={};io_={};pipe_=0;touched_=false;phase_=Phase::Closed;powerStatus_=powerConfig_=0;powerValid_=false;}
Status Controller::finish(Status s,Result& r){
    r.status=s;r.retentionRequired=touched_;if(s!=Status::Ok&&touched_)phase_=Phase::Faulted;return s;
}
Status Controller::rawRead(uint8_t pipe,Register reg,uint32_t& v,Result& r){
    r.lastRegister=layout_.registers[pipe][size_t(reg)];++r.reads;
    if(!io_.read32(io_.context,r.lastRegister,v))return Status::TransportError;
    r.lastValue=v;return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::power(Result& r){
    uint32_t status=0,config=0,after=0;
    auto s=rawRead(pipe_,Register::Power,status,r);if(s!=Status::Ok)return s;
    if(status!=0&&status!=0x10000000)return Status::PowerUnavailable;
    s=rawRead(pipe_,Register::PowerConfig,config,r);if(s!=Status::Ok)return s;
    if(config>1)return Status::PowerUnavailable;
    if(powerValid_&&(status!=powerStatus_||config!=powerConfig_))return Status::PowerUnavailable;
    s=rawRead(pipe_,Register::Power,after,r);if(s!=Status::Ok)return s;
    if(after!=status)return Status::PowerUnavailable;
    s=rawRead(pipe_,Register::PowerConfig,after,r);if(s!=Status::Ok)return s;
    if(after!=config)return Status::PowerUnavailable;
    s=rawRead(pipe_,Register::Power,after,r);if(s!=Status::Ok)return s;
    if(after!=status)return Status::PowerUnavailable;
    powerStatus_=status;powerConfig_=config;powerValid_=true;return Status::Ok;
}
Status Controller::read(uint8_t pipe,Register reg,uint32_t& v,Result& r){
    auto s=power(r);if(s!=Status::Ok)return s;
    s=rawRead(pipe,reg,v,r);return s==Status::Ok?power(r):s;
}
Status Controller::update(Register reg,uint32_t mask,uint32_t bits,Result& r){
    uint32_t old=0;auto s=read(pipe_,reg,old,r);if(s!=Status::Ok)return s;
    uint32_t value=(old&~mask)|(bits&mask);
    // MPC_OUT_RATE_CONTROL_ERROR_ACK is W1C, not an ordinary preserved bit.
    if(reg==Register::OutMux)value&=~0x80u;
    // Clearing TTU_DISABLE must not acknowledge unrelated timeout/underflow.
    if(reg==Register::HubpControl)value&=~0x84000000u;
    touched_=r.writeAttempted=true;++r.writes;r.lastValue=value;
    r.lastRegister=layout_.registers[pipe_][size_t(reg)];
    if(!io_.write32(io_.context,r.lastRegister,value))return Status::TransportError;
    s=read(pipe_,reg,old,r);if(s!=Status::Ok)return s;
    return (old&mask)==(bits&mask)?Status::Ok:Status::VerifyFailed;
}
Status Controller::wait(Register reg,uint32_t mask,uint32_t bits,const Poll& p,Result& r){
    for(uint32_t i=0;i<p.iterations;++i){uint32_t v=0;++r.polls;auto s=read(pipe_,reg,v,r);
        if(s!=Status::Ok)return s;if((v&mask)==bits)return Status::Ok;
        if(i+1<p.iterations)io_.delayUs(io_.context,p.intervalUs);
    }return Status::Timeout;
}
Status Controller::guard(Result& r){
    uint32_t v=0;auto s=read(pipe_,Register::HubpControl,v,r);if(s!=Status::Ok)return s;
    // hubp2_set_blank_regs waits HUBP_NO_OUTSTANDING_REQ, the aggregate
    // drain bit. XRQ_NO_OUTSTANDING_REQ is a separate four-bit field and
    // has no documented acceptance pattern in this path.
    if((v&0xf)!=0xb)return Status::ActivePipe;
    s=read(pipe_,Register::OtgControl,v,r);if(s!=Status::Ok)return s;
    if(v&0x10001)return Status::ActivePipe;
    s=read(pipe_,Register::OtgLock,v,r);if(s!=Status::Ok)return s;
    if(v&0x101)return Status::ForeignControl;
    s=read(pipe_,Register::LockSelect,v,r);if(s!=Status::Ok)return s;
    return v&0x70?Status::ForeignControl:Status::Ok;
}
Status Controller::routes(bool acquired,Result& r){
    const Register selectors[]={Register::Top,Register::Bottom,Register::OppId,Register::OutMux,Register::LockSelect};
    for(uint8_t n=0;n<4;++n){
        for(Register reg:selectors){
            uint32_t v=0;auto s=read(n,reg,v,r);if(s!=Status::Ok)return s;v&=15;
            if(n==pipe_){if(v!=15&&!(acquired&&reg!=Register::Bottom&&v==pipe_))return Status::ForeignControl;}
            else if(v==pipe_)return Status::ForeignControl;
        }
    }
    uint32_t v=0;auto s=read(pipe_,Register::DwbMux,v,r);if(s!=Status::Ok)return s;
    // Both requested and current DWB selectors must exclude our MPCC.
    return (v&15)==pipe_||((v>>4)&15)==pipe_?Status::ForeignControl:Status::Ok;
}
Status Controller::configure(const Layout& l,const Io& io,uint8_t pipe,const Config& c,const Poll& poll,Result& r){
    r={};if(touched_||phase_!=Phase::Closed)return finish(Status::InvalidState,r);
    Plan p={};if(!valid(l)||pipe>=4||!io.read32||!io.write32||!io.delayUs||!validPoll(poll)||plan(c,p)!=Status::Ok)
        return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;pipe_=pipe;powerValid_=false;auto s=guard(r);if(s!=Status::Ok)return finish(s,r);
    s=routes(false,r);if(s!=Status::Ok)return finish(s,r);
    struct Check { Register reg;uint32_t mask; };
    const Check admission[]={{Register::HubpClock,1},{Register::DppControl,0x10},{Register::OppControl,0x13},
        {Register::Stereo,1},{Register::Cursor,1},{Register::Pattern,1}};
    for(const auto& a:admission){uint32_t v=0;s=read(pipe_,a.reg,v,r);if(s!=Status::Ok)return finish(s,r);
        if(v&a.mask)return finish(Status::ForeignControl,r);}
    s=wait(Register::MpccStatus,7,5,poll,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Programming;
    struct Setting { Register reg;uint32_t mask,value; };
    const Setting settings[]={
        // dcn20_enable_plane -> hubp2_clk_cntl: only bit0 is commanded;
        // dynamic clock-gating status bits do not supply an all-on predicate.
        {Register::HubpClock,1,1},
        // hubp3_init writes this WHOLE debug register for DEDCN21-133.
        // Its subsequent hubp_reset only clears Linux's software cursor cache.
        {Register::HubpRequestDebug,UINT32_MAX,1u<<26},{Register::HubpControl,0x1000,0},
        {Register::DppControl,0x10,0x10},{Register::OppControl,0x11,1},
        {Register::SurfaceFormat,0x17f,p.surfaceFormat},
        {Register::FormatControl,0x3f033111,p.formatControl},
        {Register::PreDegamma,0x73,0},{Register::PreDealpha,0x11,0},{Register::PreRealpha,0x11,0},
        {Register::ColorKey,0x31,0},{Register::CmControl,1,1},
        {Register::Autocal,0x3303,0},{Register::DsclControl,1,0},
        {Register::RecoutStart,0x1fff1fff,0},{Register::RecoutSize,0x3fff3fff,p.size},
        {Register::MpcSize,0x3fff3fff,p.size},{Register::ScalerMode,7,0},
        {Register::LbPower,0x03333333,0},{Register::LbFormat,0x11,0},
        {Register::LbMemory,0x3f03,p.lbMemory},
        {Register::MpccControl,0xffff0ff3,0xffff0422}, // top only/global alpha, unity gain
        {Register::Stereo,0x3033f,0},{Register::TopGain,0x7ffff,0x1f000},
        {Register::BottomGainInside,0x7ffff,0x1f000},{Register::BottomGainOutside,0x7ffff,0x1f000},
        {Register::Ogamma,3,0},{Register::Shaper,3,0},{Register::Lut3d,3,0},
        {Register::OutCsc,3,0},{Register::Denorm,0x07ffffff,(p.denorm<<24)|0xfff000},
        {Register::ClampG,0xffffff,0xfff000},{Register::ClampB,0xffffff,0xfff000},
        {Register::OutMux,0x7fff00,0x100},
        {Register::FmtControl,0x3f0001,0},{Register::FmtDepth,0x0001ff33,0},
        {Register::FmtClamp,0x70001,1},{Register::FmtExpansion,0x11,p.expansion},
        {Register::FmtStereo,0x1fff,0},{Register::OppBuffer,0x0ff73fff,0},
        {Register::LockSelect,15,pipe},{Register::Bottom,15,15}
    };
    for(const auto& a:settings){s=update(a.reg,a.mask,a.value,r);if(s!=Status::Ok)return finish(s,r);}
    s=wait(Register::LbStatus,3,0,poll,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::OppControl,2,2,poll,r);if(s!=Status::Ok)return finish(s,r);
    // Connect last, after the pipeline registers have read back successfully.
    s=update(Register::Top,15,pipe,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::OppId,15,pipe,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::OutMux,15,pipe,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Configured;return finish(Status::Ok,r);
}
Status Controller::stop(const Poll& p,Result& r){
    r={};if(!touched_)return finish(Status::InvalidState,r);
    if(!validPoll(p))return finish(Status::InvalidArgument,r);
    auto s=guard(r);if(s!=Status::Ok)return finish(s,r);
    s=routes(true,r);if(s!=Status::Ok)return finish(s,r);phase_=Phase::Stopping;
    // mpc1_remove_mpcc releases the update-lock selector with the route.
    const Register selectors[]={Register::OutMux,Register::Top,Register::Bottom,Register::OppId,Register::LockSelect};
    for(Register reg:selectors){
        s=update(reg,15,15,r);if(s!=Status::Ok)return finish(s,r);}
    s=routes(false,r);if(s!=Status::Ok)return finish(s,r);r.disconnected=true;
    s=wait(Register::MpccStatus,7,5,p,r);if(s!=Status::Ok)return finish(s,r);r.idle=true;
    // dcn20_plane_atomic_disable: disconnect first, then HUBP clock, then DPP.
    s=update(Register::HubpClock,1,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::DppControl,0x10,0,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::OppControl,1,0,r);if(s!=Status::Ok)return finish(s,r);
    s=wait(Register::OppControl,3,0,p,r);if(s!=Status::Ok)return finish(s,r);
    clearAfterVerifiedReset();return finish(Status::Ok,r);
}
} }
