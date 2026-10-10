// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_ENGINE_CONTROLLER_HPP
#define HACKYNAVI_ENGINE_CONTROLLER_HPP
#include "HackyNaviGraphicsQueue.hpp"
#include "HackyNaviDisplayController.hpp"
#include "HackyNaviResetController.hpp"
#include "HackyNaviBootConsole.hpp"
#include "HackyNaviBootBackendCapture.hpp"
#include "HackyNaviFunctionGroup.hpp"
#include "hackynavi/device_discovery.hpp"
#include "hackynavi/runtime_rom.hpp"
#include "hackynavi/boot_display.hpp"
#include "hackynavi/boot_display_stop.hpp"
#include "hackynavi/cold_aux.hpp"
#include "hackynavi/cold_dma.hpp"
#include "hackynavi/bootstrap_quiesce.hpp"
#include "hackynavi/vram_reservations.hpp"
#include "hackynavi/nbio.hpp"
class HackyNaviClientSession;
class OSDictionary;
class OSArray;

// Native cold first-attach coordinator. The caller owns the PCI power/reset/
// removal epoch and excludes all other register, firmware, ATOM and autonomous
// power-management actors for its whole lifetime. IOPCIDevice::open alone does
// not supply that exclusion. The caller must defer PCI close, DMA-domain and
// power teardown while requiresHardwareRetirement() is true.
// No user callbacks, invented quiescence flags or test backend are accepted.
class HackyNaviEngineController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviEngineController)
public:
    enum class State { Empty, Prepared, Starting, Ready, FailedUnpublished,
        FaultedRetained, Retired };
    enum class Stage { None, Discovery, Rom, Memory, Posted, Training,
        Auxiliary, StopInheritedDisplayFirmware, BootDisplay, StopEngines, ColdDma,
        Reservations, Nbio, AllocateGart, UploadGart, BusMaster, Mmhub,
        Doorbell, FirmwareOwner, Sos, PspRing, BootConfig, Tmr, GraphicsFirmware,
        Gfxhub, GraphicsOwner, GraphicsPrepare, GraphicsStart, GraphicsVerify, ShaderVerify,
        DisplayOwner, DisplayBoot, Complete, StopDisplay, StopGraphics,
        RetireRlc, RetireFirmware, StopBootScanout, RetireGart, RetireNbio,
        VerifiedReset, DisableBusMaster, BootConsole, StopSiblingAudio,
        PreflightBootDisplay, PreflightBootConsole, ReusableVerify,
        StopDisplayConsumers, StopDisplayFirmware, ObserveBootBackend,
        RetireDisplayTrustedApplications };
    struct DisplayObservation {
        bool attempted;
        hackynavi::bootdisplay::Status status;
        uint32_t reads,lastRegister,lastValue;
        bool lastReadSucceeded;
        uint8_t failedPipe;
        hackynavi::bootdisplay::PowerProbe lastPowerProbe;
    };
    struct DisplayStopObservation {
        IOReturn result,clockRetirementResult;
        uint64_t elapsedUs;
        bool attempted,consumersStopped,physicalIdleChecked,firmwareStopped,
            runtimeRetained,policyRetained,leaseReleased,clockRetirementAttempted;
    };
    struct BackendObservation {
        bool attempted,valid;
        uint8_t path,backend,frontend,timing,opp,mpcc,dpp,bitsPerComponent;
        bool scrambled,quarterRate;
        HackyNaviBootBackendCapture::Report report;
    };
    struct DisplayTaRetirementObservation {
        IOReturn result;
        uint64_t generation,elapsedUs;
        uint32_t protocolStatus,command,applicationStatus,pspStatus,taPspStatus;
        uint32_t authenticationsDestroyed,topologiesRemoved,applicationsUnloaded;
        bool attempted,protocolAttempted,applicationAttempted,protocolRetired,retentionRequired;
    };
    // Bounded historical values only. No Operation, first-attach token,
    // reservation plan or other hardware authority can escape in this POD.
    struct Observation {
        DisplayObservation preflightDisplay;
        BackendObservation backend;
        bool preflightConsoleAttempted,scanoutAttempted,consoleAttempted;
        bool controlWritesStarted,cpAttempted,shaderAttempted,reuseAttempted;
        hackynavi::bootconsole::Capture preflightConsole,console;
        hackynavi::bootdisplaystop::Result scanout;
        HackyNaviGraphicsQueue::WriteTestResult cp;
        HackyNaviGraphicsQueue::ShaderTestResult shader;
        HackyNaviGraphicsQueue::ReuseTestResult reuse;
        DisplayStopObservation stopConsumers,stopFirmware;
        DisplayTaRetirementObservation displayTaRetirement;
    };
    // CPU allocation and PCI/resource validation only; no register writes.
    // start acquires fresh discovery and selected ROM from this actual device.
    static HackyNaviEngineController* withDevice(IOPCIDevice*, IOService* openOwner,
        IOMemoryMap* assignedUncachedBar5, IOReturn&);
    // Sleepable context, never an interrupt or workloop/spinlock context.
    // Success includes actual GPU-written graphics fence/data verification and
    // the running DAL DMCUB version response. This does not advertise Metal.
    // The actual normal scope covers GART factory allocation and a lexical
    // GART::Access spanning every subsequent descendant hardware operation.
    IOReturn start(HackyNaviFunctionGroup::Operation&);
    IOReturn createClientSession(HackyNaviFunctionGroup*,HackyNaviFunctionGroup::Operation&,
        HackyNaviClientSession*&);
    // Terminal retirement. No retry of an ambiguous startup or stop. Existing
    // boot scanout stays reserved through actual HUBP blank and request drain.
    // A retirement Access covers all existing descendants; unpublished CPU
    // cleanup remains possible before requiring readable PCI hardware.
    // A failed startup may attempt this once only when its actual GART,
    // Firmware and Prepared or Running GraphicsQueue remain healthy. Earlier failures,
    // faulted descendants and partial stop failures still need real reset.
    IOReturn stop(HackyNaviFunctionGroup::RetirementScope&);
    // Consume this provider's native MODE1/ASIC_Init retirement proof while
    // the caller continues holding the exclusive device epoch. No caller
    // boolean or assertion can replace the concrete hardware verification.
    IOReturn releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof&,
        HackyNaviFunctionGroup::RetirementScope&);
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    State state() const { return state_; }
    Stage stage() const { return stage_; }
    IOReturn lastResult() const { return result_; }
    const hackynavi::VramReservationPlan& reservations() const { return reservations_; }
    const hackynavi::MemoryLayout& memoryLayout() const { return memory_; }
    const hackynavi::bootconsole::Capture& bootConsoleCapture() const { return bootConsole_; }
    const HackyNaviGraphicsQueue::WriteTestResult& graphicsVerification() const { return writeTest_; }
    const HackyNaviGraphicsQueue::ShaderTestResult& shaderVerification() const { return shaderTest_; }
    const HackyNaviGraphicsQueue::ReuseTestResult& reuseVerification() const { return reuseTest_; }
    uint32_t bootConfiguration() const { return bootConfiguration_; }
    void snapshotObservation(Observation&);
    static const char* stageString(Stage);
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviLifecycleController;
    // Private one-shot handoff called after enterRecovery and before the
    // prepared ResetController's first MODE1 attempt. This only closes SMU
    // scratch references; it is not GPU reset or general DMA retirement.
    IOReturn prepareClockTableForReset(HackyNaviFunctionGroup::RetirementScope&);
    IOReturn initialize(IOPCIDevice*,IOService*,IOMemoryMap*);
    bool providerReady(bool requireDma=false) const;
    IOReturn programBusMaster(bool enabled);
    IOReturn startupResult(IOReturn) const;
    bool captureMemory();
    void capturePrerequisiteEvidence(bool postedCold);
    bool captureRawEvidence(const char*,const uint32_t*,size_t,uint32_t zeroGuard=0);
    void captureCpGuardEvidence();
    void recordStageEvidence(IOReturn,bool completed);
    static bool preflightAccessValid(void*);
    hackynavi::bootdisplay::Status captureDisplayPreflight();
    IOReturn observeBootBackend(HackyNaviFunctionGroup::Operation&);
    bool checkTrainingIdle();
    bool makeReservations();
    void enterStage(Stage);
    void holdHardware();
    IOReturn finish(IOReturn);
    IOReturn fail(IOReturn);
    void releaseOwners();
    void logFailure() const;
    static bool read32(void*,uint32_t,uint32_t&);
    static bool write32(void*,uint32_t,uint32_t);
    static void delayUs(void*,uint32_t);
    IOLock* lock_;
    IOPCIDevice* device_;
    IOService* openOwner_;
    IOMemoryMap* registerMap_;
    volatile uint32_t* registers_;
    uint64_t registerBytes_;
    uint8_t* discoveryBytes_;
    uint8_t* romBytes_;
    bool mmioActive_, hardwareHold_, controlWritesStarted_, busMasterChanged_;
    bool resetPreparationAttempted_,stopAttempted_,stageRecorded_;
    const HackyNaviFunctionGroup::Operation* startupScope_;
    const HackyNaviFunctionGroup::RetirementScope* retirementScope_;
    const HackyNaviFunctionGroup::FirstAttach* firstAttach_;
    State state_;
    Stage stage_;
    IOReturn result_;
    // Failed factory cleanup can outlive a null subowner. Hardware reset does
    // not erase an IODMACommand/mapper teardown failure or permit DMA reuse.
    IOReturn factoryTeardownFailure_;
    uint32_t lastRegister_,lastValue_;
    Observation observation_;
    OSDictionary* prerequisiteEvidence_; // Private CPU-only diagnostic builder.
    OSArray* stageEvidence_; // Child dictionaries are immutable once appended.
    hackynavi::DeviceDiscoveryStatus discoveryStatus_;
    hackynavi::IpLayoutStatus ipStatus_;
    hackynavi::MemoryLayoutStatus memoryStatus_;
    hackynavi::Status firmwareInfoStatus_;
    hackynavi::VramReservationStatus reservationStatus_;
    hackynavi::NbioStatus nbioStatus_;
    hackynavi::DeviceDiscoveryResult discovery_;
    hackynavi::runtimerom::Result rom_;
    hackynavi::Navi31IpLayout ip_;
    hackynavi::MemoryLayout memory_;
    hackynavi::FirmwareInfo firmwareInfo_;
    hackynavi::VramUsageInfo usage_;
    hackynavi::Status usageStatus_;
    hackynavi::bootdisplaystop::State bootScanout_;
    hackynavi::bootdisplaystop::Result bootScanoutResult_;
    hackynavi::bootconsole::Capture bootConsole_;
    hackynavi::coldaux::Status auxiliaryLayoutStatus_;
    hackynavi::coldaux::LsdmaResult lsdma_;
    HackyNaviFunctionGroup::AudioStopReport audioStop_;
    hackynavi::colddma::Result coldDma_;
    hackynavi::bootstrap::Result bootstrap_;
    hackynavi::dmcubfw::Result inheritedDisplay_;
    hackynavi::VramReservationPlan reservations_;
    hackynavi::NbioState nbio_;
    hackynavi::NbioResult nbioResult_;
    uint32_t bootConfiguration_;
    HackyNaviGraphicsQueue::WriteTestResult writeTest_;
    HackyNaviGraphicsQueue::ShaderTestResult shaderTest_;
    HackyNaviGraphicsQueue::ReuseTestResult reuseTest_;
    HackyNaviGARTController* gart_;
    HackyNaviFirmwareController* firmware_;
    HackyNaviGraphicsQueue* graphics_;
    HackyNaviDisplayController* display_;
    HackyNaviBootBackendCapture* bootBackend_;
};
#endif
