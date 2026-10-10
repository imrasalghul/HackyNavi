// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_ENGINE_CONTROLLER_HPP
#define HACKYNAVI_ENGINE_CONTROLLER_HPP
#include "HackyNaviGraphicsQueue.hpp"
#include "HackyNaviDisplayController.hpp"
#include "HackyNaviResetController.hpp"
#include "hackynavi/device_discovery.hpp"
#include "hackynavi/runtime_rom.hpp"
#include "hackynavi/boot_display.hpp"
#include "hackynavi/boot_display_stop.hpp"
#include "hackynavi/cold_aux.hpp"
#include "hackynavi/cold_dma.hpp"
#include "hackynavi/bootstrap_quiesce.hpp"
#include "hackynavi/vram_reservations.hpp"
#include "hackynavi/nbio.hpp"

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
        VerifiedReset };
    // CPU allocation and PCI/resource validation only; no register writes.
    // start acquires fresh discovery and selected ROM from this actual device.
    static HackyNaviEngineController* withDevice(IOPCIDevice*, IOService* openOwner,
        IOMemoryMap* assignedUncachedBar5, IOReturn&);
    // Sleepable context, never an interrupt or workloop/spinlock context.
    // Success includes actual GPU-written graphics fence/data verification and
    // the running DAL DMCUB version response. This does not advertise Metal.
    IOReturn start();
    // Terminal retirement. No retry of an ambiguous startup or stop. Existing
    // boot scanout stays reserved through actual HUBP blank and request drain.
    IOReturn stop();
    // Consume this provider's native MODE1/ASIC_Init retirement proof while
    // the caller continues holding the exclusive device epoch. No caller
    // boolean or assertion can replace the concrete hardware verification.
    IOReturn releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof&);
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    State state() const { return state_; }
    Stage stage() const { return stage_; }
    IOReturn lastResult() const { return result_; }
    const hackynavi::VramReservationPlan& reservations() const { return reservations_; }
    const hackynavi::MemoryLayout& memoryLayout() const { return memory_; }
    const HackyNaviGraphicsQueue::WriteTestResult& graphicsVerification() const { return writeTest_; }
    const HackyNaviGraphicsQueue::ShaderTestResult& shaderVerification() const { return shaderTest_; }
    uint32_t bootConfiguration() const { return bootConfiguration_; }
    static const char* stageString(Stage);
protected:
    bool init() override;
    void free() override;
private:
    IOReturn initialize(IOPCIDevice*,IOService*,IOMemoryMap*);
    bool providerReady(bool requireDma=false) const;
    bool captureMemory();
    bool checkTrainingIdle();
    bool makeReservations();
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
    State state_;
    Stage stage_;
    IOReturn result_;
    uint32_t lastRegister_,lastValue_;
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
    hackynavi::coldaux::Result auxiliary_;
    hackynavi::colddma::Result coldDma_;
    hackynavi::bootstrap::Result bootstrap_;
    hackynavi::dmcubfw::Result inheritedDisplay_;
    hackynavi::VramReservationPlan reservations_;
    hackynavi::NbioState nbio_;
    hackynavi::NbioResult nbioResult_;
    uint32_t bootConfiguration_;
    HackyNaviGraphicsQueue::WriteTestResult writeTest_;
    HackyNaviGraphicsQueue::ShaderTestResult shaderTest_;
    HackyNaviGARTController* gart_;
    HackyNaviFirmwareController* firmware_;
    HackyNaviGraphicsQueue* graphics_;
    HackyNaviDisplayController* display_;
};
#endif
