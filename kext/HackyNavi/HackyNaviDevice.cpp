// SPDX-License-Identifier: MIT
// BAR5 mapping and register-access pattern adapted from Cobalt64:
// https://github.com/Almosst-DEV/Cobalt64/blob/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65/src/navi48-bringup/src/Navi48Bringup.cpp#L121-L150
// Copyright (c) 2026 Navi48-MacOS contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "HackyNaviDevice.hpp"
#include "HackyNaviPciAccess.hpp"
#include "HackyNaviReadCapture.hpp"
#include <hackynavi/boot_display_capture.hpp>
#include "HackyNaviLifecycleController.hpp"
#include <hackynavi/device_discovery.hpp>
#include <hackynavi/discovery.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>

#if !defined(__x86_64__)
#error HackyNaviDevice currently supports Intel x86_64 only
#endif

OSDefineMetaClassAndStructors(HackyNaviDevice, IOService)

namespace {
constexpr UInt32 kIdentity = 0x744c1002U;
constexpr unsigned kInventoryLimit = 256;
// Conservative mapping bound for this experiment, not a probed BAR size.
constexpr IOByteCount kRegisterMapLimit = 16U * 1024U * 1024U;
constexpr IOByteCount kRequiredRegisterBytes = 0x16061U * 4U + 4U;
constexpr uint32_t kCapturePowerRevoked = 1U;
constexpr uint32_t kCaptureTerminalRevoked = 2U;
// PCI power is provided by the parent. Our off state gates all MMIO before
// acknowledging the transition; our on state requires an already-powered
// parent. This currently owns no submitted GPU work or hardware GART roots.
IOPMPowerState kPowerStates[2] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn,
     0, 0, 0, 0, 0, 0, 0, 0}
};

bool enabled() {
    int present = 0;
    return PE_parse_boot_argn("-hackynavi-device", &present, sizeof(present));
}

bool number(OSDictionary* dictionary, const char* key, UInt64 value,
            unsigned bits = 32) {
    OSNumber* object = OSNumber::withNumber(value, bits);
    if (!object) return false;
    const bool result = dictionary->setObject(key, object);
    object->release();
    return result;
}

bool string(OSDictionary* dictionary, const char* key, const char* value) {
    OSString* object = OSString::withCString(value);
    if (!object) return false;
    const bool result = dictionary->setObject(key, object);
    object->release();
    return result;
}

// Read configuration only. No power transition, decode enable, or bus-master
// enable is attempted. Revision 0xff and all-ones fields are unavailable.
bool validateConfiguration(IOPCIDevice* device, UInt32& revisionClass,
                           UInt16& command, UInt8& header, bool requirePowered = true) {
    if (!device || device->isInactive() || device->configRead32(0x00) != kIdentity)
        return false;
    revisionClass = device->configRead32(0x08);
    header = device->configRead8(0x0e);
    command = device->configRead16(kIOPCIConfigCommand);
    if (revisionClass == 0xffffffffU || (revisionClass & 0xffU) == 0xffU ||
        (revisionClass >> 24) != 0x03 || (header & 0x7fU) != 0 ||
        command == 0xffffU || (requirePowered && !(command & 0x2U))) return false;

    // Matching/start can occur before the PM parent has supplied D0. They may
    // identify the function but must not depend on MMIO decode being enabled.
    if (!requirePowered) return device->configRead32(0x00) == kIdentity;

    // This exact profile requires a readable PM capability already in D0.
    // Use a bounded read-only walk; a convenience-helper failure cannot mean
    // that the capability is absent. This remains an observation, not a lease.
    UInt8 pmOffset = 0;
    UInt16 pmHeader = 0;
    if (!hackynavi::nativepci::readCapability(device,1,pmOffset,pmHeader)||pmOffset>0xf8)
        return false;
    const UInt16 power = device->configRead16(pmOffset + 4);
    if (power == 0xffffU || (power & 3U) != 0 ||device->configRead16(pmOffset)!=pmHeader) return false;
    return device->configRead32(0x00) == kIdentity;
}

struct Inventory {
    OSArray* entries;
    bool allocationFailed;
    bool truncated;
};

void appendIp(const hackynavi::DiscoveryIp& ip, void* context) {
    auto* inventory = static_cast<Inventory*>(context);
    if (inventory->allocationFailed) return;
    if (inventory->entries->getCount() >= kInventoryLimit) {
        inventory->truncated = true;
        return;
    }
    OSDictionary* item = OSDictionary::withCapacity(12);
    OSArray* bases = OSArray::withCapacity(ip.baseAddressCount);
    if (!item || !bases) {
        if (item) item->release();
        if (bases) bases->release();
        inventory->allocationFailed = true;
        return;
    }
    bool valid = number(item, "DieID", ip.dieId) &&
        number(item, "HardwareID", ip.hardwareId) &&
        number(item, "Instance", ip.instance) &&
        number(item, "Major", ip.major) && number(item, "Minor", ip.minor) &&
        number(item, "Revision", ip.revision) &&
        number(item, "Harvest", ip.harvest) &&
        number(item, "SubRevision", ip.subRevision) &&
        number(item, "Variant", ip.variant);
    for (unsigned index = 0; valid && index < ip.baseAddressCount; ++index) {
        OSNumber* address = OSNumber::withNumber(ip.baseAddresses[index], 64);
        valid = address && bases->setObject(address);
        if (address) address->release();
    }
    valid = valid && item->setObject("RawBaseAddresses", bases);
    valid = valid && inventory->entries->setObject(item);
    bases->release();
    item->release();
    if (!valid) inventory->allocationFailed = true;
}
} // namespace

