// SPDX-License-Identifier: MIT
#include "HackyNavi.hpp"
#include <hackynavi/rom.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

OSDefineMetaClassAndStructors(HackyNavi, IOService)

namespace {
constexpr UInt32 kTargetIdentity = 0x744c1002U;
constexpr unsigned kSnapshotLimit = 16;
constexpr unsigned kRomByteLimit = 4U * 1024U * 1024U;

bool number(OSDictionary* dictionary, const char* key, UInt64 value,
            unsigned bits = 32) {
    OSNumber* object = OSNumber::withNumber(value, bits);
    if (!object) return false;
    const bool success = dictionary->setObject(key, object);
    object->release();
    return success;
}

bool string(OSDictionary* dictionary, const char* key, const char* value) {
    OSString* object = OSString::withCString(value);
    if (!object) return false;
    const bool success = dictionary->setObject(key, object);
    object->release();
    return success;
}

// The Intel IOPCIFamily registry encodes these properties as little-endian
// OSData. OSNumber is also accepted, but missing/short values are not guessed.
bool registryNumber(IORegistryEntry* entry, const char* key, UInt32& result) {
    OSObject* property = entry->copyProperty(key);
    if (!property) return false;
    bool valid = false;
    if (OSNumber* value = OSDynamicCast(OSNumber, property)) {
        result = value->unsigned32BitValue();
        valid = true;
    } else if (OSData* dataValue = OSDynamicCast(OSData, property)) {
        if (dataValue->getLength() == 4) {
            const auto* bytes = static_cast<const UInt8*>(dataValue->getBytesNoCopy());
            result = UInt32(bytes[0]) | (UInt32(bytes[1]) << 8) |
                     (UInt32(bytes[2]) << 16) | (UInt32(bytes[3]) << 24);
            valid = true;
        }
    }
    property->release();
    return valid;
}

OSArray* captureBars(IOPCIDevice* device) {
    OSArray* bars = OSArray::withCapacity(6);
    if (!bars) return nullptr;
    for (unsigned index = 0; index < 6; ++index) {
        const unsigned offset = 0x10 + index * 4;
        const UInt32 low = device->configRead32(offset);
        OSDictionary* bar = OSDictionary::withCapacity(8);
        if (!bar) { bars->release(); return nullptr; }
        bool valid = number(bar, "Index", index) &&
                     number(bar, "ConfigOffset", offset) &&
                     number(bar, "RawLow", low);
        if (low == 0xffffffffU) {
            valid = string(bar, "Kind", "Unavailable") && valid;
        } else if (low == 0) {
            valid = string(bar, "Kind", "UnassignedOrUnimplemented") && valid;
        } else if (low & 1U) {
            valid = string(bar, "Kind", "IO") && valid;
            valid = number(bar, "ConfiguredAddress", low & ~UInt32(3), 64) && valid;
        } else {
            const unsigned type = (low >> 1) & 3;
            UInt64 address = low & ~UInt32(15);
            valid = bar->setObject("Prefetchable", (low & 8) ? kOSBooleanTrue : kOSBooleanFalse) && valid;
            if (type == 2 && index < 5) {
                const UInt32 high = device->configRead32(offset + 4);
                address |= UInt64(high) << 32;
                valid = number(bar, "RawHigh", high) && valid;
                valid = string(bar, "Kind", "Memory64") && valid;
                ++index; // The next configuration dword belongs to this BAR.
            } else if (type == 0 || type == 1) {
                valid = string(bar, "Kind", type ? "MemoryBelow1M" : "Memory32") && valid;
            } else {
                valid = string(bar, "Kind", "InvalidOrReservedEncoding") && valid;
                // There is no valid address interpretation for this encoding.
                valid = bars->setObject(bar) && valid;
                bar->release();
                if (!valid) { bars->release(); return nullptr; }
                continue;
            }
            valid = number(bar, "ConfiguredAddress", address, 64) && valid;
        }
        valid = bars->setObject(bar) && valid;
        bar->release();
        if (!valid) { bars->release(); return nullptr; }
    }
    return bars;
}

void parseRom(OSDictionary* result, const OSData* data) {
    const auto* bytes = static_cast<const uint8_t*>(data->getBytesNoCopy());
    const size_t size = data->getLength();
    hackynavi::ImageInfo image = {};
    hackynavi::Status status = hackynavi::Status::NotFound;
    size_t next = 0;
    unsigned imagesExamined = 0;
    bool selected = false;
    // A supplied full flash dump can contain multiple independent ROM chains.
    // Bound enumeration as well as the byte count before entering the parser.
    while (imagesExamined < 64 && next < size) {
        status = hackynavi::findNextImage(bytes, size, next, image);
        if (status != hackynavi::Status::Ok) {
            if (status == hackynavi::Status::NotFound) next = size;
            break;
        }
        ++imagesExamined;
        next = image.offset + image.size; // Validated within the input by parser.
        if (image.vendorId == 0x1002 && image.deviceId == 0x744c && image.codeType == 0) {
            selected = true;
            break;
        }
    }
    number(result, "ImagesExamined", imagesExamined);
    // Exclusive progress offset, not a count of individual parser byte reads.
    // A NotFound result exhausts the remaining source; a capped scan does not.
    number(result, "SearchEndOffset", next, 64);
    if (!selected) {
        const bool limitReached = imagesExamined == 64 && next < size;
        string(result, "Status", limitReached ? "SearchLimitReached" : "NoMatchingLegacyPCIImage");
        if (limitReached) status = hackynavi::Status::CapacityExceeded;
        string(result, "SearchStatus", hackynavi::statusString(status));
        return;
    }
    number(result, "ImageOffset", image.offset, 64);
    number(result, "ImageBytes", image.size, 64);
    result->setObject("LegacyChecksumValid", image.checksumValid ? kOSBooleanTrue : kOSBooleanFalse);
    hackynavi::AtomInfo atom = {};
    status = hackynavi::parseAtom(bytes, size, image, atom);
    string(result, "Status", hackynavi::statusString(status));
    if (status == hackynavi::Status::Ok || status == hackynavi::Status::UnsupportedVersion) {
        number(result, "ATOMFormatRevision", atom.header.formatRevision);
        number(result, "ATOMContentRevision", atom.header.contentRevision);
        if (atom.masterData.offset != 0) {
            number(result, "MasterDataFormatRevision", atom.masterData.formatRevision);
            number(result, "MasterDataContentRevision", atom.masterData.contentRevision);
        }
    }
    if (status != hackynavi::Status::Ok) return;
    number(result, "FirmwareSubsystemVendorID", atom.subsystemVendorId);
    number(result, "FirmwareSubsystemDeviceID", atom.subsystemDeviceId);
    // These describe firmware definitions, never live VRAM or display state.
    hackynavi::VramInfo vram = {};
    status = hackynavi::parseVram(bytes, size, image, vram);
    string(result, "VRAMTableStatus", hackynavi::statusString(status));
    if (status == hackynavi::Status::Ok) {
        number(result, "FirmwareMemoryType", vram.memoryType);
        number(result, "FirmwareChannelCount", vram.channelCount);
        number(result, "FirmwareBusWidthBits", vram.busWidthBits);
        number(result, "AlternativeMemoryDefinitionCount", vram.moduleCount);
    }
    hackynavi::DisplayInfo display = {};
    status = hackynavi::parseDisplay(bytes, size, image, display);
    string(result, "DisplayTableStatus", hackynavi::statusString(status));
    if (status == hackynavi::Status::Ok)
        number(result, "FirmwareDisplayPathCount", display.pathCount);
}

OSDictionary* captureRom(IOPCIDevice* device) {
    OSDictionary* result = OSDictionary::withCapacity(28);
    if (!result) return nullptr;
    string(result, "SourceSelection", "First nonempty OSData within 4MiB: ATY,bin_image then rom-image; parse failure does not fall back");
    const char* keys[] = {"ATY,bin_image", "rom-image"};
    bool propertySeen = false;
    for (unsigned i = 0; i < 2; ++i) {
        OSObject* property = device->copyProperty(keys[i]);
        if (!property) continue;
        propertySeen = true;
        OSData* data = OSDynamicCast(OSData, property);
        const unsigned length = data ? data->getLength() : 0;
        if (!data || length == 0 || length > kRomByteLimit) {
            property->release();
            continue;
        }
        // Parse a private bounded copy, not a borrowed registry byte pointer.
        OSData* copy = OSData::withData(data, 0, length);
        property->release();
        if (!copy) { string(result, "Status", "AllocationFailed"); return result; }
        string(result, "SourceProperty", keys[i]);
        number(result, "SourceBytes", copy->getLength());
        parseRom(result, copy);
        copy->release();
        return result;
    }
    string(result, "Status", propertySeen ? "PropertyNotUsableOSData" : "NotProvidedByOS");
    return result;
}
} // namespace

