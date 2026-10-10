// SPDX-License-Identifier: MIT
#include "HackyNaviDisplayController.hpp"
#include "HackyNaviScanoutSurface.hpp"
#include <libkern/c++/OSData.h>

using namespace hackynavi;

namespace {
bool globalClockIndex(displaysmu::Clock clock,size_t& index) {
    if(clock==displaysmu::Clock::Disp){index=0;return true;}
    if(clock==displaysmu::Clock::Dpp){index=1;return true;}
    return false;
}
bool completedDivider(const displaydentist::Observation& observed,size_t index) {
    if(!(observed.validClockMask&(1u<<index)))return false;
    return index==0?(observed.dispChangeDone&&observed.dispDividerAgrees):
        (observed.dppChangeDone&&((observed.dentist>>24)&0x7fu)==observed.rawDfs[1]);
}
}

void HackyNaviDisplayController::invalidateGlobalClockReferences() {
    globalClockReferences_[0].valid=false;globalClockReferences_[1].valid=false;
    dppReferenceKHz_=dispReferenceKHz_=0;
}

IOReturn HackyNaviDisplayController::globalClockColdLocked(
    HackyNaviFunctionGroup::Operation& scope,displayidle::Result& out) {
    if(!dpmCaptureReady())return dpmReport_->result;
    if(state_!=State::Ready||!providerReady()||firmware_->gart_!=gart_||
        gart_->firmwareOwner_!=firmware_||gart_->displayOwner_!=this)return kIOReturnNotReady;
    if(scanoutOwner_||gart_->queueOwner_||gart_->hasVmOwners()||aux_.ownsPins()||ddc_.ownsController())
        return kIOReturnBusy;
    for(uint8_t p=0;p<paths_;++p)
        if(links_[p].phase()!=dio::Phase::Closed||streams_[p].requiresHardwareRetirement())return kIOReturnBusy;
    for(size_t p=0;p<displayclock::kPipeCount;++p)
        if(pipeClocks_[p].requiresHardwareRetirement())return kIOReturnBusy;
    MemoryMinimumResult coordination={};
    auto result=memoryClockReadyLocked(scope,coordination);
    if(result!=kIOReturnSuccess)return result;
    ClockResult physical={};result=checkClockIdleLocked(physical);out=physical.idle;
    if(!dpmCaptureReady())return dpmReport_->result;
    return result;
}

bool HackyNaviDisplayController::observeGlobalClock(void* context,displaysmu::Clock clock,
    displaysmu::ColdPresetStage phase,const displaysmu::ColdPresetTarget& target,
    displaysmu::ColdPresetObservation& out) {
    auto* self=static_cast<HackyNaviDisplayController*>(context);out={};size_t index=0;
    if(!globalClockIndex(clock,index)||!self->globalClockAttempt_||!self->globalClockReport_||
        !self->dpmScope_||self->globalClockAttempt_!=&self->globalClockReferences_[index])return false;
    const auto& reference=*self->globalClockAttempt_;
    auto& report=*self->globalClockReport_;
    if(reference.generation!=self->dpmScope_->generation()||reference.famsSerial!=self->fams_.serial||
        target.programmedVcoKHz!=reference.references.programmedVcoKHz||
        (target.programmedKHz&&target.programmedKHz>self->globalClockCeilingKHz_))return false;
    if(self->globalClockColdLocked(*self->dpmScope_,report.idle)!=kIOReturnSuccess)return false;
    displaydentist::Observation observed={};
    const displaydentist::Io io={self,referenceValid,read32,referenceTimeUs};
    if(displaydentist::capture(reference.layout,reference.board,reference.references,1u<<index,io,
        uint32_t(self->dpmBudget_-(self->dpmLast_-self->dpmStart_)),observed,report.capture)!=
        displaydentist::Status::Ok||!self->dpmCaptureReady()||!completedDivider(observed,index))return false;
    // Entering DISP DID127 can first raise to DID126. Bound that derived
    // programming before either message, not only the lower floored MHz.
    if(phase==displaysmu::ColdPresetStage::InitialObservation&&index==0&&
        reference.frequency.preset.dividerId==127&&observed.rawDfs[0]!=127&&
        uint64_t(observed.programmedVcoKHz)*4/248>self->globalClockCeilingKHz_)return false;
    if(self->globalClockColdLocked(*self->dpmScope_,report.idle)!=kIOReturnSuccess)return false;
    out={observed.pll,observed.programmedVcoKHz,observed.dentist,
        observed.rawDfs[index],observed.programmedKHz[index]};
    return true;
}

