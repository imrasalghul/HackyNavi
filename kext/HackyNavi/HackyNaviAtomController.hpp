// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_ATOM_CONTROLLER_HPP
#define HACKYNAVI_ATOM_CONTROLLER_HPP
#include "HackyNaviResetController.hpp"
#include "hackynavi/atom_vm.hpp"
#include <libkern/c++/OSData.h>

// Host ATOM ASIC_Init following this same provider's completed MODE1 protocol.
// The caller excludes all concurrent MMIO/ATOM, submissions, audio-function,
// power, reset, removal and firmware users throughout execution. The method is
// sleepable. PCI open/this object's lock alone does not supply that exclusion.
// Old engine/firmware/DMA owners MUST remain retained throughout recovery.
class HackyNaviAtomController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviAtomController)
public:
    enum class State { Empty, Prepared, Executing, InitializedRetained,
        FailedUnpublished, FaultedRetained, Retired };
    struct Observations {
        uint32_t scratch7Before, scratch7After;
        uint32_t waitReads, waitValue, lastRegisterDword, lastValue;
        uint16_t pciCommand;
        bool tableExecuted, firmwareWaitSatisfied, scratchMarkerSet;
        bool deadlineExpired;
    };
    enum class PreparationStep : uint8_t { Allocation, Binding, State, Provider,
        RomCopy, Image, Atom, Firmware, Program, Parameters, Profile,
        Workspace, Analysis, Complete };
    struct PreparationObservation {
        PreparationStep step;
        IOReturn result;
        uint32_t attempted,completed,subsystem,tableOffset,tableBytes,tableCrc32,scratchBase;
        uint64_t romBytes;
        hackynavi::Status imageStatus,atomStatus,firmwareStatus;
        hackynavi::atomvm::Status programStatus,parameterStatus;
        hackynavi::atomvm::Result analysis;
        bool tableHeaderValid,tableCrcValid,analysisAttempted;
    };
    // No hardware writes: binds exact ResetController provider/owner/BAR map,
    // copies its captured live ROM, validates the known Navi31 ASIC_Init profile
    // and creates CPU-only parameter/scratch/interpreter storage. May be called
    // after ResetController::prepare, before reset(), to preflight recovery.
    // Reset's actual Access guard must span this factory's provider reads.
    static HackyNaviAtomController* withReset(HackyNaviResetController*,
        IOPCIDevice*,IOService* openOwner,IOMemoryMap* assignedUncachedBar5,IOReturn&,
        PreparationObservation* observation=nullptr);
    // Requires reset protocol complete, independently restored PCI state and
    // transactions pending clear; BME must stay off. Executes real bytecode and
    // checks the firmware's hardware wait plus SCRATCH7 marker. The bytecode
    // writes that marker even after its wait counter expires, so it alone is
    // insufficient. Requires Reset's actual retirement Access with PCI recovery
    // entered; a normal operation cannot execute this command table.
    // Success is not proof that old GPU memory references retired,
    // that new firmware/engines work, or that display/acceleration is ready.
    IOReturn initializeAfterMode1();
    // Bookkeeping only, gated by this exact reset owner's fresh hardware
    // retirement proof. Does not perform another reset or retire GPU references.
    IOReturn releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof&);
    State state() const { return state_; }
    IOReturn lastResult() const { return result_; }
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    bool matchesReset(const HackyNaviResetController* reset) const { return reset_==reset; }
    const hackynavi::atomvm::Result& executionResult() const { return execution_; }
    const Observations& observations() const { return observations_; }
protected:
    bool init() override;
    void free() override;
private:
    IOReturn initialize(HackyNaviResetController*,IOPCIDevice*,IOService*,IOMemoryMap*);
    void preparationStep(PreparationStep);
    void preparationPassed();
    bool providerReady(bool busMasterOff) const;
    bool accessReady(uint32_t registerDword);
    bool deadlineReady();
    IOReturn finish(IOReturn);
    IOReturn fail(IOReturn);
    void holdHardware();
    static bool read(void*,hackynavi::atomvm::Space,uint32_t,uint32_t&);
    static bool write(void*,hackynavi::atomvm::Space,uint32_t,uint32_t);
    static bool delay(void*,uint32_t);
    static void event(void*,hackynavi::atomvm::Event,uint8_t);
    IOLock* lock_;
    HackyNaviResetController* reset_;
    IOPCIDevice* device_;
    IOService* openOwner_;
    IOMemoryMap* map_;
    volatile uint32_t* registers_;
    uint64_t registerBytes_,deadline_;
    OSData* rom_;
    hackynavi::atomvm::Workspace* workspace_;
    uint8_t* scratch_;
    size_t scratchBytes_;
    uint8_t parameters_[hackynavi::atomvm::kAsicInitParameterBytes];
    hackynavi::atomvm::Program program_;
    hackynavi::atomvm::Result execution_;
    Observations observations_;
    PreparationObservation preparation_;
    uint32_t subsystem_,scratch7Dword_,selector_;
    bool active_,hardwareHold_,selectorKnown_;
    State state_;
    IOReturn result_;
};
#endif
