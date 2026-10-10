// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_RESET_CONTROLLER_HPP
#define HACKYNAVI_RESET_CONTROLLER_HPP
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "HackyNaviFunctionGroup.hpp"
#include "hackynavi/device_discovery.hpp"
#include "hackynavi/runtime_rom.hpp"
#include "hackynavi/mode1_reset.hpp"
#include "hackynavi/nbio.hpp"
#include "hackynavi/psp.hpp"
#include "hackynavi/bootstrap_quiesce.hpp"
#include "hackynavi/boot_display_stop.hpp"
#include "hackynavi/cold_aux.hpp"
#include "hackynavi/cold_dma.hpp"
#include "hackynavi/dmcub_firmware.hpp"
class HackyNaviAtomController;

// Native, sleepable MODE1 adapter for a bare-metal Navi31. The owner excludes
// submissions, sibling-audio activity, all MMIO/configuration users and every
// power/reset/removal actor throughout prepare/reset/refresh. PCI open alone
// does not provide that exclusion. Keep every old engine/firmware/DMA owner
// retained across this call; retaining the PCI provider cannot prevent a caller
// from closing it or tearing down its DMA domain. This object never resets a
// live shared GPU.
class HackyNaviResetController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviResetController)
public:
    // Stack binding to the actual outer function-group gate. This guard holds
    // a retain, not Reset's mutex, so normal methods still serialize locally.
    // No hardware access is performed while binding. A revoked retirement
    // scope can therefore still release never-published CPU owners; every
    // hardware entry validates the actual scope before and after its access.
    class Access {
        friend class HackyNaviResetController;
    public:
        Access(HackyNaviResetController*,HackyNaviFunctionGroup::Operation&);
        Access(HackyNaviResetController*,HackyNaviFunctionGroup::RetirementScope&);
        ~Access();
        Access(const Access&)=delete;
        Access& operator=(const Access&)=delete;
        bool valid() const;
    private:
        void bind(HackyNaviResetController*);
        HackyNaviResetController* controller_;
        HackyNaviFunctionGroup::Operation* operation_;
        HackyNaviFunctionGroup::RetirementScope* retirement_;
        IOPCIDevice* device_;
        IOService* owner_;
        IOMemoryMap* map_;
        thread_t thread_;
        uint64_t serial_;
    };
    enum class State { Empty, Attached, Prepared, Resetting, ProtocolCompleteRetained,
        VerifyingRetirement, RetirementVerified, RetirementTransferred,
        FailedUnpublished, FaultedRetained };
    enum class Stage { None, Discovery, Rom, Layout, Snapshot, SmuFirmware,
        Reset, VerifyRestore, ObserveReset, RefreshPciShadow, Complete,
        VerifyPost, StopCpSdma, StopDisplayFirmware, CaptureMemory, StopScanout,
        Auxiliary, InterruptWriteback, RlcDrain, PspStopped, FinalRetirement };
    class RetirementProof {
    public:
        RetirementProof(const RetirementProof&)=delete;
        RetirementProof& operator=(const RetirementProof&)=delete;
        bool belongsTo(const HackyNaviResetController* controller) const { return issuer_==controller; }
        // Caller keeps its outer device epoch lock held from verification
        // through every subowner release and finishRetirementTransfer().
        IOReturn validateFor(IOPCIDevice*,IOService*,IOMemoryMap*) const;
    private:
        RetirementProof():issuer_(nullptr) {}
        HackyNaviResetController* issuer_;
        friend class HackyNaviResetController;
    };
    struct RetirementObservations {
        uint32_t sos,bootloaderCommand,rlcReads,rlcLastRegister,rlcLastValue;
        // Bits 0..6 mark attempted POST, CP/SDMA, DMUB, scanout, media,
        // IH/writeback/MALL and RLC observations. Other zero-initialized
        // module Result fields must not be presented as successful checks.
        uint32_t attemptedChecks;
        uint8_t rlcCheck;
        bool atomCompleted,rlcStopped,verified,deadlineExpired;
        hackynavi::bootstrap::Result cpSdma,posted;
        hackynavi::dmcubfw::Result displayFirmware;
        hackynavi::coldaux::Result auxiliary;
        hackynavi::colddma::Result otherDma;
        hackynavi::bootdisplaystop::Result scanout;
    };
    struct Observations {
        uint32_t smuFirmwareFlags, sosBefore, sosAfter, scratch3Before, scratch3After;
        uint32_t scratch7Before, scratch7After, transactionPolls;
        uint16_t pciCommand, powerControlStatus, expressDeviceStatus;
        bool resetProtocolCompleted, restoredPciVerified, transactionsPendingClear;
        bool resetRegistersObserved, engineHungAnnotationCleared;
        // These observations do not establish ASIC_Init completion or that
        // every internal VRAM / posted host-DMA reference has been retired.
    };
    // Factory validates the actual open provider and assigned uncached BAR5.
    // No reset, PCI shadow, power or engine control writes occur here.
    static HackyNaviResetController* withDevice(IOPCIDevice*, IOService* openOwner,
        IOMemoryMap* assignedUncachedBar5, IOReturn&);
    // Capture selected runtime ROM, scratch base, IP/NBIO layouts before an
    // engine fault can make discovery/ROM acquisition unavailable. Selector
    // writes only. No caller-supplied ROM, layout or readiness flags accepted.
    IOReturn prepare();
    // Retained immutable CPU-only result of the most recent prepare call.
    // Null means serialization failed or no call completed, never success.
    OSDictionary* copyPreparationEvidence();
    // Running SMU INTERRUPTS_ENABLED is checked through discovered RSMU.
    // Executes DEBUG_PARAM=0, DEBUG_MSG=2, DEBUG_RESP=0 and the mandatory 500ms
    // blackout, then fresh BME-off PCI restore and actual readback. No debug
    // ACK/disappearance poll exists. Success leaves BME off and retains this
    // object/provider/maps: host ATOM ASIC_Init and engine recovery come next.
    // Success is NOT permission to call releaseAfterDeviceReset on subowners.
    IOReturn reset();
    // Concrete post-reset retirement: completed MODE1 + this device's native
    // ASIC_Init, then stopped CP/SDMA and all VM contexts, stopped/reset DMUB,
    // HUBP blank/request drain, media/LSDMA/IH/DWB/MALL stopped observations,
    // RLC/SRM/MC/translation drain, and inactive PSP/idle bootloader. The owner
    // must continue excluding all producers through proof consumption. BME
    // remains off. A failed step retains all owners and issues no proof.
    IOReturn verifyRetirement(HackyNaviAtomController*);
    const RetirementProof* retirementProof() const {
        return state_==State::RetirementVerified && retirement_.verified?&proof_:nullptr;
    }
    const RetirementObservations& retirementObservations() const { return retirement_; }
    // After all typed subowner releases, invalidate proof and drop the reset
    // self-hold. The device epoch owner retains its ordinary reference and
    // assumes permanent PCI shadow refresh responsibility before later PM.
    IOReturn finishRetirementTransfer(const RetirementProof&);
    // IOPCI permanent shadow cannot be cleared through the public API. Once
    // installed it persists even after restore. The exclusive owner must call
    // this after later PCI/BME changes and before allowing system PM; ordinary
    // saveDeviceState(0) may otherwise silently reuse that stale shadow. This
    // method always forces a fresh native save and checks live D0/config.
    IOReturn refreshPermanentPciState(HackyNaviFunctionGroup::RetirementScope&);
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    bool permanentPciShadowInstalled() const { return permanentShadow_; }
    // Identity binding for a cooperating post-reset owner. This performs no
    // device access and does not assert power, exclusive access or retirement.
    bool matchesDevice(IOPCIDevice* device, IOService* owner, IOMemoryMap* map) const {
        return device_ == device && openOwner_ == owner && registerMap_ == map;
    }
    // Same-thread actual Access only. These never acquire Reset's mutex and
    // let the cooperating ATOM adapter reject an absent/revoked outer gate.
    bool accessReady() const;
    bool recoveryAccessReady() const;
    State state() const { return state_; }
    Stage stage() const { return stage_; }
    IOReturn lastResult() const { return result_; }
    const hackynavi::mode1::Result& protocolResult() const { return protocol_; }
    const Observations& observations() const { return observations_; }
    // Immutable CPU capture, valid after prepare succeeds, while retained.
    const uint8_t* romBytes() const { return prepared_ ? romBytes_ : nullptr; }
    size_t romByteCount() const { return prepared_ ? rom_.bytesRead : 0; }
    const hackynavi::runtimerom::Result& romCapture() const { return rom_; }
    const hackynavi::FirmwareInfo& firmwareInfo() const { return firmwareInfo_; }
