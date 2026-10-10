// SPDX-License-Identifier: MIT
// Host-only DAL mailbox model; no firmware/hardware execution is inferred.
#include "hackynavi/display_smu.hpp"
#include <iostream>
#include <vector>
namespace d=hackynavi::displaysmu;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
hackynavi::Navi31IpLayout ips(){
    hackynavi::Navi31IpLayout l={};l.valid=true;const uint16_t hw[]={11,34,255,1,42,43};const uint8_t maj[]={11,3,13,13,6,6};
    for(size_t i=0;i<6;++i){auto& b=l.blocks[i];b.hardwareId=hw[i];b.major=maj[i];b.baseCount=2;
        b.basesDwords[0]=i==3?0x16000:uint32_t(0x1000+i*0x1000);b.basesDwords[1]=0xa000;}
    return l;
}
const d::Poll poll={32,10};
struct Event {uint32_t address,value;};
struct Device {
    d::Layout layout={};d::Controller controller;d::Result result={};
    unsigned calls=0,failAt=0,onesAt=0,delays=0,delayBudget=0,statusQueries=0,statusAfter=0;
    uint32_t response=1,argument=0,version=0x4e6a00,header=1,returnedKHz=600000,bits=0x3f,error=1;
    bool acceptFailedWrite=false;std::vector<Event> writes;
    std::vector<uint32_t> coldReplies,coldResponses;size_t coldReplyIndex=0,coldResponseIndex=0;
    Device(){CHECK(d::buildLayout(ips(),0x80000,layout)==d::Status::Ok);}
    d::Io io(){return {this,read,write,delay};}
    d::Status init(){return controller.initialize(layout,io(),poll,result);}
    static bool read(void* p,uint32_t a,uint32_t& v){auto& x=*static_cast<Device*>(p);++x.calls;
        CHECK(a==x.layout.argument||a==x.layout.response);v=a==x.layout.argument?x.argument:x.response;
        if(x.calls==x.onesAt)v=UINT32_MAX;return x.calls!=x.failAt;}
    static bool write(void* p,uint32_t a,uint32_t v){auto& x=*static_cast<Device*>(p);++x.calls;x.writes.push_back({a,v});
        if(a==x.layout.message&&v==9&&(x.argument>>16)==2){
            const auto& journal=x.controller.memoryJournal();
            CHECK(journal.requestAttempted&&journal.attemptedMHz==uint16_t(x.argument));
            CHECK(!journal.commandAcknowledged&&!journal.replyRead&&!journal.fulfilled);
        }
        if(a==x.layout.message&&v==9&&((x.argument>>16)==8||(x.argument>>16)==9)) {
            const auto* j=x.controller.coldPresetJournal(d::Clock(x.argument>>16));
            if(j&&j->started) {
                const auto& step=j->stage==d::ColdPresetStage::IntermediateRequest?j->intermediate:j->final;
                CHECK(step.requestAttempted&&step.requestedMHz==uint16_t(x.argument));
                CHECK(!step.commandAcknowledged&&!step.replyRead&&!step.fulfilled);
                CHECK(x.controller.minimumMayBeActive()&&x.controller.policyMayBeActive());
            }
        }
        if(a==x.layout.message&&v==0x14) {
            const auto& j=x.controller.dmcubWaitJournal();
            CHECK(x.argument==0&&j.requestAttempted&&!j.commandAcknowledged&&!j.replyRead);
        }
        if(a==x.layout.message&&(v==0xa||v==0xd)) {
            const auto& j=v==0xa?x.controller.memoryMaximumJournal():x.controller.dcfDeepSleepJournal();
            CHECK(j.requestAttempted&&!j.commandAcknowledged&&!j.replyRead&&j.rawReply==0);
            CHECK(j.requestedMHz==(v==0xa?uint16_t(x.argument):x.argument));
            if(v==0xa)CHECK((x.argument>>16)==2);
            CHECK(x.controller.policyMayBeActive());
        }
        const bool ok=x.calls!=x.failAt;if(!ok&&!x.acceptFailedWrite)return false;
        if(a==x.layout.response){CHECK(v==0);x.response=v;}
        else if(a==x.layout.argument)x.argument=v;
        else{CHECK(a==x.layout.message);CHECK(x.response==0);x.response=x.error;
            if(!x.coldResponses.empty()) {
                CHECK(x.coldResponseIndex<x.coldResponses.size());
                x.response=x.coldResponseIndex<x.coldResponses.size()?x.coldResponses[x.coldResponseIndex++]:0;
            }
            if(v==2)x.argument=x.version;else if(v==4)x.argument=x.header;
            else if(v==9||v==0x14||v==0xa||v==0xd) {
                if(v==9&&!x.coldReplies.empty()) {
                    CHECK(x.coldReplyIndex<x.coldReplies.size());
                    x.argument=x.coldReplyIndex<x.coldReplies.size()?x.coldReplies[x.coldReplyIndex++]:0;
                } else x.argument=x.returnedKHz;
            } else if(v==0x15)x.argument=++x.statusQueries>=x.statusAfter?x.bits:0;else CHECK(false);}
        return ok;
    }
    static void delay(void* p,uint32_t us){auto& x=*static_cast<Device*>(p);++x.delays;x.delayBudget+=us;}
    d::Status prerequisite(bool maximum,uint16_t mhz,const d::Poll& p=poll){
        return maximum?controller.requestMemoryMaximum(mhz,p,result):controller.requestDcfDeepSleepFloor(mhz,p,result);
    }
    const d::AckClockJournal& journal(bool maximum)const{
        return maximum?controller.memoryMaximumJournal():controller.dcfDeepSleepJournal();
    }
};
// Numerical transport fixtures only, never hardware/divider evidence.
struct ColdDevice {
    Device mailbox;d::Clock clock=d::Clock::Disp;
    d::ColdPresetFrequency frequency={};d::ColdPresetResult result={};
    d::ColdPresetObservation samples[3]={};
    std::vector<d::ColdPresetStage> phases;std::vector<d::ColdPresetTarget> targets;
    unsigned observations=0,failObservation=0,changeObservation=0,changeKind=0;
    uint32_t required=30000,vco=4300000;
    static d::ColdPresetObservation sample(d::Clock clock,uint8_t did) {
        const uint32_t divider=did<64?did:did<96?64+2*(did-64):did<126?128+4*(did-96):did==126?248:512;
        const uint32_t dentist=clock==d::Clock::Disp?0x180000u|did|(uint32_t(did)<<8):0x180000u|(uint32_t(did)<<24);
        return {43,4300000,dentist,did,17200000/divider};
    }
    ColdDevice(){CHECK(mailbox.init()==d::Status::Ok);mailbox.calls=0;mailbox.writes.clear();configure(clock,20,127,30000);}
    void configure(d::Clock c,uint8_t initial,uint8_t final,uint32_t need) {
        clock=c;required=need;samples[0]=sample(c,initial);samples[1]=sample(c,126);samples[2]=sample(c,final);
        mailbox.coldReplies.clear();mailbox.coldReplyIndex=0;
        observations=0;phases.clear();targets.clear();
        if(c==d::Clock::Disp&&initial!=127&&final==127)mailbox.coldReplies.push_back(69354);
        mailbox.coldReplies.push_back(final==127?33593:final==126?69354:614285);
    }
    static bool observe(void* context,d::Clock c,d::ColdPresetStage phase,const d::ColdPresetTarget& target,d::ColdPresetObservation& output) {
        auto& x=*static_cast<ColdDevice*>(context);++x.observations;x.phases.push_back(phase);x.targets.push_back(target);
        CHECK(c==x.clock&&target.programmedVcoKHz==x.vco);
        const auto n=phase==d::ColdPresetStage::InitialObservation?0:phase==d::ColdPresetStage::IntermediateObservation?1:2;
        CHECK(phase==d::ColdPresetStage::InitialObservation||phase==d::ColdPresetStage::IntermediateObservation||phase==d::ColdPresetStage::FinalObservation);
        if(!n)CHECK(target.dividerId==0&&target.programmedKHz==0);
        else CHECK(target.dividerId==x.samples[n].rawDfsDid&&target.programmedKHz==x.samples[n].programmedKHz);
        output=x.samples[n];
        if(x.observations==x.changeObservation) {
            switch(x.changeKind) {
            case 0:output.rawPll=UINT32_MAX;break;
            case 1:++output.programmedVcoKHz;break;
            case 2:output.rawDentist=UINT32_MAX;break;
            case 3:output.rawDfsDid=128;break;
            case 4:++output.programmedKHz;break;
            case 5:output.rawDentist&=~(c==d::Clock::Disp?0x80000u:0x100000u);break;
            case 6:output.rawDentist^=c==d::Clock::Disp?1u:0x1000000u;break;
            case 7:output.rawDentist^=0x100u;break;
            case 8:++output.rawPll;break;
            case 9:output=sample(c,20);break;
            default:CHECK(false);break;
            }
        }
        return x.observations!=x.failObservation;
    }
    d::Status run(const d::Poll& p=poll) {
        return mailbox.controller.requestColdPreset(clock,vco,required,{this,observe},p,frequency,result);
    }
    const d::ColdPresetJournal& journal()const{return *mailbox.controller.coldPresetJournal(clock);}
};
void coldPresets() {
    // Exact primary-source branches, including old126 -> new127 still issuing
    // the sourced126 prelude, and DPP never borrowing the DISP workaround.
    for(auto clock:{d::Clock::Disp,d::Clock::Dpp})for(uint8_t old:{uint8_t(20),uint8_t(126),uint8_t(127)})
        for(uint8_t target:{uint8_t(28),uint8_t(127)}) {
            ColdDevice x;x.configure(clock,old,target,target==127?30000:600000);
            CHECK(x.run()==d::Status::Ok);const auto& j=x.journal();
            const bool intermediate=clock==d::Clock::Disp&&old!=127&&target==127;
            CHECK(j.started&&j.complete&&j.initialObservationValid&&j.usesIntermediate==intermediate);
            CHECK(j.initial.rawDfsDid==old&&j.final.observation.rawDfsDid==target&&j.final.observationValid);
            CHECK(j.stage==d::ColdPresetStage::Complete&&x.result.stage==d::ColdPresetStage::Complete);
            CHECK(j.final.requestAttempted&&j.final.commandAcknowledged&&j.final.replyRead&&j.final.fulfilled);
            CHECK(j.lastFulfilledMHz==(target==127?30:614)&&j.final.requestedMHz==j.lastFulfilledMHz);
            CHECK(x.frequency.accepted.reportedKHz==(target==127?33593:614285));
            CHECK(x.frequency.preset.dividerId==target&&x.frequency.observation.rawDfsDid==target);
            CHECK(x.observations==(intermediate?3u:2u)&&x.result.mailbox.responseReads==(intermediate?8u:4u));
            CHECK(j.intermediate.requestAttempted==intermediate&&j.intermediate.observationValid==intermediate);
            if(intermediate)CHECK(j.intermediate.requestedMHz==69&&j.intermediate.rawReply==69354&&j.intermediate.fulfilled);
            std::vector<uint32_t> requests;
            for(size_t i=1;i<x.mailbox.writes.size();++i)
                if(x.mailbox.writes[i].address==x.mailbox.layout.message&&x.mailbox.writes[i].value==9)
                    requests.push_back(x.mailbox.writes[i-1].value);
            CHECK(requests.size()==(intermediate?2u:1u));
            if(intermediate)CHECK(requests[0]==((uint32_t(clock)<<16)|69));
            CHECK(requests.back()==((uint32_t(clock)<<16)|(target==127?30u:614u)));
            const auto before=x.mailbox.calls,observations=x.observations;
            CHECK(x.run()==d::Status::InvalidState&&x.mailbox.calls==before&&x.observations==observations);
            CHECK(x.frequency.accepted.reportedKHz==0&&x.journal().complete);
            d::Frequency ignored{};CHECK(x.mailbox.controller.requestMinimum(clock,1000,poll,ignored,x.mailbox.result)==d::Status::InvalidState);
            CHECK(x.mailbox.controller.releaseMinimaAfterQuiescence(poll,x.mailbox.result)==d::Status::ResetRequired);
            CHECK(x.mailbox.calls==before&&x.mailbox.controller.minimumMayBeActive()&&x.mailbox.controller.policyMayBeActive());
            x.mailbox.controller.clearAfterVerifiedReset();CHECK(!x.journal().started&&!x.journal().final.requestAttempted);
            CHECK(!x.mailbox.controller.minimumMayBeActive()&&!x.mailbox.controller.policyMayBeActive());
        }
    // Every actual transport callback, including writes that landed before a
    // false return. Keep intermediate fulfillment if final delivery fails.
    ColdDevice baseline;CHECK(baseline.run()==d::Status::Ok);const auto count=baseline.mailbox.calls;
    for(bool delivered:{false,true})for(unsigned at=1;at<=count;++at) {
        ColdDevice x;x.mailbox.failAt=at;x.mailbox.acceptFailedWrite=delivered;
        CHECK(x.run()!=d::Status::Ok&&x.mailbox.calls==at);
        const auto& j=x.journal();CHECK(!j.complete&&x.mailbox.controller.faulted()&&!x.mailbox.controller.ready());
        CHECK(!x.frequency.accepted.reportedKHz&&!x.frequency.observation.programmedKHz);
        CHECK(j.lastFulfilledMHz==(j.final.fulfilled?30:j.intermediate.fulfilled?69:0));
        CHECK(x.mailbox.controller.policyMayBeActive()==(j.intermediate.requestAttempted||j.final.requestAttempted));
        if(j.final.requestAttempted)CHECK(j.intermediate.fulfilled&&j.intermediate.observationValid);
        const auto before=x.mailbox.calls;x.mailbox.failAt=0;
        CHECK(x.run()==d::Status::InvalidState&&x.mailbox.calls==before);
        CHECK(x.mailbox.controller.releaseMinimaAfterQuiescence(poll,x.mailbox.result)==d::Status::InvalidState&&x.mailbox.calls==before);
    }
    // Successful all-ones transport reads are still unavailable, including
    // final-phase reads after a real intermediate fulfillment was recorded.
    for(unsigned command=0;command<4;++command)for(unsigned read:{1u,5u,6u}) {
        ColdDevice x;x.mailbox.onesAt=command*6+read;
        CHECK(x.run()==d::Status::DeviceUnavailable&&x.mailbox.calls==x.mailbox.onesAt);
        const auto& j=x.journal();CHECK(!j.complete&&x.mailbox.controller.faulted());
        CHECK(j.lastFulfilledMHz==(command>=2?69:0));
        CHECK(!x.frequency.accepted.reportedKHz);
        CHECK(x.mailbox.controller.policyMayBeActive()==(j.intermediate.requestAttempted||j.final.requestAttempted));
    }
    // Every command's negative response preserves only the actual prior ACK/
    // fulfillment history; a failed status query cannot complete that step.
    for(unsigned command=0;command<4;++command)for(uint32_t code:{0xffu,0xfeu,0xfdu,0xfcu,0x77u}) {
        ColdDevice x;x.mailbox.coldResponses={1,1,1,1};x.mailbox.coldResponses[command]=code;
        CHECK(x.run()!=d::Status::Ok&&x.mailbox.coldResponseIndex==command+1);
        const auto& j=x.journal();CHECK(!j.complete&&x.mailbox.controller.faulted());
        CHECK(j.lastFulfilledMHz==(command>=2?69:0));
        CHECK(j.intermediate.commandAcknowledged==(command!=0));
        CHECK(j.final.commandAcknowledged==(command==3));
        CHECK(!j.final.fulfilled&&!x.frequency.accepted.reportedKHz);
    }
    for(unsigned phase=1;phase<=3;++phase) {
        ColdDevice x;x.failObservation=phase;
        CHECK(x.run()==d::Status::ObservationFailed&&x.observations==phase);
        CHECK(!x.journal().complete&&x.mailbox.controller.faulted()&&!x.frequency.accepted.reportedKHz);
        CHECK(x.journal().lastFulfilledMHz==(phase==1?0:phase==2?69:30));
        CHECK(x.mailbox.controller.policyMayBeActive()==(phase>1));
    }
    for(auto clock:{d::Clock::Disp,d::Clock::Dpp})for(unsigned phase=1;phase<=(clock==d::Clock::Disp?3u:2u);++phase)
        for(unsigned kind=0;kind<10;++kind) {
            if((kind==7&&clock==d::Clock::Dpp)||(phase==1&&(kind==8||kind==9)))continue;
            ColdDevice x;x.configure(clock,20,127,30000);x.changeObservation=phase;x.changeKind=kind;
            CHECK(x.run()==d::Status::InvalidClockObservation&&x.mailbox.controller.faulted());
            CHECK(!x.journal().complete&&!x.frequency.accepted.reportedKHz);
        }
    // Legal but contradictory replies cannot become a DTO reference.
    for(unsigned phase=0;phase<2;++phase)for(uint32_t reply:{0u,1u,1950001u,600000u}) {
        ColdDevice x;x.mailbox.coldReplies[phase]=reply;
        CHECK(x.run()==d::Status::InvalidFrequency&&x.mailbox.controller.faulted());
        CHECK(!x.frequency.accepted.reportedKHz&&!x.journal().complete);
    }
    // Complete two-command sequence needs eight total response reads. A
    // budget cannot reset between126 and127, even after126 was fulfilled.
    for(uint32_t budget=1;budget<=8;++budget) {
        ColdDevice x;const auto status=x.run({budget,10});
        CHECK((status==d::Status::Ok)==(budget==8));CHECK(x.result.mailbox.responseReads<=budget);
        if(budget<8)CHECK(status==d::Status::Timeout&&x.mailbox.controller.faulted()&&!x.journal().complete);
        if(budget==4)CHECK(x.journal().intermediate.fulfilled&&!x.journal().final.requestAttempted&&x.journal().lastFulfilledMHz==69);
    }
    for(auto clock:{d::Clock::Disp,d::Clock::Dpp})for(uint32_t bits:{0u,1u,2u,4u}) {
        ColdDevice x;x.configure(clock,20,127,30000);x.mailbox.bits=bits;
        CHECK(x.run()==d::Status::Timeout&&x.result.mailbox.responseReads<=poll.maxReads);
        CHECK(x.mailbox.delayBudget<=poll.maxReads*poll.delayUs&&!x.journal().complete);
    }
    for(unsigned n=0;n<6;++n) {
        ColdDevice x;auto observer=d::ColdPresetIo{&x,ColdDevice::observe};auto p=poll;
        if(n==0)x.clock=d::Clock::Dcf;if(n==1)x.vco=0;if(n==2)x.required=0;
        if(n==3)x.required=1950001;if(n==4)observer.observe=nullptr;if(n==5)p.maxReads=0;
        CHECK(x.mailbox.controller.requestColdPreset(x.clock,x.vco,x.required,observer,p,x.frequency,x.result)!=d::Status::Ok);
        CHECK(x.mailbox.calls==0&&x.observations==0&&x.mailbox.controller.ready());
    }
    {ColdDevice x;x.mailbox.coldReplies.clear();d::Frequency f{};CHECK(x.mailbox.controller.requestMinimum(d::Clock::Disp,600,poll,f,x.mailbox.result)==d::Status::Ok);
        const auto before=x.mailbox.calls;CHECK(x.run()==d::Status::InvalidState&&x.mailbox.calls==before&&x.observations==0);}
    // DISP and DPP share one DAL owner but retain independent one-shot
    // journals. Taking DPP cannot overwrite the completed DISP history.
    {ColdDevice x;CHECK(x.run()==d::Status::Ok);
        x.configure(d::Clock::Dpp,127,28,600000);CHECK(x.run()==d::Status::Ok);
        const auto* disp=x.mailbox.controller.coldPresetJournal(d::Clock::Disp);
        CHECK(disp&&disp->complete&&disp->usesIntermediate&&disp->lastFulfilledMHz==30);
        CHECK(x.journal().complete&&!x.journal().usesIntermediate&&x.journal().lastFulfilledMHz==614);
        CHECK(x.observations==2&&x.frequency.observation.programmedKHz==614285);}
    // Only an independently verified reset permits a new epoch; the helper
    // needs real mailbox initialization again before another acquisition.
    {ColdDevice x;CHECK(x.run()==d::Status::Ok);x.mailbox.controller.clearAfterVerifiedReset();
        const auto before=x.mailbox.calls;x.configure(d::Clock::Disp,127,127,30000);
        CHECK(x.run()==d::Status::InvalidState&&x.mailbox.calls==before&&x.observations==0);
        CHECK(x.mailbox.init()==d::Status::Ok);CHECK(x.run()==d::Status::Ok);
        CHECK(!x.journal().usesIntermediate&&x.journal().complete&&x.observations==2);}
    CHECK(baseline.mailbox.controller.coldPresetJournal(d::Clock::Dcf)==nullptr);
}
}
int main(){
    coldPresets();
    Device baseline;CHECK(baseline.layout.message==0x1628a*4&&baseline.layout.argument==0x16273*4&&baseline.layout.response==0x16274*4);
    CHECK(baseline.init()==d::Status::Ok&&baseline.controller.firmwareVersion()==0x4e6a00);const auto initCalls=baseline.calls;
    baseline.calls=0;d::Frequency f={};CHECK(baseline.controller.requestMinimum(d::Clock::Dpp,594,poll,f,baseline.result)==d::Status::Ok);
    const auto requestCalls=baseline.calls;CHECK(f.reportedKHz==600000&&f.fulfilledBits==0x3f);
    CHECK(baseline.controller.minimumMayBeActive()&&baseline.result.minimumRequestAttempted);
    CHECK(baseline.writes[7].address==baseline.layout.argument&&baseline.writes[7].value==((9u<<16)|594));
    const auto calls=baseline.calls;CHECK(baseline.controller.requestMinimum(d::Clock::Dpp,593,poll,f,baseline.result)==d::Status::InvalidArgument);CHECK(baseline.calls==calls);
    for(auto clock:{d::Clock::Disp,d::Clock::Dpp,d::Clock::Dcf,d::Clock::Dtb}){
        Device x;CHECK(x.init()==d::Status::Ok);CHECK(x.controller.requestMinimum(clock,600,poll,f,x.result)==d::Status::Ok);
        CHECK(x.controller.ready()&&!x.controller.faulted());
    }
    // A write returning false may already have executed on the device.
    for(bool accepted:{false,true})for(bool request:{false,true})for(unsigned at=1;at<=(request?requestCalls:initCalls);++at){
        Device x;if(request)CHECK(x.init()==d::Status::Ok);x.calls=0;x.failAt=at;x.acceptFailedWrite=accepted;
        f={123,456};auto status=request?x.controller.requestMinimum(d::Clock::Disp,600,poll,f,x.result):x.init();
        CHECK(status==d::Status::ReadFailed||status==d::Status::WriteFailed);CHECK(x.controller.faulted()&&!x.controller.ready());
        CHECK(f.reportedKHz==123&&f.fulfilledBits==456);
        const auto stopped=x.calls;x.failAt=0;
        CHECK(x.controller.requestMinimum(d::Clock::Disp,600,poll,f,x.result)==d::Status::InvalidState);CHECK(x.calls==stopped);
    }
    for(unsigned code:{0xffu,0xfeu,0xfdu,0xfcu,0x77u}){
        Device x;CHECK(x.init()==d::Status::Ok);x.error=code;
        CHECK(x.controller.requestMinimum(d::Clock::Dpp,600,poll,f,x.result)!=d::Status::Ok);
        CHECK(x.controller.minimumMayBeActive()&&x.controller.faulted());
    }
    for(bool request:{false,true})for(unsigned at:{1u,5u,6u,7u,11u,12u}){Device x;
        if(request)CHECK(x.init()==d::Status::Ok);x.calls=0;x.onesAt=at;f={7,8};
        CHECK((request?x.controller.requestMinimum(d::Clock::Dpp,600,poll,f,x.result):x.init())==d::Status::DeviceUnavailable);
        CHECK(x.controller.faulted()&&f.reportedKHz==7&&f.fulfilledBits==8);}
    {Device x;CHECK(x.init()==d::Status::Ok);x.statusAfter=3;
        CHECK(x.controller.requestMinimum(d::Clock::Dpp,600,poll,f,x.result)==d::Status::Ok);
        CHECK(x.statusQueries==3&&x.delays==2&&f.reportedKHz==600000);}
    for(unsigned invalid=0;invalid<4;++invalid){Device x;CHECK(x.init()==d::Status::Ok);const auto before=x.calls;
        const auto clock=invalid==0?d::Clock(0):d::Clock::Dpp;const uint16_t mhz=invalid==1?0:invalid==2?1951:600;
        const auto p=invalid==3?d::Poll{0,10}:poll;
        CHECK(x.controller.requestMinimum(clock,mhz,p,f,x.result)==d::Status::InvalidArgument);CHECK(x.calls==before);}
    for(uint32_t value:{0u,0x4e69ffu}){Device x;x.version=value;CHECK(x.init()==d::Status::UnsupportedFirmware);}
    {Device x;x.header=2;CHECK(x.init()==d::Status::UnsupportedFirmware);}
    for(uint32_t value:{0u,599999u,1950001u}){Device x;CHECK(x.init()==d::Status::Ok);x.returnedKHz=value;
        f={7,8};CHECK(x.controller.requestMinimum(d::Clock::Dpp,600,poll,f,x.result)==d::Status::InvalidFrequency);CHECK(f.reportedKHz==7&&f.fulfilledBits==8);}
    // Neither command ACK nor the clock's bit without DPREFCLK is completion.
    for(uint32_t bits:{0u,2u,4u}){Device x;CHECK(x.init()==d::Status::Ok);x.bits=bits;f={7,8};
        CHECK(x.controller.requestMinimum(d::Clock::Dpp,600,poll,f,x.result)==d::Status::Timeout);
        CHECK(x.result.responseReads<=poll.maxReads&&x.delays<=poll.maxReads&&x.delayBudget<=poll.maxReads*poll.delayUs);
        CHECK(f.reportedKHz==7&&x.controller.minimumMayBeActive());}
    {Device x;x.response=0;CHECK(x.init()==d::Status::Timeout);CHECK(x.writes.empty()&&x.result.responseReads==poll.maxReads);}
    {Device x;CHECK(x.init()==d::Status::Ok);x.error=0;CHECK(x.controller.requestMinimum(d::Clock::Dpp,600,poll,f,x.result)==d::Status::Timeout);CHECK(x.controller.minimumMayBeActive());}
    {Device x;x.onesAt=1;CHECK(x.init()==d::Status::DeviceUnavailable);CHECK(x.writes.empty());}
    for(unsigned field=0;field<7;++field){Device x;auto l=x.layout;auto io=x.io();auto p=poll;
        if(field==0)++l.message;if(field==1)++l.argument;if(field==2)l.barBytes=4;
        if(field==3)p.maxReads=0;if(field==4)p.delayUs=0;if(field==5)p={1000000,1000};if(field==6)io.delayUs=nullptr;
        CHECK(x.controller.initialize(l,io,p,x.result)==d::Status::InvalidArgument);CHECK(x.calls==0);}
    d::Layout l={};auto ip=ips();ip.blocks[0].major=12;CHECK(d::buildLayout(ip,0x80000,l)==d::Status::UnsupportedLayout&&!l.valid);
    CHECK(d::buildLayout(ips(),0x1628a*4+3,l)==d::Status::OutsideBar&&!l.valid);
    CHECK(d::buildLayout(ips(),0x1628a*4+4,l)==d::Status::Ok);
    // Preserve the legacy display-only zero command protocol. This model does
    // not prove firmware interprets zero as full constraint withdrawal.
    {Device x;CHECK(x.init()==d::Status::Ok);
        for(auto c:{d::Clock::Disp,d::Clock::Dpp,d::Clock::Dcf,d::Clock::Dtb})CHECK(x.controller.requestMinimum(c,600,poll,f,x.result)==d::Status::Ok);
        x.calls=0;x.writes.clear();x.returnedKHz=0;
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::Ok);
        CHECK(!x.controller.minimumMayBeActive()&&x.writes.size()==12);
        const uint32_t clocks[]={8,9,11,12};
        for(unsigned n=0;n<4;++n)CHECK(x.writes[n*3+1].address==x.layout.argument&&x.writes[n*3+1].value==(clocks[n]<<16));
        const auto before=x.calls;CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::Ok);CHECK(x.calls==before);
    }
    for(bool accepted:{false,true})for(unsigned at=1;at<=24;++at){Device x;CHECK(x.init()==d::Status::Ok);
        for(auto c:{d::Clock::Disp,d::Clock::Dpp,d::Clock::Dcf,d::Clock::Dtb})CHECK(x.controller.requestMinimum(c,600,poll,f,x.result)==d::Status::Ok);
        x.calls=0;x.failAt=at;x.acceptFailedWrite=accepted;x.returnedKHz=0;
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)!=d::Status::Ok);
        CHECK(x.controller.minimumMayBeActive()&&x.controller.faulted());
        x.failAt=0;CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::InvalidState);
    }
    // UCLK is a distinct protocol: raw reply, no inferred kHz, both 0x20/0x04
    // required. These host mailbox models do not establish target clocks.
    d::MemoryMinimum memory{};
    for(uint32_t raw:{0u,1u,600u,600000u,0xabcdef00u}){
        Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;x.writes.clear();x.returnedKHz=raw;x.bits=0x24;
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::Ok);
        CHECK(memory.requestedMHz==600&&memory.rawReply==raw&&memory.fulfilledBits==0x24);
        const auto& journal=x.controller.memoryJournal();
        CHECK(journal.minimumMHz==600&&journal.attemptedMHz==600&&journal.rawReply==raw&&journal.fulfilledBits==0x24);
        CHECK(journal.requestAttempted&&journal.commandAcknowledged&&journal.replyRead&&journal.fulfilled);
        CHECK(x.writes.size()==6&&x.writes[1].value==((2u<<16)|600)&&x.writes[2].value==9&&x.writes[5].value==0x15);
        CHECK(x.controller.minimumMayBeActive()&&x.controller.ready());
        const auto callsBefore=x.calls;
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::ResetRequired);
        CHECK(x.calls==callsBefore&&x.controller.minimumMayBeActive()&&journal.minimumMHz==600);
        CHECK(x.controller.requestMemoryMinimum(599,poll,memory,x.result)==d::Status::InvalidArgument);
        CHECK(x.calls==callsBefore&&memory.requestedMHz==0&&journal.minimumMHz==600);
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::Ok);
        CHECK(x.controller.requestMemoryMinimum(65535,poll,memory,x.result)==d::Status::Ok);
        CHECK(x.controller.memoryJournal().minimumMHz==65535);
        x.controller.clearAfterVerifiedReset();
        CHECK(!x.controller.memoryJournal().requestAttempted&&!x.controller.minimumMayBeActive()&&!x.controller.ready());
        CHECK(x.controller.memoryJournal().minimumMHz==0);
    }
    for(uint32_t bits:{0u,4u,0x20u,0x1fu}){
        Device x;CHECK(x.init()==d::Status::Ok);x.bits=bits;
        memory={7,8,9};CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::Timeout);
        CHECK(memory.requestedMHz==0&&memory.rawReply==0&&memory.fulfilledBits==0);
        const auto& journal=x.controller.memoryJournal();
        CHECK(journal.commandAcknowledged&&journal.replyRead&&!journal.fulfilled&&journal.minimumMHz==0);
        CHECK(journal.fulfilledBits==bits&&x.controller.minimumMayBeActive());
        CHECK(x.result.responseReads<=poll.maxReads&&x.delays<=poll.maxReads&&x.delayBudget<=poll.maxReads*poll.delayUs);
    }
    {Device x;CHECK(x.init()==d::Status::Ok);x.statusAfter=3;x.bits=0x24;
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::Ok);
        CHECK(x.statusQueries==3&&x.delays==2);}
    // Every request read/write boundary, including delivered-but-failed writes,
    // keeps the attempted journal and an earlier successfully fulfilled floor.
    for(bool previous:{false,true})for(bool accepted:{false,true})for(unsigned at=1;at<=requestCalls;++at){
        Device x;CHECK(x.init()==d::Status::Ok);
        if(previous)CHECK(x.controller.requestMemoryMinimum(500,poll,memory,x.result)==d::Status::Ok);
        x.calls=0;x.failAt=at;x.acceptFailedWrite=accepted;memory={7,8,9};
        const auto status=x.controller.requestMemoryMinimum(600,poll,memory,x.result);
        CHECK(status==d::Status::ReadFailed||status==d::Status::WriteFailed);
        CHECK(x.controller.faulted()&&!x.controller.ready()&&memory.requestedMHz==0&&memory.rawReply==0&&memory.fulfilledBits==0);
        const auto& journal=x.controller.memoryJournal();
        CHECK(journal.minimumMHz==(previous?500:0));
        CHECK(journal.requestAttempted==(previous||at>=4));
        if(at>=4)CHECK(journal.attemptedMHz==600&&!journal.fulfilled);
        if(at==5)CHECK(!journal.commandAcknowledged);
        if(at==6)CHECK(journal.commandAcknowledged&&!journal.replyRead);
        const auto stopped=x.calls;x.failAt=0;
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::InvalidState);
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::InvalidState&&x.calls==stopped);
    }
    for(unsigned at:{1u,5u,6u,7u,11u,12u}){
        Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;x.onesAt=at;
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::DeviceUnavailable);
        CHECK(x.controller.faulted()&&memory.requestedMHz==0&&!x.controller.memoryJournal().fulfilled);
        if(at==6)CHECK(x.controller.memoryJournal().rawReply==UINT32_MAX&&!x.controller.memoryJournal().replyRead);
    }
    for(unsigned code:{0xffu,0xfeu,0xfdu,0xfcu,0x77u,0u}){
        Device x;CHECK(x.init()==d::Status::Ok);x.error=code;
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)!=d::Status::Ok);
        CHECK(x.controller.minimumMayBeActive()&&x.controller.memoryJournal().requestAttempted);
        CHECK(!x.controller.memoryJournal().commandAcknowledged&&!x.controller.memoryJournal().fulfilled);
    }
    {Device x;CHECK(x.init()==d::Status::Ok);const auto callsBefore=x.calls;
        CHECK(x.controller.requestMemoryMinimum(0,poll,memory,x.result)==d::Status::InvalidArgument);
        CHECK(x.controller.requestMemoryMinimum(600,{0,10},memory,x.result)==d::Status::InvalidArgument);
        CHECK(x.controller.requestMemoryMinimum(600,{32,0},memory,x.result)==d::Status::InvalidArgument);
        CHECK(x.controller.requestMemoryMinimum(600,{1000000,1000},memory,x.result)==d::Status::InvalidArgument);
        CHECK(x.calls==callsBefore&&!x.controller.memoryJournal().requestAttempted);
        CHECK(x.controller.requestMinimum(d::Clock(2),600,poll,f,x.result)==d::Status::InvalidArgument&&x.calls==callsBefore);
    }
    {Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;x.writes.clear();
        CHECK(x.controller.requestMemoryMinimum(600,{1,10},memory,x.result)==d::Status::Timeout);
        CHECK(x.writes.empty()&&!x.controller.memoryJournal().requestAttempted&&!x.controller.minimumMayBeActive());}
    {Device x;x.version=0x4e69ff;CHECK(x.init()==d::Status::UnsupportedFirmware);const auto before=x.calls;
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::InvalidState&&x.calls==before);}
    {Device x;CHECK(x.init()==d::Status::Ok);
        CHECK(x.controller.requestMinimum(d::Clock::Disp,600,poll,f,x.result)==d::Status::Ok);
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::Ok);
        const auto callsBefore=x.calls;
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::ResetRequired);
        CHECK(x.calls==callsBefore&&x.controller.minimumMayBeActive());}
    // Actual DAL wait-policy command, distinct from disabling memory switching.
    for(uint32_t raw:{0u,1u,0xabcdef01u}) {
        Device x; CHECK(x.init()==d::Status::Ok);x.calls=0;x.writes.clear();x.returnedKHz=raw;
        CHECK(x.controller.disableDmcubMclkWait(poll,x.result)==d::Status::Ok);
        const auto& j=x.controller.dmcubWaitJournal();
        CHECK(j.requestAttempted&&j.commandAcknowledged&&j.replyRead&&j.rawReply==raw);
        CHECK(x.controller.policyMayBeActive()&&!x.controller.minimumMayBeActive());
        CHECK(x.writes.size()==3&&x.writes[1].value==0&&x.writes[2].value==0x14);
        const auto before=x.calls;
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::ResetRequired&&x.calls==before);
        x.controller.clearAfterVerifiedReset();CHECK(!x.controller.policyMayBeActive());
    }
    for(bool previous:{false,true})for(bool accepted:{false,true})for(unsigned at=1;at<=6;++at) {
        Device x;CHECK(x.init()==d::Status::Ok);
        if(previous)CHECK(x.controller.disableDmcubMclkWait(poll,x.result)==d::Status::Ok);
        x.calls=0;x.failAt=at;x.acceptFailedWrite=accepted;
        const auto status=x.controller.disableDmcubMclkWait(poll,x.result);
        CHECK(status==d::Status::ReadFailed||status==d::Status::WriteFailed);
        CHECK(x.controller.faulted()&&!x.controller.ready());
        const auto& j=x.controller.dmcubWaitJournal();
        CHECK(j.requestAttempted==(previous||at>=4));
        if(at==4||at==5)CHECK(!j.commandAcknowledged&&!j.replyRead);
        if(at==6)CHECK(j.commandAcknowledged&&!j.replyRead);
        const auto stopped=x.calls;
        CHECK(x.controller.disableDmcubMclkWait(poll,x.result)==d::Status::InvalidState&&x.calls==stopped);
    }
    for(unsigned code:{0xffu,0xfeu,0xfdu,0xfcu,0x77u,0u}) {
        Device x;CHECK(x.init()==d::Status::Ok);x.error=code;
        CHECK(x.controller.disableDmcubMclkWait(poll,x.result)!=d::Status::Ok);
        CHECK(x.controller.policyMayBeActive()&&!x.controller.dmcubWaitJournal().commandAcknowledged);
    }
    for(unsigned at:{1u,5u,6u}) {
        Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;x.onesAt=at;
        CHECK(x.controller.disableDmcubMclkWait(poll,x.result)==d::Status::DeviceUnavailable&&x.controller.faulted());
    }
    {Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;
        CHECK(x.controller.disableDmcubMclkWait({0,10},x.result)==d::Status::InvalidArgument&&x.calls==0);
        CHECK(x.controller.disableDmcubMclkWait({1,10},x.result)==d::Status::Timeout);
        CHECK(!x.controller.policyMayBeActive());}
    // These ACK-only commands use actual pinned DAL wire encodings. This is
    // host protocol/fault coverage, not firmware execution or a clock sample.
    for(bool maximum:{false,true})for(uint16_t mhz:{uint16_t(1),uint16_t(600),uint16_t(65535)})
        for(uint32_t raw:{0u,1u,0xabcdef01u}) {
        Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;x.writes.clear();x.returnedKHz=raw;
        CHECK(x.prerequisite(maximum,mhz)==d::Status::Ok);
        const auto& j=x.journal(maximum);
        CHECK(j.requestAttempted&&j.commandAcknowledged&&j.replyRead&&j.requestedMHz==mhz&&j.rawReply==raw);
        CHECK(x.writes.size()==3&&x.writes[1].value==((maximum?2u<<16:0u)|mhz));
        CHECK(x.writes[2].value==(maximum?0xau:0xdu));
        CHECK(x.result.responseReads==2&&x.result.hardMinBits==0&&x.statusQueries==0);
        CHECK(x.result.stage==d::Stage::Complete&&x.controller.policyMayBeActive()&&!x.controller.minimumMayBeActive());
        const auto before=x.calls;
        CHECK(x.prerequisite(maximum,mhz)==d::Status::InvalidState&&x.calls==before);
        CHECK(x.prerequisite(maximum,500)==d::Status::InvalidState&&x.calls==before);
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::ResetRequired&&x.calls==before);
        CHECK(j.requestedMHz==mhz&&j.rawReply==raw&&j.commandAcknowledged&&j.replyRead);
        x.controller.clearAfterVerifiedReset();
        CHECK(!x.journal(maximum).requestAttempted&&x.journal(maximum).requestedMHz==0&&!x.controller.policyMayBeActive());
        CHECK(x.init()==d::Status::Ok&&x.prerequisite(maximum,mhz)==d::Status::Ok);
    }
    // Both independent journals survive failure of the other command. Every
    // boundary includes writes that may have reached the device despite false.
    for(bool maximum:{false,true})for(bool other:{false,true})for(bool accepted:{false,true})for(unsigned at=1;at<=6;++at){
        Device x;CHECK(x.init()==d::Status::Ok);x.returnedKHz=123;
        if(other)CHECK(x.prerequisite(!maximum,500)==d::Status::Ok);
        x.calls=0;x.failAt=at;x.acceptFailedWrite=accepted;
        const auto s=x.prerequisite(maximum,600);
        CHECK(s==d::Status::ReadFailed||s==d::Status::WriteFailed);
        CHECK(x.controller.faulted()&&!x.controller.ready());
        const auto& j=x.journal(maximum);
        CHECK(j.requestAttempted==(at>=4));CHECK(j.commandAcknowledged==(at==6));CHECK(!j.replyRead);
        CHECK(j.requestedMHz==(at>=4?600u:0u));
        CHECK(x.controller.policyMayBeActive()==(other||at>=4));
        if(other){const auto& earlier=x.journal(!maximum);
            CHECK(earlier.requestedMHz==500&&earlier.rawReply==123&&earlier.commandAcknowledged&&earlier.replyRead);}
        const auto before=x.calls;x.failAt=0;
        CHECK(x.prerequisite(maximum,600)==d::Status::InvalidState&&x.calls==before);
        CHECK(x.prerequisite(!maximum,600)==d::Status::InvalidState&&x.calls==before);
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::InvalidState&&x.calls==before);
        CHECK(x.init()==d::Status::InvalidState&&x.calls==before);
    }
    for(bool maximum:{false,true})for(unsigned at:{1u,5u,6u}){
        Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;x.onesAt=at;
        CHECK(x.prerequisite(maximum,600)==d::Status::DeviceUnavailable&&x.controller.faulted());
        const auto& j=x.journal(maximum);CHECK(j.requestAttempted==(at>=4)&&!j.replyRead);
        if(at==6)CHECK(j.commandAcknowledged&&j.rawReply==UINT32_MAX);
    }
    for(bool maximum:{false,true})for(unsigned code:{0xffu,0xfeu,0xfdu,0xfcu,0x77u,0u}){
        Device x;CHECK(x.init()==d::Status::Ok);x.error=code;
        CHECK(x.prerequisite(maximum,600)!=d::Status::Ok);
        CHECK(x.journal(maximum).requestAttempted&&!x.journal(maximum).commandAcknowledged&&!x.journal(maximum).replyRead);
        CHECK(x.controller.policyMayBeActive()&&x.controller.faulted());
        CHECK(x.result.responseReads<=poll.maxReads&&x.delayBudget<=poll.maxReads*poll.delayUs);
    }
    for(bool maximum:{false,true}){
        Device x;CHECK(x.init()==d::Status::Ok);x.calls=0;
        CHECK(x.prerequisite(maximum,0)==d::Status::InvalidArgument);
        for(const d::Poll bad: {d::Poll{0,10},d::Poll{32,0},d::Poll{1000000,1000}})
            CHECK(x.prerequisite(maximum,600,bad)==d::Status::InvalidArgument);
        CHECK(x.calls==0&&!x.journal(maximum).requestAttempted&&x.controller.ready());
        CHECK(x.prerequisite(maximum,600,{1,10})==d::Status::Timeout);
        CHECK(!x.journal(maximum).requestAttempted&&!x.controller.policyMayBeActive()&&x.controller.faulted());
    }
    {Device x;CHECK(x.init()==d::Status::Ok);const auto before=x.calls;
        CHECK(x.controller.requestDcfDeepSleepFloor(65536,poll,x.result)==d::Status::InvalidArgument);
        CHECK(x.controller.requestDcfDeepSleepFloor(UINT32_MAX,poll,x.result)==d::Status::InvalidArgument);
        CHECK(x.calls==before&&!x.controller.dcfDeepSleepJournal().requestAttempted);}
    {Device x;CHECK(x.init()==d::Status::Ok);
        CHECK(x.controller.requestMemoryMinimum(500,poll,memory,x.result)==d::Status::Ok);
        const auto before=x.calls;
        CHECK(x.controller.requestMemoryMaximum(499,poll,x.result)==d::Status::InvalidArgument&&x.calls==before);
        CHECK(!x.controller.memoryMaximumJournal().requestAttempted&&x.controller.memoryJournal().minimumMHz==500);
        CHECK(x.controller.requestMemoryMaximum(500,poll,x.result)==d::Status::Ok);
        CHECK(x.controller.requestMemoryMinimum(500,poll,memory,x.result)==d::Status::Ok);
        const auto held=x.calls;memory={7,8,9};
        CHECK(x.controller.requestMemoryMinimum(501,poll,memory,x.result)==d::Status::InvalidArgument&&x.calls==held);
        CHECK(memory.requestedMHz==0&&memory.rawReply==0&&memory.fulfilledBits==0);
        CHECK(x.controller.memoryJournal().minimumMHz==500&&x.controller.memoryMaximumJournal().requestedMHz==500);}
    {Device x;CHECK(x.init()==d::Status::Ok);
        CHECK(x.controller.requestMemoryMaximum(600,poll,x.result)==d::Status::Ok);
        const auto before=x.calls;
        CHECK(x.controller.requestMemoryMinimum(601,poll,memory,x.result)==d::Status::InvalidArgument&&x.calls==before);
        CHECK(!x.controller.memoryJournal().requestAttempted);
        CHECK(x.controller.requestMemoryMinimum(600,poll,memory,x.result)==d::Status::Ok);}
    for(bool maximum:{false,true}){Device x;CHECK(x.init()==d::Status::Ok);
        CHECK(x.controller.requestMinimum(d::Clock::Disp,600,poll,f,x.result)==d::Status::Ok);
        CHECK(x.prerequisite(maximum,600)==d::Status::Ok);const auto before=x.calls;
        CHECK(x.controller.releaseMinimaAfterQuiescence(poll,x.result)==d::Status::ResetRequired&&x.calls==before);
        CHECK(x.controller.minimumMayBeActive()&&x.controller.policyMayBeActive());}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