bool HackyNavi::init(OSDictionary* properties) {
    lock_ = nullptr;
    notifier_ = nullptr;
    snapshots_ = nullptr;
    stopping_ = true;
    return IOService::init(properties);
}

bool HackyNavi::start(IOService* provider) {
    if (!IOService::start(provider)) return false;
    lock_ = IOLockAlloc();
    snapshots_ = OSDictionary::withCapacity(kSnapshotLimit);
    OSDictionary* matching = serviceMatching("IOPCIDevice");
    if (!lock_ || !snapshots_ || !matching) {
        if (matching) matching->release();
        IOService::stop(provider);
        return false; // free() releases the successfully allocated members.
    }
    setProperty("DiagnosticMode", "PassivePCISnapshot");
    setProperty("TargetArchitecture", "Navi31-gfx1100");
    setProperty("TargetRuntime", "macOS 15.8.1 Intel; build baseline 15.0");
    setProperty("HardwareAccess", "PCI configuration reads only");
    setProperty("SnapshotSemantics", "Historical first-match observations; no hot-unplug tracking");
    setProperty("BARSizeStatus", "NotProbed");
    setProperty("AccelerationImplemented", kOSBooleanFalse);
    setProperty("SnapshotLimit", kSnapshotLimit, 32);
    setProperty("ObservedDeviceCount", UInt64(0), 32);
    stopping_ = false;
    // Existing matches may call matched() before this assignment completes.
    notifier_ = addMatchingNotification(gIOFirstMatchNotification, matching,
                                       &HackyNavi::matched, this);
    matching->release(); // This API retains, rather than consumes, matching.
    if (!notifier_) {
        stopping_ = true;
        IOService::stop(provider);
        return false;
    }
    registerService();
    IOLog("HackyNavi: read-only Navi 31 PCI observer started (no acceleration)\n");
    return true;
}

