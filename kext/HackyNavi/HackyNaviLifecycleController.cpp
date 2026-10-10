// SPDX-License-Identifier: MIT
#include "HackyNaviLifecycleController.hpp"
#include "HackyNaviRecoveryEvidence.hpp"
#include <IOKit/IOLib.h>
OSDefineMetaClassAndStructors(HackyNaviLifecycleController,OSObject)

bool HackyNaviLifecycleController::init() {
    lock_=nullptr;device_=nullptr;openOwner_=nullptr;registerMap_=nullptr;
    engine_=nullptr;reset_=nullptr;atom_=nullptr;state_=State::Empty;
    result_=kIOReturnSuccess;epochHold_=false;
    recoveryAudio_={};
    preparation_={};preparationRecording_=false;
    if(!OSObject::init())return false;
    lock_=IOLockAlloc();return lock_!=nullptr;
}
HackyNaviLifecycleController* HackyNaviLifecycleController::withDevice(IOPCIDevice* device,
    IOService* owner,IOMemoryMap* map,IOReturn& result) {
    result=kIOReturnNoMemory;auto* self=new HackyNaviLifecycleController;
    if(!self)return nullptr;
    if(!self->init()){self->release();return nullptr;}
    result=self->initialize(device,owner,map);
    if(result!=kIOReturnSuccess){self->release();return nullptr;}
    return self;
}
IOReturn HackyNaviLifecycleController::initialize(IOPCIDevice* device,IOService* owner,IOMemoryMap* map) {
    if(!device||!owner||!map)return kIOReturnBadArgument;
    device_=device;device_->retain();openOwner_=owner;openOwner_->retain();registerMap_=map;map->retain();
    IOReturn result=kIOReturnError;
    engine_=HackyNaviEngineController::withDevice(device,owner,map,result);
    if(!engine_)return result;
    reset_=HackyNaviResetController::withDevice(device,owner,map,result);
    if(!reset_)return result;
    stage(State::Bound);return kIOReturnSuccess;
}
void HackyNaviLifecycleController::stage(State next) {
    state_=next;
    IOLog("HackyNavi lifecycle: %s\n",stateString(next));
}
void HackyNaviLifecycleController::holdEpoch(){if(!epochHold_){retain();epochHold_=true;}}
bool HackyNaviLifecycleController::hardwareRetained() const {
    return (engine_&&engine_->requiresHardwareRetirement())||
        (reset_&&reset_->requiresHardwareRetirement())||(atom_&&atom_->requiresHardwareRetirement());
}
IOReturn HackyNaviLifecycleController::finish(IOReturn result){
    if(preparationRecording_) {publishRecoveryPreparation(result);preparationRecording_=false;}
    result_=result;IOLockUnlock(lock_);return result;
}
void HackyNaviLifecycleController::publishRecoveryPreparation(IOReturn result) {
    using namespace hackynavi::recoveryevidence;
    auto* out=OSDictionary::withCapacity(24);
    if(!out) {openOwner_->setProperty("RecoveryPreparationEvidenceTruncated",kOSBooleanTrue);return;}
    const auto& p=preparation_;
    bool ok=historical(out)&&number(out,"FormatVersion",1)&&number(out,"OperationGeneration",p.generation)&&
        number(out,"Result",uint32_t(result))&&text(out,"StateBefore",stateString(p.stateBefore))&&
        text(out,"StateAfter",stateString(state_))&&flag(out,"StateAccepted",p.stateAccepted)&&
        flag(out,"ScopeChecked",p.scopeChecked)&&flag(out,"ScopeAccepted",p.scopeAccepted)&&
        flag(out,"ResetAccessChecked",p.accessChecked)&&flag(out,"ResetAccessAccepted",p.accessAccepted)&&
        flag(out,"ResetPreparationAttempted",p.resetAttempted)&&flag(out,"AtomPreparationAttempted",p.atomAttempted)&&
        flag(out,"FinalScopeChecked",p.finalScopeChecked)&&flag(out,"FinalScopeAccepted",p.finalScopeAccepted);
    bool complete=true;
    if(p.resetAttempted) {
        ok=number(out,"ResetPreparationResult",uint32_t(p.resetResult))&&ok;
        auto* reset=reset_->copyPreparationEvidence();
        complete=reset!=nullptr;
        ok=flag(out,"ResetEvidenceAvailable",complete)&&ok;
        if(reset) {ok=out->setObject("ResetPreparation",reset)&&ok;reset->release();}
    }
    if(p.atomAttempted) {
        ok=number(out,"AtomPreparationResult",uint32_t(p.atomResult))&&ok;
        auto* atom=OSDictionary::withCapacity(28);
        if(!atom)ok=false;
        else {
            const auto& a=p.atom;
            bool saved=historical(atom)&&number(atom,"Step",unsigned(a.step))&&number(atom,"Result",uint32_t(a.result))&&
                number(atom,"AttemptedSteps",a.attempted)&&number(atom,"CompletedSteps",a.completed)&&
                number(atom,"RomBytes",a.romBytes)&&number(atom,"PciSubsystem",a.subsystem)&&
                number(atom,"ImageStatus",unsigned(a.imageStatus))&&number(atom,"AtomStatus",unsigned(a.atomStatus))&&
                number(atom,"FirmwareInfoStatus",unsigned(a.firmwareStatus))&&number(atom,"ProgramStatus",unsigned(a.programStatus))&&
                number(atom,"ParameterStatus",unsigned(a.parameterStatus))&&number(atom,"TableOffset",a.tableOffset)&&
                flag(atom,"TableHeaderValid",a.tableHeaderValid)&&number(atom,"TableBytes",a.tableBytes)&&
                flag(atom,"TableCrcRead",a.tableCrcValid)&&number(atom,"TableCrc32",a.tableCrc32)&&
                number(atom,"ScratchBaseDwords",a.scratchBase)&&flag(atom,"AnalysisAttempted",a.analysisAttempted);
            if(a.analysisAttempted) {
                const auto& v=a.analysis;
                saved=number(atom,"AnalysisStatus",unsigned(v.status))&&number(atom,"AnalyzedTables",v.analyzedTables)&&
                    number(atom,"AnalyzedInstructions",v.analyzedInstructions)&&number(atom,"AnalysisTableIndex",v.tableIndex)&&
                    number(atom,"AnalysisTableOffset",v.tableOffset)&&number(atom,"AnalysisInstructionOffset",v.instructionOffset)&&
                    number(atom,"AnalysisOpcode",v.opcode)&&words(atom,"AnalyzedOpcodes",v.opcodes,4)&&
                    words(atom,"AnalyzedCalledTables",v.calledTables,8)&&saved;
            }
            ok=saved&&out->setObject("AtomPreparation",atom)&&ok;atom->release();
        }
    }
    ok=flag(out,"EvidenceComplete",complete)&&ok;
    if(ok)ok=openOwner_->setProperty("RecoveryPreparation",out);
    openOwner_->setProperty("RecoveryPreparationEvidenceTruncated",ok&&complete?kOSBooleanFalse:kOSBooleanTrue);
    out->release();
}
IOReturn HackyNaviLifecycleController::fail(IOReturn result) {
    // A permanent PCI shadow is an ongoing native ownership obligation even
    // when hardware has retired. Never free its refresh owner accidentally.
    const bool retained=hardwareRetained()||(reset_&&reset_->permanentPciShadowInstalled());
    stage(retained?State::FaultedRetained:State::FailedUnpublished);
    if(retained)holdEpoch();
    const bool drop=epochHold_&&!retained;if(drop)epochHold_=false;
    IOLog("HackyNavi lifecycle: result=0x%x engine=%s reset-stage=%u atom-state=%u epoch=%u\n",
        unsigned(result),engine_?HackyNaviEngineController::stageString(engine_->stage()):"Unavailable",
        reset_?unsigned(reset_->stage()):0,atom_?unsigned(atom_->state()):0,unsigned(epochHold_));
    finish(result);if(drop)release();return result;
}
IOReturn HackyNaviLifecycleController::prepareRecovery(HackyNaviFunctionGroup::Operation& scope) {
    IOLockLock(lock_);preparation_={};preparation_.stateBefore=state_;preparation_.generation=scope.generation();
    preparation_.resetResult=preparation_.atomResult=kIOReturnNotReady;preparationRecording_=true;
    if(state_!=State::Bound)return finish(kIOReturnNotReady);
    preparation_.stateAccepted=true;preparation_.scopeChecked=true;
    preparation_.scopeAccepted=scope.matchesGraphics(device_,openOwner_);
    if(!preparation_.scopeAccepted)return finish(kIOReturnOffline);
    HackyNaviResetController::Access access(reset_,scope);
    preparation_.accessChecked=true;preparation_.accessAccepted=access.valid();
    if(!preparation_.accessAccepted)return finish(kIOReturnOffline);
    preparation_.resetAttempted=true;
    IOReturn result=reset_->prepare();preparation_.resetResult=result;if(result!=kIOReturnSuccess)return fail(result);
    preparation_.atomAttempted=true;
    atom_=HackyNaviAtomController::withReset(reset_,device_,openOwner_,registerMap_,result,&preparation_.atom);
    preparation_.atomResult=result;
    if(!atom_)return fail(result);
    preparation_.finalScopeChecked=true;preparation_.finalScopeAccepted=scope.matchesGraphics(device_,openOwner_);
    if(!preparation_.finalScopeAccepted)return fail(kIOReturnOffline);
    stage(State::RecoveryPrepared);return finish(kIOReturnSuccess);
}
IOReturn HackyNaviLifecycleController::start(HackyNaviFunctionGroup::Operation& scope) {
    IOLockLock(lock_);if(state_!=State::RecoveryPrepared)return finish(kIOReturnNotReady);
    if(!scope.matchesGraphics(device_,openOwner_))return finish(kIOReturnOffline);
    holdEpoch();stage(State::Starting);
    const IOReturn result=engine_->start(scope);if(result!=kIOReturnSuccess)return fail(result);
    stage(State::Running);return finish(kIOReturnSuccess);
}
IOReturn HackyNaviLifecycleController::completeRetirement(HackyNaviFunctionGroup::RetirementScope& scope) {
    // retire() or the public refresh entry owns the one Reset::Access guard.
    // Do not bind another here: proof validation and all typed releases must
    // remain in the same guard, including the final permanent-shadow refresh.
    if(hardwareRetained())return fail(kIOReturnNotReady);
    if(reset_->permanentPciShadowInstalled()) {
        if(!scope.matchesGraphics(device_,openOwner_)||!scope.permitsPciRecovery())return fail(kIOReturnOffline);
        const IOReturn result=reset_->refreshPermanentPciState(scope);
        if(result!=kIOReturnSuccess)return fail(result);
        if(!scope.matchesGraphics(device_,openOwner_))return fail(kIOReturnOffline);
        holdEpoch();stage(State::RetiredShadowMaintenance);
        return finish(kIOReturnSuccess);
    }
    stage(State::Retired);const bool drop=epochHold_;epochHold_=false;
    finish(kIOReturnSuccess);if(drop)release();return kIOReturnSuccess;
}
IOReturn HackyNaviLifecycleController::createClientSession(HackyNaviFunctionGroup* group,
    HackyNaviFunctionGroup::Operation& scope,HackyNaviClientSession*& out) {
    out=nullptr;IOLockLock(lock_);
    if(state_!=State::Running||!engine_||!scope.belongsTo(group)||!scope.matchesGraphics(device_,openOwner_))
        return finish(kIOReturnNotReady);
    return finish(engine_->createClientSession(group,scope,out));
}
IOReturn HackyNaviLifecycleController::retireAfterResetLocked(HackyNaviFunctionGroup::RetirementScope& scope) {
    if(!atom_||reset_->state()!=HackyNaviResetController::State::Prepared)return fail(kIOReturnNotReady);
    // A startup failure can be the sibling HDA stop itself. Owning its PCI
    // function is not evidence that its controller stopped. Refresh actual
    // audio stop before a GPU-wide reset, under this cleanup scope/deadline.
    const uint32_t remaining=scope.remainingUs();
    if(!remaining)return fail(kIOReturnTimeout);
    IOReturn result=scope.quiesceAudio(remaining<1000000?remaining:1000000,recoveryAudio_);
    if(result!=kIOReturnSuccess||!recoveryAudio_.complete)
        return fail(result==kIOReturnSuccess?kIOReturnNotReady:result);
    // The callback's atomic terminal/recovery handshake must precede every
    // MODE1/PCI restore dependency. A termination workloop cannot wait for it.
    result=scope.enterRecovery();if(result!=kIOReturnSuccess)return fail(result);
    if(!scope.matchesGraphics(device_,openOwner_))return fail(kIOReturnOffline);
    // The retained SMU table producer must be closed in its original firmware
    // epoch. MODE1/ASIC_Init is not evidence of a cleared driver-table pointer.
    // This is terminal reconciliation only; common firmware readiness stays
    // revoked and no old consumer is allowed to resume before the real reset.
    result=engine_->prepareClockTableForReset(scope);if(result!=kIOReturnSuccess)return fail(result);
    holdEpoch();stage(State::Resetting);scope.progress(uint32_t(State::Resetting));result=reset_->reset();
    if(result!=kIOReturnSuccess)return fail(result);
    stage(State::Reinitializing);result=atom_->initializeAfterMode1();
    if(result!=kIOReturnSuccess)return fail(result);
    stage(State::VerifyingRetirement);result=reset_->verifyRetirement(atom_);
    if(result!=kIOReturnSuccess)return fail(result);
    const auto* proof=reset_->retirementProof();if(!proof)return fail(kIOReturnNotReady);
    stage(State::RetiringOwners);
    result=engine_->releaseAfterVerifiedReset(*proof,scope);if(result!=kIOReturnSuccess)return fail(result);
    result=atom_->releaseAfterVerifiedReset(*proof);if(result!=kIOReturnSuccess)return fail(result);
    result=reset_->finishRetirementTransfer(*proof);if(result!=kIOReturnSuccess)return fail(result);
    return completeRetirement(scope);
}
IOReturn HackyNaviLifecycleController::retire(HackyNaviFunctionGroup::RetirementScope& scope) {
    IOLockLock(lock_);
    if(state_==State::Retired)return finish(kIOReturnSuccess);
    HackyNaviResetController::Access access(reset_,scope);
    if(state_==State::RetiredShadowMaintenance)return completeRetirement(scope);
    if(state_!=State::Bound&&state_!=State::RecoveryPrepared&&state_!=State::Running&&
        state_!=State::FailedUnpublished&&state_!=State::FaultedRetained)return finish(kIOReturnNotReady);
    // An unplug before any control/DMA publication requires CPU cleanup only.
    // Do not demand readable hardware to discard those unpublished owners.
    if((hardwareRetained()||reset_->permanentPciShadowInstalled())&&
        (!access.valid()||!scope.matchesGraphics(device_,openOwner_)))return finish(kIOReturnOffline);
    // Engine independently admits one orderly stop only with its actual
    // healthy descendants. A late CPU-only startup failure can qualify;
    // ambiguous child state, earlier startup and failed stop still use reset.
    if(engine_->state()==HackyNaviEngineController::State::Ready||
        engine_->state()==HackyNaviEngineController::State::Prepared||
        engine_->state()==HackyNaviEngineController::State::FailedUnpublished||
        engine_->state()==HackyNaviEngineController::State::FaultedRetained) {
        stage(State::Stopping);scope.progress(uint32_t(State::Stopping));const IOReturn result=engine_->stop(scope);
        if(result==kIOReturnSuccess)return completeRetirement(scope);
        if(!hardwareRetained())return fail(result);
    }
    return retireAfterResetLocked(scope);
}
IOReturn HackyNaviLifecycleController::refreshPermanentPciState(HackyNaviFunctionGroup::RetirementScope& scope) {
    IOLockLock(lock_);
    if(state_!=State::RetiredShadowMaintenance)return finish(kIOReturnNotReady);
    HackyNaviResetController::Access access(reset_,scope);
    if(!access.valid())return finish(kIOReturnOffline);
    return completeRetirement(scope);
}
void HackyNaviLifecycleController::snapshot(Snapshot& out) {
    IOLockLock(lock_);out={};out.state=state_;out.operationResult=result_;
    out.engineResult=out.resetResult=out.atomResult=kIOReturnNotReady;
    if(engine_){out.engineStage=engine_->stage();out.engineResult=engine_->lastResult();
        out.engineRetained=engine_->requiresHardwareRetirement();
        engine_->snapshotObservation(out.engineObservation);}
    if(reset_){out.resetStage=reset_->stage();out.resetResult=reset_->lastResult();
        out.resetRetained=reset_->requiresHardwareRetirement();out.permanentPciShadow=reset_->permanentPciShadowInstalled();}
    if(atom_){out.atomState=atom_->state();out.atomResult=atom_->lastResult();out.atomRetained=atom_->requiresHardwareRetirement();}
    out.deviceEpochRetained=epochHold_;IOLockUnlock(lock_);
}
bool HackyNaviLifecycleController::requiresDeviceEpoch() const {
    IOLockLock(lock_);const bool held=epochHold_;IOLockUnlock(lock_);return held;
}
const char* HackyNaviLifecycleController::stateString(State state) {
    switch(state) {
#define NAME(x) case State::x:return #x
    NAME(Empty);NAME(Bound);NAME(RecoveryPrepared);NAME(Starting);NAME(Running);
    NAME(Stopping);NAME(Resetting);NAME(Reinitializing);NAME(VerifyingRetirement);NAME(RetiringOwners);
    NAME(FailedUnpublished);NAME(FaultedRetained);NAME(Retired);NAME(RetiredShadowMaintenance);
#undef NAME
    }return "Unknown";
}
void HackyNaviLifecycleController::free() {
    // Every actual hardware operation first takes the self hold. This path is
    // reachable only before publication or after ordinary successful retirement.
    if(atom_)atom_->release();if(reset_)reset_->release();if(engine_)engine_->release();
    if(registerMap_)registerMap_->release();if(openOwner_)openOwner_->release();if(device_)device_->release();
    if(lock_)IOLockFree(lock_);OSObject::free();
}