IOReturn HackyNaviDisplayController::requestGlobalClockPreset(
    HackyNaviFunctionGroup::Operation& scope,displaysmu::Clock clock,uint32_t requiredKHz,
    uint32_t timeoutUs,GlobalClockResult& out) {
    out={};out.result=kIOReturnBadArgument;size_t index=0;
    if(!globalClockIndex(clock,index)||!requiredKHz||!timeoutUs||timeoutUs>2000000)return out.result;
    if(!gart_||!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result=kIOReturnOffline;
    const uint64_t started=referenceTimeUs(nullptr);DpmCaptureResult guard={};
    lockOwners();dpmScope_=&scope;dpmRetirementScope_=nullptr;dpmReport_=&guard;
    dpmStart_=dpmLast_=started;dpmBudget_=timeoutUs;guard.result=kIOReturnSuccess;
    const auto done=[&](IOReturn status) {
        if(guard.result==kIOReturnSuccess)guard.result=status;
        out.result=guard.result;out.elapsedUs=guard.elapsedUs;
        const auto* journal=clockSmu_.coldPresetJournal(clock);if(journal)out.journal=*journal;
        if(out.result!=kIOReturnSuccess) {
            out.frequency={};
            if(globalClockAttempt_)globalClockAttempt_->valid=false;
        }
        globalClockAttempt_=nullptr;globalClockReport_=nullptr;globalClockCeilingKHz_=0;
        return finishDpmCapture(out.result);
    };
    if(!dpmCaptureReady())return done(guard.result);
    if(!dpmInventoryValid_||dpmGeneration_!=scope.generation()||!dpmSerial_||
        !clockSmu_.ready()||!fams_.complete)return done(kIOReturnNotReady);
    const auto* previous=clockSmu_.coldPresetJournal(clock);
    if(!previous||previous->started)return done(kIOReturnNotPermitted);
    if(globalClockSerial_==UINT64_MAX)return done(kIOReturnNoResources);
    gart_->mmioActive_=true;
    const auto admission=globalClockColdLocked(scope,out.idle);
    if(admission!=kIOReturnSuccess)return done(admission);
    const auto& table=dpmInventory_.clocks[index+3];
    if(table.clock!=(index?displaysmu::DpmClock::Dpp:displaysmu::DpmClock::Disp)||
        !table.levelCount||table.levelCount>displaysmu::kDpmLevelCapacity||!table.dcMaxMHz)
        return done(kIOReturnUnsupported);
    uint32_t maximumMHz=0;
    for(uint8_t i=0;i<table.levelCount;++i)if(table.frequencyMHz[i]>maximumMHz)maximumMHz=table.frequencyMHz[i];
    if(maximumMHz>table.dcMaxMHz)maximumMHz=table.dcMaxMHz;
    if(maximumMHz>1950)maximumMHz=1950;
    displaydentist::Preset preset={};
    if(displaydentist::plan(dpmReferences_.programmedVcoKHz,requiredKHz,preset)!=displaydentist::Status::Ok||
        preset.programmedKHz>maximumMHz*1000)return done(kIOReturnBadArgument);
    auto& reference=globalClockReferences_[index];reference={};
    reference.board=dpmBoard_;reference.references=dpmReferences_;
    if(displaydentist::buildLayout(static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy()),
        gart_->discovery_->getLength(),gart_->registerBytes_,reference.layout)!=displaydentist::Status::Ok)
        return done(kIOReturnUnsupported);
    reference.generation=scope.generation();reference.serial=++globalClockSerial_;
    reference.inventorySerial=dpmSerial_;reference.famsSerial=fams_.serial;reference.frequency.preset=preset;
    out.deviceGeneration=reference.generation;out.serial=reference.serial;out.inventorySerial=reference.inventorySerial;
    globalClockAttempt_=&reference;globalClockReport_=&out;globalClockCeilingKHz_=maximumMHz*1000;
    if(index)dppReferenceKHz_=0;else dispReferenceKHz_=0;
    // Retain the actual selected source privately before invalidating the
    // observation cache and before any possibly delivered clock mutation.
    invalidateDpmInventory();if(!hardwareHold_){retain();hardwareHold_=true;}
    const displaysmu::ColdPresetIo io={this,observeGlobalClock};const displaysmu::Poll poll={100000,10};
    const auto result=clockSmu_.requestColdPreset(clock,reference.references.programmedVcoKHz,
        requiredKHz,io,poll,out.frequency,out.transition);
    if(result!=displaysmu::Status::Ok||!dpmCaptureReady())return done(fault());
    if(globalClockColdLocked(scope,out.idle)!=kIOReturnSuccess)return done(fault());
    const auto* journal=clockSmu_.coldPresetJournal(clock);
    if(!journal||!journal->complete)return done(fault());
    reference.frequency=out.frequency;reference.valid=true;
    if(index)dppReferenceKHz_=reference.frequency.observation.programmedKHz;
    else dispReferenceKHz_=reference.frequency.observation.programmedKHz;
    return done(kIOReturnSuccess);
}

