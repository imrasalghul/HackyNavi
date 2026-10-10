// SPDX-License-Identifier: MIT
// Native DP protocol follows Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// display/dc/link/protocols/link_dp_training.c, link_dp_training_8b_10b.[ch],
// link_dp_capability.c; drm/display/drm_dp_helper.c; include/drm/display/drm_dp.h.
// Copyright 2022 Advanced Micro Devices, Inc.
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
#include "hackynavi/dp_training.hpp"
namespace hackynavi { namespace dptrain {
namespace {
bool lanesValid(uint8_t n) { return n==1 || n==2 || n==4; }
bool rateValid(uint32_t r) { return r==162000 || r==270000 || r==540000 || r==810000; }
bool valid(const Config& c) { return lanesValid(c.lanes) && rateValid(c.symbolClockKHz) &&
    lanesValid(c.source.maxLanes) && rateValid(c.source.maxSymbolClockKHz); }
bool valid(const Io& io) { return io.readDpcd && io.writeDpcd && io.sourceEnable && io.sourcePattern &&
    io.sourceDrive && io.sourceVideo && io.sourceDisable && io.auxHealthy && io.sourceHealthy && io.delayUs; }
class Training {
    const Io& io; const Config& cfg; Result& out;
    bool changed; uint8_t power;
    AuxStatus rd(uint32_t address,uint8_t* data,uint8_t bytes) {
        ++out.auxReads;out.lastDpcd=address;return io.readDpcd(io.context,address,data,bytes);
    }
    bool wr(uint32_t address,const uint8_t* data,uint8_t bytes) {
        ++out.auxWrites;out.lastDpcd=address;return io.writeDpcd(io.context,address,data,bytes)==AuxStatus::Ok;
    }
    bool wb(uint32_t address,uint8_t value) {return wr(address,&value,1);}
    void delay(uint32_t us) {out.delayUs+=us;io.delayUs(io.context,us);}
    Status fail(Status status) {
        out.status=status;
        if(!changed)return status;
        out.cleanupAttempted=true;
        if(io.auxHealthy(io.context)) {
            uint8_t observed=0xff;
            out.sinkPatternCleared=wb(0x102,0) && rd(0x102,&observed,1)==AuxStatus::Ok && !(observed&0x2f);
            if(out.sinkPatternCleared)out.patternMayBeActive=false;
            // A write can have reached the sink even if the host loses its ACK.
            // Include bit2: D3_AUX_ON is 5, despite this Linux header's older
            // DP_SET_POWER_MASK=3. Leaving bit2 set would encode 6, not D3.
            if(io.auxHealthy(io.context))out.sinkPoweredDown=wb(0x600,uint8_t((power&~7U)|2)) &&
                rd(0x600,&observed,1)==AuxStatus::Ok && (observed&7)==2;
        }
        if(io.sourceHealthy(io.context)) {
            ++out.sourceCommands;out.sourceDisabled=io.sourceDisable(io.context);
        }
        out.recoveryRequired=!out.sourceDisabled || !out.sinkPatternCleared || !out.sinkPoweredDown;
        return status;
    }
    bool crDone() const {
        for(uint8_t n=0;n<cfg.lanes;++n)if(!(out.statusBytes[n/2]&(1U<<((n%2)*4))))return false;
        return true;
    }
    bool eqDone() const {
        for(uint8_t n=0;n<cfg.lanes;++n)if(((out.statusBytes[n/2]>>((n%2)*4))&7)!=7)return false;
        return (out.statusBytes[2]&1)!=0;
    }
    void adjust() {
        uint8_t vs=0,pre=0;
        for(uint8_t n=0;n<cfg.lanes;++n) {
            const uint8_t req=uint8_t(out.statusBytes[4+n/2]>>((n%2)*4));
            if((req&3)>vs)vs=req&3;
            if(((req>>2)&3)>pre)pre=(req>>2)&3;
        }
        if(pre>3-vs)pre=uint8_t(3-vs);
        out.swing=vs;out.preEmphasis=pre;
    }
    bool driveAndNotify(uint8_t pattern,bool first) {
        ++out.sourceCommands;
        if(!io.sourceDrive(io.context,out.swing,out.preEmphasis)) {out.status=Status::SourceFailed;return false;}
        // The source and every sink lane receive the same clamped levels.
        out.laneSet=uint8_t(out.swing|(out.preEmphasis<<3)|(out.swing==3?4:0)|(out.preEmphasis==3?0x20:0));
        uint8_t bytes[5]={pattern,out.laneSet,out.laneSet,out.laneSet,out.laneSet};
        out.patternMayBeActive=true;
        if(!wr(first?0x102:0x103,bytes+(first?0:1),uint8_t(cfg.lanes+(first?1:0)))) {out.status=Status::AuxFailed;return false;}
        return true;
    }
    Status capabilities() {
        out.stage=Stage::Capabilities;
        // Throwaway probe wakes sinks whose first DPCD reply after power-save
        // is corrupted (drm_dp_dpcd_read's HP ZR24w workaround).
        uint8_t probe=0;
        if(rd(0x102,&probe,1)!=AuxStatus::Ok || rd(0,out.sink.raw,15)!=AuxStatus::Ok)return Status::AuxFailed;
        uint8_t* caps=out.sink.raw;
        if(caps[14]&0x80) {
            uint8_t ext[15]={};if(rd(0x2200,ext,15)!=AuxStatus::Ok)return Status::AuxFailed;
            if(ext[0]>=caps[0]) {for(unsigned n=0;n<15;++n)caps[n]=ext[n];out.sink.extended=true;}
        }
        out.sink.revision=caps[0];out.sink.maxLanes=caps[2]&31;
        out.sink.maxSymbolClockKHz=uint32_t(caps[1])*27000;
        out.sink.tps3=(caps[2]&0x40)!=0;out.sink.tps4=(caps[3]&0x80)!=0;
        out.sink.downspread=(caps[3]&1)!=0;
        if(caps[0]<0x11 || caps[0]>0x20 || !lanesValid(out.sink.maxLanes) ||
            !rateValid(out.sink.maxSymbolClockKHz) || !(caps[2]&0x80) || !(caps[6]&1) || (caps[13]&1))
            return Status::UnsupportedReceiver;
        if(caps[5]&1)return Status::UnsupportedTopology; // branch/converter, not a native endpoint
        if(caps[0]>=0x12) {
            uint8_t mst=0,mode=0;
            if(rd(0x21,&mst,1)!=AuxStatus::Ok || rd(0x111,&mode,1)!=AuxStatus::Ok)return Status::AuxFailed;
            out.sink.mstCapable=(mst&1)!=0;
            if(mode&7)return Status::UnsupportedTopology;
        }
        uint8_t repeater[8]={};const auto ret=rd(0xf0000,repeater,8);
        if(ret==AuxStatus::Failed)return Status::AuxFailed;
        // Native sinks can NACK this optional range. Any count is a repeater
        // topology, including malformed count encodings; never train through it.
        if(ret==AuxStatus::Ok && repeater[2])return Status::UnsupportedTopology;
        if(caps[0]>=0x14) {
            uint8_t fec=0,dsc=0;
            if(rd(0x120,&fec,1)!=AuxStatus::Ok || rd(0x160,&dsc,1)!=AuxStatus::Ok)return Status::AuxFailed;
            if((fec&1) || (dsc&3))return Status::UnsupportedReceiver;
        }
        if(cfg.lanes>cfg.source.maxLanes || cfg.lanes>out.sink.maxLanes ||
            cfg.symbolClockKHz>cfg.source.maxSymbolClockKHz || cfg.symbolClockKHz>out.sink.maxSymbolClockKHz ||
            (cfg.downspread && !out.sink.downspread) || (cfg.symbolClockKHz==810000 && !out.sink.tps4))return Status::LinkLimits;
        out.eqPattern=out.sink.tps4?dio::Pattern::Tps4:out.sink.tps3?dio::Pattern::Tps3:dio::Pattern::Tps2;
        const uint8_t interval=caps[0]>=0x12?caps[14]&0x7f:0;
        if(interval>6)return Status::UnsupportedReceiver;
        out.sink.crDelayUs=interval?uint32_t(interval)*4000:((caps[0]>=0x12 && caps[14])?400:100);
        const uint32_t eq[]={400,4000,8000,12000,16000,32000,64000};out.sink.eqDelayUs=eq[interval];
        return Status::Ok;
    }
public:
    Training(const Io& i,const Config& c,Result& r):io(i),cfg(c),out(r),changed(false),power(0) {}
    Status run() {
        auto status=capabilities();if(status!=Status::Ok)return fail(status);
        out.stage=Stage::PowerUp;
        if(rd(0x600,&power,1)!=AuxStatus::Ok)return fail(Status::AuxFailed);
        changed=true;out.sinkPowerMayBeChanged=true;
        if(!wb(0x600,uint8_t((power&~7U)|1)))return fail(Status::AuxFailed);
        delay(1000);uint8_t actual=0;
        if(rd(0x600,&actual,1)!=AuxStatus::Ok)return fail(Status::AuxFailed);
        if((actual&7)!=1)return fail(Status::VerifyFailed);
        out.stage=Stage::Configure;
        // No post-LT adjustment grant, FEC or alternate coding. Source PLL
        // SSC setting was chosen by the clock owner and must equal cfg.
        if(!wb(0x102,0) || !wb(0x108,1) || !wb(0x107,cfg.downspread?0x10:0) ||
            !wb(0x101,uint8_t(0x80|cfg.lanes)) || !wb(0x100,uint8_t(cfg.symbolClockKHz/27000)))return fail(Status::AuxFailed);
        out.stage=Stage::SourceEnable;out.sourceEnableAttempted=true;++out.sourceCommands;
        if(!io.sourceEnable(io.context,cfg.symbolClockKHz))return fail(Status::SourceFailed);
        out.stage=Stage::ClockRecovery;++out.sourceCommands;
        if(!io.sourcePattern(io.context,dio::Pattern::Tps1))return fail(Status::SourceFailed);
        uint32_t same=0;
        for(unsigned n=0;n<100 && same<5;++n) {
            ++out.crAttempts;
            if(!driveAndNotify(0x21,n==0))return fail(out.status);
            delay(out.sink.crDelayUs);
            if(rd(0x202,out.statusBytes,6)!=AuxStatus::Ok)return fail(Status::AuxFailed);
            if(crDone())break;
            if(out.swing==3)return fail(Status::ClockRecoveryFailed);
            const uint8_t previous=out.swing;adjust();same=out.swing==previous?same+1:0;
        }
        if(!crDone())return fail(Status::ClockRecoveryFailed);
        out.stage=Stage::Equalization;++out.sourceCommands;
        if(!io.sourcePattern(io.context,out.eqPattern))return fail(Status::SourceFailed);
        const uint8_t eq=out.eqPattern==dio::Pattern::Tps4?7:out.eqPattern==dio::Pattern::Tps3?0x23:0x22;
        bool equalized=false;
        for(unsigned n=0;n<6;++n) {
            ++out.eqAttempts;if(!driveAndNotify(eq,n==0))return fail(out.status);
            delay(out.sink.eqDelayUs);
            if(rd(0x202,out.statusBytes,6)!=AuxStatus::Ok)return fail(Status::AuxFailed);
            if(!crDone())return fail(Status::ClockRecoveryFailed);
            if(eqDone()) {equalized=true;break;}
            adjust();
        }
        if(!equalized)return fail(Status::EqualizationFailed);
        out.stage=Stage::ExitTraining;
        if(!wb(0x102,0))return fail(Status::AuxFailed);
        uint8_t pattern=0xff;
        if(rd(0x102,&pattern,1)!=AuxStatus::Ok)return fail(Status::AuxFailed);
        if(pattern&0x2f)return fail(Status::VerifyFailed);
        out.patternMayBeActive=false;out.sinkPatternCleared=true;
        out.stage=Stage::VideoIdle;++out.sourceCommands;
        if(!io.sourceVideo(io.context))return fail(Status::SourceFailed);
        out.stage=Stage::VerifyLink;delay(5000);
        if(rd(0x202,out.statusBytes,6)!=AuxStatus::Ok)return fail(Status::AuxFailed);
        if(!eqDone())return fail(Status::LinkLost);
        out.trained=true;out.stage=Stage::Complete;out.status=Status::Ok;return Status::Ok;
    }
};
struct Hardware {
    dpaux::Engine& aux;dio::Link& source;dmcub::Inbox& inbox;
    const dmcub::PollOptions& polls;const Delay& delay;HardwareResult& out;
    static AuxStatus access(void* c,uint32_t at,uint8_t* read,const uint8_t* write,uint8_t bytes) {
        auto& h=*static_cast<Hardware*>(c);
        const dpaux::Request req={read?dpaux::Operation::NativeRead:dpaux::Operation::NativeWrite,at,bytes,false,write};
        h.out.auxStatus=h.aux.transferWithRetries(req,h.out.aux);
        if(h.out.auxStatus==dpaux::Status::Nack)return AuxStatus::Nack;
        if(h.out.auxStatus!=dpaux::Status::Ok)return AuxStatus::Failed;
        if(read) {
            if(h.out.aux.length!=bytes)return AuxStatus::Failed;
            for(uint8_t n=0;n<bytes;++n)read[n]=h.out.aux.data[n];
        }
        return AuxStatus::Ok;
    }
    static AuxStatus read(void* c,uint32_t at,uint8_t* p,uint8_t n) {return access(c,at,p,nullptr,n);}
    static AuxStatus write(void* c,uint32_t at,const uint8_t* p,uint8_t n) {return access(c,at,nullptr,p,n);}
    static bool enable(void* c,uint32_t clock) {auto& h=*static_cast<Hardware*>(c);return (h.out.sourceStatus=h.source.enable(h.inbox,clock,h.polls,h.out.source))==dio::Status::Ok;}
    static bool pattern(void* c,dio::Pattern p) {auto& h=*static_cast<Hardware*>(c);return (h.out.sourceStatus=h.source.trainingPattern(p,h.out.source))==dio::Status::Ok;}
    static bool drive(void* c,uint8_t v,uint8_t p) {auto& h=*static_cast<Hardware*>(c);return (h.out.sourceStatus=h.source.drive(h.inbox,v,p,0,h.polls,h.out.source))==dio::Status::Ok;}
    static bool video(void* c) {auto& h=*static_cast<Hardware*>(c);return (h.out.sourceStatus=h.source.videoPattern(h.out.source))==dio::Status::Ok;}
    static bool disable(void* c) {auto& h=*static_cast<Hardware*>(c);return (h.out.sourceStatus=h.source.disable(h.inbox,h.polls,h.out.source))==dio::Status::Ok;}
    static bool auxHealthy(void* c) {return static_cast<Hardware*>(c)->aux.isOpen();}
    static bool sourceHealthy(void* c) {auto& h=*static_cast<Hardware*>(c);return !h.source.needsRecovery() && h.inbox.isOpen();}
    static void wait(void* c,uint32_t us) {auto& h=*static_cast<Hardware*>(c);h.delay.delayUs(h.delay.context,us);}
};
} // namespace
Status train(const Io& io,const Config& cfg,Result& out) {
    out=Result{};
    if(!valid(io) || !valid(cfg)) {out.status=Status::InvalidArgument;return out.status;}
    if(!io.auxHealthy(io.context) || !io.sourceHealthy(io.context)) {out.status=Status::InvalidState;return out.status;}
    return Training(io,cfg,out).run();
}
Status train(dpaux::Engine& aux,dio::Link& source,dmcub::Inbox& inbox,const Config& cfg,
    const dmcub::PollOptions& polls,const Delay& delay,HardwareResult& out) {
    out=HardwareResult{};
    if(!valid(cfg) || !delay.delayUs || !polls.maxPolls || polls.maxPolls>1000000 || !polls.delayUs ||
        polls.delayUs>100 || uint64_t(polls.maxPolls)*polls.delayUs>2000000) {out.training.status=Status::InvalidArgument;return out.training.status;}
    const auto& route=source.route();
    if(!aux.isOpen() || !inbox.isOpen() || source.phase()!=dio::Phase::Prepared || (route.connectorObject&0xff)!=0x13 ||
        source.laneCount()!=cfg.lanes || aux.engineIndex()!=route.ddc || aux.hpdIndex()!=route.hpd) {
        out.training.status=Status::InvalidState;return out.training.status;
    }
    Hardware h={aux,source,inbox,polls,delay,out};
    const Io io={&h,Hardware::read,Hardware::write,Hardware::enable,Hardware::pattern,Hardware::drive,
        Hardware::video,Hardware::disable,Hardware::auxHealthy,Hardware::sourceHealthy,Hardware::wait};
    return train(io,cfg,out.training);
}
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(AuxFailed);NAME(SourceFailed);
    NAME(UnsupportedReceiver);NAME(UnsupportedTopology);NAME(LinkLimits);NAME(VerifyFailed);
    NAME(ClockRecoveryFailed);NAME(EqualizationFailed);NAME(LinkLost);
#undef NAME
    }return "Unknown";
}
} }
