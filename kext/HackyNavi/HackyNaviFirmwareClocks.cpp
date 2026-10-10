// SPDX-License-Identifier: MIT
#include "HackyNaviFirmwareController.hpp"
#include "HackyNaviDisplayController.hpp"
#include "hackynavi/dmcub.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>

using namespace hackynavi;
namespace {
constexpr uint32_t kMetricsPage=4096;
IOReturn clockError(smuclock::Status status) {
    if(status==smuclock::Status::Ok)return kIOReturnSuccess;
    if(status==smuclock::Status::Timeout)return kIOReturnTimeout;
    if(status==smuclock::Status::UnsupportedInterface||status==smuclock::Status::UnsupportedLayout||
        status==smuclock::Status::DisabledFeature)return kIOReturnUnsupported;
    return kIOReturnIOError;
}
}
uint64_t HackyNaviFirmwareController::clockTime(void*) {
    uint64_t ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);return ns/1000;
}
bool HackyNaviFirmwareController::clockValid(void* context) {
    auto* self=static_cast<HackyNaviFirmwareController*>(context);
    if(self->clockFailure_!=kIOReturnSuccess)return false;
    const bool normal=self->clockScope_&&self->gart_->normalAccessReady()&&
        self->clockScope_->matchesGraphics(self->gart_->device_,self->gart_->openOwner_)&&
        self->clockScope_->generation()==self->clockGeneration_;
    // Retirement scopes intentionally have the newly revoked generation.
    // They close this retained old owner, never reopen it in that generation.
    const bool retiring=self->clockRetirementScope_&&self->gart_->accessReady()&&
        self->clockRetirementScope_->matchesGraphics(self->gart_->device_,self->gart_->openOwner_);
    const bool recovery=self->clockRecovery_&&retiring&&self->clockRetirementScope_->recoveryWindowOpen();
    if((!normal&&!retiring)||(self->clockRecovery_&&!recovery)||
        (self->state_==State::FaultedRetained&&!recovery)||!self->providerReady()) {
        self->clockFailure_=kIOReturnOffline;return false;
    }
    const auto now=clockTime(self);
    if(now<self->clockLast_){self->clockFailure_=kIOReturnError;return false;}
    self->clockLast_=now;
    if(now-self->clockStart_>=self->clockBudget_){self->clockFailure_=kIOReturnTimeout;return false;}
    return true;
}
bool HackyNaviFirmwareController::clockRead(void* context,uint32_t reg,uint32_t& value) {
    return clockValid(context)&&read32(context,reg,value)&&clockValid(context);
}
bool HackyNaviFirmwareController::clockWrite(void* context,uint32_t reg,uint32_t value) {
    return clockValid(context)&&write32(context,reg,value)&&clockValid(context);
}
void HackyNaviFirmwareController::clockDelay(void* context,uint32_t us) {
    auto* self=static_cast<HackyNaviFirmwareController*>(context);
    if(!clockValid(context))return;
    if(us>=self->clockBudget_-(self->clockLast_-self->clockStart_)) {
        self->clockFailure_=kIOReturnTimeout;return;
    }
    if(us>=1000)IOSleep((us+999)/1000);else IODelay(us);
    (void)clockValid(context);
}
bool HackyNaviFirmwareController::clockVisibility(void* context) {
    return clockValid(context)&&beforeReadback(context)&&clockValid(context);
}
void HackyNaviFirmwareController::clockFault(void* context) {
    auto* self=static_cast<HackyNaviFirmwareController*>(context);
    // Companion callback executes under Firmware -> GART. Never recurse into
    // fault(), which unlocks those locks. Sealing only the companion would let
    // other firmware users keep treating the old smu::State as healthy.
    self->state_=State::FaultedRetained;self->gart_->firmwareReady_=false;
}
IOReturn HackyNaviFirmwareController::beginClock(HackyNaviFunctionGroup::Operation& scope,
    uint32_t timeoutUs,ClockResult& out,uint64_t started) {
    out={};out.result=kIOReturnBadArgument;
    if(!timeoutUs||timeoutUs>2000000)return out.result;
    if(!gart_||!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result=kIOReturnOffline;
    if(!started)started=clockTime(this);
    const auto result=begin(State::FirmwareLoaded);if(result!=kIOReturnSuccess)return out.result=result;
    if(clockGeneration_&&scope.generation()!=clockGeneration_)return out.result=finish(kIOReturnOffline);
    clockScope_=&scope;clockRetirementScope_=nullptr;clockGeneration_=scope.generation();
    clockStart_=clockLast_=started;clockBudget_=timeoutUs;clockFailure_=kIOReturnSuccess;
    if(!clockValid(this))return finishClock(out,clockFailure_);
    return kIOReturnSuccess; // Same locks and actual scope remain live.
}
IOReturn HackyNaviFirmwareController::finishClock(ClockResult& out,IOReturn result,
    HackyNaviDisplayController* display) {
    // The paired Display callbacks use the same deadline and actual scope.
    // Preserve their first failure before clearing the lexical binding.
    if(display&&!display->dpmCaptureReady()&&clockFailure_==kIOReturnSuccess)
        clockFailure_=display->dpmReport_->result;
    if(display&&(clockFailure_!=kIOReturnSuccess||
        display->state_==HackyNaviDisplayController::State::FaultedRetained))clockFault(this);
    // A deadline/scope loss at this last boundary must not return a faulted
    // result while leaving the shared firmware authority usable. Preserve
    // completed command history, but seal the actual native owner.
    if(result==kIOReturnSuccess&&!clockValid(this))clockFault(this);
    out.result=clockFailure_==kIOReturnSuccess?result:clockFailure_;
    out.generation=clockGeneration_;out.elapsedUs=clockLast_-clockStart_;out.journal=clocks_.journal();
    if(out.result!=kIOReturnSuccess)
        IOLog("HackyNavi: firmware clocks result=0x%x bind=%u operation=%u table-close=%u automatic=%u "
            "awake=%u idle=%u elapsed-us=%llu retained-table=%u retained-range=%u retained-ds=%u retained-df=%u\n",
            out.result,unsigned(out.bind.status),unsigned(out.operation.status),unsigned(out.tableClose.status),
            unsigned(out.automatic.status),unsigned(out.awake.status),unsigned(out.idle.status),
            static_cast<unsigned long long>(out.elapsedUs),unsigned(clocks_.tableRetentionRequired()),
            unsigned(clocks_.rangeRetentionRequired()),unsigned(clocks_.deepSleepRetentionRequired()),
            unsigned(clocks_.dfCstateRetentionRequired()));
    if(display) {
        if(state_==State::FaultedRetained)(void)display->fault();
        if(out.result!=kIOReturnSuccess)display->invalidateDpmInventory();
        display->dpmScope_=nullptr;display->dpmRetirementScope_=nullptr;
        display->dpmReport_=nullptr;display->dpmBudget_=0;
    }
    clockScope_=nullptr;clockRetirementScope_=nullptr;clockBudget_=0;clockRecovery_=false;
    return finish(out.result);
}
IOReturn HackyNaviFirmwareController::bindClock(ClockResult& out) {
    if(clocks_.ready())return kIOReturnSuccess;
    holdHardware();
    const smuclock::Io io={this,clockValid,clockRead,clockWrite,clockDelay,clockTime,clockVisibility,clockFault};
    const smuclock::Poll poll={clockBudget_,100000,10};
    return clockError(clocks_.bind(gart_->ipLayout_,gart_->registerBytes_,smu_,io,poll,out.bind));
}
IOReturn HackyNaviFirmwareController::clockQuiescent(ClockResult& out,
    HackyNaviDisplayController* display) {
    const auto phase=displayFirmware_.phase();
    const bool firmwareStopped=phase==dmcubfw::Phase::Loaded||
        (clockRetirementScope_&&phase==dmcubfw::Phase::Stopped);
    if(gart_->queueOwner_||gart_->hasVmOwners()||!gart_->discovery_)return kIOReturnBusy;
    if(display) {
        if(display->firmware_!=this||display->gart_!=gart_||gart_->displayOwner_!=display)
            return kIOReturnBusy;
    } else if(gart_->displayOwner_||!firmwareStopped||displayFirmware_.needsRuntimeRetention())
        return kIOReturnBusy;
    const smu::Io io={this,clockRead,clockWrite,clockDelay};
    if(smu::verifyAwake(io,{100000,10},smu_,out.awake)!=smu::Status::Ok) {
        clockFault(this);return kIOReturnNotReady;
    }
    if(display)return display->firmwareClockQuiescentLocked(out);
    const auto* discovery=static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy());
    const auto bytes=gart_->discovery_->getLength();
    dmcub::Layout firmware{};displayidle::Layout idle{};
    if(dmcub::buildLayout(discovery,bytes,gart_->registerBytes_,firmware)!=dmcub::Status::Ok||
        displayidle::buildLayout(discovery,bytes,gart_->registerBytes_,idle)!=displayidle::Status::Ok)
        return kIOReturnUnsupported;
    uint32_t before=0,control=0,after=0;
    const auto reset=firmware.registers[size_t(dmcub::Register::Control2)];
    const auto enable=firmware.registers[size_t(dmcub::Register::Control)];
    if(!clockRead(this,reset,before)||before==UINT32_MAX||!(before&1u)||
        !clockRead(this,enable,control)||control==UINT32_MAX||(control&0x10000u)||
        !clockRead(this,reset,after)||after!=before)return kIOReturnNotReady;
    return displayidle::verify(idle,{this,clockRead},out.idle)==displayidle::Status::Ok?
        kIOReturnSuccess:kIOReturnNotReady;
}
IOReturn HackyNaviFirmwareController::requestSocFabricRange(HackyNaviFunctionGroup::Operation& scope,
    smuclock::Clock clock,uint16_t minimum,uint16_t maximum,uint32_t timeoutUs,ClockResult& out) {
    return changeClockPolicy(scope,ClockPolicy::Range,clock,minimum,maximum,timeoutUs,out);
}
IOReturn HackyNaviFirmwareController::changeClockPolicy(HackyNaviFunctionGroup::Operation& scope,
    ClockPolicy policy,smuclock::Clock clock,uint16_t minimum,uint16_t maximum,uint32_t timeoutUs,
    ClockResult& out,HackyNaviDisplayController* display,uint64_t started) {
    out={};out.result=kIOReturnBadArgument;
    if(policy==ClockPolicy::Range&&((clock!=smuclock::Clock::Soc&&clock!=smuclock::Clock::Fabric)||
        !minimum||maximum<minimum||maximum>=UINT16_MAX-1))return out.result;
    if(display&&(display->firmware_!=this||display->gart_!=gart_||display->dpmScope_||
        display->dpmRetirementScope_||display->dpmReport_))return out.result=kIOReturnNotReady;
    auto result=beginClock(scope,timeoutUs,out,started);if(result!=kIOReturnSuccess)return result;
    HackyNaviDisplayController::DpmCaptureResult guard{};
    if(display) {
        guard.result=kIOReturnSuccess;display->dpmScope_=&scope;display->dpmReport_=&guard;
        display->dpmStart_=display->dpmLast_=clockStart_;display->dpmBudget_=clockBudget_;
    }
    result=clockQuiescent(out,display);
    if(result==kIOReturnSuccess&&display)display->invalidateDpmInventory();
    if(result==kIOReturnSuccess)result=bindClock(out);
    if(result==kIOReturnSuccess) {
        // bind can issue real interface/feature queries. Recheck the owned
        // running inbox and cold pipes immediately before a policy request.
        if(display)result=clockQuiescent(out,display);
        if(result==kIOReturnSuccess) {
            const smuclock::Poll poll={clockBudget_,100000,10};
            if(policy==ClockPolicy::Range)
                result=clockError(clocks_.requestSoftRange(clock,minimum,maximum,poll,out.operation));
            else if(policy==ClockPolicy::InhibitDeepSleep)
                result=clockError(clocks_.inhibitSocFabricDeepSleep(poll,out.operation));
            else result=clockError(clocks_.disallowDfCstate(poll,out.operation));
        }
    }
    if(result==kIOReturnSuccess) {
        result=clockQuiescent(out,display);
        if(result!=kIOReturnSuccess)clockFault(this);
    }
    return finishClock(out,result,display);
}
IOReturn HackyNaviFirmwareController::inhibitSocFabricDeepSleep(HackyNaviFunctionGroup::Operation& scope,
    uint32_t timeoutUs,ClockResult& out) {
    return changeClockPolicy(scope,ClockPolicy::InhibitDeepSleep,smuclock::Clock::Soc,0,0,timeoutUs,out);
}
IOReturn HackyNaviFirmwareController::disallowDfCstate(HackyNaviFunctionGroup::Operation& scope,
    uint32_t timeoutUs,ClockResult& out) {
    return changeClockPolicy(scope,ClockPolicy::DisallowDfCstate,smuclock::Clock::Soc,0,0,timeoutUs,out);
}
IOReturn HackyNaviFirmwareController::prepareMetricsPage(ClockResult& out) {
    auto& slot=slots_[MetricsTable];
    if(clocks_.journal().addressLowAcknowledged)return kIOReturnSuccess;
    if(clocks_.journal().addressHighAttempted)return kIOReturnNotReady;
    if(!slot.handle.owner) {
        if(!clockValid(this))return clockFailure_;
        if(gart_->allocator_.allocate(kMetricsPage,kMetricsPage,slot)!=VramAllocatorStatus::Ok)return kIOReturnNoSpace;
    }
    static_assert(psp::kStagingBytes>=kMetricsPage,"firmware workspace page");
    bzero(workspace_,kMetricsPage);
    const VramTransferIo io={this,gart_->registerBytes_,clockRead,clockWrite};VramTransferResult transfer{};
    HdpResult hdp{};
    if(uploadVram(gart_->allocator_,slot.handle,0,workspace_,kMetricsPage,io,transfer)!=VramTransferStatus::Ok||
        !clockValid(this)||hackynavi::flushHdp({this,clockRead,clockWrite},gart_->hdpState_,hdp)!=HdpStatus::Ok||
        !clockValid(this)) {
        clockFault(this);return kIOReturnIOError;
    }
    holdHardware();
    return clockError(clocks_.publishMetrics(gart_->allocator_,slot.handle,{clockBudget_,100000,10},out.operation));
}
IOReturn HackyNaviFirmwareController::captureSmuMetrics(HackyNaviFunctionGroup::Operation& scope,
    uint32_t timeoutUs,MetricsObservation& out) {
    out={};auto result=beginClock(scope,timeoutUs,out.operation);if(result!=kIOReturnSuccess)return result;
    result=bindClock(out.operation);
    if(result==kIOReturnSuccess)result=prepareMetricsPage(out.operation);
    if(result==kIOReturnSuccess)result=clockError(clocks_.captureMetrics(
        {clockBudget_,100000,10},out.metrics,out.operation.operation));
    result=finishClock(out.operation,result);out.valid=result==kIOReturnSuccess;
    if(!out.valid)out.metrics={};return result;
}
IOReturn HackyNaviFirmwareController::retireClockState(HackyNaviFunctionGroup::RetirementScope& scope,
    uint32_t timeoutUs,ClockResult& out) {
    return retireClockStateInternal(scope,timeoutUs,out);
}
IOReturn HackyNaviFirmwareController::retireClockStateInternal(
    HackyNaviFunctionGroup::RetirementScope& scope,uint32_t timeoutUs,ClockResult& out,
    HackyNaviDisplayController* display,uint64_t started) {
    out={};out.result=kIOReturnBadArgument;
    if(!timeoutUs||timeoutUs>2000000)return out.result;
    if(!gart_||!gart_->accessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result=kIOReturnOffline;
    if(display&&(display->firmware_!=this||display->gart_!=gart_||display->dpmScope_||
        display->dpmRetirementScope_||display->dpmReport_))return out.result=kIOReturnNotReady;
    if(!started)started=clockTime(this);
    IOLockLock(lock_);IOLockLock(gart_->lock_);
    clockScope_=nullptr;clockRetirementScope_=&scope;clockStart_=clockLast_=started;
    clockBudget_=timeoutUs;clockFailure_=kIOReturnSuccess;clockRecovery_=false;
    HackyNaviDisplayController::DpmCaptureResult guard{};
    if(display) {
        guard.result=kIOReturnSuccess;display->dpmRetirementScope_=&scope;display->dpmReport_=&guard;
        display->dpmStart_=display->dpmLast_=clockStart_;display->dpmBudget_=clockBudget_;
    }
    if(gart_->queueOwner_||gart_->hasVmOwners()||
        (!display&&(gart_->displayOwner_||displayFirmware_.needsRuntimeRetention())))
        return finishClock(out,kIOReturnBusy,display);
    if(!clocks_.retentionRequired())return finishClock(out,kIOReturnSuccess,display);
    if(!clockValid(this))return finishClock(out,clockFailure_,display);
    if(display)display->invalidateDpmInventory();
    gart_->mmioActive_=true;
    if(clocks_.tableRetentionRequired()) {
        auto result=display?clockQuiescent(out,display):kIOReturnSuccess;
        if(result==kIOReturnSuccess)
            result=clockError(clocks_.sealMetricsAfterCompletedTransfer({timeoutUs,100000,10},out.tableClose));
        if(result==kIOReturnSuccess&&display) {
            result=clockQuiescent(out,display);
            if(result!=kIOReturnSuccess)clockFault(this);
        }
        if(result!=kIOReturnSuccess)return finishClock(out,result,display);
    }
    // Returning touched ranges to the sourced automatic policy is independent
    // of completing the scratch transfer. It requires fresh idle/reset checks
    // on both sides, with the same retirement scope, locks and total deadline.
    if(clocks_.rangeRetentionRequired()) {
        auto result=clockQuiescent(out,display);
        if(result==kIOReturnSuccess)result=clockError(clocks_.returnRangesToAutomaticAfterQuiescence(
            {timeoutUs,100000,10},out.automatic));
        if(result==kIOReturnSuccess) {
            result=clockQuiescent(out,display);
            if(result!=kIOReturnSuccess)clockFault(this);
        }
        if(result!=kIOReturnSuccess)return finishClock(out,result,display);
    }
    // An owned external-client DF inhibit ends in the explicit ALLOW policy.
    // No prior-allowance getter exists; do not call this a restoration.
    if(clocks_.dfCstateRetentionRequired()) {
        auto result=clockQuiescent(out,display);
        if(result==kIOReturnSuccess)result=clockError(clocks_.allowDfCstateAfterQuiescence(
            {timeoutUs,100000,10},out.dfCstateAllow));
        if(result==kIOReturnSuccess) {
            result=clockQuiescent(out,display);
            if(result!=kIOReturnSuccess)clockFault(this);
        }
        if(result!=kIOReturnSuccess)return finishClock(out,result,display);
    }
    if(clocks_.deepSleepRetentionRequired()) {
        auto result=clockQuiescent(out,display);
        if(result==kIOReturnSuccess)result=clockError(clocks_.restoreSocFabricDeepSleepAfterQuiescence(
            {timeoutUs,100000,10},out.deepSleepRestore));
        if(result==kIOReturnSuccess) {
            result=clockQuiescent(out,display);
            if(result!=kIOReturnSuccess)clockFault(this);
        }
        if(result!=kIOReturnSuccess)return finishClock(out,result,display);
    }
    state_=State::Quiesced;return finishClock(out,kIOReturnSuccess,display);
}
IOReturn HackyNaviFirmwareController::prepareClockTableForReset(
    HackyNaviFunctionGroup::RetirementScope& scope,uint32_t timeoutUs,ClockResult& out) {
    out={};out.result=kIOReturnBadArgument;
    if(!timeoutUs||timeoutUs>2000000)return out.result;
    if(!gart_||!gart_->accessReady()||!scope.recoveryWindowOpen()||
        !scope.matchesGraphics(gart_->device_,gart_->openOwner_))return out.result=kIOReturnOffline;
    const auto started=clockTime(this);IOLockLock(lock_);IOLockLock(gart_->lock_);
    // No SMU publication needs no extra hardware proof. In particular, early
    // PSP failures must remain recoverable through the original reset path.
    if(!clocks_.retentionRequired())return out.result=finish(kIOReturnSuccess);
    // A table transfer does not withdraw clock policies. This pre-reset path
    // neither lowers clocks under live consumers nor manufactures their stop.
    if(clocks_.rangeRetentionRequired()||clocks_.deepSleepRetentionRequired()||clocks_.dfCstateRetentionRequired())
        return out.result=finish(kIOReturnBusy);
    clockScope_=nullptr;clockRetirementScope_=&scope;clockRecovery_=true;
    clockStart_=clockLast_=started;clockBudget_=timeoutUs;clockFailure_=kIOReturnSuccess;
    // Recovery can only make this exact scratch reference terminal. All
    // existing consumers remain faulted until the separate real reset proof.
    state_=State::FaultedRetained;gart_->firmwareReady_=false;
    if(!clockValid(this))return finishClock(out,clockFailure_);
    gart_->mmioActive_=true;
    const smuclock::Poll poll={timeoutUs,100000,10};
    const auto status=clocks_.faulted()?clocks_.reconcileMetricsForRetirement(poll,out.tableClose):
        clocks_.sealMetricsAfterCompletedTransfer(poll,out.tableClose);
    const auto result=clockError(status);
    if(result!=kIOReturnSuccess)return finishClock(out,result);
    if(clocks_.retentionRequired())return finishClock(out,kIOReturnBusy);
    return finishClock(out,kIOReturnSuccess);
}