IOReturn HackyNaviDisplayController::verifyGlobalClockReferenceLocked(
    HackyNaviFunctionGroup::Operation& scope,displaysmu::Clock clock,uint64_t started,
    uint32_t timeoutUs,ProgrammedClocksResult& out) {
    out={};out.result=kIOReturnBadArgument;size_t index=0;
    if(!globalClockIndex(clock,index)||!timeoutUs||timeoutUs>2000000||dpmReport_||
        dpmScope_||dpmRetirementScope_)return out.result;
    DpmCaptureResult guard={};guard.result=kIOReturnSuccess;
    dpmScope_=&scope;dpmReport_=&guard;dpmStart_=dpmLast_=started;dpmBudget_=timeoutUs;
    const auto done=[&](IOReturn status) {
        if(guard.result==kIOReturnSuccess)guard.result=status;
        out.result=guard.result;out.elapsedUs=guard.elapsedUs;
        if(out.result!=kIOReturnSuccess){invalidateGlobalClockReferences();out.observation={};}
        dpmScope_=nullptr;dpmReport_=nullptr;dpmBudget_=0;
        return out.result;
    };
    if(!dpmCaptureReady())return done(guard.result);
    const auto& reference=globalClockReferences_[index];
    const auto* journal=clockSmu_.coldPresetJournal(clock);
    if(!reference.valid||!reference.serial||!reference.inventorySerial||reference.generation!=scope.generation()||
        reference.famsSerial!=fams_.serial||!journal||!journal->complete||!journal->final.observationValid||
        state_!=State::Ready||!providerReady()||!clockSmu_.ready())return done(kIOReturnNotReady);
    const auto& expected=reference.frequency.observation;
    if(expected.rawPll!=journal->final.observation.rawPll||expected.rawDfsDid!=journal->final.observation.rawDfsDid||
        expected.programmedKHz!=journal->final.observation.programmedKHz)return done(fault());
    MemoryMinimumResult coordination={};gart_->mmioActive_=true;
    if(memoryClockReadyLocked(scope,coordination)!=kIOReturnSuccess)return done(fault());
    ClockResult physical={};
    auto result=checkClockIdleLocked(physical);out.idle=physical.idle;
    if(result!=kIOReturnSuccess||!dpmCaptureReady())return done(result);
    const displaydentist::Io io={this,referenceValid,read32,referenceTimeUs};
    if(displaydentist::capture(reference.layout,reference.board,reference.references,1u<<index,io,
        uint32_t(dpmBudget_-(dpmLast_-dpmStart_)),out.observation,out.capture)!=displaydentist::Status::Ok||
        !dpmCaptureReady())return done(fault());
    const auto& observed=out.observation;
    if(!completedDivider(observed,index)||observed.pll!=expected.rawPll||
        observed.rawDfs[index]!=expected.rawDfsDid||observed.programmedKHz[index]!=expected.programmedKHz||
        observed.programmedVcoKHz!=expected.programmedVcoKHz)return done(fault());
    if(memoryClockReadyLocked(scope,coordination)!=kIOReturnSuccess||!dpmCaptureReady())return done(fault());
    out.deviceGeneration=reference.generation;out.inventorySerial=reference.inventorySerial;
    return done(kIOReturnSuccess);
}

