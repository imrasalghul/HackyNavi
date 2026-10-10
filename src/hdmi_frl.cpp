// SPDX-License-Identifier: MIT
/*
 * Copyright 2022 Advanced Micro Devices, Inc.
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
// Adapted protocol from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0
// link/protocols/link_hdmi_frl.c and dc_hdmi_types.h. Deliberate differences:
// propagate every I/O error; deadlines include callback time; no test-mode
// success; no rate fallback without mode recalculation; retained failed cleanup.
#include "hackynavi/hdmi_frl.hpp"
namespace hackynavi { namespace hdmifrl { namespace {
bool validConfig(const Config& c){return c.rateCode>=1&&c.rateCode<=6&&
    c.sourceMaxRateCode>=c.rateCode&&c.sourceMaxRateCode<=6&&
    c.sinkMaxRateCode>=c.rateCode&&c.sinkMaxRateCode<=6&&c.maxFfe<=3&&
    c.modePlanIdentity&&c.routeGeneration;}
bool validIo(const Io& i){return i.enter&&i.leave&&i.valid&&i.nowUs&&i.delayUs&&
    i.readScdc&&i.writeScdc&&i.ddcHealthy&&i.prepareSource&&i.drive&&i.patterns&&i.packetMode&&i.stopSource;}
}
Link::Link():config_{},io_{},phase_(Phase::Closed),sourceDirty_(false),sinkDirty_(false),ddcDirty_(false),
    trainingObserved_(false),startObserved_(false),ffe_(0),lastTime_(0){}
void Link::clearAfterVerifiedReset(){config_={};io_={};phase_=Phase::Closed;
    sourceDirty_=sinkDirty_=ddcDirty_=trainingObserved_=startObserved_=false;ffe_=0;lastTime_=0;}
Status Link::finish(Status s,Result& r){r.status=s;r.phase=phase_;r.ffe=ffe_;
    r.sourceMayBeChanged=sourceDirty_;r.sinkMayBeChanged=sinkDirty_;
    r.trainingObserved=trainingObserved_;r.startObserved=startObserved_;
    r.ddcRecoveryRequired=ddcDirty_;
    r.retentionRequired=requiresHardwareRetirement();return s;}
Status Link::clock(uint64_t& t){if(!io_.valid(io_.context))return Status::Revoked;
    if(!io_.nowUs(io_.context,t)||t<lastTime_)return Status::ClockFailed;
    lastTime_=t;return Status::Ok;}
Status Link::read(uint8_t off,uint8_t* p,size_t n,Result& r){
    if(!io_.valid(io_.context))return Status::Revoked;
    ddcDirty_=!io_.ddcHealthy(io_.context);if(ddcDirty_)return Status::ScdcFailed;
    ++r.reads;r.lastOffset=off;const bool ok=io_.readScdc(io_.context,off,p,n);
    ddcDirty_=!io_.ddcHealthy(io_.context);if(!ok||ddcDirty_)return Status::ScdcFailed;
    r.lastValue=p[n-1];return io_.valid(io_.context)?Status::Ok:Status::Revoked;}
Status Link::write(uint8_t off,uint8_t value,Result& r){
    if(!io_.valid(io_.context))return Status::Revoked;
    ddcDirty_=!io_.ddcHealthy(io_.context);if(ddcDirty_)return Status::ScdcFailed;
    ++r.writes;r.lastOffset=off;r.lastValue=value;sinkDirty_=true;
    const bool ok=io_.writeScdc(io_.context,off,value);
    ddcDirty_=!io_.ddcHealthy(io_.context);if(!ok||ddcDirty_)return Status::ScdcFailed;
    return io_.valid(io_.context)?Status::Ok:Status::Revoked;}
Status Link::source(bool (*fn)(void*),Result& r){
    if(!io_.valid(io_.context))return Status::Revoked;
    ++r.sourceCommands;sourceDirty_=true;
    if(!fn(io_.context))return Status::SourceFailed;
    return io_.valid(io_.context)?Status::Ok:Status::Revoked;}
Status Link::cleanup(Result& r){
    // A failed source/DDC operation may have landed. Never clear the receiver
    // rate while the transmitter may still be running; never retry after revoke.
    Status s=Status::Ok;
    if(sourceDirty_){s=source(io_.stopSource,r);if(s!=Status::Ok)return s;sourceDirty_=false;}
    r.sourceOff=true;
    ddcDirty_=!io_.ddcHealthy(io_.context);if(ddcDirty_)return Status::ScdcFailed;
    if(sinkDirty_){s=write(0x31,0,r);if(s!=Status::Ok)return s;
        uint8_t v=0;s=read(0x31,&v,1,r);if(s!=Status::Ok)return s;
        if(v)return Status::VerifyFailed;sinkDirty_=false;}
    r.sinkRateOff=true;trainingObserved_=startObserved_=false;return Status::Ok;}
Status Link::failed(Status original,Result& r){
    const auto clean=cleanup(r);
    phase_=clean==Status::Ok?Phase::Closed:Phase::Faulted;
    return finish(original,r);}
Status Link::waitTick(uint64_t start,uint32_t budget,Status timeout,Result& r){
    uint64_t before=0,after=0;auto s=clock(before);if(s!=Status::Ok)return s;
    if(before-start>=budget)return timeout;
    io_.delayUs(io_.context,2000);s=clock(after);if(s!=Status::Ok)return s;
    if(after<=before)return Status::ClockFailed;
    r.elapsedUs=after-start;
    if(after-start>budget)return timeout;
    ++r.polls;return Status::Ok;
}
Status Link::training(Result& r){
    uint8_t v=0;auto s=read(1,&v,1,r);if(s!=Status::Ok)return s;
    if(!v||v==255)return Status::UnsupportedReceiver;
    // A test box may request special PHY waveforms or unbounded training.
    // Those require a separate compliance workflow, not production success.
    s=read(0x35,&v,1,r);if(s!=Status::Ok)return s;
    if(v)return Status::UnsupportedTestRequest;
    s=read(0x31,&v,1,r);if(s!=Status::Ok)return s;
    if(v){s=write(0x31,0,r);if(s!=Status::Ok)return s;}
    s=write(2,1,r);if(s!=Status::Ok)return s;
    uint64_t start=0;s=clock(start);if(s!=Status::Ok)return s;
    bool ready=false;
    for(unsigned n=0;n<105;++n){
        s=waitTick(start,210000,Status::ReadyTimeout,r);if(s!=Status::Ok)return s;
        s=read(0x40,&v,1,r);if(s!=Status::Ok)return s;
        uint64_t now=0;s=clock(now);if(s!=Status::Ok)return s;
        if(now-start>210000)return Status::ReadyTimeout;
        if(v&0x40){ready=true;break;}
    }
    if(!ready)return Status::ReadyTimeout;
    s=read(0x10,&v,1,r);if(s!=Status::Ok)return s;
    if(v&0x20){s=write(0x10,0x20,r);if(s!=Status::Ok)return s;}
    if(!io_.valid(io_.context))return Status::Revoked;
    sourceDirty_=true;++r.sourceCommands;
    if(!io_.prepareSource(io_.context,config_))return Status::SourceFailed;
    if(!io_.valid(io_.context))return Status::Revoked;
    ++r.sourceCommands;if(!io_.drive(io_.context,0))return Status::SourceFailed;
    s=clock(start);if(s!=Status::Ok)return s;
    s=write(0x31,uint8_t(config_.rateCode|(config_.maxFfe<<4)),r);if(s!=Status::Ok)return s;
    s=read(0x31,&v,1,r);if(s!=Status::Ok)return s;
    if(v!=uint8_t(config_.rateCode|(config_.maxFfe<<4)))return Status::VerifyFailed;
    const unsigned lanes=config_.rateCode<=2?3:4;
    for(unsigned n=0;n<105;++n){
        s=waitTick(start,210000,Status::TrainingTimeout,r);if(s!=Status::Ok)return s;
        s=read(0x10,&v,1,r);if(s!=Status::Ok)return s;
        uint64_t now=0;s=clock(now);if(s!=Status::Ok)return s;
        if(now-start>210000)return Status::TrainingTimeout;
        if(v&8)return Status::UnsupportedTestRequest;
        if(!(v&0x20))continue;
        uint8_t raw[2]={};s=read(0x41,raw,2,r);if(s!=Status::Ok)return s;
        s=clock(now);if(s!=Status::Ok)return s;
        if(now-start>210000)return Status::TrainingTimeout;
        uint8_t request[4]={uint8_t(raw[0]&15),uint8_t(raw[0]>>4),uint8_t(raw[1]&15),uint8_t(raw[1]>>4)};
        bool zero=true,lower=false,nextFfe=false,ltp3=false;
        for(unsigned l=0;l<lanes;++l){const auto p=request[l];zero=zero&&!p;
            lower=lower||p==15;nextFfe=nextFfe||p==14;ltp3=ltp3||p==3;
            if(p>=9&&p<=13)return Status::InvalidPattern;}
        // W1C only FLT_UPDATE: do not acknowledge unrelated concurrent events.
        s=write(0x10,0x20,r);if(s!=Status::Ok)return s;
        s=clock(now);if(s!=Status::Ok)return s;
        if(now-start>210000)return Status::TrainingTimeout;
        if(lower)return Status::LowerRateRequired;
        if(zero){trainingObserved_=true;phase_=Phase::Trained;return Status::Ok;}
        if(ltp3)continue; // AMD normal-training LTP3 handling; never test-mode success.
        if(nextFfe){
            if(ffe_==config_.maxFfe)return Status::FfeExhausted;
            if(!io_.valid(io_.context))return Status::Revoked;
            ++ffe_;++r.sourceCommands;
            if(!io_.drive(io_.context,ffe_))return Status::SourceFailed;
            continue; // Preserve lane patterns during the FFE transition.
        }
        uint8_t patterns[4]={255,255,255,255};
        for(unsigned l=0;l<lanes;++l)if(request[l])patterns[l]=uint8_t(request[l]-1);
        if(!io_.valid(io_.context))return Status::Revoked;
        ++r.sourceCommands;if(!io_.patterns(io_.context,patterns))return Status::SourceFailed;
    }
    return Status::TrainingTimeout;
}
Status Link::train(const Config& c,const Io& io,Result& r){
    r={};if(phase_!=Phase::Closed||requiresHardwareRetirement())return finish(Status::InvalidState,r);
    if(!validConfig(c)||!validIo(io))return finish(Status::InvalidArgument,r);
    if(!io.enter(io.context,Operation::Train,c))return finish(Status::AdmissionFailed,r);
    config_=c;io_=io;lastTime_=0;ffe_=0;trainingObserved_=startObserved_=false;phase_=Phase::Training;
    const auto s=training(r);const auto result=s==Status::Ok?finish(s,r):failed(s,r);
    io_.leave(io_.context);return result;
}
Status Link::startPackets(Result& r){
    r={};if(phase_!=Phase::Trained)return finish(Status::InvalidState,r);
    if(!io_.enter(io_.context,Operation::StartPackets,config_))return finish(Status::AdmissionFailed,r);
    auto s=source(io_.packetMode,r);
    if(s==Status::Ok){phase_=Phase::Packets;uint64_t start=0;s=clock(start);
        if(s==Status::Ok){s=Status::StartTimeout;
            for(unsigned n=0;n<100;++n){
                s=waitTick(start,200000,Status::StartTimeout,r);if(s!=Status::Ok)break;
                uint8_t v=0;s=read(0x10,&v,1,r);if(s!=Status::Ok)break;
                uint64_t now=0;s=clock(now);if(s!=Status::Ok)break;
                if(now-start>200000){s=Status::StartTimeout;break;}
                if(v&0x28){s=(v&8)?Status::UnsupportedTestRequest:Status::RetrainRequired;break;}
                if(v&0x10){s=write(0x10,0x10,r);if(s==Status::Ok)s=clock(now);
                    if(s==Status::Ok&&now-start>200000)s=Status::StartTimeout;
                    if(s==Status::Ok){startObserved_=true;phase_=Phase::Started;}break;}
                s=Status::StartTimeout;
            }
        }
    }
    const auto result=s==Status::Ok?finish(s,r):failed(s,r);io_.leave(io_.context);return result;
}
Status Link::stop(Result& r){
    r={};if(phase_==Phase::Closed&&!requiresHardwareRetirement())return finish(Status::Ok,r);
    if(!io_.enter(io_.context,Operation::Stop,config_))return finish(Status::AdmissionFailed,r);
    const auto s=cleanup(r);phase_=s==Status::Ok?Phase::Closed:Phase::Faulted;
    const auto result=finish(s,r);io_.leave(io_.context);return result;
}
const char* statusString(Status s){switch(s){
#define S(x) case Status::x:return #x
    S(Ok);S(InvalidArgument);S(InvalidState);S(AdmissionFailed);S(Revoked);S(ClockFailed);
    S(ScdcFailed);S(SourceFailed);S(VerifyFailed);S(UnsupportedReceiver);S(UnsupportedTestRequest);
    S(ReadyTimeout);S(TrainingTimeout);S(StartTimeout);S(LowerRateRequired);S(RetrainRequired);
    S(InvalidPattern);S(FfeExhausted);
#undef S
    }return "Unknown";}
} }
