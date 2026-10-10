// SPDX-License-Identifier: MIT
// DCN3.2 inherited HDMI mute sequence from Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0 dcn30_hwseq.c,
// dcn10_optc.c, dcn10_stream_encoder.c and dcn30_afmt.c.
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
#include "hackynavi/boot_hdmi_stream.hpp"
namespace hackynavi { namespace boothdmistream { namespace {
using B=bootbackend::Bank;using R=Register;
const uint16_t offsets[]={0x20b0,0x2093,0x2096,0x2099,0x209a,0x2097,0x209b,0x209c,0x209d,
    0x209f,0x20a0,0x20a1,0x20a2,0x20a3,0x20a4,0x20a5,0x20a6,0x20a7,
    0x2082,0x2075,0x2083,0x2085,0x2074,0x2087,0x206a,0x206b,0x206c,0x206d,0x2081,0x2095};
static_assert(sizeof(offsets)/sizeof(offsets[0])==kRegisters,"register inventory");
const uint16_t linkOffsets[]={0x1f04,0x20b1,0x20b2};
const uint16_t frontendOffsets[]={0x208b,0x2091,0x210c,0x2094,0x2098,0x209e};
const uint16_t timingOffsets[]={0x1b41,0x1b85,0x1b8a,0x1b91,0x1b8b,0x1b54,0x1b82,0x1b44,0x1aca,0x1acb,0x1acc,0x1ad0};
const uint16_t combinerOffsets[]={0,1,2,5,0xe,0x3d8};
Domain domain(size_t r){return (r>=18&&r<=23)||r==28?Domain::Afmt:r>=24&&r<=27?Domain::Vpg:Domain::Hdmi;}
size_t count(B b){return b==B::Link?3:b==B::Frontend?6:b==B::Timing?12:6;}
size_t instances(B b){return b==B::Link||b==B::Frontend?5:4;}
struct Dmu {unsigned count;bool valid;uint64_t pipe,mpc;};
void collect(const DiscoveryIp& ip,void* p){if(ip.hardwareId!=271)return;auto& d=*static_cast<Dmu*>(p);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid){d.pipe=ip.baseAddresses[2];d.mpc=ip.baseAddresses[3];}}
bool pollValid(const Poll& p){return p.iterations&&p.intervalUs&&p.intervalUs<=100&&uint64_t(p.iterations)*p.intervalUs<=700000;}
}
Controller::Controller():phase_(Phase::Fresh),io_{},route_{},pipeBase_(0),mpcBase_(0),addresses_{},initial_{},expected_{},
    statusAddress_(0),generalAddress_(0),expectedGeneral_(0),attempted_(0),acknowledged_(0),cycles_(0),avmuteSettled_(false){}
Status Controller::finish(Status s,Result& r){r.status=s;r.attempted=attempted_;r.acknowledged=acknowledged_;
    r.cycles=cycles_;r.retentionRequired=retentionRequired();return s;}
Status Controller::fail(Status s,Result& r){phase_=Phase::Faulted;return finish(s,r);}
uint32_t Controller::backendAddress(B b,uint8_t n,uint8_t r) const {
    if(b==B::Link)return uint32_t((pipeBase_+linkOffsets[r]+n*(r?0x100u:1u))*4);
    if(b==B::Frontend)return uint32_t((pipeBase_+frontendOffsets[r]+n*0x100u)*4);
    if(b==B::Timing)return uint32_t((pipeBase_+timingOffsets[r]+n*(r<8?0x80u:0x10u))*4);
    return uint32_t((mpcBase_+combinerOffsets[r]+n*(r==5?4u:0x15u))*4);
}
bool Controller::read(Domain d,uint8_t n,uint32_t a,uint32_t& v,Result& r){
    auto& b=io_.backend;r.lastRegister=a;r.lastValue=0;r.lastReadSucceeded=false;
    if(!b.valid(b.context)||!io_.permit(b.context,d,n,a,false)||!b.valid(b.context)){r.status=Status::AccessDenied;return false;}
    ++r.reads;if(!b.read32(b.context,a,v)){r.status=Status::ReadFailed;return false;}
    r.lastReadSucceeded=true;r.lastValue=v;
    if(!b.valid(b.context)||!io_.permit(b.context,d,n,a,false)||!b.valid(b.context)){r.status=Status::AccessDenied;return false;}
    if(v==UINT32_MAX){r.status=Status::DeviceUnavailable;return false;}return true;
}
bool Controller::clock(Result& r){uint32_t v=0;if(!read(Domain::Hdmi,route_.frontendIndex,addresses_[0],v,r))return false;
    if(v!=0x101){r.status=Status::ClockUnavailable;return false;}return true;}
bool Controller::supported(Result& r){
    const auto* x=initial_;
    // Intentionally narrow immediate-update PCM profile. Never RMW W1C,
    // self-clearing update strobes, a test mode or an unknown packet producer.
    if(x[0]!=0x101||x[size_t(R::Metadata)]||x[size_t(R::AudioDelay)]&~0x30u||
       x[size_t(R::Info0)]&~0x30u||x[size_t(R::Info1)]&~0x3f00u||
       x[size_t(R::Acr)]&~0x80071133u||x[size_t(R::Generic0)]&~0x33333333u||
       x[size_t(R::Generic6)]&~0x03333333u||x[size_t(R::Immediate)]||
       x[size_t(R::Line10)]&~0xffffu||x[size_t(R::DoubleBuffer)]!=0x1000u||
       x[size_t(R::AudioPacket)]&~0x801u||x[size_t(R::AudioPacket2)]&~0xff00u||
       x[size_t(R::AudioInfo)]&~0x40u||x[size_t(R::AudioSource)]>=5||
       x[size_t(R::AudioVbi)]&~0x011f0000u||x[size_t(R::AudioMemory)]&~1u||
       x[size_t(R::VpgFrame)]||x[size_t(R::VpgImmediate)]||x[size_t(R::VpgStatus)]||
       x[size_t(R::VpgMemory)]&~1u||x[size_t(R::AudioStatus)]&~0x10u||
       x[size_t(R::HdmiStatus)]&~1u||x[size_t(R::HdmiStatus)]!=expectedGeneral_||
       (route_.frontend[route_.frontendIndex][3]&0x100u)||
       route_.frontend[route_.frontendIndex][4]!=0x31u){r.status=Status::UnsupportedState;return false;}
    return true;
}
bool Controller::guard(Result& r){
    if(!clock(r))return false;
    for(unsigned bank=0;bank<4;++bank)for(uint8_t n=0;n<instances(B(bank));++n)for(uint8_t q=0;q<count(B(bank));++q){
        const B b=B(bank);const uint32_t a=backendAddress(b,n,q);uint32_t v=0;
        auto& io=io_.backend;
        if(!io.canRead(io.context,b,n,a)){r.status=Status::AccessDenied;return false;}
        if(!read(Domain::Backend,n,a,v,r))return false;
        if(!io.canRead(io.context,b,n,a)){r.status=Status::AccessDenied;return false;}
        uint32_t want=b==B::Link?route_.link[n][q]:b==B::Frontend?route_.frontend[n][q]:
            b==B::Timing?route_.timing[n][q]:route_.combiner[n][q];
        if(b==B::Frontend&&n==route_.frontendIndex&&q==5)want=expectedGeneral_;
        if(v!=want){r.status=Status::Changed;return false;}
    }
    const auto t=route_.timingIndex;
    if((route_.timing[t][0]&0x10001u)!=0x10001u||!(route_.timing[t][1]&0x100u)){r.status=Status::TimingStopped;return false;}
    for(size_t q=1;q<kRegisters;++q){
        uint32_t v=0;if(!read(domain(q),route_.frontendIndex,addresses_[q],v,r))return false;
        if(q==size_t(R::HdmiStatus)&&attempted_&&!avmuteSettled_){
            if(v&~1u){r.status=Status::Changed;return false;}
        }else if(v!=expected_[q]){r.status=Status::Changed;return false;}
    }
    return clock(r);
}
Status Controller::capture(const uint8_t* discovery,size_t bytes,uint64_t bar,const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io& io,Result& r){
    r={};if(phase_!=Phase::Fresh)return finish(Status::InvalidState,r);
    if(!io.backend.valid||!io.backend.canRead||!io.backend.read32||!io.permit||!io.write32||!io.delayUs)return finish(Status::InvalidArgument,r);
    DiscoveryInfo info{};Dmu d{};
    if(parseDiscovery(discovery,bytes,info,collect,&d)!=DiscoveryStatus::Ok||d.count!=1||!d.valid||
       info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id||d.pipe>UINT32_MAX/4||d.mpc>UINT32_MAX/4||bar<4)
        return finish(Status::DiscoveryRejected,r);
    io_=io;pipeBase_=d.pipe;mpcBase_=d.mpc;phase_=Phase::Faulted;
    const auto captured=bootbackend::capture(discovery,bytes,bar,rom,romBytes,imageOffset,io.backend,route_,r.backend);
    r.reads=r.backend.reads;r.lastRegister=r.backend.lastRegister;r.lastValue=r.backend.lastValue;r.lastReadSucceeded=r.backend.lastReadSucceeded;
    if(captured!=bootbackend::Status::Ok)return finish(Status::CaptureRejected,r);
    for(size_t q=0;q<kRegisters;++q){const uint64_t a=(d.pipe+offsets[q]+route_.frontendIndex*0x100u)*4;
        if(a>UINT32_MAX||a>bar-4)return finish(Status::DiscoveryRejected,r);
        addresses_[q]=uint32_t(a);
        for(size_t k=0;k<q;++k)if(addresses_[k]==a)return finish(Status::DiscoveryRejected,r);
        for(unsigned bank=0;bank<4;++bank)for(uint8_t n=0;n<instances(B(bank));++n)for(uint8_t reg=0;reg<count(B(bank));++reg)
            if(a==backendAddress(B(bank),n,reg))return finish(Status::DiscoveryRejected,r);
    }
    const uint64_t status=(d.pipe+0x1b49+route_.timingIndex*0x80u)*4;
    if(status>UINT32_MAX||status>bar-4)return finish(Status::DiscoveryRejected,r);
    statusAddress_=uint32_t(status);generalAddress_=backendAddress(B::Frontend,route_.frontendIndex,5);
    for(auto a:addresses_)if(a==statusAddress_)return finish(Status::DiscoveryRejected,r);
    for(unsigned bank=0;bank<4;++bank)for(uint8_t n=0;n<instances(B(bank));++n)for(uint8_t reg=0;reg<count(B(bank));++reg)
        if(statusAddress_==backendAddress(B(bank),n,reg))return finish(Status::DiscoveryRejected,r);
    expectedGeneral_=route_.frontend[route_.frontendIndex][5];
    if(!clock(r))return finish(r.status,r);
    for(size_t q=0;q<kRegisters;++q){if(!read(domain(q),route_.frontendIndex,addresses_[q],initial_[q],r))return finish(r.status,r);expected_[q]=initial_[q];}
    if(!clock(r)||!supported(r)||!guard(r))return finish(r.status,r);
    phase_=Phase::Armed;return finish(Status::Ok,r);
}
bool Controller::write(Domain d,uint32_t a,uint32_t value,uint8_t bit,Result& r){
    if(!guard(r))return false;auto& b=io_.backend;
    if(!b.valid(b.context)||!io_.permit(b.context,d,route_.frontendIndex,a,true)||!b.valid(b.context)){r.status=Status::AccessDenied;return false;}
    attempted_|=bit;++r.writes;r.lastRegister=a;r.lastValue=value;r.lastReadSucceeded=false;
    if(!io_.write32(b.context,a,value)){r.status=Status::WriteFailed;return false;}
    if(!b.valid(b.context)||!io_.permit(b.context,d,route_.frontendIndex,a,true)||!b.valid(b.context)){r.status=Status::AccessDenied;return false;}
    uint32_t observed=0;if(!read(d,route_.frontendIndex,a,observed,r))return false;
    if(observed!=value){r.status=Status::Changed;return false;}
    acknowledged_|=bit;return true;
}
bool Controller::waitState(uint32_t wanted,const Poll& p,uint32_t& left,Result& r){
    while(left){--left;++r.polls;if(!guard(r))return false;
        uint32_t v=0;if(!read(Domain::Timing,route_.timingIndex,statusAddress_,v,r))return false;
        if((v&~0x7000fu)||(v&3u)==3u){r.status=Status::ContradictoryStatus;return false;}
        if(!guard(r))return false;
        if((v&3u)==wanted)return true;
        if(left){if(!io_.backend.valid(io_.backend.context)||!io_.delayUs(io_.backend.context,p.intervalUs)||!io_.backend.valid(io_.backend.context)){r.status=Status::AccessDenied;return false;}}
    }
    r.status=Status::Timeout;return false;
}
Status Controller::mute(const Poll& p,Result& r){
    r={};if(phase_!=Phase::Armed)return finish(Status::InvalidState,r);
    if(!pollValid(p))return finish(Status::InvalidArgument,r);
    if(!write(Domain::Hdmi,generalAddress_,expectedGeneral_|1u,1,r))return fail(r.status,r);
    expectedGeneral_|=1u;
    uint32_t left=p.iterations;
    if(!waitState(2,p,left,r))return fail(r.status,r);
    for(unsigned n=0;n<3;++n){if(!waitState(1,p,left,r)||!waitState(2,p,left,r))return fail(r.status,r);++cycles_;}
    // Actual HDMI_ACTIVE_AVMUTE is additionally checked, never invented from
    // a successful write or guessed delay. Three cycles remain mandatory.
    expected_[size_t(R::HdmiStatus)]=1;avmuteSettled_=true;
    if(!guard(r))return fail(r.status,r);
    const auto q=size_t(R::AudioPacket);const uint32_t muted=expected_[q]&~1u;
    if(!write(Domain::Afmt,addresses_[q],muted,2,r))return fail(r.status,r);
    expected_[q]=muted;
    if(!guard(r))return fail(r.status,r);
    phase_=Phase::MutedRetained;return finish(Status::Ok,r);
}
const char* statusString(Status s){switch(s){
    case Status::Ok:return "ok";case Status::InvalidArgument:return "invalid-argument";case Status::InvalidState:return "invalid-state";
    case Status::DiscoveryRejected:return "discovery-rejected";case Status::CaptureRejected:return "capture-rejected";
    case Status::UnsupportedState:return "unsupported-state";case Status::ClockUnavailable:return "clock-unavailable";
    case Status::AccessDenied:return "access-denied";case Status::ReadFailed:return "read-failed";case Status::WriteFailed:return "write-failed";
    case Status::DeviceUnavailable:return "device-unavailable";case Status::Changed:return "changed";case Status::TimingStopped:return "timing-stopped";
    case Status::ContradictoryStatus:return "contradictory-status";case Status::Timeout:return "timeout";}return "unknown";}
} }
