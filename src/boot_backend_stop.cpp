// SPDX-License-Identifier: MIT
// DCN3.2 terminal subtransaction, derived from Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0 dcn10_mpc.c,
// dcn10_hwseq.c, dcn20_opp.c, dcn32_optc.c and generated register tables.
// Copyright 2012-2022 Advanced Micro Devices, Inc.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
#include "hackynavi/boot_backend_stop.hpp"
namespace hackynavi { namespace bootbackendstop { namespace {
using Bank=bootbackend::Bank;
const uint16_t linkOffsets[]={0x1f04,0x20b1,0x20b2};
const uint16_t frontendOffsets[]={0x208b,0x2091,0x210c,0x2094,0x2098,0x209e};
const uint16_t timingOffsets[]={0x1b41,0x1b85,0x1b8a,0x1b91,0x1b8b,0x1b54,0x1b82,0x1b44,0x1aca,0x1acb,0x1acc,0x1ad0};
const uint16_t combinerOffsets[]={0,1,2,5,0xe,0x3d8};
struct Dmu { unsigned count;bool valid;uint64_t pipe,mpc; };
void collect(const DiscoveryIp& ip,void* p) {
    if(ip.hardwareId!=271)return;auto& d=*static_cast<Dmu*>(p);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid){d.pipe=ip.baseAddresses[2];d.mpc=ip.baseAddresses[3];}
}
size_t count(Bank b){return b==Bank::Link?3:b==Bank::Frontend?6:b==Bank::Timing?12:6;}
size_t instances(Bank b){return b==Bank::Link||b==Bank::Frontend?5:4;}
bool pollValid(const Poll& p){return p.iterations&&p.intervalUs&&p.intervalUs<=100&&uint64_t(p.iterations)*p.intervalUs<=150000;}
}
Controller::Controller():phase_(Phase::Fresh),io_{},drain_(nullptr),initial_{},expected_{},hubp_{},
    pipeBase_(0),mpcBase_(0),vtgAddress_(0),vtg_(0),dpgAddress_(0),dpgStatusAddress_(0),dpg_(0),dpgStatus_(0),attempted_(0),acknowledged_(0){}
Status Controller::finish(Status s,Result& r){r.status=s;r.attempted=attempted_;r.acknowledged=acknowledged_;r.retentionRequired=retentionRequired();return s;}
Status Controller::fail(Status s,Result& r){phase_=Phase::Faulted;return finish(s,r);}
uint32_t Controller::address(Bank b,uint8_t n,uint8_t r) const {
    if(b==Bank::Link)return uint32_t((pipeBase_+linkOffsets[r]+n*(r?0x100u:1u))*4);
    if(b==Bank::Frontend)return uint32_t((pipeBase_+frontendOffsets[r]+n*0x100u)*4);
    if(b==Bank::Timing)return uint32_t((pipeBase_+timingOffsets[r]+n*(r<8?0x80u:0x10u))*4);
    return uint32_t((mpcBase_+combinerOffsets[r]+n*(r==5?4u:0x15u))*4);
}
uint32_t& Controller::expected(Bank b,uint8_t n,uint8_t r){
    return b==Bank::Link?expected_.link[n][r]:b==Bank::Frontend?expected_.frontend[n][r]:
        b==Bank::Timing?expected_.timing[n][r]:expected_.combiner[n][r];
}
bool Controller::read(Domain d,uint8_t n,uint32_t a,uint32_t& value,Result& r){
    r.lastRegister=a;r.lastValue=0;r.lastReadSucceeded=false;
    auto& b=io_.backend;
    if(!b.valid(b.context)||!io_.permit(b.context,d,n,a,false)||!b.valid(b.context)){r.status=Status::AccessDenied;return false;}
    ++r.reads;if(!b.read32(b.context,a,value)){r.status=Status::ReadFailed;return false;}
    r.lastReadSucceeded=true;r.lastValue=value;
    if(!b.valid(b.context)||!io_.permit(b.context,d,n,a,false)||!b.valid(b.context)){r.status=Status::AccessDenied;return false;}
    if(value==UINT32_MAX){r.status=Status::DeviceUnavailable;return false;}return true;
}
bool Controller::power(uint8_t p,Result& r){
    const auto& snap=drain_->capture().snapshot;
    const uint32_t regs[]={hubp_.power[p],hubp_.powerConfig[p],hubp_.power[p]};
    const uint32_t wanted[]={snap.power[p],snap.powerConfig[p],snap.power[p]};
    for(unsigned i=0;i<3;++i){uint32_t v=0;if(!read(Domain::HubpPower,p,regs[i],v,r))return false;
        if(v!=wanted[i]){r.status=Status::Changed;return false;}}
    return true;
}
bool Controller::drained(Result& r){
    if(!drain_||drain_->phase()!=bootdisplaystop::Phase::Stopped||!drain_->capture().complete){r.status=Status::DrainRequired;return false;}
    const auto& c=drain_->capture();
    for(size_t i=0;i<bootdisplay::kVgaCount;++i){uint32_t v=0;
        if(!read(Domain::Global,0,hubp_.vga[i],v,r))return false;
        if(v!=c.snapshot.vga[i]){r.status=Status::Changed;return false;}}
    for(size_t i=0;i<bootdisplay::kGlobalRegisterCount;++i){uint32_t v=0;
        if(!read(Domain::Global,0,hubp_.global[i],v,r))return false;
        if(v!=c.snapshot.global[i]){r.status=Status::Changed;return false;}}
    // The entire actual captured reader set, not a guessed HUBP->DPP route.
    for(uint8_t p=0;p<4;++p){
        if(!power(p,r))return false;if(c.poweredOffMask&(1u<<p))continue;
        uint32_t v=0;
        for(size_t reg=0;reg<bootdisplay::kPipeRegisterCount;++reg){
            if(!power(p,r)||!read(Domain::Hubp,p,hubp_.pipe[p][reg],v,r))return false;
            const uint32_t initial=c.snapshot.pipe[p][reg];
            const uint32_t want=reg==0&&!(initial&1)?(initial&~0x84001000u)|1:initial;
            const uint32_t mask=reg==0?~0x70ff000au:UINT32_MAX;
            if(((v^want)&mask)||(reg==0&&(v&3)!=3)){r.status=Status::DrainRequired;return false;}
        }
        // Read clocks only inside the same stable ON domain. Never OFF windows.
        if(!power(p,r)||!read(Domain::Hubp,p,hubp_.pipe[p][0]+4,v,r))return false;
        if(!(v&1)){r.status=Status::DrainRequired;return false;}
        if(!power(p,r))return false;
    }
    return true;
}
bool Controller::guard(bool external,bool transition,Result& r){
    if(!drained(r))return false;
    const auto t=initial_.timingIndex,m=initial_.mpcc,f=initial_.frontendIndex,be=initial_.backend;
    for(unsigned bank=0;bank<4;++bank)for(uint8_t n=0;n<instances(Bank(bank));++n)for(uint8_t reg=0;reg<count(Bank(bank));++reg){
        const auto b=Bank(bank);uint32_t v=0;if(!read(Domain::Backend,n,address(b,n,reg),v,r))return false;
        uint32_t want=expected(b,n,reg),mask=UINT32_MAX;
        if(b==Bank::Combiner&&n==m&&reg==4&&attempted_){
            // Status is a live completion, not the double-buffered mux image.
            if(v!=1&&v!=2&&v!=5){r.status=Status::Changed;return false;}
            if((phase_==Phase::MpcDetached||phase_==Phase::TimingStopped)&&v!=1&&v!=5){r.status=Status::Changed;return false;}continue;
        }
        if(external){
            if(b==Bank::Link&&n==be&&reg==1)want=initial_.link[be][1]&~0x7f00u;
            if(b==Bank::Link&&n==be&&reg==2)want=0;
            // Concrete DCN3.2 has no disable_fifo callback. Its input-mode
            // reset preserves FIFO_ENABLE and only sets OUTPUT_PIXEL_MODE=0
            // (already zero here). DIG_SYMCLK_FE_ON is read-only status;
            // neither value proves PHY/clock ownership after transmitter stop.
            if(b==Bank::Frontend&&n==f&&reg==0)mask&=~0x01000000u;
            if(b==Bank::Frontend&&n==f&&reg==3)want=(initial_.frontend[f][3]&~0x01000006u)|0x19u;
            if(b==Bank::Frontend&&n==f&&reg==5)want=1;
        }
        if(transition&&b==Bank::Timing&&n==t){if(reg==0)mask&=~0x10000u;if(reg==1)mask&=~0x10000u;}
        if((v^want)&mask){r.status=external&&(b==Bank::Link||b==Bank::Frontend)?Status::LinkNotStopped:Status::Changed;return false;}
    }
    uint32_t v=0;if(!read(Domain::Vtg,t,vtgAddress_,v,r))return false;
    if(v!=vtg_){r.status=Status::Changed;return false;}
    if(!transition&&phase_!=Phase::TimingStopped&&(!(vtg_&0x80000000u)||(expected_.timing[t][0]&0x10001u)!=0x10001u||!(expected_.timing[t][1]&0x100))){r.status=Status::TimingNotRunning;return false;}
    if(!read(Domain::Dpg,initial_.opp,dpgAddress_,v,r))return false;
    if(v!=dpg_){r.status=Status::Changed;return false;}
    if(!read(Domain::Dpg,initial_.opp,dpgStatusAddress_,v,r))return false;
    if(v!=dpgStatus_){r.status=Status::Changed;return false;}
    return drained(r);
}
Status Controller::capture(const uint8_t* discovery,size_t bytes,uint64_t bar,const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io& io,
    const bootdisplaystop::State& drain,Result& r){
    r={};if(phase_!=Phase::Fresh)return finish(Status::InvalidState,r);
    if(!io.backend.valid||!io.backend.canRead||!io.backend.read32||!io.permit||!io.write32||!io.delayUs)return finish(Status::InvalidArgument,r);
    if(drain.phase()!=bootdisplaystop::Phase::Stopped||!drain.capture().complete)return finish(Status::DrainRequired,r);
    DiscoveryInfo info{};Dmu d{};
    if(parseDiscovery(discovery,bytes,info,collect,&d)!=DiscoveryStatus::Ok||d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id||
        d.pipe>UINT32_MAX/4||d.mpc>UINT32_MAX/4||bar<4)return finish(Status::DiscoveryRejected,r);
    if(bootdisplay::buildLayout(discovery,bytes,bar,hubp_)!=bootdisplay::Status::Ok)return finish(Status::DiscoveryRejected,r);
    io_=io;drain_=&drain;pipeBase_=d.pipe;mpcBase_=d.mpc;
    // Every hardware failure after this admission seals this one-shot owner.
    phase_=Phase::Faulted;
    const auto captured=bootbackend::capture(discovery,bytes,bar,rom,romBytes,imageOffset,io.backend,initial_,r.backend);
    r.reads=r.backend.reads;r.lastRegister=r.backend.lastRegister;r.lastValue=r.backend.lastValue;r.lastReadSucceeded=r.backend.lastReadSucceeded;
    if(captured!=bootbackend::Status::Ok)return finish(Status::CaptureRejected,r);
    expected_=initial_;
    const uint64_t extras[]={(d.pipe+0x52e +initial_.timingIndex)*4,
        (d.pipe+0x1854+initial_.opp*0x5a)*4,(d.pipe+0x185b+initial_.opp*0x5a)*4};
    for(uint64_t a:extras){if(a>UINT32_MAX||a>bar-4)return finish(Status::DiscoveryRejected,r);
        for(unsigned b=0;b<4;++b)for(uint8_t n=0;n<instances(Bank(b));++n)for(uint8_t q=0;q<count(Bank(b));++q)
            if(a==address(Bank(b),n,q))return finish(Status::DiscoveryRejected,r);}
    vtgAddress_=uint32_t(extras[0]);dpgAddress_=uint32_t(extras[1]);dpgStatusAddress_=uint32_t(extras[2]);
    if(!read(Domain::Vtg,initial_.timingIndex,vtgAddress_,vtg_,r)||!read(Domain::Dpg,initial_.opp,dpgAddress_,dpg_,r)||
        !read(Domain::Dpg,initial_.opp,dpgStatusAddress_,dpgStatus_,r))return finish(r.status,r);
    if(!(vtg_&0x80000000u)||(vtg_&0x8000u))return finish(Status::TimingNotRunning,r);
    if(!(dpg_&1)||(dpg_&~0x01ff3171u)||dpgStatus_)return finish(Status::OPPNotBlank,r);
    if(!guard(false,false,r))return finish(r.status,r);
    phase_=Phase::Armed;return finish(Status::Ok,r);
}
bool Controller::write(Bank b,uint8_t n,uint8_t reg,uint32_t value,uint32_t readbackMask,uint16_t bit,Result& r){
    uint32_t v=0;const uint32_t a=address(b,n,reg);
    if(!read(Domain::Backend,n,a,v,r))return false;
    if((v^expected(b,n,reg))&readbackMask){r.status=Status::Changed;return false;}
    auto& io=io_.backend;
    if(!io.valid(io.context)||!io_.permit(io.context,Domain::Backend,n,a,true)||!io.valid(io.context)){r.status=Status::AccessDenied;return false;}
    attempted_|=bit;++r.writes;r.lastRegister=a;r.lastValue=value;r.lastReadSucceeded=false;
    if(!io_.write32(io.context,a,value)){r.status=Status::WriteFailed;return false;}
    if(!io.valid(io.context)||!io_.permit(io.context,Domain::Backend,n,a,true)||!io.valid(io.context)){r.status=Status::AccessDenied;return false;}
    if(!read(Domain::Backend,n,a,v,r))return false;
    if((v^value)&readbackMask){r.status=Status::Changed;return false;}
    expected(b,n,reg)=value;acknowledged_|=bit;return true;
}
bool Controller::writeVtg(uint32_t value,uint16_t bit,Result& r){
    auto& io=io_.backend;uint32_t v=0;
    if(!read(Domain::Vtg,initial_.timingIndex,vtgAddress_,v,r))return false;
    if(v!=vtg_){r.status=Status::Changed;return false;}
    if(!io.valid(io.context)||!io_.permit(io.context,Domain::Vtg,initial_.timingIndex,vtgAddress_,true)||!io.valid(io.context)){r.status=Status::AccessDenied;return false;}
    attempted_|=bit;++r.writes;r.lastRegister=vtgAddress_;r.lastValue=value;r.lastReadSucceeded=false;
    if(!io_.write32(io.context,vtgAddress_,value)){r.status=Status::WriteFailed;return false;}
    if(!io.valid(io.context)||!io_.permit(io.context,Domain::Vtg,initial_.timingIndex,vtgAddress_,true)||!io.valid(io.context)){r.status=Status::AccessDenied;return false;}
    if(!read(Domain::Vtg,initial_.timingIndex,vtgAddress_,v,r))return false;
    if(v!=value){r.status=Status::Changed;return false;}vtg_=value;acknowledged_|=bit;return true;
}
Status Controller::disconnect(const Poll& poll,Result& r){
    r={};if(phase_!=Phase::Armed)return finish(Status::InvalidState,r);if(!pollValid(poll))return finish(Status::InvalidArgument,r);
    const uint8_t indices[]={initial_.opp,initial_.mpcc,initial_.mpcc,initial_.mpcc,initial_.mpcc};
    const uint8_t regs[]={5,0,1,2,3};
    for(unsigned i=0;i<5;++i){
        // MPC_OUT_MUX is REG_UPDATE in Linux; preserve the other output fields.
        // The four MPCC selector registers are REG_SET(...,0,...,0xf).
        const uint32_t value=i?(15u):((expected_.combiner[initial_.opp][5]&~15u)|15u);
        if(!guard(false,false,r)||!write(Bank::Combiner,indices[i],regs[i],value,UINT32_MAX,uint16_t(1u<<i),r))return fail(r.status,r);
    }
    for(uint32_t i=0;i<poll.iterations;++i){
        if(!guard(false,false,r))return fail(r.status,r);uint32_t v=0;++r.polls;
        if(!read(Domain::Backend,initial_.mpcc,address(Bank::Combiner,initial_.mpcc,4),v,r))return fail(r.status,r);
        if(v==1||v==5){phase_=Phase::MpcDetached;if(!guard(false,false,r))return fail(r.status,r);return finish(Status::Ok,r);}
        if(v!=2)return fail(Status::Changed,r);
        if(i+1<poll.iterations&&(!io_.backend.valid(io_.backend.context)||!io_.delayUs(io_.backend.context,poll.intervalUs)||!io_.backend.valid(io_.backend.context)))return fail(Status::AccessDenied,r);
    }
    return fail(Status::Timeout,r);
}
Status Controller::stopTiming(const Poll& poll,Result& r){
    r={};if(phase_!=Phase::MpcDetached)return finish(Status::InvalidState,r);if(!pollValid(poll))return finish(Status::InvalidArgument,r);
    if(!guard(true,false,r))return fail(r.status,r);const uint8_t t=initial_.timingIndex;
    if(!write(Bank::Timing,t,9,0xffff0000u,UINT32_MAX,1u<<5,r)||!guard(true,false,r)||
        !write(Bank::Timing,t,11,0,UINT32_MAX,1u<<6,r)||!guard(true,false,r)||
        !write(Bank::Timing,t,0,expected_.timing[t][0]&~1u,~0x10000u,1u<<7,r)||!guard(true,true,r)||
        !writeVtg(vtg_&~0x80000000u,1u<<8,r))return fail(r.status,r);
    for(uint32_t i=0;i<poll.iterations;++i){
        if(!guard(true,true,r))return fail(r.status,r);uint32_t control=0,clock=0;++r.polls;
        if(!read(Domain::Backend,t,address(Bank::Timing,t,0),control,r)||!read(Domain::Backend,t,address(Bank::Timing,t,1),clock,r))return fail(r.status,r);
        if(!(control&0x10001u)&&!(clock&0x10000u)){
            expected_.timing[t][0]=control;expected_.timing[t][1]=clock;
            phase_=Phase::TimingStopped;
            // The final observation must retain actual stopped bits exactly;
            // transition masks must not hide a reasserted master/BUSY state.
            if(!guard(true,false,r))return fail(r.status,r);return finish(Status::Ok,r);
        }
        if(i+1<poll.iterations&&(!io_.backend.valid(io_.backend.context)||!io_.delayUs(io_.backend.context,poll.intervalUs)||!io_.backend.valid(io_.backend.context)))return fail(Status::AccessDenied,r);
    }
    return fail(Status::Timeout,r);
}
const char* statusString(Status s){switch(s){
#define S(x) case Status::x:return #x
    S(Ok);S(InvalidArgument);S(InvalidState);S(DiscoveryRejected);S(CaptureRejected);S(DrainRequired);S(AccessDenied);S(ReadFailed);S(WriteFailed);
    S(DeviceUnavailable);S(Changed);S(OPPNotBlank);S(TimingNotRunning);S(LinkNotStopped);S(Timeout);
#undef S
    }return "Unknown";}
} }
