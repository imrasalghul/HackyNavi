// SPDX-License-Identifier: MIT
// Synthetic decoded metadata and live-capture evidence. No hardware access.
#include "hackynavi/vram_reservations.hpp"
#include <iostream>
#include <vector>
using namespace hackynavi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "line " << __LINE__ << ": " #x "\n"; } } while (0)
constexpr uint64_t MiB = UINT64_C(1) << 20, GiB = UINT64_C(1) << 30;

VramReservationInput fixture(uint64_t bytes = 24 * GiB, uint32_t tailKiB = 4096) {
    VramReservationInput in = {};
    MemoryLayoutInput memory = {0x8000, static_cast<uint32_t>(0x8000 + (bytes - 1) / (16 * MiB)),
                                0x1000, bytes, 16 * GiB, 256 * MiB};
    CHECK(deriveMemoryLayout(memory, in.memory) == MemoryLayoutStatus::Ok);
    in.firmwareStatus = Status::Ok;
    in.firmware.table = {0x600, 108, 3, 4};
    in.firmware.present = true;
    in.firmware.firmwareCapability = 0x104000;
    in.firmware.biosScratchRegisterDword = 0x4c;
    in.firmware.firmwareReservedKiB = tailKiB;
    in.firmwareEvidence = VramReservationEvidence::RomImage;
    in.usageStatus = Status::Ok;
    in.usage.table = TableInfo{0x700, 12, 2, 1}; in.usage.present = true;
    in.usage.firmware.startAddressPresent = true;
    in.usageEvidence = VramReservationEvidence::RomImage;
    in.preOsFramebufferInventoryComplete = true;
    in.discoveryCaptureStatus = DeviceDiscoveryStatus::Ok;
    in.discoveryBinaryStatus = DiscoveryStatus::Ok;
    in.discovery = DeviceDiscoveryResult{bytes, bytes - 65536, 10240, 1, 1756, 1756, false, true};
    in.additionalReservationInventoryComplete = true;
    in.memoryTrainingInactive = true;
    return in;
}
VramUsageRegion region(uint32_t startKiB, uint32_t sizeKiB, uint8_t flags) {
    return VramUsageRegion{true, startKiB | (uint32_t(flags) << 30), startKiB, sizeKiB, flags};
}
bool zero(const VramReservationPlan& p) {
    if (p.complete || p.missingEvidence || p.count || p.reservedBytes || p.driverScratchKiB) return false;
    for (size_t i = 0; i < kVramReservationPlanRanges; ++i)
        if (p.ranges[i].offset || p.ranges[i].bytes || p.provenance[i]) return false;
    return true;
}
void rejects(const VramReservationInput& in, VramReservationStatus expected) {
    VramReservationPlan out{}; out.complete = true; out.count = 1; out.ranges[0] = {123, 456};
    CHECK(buildVramReservations(in, out) == expected);
    CHECK(zero(out));
}
void basics() {
    auto in = fixture(); VramReservationPlan p{};
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    CHECK(p.complete && !p.missingEvidence && p.count == 1 && p.reservedBytes == 4 * MiB);
    CHECK(p.ranges[0].offset == 24 * GiB - 4 * MiB && p.ranges[0].bytes == 4 * MiB);
    CHECK(p.provenance[0] == (ReservationFirmwareTail | ReservationFromRom |
                              ReservationDiscovery | ReservationFromRuntime));
    VramAllocator allocator;
    CHECK(allocator.configure(in.memory, {0, in.memory.vramBytes}, p.ranges, p.count) == VramAllocatorStatus::Ok);
    CHECK(allocator.freeBytes() == in.memory.vramBytes - p.reservedBytes);
    // Unaligned live ranges are conservatively rounded and overlapping/touching contributors unioned.
    VramRange scanout[] = {{8193, 4096}, {0, 1}};
    VramRange additional[] = {{4096, 4096}, {16383, 2}, {8192, 1}};
    in.preOsFramebuffers = scanout; in.preOsFramebufferCount = 2;
    in.additionalReservations = additional; in.additionalReservationCount = 3;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    CHECK(p.count == 2 && p.ranges[0].offset == 0 && p.ranges[0].bytes == 20480);
    CHECK(p.reservedBytes == 4 * MiB + 20480);
    CHECK(p.provenance[0] == (ReservationPreOsFramebuffer | ReservationAdditional | ReservationFromRuntime));
    // A larger protected tail subsumes firmware/discovery; source labels survive.
    in = fixture(); in.firmware.table = TableInfo{0x600, 172, 3, 5};
    in.firmware.protectedRegionSizePresent = true; in.firmware.protectedRegionSizeKiB = 8192;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    CHECK(p.count == 1 && p.ranges[0].offset == 24 * GiB - 8 * MiB && p.reservedBytes == 8 * MiB);
    CHECK((p.provenance[0] & (ReservationFirmwareTail | ReservationProtectedTail)) ==
          (ReservationFirmwareTail | ReservationProtectedTail));
    in.firmware.protectedRegionSizeKiB = 0;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.reservedBytes == 4 * MiB);
    CHECK(!(p.provenance[0] & ReservationProtectedTail));
}
void usage() {
    VramReservationPlan p{};
    auto in = fixture();
    in.usage.firmware = region(9, 3, 0); in.usage.driver.sizeKiB = 37;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    CHECK(p.count == 2 && p.ranges[0].offset == 8192 && p.ranges[0].bytes == 4096 && p.driverScratchKiB == 37);
    CHECK(p.provenance[0] == (ReservationFirmwareUsage | ReservationFromRom));
    in.usage.firmware = region(9, 3, 2);
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.driverScratchKiB == 0);
    in.usage.firmware = region(9, 3, 1);
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.count == 1 && p.driverScratchKiB == 37);
    in.usage.firmware = region(9, 0, 3);
    rejects(in, VramReservationStatus::UnsupportedUsageFlags);
    in.usage.table = TableInfo{0x700, 48, 2, 2};
    in.usage.firmware = region(9, 3, 0); in.usage.driver = region(12, 5, 0);
    in.usageEvidence = VramReservationEvidence::Runtime;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    CHECK(p.count == 2 && p.ranges[0].offset == 8192 && p.ranges[0].bytes == 12288 && p.driverScratchKiB == 0);
    CHECK(p.provenance[0] == (ReservationFirmwareUsage | ReservationDriverUsage | ReservationFromRuntime));
    in.usage.driver = region(12, 5, 2);
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    in.usage.driver = region(12, 5, 1);
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.ranges[0].bytes == 4096);
    in.usage.driver = region(12, 5, 3); rejects(in, VramReservationStatus::UnsupportedUsageFlags);
    in.usage.driver = region(12, UINT32_MAX, 0); rejects(in, VramReservationStatus::OutsideVram);
    in.usage.driver = region(0x3fffffff, 1, 0); rejects(in, VramReservationStatus::OutsideVram);
    // No-reservation flags never subtract a different source's reserved region.
    in = fixture(); in.usage.firmware = region(static_cast<uint32_t>((24 * GiB - 4 * MiB) >> 10), 4096, 1);
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.reservedBytes == 4 * MiB);
    in.usageStatus = Status::NotFound; in.usage = VramUsageInfo{};
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.count == 1);
}
void training() {
    auto in = fixture(); VramReservationPlan p{};
    in.firmware.firmwareCapability |= 0x400; in.firmware.twoStageMemoryTraining = true;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    CHECK(p.count == 2 && p.ranges[0].offset == 24 * GiB - 5 * MiB && p.ranges[0].bytes == 4096);
    CHECK(p.reservedBytes == 4 * MiB + 4096);
    CHECK(p.provenance[0] == (ReservationTrainingHost | ReservationFromRom));
    CHECK(p.provenance[1] & ReservationTrainingFirmware);
    // Linux rounds UP to1MiB, not down; a1KiB tail also does not cover P2C data.
    in.firmware.firmwareReservedKiB = 1;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
    CHECK(p.ranges[0].offset == 24 * GiB - MiB && p.ranges[0].bytes == 4096);
    CHECK(p.count == 4); // host training, captured discovery, P2C training, tail
    CHECK(p.ranges[2].offset == 24 * GiB - 32768 && p.ranges[2].bytes == 4096);
    CHECK(p.reservedBytes == 4096 + 12288 + 4096 + 4096);
    in.memoryTrainingInactive = false;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::MissingEvidence);
    CHECK(!p.complete && p.missingEvidence == MissingInactiveMemoryTraining && p.count == 4);
    in.firmware.firmwareReservedKiB = static_cast<uint32_t>((24 * GiB - MiB + 1024) >> 10);
    rejects(in, VramReservationStatus::OutsideVram);
}
void missingEvidence() {
    VramReservationPlan p{};
    for (unsigned which = 0; which < 7; ++which) {
        auto in = fixture(); uint32_t expected = 0;
        switch (which) {
        case 0: in.firmwareStatus = Status::NotFound; in.firmware = FirmwareInfo{}; expected = MissingFirmwareInfo; break;
        case 1: in.firmwareStatus = Status::UnsupportedVersion; expected = MissingFirmwareInfo; break;
        case 2: in.usageStatus = Status::UnsupportedVersion; expected = MissingVramUsage; break;
        case 3: in.preOsFramebufferInventoryComplete = false; expected = MissingPreOsFramebufferInventory; break;
        case 4: in.discoveryCaptureStatus = DeviceDiscoveryStatus::TransportError; expected = MissingLiveDiscovery; break;
        case 5: in.discoveryBinaryStatus = DiscoveryStatus::ChecksumMismatch; expected = MissingLiveDiscovery; break;
        case 6: in.additionalReservationInventoryComplete = false; expected = MissingAdditionalReservationInventory; break;
        }
        CHECK(buildVramReservations(in, p) == VramReservationStatus::MissingEvidence);
        CHECK(!p.complete && p.missingEvidence == expected && p.count == 1);
        if (which < 2) CHECK(p.ranges[0].bytes == 12288 && p.provenance[0] == (ReservationDiscovery | ReservationFromRuntime));
    }
    auto in = fixture(); in.firmwareStatus = Status::NotFound; in.firmware = FirmwareInfo{};
    in.usageStatus = Status::NotFound; in.usage = VramUsageInfo{};
    in.usageEvidence = VramReservationEvidence::Unavailable;
    in.discoveryCaptureStatus = DeviceDiscoveryStatus::BadSignature;
    in.additionalReservationInventoryComplete = in.preOsFramebufferInventoryComplete = false;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::MissingEvidence);
    CHECK(!p.count && !p.reservedBytes && p.missingEvidence == (MissingFirmwareInfo | MissingVramUsage |
          MissingLiveDiscovery | MissingPreOsFramebufferInventory | MissingAdditionalReservationInventory));
}
void invalidMetadata() {
    for (unsigned which = 0; which < 23; ++which) {
        auto in = fixture();
        switch (which) {
        case 0: in.firmware.present = false; break;
        case 1: in.firmware.table.offset = 0; break;
        case 2: in.firmware.table.size = 107; break;
        case 3: in.firmware.table.formatRevision = 4; break;
        case 4: in.firmware.table.contentRevision = 6; break;
        case 5: in.firmware.twoStageMemoryTraining = true; break;
        case 6: in.firmware.protectedRegionSizePresent = true; break;
        case 7: in.firmware.protectedRegionSizeKiB = 1; break;
        case 8: in.firmwareEvidence = VramReservationEvidence::Unavailable; break;
        case 9: in.firmwareStatus = Status::Truncated; break;
        case 10: in.firmwareStatus = Status::NotFound; break;
        case 11: in.usage.present = false; break;
        case 12: in.usage.table.offset = 0; break;
        case 13: in.usage.table.size = 11; break;
        case 14: in.usage.table.contentRevision = 3; break;
        case 15: in.usage.firmware.startAddressPresent = false; break;
        case 16: in.usage.firmware.startAddressKiBRaw = 1; break;
        case 17: in.usage.firmware.operationFlags = 1; break;
        case 18: in.usage.firmware.sizeKiB = 65536; break;
        case 19: in.usage.driver.startAddressPresent = true; break;
        case 20: in.usage.driver.sizeKiB = 65536; break;
        case 21: in.usageStatus = Status::NotFound; break;
        case 22: in.usageEvidence = VramReservationEvidence::Unavailable; break;
        }
        rejects(in, VramReservationStatus::MetadataRejected);
    }
    auto in = fixture(); in.firmware.firmwareReservedKiB = 0;
    rejects(in, VramReservationStatus::UnsupportedZeroReservation);
    in.firmware.firmwareReservedKiB = UINT32_MAX; rejects(in, VramReservationStatus::OutsideVram);
    in = fixture(); in.firmware.table = TableInfo{0x600, 172, 3, 5};
    in.firmware.protectedRegionSizePresent = true; in.firmware.protectedRegionSizeKiB = UINT32_MAX;
    rejects(in, VramReservationStatus::OutsideVram);
    for (unsigned which = 0; which < 11; ++which) {
        in = fixture();
        switch (which) {
        case 0: in.discovery.vramBytes--; break;
        case 1: in.discovery.selectorWritesAttempted = false; break;
        case 2: in.discovery.readyPolls = 0; break;
        case 3: in.discovery.readyPolls = kDeviceDiscoveryMaxReadyPolls + 1; break;
        case 4: in.discovery.bytesCopied--; break;
        case 5: in.discovery.binarySize = in.discovery.bytesCopied = 11; break;
        case 6: in.discovery.binarySize = in.discovery.bytesCopied = 65536; break;
        case 7: in.discovery.regionSize = 4; break;
        case 8: in.discovery.sourceOffset++; break;
        case 9: in.discovery.regionSize++; break;
        case 10: in.discovery.sourceOffset += 4096; break;
        }
        rejects(in, VramReservationStatus::InvalidDiscovery);
    }
    in = fixture(); in.discovery.usedScratch = true; in.discovery.sourceOffset = 24 * GiB - 4096;
    rejects(in, VramReservationStatus::OutsideVram);
    in.discovery.sourceOffset = UINT64_MAX - 3; rejects(in, VramReservationStatus::OutsideVram);
    in.discovery.sourceOffset = 4096; VramReservationPlan p{};
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.ranges[0].offset == 4096 && p.ranges[0].bytes == 12288);
}
void rangeFailuresAndCapacity() {
    auto in = fixture(); VramRange range{0, 0};
    in.preOsFramebuffers = &range; in.preOsFramebufferCount = 1;
    rejects(in, VramReservationStatus::OutsideVram);
    range = {24 * GiB, 1}; rejects(in, VramReservationStatus::OutsideVram);
    range = {24 * GiB - 1, 2}; rejects(in, VramReservationStatus::OutsideVram);
    range = {UINT64_MAX, 1}; rejects(in, VramReservationStatus::OutsideVram);
    range = {1, UINT64_MAX}; rejects(in, VramReservationStatus::OutsideVram);
    in.preOsFramebuffers = nullptr; rejects(in, VramReservationStatus::InvalidArgument);
    in = fixture(); in.additionalReservationCount = 1; rejects(in, VramReservationStatus::InvalidArgument);
    in = fixture(); in.firmwareEvidence = static_cast<VramReservationEvidence>(99); rejects(in, VramReservationStatus::InvalidArgument);
    in = fixture(); in.memory.vramEndInclusive++; rejects(in, VramReservationStatus::InvalidMemoryLayout);
    in = fixture(); in.memory.vramPageBaseOffset = UINT64_MAX; rejects(in, VramReservationStatus::InvalidMemoryLayout);
    in = fixture(); in.memory.vramBytes = 0; rejects(in, VramReservationStatus::InvalidMemoryLayout);
    VramRange ranges[33] = {};
    for (size_t i = 0; i < 33; ++i) ranges[i] = {i * 8192, 4096};
    in = fixture(); in.additionalReservations = ranges; in.additionalReservationCount = 33;
    rejects(in, VramReservationStatus::CapacityExceeded);
    in.additionalReservationCount = 32; rejects(in, VramReservationStatus::CapacityExceeded); // plus separate FW tail
    in.additionalReservationCount = 31; VramReservationPlan p{};
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.count == 32);
    in.additionalReservationCount = 32;
    range = {0, 32 * 8192}; in.preOsFramebuffers = &range; in.preOsFramebufferCount = 1;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.count == 2 && p.ranges[0].bytes == 32 * 8192);
    // The maximum input counts can all overlap; final capacity applies after union.
    for (auto& r : ranges) r = {1, 1};
    in.preOsFramebuffers = ranges; in.preOsFramebufferCount = 32;
    CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok && p.count == 2 && p.ranges[0].bytes == 4096);
}
void randomizedUnion() {
    const uint64_t vramBytes = 16 * MiB;
    auto in = fixture(vramBytes, 4);
    in.discovery.usedScratch = true; in.discovery.sourceOffset = vramBytes - 4096; in.discovery.regionSize = 4096;
    VramRange scanout[8], additional[8];
    in.preOsFramebuffers = scanout; in.additionalReservations = additional;
    uint32_t rng = 0x744c7900;
    for (unsigned trial = 0; trial < 1000; ++trial) {
        std::vector<bool> expected(static_cast<size_t>(vramBytes / 4096), false);
        expected.back() = true;
        rng = rng * 1664525u + 1013904223u; in.preOsFramebufferCount = rng % 9;
        rng = rng * 1664525u + 1013904223u; in.additionalReservationCount = rng % 9;
        for (unsigned category = 0; category < 2; ++category) {
            const size_t count = category ? in.additionalReservationCount : in.preOsFramebufferCount;
            VramRange* ranges = category ? additional : scanout;
            for (size_t i = 0; i < count; ++i) {
                rng = rng * 1664525u + 1013904223u; const uint64_t begin = rng % (vramBytes - 65536);
                rng = rng * 1664525u + 1013904223u; const uint64_t bytes = 1 + rng % 65536;
                ranges[i] = {begin, bytes};
                for (uint64_t page = begin / 4096; page <= (begin + bytes - 1) / 4096; ++page)
                    expected[static_cast<size_t>(page)] = true;
            }
        }
        VramReservationPlan p{};
        CHECK(buildVramReservations(in, p) == VramReservationStatus::Ok);
        uint64_t total = 0, previousEnd = 0;
        std::vector<bool> actual(expected.size(), false);
        for (size_t i = 0; i < p.count; ++i) {
            CHECK(p.ranges[i].bytes && !(p.ranges[i].offset & 4095) && !(p.ranges[i].bytes & 4095));
            CHECK(!i || previousEnd < p.ranges[i].offset); // touching ranges must have merged
            previousEnd = p.ranges[i].offset + p.ranges[i].bytes;
            CHECK(previousEnd <= vramBytes);
            total += p.ranges[i].bytes;
            for (uint64_t page = p.ranges[i].offset / 4096; page < previousEnd / 4096; ++page) {
                CHECK(!actual[static_cast<size_t>(page)]);
                actual[static_cast<size_t>(page)] = true;
            }
        }
        CHECK(actual == expected && p.reservedBytes == total && p.complete);
        VramAllocator allocator;
        CHECK(allocator.configure(in.memory, {0, vramBytes}, p.ranges, p.count) == VramAllocatorStatus::Ok);
        CHECK(allocator.freeBytes() + p.reservedBytes == vramBytes);
    }
}
} // namespace
int main() {
    basics(); usage(); training(); missingEvidence(); invalidMetadata(); rangeFailuresAndCapacity(); randomizedUnion();
    std::cout << checks << " VRAM reservation assertions, " << failures << " failures; synthetic evidence only.\n";
    return failures ? 1 : 0;
}