IOReturn HackyNaviDisplayController::configureDppClock(HackyNaviFunctionGroup::Operation& scope,
    uint8_t pipe,uint32_t requiredKHz,uint8_t k1,uint8_t k2,uint32_t timeoutUs,ClockResult& out) {
    out={};
    if(pipe>=displayclock::kPipeCount||!requiredKHz||(k1!=1&&k1!=2)||(k2!=1&&k2!=2&&k2!=4)||
        !timeoutUs||timeoutUs>2000000)return kIOReturnBadArgument;
    if(!gart_||!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return kIOReturnOffline;
    const uint64_t started=referenceTimeUs(nullptr);lockOwners();ProgrammedClocksResult reference={};
    auto result=verifyGlobalClockReferenceLocked(scope,displaysmu::Clock::Dpp,started,timeoutUs,reference);
    out.idle=reference.idle;if(result!=kIOReturnSuccess)return finish(result);
    DpmCaptureResult guard={};guard.result=kIOReturnSuccess;
    dpmScope_=&scope;dpmReport_=&guard;dpmStart_=dpmLast_=started;dpmBudget_=timeoutUs;
    if(!dpmCaptureReady())return finishDpmCapture(guard.result);
    if(!dppReferenceKHz_||requiredKHz>dppReferenceKHz_)return finishDpmCapture(kIOReturnBadArgument);
    if(!pipePowers_[pipe].powered()||gart_->queueOwner_||gart_->hasVmOwners()||
        (scanoutOwner_&&scanoutOwner_->requiresHardwareRetirement()))return finishDpmCapture(kIOReturnBusy);
    if(aux_.ownsPins()||ddc_.ownsController())return finishDpmCapture(kIOReturnBusy);
    for(uint8_t p=0;p<paths_;++p)
        if(links_[p].phase()!=dio::Phase::Closed||streams_[p].requiresHardwareRetirement())
            return finishDpmCapture(kIOReturnBusy);
    if(out.idle.poweredOffMask&(1u<<pipe))return finishDpmCapture(fault());
    const displayclock::Config config={dppReferenceKHz_,requiredKHz,k1,k2};
    const displayclock::Io io={this,read32,write32};
    const auto status=pipeClocks_[pipe].configure(pipeClockLayout_,io,pipe,config,out.pipe);
    if(status==displayclock::Status::Ok&&dpmCaptureReady())return finishDpmCapture(kIOReturnSuccess);
    return finishDpmCapture(out.pipe.writeAttempted||status==displayclock::Status::TransportError||
        status==displayclock::Status::DeviceUnavailable?fault():kIOReturnIOError);
}

IOReturn HackyNaviDisplayController::captureProgrammedClocks(
    HackyNaviFunctionGroup::Operation& scope,uint32_t mask,uint32_t timeoutUs,ProgrammedClocksResult& out) {
    out={};out.result=kIOReturnBadArgument;
    if(!mask||(mask&~displaydentist::kAllClocks)||!timeoutUs||timeoutUs>2000000)return out.result;
    if(!gart_||!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result=kIOReturnOffline;
    const uint64_t started=referenceTimeUs(nullptr);DpmCaptureResult guard={};
    lockOwners();dpmScope_=&scope;dpmRetirementScope_=nullptr;dpmReport_=&guard;
    dpmStart_=dpmLast_=started;dpmBudget_=timeoutUs;guard.result=kIOReturnSuccess;
    const auto done=[&](IOReturn status) {
        if(guard.result==kIOReturnSuccess)guard.result=status;
        out.result=guard.result;out.elapsedUs=guard.elapsedUs;
        if(out.result!=kIOReturnSuccess)out.observation={};
        return finishDpmCapture(out.result);
    };
    if(!dpmCaptureReady())return done(guard.result);
    if(!dpmInventoryValid_||state_!=State::Ready||!providerReady()||
        dpmGeneration_!=scope.generation())return done(kIOReturnNotReady);
    if(scanoutOwner_)return done(kIOReturnBusy);
    for(uint8_t p=0;p<paths_;++p)
        if(links_[p].phase()!=dio::Phase::Closed||streams_[p].requiresHardwareRetirement())return done(kIOReturnBusy);
    for(size_t p=0;p<displayclock::kPipeCount;++p)
        if(pipeClocks_[p].requiresHardwareRetirement())return done(kIOReturnBusy);
    gart_->mmioActive_=true;ClockResult idle={};
    auto status=checkClockIdleLocked(idle);out.idle=idle.idle;
    if(status!=kIOReturnSuccess||!dpmCaptureReady())return done(status);
    displaydentist::Layout layout={};
    if(displaydentist::buildLayout(static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy()),
        gart_->discovery_->getLength(),gart_->registerBytes_,layout)!=displaydentist::Status::Ok)
        return done(kIOReturnUnsupported);
    const displaydentist::Io io={this,referenceValid,read32,referenceTimeUs};
    const auto captured=displaydentist::capture(layout,dpmBoard_,dpmReferences_,mask,io,
        uint32_t(dpmBudget_-(dpmLast_-dpmStart_)),out.observation,out.capture);
    if(captured!=displaydentist::Status::Ok||!dpmCaptureReady())return done(kIOReturnNotReady);
    status=checkClockIdleLocked(idle);out.idle=idle.idle;
    if(status!=kIOReturnSuccess||!dpmCaptureReady())return done(status);
    out.deviceGeneration=dpmGeneration_;out.inventorySerial=dpmSerial_;
    return done(kIOReturnSuccess);
}

IOReturn HackyNaviDisplayController::requestMemoryMaximum(HackyNaviFunctionGroup::Operation& scope,
    uint16_t mhz,uint32_t timeoutUs,AckClockPolicyResult& out) {
    return requestAckClockPolicy(scope,true,mhz,timeoutUs,out);
}
IOReturn HackyNaviDisplayController::requestDcfDeepSleepFloor(HackyNaviFunctionGroup::Operation& scope,
    uint16_t mhz,uint32_t timeoutUs,AckClockPolicyResult& out) {
    return requestAckClockPolicy(scope,false,mhz,timeoutUs,out);
}
IOReturn HackyNaviDisplayController::requestAckClockPolicy(HackyNaviFunctionGroup::Operation& scope,
    bool memoryMaximum,uint16_t mhz,uint32_t timeoutUs,AckClockPolicyResult& out) {
    out={};out.result=kIOReturnBadArgument;
    if(!mhz||!timeoutUs||timeoutUs>2000000)return out.result;
    if(!gart_||!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result=kIOReturnOffline;
    const uint64_t started=referenceTimeUs(nullptr);DpmCaptureResult guard={};MemoryMinimumResult coordination={};
    lockOwners();dpmScope_=&scope;dpmRetirementScope_=nullptr;dpmReport_=&guard;
    dpmStart_=dpmLast_=started;dpmBudget_=timeoutUs;guard.result=kIOReturnSuccess;
    const auto done=[&](IOReturn status) {
        if(guard.result==kIOReturnSuccess)guard.result=status;
        out.result=guard.result;out.elapsedUs=guard.elapsedUs;
        out.deviceGeneration=memoryGeneration_;out.serial=memorySerial_;
        out.journal=memoryMaximum?clockSmu_.memoryMaximumJournal():clockSmu_.dcfDeepSleepJournal();
        return finishDpmCapture(out.result);
    };
    if(!dpmCaptureReady())return done(guard.result);
    if((state_!=State::Prepared&&state_!=State::Ready)||!providerReady())return done(kIOReturnNotReady);
    if(memoryGeneration_&&memoryGeneration_!=scope.generation())return done(kIOReturnOffline);
    if(memorySerial_==UINT64_MAX)return done(kIOReturnNoResources);
    const auto& previous=memoryMaximum?clockSmu_.memoryMaximumJournal():clockSmu_.dcfDeepSleepJournal();
    if(previous.requestAttempted)return done(kIOReturnNotPermitted);
    if(memoryMaximum&&mhz<clockSmu_.memoryJournal().minimumMHz)return done(kIOReturnBadArgument);
    if(scanoutOwner_||gart_->queueOwner_||gart_->hasVmOwners()||aux_.ownsPins()||ddc_.ownsController())
        return done(kIOReturnBusy);
    for(uint8_t p=0;p<paths_;++p)
        if(links_[p].phase()!=dio::Phase::Closed||streams_[p].requiresHardwareRetirement())return done(kIOReturnBusy);
    for(size_t p=0;p<displayclock::kPipeCount;++p)
        if(pipeClocks_[p].requiresHardwareRetirement())return done(kIOReturnBusy);
    gart_->mmioActive_=true;
    auto status=memoryClockReadyLocked(scope,coordination);if(status!=kIOReturnSuccess)return done(status);
    ClockResult idle={};status=checkClockIdleLocked(idle);out.idle=idle.idle;
    if(status!=kIOReturnSuccess||!dpmCaptureReady())return done(status);
    // Pin before any mailbox write. A failed callback may have delivered it.
    if(!hardwareHold_){retain();hardwareHold_=true;}
    invalidateDpmInventory();const displaysmu::Poll poll={100000,10};
    const displaysmu::Io io={this,read32,write32,delayUs};
    if(!clockSmu_.ready()&&clockSmu_.initialize(clockSmuLayout_,io,poll,out.initialize)!=displaysmu::Status::Ok)
        return done(fault());
    if(clockSmu_.queryDpmInventory(poll,dpmInventory_,out.inventory)!=displaysmu::Status::Ok||!dpmCaptureReady())
        return done(fault());
    const auto& table=dpmInventory_.clocks[memoryMaximum?1:5];
    bool supported=table.clock==(memoryMaximum?displaysmu::DpmClock::Memory:displaysmu::DpmClock::Dcf)&&
        mhz<=table.dcMaxMHz;
    // A hard UCLK maximum selects an actual advertised value. The independent
    // DCF deep-sleep floor is a MHz requirement, not a normal DPM level index.
    if(supported&&memoryMaximum) {
        if(table.fineGrained)supported=mhz>=table.frequencyMHz[0]&&mhz<=table.frequencyMHz[1];
        else {supported=false;for(uint8_t i=0;i<table.levelCount;++i)if(mhz==table.frequencyMHz[i])supported=true;}
    } else if(supported) {
        uint16_t highest=0;for(uint8_t i=0;i<table.levelCount;++i)if(table.frequencyMHz[i]>highest)highest=table.frequencyMHz[i];
        supported=mhz<=highest;
    }
    invalidateDpmInventory();if(!supported)return done(kIOReturnBadArgument);
    if(memoryClockReadyLocked(scope,coordination)!=kIOReturnSuccess||!dpmCaptureReady())return done(fault());
    memoryGeneration_=scope.generation();++memorySerial_;
    const auto requested=memoryMaximum?clockSmu_.requestMemoryMaximum(mhz,poll,out.request):
        clockSmu_.requestDcfDeepSleepFloor(mhz,poll,out.request);
    if(requested!=displaysmu::Status::Ok||!dpmCaptureReady())return done(fault());
    if(memoryClockReadyLocked(scope,coordination)!=kIOReturnSuccess||!dpmCaptureReady())return done(fault());
    status=checkClockIdleLocked(idle);out.idle=idle.idle;
    if(status!=kIOReturnSuccess||!dpmCaptureReady())return done(fault());
    return done(kIOReturnSuccess);
}
