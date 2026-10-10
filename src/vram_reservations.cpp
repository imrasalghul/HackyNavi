// SPDX-License-Identifier: MIT
// Original bounded reservation union builder. Primary format/policy references
// in Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu/amdgpu_ttm.c:1741-1795,1899-1914 (FW tail and training data blocks),
// amdgpu/amdgpu_psp.h:246-249; psp_v13_0.c:616-651 (temporary long-training damage),
// amdgpu/amdgpu_atomfirmware.c:105-169,1006-1051 (usage flags, CPU scratch, FW size),
// include/atomfirmware.h:575-647,742-800 (3.5 protected tail and usage formats),
// include/atombios.h:4298-4302 (operation flags). Explicit usage ranges are
// conservatively protected even where Linux's bare-metal path only uses them
// for CPU scratch metadata; no upstream allocation implementation is copied.
#include "hackynavi/vram_reservations.hpp"

namespace hackynavi {
namespace {
constexpr uint64_t kPage = 4096, kMiB = UINT64_C(1) << 20;
constexpr size_t kCandidates = 2 * kVramReservationInputRanges + 7;
struct Candidate { VramRange range; uint32_t provenance; };
struct Builder {
    Candidate candidates[kCandidates];
    size_t count;
    uint64_t vramBytes;
    bool append(uint64_t offset, uint64_t bytes, uint32_t provenance) {
        if (!bytes || offset >= vramBytes || bytes > vramBytes - offset || count == kCandidates)
            return false;
        const uint64_t begin = offset & ~(kPage - 1);
        const uint64_t end = offset + bytes;
        // Validated VRAM is page-aligned and <=48 bits: round-up cannot wrap.
        const uint64_t roundedEnd = (end + kPage - 1) & ~(kPage - 1);
        if (roundedEnd > vramBytes) return false;
        candidates[count++] = Candidate{{begin, roundedEnd - begin}, provenance};
        return true;
    }
};
bool evidenceValid(VramReservationEvidence value) {
    return value == VramReservationEvidence::Unavailable || value == VramReservationEvidence::RomImage ||
           value == VramReservationEvidence::Runtime;
}
uint32_t origin(VramReservationEvidence evidence) {
    return evidence == VramReservationEvidence::RomImage ? ReservationFromRom : ReservationFromRuntime;
}
bool absent(const TableInfo& table, bool present) {
    return !present && !table.offset && !table.size && !table.formatRevision && !table.contentRevision;
}
bool regionConsistent(const VramUsageRegion& region, bool addressPresent) {
    if (region.startAddressPresent != addressPresent) return false;
    if (!addressPresent)
        return !region.startAddressKiBRaw && !region.startAddressKiB && !region.operationFlags;
    return region.startAddressKiB == (region.startAddressKiBRaw & 0x3fffffffu) &&
           region.operationFlags == (region.startAddressKiBRaw >> 30);
}
bool firmwareConsistent(const FirmwareInfo& f) {
    return f.present && f.table.offset && f.table.formatRevision == 3 &&
        (f.table.contentRevision == 4 || f.table.contentRevision == 5) &&
        f.table.size >= (f.table.contentRevision == 4 ? 108 : 172) &&
        f.twoStageMemoryTraining == bool(f.firmwareCapability & 0x400u) &&
        f.protectedRegionSizePresent == (f.table.contentRevision == 5) &&
        (f.protectedRegionSizePresent || f.protectedRegionSizeKiB == 0);
}
bool usageConsistent(const VramUsageInfo& u) {
    return u.present && u.table.offset && u.table.formatRevision == 2 &&
        (u.table.contentRevision == 1 || u.table.contentRevision == 2) &&
        u.table.size >= (u.table.contentRevision == 1 ? 12 : 48) &&
        regionConsistent(u.firmware, true) && u.firmware.sizeKiB <= UINT16_MAX &&
        regionConsistent(u.driver, u.table.contentRevision == 2) &&
        (u.table.contentRevision == 2 || u.driver.sizeKiB <= UINT16_MAX);
}
VramReservationStatus addUsage(Builder& b, const VramUsageRegion& region, uint32_t provenance) {
    if (region.operationFlags == 3) return VramReservationStatus::UnsupportedUsageFlags;
    // A no-reservation declaration does not override any other contributor.
    if (region.operationFlags == 1 || !region.sizeKiB) return VramReservationStatus::Ok;
    return b.append(uint64_t(region.startAddressKiB) << 10, uint64_t(region.sizeKiB) << 10, provenance)
        ? VramReservationStatus::Ok : VramReservationStatus::OutsideVram;
}
bool captureConsistent(const DeviceDiscoveryResult& d, uint64_t vramBytes) {
    if (d.vramBytes != vramBytes || !d.selectorWritesAttempted || !d.readyPolls ||
        d.readyPolls > kDeviceDiscoveryMaxReadyPolls || d.binarySize < 12 ||
        d.binarySize >= kDeviceDiscoveryMaxBytes || d.bytesCopied != d.binarySize ||
        d.regionSize < d.binarySize || ((d.regionSize | d.sourceOffset) & 3u)) return false;
    return d.usedScratch || (vramBytes >= 65536 && d.sourceOffset == vramBytes - 65536 &&
                            d.regionSize == kDeviceDiscoveryDefaultRegionBytes);
}
} // namespace

VramReservationStatus buildVramReservations(const VramReservationInput& in,
                                           VramReservationPlan& out) {
    out = VramReservationPlan{};
    if (!evidenceValid(in.firmwareEvidence) || !evidenceValid(in.usageEvidence) ||
        (in.preOsFramebufferCount && !in.preOsFramebuffers) ||
        (in.additionalReservationCount && !in.additionalReservations))
        return VramReservationStatus::InvalidArgument;
    if (in.preOsFramebufferCount > kVramReservationInputRanges ||
        in.additionalReservationCount > kVramReservationInputRanges)
        return VramReservationStatus::CapacityExceeded;
    uint64_t translated = 0;
    if (vramMcAddress(in.memory, 0, in.memory.vramBytes, translated) != MemoryLayoutStatus::Ok ||
        vramPhysicalAddress(in.memory, 0, in.memory.vramBytes, translated) != MemoryLayoutStatus::Ok)
        return VramReservationStatus::InvalidMemoryLayout;
    Builder b = {}; b.vramBytes = in.memory.vramBytes;
    uint32_t missing = 0, driverScratch = 0;
    if (in.firmwareStatus == Status::Ok) {
        if (!firmwareConsistent(in.firmware) || in.firmwareEvidence == VramReservationEvidence::Unavailable)
            return VramReservationStatus::MetadataRejected;
        const uint64_t tail = uint64_t(in.firmware.firmwareReservedKiB) << 10;
        if (!tail) return VramReservationStatus::UnsupportedZeroReservation;
        if (tail > b.vramBytes || !b.append(b.vramBytes - tail, tail,
                ReservationFirmwareTail | origin(in.firmwareEvidence)))
            return VramReservationStatus::OutsideVram;
        const uint64_t protectedTail = uint64_t(in.firmware.protectedRegionSizeKiB) << 10;
        if (protectedTail && (protectedTail > b.vramBytes || !b.append(b.vramBytes - protectedTail,
                protectedTail, ReservationProtectedTail | origin(in.firmwareEvidence))))
            return VramReservationStatus::OutsideVram;
        if (in.firmware.twoStageMemoryTraining) {
            if (b.vramBytes - tail < kMiB) return VramReservationStatus::OutsideVram;
            const uint64_t raw = b.vramBytes - tail - kMiB;
            const uint64_t training = (raw + kMiB - 1) & ~(kMiB - 1);
            if (!b.append(training, 4096, ReservationTrainingHost | origin(in.firmwareEvidence)) ||
                !b.append(b.vramBytes - 32768, 4096,
                          ReservationTrainingFirmware | origin(in.firmwareEvidence)))
                return VramReservationStatus::OutsideVram;
            if (!in.memoryTrainingInactive) missing |= MissingInactiveMemoryTraining;
        }
    } else if (in.firmwareStatus == Status::NotFound || in.firmwareStatus == Status::UnsupportedVersion) {
        if (in.firmwareStatus == Status::NotFound && !absent(in.firmware.table, in.firmware.present))
            return VramReservationStatus::MetadataRejected;
        missing |= MissingFirmwareInfo;
    } else {
        return VramReservationStatus::MetadataRejected;
    }
    if (in.usageStatus == Status::Ok) {
        if (!usageConsistent(in.usage) || in.usageEvidence == VramReservationEvidence::Unavailable)
            return VramReservationStatus::MetadataRejected;
        VramReservationStatus status = addUsage(b, in.usage.firmware,
                                               ReservationFirmwareUsage | origin(in.usageEvidence));
        if (status != VramReservationStatus::Ok) return status;
        if (in.usage.table.contentRevision == 2) {
            status = addUsage(b, in.usage.driver, ReservationDriverUsage | origin(in.usageEvidence));
            if (status != VramReservationStatus::Ok) return status;
        } else {
            driverScratch = in.usage.firmware.operationFlags == 2 ? 0 : in.usage.driver.sizeKiB;
        }
    } else if (in.usageStatus == Status::NotFound) {
        if (!absent(in.usage.table, in.usage.present)) return VramReservationStatus::MetadataRejected;
        if (in.usageEvidence == VramReservationEvidence::Unavailable) missing |= MissingVramUsage;
    } else if (in.usageStatus == Status::UnsupportedVersion) {
        missing |= MissingVramUsage;
    } else {
        return VramReservationStatus::MetadataRejected;
    }
    for (size_t i = 0; i < in.preOsFramebufferCount; ++i)
        if (!b.append(in.preOsFramebuffers[i].offset, in.preOsFramebuffers[i].bytes,
                      ReservationPreOsFramebuffer | ReservationFromRuntime))
            return VramReservationStatus::OutsideVram;
    if (!in.preOsFramebufferInventoryComplete) missing |= MissingPreOsFramebufferInventory;
    for (size_t i = 0; i < in.additionalReservationCount; ++i)
        if (!b.append(in.additionalReservations[i].offset, in.additionalReservations[i].bytes,
                      ReservationAdditional | ReservationFromRuntime))
            return VramReservationStatus::OutsideVram;
    if (!in.additionalReservationInventoryComplete) missing |= MissingAdditionalReservationInventory;
    if (in.discoveryCaptureStatus == DeviceDiscoveryStatus::Ok &&
        in.discoveryBinaryStatus == DiscoveryStatus::Ok) {
        if (!captureConsistent(in.discovery, b.vramBytes)) return VramReservationStatus::InvalidDiscovery;
        if (!b.append(in.discovery.sourceOffset, in.discovery.regionSize,
                      ReservationDiscovery | ReservationFromRuntime))
            return VramReservationStatus::OutsideVram;
    } else {
        missing |= MissingLiveDiscovery;
    }
    // Sort the whole bounded candidate set BEFORE enforcing final capacity:
    // later bridge ranges may merge more than32 raw inputs into fewer outputs.
    for (size_t i = 1; i < b.count; ++i) {
        const Candidate value = b.candidates[i];
        size_t at = i;
        while (at && b.candidates[at - 1].range.offset > value.range.offset) {
            b.candidates[at] = b.candidates[at - 1]; --at;
        }
        b.candidates[at] = value;
    }
    VramReservationPlan plan = {};
    for (size_t i = 0; i < b.count; ++i) {
        const Candidate& candidate = b.candidates[i];
        if (plan.count) {
            VramRange& last = plan.ranges[plan.count - 1];
            if (candidate.range.offset <= last.offset + last.bytes) {
                const uint64_t end = candidate.range.offset + candidate.range.bytes;
                if (end > last.offset + last.bytes) last.bytes = end - last.offset;
                plan.provenance[plan.count - 1] |= candidate.provenance;
                continue;
            }
        }
        if (plan.count == kVramReservationPlanRanges) return VramReservationStatus::CapacityExceeded;
        plan.ranges[plan.count] = candidate.range;
        plan.provenance[plan.count++] = candidate.provenance;
    }
    for (size_t i = 0; i < plan.count; ++i) plan.reservedBytes += plan.ranges[i].bytes;
    plan.driverScratchKiB = driverScratch;
    plan.missingEvidence = missing;
    plan.complete = !missing;
    out = plan;
    return missing ? VramReservationStatus::MissingEvidence : VramReservationStatus::Ok;
}

const char* vramReservationStatusString(VramReservationStatus status) {
    switch (status) {
    case VramReservationStatus::Ok: return "ok";
    case VramReservationStatus::MissingEvidence: return "reservation evidence incomplete";
    case VramReservationStatus::InvalidArgument: return "invalid reservation argument";
    case VramReservationStatus::InvalidMemoryLayout: return "invalid reservation memory layout";
    case VramReservationStatus::MetadataRejected: return "reservation metadata rejected";
    case VramReservationStatus::UnsupportedZeroReservation: return "zero firmware reservation unsupported";
    case VramReservationStatus::UnsupportedUsageFlags: return "unsupported VRAM usage operation flags";
    case VramReservationStatus::InvalidDiscovery: return "inconsistent live discovery evidence";
    case VramReservationStatus::OutsideVram: return "reservation lies outside VRAM";
    case VramReservationStatus::CapacityExceeded: return "reservation capacity exceeded";
    }
    return "unknown reservation status";
}
} // namespace hackynavi
