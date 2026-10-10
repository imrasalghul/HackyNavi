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
                           UInt16& command, UInt8& header) {
    if (!device || device->isInactive() || device->configRead32(0x00) != kIdentity)
        return false;
    revisionClass = device->configRead32(0x08);
    header = device->configRead8(0x0e);
    command = device->configRead16(kIOPCIConfigCommand);
    if (revisionClass == 0xffffffffU || (revisionClass & 0xffU) == 0xffU ||
        (revisionClass >> 24) != 0x03 || (header & 0x7fU) != 0 ||
        command == 0xffffU || !(command & 0x2U)) return false;

    // A PM capability, if present, must already report D0. This snapshot is
    // not a power-management lease and cannot exclude an external transition.
    UInt8 pmOffset = 0;
    if (device->findPCICapability(0x01, &pmOffset)) {
        if (pmOffset < 0x40 || pmOffset > 0xf8 || (pmOffset & 3U)) return false;
        const UInt16 power = device->configRead16(pmOffset + 4);
        if (power == 0xffffU || (power & 3U) != 0) return false;
    }
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
    if (!IOService::init(properties)) return false;
    registryReady_ = true;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}

IOService* HackyNaviDevice::probe(IOService* provider, SInt32* score) {
    // Gate before claiming the IOPCIDevice or touching any configuration/MMIO.
    if (!enabled()) return nullptr;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    UInt32 revisionClass = 0;
    UInt16 command = 0;
    UInt8 header = 0;
    if (!validateConfiguration(device, revisionClass, command, header))
        return nullptr;
    return IOService::probe(provider, score);
}

bool HackyNaviDevice::mapRegisters() {
    // BAR5 must be its own assigned 32-bit memory BAR. Never size it by writing
    // config space, and never interpret the upper half of a 64-bit BAR as BAR5.
    const UInt32 bar4 = pci_->configRead32(kIOPCIConfigBaseAddress4);
    const UInt32 bar5 = pci_->configRead32(kIOPCIConfigBaseAddress5);
    if (bar4 == 0xffffffffU || (!(bar4 & 1U) && (bar4 & 6U) == 4U) ||
        bar5 == 0xffffffffU || (bar5 & 7U) != 0 || (bar5 & ~UInt32(15)) == 0)
        return false;
    IODeviceMemory* memory = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (!memory || memory->getPhysicalAddress() != (bar5 & ~UInt32(15)) ||
        memory->getLength() < kRequiredRegisterBytes ||
        memory->getLength() > kRegisterMapLimit) return false;
    // The descriptor belongs to the provider; retain only the returned map.
    registerMap_ = memory->map(kIOMapInhibitCache);
    if (!registerMap_) return false;
    registerBytes_ = registerMap_->getLength();
    const IOVirtualAddress address = registerMap_->getVirtualAddress();
    if (!address || (address & 3U) || registerBytes_ < kRequiredRegisterBytes ||
        registerBytes_ > kRegisterMapLimit) return false;
    registers_ = reinterpret_cast<volatile uint32_t*>(address);
    return true;
}

bool HackyNaviDevice::accessAllowed(uint32_t offset) const {
    return (acquisitionActive_ || memoryCaptureActive_ || displayCaptureActive_ ||
        romCaptureActive_ || postedCaptureActive_ || auxCaptureActive_) &&
        !stopping_ && powerAvailable_ &&
        pmInitialized_ && opened_ && pci_ &&
        !pci_->isInactive() && registers_ && (offset & 3U) == 0 &&
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
    return true;
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
    return true;
}

void HackyNaviDevice::delayUs(void*, uint32_t microseconds) {
    // The enclosing IOLock is a mutex, not a spinlock. Yield for the 1ms polls.
    if (microseconds >= 1000) IOSleep((microseconds + 999) / 1000);
    else IODelay(microseconds);
}