bool HackyNaviDevice::init(OSDictionary* properties) {
    lock_ = nullptr;
    captureCall_ = nullptr;
    capturePending_ = false;
    captureSelfHeld_ = false;
    engineMode_=groupCreationAttempted_=engineAttempted_=false;
    engineNotificationSerial_=0;functionGroup_=nullptr;lifecycle_=nullptr;accelerator_=nullptr;
    acceleratorReady_=false;
    acceleratorGeneration_=0;
    captureRevocations_ = kCapturePowerRevoked;
    powerGeneration_ = 0;
    captureStage_ = "NotStarted";
    engineCaptureScope_ = nullptr;
    pci_ = nullptr;
    registerMap_ = nullptr;
    registers_ = nullptr;
    registerBytes_ = 0;
    opened_ = false;
    stopping_ = true;
    registryReady_ = false;
    pmInitialized_ = false;
    powerAvailable_ = false;
    snapshotValid_ = false;
    acquisitionActive_ = false;
    memoryCaptureActive_ = false;
    displayCaptureActive_ = false;
    displayContextCaptureActive_ = false;
    displayContextGeneration_ = 0;
    displayContextAccessFailure_ = nullptr;
    romCaptureActive_ = postedCaptureActive_ = auxCaptureActive_ = false;
    postedRegisterOffsets_[0] = postedRegisterOffsets_[1] = 0;
    runtimeRom_ = nullptr;
    romLayout_ = hackynavi::runtimerom::Layout{};
    romCapture_ = hackynavi::runtimerom::Result{};
    firmwareInfo_ = hackynavi::FirmwareInfo{};
    vramUsage_ = hackynavi::VramUsageInfo{};
    firmwareInfoStatus_ = vramUsageStatus_ = hackynavi::Status::NotFound;
    postedCapture_ = hackynavi::bootstrap::Result{};
    postedCapture_.status = hackynavi::bootstrap::Status::InvalidArgument;
    auxLayout_ = hackynavi::coldaux::Layout{};
    auxCapture_ = hackynavi::coldaux::Result{};
    for (unsigned i = 0; i < 3; ++i) memoryRegisterOffsets_[i] = 0;
    ipLayout_ = hackynavi::Navi31IpLayout{};
    memoryLayout_ = hackynavi::MemoryLayout{};
    discoveryBytes_ = nullptr;
    discoveryCapture_ = hackynavi::DeviceDiscoveryResult{};
    hdpLayout_ = hackynavi::HdpLayout{};
    displayLayout_ = hackynavi::bootdisplay::Layout{};
    displayCapture_ = hackynavi::bootdisplay::Capture{};
    displayContextLayout_ = hackynavi::bootdisplaycontext::Layout{};
    displayContextCapture_ = hackynavi::bootdisplaycontext::Capture{};
    if (!IOService::init(properties)) return false;
    registryReady_ = true;
    lock_ = IOLockAlloc();
    if (!lock_) return false;
    // Acquisition can sleep and read thousands of words. Do not put it on the
    // default high-priority callout group. ONCE also prevents overlapping
    // invocations and makes cancel_wait drain an in-flight callback even when
    // it cancels a pending requeue (XNU thread_call.c:thread_call_cancel_wait).
    captureCall_ = thread_call_allocate_with_options(captureThread, this,
        THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    return captureCall_ != nullptr;
}

IOService* HackyNaviDevice::probe(IOService* provider, SInt32* score) {
    // Gate before claiming the IOPCIDevice or touching any configuration/MMIO.
    if (!enabled()) return nullptr;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    UInt32 revisionClass = 0;
    UInt16 command = 0;
    UInt8 header = 0;
    if (!validateConfiguration(device, revisionClass, command, header, false))
        return nullptr;
    return IOService::probe(provider, score);
}

bool HackyNaviDevice::mapRegisters() {
    if (!powerAvailable_ || !pmInitialized_ || stopping_ || isInactive() || !opened_ ||
        !pci_ || pci_->isInactive() || registerMap_) return false;
    // BAR5 must be its own assigned 32-bit memory BAR. Never size it by writing
    // config space, and never interpret the upper half of a 64-bit BAR as BAR5.
    const UInt32 bar4 = pci_->configRead32(kIOPCIConfigBaseAddress4);
    const UInt32 bar5 = pci_->configRead32(kIOPCIConfigBaseAddress5);
    if (bar4 == 0xffffffffU || (!(bar4 & 1U) && (bar4 & 6U) == 4U) ||
        bar5 == 0xffffffffU || (bar5 & 7U) != 0 || (bar5 & ~UInt32(15)) == 0)
        return false;
    hackynavi::nativepci::AssignedMemory memory(pci_,kIOPCIConfigBaseAddress5);
    if (!memory || memory->getPhysicalAddress() != (bar5 & ~UInt32(15)) ||
        memory->getLength() < kRequiredRegisterBytes ||
        memory->getLength() > kRegisterMapLimit) return false;
    // The property descriptor is retained by the stack helper while mapping.
    IOMemoryMap* map = memory->map(kIOMapInhibitCache);
    if (!map) return false;
    const IOByteCount bytes = map->getLength();
    const IOVirtualAddress address = map->getVirtualAddress();
    if (!address || (address & 3U) || bytes < kRequiredRegisterBytes ||
        bytes > kRegisterMapLimit || map->getPhysicalAddress() != (bar5 & ~UInt32(15))) {
        map->release();
        return false;
    }
    registerMap_ = map;
    registerBytes_ = bytes;
    registers_ = reinterpret_cast<volatile uint32_t*>(address);
    return true;
}

void HackyNaviDevice::unmapRegistersLocked() {
    // Diagnostic transactions hold lock_. Engine mode must complete paired
    // retirement outside this mutex before reaching teardown/unmap.
    registers_ = nullptr;
    registerBytes_ = 0;
    IOMemoryMap* map = registerMap_;
    registerMap_ = nullptr;
    if (map) map->release();
    if (registryReady_) setProperty("RegisterBytes", UInt64(0), 64);
}

bool HackyNaviDevice::captureEpochValid() const {
    return !captureRevoked() && !stopping_ && !isInactive() && powerAvailable_ &&
        pmInitialized_ && opened_ && pci_ && !pci_->isInactive() &&
        (!engineMode_ || (engineCaptureScope_ && engineCaptureScope_->matchesGraphics(pci_,
            const_cast<HackyNaviDevice*>(this))));
}
bool HackyNaviDevice::accessAllowed(uint32_t offset) const {
    return (acquisitionActive_ || memoryCaptureActive_ || displayCaptureActive_ || displayContextCaptureActive_ ||
        romCaptureActive_ || postedCaptureActive_ || auxCaptureActive_) &&
        captureEpochValid() && registers_ && (offset & 3U) == 0 &&
        registerBytes_ >= 4 && offset <= registerBytes_ - 4;
}

bool HackyNaviDevice::read32(void* context, uint32_t offset, uint32_t& value) {
    auto* self = static_cast<HackyNaviDevice*>(context);
    // Each stage admits only its own explicit read set, never arbitrary MMIO.
    if(self->romCaptureActive_) {
        const auto& l=self->romLayout_;
        if(!l.valid || (offset!=l.romData && offset!=l.romOffsetControl &&
            offset!=l.rsmuIndex && offset!=l.rsmuData && offset!=l.rsmuIndexHigh))return false;
    } else if(self->postedCaptureActive_) {
        if(offset!=self->postedRegisterOffsets_[0] && offset!=self->postedRegisterOffsets_[1])return false;
    } else if(self->auxCaptureActive_) {
        const auto& l=self->auxLayout_;bool allowed=offset==l.sos;
        for(size_t v=0;v<hackynavi::coldaux::kVcnInstances;++v){
            allowed=allowed || offset==l.vcnPower[v] || offset==l.vcnPowerConfig[v];
            for(size_t i=0;i<hackynavi::coldaux::kVcnRegisters;++i)allowed=allowed || offset==l.vcn[v][i];
        }
        for(size_t i=0;i<hackynavi::coldaux::kJpegRegisters;++i)allowed=allowed || offset==l.jpeg[i];
        for(size_t i=0;i<hackynavi::coldaux::kLsdmaRegisters;++i)allowed=allowed || offset==l.lsdma[i];
        if(!l.valid || !allowed)return false;
    } else if (self->displayContextCaptureActive_) {
        const auto& layout = self->displayContextLayout_;
        bool allowed = false;
        for (size_t p = 0; p < hackynavi::bootdisplaycontext::kPipeCount; ++p)
            allowed = allowed || offset == layout.power[p] || offset == layout.powerConfig[p];
        for (size_t i = 0; i < hackynavi::bootdisplaycontext::kRegisterCount; ++i)
            allowed = allowed || offset == layout.registers[i];
        if (!layout.valid || !allowed || !bootDisplayContextAccessValid(self)) return false;
    } else if (self->displayCaptureActive_) {
        // The complete DCN layout was resolved and bounded before this stage.
        // It admits reads only; the selector-write callback is unavailable.
        const auto& layout = self->displayLayout_;
        bool allowed = layout.sos && offset==layout.sos;
        for (size_t i = 0; i < hackynavi::bootdisplay::kVgaCount; ++i)
            allowed = allowed || offset == layout.vga[i];
        for (size_t i = 0; i < hackynavi::bootdisplay::kGlobalRegisterCount; ++i)
            allowed = allowed || offset == layout.global[i];
        for (size_t p = 0; p < hackynavi::bootdisplay::kPipeCount; ++p)
            allowed = allowed || offset == layout.power[p] || offset==layout.powerConfig[p];
        for (size_t p = 0; p < hackynavi::bootdisplay::kPipeCount; ++p)
            for (size_t i = 0; i < hackynavi::bootdisplay::kPipeRegisterCount; ++i)
                allowed = allowed || offset == layout.pipe[p][i];
        if (!layout.valid || !allowed) return false;
    } else if (self->memoryCaptureActive_) {
        if (offset != self->memoryRegisterOffsets_[0] &&
            offset != self->memoryRegisterOffsets_[1] &&
            offset != self->memoryRegisterOffsets_[2]) return false;
    } else if (self->acquisitionActive_) {
        switch (offset) {
        case 0x16061U * 4U: // MP0 firmware-ready
        case 0xde3U * 4U:   // Configured VRAM size
        case 0x94U * 4U: case 0x95U * 4U: case 0x96U * 4U: // Driver scratch
        case 0x1U * 4U:    // MM_DATA read only
            break;
        default: return false;
        }
    } else return false;
    if (!self->accessAllowed(offset)) return false;
    // Intel uncached MMIO preserves device access ordering. Compiler barriers
    // also keep ordinary memory accesses outside each volatile transaction.
    __asm__ __volatile__("" ::: "memory");
    value = self->registers_[offset / 4];
    OSSynchronizeIO();
    __asm__ __volatile__("" ::: "memory");
    return self->captureEpochValid();
}

bool HackyNaviDevice::write32(void* context, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviDevice*>(context);
    // Selector writes only. ROM_DATA, RSMU_DATA and MM_DATA are never writable.
    bool allowed=false;
    if(self->romCaptureActive_ && self->romLayout_.valid) {
        const auto& l=self->romLayout_;
        allowed=(offset==l.rsmuIndex && value==l.romOffsetControl) ||
            (offset==l.romIndex && !(value&0x1ffff) && value<=(0x7fU<<17));
    } else if(self->acquisitionActive_)allowed=offset==0 || offset==0x18;
    if(!allowed || !self->accessAllowed(offset))return false;
    __asm__ __volatile__("" ::: "memory");
    self->registers_[offset / 4] = value;
    OSSynchronizeIO();
    __asm__ __volatile__("" ::: "memory");
    return self->captureEpochValid();
}

void HackyNaviDevice::delayUs(void*, uint32_t microseconds) {
    // The enclosing IOLock is a mutex, not a spinlock. Yield for the 1ms polls.
    if (microseconds >= 1000) IOSleep((microseconds + 999) / 1000);
    else IODelay(microseconds);
}

bool HackyNaviDevice::captureDiscovery() {
    captureStageLocked("DiscoveryAcquisition");
    if (discoveryBytes_) return false;
    auto* buffer = static_cast<uint8_t*>(IOMalloc(hackynavi::kDeviceDiscoveryMaxBytes));
    if (!buffer) return false;
    const hackynavi::DeviceDiscoveryIo io = {this, read32, write32, delayUs};
    const hackynavi::DeviceDiscoveryOptions options = {2000, 1000};
    hackynavi::DeviceDiscoveryResult capture = {};
    acquisitionActive_ = true;
    const auto acquired = hackynavi::acquireDeviceDiscovery(io, options, buffer,
        hackynavi::kDeviceDiscoveryMaxBytes, capture);
    acquisitionActive_ = false;
    // Keep transport failure details independently of the validated snapshot.
    // No hardware addresses in this historical record confer access authority.
    OSDictionary* acquisition = OSDictionary::withCapacity(7);
    if (acquisition) {
        const bool recorded = string(acquisition, "Status", hackynavi::deviceDiscoveryStatusString(acquired)) &&
            number(acquisition, "BytesCopied", capture.bytesCopied, 64) &&
            number(acquisition, "ReadyPolls", capture.readyPolls) &&
            number(acquisition, "SourceVRAMOffset", capture.sourceOffset, 64) &&
            acquisition->setObject("SelectorWritesAttempted", capture.selectorWritesAttempted ? kOSBooleanTrue : kOSBooleanFalse) &&
            setProperty("DiscoveryAcquisition", acquisition);
        if (!recorded) IOLog("HackyNaviDevice: acquisition diagnostic publication failed\n");
        acquisition->release();
    }
    if (acquired != hackynavi::DeviceDiscoveryStatus::Ok) {
        IOLog("HackyNaviDevice: acquisition failed: %s; bytes=%llu; selector writes attempted=%u\n",
              hackynavi::deviceDiscoveryStatusString(acquired),
              static_cast<unsigned long long>(capture.bytesCopied),
              unsigned(capture.selectorWritesAttempted));
        IOFree(buffer, hackynavi::kDeviceDiscoveryMaxBytes);
        return false;
    }
    captureStageLocked("DiscoveryParsingAndLayout");
    OSArray* entries = OSArray::withCapacity(64);
    OSDictionary* report = OSDictionary::withCapacity(24);
    if (!entries || !report) {
        if (entries) entries->release();
        if (report) report->release();
        IOFree(buffer, hackynavi::kDeviceDiscoveryMaxBytes);
        return false;
    }
    Inventory inventory = {entries, false, false};
    hackynavi::DiscoveryInfo discovery = {};
    const auto parsed = hackynavi::parseDiscovery(buffer, capture.binarySize,
        discovery, appendIp, &inventory);
    hackynavi::IpLayoutDiagnostic diagnostic = {};
    const auto layoutStatus = hackynavi::buildNavi31IpLayout(buffer,
        capture.binarySize, ipLayout_, diagnostic);
    const auto hdpStatus = hackynavi::buildHdpLayout(buffer, capture.binarySize,
        registerBytes_, hdpLayout_);
    bool valid = parsed == hackynavi::DiscoveryStatus::Ok && !inventory.allocationFailed &&
        layoutStatus == hackynavi::IpLayoutStatus::Ok;
    if (!valid) {
        IOLog("HackyNaviDevice: discovery parse/publication failed: %s; allocation failure=%u\n",
              hackynavi::discoveryStatusString(parsed), unsigned(inventory.allocationFailed));
        IOLog("HackyNaviDevice: Navi31 register layout: %s; block=%s die=%u instance=%u\n",
              hackynavi::ipLayoutStatusString(layoutStatus),
              hackynavi::navi31BlockString(diagnostic.block),
              unsigned(diagnostic.dieId), unsigned(diagnostic.instance));
    } else {
        valid = string(report, "Status", inventory.truncated ? "ValidatedInventoryTruncated" : "Validated") &&
            string(report, "AcquisitionStatus", hackynavi::deviceDiscoveryStatusString(acquired)) &&
            string(report, "RegisterLayoutStatus", hackynavi::ipLayoutStatusString(layoutStatus)) &&
            string(report, "HDPLayoutStatus", hackynavi::hdpStatusString(hdpStatus)) &&
            string(report, "BaseAddressEncoding", "Raw firmware values; not CPU physical addresses") &&
            string(report, "Source", capture.usedScratch ? "DriverScratchVRAM" : "VRAMEndMinus64KiB") &&
            number(report, "SourceVRAMOffset", capture.sourceOffset, 64) &&
            number(report, "VRAMBytes", capture.vramBytes, 64) &&
            number(report, "RegionBytes", capture.regionSize) &&
            number(report, "BinaryBytes", capture.binarySize) &&
            number(report, "ReadyPolls", capture.readyPolls) &&
            number(report, "BinaryMajor", discovery.binaryMajor) &&
            number(report, "BinaryMinor", discovery.binaryMinor) &&
            number(report, "IPTableVersion", discovery.ipVersion) &&
            number(report, "DieCount", discovery.dieCount) &&
            number(report, "IPCount", discovery.ipCount) &&
            number(report, "PublishedIPCount", entries->getCount()) &&
            number(report, "InventoryLimit", kInventoryLimit) &&
            report->setObject("BaseAddresses64Bit", discovery.baseAddresses64Bit ? kOSBooleanTrue : kOSBooleanFalse) &&
            report->setObject("SelectorWritesAttempted", capture.selectorWritesAttempted ? kOSBooleanTrue : kOSBooleanFalse) &&
            report->setObject("SelectorsRestored", kOSBooleanFalse) &&
            report->setObject("IPInventory", entries) &&
            setProperty("Discovery", report);
    }
    // Neither collection is changed after IORegistry publication.
    entries->release();
    report->release();
    // Keep independent evidence after a semantic/publishing rejection. Every
    // next domain still requires its own inputs and the current capture gate;
    // no read is attempted after actual scope/power revocation.
    if(parsed==hackynavi::DiscoveryStatus::Ok && layoutStatus==hackynavi::IpLayoutStatus::Ok) {
        if(captureEpochValid())valid=captureRuntimeRom(buffer,capture.binarySize)&&valid;
        if(captureEpochValid())valid=captureColdState(buffer,capture.binarySize)&&valid;
        const bool memoryValid=captureEpochValid()&&captureMemoryLayout(capture.vramBytes);
        valid=memoryValid&&valid;
        if(memoryValid&&captureEpochValid())valid=captureBootDisplay(buffer,capture.binarySize)&&valid;
        if(memoryValid&&captureEpochValid())valid=captureBootDisplayContext(buffer,capture.binarySize)&&valid;
    }
    if (valid) {
        discoveryBytes_ = buffer;
        discoveryCapture_ = capture;
    } else {
        IOFree(buffer, hackynavi::kDeviceDiscoveryMaxBytes);
    }
    return valid;
}

bool HackyNaviDevice::captureRuntimeRom(const uint8_t* discovery,size_t bytes) {
    captureStageLocked("RuntimeROM");
    namespace rr=hackynavi::runtimerom;
    if(runtimeRom_)return false;
    romCapture_=rr::Result{};
    auto status=rr::buildLayout(discovery,bytes,registerBytes_,romLayout_);
    const char* binding="NotAcquired";
    auto fwStatus=hackynavi::Status::NotFound,usageStatus=hackynavi::Status::NotFound;
    UInt32 revisionClass=0;UInt16 command=0;UInt8 header=0;
    const UInt32 subsystemBefore=pci_->configRead32(0x2c);
    if(status==rr::Status::Ok) {
        auto* buffer=static_cast<uint8_t*>(IOMalloc(rr::kRecommendedCapacity));
        if(!buffer)return false;
        if(subsystemBefore!=UINT32_MAX && validateConfiguration(pci_,revisionClass,command,header)) {
            romCaptureActive_=true;
            status=rr::acquire(romLayout_,{this,read32,write32},buffer,rr::kRecommendedCapacity,romCapture_);
            romCaptureActive_=false;
            if(status==rr::Status::Ok) {
                const UInt32 subsystemAfter=pci_->configRead32(0x2c);
                const UInt32 atomSubsystem=UInt32(romCapture_.atom.subsystemVendorId)|
                    (UInt32(romCapture_.atom.subsystemDeviceId)<<16);
                if(subsystemAfter==subsystemBefore && subsystemAfter==atomSubsystem &&
                    validateConfiguration(pci_,revisionClass,command,header)) {
                    runtimeRom_=OSData::withBytes(buffer,unsigned(romCapture_.image.size));
                    if(!runtimeRom_){IOFree(buffer,rr::kRecommendedCapacity);return false;}
                    binding="ValidatedPCIAndSubsystem";
                    fwStatus=hackynavi::parseFirmwareInfo(buffer,romCapture_.image.size,romCapture_.image,firmwareInfo_);
                    usageStatus=hackynavi::parseVramUsage(buffer,romCapture_.image.size,romCapture_.image,vramUsage_);
                } else binding="PCIOrSubsystemMismatch";
            }
        } else {status=rr::Status::DeviceUnavailable;binding="PCIUnavailable";}
        IOFree(buffer,rr::kRecommendedCapacity);
    }
    romCapture_.status=status;
    firmwareInfoStatus_=fwStatus;vramUsageStatus_=usageStatus;
    OSDictionary* report=OSDictionary::withCapacity(18);
    if(!report)return false;
    const bool valid=string(report,"Status",rr::statusString(status)) &&
        string(report,"Binding",binding) && string(report,"Source","LiveSMUIOSelectedLegacyROM") &&
        string(report,"FirmwareInfoStatus",hackynavi::statusString(fwStatus)) &&
        string(report,"VRAMUsageStatus",hackynavi::statusString(usageStatus)) &&
        number(report,"BankByteOffset",romCapture_.bankByteOffset) &&
        number(report,"BytesRead",romCapture_.bytesRead,64) &&
        number(report,"BytesRequired",romCapture_.bytesRequired,64) &&
        number(report,"Reads",romCapture_.reads) && number(report,"SelectorWrites",romCapture_.writes) &&
        number(report,"LastRegister",romCapture_.lastRegister) &&
        number(report,"PCISubsystem",subsystemBefore) &&
        number(report,"ATOMSubsystemVendor",romCapture_.atom.subsystemVendorId,16) &&
        number(report,"ATOMSubsystemDevice",romCapture_.atom.subsystemDeviceId,16) &&
        report->setObject("RetainedForPowerGeneration",runtimeRom_?kOSBooleanTrue:kOSBooleanFalse) &&
        report->setObject("FirmwareInfoPresent",firmwareInfo_.present?kOSBooleanTrue:kOSBooleanFalse) &&
        setProperty("RuntimeROM",report);
    report->release();
    IOLog("HackyNaviDevice: runtime ROM %s; binding=%s; bytes=%llu\n",rr::statusString(status),binding,
        static_cast<unsigned long long>(romCapture_.bytesRead));
    // An unsupported/failed ROM remains explicit diagnostic evidence. The
    // independently valid discovery snapshot is still useful; no startup
    // consumer may proceed without runtimeRom_ and its successful decoders.
    return valid;
}

bool HackyNaviDevice::captureColdState(const uint8_t* discovery,size_t bytes) {
    captureStageLocked("PostedAndAuxiliaryState");
    namespace bs=hackynavi::bootstrap;namespace ca=hackynavi::coldaux;
    postedCapture_=bs::Result{};postedCapture_.status=bs::Status::InvalidArgument;
    const bool haveFirmware=runtimeRom_ && firmwareInfoStatus_==hackynavi::Status::Ok && firmwareInfo_.present;
    if(haveFirmware) {
        hackynavi::ResolvedRegister sos={};
        const uint64_t scratch=(uint64_t(firmwareInfo_.biosScratchRegisterDword)+7)*4;
        if(hackynavi::resolveNavi31Register(ipLayout_,hackynavi::Navi31Block::Mp0,0,0x91,
            registerBytes_,sos)==hackynavi::IpLayoutStatus::Ok && scratch<=UINT32_MAX &&
            registerBytes_>=4 && scratch<=registerBytes_-4) {
            postedRegisterOffsets_[0]=sos.mmioByteOffset;postedRegisterOffsets_[1]=uint32_t(scratch);
            postedCaptureActive_=true;
            bs::checkPostedColdState(ipLayout_,registerBytes_,firmwareInfo_.biosScratchRegisterDword,
                {this,read32,nullptr,nullptr},postedCapture_);
            postedCaptureActive_=false;
        }
    }
    OSDictionary* posted=OSDictionary::withCapacity(8);
    if(!posted)return false;
    bool valid=string(posted,"Status",haveFirmware?bs::statusString(postedCapture_.status):"NoValidatedRuntimeFirmwareInfo") &&
        number(posted,"BiosScratch7",postedCapture_.biosScratch7) &&
        number(posted,"SOSSignOfLife",postedCapture_.sosSignOfLife) &&
        number(posted,"Reads",postedCapture_.readAttempts) &&
        number(posted,"LastRegister",postedCapture_.lastOffset) &&
        string(posted,"OwnershipStatus","DiagnosticOnlyExternalExclusionNotEstablished") &&
        setProperty("PostedColdState",posted);
    posted->release();
    auxCapture_=ca::Result{};auxCapture_.instance=0xff;
    ca::PowerInventory inventory={};inventory.failedInstance=0xff;
    for(size_t v=0;v<ca::kVcnInstances;++v)inventory.probes[v].instance=static_cast<uint8_t>(v);
    auto status=ca::buildLayout(discovery,bytes,registerBytes_,auxLayout_);
    const bool postedCold=postedCapture_.status==bs::Status::Ok;
    bool classifierAttempted=false;
    if(postedCold && status==ca::Status::Ok) {
        auxCaptureActive_=true;
        // POWER_STATUS is Linux's first, always-readable dump entry. Do not
        // access PGFSM/client windows unless both instances were coherently
        // readable with TILES_OFF clear. This observation is not a DMA proof.
        if(ca::inventoryVcnPower(auxLayout_,{this,read32},inventory)==ca::PowerInventoryStatus::ReadAccessObserved) {
            classifierAttempted=true;
            status=ca::checkStopped(auxLayout_,{this,read32},auxCapture_);
        } else status=ca::Status::PowerUnavailable;
        auxCaptureActive_=false;
    }
    auxCapture_.status=postedCold?status:ca::Status::InvalidArgument;
    OSDictionary* aux=OSDictionary::withCapacity(12);
    if(!aux)return false;
    OSDictionary* power=OSDictionary::withCapacity(10);
    OSArray* probes=OSArray::withCapacity(ca::kVcnInstances);
    if(!power || !probes){if(power)power->release();if(probes)probes->release();aux->release();return false;}
    bool powerValid=string(power,"Status",ca::statusString(inventory.status)) &&
        number(power,"ReadsAttempted",inventory.reads) && number(power,"LastRegister",inventory.lastRegister) &&
        number(power,"LastValue",inventory.lastValue) && number(power,"FailedInstance",inventory.failedInstance,8) &&
        power->setObject("LastReadSucceeded",inventory.lastReadSucceeded?kOSBooleanTrue:kOSBooleanFalse) &&
        power->setObject("CurrentHardwareAuthority",kOSBooleanFalse);
    for(size_t v=0;powerValid && v<ca::kVcnInstances;++v) {
        const auto& probe=inventory.probes[v];OSDictionary* item=OSDictionary::withCapacity(8);
        if(!item){powerValid=false;break;}
        powerValid=number(item,"Instance",probe.instance,8) && number(item,"Register",probe.reg) &&
            number(item,"Before",probe.raw[0]) && number(item,"After",probe.raw[1]) &&
            number(item,"AttemptedMask",probe.attemptedMask,8) &&
            number(item,"ReadSucceededMask",probe.readSucceededMask,8) &&
            number(item,"ValidMask",probe.validMask,8) && probes->setObject(item);
        item->release();
    }
    powerValid=powerValid && power->setObject("Probes",probes) && aux->setObject("VcnPowerInventory",power);
    probes->release();power->release();
    const char* auxiliaryStatus=!postedCold?"NotCheckedWithoutPostedColdState":
        (!classifierAttempted && inventory.status!=ca::PowerInventoryStatus::NotAttempted?
        "NotCheckedWithoutVcnRegisterAccess":ca::statusString(status));
    valid=valid && powerValid && string(aux,"Status",auxiliaryStatus) &&
        aux->setObject("ClassifierAttempted",classifierAttempted?kOSBooleanTrue:kOSBooleanFalse) &&
        number(aux,"Reads",auxCapture_.reads) && number(aux,"LastRegister",auxCapture_.lastRegister) &&
        number(aux,"LastValue",auxCapture_.lastValue) && number(aux,"FailedInstance",auxCapture_.instance,8) &&
        number(aux,"PoweredOffMask",auxCapture_.poweredOffMask,8) &&
        string(aux,"OwnershipStatus","DiagnosticOnlyExternalExclusionNotEstablished") && setProperty("ColdAuxiliaryEngines",aux);
    aux->release();return valid;
}

bool HackyNaviDevice::captureMemoryLayout(uint64_t vramBytes) {
    captureStageLocked("MemoryLayout");
    // Linux pinned MMHUB3 generated offsets, BASE_IDX=0, in DWORD units.
    // No assumed address from another GPU or ROM is used to access registers.
    const uint32_t relative[3] = {0x08ec, 0x08ed, 0x08d7};
    for (unsigned i = 0; i < 3; ++i) {
        hackynavi::ResolvedRegister resolved = {};
        const auto status = hackynavi::resolveNavi31Register(ipLayout_,
            hackynavi::Navi31Block::Mmhub, 0, relative[i], registerBytes_, resolved);
        if (status != hackynavi::IpLayoutStatus::Ok) {
            IOLog("HackyNaviDevice: MMHUB register %u resolution failed: %s\n",
                  i, hackynavi::ipLayoutStatusString(status));
            return false;
        }
        memoryRegisterOffsets_[i] = resolved.mmioByteOffset;
    }
    // Observe assigned BAR0; do not map it, resize it, or treat its CPU address
    // as a GPU address. Its size only bounds the CPU-visible aperture.
    const UInt32 bar0 = pci_->configRead32(kIOPCIConfigBaseAddress0);
    if (bar0 == UINT32_MAX || (bar0 & 1U) ||
        ((bar0 & 6U) != 0 && (bar0 & 6U) != 4U)) {
        IOLog("HackyNaviDevice: BAR0 is unavailable or has an unsupported type\n");
        return false;
    }
    uint64_t barAddress = bar0 & ~UInt32(15);
    if ((bar0 & 6U) == 4U)
        barAddress |= uint64_t(pci_->configRead32(kIOPCIConfigBaseAddress1)) << 32;
    hackynavi::nativepci::AssignedMemory bar(pci_,kIOPCIConfigBaseAddress0);
    if (!bar || bar->getPhysicalAddress() != barAddress) {
        IOLog("HackyNaviDevice: BAR0 assigned resource does not match PCI configuration\n");
        return false;
    }
    uint32_t values[3] = {};
    bool valid = true;
    memoryCaptureActive_ = true;
    for (unsigned i = 0; valid && i < 3; ++i)
        valid = read32(this, memoryRegisterOffsets_[i], values[i]);
    // Detect an observed transition; equal samples are not an atomic hardware
    // snapshot or a lease against external power/reset actors.
    for (unsigned i = 0; valid && i < 3; ++i) {
        uint32_t again = 0;
        valid = read32(this, memoryRegisterOffsets_[i], again) && again == values[i];
    }
    memoryCaptureActive_ = false;
    if (!valid) {
        IOLog("HackyNaviDevice: MMHUB memory registers unavailable or changed during capture\n");
        return false;
    }
    const hackynavi::MemoryLayoutInput input = {values[0], values[1], values[2],
        vramBytes, barAddress, bar->getLength()};
    const auto status = hackynavi::deriveMemoryLayout(input, memoryLayout_);
    if (status != hackynavi::MemoryLayoutStatus::Ok) {
        IOLog("HackyNaviDevice: memory layout rejected: %s\n",
              hackynavi::memoryLayoutStatusString(status));
        return false;
    }
    OSDictionary* report = OSDictionary::withCapacity(14);
    if (!report) return false;
    valid = string(report, "Status", "ValidatedRegisterSnapshot") &&
        string(report, "AllocationStatus", "ReservationsNotEstablished") &&
        number(report, "FramebufferBaseRaw", values[0]) &&
        number(report, "FramebufferTopRaw", values[1]) &&
        number(report, "FramebufferOffsetRaw", values[2]) &&
        number(report, "MCWindowStart", memoryLayout_.mcWindowStart, 64) &&
        number(report, "MCWindowEndInclusive", memoryLayout_.mcWindowEndInclusive, 64) &&
        number(report, "VRAMStart", memoryLayout_.vramStart, 64) &&
        number(report, "VRAMEndInclusive", memoryLayout_.vramEndInclusive, 64) &&
        number(report, "VRAMBytes", memoryLayout_.vramBytes, 64) &&
        number(report, "VRAMPageBaseOffset", memoryLayout_.vramPageBaseOffset, 64) &&
        number(report, "BAR0PhysicalAddress", memoryLayout_.bar0PhysicalAddress, 64) &&
        number(report, "BAR0Bytes", memoryLayout_.bar0Bytes, 64) &&
        number(report, "CPUVisibleVRAMBytes", memoryLayout_.cpuVisibleVramBytes, 64) &&
        setProperty("MemoryLayout", report);
    report->release();
    return valid;
}

bool HackyNaviDevice::captureBootDisplay(const uint8_t* discovery, size_t bytes) {
    captureStageLocked("BootDisplay");
    namespace bd = hackynavi::bootdisplay;
    HackyNaviReadCapture<bd::ReadCapture> readCapture;
    displayCapture_ = bd::Capture{};
    displayCapture_.failedPipe = 0xff;
    displayCapture_.lastPowerProbe.pipe = 0xff;
    bd::PowerInventory powerInventory = {};
    powerInventory.failedPipe = 0xff;
    for (auto& probe : powerInventory.probes) probe.pipe = 0xff;
    bool classifierAttempted = false;
    auto status = bd::buildLayout(discovery, bytes, registerBytes_, displayLayout_);
    auto inventoryStatus = status;
    if (status == bd::Status::Ok) {
        const bd::Io io = {this, read32};
        displayCaptureActive_ = true;
        // Read the always-on power controls independently of classification.
        // A contrary desired-state bit must not hide CONFIG or other pipes.
        // Stop on transport loss/all-ones; no HUBP access follows that failure.
        inventoryStatus = bd::inventoryPower(displayLayout_, io, powerInventory);
        status = inventoryStatus;
        if (inventoryStatus == bd::Status::Ok) {
            classifierAttempted = true;
            status = bd::captureRecorded(displayLayout_,memoryLayout_,io,displayCapture_,readCapture.get(),
                postedCapture_.status==hackynavi::bootstrap::Status::Ok);
        }
        displayCaptureActive_ = false;
    }
    // This is diagnostic capture. Two equal snapshots do not establish that
    // firmware/another display programmer cannot change the addresses later.
    // Never configure an allocator or a GART from this report alone.
    OSDictionary* report = OSDictionary::withCapacity(15);
    OSDictionary* powerProbe = OSDictionary::withCapacity(5);
    OSDictionary* inventoryReport = OSDictionary::withCapacity(9);
    OSArray* inventoryProbes = OSArray::withCapacity(bd::kPipeCount);
    OSArray* planes = OSArray::withCapacity(bd::kPipeCount);
    if (!report || !powerProbe || !inventoryReport || !inventoryProbes || !planes) {
        if (report) report->release();
        if (powerProbe) powerProbe->release();
        if (inventoryReport) inventoryReport->release();
        if (inventoryProbes) inventoryProbes->release();
        if (planes) planes->release();
        return false;
    }
    // These are the actual successful reads from the last power probe, even
    // when the first status was rejected. The mask distinguishes unavailable
    // fields from a real zero; reporting performs no additional MMIO.
    const auto& probe = displayCapture_.lastPowerProbe;
    bool valid = number(powerProbe, "Pipe", probe.pipe, 8) &&
        number(powerProbe, "ReadsSucceededMask", probe.readsSucceededMask, 8) &&
        number(powerProbe, "StatusBefore", probe.statusBefore) &&
        number(powerProbe, "Config", probe.config) &&
        number(powerProbe, "StatusAfter", probe.statusAfter);
    for (size_t i = 0; valid && i < bd::kPipeCount; ++i) {
        const auto& observed = powerInventory.probes[i];
        OSDictionary* item = OSDictionary::withCapacity(5);
        if (!item) { valid = false; break; }
        valid = number(item, "Pipe", observed.pipe, 8) &&
            number(item, "ReadsSucceededMask", observed.readsSucceededMask, 8) &&
            number(item, "StatusBefore", observed.statusBefore) &&
            number(item, "Config", observed.config) &&
            number(item, "StatusAfter", observed.statusAfter) && inventoryProbes->setObject(item);
        item->release();
    }
    valid = valid && string(inventoryReport, "Status", bd::statusString(inventoryStatus)) &&
        number(inventoryReport, "ReadsAttempted", powerInventory.readsAttempted) &&
        number(inventoryReport, "LastRegister", powerInventory.lastRegister) &&
        number(inventoryReport, "LastValue", powerInventory.lastValue) &&
        number(inventoryReport, "FailedPipe", powerInventory.failedPipe, 8) &&
        inventoryReport->setObject("LastReadSucceeded", powerInventory.lastReadSucceeded ? kOSBooleanTrue : kOSBooleanFalse) &&
        inventoryReport->setObject("CurrentHardwareAuthority", kOSBooleanFalse) &&
        inventoryReport->setObject("Probes", inventoryProbes);
    // Preserve a fully read snapshot even when decoding later rejects a plane
    // format. Unread gated windows are omitted, never represented as zeros.
    // This is CPU-only serialization of the captured observations.
    if (valid && displayCapture_.snapshotReadComplete) {
        const auto words = [](const uint32_t* values, size_t count) -> OSArray* {
            OSArray* out = OSArray::withCapacity(static_cast<unsigned>(count));
            if (!out) return nullptr;
            for (size_t i = 0; i < count; ++i) {
                OSNumber* item = OSNumber::withNumber(values[i], 32);
                const bool ok = item && out->setObject(item);
                if (item) item->release();
                if (!ok) { out->release(); return nullptr; }
            }
            return out;
        };
        OSDictionary* raw = OSDictionary::withCapacity(6);
        OSArray* pipes = OSArray::withCapacity(bd::kPipeCount);
        OSArray* globalOffsets = words(displayLayout_.global, bd::kGlobalRegisterCount);
        OSArray* globalValues = words(displayCapture_.snapshot.global, bd::kGlobalRegisterCount);
        OSArray* vgaOffsets = words(displayLayout_.vga, bd::kVgaCount);
        OSArray* vgaValues = words(displayCapture_.snapshot.vga, bd::kVgaCount);
        valid = raw && pipes && globalOffsets && globalValues && vgaOffsets && vgaValues;
        for (size_t p = 0; valid && p < bd::kPipeCount; ++p) {
            const bool off = displayCapture_.snapshot.power[p] == 0x80000000 ||
                displayCapture_.snapshot.power[p] == 0x90000000;
            OSDictionary* item = OSDictionary::withCapacity(6);
            OSArray* offsets = off ? nullptr : words(displayLayout_.pipe[p], bd::kPipeRegisterCount);
            OSArray* values = off ? nullptr : words(displayCapture_.snapshot.pipe[p], bd::kPipeRegisterCount);
            valid = item && number(item, "Pipe", p, 8) &&
                number(item, "PowerStatus", displayCapture_.snapshot.power[p]) &&
                number(item, "PowerConfig", displayCapture_.snapshot.powerConfig[p]) &&
                item->setObject("WindowRead", off ? kOSBooleanFalse : kOSBooleanTrue) &&
                (off || (offsets && values && item->setObject("RegisterOffsets", offsets) &&
                    item->setObject("RegisterValues", values))) && pipes->setObject(item);
            if (offsets) offsets->release();
            if (values) values->release();
            if (item) item->release();
        }
        valid = valid && raw->setObject("GlobalRegisterOffsets", globalOffsets) &&
            raw->setObject("GlobalRegisterValues", globalValues) &&
            raw->setObject("VgaRegisterOffsets", vgaOffsets) && raw->setObject("VgaRegisterValues", vgaValues) &&
            raw->setObject("CurrentHardwareAuthority", kOSBooleanFalse) && raw->setObject("Pipes", pipes) &&
            report->setObject("RawSnapshot", raw);
        if (raw) raw->release();
        if (pipes) pipes->release();
        if (globalOffsets) globalOffsets->release();
        if (globalValues) globalValues->release();
        if (vgaOffsets) vgaOffsets->release();
        if (vgaValues) vgaValues->release();
    }
    for (size_t i = 0; valid && i < displayCapture_.planeCount; ++i) {
        const auto& plane = displayCapture_.planes[i];
        OSDictionary* item = OSDictionary::withCapacity(11);
        if (!item) { valid = false; break; }
        valid = number(item, "Pipe", plane.pipe) &&
            number(item, "BytesPerPixel", plane.bytesPerPixel) &&
            number(item, "PitchPixels", plane.pitchPixels) &&
            number(item, "ViewportX", plane.x) && number(item, "ViewportY", plane.y) &&
            number(item, "Width", plane.width) && number(item, "Height", plane.height) &&
            number(item, "SurfaceMCAddress", plane.surfaceMcAddress, 64) &&
            number(item, "FootprintBytes", plane.footprintBytes, 64) &&
            number(item, "VRAMOffset", plane.reservation.offset, 64) &&
            number(item, "RoundedBytes", plane.reservation.bytes, 64) &&
            planes->setObject(item);
        item->release();
    }
    const bool rawPublished=readCapture.publish(report,"RawReadJournal");
    if(!rawPublished)report->setObject("ReadJournalPublicationIncomplete",kOSBooleanTrue);
    valid = valid && string(report, "CaptureStatus", bd::statusString(status)) &&
        report->setObject("CompleteClassification", displayCapture_.complete ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("ReservationUseAllowed", kOSBooleanFalse) &&
        number(report, "PlaneCount", displayCapture_.planeCount) &&
        number(report, "PoweredOffMask", displayCapture_.poweredOffMask,8) &&
        number(report, "ReadsAttempted", displayCapture_.readsAttempted) &&
        number(report, "LastRegister", displayCapture_.lastRegister) &&
        number(report, "LastValue", displayCapture_.lastValue) &&
        report->setObject("LastReadSucceeded", displayCapture_.lastReadSucceeded ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("LastPowerProbe", powerProbe) &&
        report->setObject("PowerInventory", inventoryReport) &&
        report->setObject("ClassifierAttempted", classifierAttempted ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("SnapshotReadComplete", displayCapture_.snapshotReadComplete ? kOSBooleanTrue : kOSBooleanFalse) &&
        number(report, "FailedPipe", displayCapture_.failedPipe) &&
        string(report, "Scope", "Observed scanout footprint; ownership and complete display allocation inventory unestablished") &&
        report->setObject("Planes", planes) && setProperty("BootDisplay", report);
    planes->release();
    powerProbe->release();
    inventoryProbes->release();
    inventoryReport->release();
    report->release();
    // Unsupported scanout is reported explicitly; it does not invalidate the
    // independently captured discovery/memory snapshot or enable allocation.
    return valid;
}

bool HackyNaviDevice::captureBootDisplayContext(const uint8_t* discovery, size_t bytes) {
    captureStageLocked("BootDisplayContext");
    namespace dc = hackynavi::bootdisplaycontext;
    displayContextCapture_ = dc::Capture{};
    auto& captured = displayContextCapture_;
    captured.failedPass = captured.failedPipe = captured.failedRegisterIndex = 0xff;
    captured.lastPowerProbe.pipe = 0xff;
    displayContextGeneration_ = powerGeneration_;
    displayContextAccessFailure_ = nullptr;
    auto status = dc::buildLayout(discovery, bytes, registerBytes_, displayContextLayout_);
    const bool prerequisite = displayLayout_.valid && displayCapture_.snapshotReadComplete;
    bool attempted = false;
    if (status == dc::Status::Ok && prerequisite && !captureRevoked() &&
        !stopping_ && powerAvailable_ && pmInitialized_ && opened_ && !isInactive()) {
        displayContextCaptureActive_ = true;
        // Keep the callback state distinct from the captured/layout objects.
        // Only this access stage admits the resolved additional read set.
        struct Access { HackyNaviDevice* device; } access = {this};
        const dc::Io io = {&access,
            [](void* p) -> bool {
                return bootDisplayContextAccessValid(static_cast<Access*>(p)->device);
            },
            [](void* p, uint32_t offset, uint32_t& value) -> bool {
                return read32(static_cast<Access*>(p)->device, offset, value);
            }};
        attempted = true;
        status = dc::capture(displayContextLayout_, io, captured);
        displayContextCaptureActive_ = false;
    }
    // This is a source generation for historical observations, not a usable
    // FunctionGroup operation or a lease on firmware display state.
    const bool generationCurrent = attempted && !displayContextAccessFailure_ &&
        !captureRevoked() && displayContextGeneration_ == powerGeneration_ &&
        powerAvailable_ && pmInitialized_ && !stopping_ && !isInactive();
    const auto words = [](const uint32_t* values, size_t count) -> OSArray* {
        OSArray* out = OSArray::withCapacity(static_cast<unsigned>(count));
        if (!out) return nullptr;
        for (size_t i = 0; i < count; ++i) {
            OSNumber* item = OSNumber::withNumber(values[i], 32);
            const bool ok = item && out->setObject(item);
            if (item) item->release();
            if (!ok) { out->release(); return nullptr; }
        }
        return out;
    };
    OSDictionary* report = OSDictionary::withCapacity(27);
    OSDictionary* probe = OSDictionary::withCapacity(5);
    OSArray* passes = OSArray::withCapacity(dc::kPasses);
    OSArray* offsets = displayContextLayout_.valid ?
        words(displayContextLayout_.registers, dc::kRegisterCount) : nullptr;
    OSArray* powerOffsets = displayContextLayout_.valid ?
        words(displayContextLayout_.power, dc::kPipeCount) : nullptr;
    OSArray* configOffsets = displayContextLayout_.valid ?
        words(displayContextLayout_.powerConfig, dc::kPipeCount) : nullptr;
    bool valid = report && probe && passes &&
        (!displayContextLayout_.valid || (offsets && powerOffsets && configOffsets));
    for (size_t pass = 0; valid && pass < dc::kPasses; ++pass) {
        OSDictionary* item = OSDictionary::withCapacity(8);
        OSArray* values = words(captured.values[pass], dc::kRegisterCount);
        OSArray* power = words(captured.powerStatus[pass], dc::kPipeCount);
        OSArray* config = words(captured.powerConfig[pass], dc::kPipeCount);
        valid = item && values && power && config && number(item, "Pass", pass, 8) &&
            number(item, "AttemptedMask", captured.attemptedMask[pass]) &&
            number(item, "ReadSucceededMask", captured.readSucceededMask[pass]) &&
            number(item, "ValidMask", captured.validMask[pass]) &&
            number(item, "PowerValidMask", captured.powerValidMask[pass], 8) &&
            item->setObject("RegisterValues", values) &&
            item->setObject("PowerStatus", power) && item->setObject("PowerConfig", config) &&
            passes->setObject(item);
        if (values) values->release();
        if (power) power->release();
        if (config) config->release();
        if (item) item->release();
    }
    const auto& lastProbe = captured.lastPowerProbe;
    valid = valid && number(probe, "Pipe", lastProbe.pipe, 8) &&
        number(probe, "ReadsSucceededMask", lastProbe.readsSucceededMask, 8) &&
        number(probe, "StatusBefore", lastProbe.statusBefore) &&
        number(probe, "Config", lastProbe.config) && number(probe, "StatusAfter", lastProbe.statusAfter);
    const char* captureStatus = attempted ? dc::statusString(status) :
        status != dc::Status::Ok ? dc::statusString(status) :
        !prerequisite ? "NotAttemptedWithoutCompleteBootDisplaySnapshot" : "NotAttemptedWithoutPowerGeneration";
    valid = valid && string(report, "CaptureStatus", captureStatus) &&
        string(report, "NativeAccessFailure", displayContextAccessFailure_ ? displayContextAccessFailure_ : "NoneObserved") &&
        number(report, "PowerGeneration", displayContextGeneration_, 64) &&
        report->setObject("CaptureAttempted", attempted ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("PrerequisiteSnapshotComplete", prerequisite ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("NativeGenerationStillCurrent", generationCurrent ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("HistoricalEvidenceOnly", kOSBooleanTrue) &&
        report->setObject("CurrentHardwareAuthority", kOSBooleanFalse) &&
        report->setObject("ReservationUseAllowed", kOSBooleanFalse) &&
        report->setObject("EngineReadinessEstablished", kOSBooleanFalse) &&
        report->setObject("PhysicalAddressModeEstablished", kOSBooleanFalse) &&
        report->setObject("ReadsComplete", captured.readsComplete ? kOSBooleanTrue : kOSBooleanFalse) &&
        number(report, "CompletedPasses", captured.completedPasses, 8) &&
        number(report, "ChangedMask", captured.changedMask) &&
        number(report, "PowerChangedMask", captured.powerChangedMask, 8) &&
        number(report, "FailedPass", captured.failedPass, 8) &&
        number(report, "FailedPipe", captured.failedPipe, 8) &&
        number(report, "FailedRegisterIndex", captured.failedRegisterIndex, 8) &&
        number(report, "ReadsAttempted", captured.readsAttempted) &&
        number(report, "LastRegister", captured.lastRegister) &&
        number(report, "LastValue", captured.lastValue) &&
        report->setObject("LastReadSucceeded", captured.lastReadSucceeded ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("LastPowerProbe", probe) && report->setObject("Passes", passes) &&
        (!displayContextLayout_.valid || (report->setObject("RegisterOffsets", offsets) &&
            report->setObject("PowerStatusOffsets", powerOffsets) && report->setObject("PowerConfigOffsets", configOffsets))) &&
        string(report, "Scope", "Raw timing, HUBP ancillary and VM context words; unread values require masks; no ownership or address-domain inference") &&
        setProperty("BootDisplayContext", report);
    if (offsets) offsets->release();
    if (powerOffsets) powerOffsets->release();
    if (configOffsets) configOffsets->release();
    if (passes) passes->release();
    if (probe) probe->release();
    if (report) report->release();
    return valid;
}

bool HackyNaviDevice::bootDisplayContextAccessValid(void* context) {
    auto* self = static_cast<HackyNaviDevice*>(context);
    if (!self || self->displayContextAccessFailure_) return false;
    // The diagnostic worker holds lock_ throughout capture. Native power-off
    // and termination publish revocation before waiting on that mutex. This
    // excludes this driver's teardown; it is not a firmware/display lease.
    if (!self->displayContextCaptureActive_ || !self->displayContextGeneration_ ||
        self->displayContextGeneration_ != self->powerGeneration_ ||
        !self->displayContextLayout_.valid || !self->displayLayout_.valid ||
        !self->displayCapture_.snapshotReadComplete || !self->accessAllowed(0)) {
        self->displayContextAccessFailure_ = "PowerOrGenerationUnavailable";
        return false;
    }
    UInt32 revisionClass = 0;
    UInt16 command = 0;
    UInt8 header = 0;
    if (!self->pci_->isOpen(self) ||
        !validateConfiguration(self->pci_, revisionClass, command, header)) {
        self->displayContextAccessFailure_ = "PCIConfigurationUnavailable";
        return false;
    }
    if (self->captureRevoked()) {
        self->displayContextAccessFailure_ = "PowerOrGenerationUnavailable";
        return false;
    }
    const UInt32 bar4 = self->pci_->configRead32(kIOPCIConfigBaseAddress4);
    const UInt32 bar5 = self->pci_->configRead32(kIOPCIConfigBaseAddress5);
    auto* map = self->registerMap_;
    if (bar4 == UINT32_MAX || (!(bar4 & 1U) && (bar4 & 6U) == 4U) ||
        bar5 == UINT32_MAX || (bar5 & 7U) || !(bar5 & ~UInt32(15)) || !map ||
        map->getPhysicalAddress() != (bar5 & ~UInt32(15)) ||
        map->getLength() != self->registerBytes_ ||
        self->displayContextLayout_.barBytes != self->registerBytes_ ||
        reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress()) != self->registers_ ||
        (map->getMapOptions() & kIOMapCacheMask) != kIOMapInhibitCache) {
        self->displayContextAccessFailure_ = "BARMappingUnavailable";
        return false;
    }
    if (!self->accessAllowed(0) || self->displayContextGeneration_ != self->powerGeneration_) {
        self->displayContextAccessFailure_ = "PowerOrGenerationUnavailable";
        return false;
    }
    return true;
}

bool HackyNaviDevice::start(IOService* provider) {
    if (!enabled() || !lock_ || !IOService::start(provider)) return false;
    bool success = false;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (device) device->retain();
    UInt32 revisionClass = 0;
    UInt16 command = 0;
    UInt8 header = 0;
    // open() takes provider arbitration and sends interest notifications. Like
    // close(), it must execute outside the acquisition mutex. Matching keeps
    // the provider's configuration phase active until start() returns; phase-2
    // teardown is deferred by XNU, but phase-1 may already mark us inactive.
    const bool opened = device && !isInactive() &&
        validateConfiguration(device, revisionClass, command, header, false) &&
        device->open(this);
    IOLockLock(lock_);
    pci_ = device;
    opened_ = opened;
    if (!isInactive() && opened_ &&
        validateConfiguration(pci_, revisionClass, command, header, false)) {
            int engineArgument=0;
            engineMode_=PE_parse_boot_argn("-hackynavi-engines",&engineArgument,sizeof(engineArgument));
            stopping_ = false;
            success = setProperty("Mode", engineMode_?"ExperimentalEngineBringup":"ExperimentalDeviceBringup") &&
                    setProperty("RequiredBootArgument", "-hackynavi-device") &&
                    setProperty("EngineBringupRequested",engineMode_?kOSBooleanTrue:kOSBooleanFalse) &&
                    setProperty("EngineStartupVerified",kOSBooleanFalse) &&
                    setProperty("EngineActive",kOSBooleanFalse) &&
                    setProperty("AccelerationAvailable", kOSBooleanFalse) &&
                    setProperty("DisplayInitializationAvailable", kOSBooleanFalse) &&
                    setProperty("PCIIdentity", UInt64(kIdentity), 32) &&
                    setProperty("PCIRevision", UInt64(revisionClass & 0xffU), 8) &&
                    setProperty("PCIClass", UInt64(revisionClass >> 8), 24) &&
                    setProperty("PCICommandAtStart", UInt64(command), 16) &&
                    setProperty("PCIHeaderType", UInt64(header), 8) &&
                    setProperty("RegisterBAR", UInt64(5), 32) &&
                    setProperty("RegisterBytes", UInt64(registerBytes_), 64) &&
                    setProperty("PowerGeneration", UInt64(powerGeneration_), 64) &&
                    setProperty("SnapshotGeneration", UInt64(0), 64) &&
                    setProperty("SnapshotStatus", "WaitingForPower") &&
                    setProperty("SnapshotValid", kOSBooleanFalse);
    }
    IOLockUnlock(lock_);
    if (success) {
        // These calls may arrange or invoke callbacks. Never hold lock_ over
        // PM registration, requests or PMstop(). setPowerState performs all
        // acquisition scheduling after the parent-on notification, not in
        // start(). The thread-call worker performs bounded sleepable MMIO.
        PMinit();
        IOLockLock(lock_);
        pmInitialized_ = true;
        IOLockUnlock(lock_);
        provider->joinPMtree(this);
        success = registerPowerDriver(this, kPowerStates, 2) == IOPMNoErr;
        if (success) success = changePowerStateTo(0) == IOPMNoErr;
        if (success) success = changePowerStateToPriv(1) == IOPMNoErr;
    }
    if (!success) {
        IOLog("HackyNaviDevice: start failed; releasing mapping and PCI open\n");
        releaseResources();
        IOService::stop(provider);
        return false;
    }
    // Capture may already have completed or may still await its initial power
    // callback. SnapshotStatus carries that outcome independently of matching.
    // PM serializes normal power transitions; firmware/SMM and external reset
    // actors are still outside this service's exclusion domain.
    registerService();
    IOLog("HackyNaviDevice: power-managed PCI owner started; capture runs on power-up\n");
    return true;
}

void HackyNaviDevice::clearSnapshotLocked() {
    snapshotValid_ = false;
    acquisitionActive_ = false;
    memoryCaptureActive_ = false;
    displayCaptureActive_ = false;
    displayContextCaptureActive_ = false;
    displayContextGeneration_ = 0;
    displayContextAccessFailure_ = nullptr;
    romCaptureActive_=postedCaptureActive_=auxCaptureActive_=false;
    postedRegisterOffsets_[0]=postedRegisterOffsets_[1]=0;
    if(runtimeRom_){runtimeRom_->release();runtimeRom_=nullptr;}
    romLayout_=hackynavi::runtimerom::Layout{};
    romCapture_=hackynavi::runtimerom::Result{};
    romCapture_.status=hackynavi::runtimerom::Status::InvalidArgument;
    firmwareInfo_=hackynavi::FirmwareInfo{};vramUsage_=hackynavi::VramUsageInfo{};
    firmwareInfoStatus_=vramUsageStatus_=hackynavi::Status::NotFound;
    postedCapture_=hackynavi::bootstrap::Result{};
    postedCapture_.status=hackynavi::bootstrap::Status::InvalidArgument;
    auxLayout_=hackynavi::coldaux::Layout{};auxCapture_=hackynavi::coldaux::Result{};
    auxCapture_.status=hackynavi::coldaux::Status::InvalidArgument;
    ipLayout_ = hackynavi::Navi31IpLayout{};
    memoryLayout_ = hackynavi::MemoryLayout{};
    hdpLayout_ = hackynavi::HdpLayout{};
    displayLayout_ = hackynavi::bootdisplay::Layout{};
    displayCapture_ = hackynavi::bootdisplay::Capture{};
    displayContextLayout_ = hackynavi::bootdisplaycontext::Layout{};
    displayContextCapture_ = hackynavi::bootdisplaycontext::Capture{};
    if (discoveryBytes_) {
        IOFree(discoveryBytes_, hackynavi::kDeviceDiscoveryMaxBytes);
        discoveryBytes_ = nullptr;
    }
    discoveryCapture_ = hackynavi::DeviceDiscoveryResult{};
    for (unsigned i = 0; i < 3; ++i) memoryRegisterOffsets_[i] = 0;
    // Remove the published evidence together with the in-memory snapshot, so
    // callers cannot confuse pre-sleep bases with the new power generation.
    removeProperty("Discovery");
    removeProperty("DiscoveryAcquisition");
    removeProperty("MemoryLayout");
    removeProperty("BootDisplay");
    removeProperty("BootDisplayContext");
    removeProperty("BootConsole");
    removeProperty("RuntimeROM");removeProperty("PostedColdState");removeProperty("ColdAuxiliaryEngines");
    setProperty("SnapshotValid", kOSBooleanFalse);
    setProperty("SnapshotGeneration", UInt64(0), 64);
}

void HackyNaviDevice::captureStageLocked(const char* stage) {
    captureStage_ = stage;
    setProperty("CaptureStage", stage);
    setProperty("CapturePowerGeneration", UInt64(powerGeneration_), 64);
    IOLog("HackyNaviDevice: capture generation %llu stage %s\n",
        static_cast<unsigned long long>(powerGeneration_), stage);
}

void HackyNaviDevice::saveCaptureObservationLocked(uint64_t generation, bool revoked) {
    // Historical evidence survives clearing rejected or revoked live snapshots.
    // Published child dictionaries are immutable; retaining them does not retain
    // a BAR mapping, DMA buffer or usable hardware-generation lease.
    OSDictionary* observation = OSDictionary::withCapacity(13);
    if (!observation) return;
    bool valid = number(observation, "PowerGeneration", generation, 64) &&
        string(observation, "LastStage", captureStage_) &&
        string(observation, "Outcome", snapshotValid_ ? "Validated" : revoked ? "Revoked" : "Failed") &&
        observation->setObject("HistoricalEvidenceOnly", kOSBooleanTrue) &&
        observation->setObject("CurrentHardwareAuthority", kOSBooleanFalse);
    if(engineCaptureScope_)valid=number(observation,"EngineOperationGeneration",engineCaptureScope_->generation(),64)&&valid;
    const char* keys[] = {"DiscoveryAcquisition", "Discovery", "RuntimeROM",
        "PostedColdState", "ColdAuxiliaryEngines", "MemoryLayout", "BootDisplay", "BootDisplayContext", "BootConsole"};
    for (const char* key : keys) {
        OSObject* value = getProperty(key);
        if (value) valid = observation->setObject(key, value) && valid;
    }
    if (!valid || !setProperty("LastCaptureObservation", observation))
        IOLog("HackyNaviDevice: historical capture diagnostic publication failed\n");
    observation->release();
}

bool HackyNaviDevice::captureRevoked() const {
    return __atomic_load_n(&captureRevocations_, __ATOMIC_ACQUIRE) != 0;
}

void HackyNaviDevice::advancePowerGenerationLocked() {
    ++powerGeneration_;
    if (registryReady_) setProperty("PowerGeneration", UInt64(powerGeneration_), 64);
}

void HackyNaviDevice::queueCaptureLocked() {
    if (!captureCall_ || capturePending_ || stopping_ || !pmInitialized_ ||
        captureRevoked() || !powerAvailable_ || !opened_ || isInactive() ||
        (engineMode_&&engineAttempted_)) return;
    capturePending_ = true;
    // Hold the service until terminal cancellation/drain, rather than dropping
    // the last reference from inside this callback and entering free() there.
    if (!captureSelfHeld_) { retain(); captureSelfHeld_ = true; }
    thread_call_enter(captureCall_);
}

void HackyNaviDevice::captureThread(thread_call_param_t context, thread_call_param_t) {
    auto* self = static_cast<HackyNaviDevice*>(context);
    if(self->engineMode_){self->engineWorker();return;}
    IOLockLock(self->lock_);
    // Coalesce a queued old generation into the newest powered generation.
    // Power callbacks and this entire MMIO transaction share lock_, so an
    // off/on sequence cannot expose the previous generation's BAR mapping.
    if (!self->captureRevoked() && self->pmInitialized_ && self->powerAvailable_ && !self->stopping_ &&
        !self->isInactive() && self->opened_) {
        const uint64_t generation = self->powerGeneration_;
        UInt32 revisionClass = 0;
        UInt16 command = 0;
        UInt8 header = 0;
        self->setProperty("SnapshotStatus", "CapturingOnSleepableWorker");
        self->removeProperty("LastCaptureObservation");
        self->clearSnapshotLocked();
        self->unmapRegistersLocked();
        self->captureStageLocked("PCIConfiguration");
        self->snapshotValid_ = validateConfiguration(self->pci_, revisionClass, command, header);
        if (self->snapshotValid_) {
            self->captureStageLocked("BAR5Mapping");
            self->snapshotValid_ = self->mapRegisters();
        }
        if (self->snapshotValid_) self->snapshotValid_ = self->captureDiscovery();
        // A terminal/power-off caller publishes revocation before waiting for
        // lock_. A revocation observed after the final read rejects publication.
        // No older queued invocation owns a generation: the mutex binds this
        // whole acquisition and its result to the current powered generation.
        const bool revoked = self->captureRevoked();
        self->snapshotValid_ = self->snapshotValid_ && !revoked &&
            generation == self->powerGeneration_;
        if (self->snapshotValid_) self->captureStageLocked("Complete");
        self->saveCaptureObservationLocked(generation, revoked);
        if (!self->snapshotValid_) {
            self->clearSnapshotLocked();
            self->powerAvailable_ = false;
            self->unmapRegistersLocked();
        } else {
            self->setProperty("RegisterBytes", UInt64(self->registerBytes_), 64);
        }
        self->setProperty("SnapshotValid", self->snapshotValid_ ? kOSBooleanTrue : kOSBooleanFalse);
        self->setProperty("SnapshotGeneration", UInt64(self->snapshotValid_ ? generation : 0), 64);
        self->setProperty("SnapshotStatus", self->snapshotValid_ ? "ValidatedPowerUpCapture" :
            revoked ? "PowerUpCaptureRevoked" : "PowerUpCaptureFailed");
        IOLog("HackyNaviDevice: power generation %llu capture %s\n",
            static_cast<unsigned long long>(self->powerGeneration_), self->snapshotValid_ ? "validated" : "failed");
    }
    self->capturePending_ = false;
    IOLockUnlock(self->lock_);
}

IOReturn HackyNaviDevice::setPowerState(unsigned long ordinal, IOService*) {
    if (!lock_) return IOPMAckImplied;
    if (ordinal == 0)
        __atomic_fetch_or(&captureRevocations_, kCapturePowerRevoked, __ATOMIC_RELEASE);
    if (ordinal == 0) retireEngines(HackyNaviFunctionGroup::RetirementReason::PowerDown);
    IOLockLock(lock_);
    if (!pmInitialized_ || stopping_ || isInactive() || ordinal > 1) {
        powerAvailable_ = false;
        IOLockUnlock(lock_);
        return IOPMAckImplied;
    }
    if (ordinal == 0) {
        // Paired engine retirement above precedes this mapping release/ACK.
        // The same mutex separately excludes diagnostic selector transactions.
        powerAvailable_ = false;
        advancePowerGenerationLocked();
        clearSnapshotLocked();
        unmapRegistersLocked();
        setProperty("SnapshotStatus", "PoweredOff");
    } else if (!powerAvailable_) {
        powerAvailable_ = true;
        // PM serializes on/off callbacks. Clearing this bit cannot revive a
        // terminally revoked service; terminal requests have their own bit.
        __atomic_fetch_and(&captureRevocations_, ~kCapturePowerRevoked, __ATOMIC_RELEASE);
        advancePowerGenerationLocked();
        clearSnapshotLocked();
        // Reacquire the assigned resource after every parent-on transition.
        // A pre-sleep BAR mapping is not a lease on the new configuration.
        unmapRegistersLocked();
        setProperty("SnapshotStatus", "PowerUpCaptureQueued");
        // This wake can race the worker's final pending=false publication
        // before a FunctionGroup exists to deliver its own notification.
        if(engineMode_) {
            if(engineNotificationSerial_==UINT64_MAX)
                panic("HackyNavi: engine notification serial exhausted");
            ++engineNotificationSerial_;
        }
        queueCaptureLocked();
    }
    IOLockUnlock(lock_);
    return IOPMAckImplied;
}

void HackyNaviDevice::systemWillShutdown(IOOptionBits specifier) {
    __atomic_fetch_or(&captureRevocations_, kCaptureTerminalRevoked, __ATOMIC_RELEASE);
    retireEngines(HackyNaviFunctionGroup::RetirementReason::SystemShutdown);
    if (lock_) {
        IOLockLock(lock_);
        stopping_ = true;
        powerAvailable_ = false;
        advancePowerGenerationLocked();
        clearSnapshotLocked();
        unmapRegistersLocked();
        setProperty("SnapshotStatus", "SystemShutdown");
        IOLockUnlock(lock_);
    }
    IOService::systemWillShutdown(specifier);
}

void HackyNaviDevice::stopPowerManagement() {
    __atomic_fetch_or(&captureRevocations_, kCaptureTerminalRevoked, __ATOMIC_RELEASE);
    if (lock_) IOLockLock(lock_);
    stopping_ = true;
    powerAvailable_ = false;
    const bool initialized = pmInitialized_;
    pmInitialized_ = false;
    if (lock_) IOLockUnlock(lock_);
    // XNU PMstop inhibits future driver calls and waits for prior calls, but
    // power-plane detach is asynchronous and the wait itself has a deadline.
    // stopping_/pmInitialized_ plus lock_ revoke MMIO independently of that
    // wait. Never hold lock_ across PMstop: callbacks acquire the same lock.
    if (initialized) PMstop();
}

bool HackyNaviDevice::willTerminate(IOService* provider, IOOptionBits options) {
    __atomic_fetch_or(&captureRevocations_, kCaptureTerminalRevoked, __ATOMIC_RELEASE);
    retireEngines(HackyNaviFunctionGroup::RetirementReason::ProviderTerminating);
    if (lock_) {
        IOLockLock(lock_);
        if (provider == pci_) {
            // Actual engine retirement has joined outside lock_ before these
            // observations and the Device's mapping reference are withdrawn.
            stopping_ = true;
            powerAvailable_ = false;
            advancePowerGenerationLocked();
            clearSnapshotLocked();
            unmapRegistersLocked();
            setProperty("SnapshotStatus", "ProviderTerminated");
        }
        IOLockUnlock(lock_);
    }
    return IOService::willTerminate(provider, options);
}

bool HackyNaviDevice::didTerminate(IOService* provider, IOOptionBits options, bool* defer) {
    // XNU schedules stop only after an open client closes its provider.
    // Waiting until stop() to close would leave this service permanently open
    // on provider removal. releaseResources joins retirement before closing.
    bool ours = false;
    if (lock_) {
        IOLockLock(lock_);
        ours = provider == pci_;
        IOLockUnlock(lock_);
    }
    if (ours) releaseResources();
    return IOService::didTerminate(provider, options, defer);
}

void HackyNaviDevice::releaseResources() {
    __atomic_fetch_or(&captureRevocations_, kCaptureTerminalRevoked, __ATOMIC_RELEASE);
    retireEngines(HackyNaviFunctionGroup::RetirementReason::Close);
    stopPowerManagement();
    // No future enqueue is possible after stopping_/pmInitialized_ revoke it.
    // Never wait while holding the mutex needed by the worker. A completed
    // ONCE cancellation/drain finishes before dropping the worker's service
    // hold, including when a completed generation has already been requeued.
    if (captureCall_) thread_call_cancel_wait(captureCall_);
    // Draining also covers a worker-local group which lost publication to
    // termination. No group can be installed once stopping_ is set.
    if (lock_) IOLockLock(lock_);
    HackyNaviFunctionGroup* group=functionGroup_;functionGroup_=nullptr;
    HackyNaviLifecycleController* lifecycle=lifecycle_;lifecycle_=nullptr;
    if (lock_) IOLockUnlock(lock_);
    if(group){group->shutdown();group->release();}
    if(lifecycle)lifecycle->release();
    if (lock_) IOLockLock(lock_);
    stopping_ = true;
    acquisitionActive_ = false;
    memoryCaptureActive_ = false;
    displayCaptureActive_ = false;
    displayContextCaptureActive_ = false;
    displayContextGeneration_ = 0;
    displayContextAccessFailure_ = nullptr;
    romCaptureActive_=postedCaptureActive_=auxCaptureActive_=false;
    snapshotValid_ = false;
    capturePending_ = false;
    const bool dropCaptureHold = captureSelfHeld_;
    captureSelfHeld_ = false;
    // free() can run after an unsuccessful superclass init(). Do not access
    // registry storage unless the superclass completed its initialization.
    if (registryReady_) {
        removeProperty("Discovery");
        removeProperty("DiscoveryAcquisition");
        removeProperty("MemoryLayout");
        removeProperty("BootDisplay");
        removeProperty("BootDisplayContext");
        removeProperty("RuntimeROM");removeProperty("PostedColdState");removeProperty("ColdAuxiliaryEngines");
        setProperty("SnapshotValid", kOSBooleanFalse);
        setProperty("SnapshotGeneration", UInt64(0), 64);
        setProperty("SnapshotStatus", "Stopped");
        setProperty("RegisterBytes", UInt64(0), 64);
    }
    ipLayout_ = hackynavi::Navi31IpLayout{};
    memoryLayout_ = hackynavi::MemoryLayout{};
    hdpLayout_ = hackynavi::HdpLayout{};
    displayLayout_ = hackynavi::bootdisplay::Layout{};
    displayCapture_ = hackynavi::bootdisplay::Capture{};
    displayContextLayout_ = hackynavi::bootdisplaycontext::Layout{};
    displayContextCapture_ = hackynavi::bootdisplaycontext::Capture{};
    OSData* rom=runtimeRom_;runtimeRom_=nullptr;
    romLayout_=hackynavi::runtimerom::Layout{};
    romCapture_=hackynavi::runtimerom::Result{};
    romCapture_.status=hackynavi::runtimerom::Status::InvalidArgument;
    firmwareInfo_=hackynavi::FirmwareInfo{};vramUsage_=hackynavi::VramUsageInfo{};
    firmwareInfoStatus_=vramUsageStatus_=hackynavi::Status::NotFound;
    postedCapture_=hackynavi::bootstrap::Result{};
    postedCapture_.status=hackynavi::bootstrap::Status::InvalidArgument;
    auxLayout_=hackynavi::coldaux::Layout{};auxCapture_=hackynavi::coldaux::Result{};
    auxCapture_.status=hackynavi::coldaux::Status::InvalidArgument;
    postedRegisterOffsets_[0]=postedRegisterOffsets_[1]=0;
    uint8_t* discovery = discoveryBytes_;
    discoveryBytes_ = nullptr;
    discoveryCapture_ = hackynavi::DeviceDiscoveryResult{};
    registers_ = nullptr;
    registerBytes_ = 0;
    IOMemoryMap* map = registerMap_;
    registerMap_ = nullptr;
    IOPCIDevice* device = pci_;
    pci_ = nullptr;
    const bool opened = opened_;
    opened_ = false;
    if (lock_) IOLockUnlock(lock_);
    // Provider arbitration and object release occur outside the access lock.
    if (discovery) IOFree(discovery, hackynavi::kDeviceDiscoveryMaxBytes);
    if(rom)rom->release();
    if (map) map->release();
    if (device) {
        if (opened) device->close(this);
        device->release();
    }
    if (dropCaptureHold) release();
}

void HackyNaviDevice::stop(IOService* provider) {
    releaseResources();
    IOService::stop(provider);
}

void HackyNaviDevice::free() {
    releaseResources();
    if (captureCall_) { thread_call_free(captureCall_); captureCall_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    IOService::free();
}
