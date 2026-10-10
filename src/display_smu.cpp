/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2021-2022 Advanced Micro Devices, Inc.
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
// Port of DCN3.2 DAL mailbox and hard-min completion protocol from Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0. Stricter bounded failures below.
#include "hackynavi/display_smu.hpp"
namespace hackynavi { namespace displaysmu { namespace {
bool valid(const Layout& l){
    return l.valid&&l.barBytes>=4&&!(l.message&3)&&l.message>=0x28a*4&&
        l.argument==l.message-(0x28a-0x273)*4&&l.response==l.argument+4&&l.message<=l.barBytes-4;
}
bool valid(const Poll& p){return p.maxReads&&p.maxReads<=1000000&&p.delayUs&&p.delayUs<=1000&&uint64_t(p.maxReads)*p.delayUs<=2000000;}
int index(Clock clock){switch(clock){case Clock::Disp:return 0;case Clock::Dpp:return 1;case Clock::Dcf:return 2;case Clock::Dtb:return 3;}return -1;}
uint32_t mask(int n){const uint32_t bits[]={1,2,8,16};return bits[n]|4u;}
Status decode(uint32_t v){switch(v){case 1:return Status::Ok;case 0xff:return Status::CommandFailed;
    case 0xfe:return Status::UnknownCommand;case 0xfd:return Status::BadPrerequisite;case 0xfc:return Status::Busy;default:return Status::UnknownResponse;}}
}
Status buildLayout(const Navi31IpLayout& ip,uint64_t bar,Layout& output){
    output={};ResolvedRegister r={};Layout l={};const uint32_t offsets[]={0x28a,0x273,0x274};
    const uint16_t hardwareIds[]={11,34,255,1,42,43};
    const uint8_t majorVersions[]={11,3,13,13,6,6};
    if(!ip.valid)return Status::UnsupportedLayout;
    for(size_t n=0;n<kNavi31BlockCount;++n){const auto& b=ip.blocks[n];
        if(b.hardwareId!=hardwareIds[n]||b.major!=majorVersions[n]||b.minor||b.revision||b.dieId||b.instance||
            !b.baseCount||b.baseCount>kNavi31MaxBases)return Status::UnsupportedLayout;}
    uint32_t* fields[]={&l.message,&l.argument,&l.response};
    for(size_t n=0;n<3;++n){auto s=resolveNavi31Register(ip,Navi31Block::Mp1,0,offsets[n],bar,r);
        if(s!=IpLayoutStatus::Ok)return s==IpLayoutStatus::OutsideBar||s==IpLayoutStatus::AddressOverflow?Status::OutsideBar:Status::UnsupportedLayout;
        *fields[n]=r.mmioByteOffset;}
    l.valid=true;l.barBytes=bar;if(!valid(l))return Status::UnsupportedLayout;output=l;return Status::Ok;
}
Controller::Controller():layout_{},io_{},ready_(false),faulted_(false),minimum_(false),version_(0),requested_{},memory_{},dmcubWait_{},memoryMaximum_{},dcfDeepSleep_{},coldPresets_{}{}
void Controller::clearAfterVerifiedReset(){layout_={};io_={};ready_=faulted_=minimum_=false;version_=0;for(auto& r:requested_)r=0;memory_={};dmcubWait_={};memoryMaximum_={};dcfDeepSleep_={};for(auto& j:coldPresets_)j={};}
Status Controller::finish(Status s,Result& r){r.status=s;return s;}
Status Controller::read(uint32_t a,uint32_t& v,Result& r){
    ++r.reads;r.lastRegister=a;if(!io_.read32(io_.context,a,v))return Status::ReadFailed;
    r.lastValue=v;return v==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::write(uint32_t a,uint32_t v,Result& r){
    ++r.writes;r.lastRegister=a;r.lastValue=v;r.mailboxWriteAttempted=true;
    return io_.write32(io_.context,a,v)?Status::Ok:Status::WriteFailed;
}
Status Controller::response(const Poll& p,Result& r){
    while(r.responseReads<p.maxReads){++r.responseReads;auto s=read(layout_.response,r.response,r);
        if(s!=Status::Ok)return s;
        if(r.response)return decode(r.response);
        if(r.responseReads<p.maxReads){++r.delays;io_.delayUs(io_.context,p.delayUs);}}
    return Status::Timeout;
}
Status Controller::send(uint32_t message,uint32_t arg,const Poll& p,uint32_t& output,Result& r,ColdPresetStep* cold){
    r.message=message;r.argument=arg;r.stage=Stage::PreviousResponse;auto s=response(p,r);if(s!=Status::Ok)return s;
    // Do not launch another command if no budget remains to observe completion.
    if(r.responseReads>=p.maxReads)return Status::Timeout;
    r.stage=Stage::ClearResponse;s=write(layout_.response,0,r);if(s!=Status::Ok)return s;
    r.stage=Stage::Argument;s=write(layout_.argument,arg,r);if(s!=Status::Ok)return s;
    r.stage=Stage::Message;if(message==9)r.minimumRequestAttempted=minimum_=true;
    if(cold)cold->requestAttempted=true; // A failed callback may have delivered it.
    const bool memoryRequest=message==9&&(arg>>16)==2;
    if(memoryRequest){
        // Journal BEFORE the write: a callback returning false may still have
        // delivered the request. Preserve an earlier fulfilled floor on error.
        memory_.attemptedMHz=uint16_t(arg);memory_.requestAttempted=true;
        memory_.commandAcknowledged=memory_.replyRead=memory_.fulfilled=false;
        memory_.rawReply=memory_.fulfilledBits=0;
    }
    const bool disableWait=message==0x14&&arg==0;
    if(disableWait)dmcubWait_={0,true,false,false};
    AckClockJournal* prerequisite=message==0xa&&(arg>>16)==2?&memoryMaximum_:
        message==0xd?&dcfDeepSleep_:nullptr;
    if(prerequisite)*prerequisite={message==0xa?uint32_t(uint16_t(arg)):arg,0,true,false,false};
    s=write(layout_.message,message,r);if(s!=Status::Ok)return s;
    r.stage=Stage::Response;s=response(p,r);if(s!=Status::Ok)return s;
    if(cold)cold->commandAcknowledged=true;
    if(memoryRequest)memory_.commandAcknowledged=true;
    if(disableWait)dmcubWait_.commandAcknowledged=true;
    if(prerequisite)prerequisite->commandAcknowledged=true;
    r.stage=Stage::Output;s=read(layout_.argument,output,r);
    if(cold){cold->rawReply=output;cold->replyRead=s==Status::Ok;}
    if(memoryRequest){memory_.rawReply=output;memory_.replyRead=s==Status::Ok;}
    if(disableWait){dmcubWait_.rawReply=output;dmcubWait_.replyRead=s==Status::Ok;}
    if(prerequisite){prerequisite->rawReply=output;prerequisite->replyRead=s==Status::Ok;}
    return s;
}
Status Controller::initialize(const Layout& l,const Io& io,const Poll& p,Result& r){
    r={};if(ready_||faulted_)return finish(Status::InvalidState,r);
    if(!valid(l)||!valid(p)||!io.read32||!io.write32||!io.delayUs)return finish(Status::InvalidArgument,r);
    layout_=l;io_=io;uint32_t version=0,header=0;auto s=send(2,0,p,version,r);
    if(s==Status::Ok)s=send(4,0,p,header,r);
    // ReturnHardMinStatus is supported on Navi31 only from PMFW 78.106.0.
    // No success fallback on older firmware without a completion contract.
    if(s==Status::Ok&&(version<0x4e6a00||header!=1))s=Status::UnsupportedFirmware;
    if(s!=Status::Ok){faulted_=true;return finish(s,r);}
    version_=version;ready_=true;r.stage=Stage::Complete;return finish(Status::Ok,r);
}
Status Controller::requestMinimum(Clock clock,uint16_t mhz,const Poll& p,Frequency& output,Result& r){
    r={};if(!ready_||faulted_)return finish(Status::InvalidState,r);const int n=index(clock);
    if(n>=0&&n<2&&coldPresets_[n].started)return finish(Status::InvalidState,r);
    if(n<0||!mhz||!valid(p)||(n<2&&mhz>1950)||mhz<requested_[n])return finish(Status::InvalidArgument,r);
    uint32_t freq=0,bits=0;auto s=send(9,(uint32_t(clock)<<16)|mhz,p,freq,r);
    if(s==Status::Ok&&(!freq||freq<uint32_t(mhz)*1000||(n<2&&freq>1950000)))s=Status::InvalidFrequency;
    while(s==Status::Ok){s=send(0x15,0,p,bits,r);r.hardMinBits=bits;if(s==Status::Ok)r.stage=Stage::HardMinimum;
        if(s!=Status::Ok||(bits&mask(n))==mask(n))break;
        if(r.responseReads>=p.maxReads){s=Status::Timeout;break;}
        ++r.delays;io_.delayUs(io_.context,p.delayUs);
    }
    if(s!=Status::Ok){faulted_=true;ready_=false;return finish(s,r);}
    requested_[n]=mhz;output={freq,bits};r.stage=Stage::Complete;return finish(Status::Ok,r);
}
const ColdPresetJournal* Controller::coldPresetJournal(Clock clock) const {
    const int n=index(clock);return n>=0&&n<2?&coldPresets_[n]:nullptr;
}
namespace {
bool coldObservation(Clock clock,const ColdPresetTarget& target,const ColdPresetObservation& observed) {
    if(observed.rawPll==UINT32_MAX||observed.rawDentist==UINT32_MAX||
        observed.programmedVcoKHz!=target.programmedVcoKHz)return false;
    uint32_t divider=0;
    if(displaydentist::decodeDivider(observed.rawDfsDid,divider)!=displaydentist::Status::Ok)return false;
    const uint64_t derived=uint64_t(target.programmedVcoKHz)*4/divider;
    if(!derived||derived>1950000||observed.programmedKHz!=derived)return false;
    if(target.dividerId&&(observed.rawDfsDid!=target.dividerId||observed.programmedKHz!=target.programmedKHz))return false;
    if(clock==Clock::Disp)return (observed.rawDentist&0x80000)&&
        (observed.rawDentist&0x7f)==observed.rawDfsDid&&((observed.rawDentist>>8)&0x7f)==observed.rawDfsDid;
    return (observed.rawDentist&0x100000)&&((observed.rawDentist>>24)&0x7f)==observed.rawDfsDid;
}
}
Status Controller::requestColdStep(Clock clock,ColdPresetStep& step,const Poll& poll,Result& r) {
    uint32_t reply=0,bits=0;
    auto status=send(9,(uint32_t(clock)<<16)|step.requestedMHz,poll,reply,r,&step);
    if(status==Status::Ok&&(!reply||reply<uint32_t(step.requestedMHz)*1000||reply>1950000))status=Status::InvalidFrequency;
    const auto required=mask(index(clock));
    while(status==Status::Ok) {
        status=send(0x15,0,poll,bits,r);r.hardMinBits=bits;
        if(status!=Status::Ok)break;
        step.fulfilledBits=bits;r.stage=Stage::HardMinimum;
        if((bits&required)==required){step.fulfilled=true;requested_[index(clock)]=step.requestedMHz;break;}
        if(r.responseReads>=poll.maxReads){status=Status::Timeout;break;}
        ++r.delays;io_.delayUs(io_.context,poll.delayUs);
    }
    return status;
}
Status Controller::requestColdPreset(Clock clock,uint32_t vco,uint32_t required,
    const ColdPresetIo& observation,const Poll& poll,ColdPresetFrequency& output,ColdPresetResult& r) {
    output={};r={};const int n=index(clock);
    const auto finishCold=[&](Status status){r.status=status;r.mailbox.status=status;return status;};
    if(!ready_||faulted_)return finishCold(Status::InvalidState);
    if(n<0||n>1||!observation.observe||!valid(poll))return finishCold(Status::InvalidArgument);
    if(coldPresets_[n].started||requested_[n])return finishCold(Status::InvalidState);
    displaydentist::Preset preset={};
    if(displaydentist::plan(vco,required,preset)!=displaydentist::Status::Ok)return finishCold(Status::InvalidFrequency);
    auto& journal=coldPresets_[n];journal={};journal.started=true;
    journal.programmedVcoKHz=vco;journal.requiredKHz=required;journal.finalPreset=preset;
    const auto fail=[&](Status status){faulted_=true;ready_=false;return finishCold(status);};
    const auto observe=[&](ColdPresetStage stage,const ColdPresetTarget& target,ColdPresetObservation& value) {
        r.stage=journal.stage=stage;
        if(!observation.observe(observation.context,clock,stage,target,value))return Status::ObservationFailed;
        if(!coldObservation(clock,target,value))return Status::InvalidClockObservation;
        if(stage!=ColdPresetStage::InitialObservation&&value.rawPll!=journal.initial.rawPll)return Status::InvalidClockObservation;
        return Status::Ok;
    };
    auto status=observe(ColdPresetStage::InitialObservation,{vco,0,0},journal.initial);
    if(status!=Status::Ok)return fail(status);
    journal.initialObservationValid=true;
    journal.usesIntermediate=clock==Clock::Disp&&preset.dividerId==127&&journal.initial.rawDfsDid!=127;
    if(journal.usesIntermediate) {
        uint32_t divider=0;
        if(displaydentist::decodeDivider(126,divider)!=displaydentist::Status::Ok)return fail(Status::InvalidFrequency);
        const uint64_t temporary=uint64_t(vco)*4/divider;
        if(!temporary||temporary>1950000||temporary/1000==0||temporary/1000<=preset.requestMHz)
            return fail(Status::InvalidFrequency);
        journal.intermediate.requestedMHz=uint16_t(temporary/1000);
        r.stage=journal.stage=ColdPresetStage::IntermediateRequest;
        status=requestColdStep(clock,journal.intermediate,poll,r.mailbox);
        if(journal.intermediate.fulfilled)journal.lastFulfilledMHz=journal.intermediate.requestedMHz;
        if(status!=Status::Ok)return fail(status);
        status=observe(ColdPresetStage::IntermediateObservation,{vco,uint32_t(temporary),126},journal.intermediate.observation);
        if(status!=Status::Ok)return fail(status);
        if(journal.intermediate.rawReply!=journal.intermediate.observation.programmedKHz)return fail(Status::InvalidFrequency);
        journal.intermediate.observationValid=true;
    }
    journal.final.requestedMHz=preset.requestMHz;r.stage=journal.stage=ColdPresetStage::FinalRequest;
    status=requestColdStep(clock,journal.final,poll,r.mailbox);
    if(journal.final.fulfilled)journal.lastFulfilledMHz=journal.final.requestedMHz;
    if(status!=Status::Ok)return fail(status);
    status=observe(ColdPresetStage::FinalObservation,{vco,preset.programmedKHz,preset.dividerId},journal.final.observation);
    if(status!=Status::Ok)return fail(status);
    if(journal.final.rawReply!=journal.final.observation.programmedKHz)return fail(Status::InvalidFrequency);
    journal.final.observationValid=true;journal.complete=true;r.stage=journal.stage=ColdPresetStage::Complete;
    r.mailbox.stage=Stage::Complete;output={preset,{journal.final.rawReply,journal.final.fulfilledBits},journal.final.observation};
    return finishCold(Status::Ok);
}
Status Controller::requestMemoryMinimum(uint16_t mhz,const Poll& p,MemoryMinimum& output,Result& r){
    output={};r={};
    if(!ready_||faulted_)return finish(Status::InvalidState,r);
    if(!mhz||!valid(p)||mhz<memory_.minimumMHz||
        (memoryMaximum_.requestAttempted&&mhz>memoryMaximum_.requestedMHz))return finish(Status::InvalidArgument,r);
    uint32_t raw=0,bits=0;
    auto s=send(9,(2u<<16)|mhz,p,raw,r);
    // dcn32_clk_mgr_smu_msg.c:193-301, pinned Linux3857c2fe. Its UCLK
    // callers ignore the returned argument; do not invent a frequency unit.
    // Zero is a valid raw reply here, unlike the interpreted display path.
    while(s==Status::Ok){
        s=send(0x15,0,p,bits,r);r.hardMinBits=bits;
        if(s==Status::Ok){r.stage=Stage::HardMinimum;memory_.fulfilledBits=bits;}
        if(s!=Status::Ok||(bits&0x24u)==0x24u)break;
        if(r.responseReads>=p.maxReads){s=Status::Timeout;break;}
        ++r.delays;io_.delayUs(io_.context,p.delayUs);
    }
    if(s!=Status::Ok){faulted_=true;ready_=false;return finish(s,r);}
    memory_.minimumMHz=mhz;memory_.fulfilled=true;
    output={mhz,raw,bits};r.stage=Stage::Complete;return finish(Status::Ok,r);
}
Status Controller::requestMemoryMaximum(uint16_t mhz,const Poll& p,Result& r){
    r={};
    if(!ready_||faulted_||memoryMaximum_.requestAttempted)return finish(Status::InvalidState,r);
    if(!mhz||!valid(p)||mhz<memory_.minimumMHz)return finish(Status::InvalidArgument,r);
    uint32_t raw=0;
    // Pinned dcn30_smu_set_hard_max_by_freq, called for PPCLK_UCLK by
    // dcn32_set_hard_max_memclk. ACK only; no maximum fulfillment bit exists.
    const auto status=send(0xa,(2u<<16)|mhz,p,raw,r);
    if(status!=Status::Ok){faulted_=true;ready_=false;return finish(status,r);}
    r.stage=Stage::Complete;return finish(Status::Ok,r);
}
Status Controller::requestDcfDeepSleepFloor(uint32_t mhz,const Poll& p,Result& r){
    r={};
    if(!ready_||faulted_||dcfDeepSleep_.requestAttempted)return finish(Status::InvalidState,r);
    if(!mhz||mhz>UINT16_MAX||!valid(p))return finish(Status::InvalidArgument,r);
    uint32_t raw=0;
    // Pinned dcn30_smu_set_min_deep_sleep_dcef_clk takes MHz directly;
    // dcn32_update_clocks rounds calculated kHz upward before calling it.
    // Its caller ignores the output argument. Preserve raw data without units.
    const auto status=send(0xd,mhz,p,raw,r);
    if(status!=Status::Ok){faulted_=true;ready_=false;return finish(status,r);}
    r.stage=Stage::Complete;return finish(Status::Ok,r);
}
Status Controller::queryDpmInventory(const Poll& p,DpmInventory& output,Result& r){
    output={};r={};
    if(!ready_||faulted_)return finish(Status::InvalidState,r);
    if(!valid(p))return finish(Status::InvalidArgument,r);
    // Linux dcn32_clk_mgr.c:dcn32_init_single_clock +
    // dcn30_clk_mgr_smu_msg.c:dcn30_smu_get_dpm_freq_by_index. Metadata bits
    // 7:0 are discrete count; 28/29/30 modes; 31 means fine-grained min/max.
    const DpmClock clocks[]={DpmClock::Soc,DpmClock::Memory,DpmClock::Fabric,
        DpmClock::Disp,DpmClock::Dpp,DpmClock::Dcf,DpmClock::Dtb};
    DpmInventory observed={};observed.firmwareVersion=version_;
    Status status=Status::Ok;
    for(unsigned pass=0;pass<2&&status==Status::Ok;++pass) {
        for(size_t n=0;n<kDpmClockCount&&status==Status::Ok;++n) {
            uint32_t metadata=0;
            const uint32_t selector=uint32_t(clocks[n])<<16;
            status=send(0xb,selector|0xff,p,metadata,r);if(status!=Status::Ok)break;
            if(metadata&0x0fffff00){status=Status::InvalidDpmInventory;break;}
            const bool fine=(metadata&0x80000000)!=0;
            const uint32_t levels=fine?2:(metadata&0xff);
            if(!levels){status=Status::InvalidDpmInventory;break;}
            if(levels>kDpmLevelCapacity){status=Status::InventoryCapacityExceeded;break;}
            auto& table=observed.clocks[n];
            if(pass&&table.metadata!=metadata){status=Status::ChangedDpmInventory;break;}
            if(!pass){table.clock=clocks[n];table.metadata=metadata;table.levelCount=uint8_t(levels);table.fineGrained=fine;}
            for(uint32_t level=0;level<levels;++level) {
                uint32_t raw=0;status=send(0xb,selector|level,p,raw,r);if(status!=Status::Ok)break;
                const uint16_t mhz=uint16_t(raw);
                if(!mhz){status=Status::InvalidFrequency;break;}
                if(pass&&table.rawFrequency[level]!=raw){status=Status::ChangedDpmInventory;break;}
                if(!pass){table.rawFrequency[level]=raw;table.frequencyMHz[level]=mhz;}
            }
            if(status==Status::Ok&&fine&&table.frequencyMHz[0]>table.frequencyMHz[1])status=Status::InvalidDpmInventory;
            if(status==Status::Ok) {
                uint32_t dcMax=0;status=send(0xc,selector,p,dcMax,r);
                if(status==Status::Ok&&(!dcMax||dcMax>UINT16_MAX))status=Status::InvalidFrequency;
                if(status==Status::Ok&&pass&&dcMax!=table.dcMaxMHz)status=Status::ChangedDpmInventory;
                if(status==Status::Ok&&!pass)table.dcMaxMHz=dcMax;
            }
        }
    }
    if(status!=Status::Ok){faulted_=true;ready_=false;return finish(status,r);}
    output=observed;r.stage=Stage::Complete;return finish(Status::Ok,r);
}
Status Controller::disableDmcubMclkWait(const Poll& p,Result& r){
    r={};
    if(!ready_||faulted_)return finish(Status::InvalidState,r);
    if(!valid(p))return finish(Status::InvalidArgument,r);
    uint32_t raw=0;
    // dcn32_smu_wait_for_dmub_ack_mclk(false), pinned Linux3857c2fe.
    // Raw argument output has no defined status/value contract. Only the
    // actual response ACK is interpreted; zero is a legitimate raw reply.
    const auto status=send(0x14,0,p,raw,r);
    if(status!=Status::Ok){faulted_=true;ready_=false;return finish(status,r);}
    r.stage=Stage::Complete;return finish(Status::Ok,r);
}
Status Controller::releaseMinimaAfterQuiescence(const Poll& p,Result& r){
    r={};if(!ready_||faulted_)return finish(Status::InvalidState,r);
    if(!valid(p))return finish(Status::InvalidArgument,r);
    // No audited primary contract establishes a zero-MHz UCLK withdrawal.
    // Do not partially release other minima while reporting the owner retired.
    if(memory_.requestAttempted||policyMayBeActive())return finish(Status::ResetRequired,r);
    const Clock clocks[]={Clock::Disp,Clock::Dpp,Clock::Dcf,Clock::Dtb};
    for(size_t n=0;n<4;++n)if(requested_[n]){
        uint32_t unused=0;const auto status=send(9,uint32_t(clocks[n])<<16,p,unused,r);
        if(status!=Status::Ok){faulted_=true;ready_=false;return finish(status,r);}
        requested_[n]=0;
    }
    minimum_=false;r.stage=Stage::Complete;return finish(Status::Ok,r);
}
} }
