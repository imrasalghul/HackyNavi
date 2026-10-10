// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DEVICE_HPP
#define HACKYNAVI_DEVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <stdint.h>
#include <hackynavi/ip_layout.hpp>
#include <hackynavi/memory_layout.hpp>
#include <hackynavi/device_discovery.hpp>
#include <hackynavi/hdp.hpp>
#include <hackynavi/boot_display.hpp>
#include <hackynavi/boot_display_context.hpp>
#include <hackynavi/runtime_rom.hpp>
#include <hackynavi/bootstrap_quiesce.hpp>
#include <hackynavi/cold_aux.hpp>
#include "HackyNaviFunctionGroup.hpp"

class IOPCIDevice;
class IOMemoryMap;
class OSData;
class HackyNaviLifecycleController;
class HackyNaviClientSession;
class IOUserClient;

// Experimental, boot-argument-gated PCI owner. This is separate from the
// passive IOResources observer. No user client is exposed. The additional
// -hackynavi-engines opt-in selects real paired-function engine activation.
// The power callback schedules a sleepable acquisition/activation worker
// after the power parent has powered up. The access mutex and generation
// revoke that worker's MMIO before a power-down/removal acknowledgement.
class HackyNaviDevice : public IOService {
    OSDeclareDefaultStructors(HackyNaviDevice)

public:
    bool init(OSDictionary* properties = nullptr) override;
    IOService* probe(IOService* provider, SInt32* score) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    void free() override;
    bool willTerminate(IOService* provider, IOOptionBits options) override;
    bool didTerminate(IOService* provider, IOOptionBits options, bool* defer) override;
    IOReturn setPowerState(unsigned long ordinal, IOService* device) override;
    void systemWillShutdown(IOOptionBits specifier) override;
    IOReturn newUserClient(task_t,void*,UInt32,OSDictionary*,IOUserClient**) override;
    IOReturn createClientSession(HackyNaviClientSession*&,IOService* accelerator=nullptr,
        uint64_t acceleratorGeneration=0);
    using IOService::callPlatformFunction;
    IOReturn callPlatformFunction(const OSSymbol*, bool, void*, void*, void*, void*) override;

private:
    IOReturn publishAcceleratorReadiness(HackyNaviFunctionGroup::Operation&);
    IOReturn publishAccelerator(IOService*, uint64_t&);
    IOReturn newAcceleratorUserClient(IOService*,task_t,void*,UInt32,OSDictionary*,
        IOUserClient**,uint64_t&);
    IOReturn createNativeUserClient(task_t,void*,UInt32,OSDictionary*,IOService*,
        uint64_t,IOUserClient**);
    void retireAccelerator();
    bool mapRegisters();
    void unmapRegistersLocked();
    bool captureDiscovery();
    bool captureMemoryLayout(uint64_t vramBytes);
    bool captureRuntimeRom(const uint8_t* discovery, size_t bytes);
    bool captureColdState(const uint8_t* discovery, size_t bytes);
    bool captureBootDisplay(const uint8_t* discovery, size_t bytes);
    bool captureBootDisplayContext(const uint8_t* discovery, size_t bytes);
    void capturePreEngineEvidence(HackyNaviFunctionGroup::Operation&);
    bool captureEpochValid() const;
    static bool bootDisplayContextAccessValid(void* context);
    void clearSnapshotLocked();
    void captureStageLocked(const char* stage);
    void saveCaptureObservationLocked(uint64_t generation, bool revoked);
    void stopPowerManagement();
    void releaseResources();
    void queueCaptureLocked();
    void advancePowerGenerationLocked();
    bool captureRevoked() const;
    static void captureThread(thread_call_param_t,thread_call_param_t);
    void engineWorker();
    IOReturn runEngineAttempt(HackyNaviFunctionGroup*);
    void retireEngines(HackyNaviFunctionGroup::RetirementReason);
    static void functionGroupChanged(IOService*,HackyNaviFunctionGroup*,
        HackyNaviFunctionGroup::Event,uint8_t);
    static IOReturn retireEngineOwner(IOService*,HackyNaviFunctionGroup::RetirementScope&);
    void publishEngineObservation(HackyNaviLifecycleController*,IOReturn,bool retired);
    // Callbacks require lock_ to be held for the entire acquisition, a live
    // provider opened by this service, and an uncached assigned BAR5 mapping.
    static bool read32(void* context, uint32_t byteOffset, uint32_t& value);
    static bool write32(void* context, uint32_t byteOffset, uint32_t value);
    static void delayUs(void* context, uint32_t microseconds);
    bool accessAllowed(uint32_t byteOffset) const;

