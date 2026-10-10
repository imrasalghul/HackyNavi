// SPDX-License-Identifier: MIT
#include "HackyNaviDisplayController.hpp"
#include <IOKit/IOLib.h>

using namespace hackynavi;

// These entry points alone pass a Display to Firmware's private clock path.
// The caller's actual FunctionGroup/GART access precedes every descendant
// mutex. Firmware acquires and releases only its own and GART's mutexes;
// this wrapper retains Display's mutex until the lexical guard is cleared.
IOReturn HackyNaviDisplayController::requestSocFabricRange(
    HackyNaviFunctionGroup::Operation& scope,smuclock::Clock clock,uint16_t minimum,
    uint16_t maximum,uint32_t timeoutUs,HackyNaviFirmwareController::ClockResult& out) {
    out={};out.result=kIOReturnOffline;
    if(!firmware_||!gart_||!gart_->normalAccessReady()||
        !scope.matchesGraphics(gart_->device_,gart_->openOwner_))return out.result;
    const auto started=HackyNaviFirmwareController::clockTime(nullptr);
    IOLockLock(lock_);
    const auto result=firmware_->changeClockPolicy(scope,HackyNaviFirmwareController::ClockPolicy::Range,
        clock,minimum,maximum,timeoutUs,out,this,started);
    IOLockUnlock(lock_);return result;
}
IOReturn HackyNaviDisplayController::inhibitSocFabricDeepSleep(
    HackyNaviFunctionGroup::Operation& scope,uint32_t timeoutUs,HackyNaviFirmwareController::ClockResult& out) {
    out={};out.result=kIOReturnOffline;
    if(!firmware_||!gart_||!gart_->normalAccessReady()||
        !scope.matchesGraphics(gart_->device_,gart_->openOwner_))return out.result;
    const auto started=HackyNaviFirmwareController::clockTime(nullptr);
    IOLockLock(lock_);
    const auto result=firmware_->changeClockPolicy(scope,HackyNaviFirmwareController::ClockPolicy::InhibitDeepSleep,
        smuclock::Clock::Soc,0,0,timeoutUs,out,this,started);
    IOLockUnlock(lock_);return result;
}
IOReturn HackyNaviDisplayController::disallowDfCstate(
    HackyNaviFunctionGroup::Operation& scope,uint32_t timeoutUs,HackyNaviFirmwareController::ClockResult& out) {
    out={};out.result=kIOReturnOffline;
    if(!firmware_||!gart_||!gart_->normalAccessReady()||
        !scope.matchesGraphics(gart_->device_,gart_->openOwner_))return out.result;
    const auto started=HackyNaviFirmwareController::clockTime(nullptr);
    IOLockLock(lock_);
    const auto result=firmware_->changeClockPolicy(scope,HackyNaviFirmwareController::ClockPolicy::DisallowDfCstate,
        smuclock::Clock::Soc,0,0,timeoutUs,out,this,started);
    IOLockUnlock(lock_);return result;
}
IOReturn HackyNaviDisplayController::retireFirmwareClockState(
    HackyNaviFunctionGroup::RetirementScope& scope,uint32_t timeoutUs,
    HackyNaviFirmwareController::ClockResult& out) {
    out={};out.result=kIOReturnOffline;
    if(!firmware_||!gart_||!gart_->accessReady()||
        !scope.matchesGraphics(gart_->device_,gart_->openOwner_))return out.result;
    const auto started=HackyNaviFirmwareController::clockTime(nullptr);
    IOLockLock(lock_);
    const auto result=firmware_->retireClockStateInternal(scope,timeoutUs,out,this,started);
    IOLockUnlock(lock_);return result;
}

IOReturn HackyNaviDisplayController::firmwareClockQuiescentLocked(
    HackyNaviFirmwareController::ClockResult& out) {
    // This method is called only with Display/Firmware/GART held, after the
    // clock owner has installed the exact Operation or RetirementScope into
    // both callback guards. No copied journal/result grants access.
    const auto* operation=firmware_->clockScope_;
    const auto* retirement=firmware_->clockRetirementScope_;
    const bool normal=operation&&!retirement&&dpmScope_==operation&&!dpmRetirementScope_&&
        fams_.deviceGeneration==operation->generation();
    // Revocation advances the FunctionGroup generation. Require the retained
    // old Display/Firmware journals to agree, and a genuine newer cleanup
    // scope for the exact current PCI owner; never relabel the old journals.
    const bool retiring=retirement&&!operation&&dpmRetirementScope_==retirement&&!dpmScope_&&
        retirement->generation()>fams_.deviceGeneration;
    if((!normal&&!retiring)||!dpmReport_||dpmStart_!=firmware_->clockStart_||
        dpmBudget_!=firmware_->clockBudget_||!dpmCaptureReady())return kIOReturnOffline;
    if(state_!=State::Ready||!hardwareHold_||!providerReady(retiring)||
        firmware_->gart_!=gart_||gart_->firmwareOwner_!=firmware_||gart_->displayOwner_!=this||
        !firmware_->displayFirmware_.ready()||!clockSmu_.ready()||!fams_.deviceGeneration||
        fams_.deviceGeneration!=firmware_->clockGeneration_||!fams_.serial||
        !fams_.started||!fams_.complete||!fams_.dalAcknowledged||!fams_.commandWritten||
        !fams_.commandSubmitted||!fams_.commandConsumed)return kIOReturnNotReady;
    const auto& dal=clockSmu_.dmcubWaitJournal();
    if(!dal.requestAttempted||!dal.commandAcknowledged||!dal.replyRead||
        !inbox_.isOpen()||inbox_.pending())return kIOReturnNotReady;
    if(gart_->queueOwner_||gart_->hasVmOwners()||scanoutOwner_||aux_.ownsPins()||ddc_.ownsController())
        return kIOReturnBusy;
    for(uint8_t path=0;path<paths_;++path)
        if(links_[path].phase()!=dio::Phase::Closed||streams_[path].requiresHardwareRetirement())
            return kIOReturnBusy;
    for(size_t pipe=0;pipe<displayclock::kPipeCount;++pipe)
        if(pipeClocks_[pipe].requiresHardwareRetirement())return kIOReturnBusy;
    if(inbox_.checkIdle(out.displayCoordination)!=dmcub::Status::Ok||!dpmCaptureReady())return fault();
    ClockResult physical{};
    const auto result=checkClockIdleLocked(physical);out.idle=physical.idle;
    if(result!=kIOReturnSuccess)return result;
    // Bracket the physical inventory with the real mapping and producer/RPTR
    // check. An empty ring alone never substitutes for physical pipe idle.
    if(inbox_.checkIdle(out.displayCoordination)!=dmcub::Status::Ok||!dpmCaptureReady())return fault();
    out.famsSerial=fams_.serial;out.runningDisplayCoordinated=true;
    return kIOReturnSuccess;
}