bool HackyNaviDevice::captureDiscovery() {
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
    if (acquired != hackynavi::DeviceDiscoveryStatus::Ok) {
        IOLog("HackyNaviDevice: acquisition failed: %s; bytes=%llu; selector writes attempted=%u\n",
              hackynavi::deviceDiscoveryStatusString(acquired),
              static_cast<unsigned long long>(capture.bytesCopied),
              unsigned(capture.selectorWritesAttempted));
        IOFree(buffer, hackynavi::kDeviceDiscoveryMaxBytes);
        return false;
    }
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
    valid = valid && captureRuntimeRom(buffer,capture.binarySize);
    valid = valid && captureColdState(buffer,capture.binarySize);
    valid = valid && captureMemoryLayout(capture.vramBytes);
    valid = valid && captureBootDisplay(buffer, capture.binarySize);
    if (valid) {
        discoveryBytes_ = buffer;
        discoveryCapture_ = capture;
    } else {
        IOFree(buffer, hackynavi::kDeviceDiscoveryMaxBytes);
    }
    return valid;
}

bool HackyNaviDevice::captureRuntimeRom(const uint8_t* discovery,size_t bytes) {
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
    auxCapture_=ca::Result{};
    auto status=ca::buildLayout(discovery,bytes,registerBytes_,auxLayout_);
    const bool postedCold=postedCapture_.status==bs::Status::Ok;
    if(postedCold && status==ca::Status::Ok) {
        auxCaptureActive_=true;
        status=ca::checkStopped(auxLayout_,{this,read32},auxCapture_);
        auxCaptureActive_=false;
    }
    auxCapture_.status=postedCold?status:ca::Status::InvalidArgument;
    OSDictionary* aux=OSDictionary::withCapacity(10);
    if(!aux)return false;
    valid=valid && string(aux,"Status",postedCold?ca::statusString(status):"NotCheckedWithoutPostedColdState") &&
        number(aux,"Reads",auxCapture_.reads) && number(aux,"LastRegister",auxCapture_.lastRegister) &&
        number(aux,"LastValue",auxCapture_.lastValue) && number(aux,"FailedInstance",auxCapture_.instance,8) &&
        number(aux,"PoweredOffMask",auxCapture_.poweredOffMask,8) &&
        string(aux,"OwnershipStatus","DiagnosticOnlyExternalExclusionNotEstablished") && setProperty("ColdAuxiliaryEngines",aux);
    aux->release();return valid;
}

bool HackyNaviDevice::captureMemoryLayout(uint64_t vramBytes) {
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
    IODeviceMemory* bar = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
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
    namespace bd = hackynavi::bootdisplay;
    displayCapture_ = bd::Capture{};
    displayCapture_.failedPipe = 0xff;
    auto status = bd::buildLayout(discovery, bytes, registerBytes_, displayLayout_);
    if (status == bd::Status::Ok) {
        const bd::Io io = {this, read32};
        displayCaptureActive_ = true;
        status = postedCapture_.status==hackynavi::bootstrap::Status::Ok?
            bd::capturePostedCold(displayLayout_,memoryLayout_,io,displayCapture_):
            bd::capture(displayLayout_, memoryLayout_, io, displayCapture_);
        displayCaptureActive_ = false;
    }
    // This is diagnostic capture. Two equal snapshots do not establish that
    // firmware/another display programmer cannot change the addresses later.
    // Never configure an allocator or a GART from this report alone.
    OSDictionary* report = OSDictionary::withCapacity(9);
    OSArray* planes = OSArray::withCapacity(bd::kPipeCount);
    if (!report || !planes) {
        if (report) report->release();
        if (planes) planes->release();
        return false;
    }
    bool valid = true;
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
    valid = valid && string(report, "CaptureStatus", bd::statusString(status)) &&
        report->setObject("CompleteClassification", displayCapture_.complete ? kOSBooleanTrue : kOSBooleanFalse) &&
        report->setObject("ReservationUseAllowed", kOSBooleanFalse) &&
        number(report, "PlaneCount", displayCapture_.planeCount) &&
        number(report, "PoweredOffMask", displayCapture_.poweredOffMask,8) &&
        number(report, "ReadsAttempted", displayCapture_.readsAttempted) &&
        number(report, "LastRegister", displayCapture_.lastRegister) &&
        number(report, "FailedPipe", displayCapture_.failedPipe) &&
        string(report, "Scope", "Observed scanout footprint; ownership and complete display allocation inventory unestablished") &&
        report->setObject("Planes", planes) && setProperty("BootDisplay", report);
    planes->release();
    report->release();
    // Unsupported scanout is reported explicitly; it does not invalidate the
    // independently captured discovery/memory snapshot or enable allocation.
    return valid;
}