    IOLock* lock_;
    thread_call_t captureCall_;
    bool capturePending_;
    bool captureSelfHeld_;
    bool engineMode_,groupCreationAttempted_,engineAttempted_;
    uint64_t engineNotificationSerial_;
    HackyNaviFunctionGroup* functionGroup_;
    HackyNaviLifecycleController* lifecycle_;
    const HackyNaviFunctionGroup::Operation* engineCaptureScope_; // Lexical, never diagnostic authority.
    IOService* accelerator_; // Owned late child reference; no SystemKC hard link.
    bool acceleratorReady_; // Private verified-engine state; registry key is only a match hint.
    uint64_t acceleratorGeneration_; // Current child publication, never accepted from properties.
    // Atomic publication lets a power-off/terminal callback interrupt bounded
    // acquisition before waiting for lock_. Only a serialized power-on clears
    // the power bit; a terminal request is permanent for this service instance.
    uint32_t captureRevocations_;
    uint64_t powerGeneration_;
    const char* captureStage_; // Static diagnostic label, protected by lock_.
    IOPCIDevice* pci_;
    IOMemoryMap* registerMap_;
    volatile uint32_t* registers_;
    IOByteCount registerBytes_;
    bool opened_;
    bool stopping_;
    bool registryReady_;
    bool pmInitialized_;
    bool powerAvailable_;
    bool snapshotValid_;
    bool acquisitionActive_;
    bool memoryCaptureActive_;
    bool displayCaptureActive_;
    bool displayContextCaptureActive_;
    uint64_t displayContextGeneration_;
    const char* displayContextAccessFailure_; // Static diagnostic label; no hardware authority.
    bool romCaptureActive_;
    bool postedCaptureActive_;
    bool auxCaptureActive_;
    uint32_t postedRegisterOffsets_[2];
    uint32_t memoryRegisterOffsets_[3];
    hackynavi::Navi31IpLayout ipLayout_;
    hackynavi::MemoryLayout memoryLayout_;
    // Keep the validated acquisition and its actual containing region for
    // subsequent controller construction/reservation accounting. Firmware may
    // still change later; this snapshot is not a power/ownership lease.
    uint8_t* discoveryBytes_;
    hackynavi::DeviceDiscoveryResult discoveryCapture_;
    hackynavi::HdpLayout hdpLayout_;
    hackynavi::bootdisplay::Layout displayLayout_;
    hackynavi::bootdisplay::Capture displayCapture_;
    hackynavi::bootdisplaycontext::Layout displayContextLayout_;
    hackynavi::bootdisplaycontext::Capture displayContextCapture_;
    // Selected runtime legacy image belongs to this power generation. It is
    // copied into OSData only after PCI/subsystem binding and bounded parsing.
    OSData* runtimeRom_;
    hackynavi::runtimerom::Layout romLayout_;
    hackynavi::runtimerom::Result romCapture_;
    hackynavi::FirmwareInfo firmwareInfo_;
    hackynavi::VramUsageInfo vramUsage_;
    hackynavi::Status firmwareInfoStatus_, vramUsageStatus_;
    hackynavi::bootstrap::Result postedCapture_;
    hackynavi::coldaux::Layout auxLayout_;
    hackynavi::coldaux::Result auxCapture_;
};

#endif