bool HackyNavi::matched(void* target, void*, IOService* service, IONotifier*) {
    auto* self = static_cast<HackyNavi*>(target);
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, service);
    if (!device) return true;
    // Do not read configuration space of unrelated devices.
    UInt32 vendor = 0, product = 0;
    if (!registryNumber(device, "vendor-id", vendor) || vendor != 0x1002 ||
        !registryNumber(device, "device-id", product) || product != 0x744c)
        return true;
    IOLockLock(self->lock_);
    if (!self->stopping_ && !device->isInactive()) self->capture(device);
    IOLockUnlock(self->lock_);
    // IOKit retains service throughout this callback; nothing is stored later.
    return true;
}

void HackyNavi::capture(IOPCIDevice* device) {
    char key[24];
    snprintf(key, sizeof(key), "%llx", static_cast<unsigned long long>(device->getRegistryEntryID()));
    if (snapshots_->getObject(key)) return;
    if (snapshots_->getCount() >= kSnapshotLimit) {
        setProperty("SnapshotLimitReached", kOSBooleanTrue);
        return;
    }
    OSDictionary* snapshot = OSDictionary::withCapacity(16);
    if (!snapshot) return;
    number(snapshot, "RegistryEntryID", device->getRegistryEntryID(), 64);
    number(snapshot, "RegistryVendorID", 0x1002);
    number(snapshot, "RegistryDeviceID", 0x744c);
    const UInt32 identity = device->configRead32(0x00);
    number(snapshot, "PCIIdentityRaw", identity);
    if (identity != kTargetIdentity) {
        string(snapshot, "Status", "ConfigurationUnavailableOrIdentityMismatch");
    } else {
        const UInt32 revisionClass = device->configRead32(0x08);
        const UInt32 header = device->configRead32(0x0c);
        number(snapshot, "VendorID", identity & 0xffff);
        number(snapshot, "DeviceID", identity >> 16);
        number(snapshot, "RevisionID", revisionClass & 0xff);
        number(snapshot, "ClassCode", revisionClass >> 8);
        number(snapshot, "HeaderType", (header >> 16) & 0x7f);
        if (((header >> 16) & 0x7f) == 0) {
            const UInt32 subsystem = device->configRead32(0x2c);
            number(snapshot, "SubsystemVendorID", subsystem & 0xffff);
            number(snapshot, "SubsystemDeviceID", subsystem >> 16);
            OSArray* bars = captureBars(device);
            if (bars) { snapshot->setObject("ConfiguredBARs", bars); bars->release(); }
            else string(snapshot, "BARCaptureStatus", "AllocationFailed");
        }
        string(snapshot, "Status", device->configRead32(0x00) == identity ?
               "CapturedBestEffort" : "DeviceChangedDuringCapture");
    }
    OSDictionary* rom = captureRom(device);
    if (rom) { snapshot->setObject("VBIOS", rom); rom->release(); }
    // Never mutate a dictionary after publication: IORegistry readers may
    // serialize it concurrently. Only this private outer dictionary changes.
    if (snapshots_->setObject(key, snapshot)) {
        OSDictionary* published = OSDictionary::withDictionary(snapshots_);
        if (published) {
            if (setProperty("DeviceSnapshots", published))
                setProperty("ObservedDeviceCount", snapshots_->getCount(), 32);
            else
                setProperty("SnapshotPublicationFailed", kOSBooleanTrue);
            published->release();
        } else setProperty("SnapshotPublicationFailed", kOSBooleanTrue);
    }
    snapshot->release();
}

void HackyNavi::removeNotifier() {
    if (lock_) {
        IOLockLock(lock_);
        stopping_ = true;
        IOLockUnlock(lock_);
    }
    // remove() drains callbacks and releases the notifier. Calling it while
    // holding lock_ could deadlock with a callback waiting to acquire lock_.
    if (notifier_) {
        notifier_->remove();
        notifier_ = nullptr;
    }
}

void HackyNavi::stop(IOService* provider) {
    removeNotifier();
    IOService::stop(provider);
}

void HackyNavi::free() {
    removeNotifier();
    if (snapshots_) { snapshots_->release(); snapshots_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    IOService::free();
}