bool HackyNaviDevice::start(IOService* provider) {
    if (!enabled() || !lock_ || !IOService::start(provider)) return false;
    bool success = false;
    IOLockLock(lock_);
    pci_ = OSDynamicCast(IOPCIDevice, provider);
    if (pci_) pci_->retain();
    UInt32 revisionClass = 0;
    UInt16 command = 0;
    UInt8 header = 0;
    if (pci_ && validateConfiguration(pci_, revisionClass, command, header)) {
        opened_ = pci_->open(this);
        if (opened_ && validateConfiguration(pci_, revisionClass, command, header)) {
            stopping_ = false;
            if (mapRegisters()) {
                success = setProperty("Mode", "ExperimentalDeviceBringup") &&
                    setProperty("RequiredBootArgument", "-hackynavi-device") &&
                    setProperty("AccelerationAvailable", kOSBooleanFalse) &&
                    setProperty("DisplayInitializationAvailable", kOSBooleanFalse) &&
                    setProperty("PCIIdentity", UInt64(kIdentity), 32) &&
                    setProperty("PCIRevision", UInt64(revisionClass & 0xffU), 8) &&
                    setProperty("PCIClass", UInt64(revisionClass >> 8), 24) &&
                    setProperty("PCICommandAtStart", UInt64(command), 16) &&
                    setProperty("PCIHeaderType", UInt64(header), 8) &&
                    setProperty("RegisterBAR", UInt64(5), 32) &&
                    setProperty("RegisterBytes", UInt64(registerBytes_), 64) &&
                    setProperty("SnapshotStatus", "WaitingForPower") &&
                    setProperty("SnapshotValid", kOSBooleanFalse);
            }
        }
    }
    IOLockUnlock(lock_);
    if (success) {
        // These calls may arrange or invoke callbacks. Never hold lock_ over
        // PM registration, requests or PMstop(). setPowerState performs all
        // discovery MMIO after the parent-on notification, not in start().
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
    if (discoveryBytes_) {
        IOFree(discoveryBytes_, hackynavi::kDeviceDiscoveryMaxBytes);
        discoveryBytes_ = nullptr;
    }
    discoveryCapture_ = hackynavi::DeviceDiscoveryResult{};
    for (unsigned i = 0; i < 3; ++i) memoryRegisterOffsets_[i] = 0;
    // Remove the published evidence together with the in-memory snapshot, so
    // callers cannot confuse pre-sleep bases with the new power generation.
    removeProperty("Discovery");
    removeProperty("MemoryLayout");
    removeProperty("BootDisplay");
    removeProperty("RuntimeROM");removeProperty("PostedColdState");removeProperty("ColdAuxiliaryEngines");
    setProperty("SnapshotValid", kOSBooleanFalse);
}

IOReturn HackyNaviDevice::setPowerState(unsigned long ordinal, IOService*) {
    if (!lock_) return IOPMAckImplied;
    IOLockLock(lock_);
    if (!pmInitialized_ || stopping_ || ordinal > 1) {
        powerAvailable_ = false;
        IOLockUnlock(lock_);
        return IOPMAckImplied;
    }
    if (ordinal == 0) {
        // The same mutex excludes every complete selector/read transaction.
        // No DMA or engine has been enabled by this service, so no GPU work
        // needs retirement before this acknowledgement. That must change
        // before a GART controller is activated from this service.
        powerAvailable_ = false;
        clearSnapshotLocked();
        setProperty("SnapshotStatus", "PoweredOff");
    } else if (!powerAvailable_) {
        powerAvailable_ = true;
        clearSnapshotLocked();
        UInt32 revisionClass = 0;
        UInt16 command = 0;
        UInt8 header = 0;
        // The power callback orders us after the parent, but configuration
        // must still be accessible and the assigned BAR must still match.
        const UInt32 bar5 = pci_ ? pci_->configRead32(kIOPCIConfigBaseAddress5) : 0;
        const bool mapMatches = registerMap_ && bar5 != UINT32_MAX &&
            (bar5 & 7U) == 0 && (bar5 & ~UInt32(15)) != 0 &&
            registerMap_->getPhysicalAddress() == (bar5 & ~UInt32(15));
        snapshotValid_ = opened_ && mapMatches &&
            validateConfiguration(pci_, revisionClass, command, header) && captureDiscovery();
        if (!snapshotValid_) clearSnapshotLocked();
        setProperty("SnapshotValid", snapshotValid_ ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("SnapshotStatus", snapshotValid_ ? "ValidatedPowerUpCapture" : "PowerUpCaptureFailed");
        IOLog("HackyNaviDevice: power-up capture %s\n", snapshotValid_ ? "validated" : "failed");
    }
    IOLockUnlock(lock_);
    return IOPMAckImplied;
}

void HackyNaviDevice::systemWillShutdown(IOOptionBits specifier) {
    if (lock_) {
        IOLockLock(lock_);
        stopping_ = true;
        powerAvailable_ = false;
        clearSnapshotLocked();
        setProperty("SnapshotStatus", "SystemShutdown");
        IOLockUnlock(lock_);
    }
    IOService::systemWillShutdown(specifier);
}

void HackyNaviDevice::stopPowerManagement() {
    if (lock_) IOLockLock(lock_);
    stopping_ = true;
    powerAvailable_ = false;
    const bool initialized = pmInitialized_;
    pmInitialized_ = false;
    if (lock_) IOLockUnlock(lock_);
    // PMstop is synchronous with the power callbacks, which take lock_. It
    // must run without that lock, before dropping the BAR/provider or lock.
    if (initialized) PMstop();
}

void HackyNaviDevice::releaseResources() {
    stopPowerManagement();
    if (lock_) IOLockLock(lock_);
    stopping_ = true;
    acquisitionActive_ = false;
    memoryCaptureActive_ = false;
    displayCaptureActive_ = false;
    romCaptureActive_=postedCaptureActive_=auxCaptureActive_=false;
    snapshotValid_ = false;
    // free() can run after an unsuccessful superclass init(). Do not access
    // registry storage unless the superclass completed its initialization.
    if (registryReady_) {
        removeProperty("Discovery");
        removeProperty("MemoryLayout");
        removeProperty("BootDisplay");
        removeProperty("RuntimeROM");removeProperty("PostedColdState");removeProperty("ColdAuxiliaryEngines");
        setProperty("SnapshotValid", kOSBooleanFalse);
        setProperty("SnapshotStatus", "Stopped");
    }
    ipLayout_ = hackynavi::Navi31IpLayout{};
    memoryLayout_ = hackynavi::MemoryLayout{};
    hdpLayout_ = hackynavi::HdpLayout{};
    displayLayout_ = hackynavi::bootdisplay::Layout{};
    displayCapture_ = hackynavi::bootdisplay::Capture{};
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
}

void HackyNaviDevice::stop(IOService* provider) {
    releaseResources();
    IOService::stop(provider);
}

void HackyNaviDevice::free() {
    releaseResources();
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    IOService::free();
}