protected:
    bool init() override;
    void free() override;
private:
    struct PciSnapshot {
        uint32_t identity, revisionClass, subsystem, header, bars[6], rom;
        uint16_t command, expressCapabilities, controls[4];
        uint8_t pmOffset, expressOffset, controlCount;
    };
    enum class PreparationStep : uint8_t { Admission, Discovery, IpLayout,
        RomLayout, RomCapture, Subsystem, Firmware, Hdp, Nbio, Psp,
        DebugMessage, DebugResponse, ScratchLayout, PciSnapshot, PciVerify,
        Sos, Scratch3, Scratch7, Complete };
    struct PreparationObservation {
        State stateBefore;
        PreparationStep step;
        uint32_t attempted,completed,rawAttempted,rawReadSucceeded,rawValid;
        uint32_t subsystem;
        hackynavi::IpLayoutDiagnostic diagnostic;
        hackynavi::HdpStatus hdpStatus;
        hackynavi::NbioStatus nbioStatus;
        hackynavi::psp::Status pspStatus;
        hackynavi::IpLayoutStatus messageStatus,responseStatus;
        PciSnapshot pci;
    };
    void preparationStep(PreparationStep);
    void preparationPassed();
    void capturePreparationEvidence(IOReturn);
    IOReturn initialize(IOPCIDevice*,IOService*,IOMemoryMap*);
    const Access* currentAccess() const;
    bool blackoutAccessReady() const;
    bool providerReady() const;
    bool snapshotPci(PciSnapshot&) const;
    bool verifyPci(const PciSnapshot&) const;
    bool readSmuFirmware(uint32_t&);
    bool observeAfterReset();
    bool captureRetirementMemory();
    bool retirementDeadlineReady();
    bool verifyPspStopped();
    bool verifyRlcStopped();
    IOReturn validateRetirement(IOPCIDevice*,IOService*,IOMemoryMap*);
    bool validOffset(uint32_t) const;
    void holdHardware();
    IOReturn finish(IOReturn);
    IOReturn fail(IOReturn);
    static bool read32(void*,uint32_t,uint32_t&);
    static bool write32(void*,uint32_t,uint32_t);
    static bool readPci32(void*,uint32_t,uint32_t&);
    static bool writePci16(void*,uint32_t,uint16_t);
    static bool savePciState(void*);
    static bool restorePciState(void*);
    static void delayMs(void*,uint32_t);
    static void delayUs(void*,uint32_t);
    IOLock* lock_;
    Access* access_;
    thread_t accessThread_;
    uint64_t accessSerial_,proofAccessSerial_;
    IOPCIDevice* device_;
    IOService* openOwner_;
    IOMemoryMap* registerMap_;
    volatile uint32_t* registers_;
    uint64_t registerBytes_,retirementDeadline_;
    uint8_t *discoveryBytes_, *romBytes_;
    bool active_, prepared_, hardwareHold_, permanentShadow_, shadowSaved_;
    bool triggerAttempted_, responseAttempted_, blackout_, blackoutFinished_;
    State state_;
    Stage stage_;
    IOReturn result_;
    uint32_t lastRegister_,lastValue_,scratch3_,scratch7_,debugMessage_,debugResponse_;
    hackynavi::DeviceDiscoveryStatus discoveryStatus_;
    hackynavi::IpLayoutStatus ipStatus_;
    hackynavi::Status firmwareStatus_;
    hackynavi::DeviceDiscoveryResult discovery_;
    hackynavi::runtimerom::Layout romLayout_;
    hackynavi::runtimerom::Result rom_;
    hackynavi::Navi31IpLayout ip_;
    hackynavi::HdpLayout hdp_;
    hackynavi::NbioLayout nbio_;
    hackynavi::psp::Layout psp_;
    hackynavi::FirmwareInfo firmwareInfo_;
    hackynavi::mode1::Result protocol_;
    PciSnapshot saved_;
    Observations observations_;
    RetirementProof proof_;
    RetirementObservations retirement_;
    PreparationObservation preparation_;
    OSDictionary* preparationEvidence_;
    bool preparationRecording_;
    hackynavi::MemoryLayout retirementMemory_;
    hackynavi::bootdisplaystop::State retirementScanout_;
};
#endif
