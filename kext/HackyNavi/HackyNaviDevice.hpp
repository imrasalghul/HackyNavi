// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DEVICE_HPP
#define HACKYNAVI_DEVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <stdint.h>
#include <hackynavi/ip_layout.hpp>
#include <hackynavi/memory_layout.hpp>
#include <hackynavi/device_discovery.hpp>
#include <hackynavi/hdp.hpp>
#include <hackynavi/boot_display.hpp>
#include <hackynavi/runtime_rom.hpp>
#include <hackynavi/bootstrap_quiesce.hpp>
#include <hackynavi/cold_aux.hpp>

class IOPCIDevice;
class IOMemoryMap;
class OSData;

// Experimental, boot-argument-gated PCI owner. This is separate from the
// passive IOResources observer. No user client or asynchronous MMIO entry point
// is exposed. Discovery and memory-layout capture run on the IOKit power-up
// callback, after the power parent has powered up, under the access mutex.
class HackyNaviDevice : public IOService {
    OSDeclareDefaultStructors(HackyNaviDevice)

public:
    bool init(OSDictionary* properties = nullptr) override;
    IOService* probe(IOService* provider, SInt32* score) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long ordinal, IOService* device) override;
    void systemWillShutdown(IOOptionBits specifier) override;

private:
    bool mapRegisters();
    bool captureDiscovery();
    bool captureMemoryLayout(uint64_t vramBytes);
    bool captureRuntimeRom(const uint8_t* discovery, size_t bytes);
    bool captureColdState(const uint8_t* discovery, size_t bytes);
    bool captureBootDisplay(const uint8_t* discovery, size_t bytes);
    void clearSnapshotLocked();
    void stopPowerManagement();
    void releaseResources();
    // Callbacks require lock_ to be held for the entire acquisition, a live
    // provider opened by this service, and an uncached assigned BAR5 mapping.
    static bool read32(void* context, uint32_t byteOffset, uint32_t& value);
    static bool write32(void* context, uint32_t byteOffset, uint32_t value);
    static void delayUs(void* context, uint32_t microseconds);
    bool accessAllowed(uint32_t byteOffset) const;

    IOLock* lock_;
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
