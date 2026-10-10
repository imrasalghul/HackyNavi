// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_LIFECYCLE_CONTROLLER_HPP
#define HACKYNAVI_LIFECYCLE_CONTROLLER_HPP
#include "HackyNaviEngineController.hpp"
#include "HackyNaviAtomController.hpp"

// Sleepable native engine/recovery sequencing. This is NOT a PM veto or a
// substitute for PCI function-group ownership. The caller must exclude GPU
// and sibling-audio clients, firmware producers, reset and parent power loss
// throughout every operation and while requiresDeviceEpoch() remains true.
// Startup requires the actual FunctionGroup Operation and a registered owner
// retirement handler; a diagnostic PCI open by itself cannot activate engines.
// A failed reset retains actual owners; no error return authorizes a PM ACK,
// BAR unmap, PCI close, DMA-domain teardown or freeing published allocations.
class HackyNaviLifecycleController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviLifecycleController)
public:
    enum class State { Empty, Bound, RecoveryPrepared, Starting, Running,
        Stopping, Resetting, Reinitializing, VerifyingRetirement, RetiringOwners,
        FailedUnpublished, FaultedRetained, Retired, RetiredShadowMaintenance };
    struct Snapshot {
        State state;
        HackyNaviEngineController::Stage engineStage;
        HackyNaviResetController::Stage resetStage;
        HackyNaviAtomController::State atomState;
        IOReturn operationResult, engineResult, resetResult, atomResult;
        bool engineRetained,resetRetained,atomRetained,permanentPciShadow,deviceEpochRetained;
        HackyNaviEngineController::Observation engineObservation;
    };
    // CPU allocations and exact PCI/BAR identity binding only; no MMIO writes,
    // reset, DMA publication, sibling claiming or accelerator advertisement.
    static HackyNaviLifecycleController* withDevice(IOPCIDevice*,IOService* openOwner,
        IOMemoryMap* assignedUncachedBar5,IOReturn&);
    // Capture recovery inputs before an engine fault can make ROM/discovery
    // unavailable. Uses real selector-based reads, then validates the native
    // ASIC_Init interpreter profile without executing it or resetting anything.
    // One Reset::Access binds this operation through both capture and ATOM
    // factory reads, then expires when preparation returns.
    IOReturn prepareRecovery(HackyNaviFunctionGroup::Operation&);
    // Sleepable thread context only. Requires prepareRecovery success. Start
    // really runs EngineController and its fence/shader/DMCUB verification.
    // Failure does not automatically reset a potentially shared PCI device.
    IOReturn start(HackyNaviFunctionGroup::Operation&);
    IOReturn createClientSession(HackyNaviFunctionGroup*,HackyNaviFunctionGroup::Operation&,
        HackyNaviClientSession*&);
    // First performs actual normal engine stop. A published/ambiguous failure
    // then runs MODE1 -> native ASIC_Init -> hardware retirement verification
    // -> typed Engine/ATOM releases -> reset proof transfer. All owners remain
    // retained on any failed step. Reset is one-shot; failed recovery is not
    // retried by re-running a command against unknown hardware state.
    // One Reset::Access spans the full callback, including verification, every
    // typed owner release and permanent-shadow refresh. Never-published CPU
    // cleanup remains possible when a revoked scope cannot authorize hardware.
    IOReturn retire(HackyNaviFunctionGroup::RetirementScope&);
    // MODE1 installs a permanent IOPCI shadow. A successful fallback therefore
    // keeps the epoch/self hold even after DMA retirement; the outer PCI owner
    // must retain this owner and refresh the shadow after later config/BME
    // changes and before PM. There is no invented "clear permanent" API.
    IOReturn refreshPermanentPciState(HackyNaviFunctionGroup::RetirementScope&);
    // These observations take the same sleepable mutex as state transitions;
    // a read of the ownership hold must not race retirement's final release.
    // The caller retains this object while invoking any method. A returned
    // snapshot is an observation, not a new PCI-power/reset exclusion lease.
    void snapshot(Snapshot&);
    bool requiresDeviceEpoch() const;
    static const char* stateString(State);
protected:
    bool init() override;
    void free() override;
private:
    IOReturn initialize(IOPCIDevice*,IOService*,IOMemoryMap*);
    IOReturn retireAfterResetLocked(HackyNaviFunctionGroup::RetirementScope&);
    IOReturn finish(IOReturn);
    IOReturn fail(IOReturn);
    IOReturn completeRetirement(HackyNaviFunctionGroup::RetirementScope&);
    bool hardwareRetained() const;
    void stage(State);
    void holdEpoch();
    void publishRecoveryPreparation(IOReturn);
    IOLock* lock_;
    IOPCIDevice* device_;
    IOService* openOwner_;
    IOMemoryMap* registerMap_;
    HackyNaviEngineController* engine_;
    HackyNaviResetController* reset_;
    HackyNaviAtomController* atom_;
    State state_;
    IOReturn result_;
    HackyNaviFunctionGroup::AudioStopReport recoveryAudio_;
    struct PreparationObservation {
        State stateBefore;
        uint64_t generation;
        bool stateAccepted,scopeChecked,scopeAccepted,accessChecked,accessAccepted;
        bool resetAttempted,atomAttempted,finalScopeChecked,finalScopeAccepted;
        IOReturn resetResult,atomResult;
        HackyNaviAtomController::PreparationObservation atom;
    } preparation_;
    bool preparationRecording_;
    bool epochHold_;
};
#endif
