// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_VRAM_RESERVATIONS_HPP
#define HACKYNAVI_VRAM_RESERVATIONS_HPP
#include "hackynavi/rom.hpp"
#include "hackynavi/discovery.hpp"
#include "hackynavi/device_discovery.hpp"
#include "hackynavi/vram_allocator.hpp"

namespace hackynavi {
enum : size_t { kVramReservationInputRanges = 32, kVramReservationPlanRanges = 32 };
enum class VramReservationEvidence { Unavailable, RomImage, Runtime };
enum class VramReservationStatus {
    Ok, MissingEvidence, InvalidArgument, InvalidMemoryLayout, MetadataRejected,
    UnsupportedZeroReservation, UnsupportedUsageFlags, InvalidDiscovery,
    OutsideVram, CapacityExceeded
};
enum VramReservationProvenance : uint32_t {
    ReservationFirmwareTail = 1u << 0, ReservationProtectedTail = 1u << 1,
    ReservationFirmwareUsage = 1u << 2, ReservationDriverUsage = 1u << 3,
    ReservationPreOsFramebuffer = 1u << 4, ReservationDiscovery = 1u << 5,
    ReservationTrainingHost = 1u << 6, ReservationTrainingFirmware = 1u << 7,
    ReservationAdditional = 1u << 8, ReservationFromRom = 1u << 9,
    ReservationFromRuntime = 1u << 10
};
enum VramReservationMissing : uint32_t {
    MissingFirmwareInfo = 1u << 0, MissingVramUsage = 1u << 1,
    MissingPreOsFramebufferInventory = 1u << 2, MissingLiveDiscovery = 1u << 3,
    MissingAdditionalReservationInventory = 1u << 4,
    MissingInactiveMemoryTraining = 1u << 5
};
struct VramReservationInput {
    MemoryLayout memory;
    Status firmwareStatus;
    FirmwareInfo firmware;
    VramReservationEvidence firmwareEvidence;
    Status usageStatus;
    VramUsageInfo usage;
    VramReservationEvidence usageEvidence;
    // Live VRAM-relative byte intervals, including every active/pending plane,
    // cursor or VGA allocation. Zero count is meaningful only if complete.
    const VramRange* preOsFramebuffers;
    size_t preOsFramebufferCount;
    bool preOsFramebufferInventoryComplete;
    DeviceDiscoveryStatus discoveryCaptureStatus;
    DiscoveryStatus discoveryBinaryStatus;
    DeviceDiscoveryResult discovery;
    // Other known live intervals (PSP/TMR, prior queues, staging, etc.). The
    // caller's inventory assertion is separate from exclusive device ownership.
    const VramRange* additionalReservations;
    size_t additionalReservationCount;
    bool additionalReservationInventoryComplete;
    bool memoryTrainingInactive;
};
struct VramReservationPlan {
    bool complete; // All declared evidence categories supplied, NOT ownership.
    uint32_t missingEvidence;
    size_t count;
    VramRange ranges[kVramReservationPlanRanges];
    uint32_t provenance[kVramReservationPlanRanges];
    uint64_t reservedBytes;
    uint32_t driverScratchKiB; // Usage2.1 CPU scratch request unless shared-SR-IOV;
                              // zero means Linux's default, not no allocation.
};

// Bare-metal Navi31 policy, no hardware reads/writes and no allocations.
// Inputs must come from the bounded decoders/capture APIs and a coherent live
// MemoryLayout. The caller supplies provenance; this function cannot attest it.
// It validates metadata consistency and ranges, rounds outward to4KiB, then
// sorts/unions overlap AND adjacency. Merged provenance labels describe all
// contributors, not the exact original byte boundaries. At most32 final ranges
// are allowed; both external arrays are independently capped at32 entries.
//
// FirmwareInfo3.4/3.5 must report a positive firmware tail. Zero is explicitly
// unsupported here: Linux's context-dependent64KiB fallback is not assumed.
// Protected3.5 tail is also reserved. ROM declarations are accepted as
// conservative evidence, not mistaken for refreshed runtime table contents.
// Explicit usage firmware/2.2-driver ranges with flags0 or2 are protected as a
// conservative superset of Linux's bare-metal allocation policy. Flag1 declares
// no reservation; flag3 is unsupported. Usage2.1's driver field is CPU scratch
// metadata, not an inferred VRAM address. A bounded, confirmed absent usage
// table is allowed; unsupported/unavailable tables leave missing evidence.
//
// With training capability enabled, reserve Linux's4KiB host training block at
// ALIGN_UP(vramBytes-firmwareTailBytes-1MiB,1MiB), plus its4KiB firmware source at
// vramBytes-32KiB. Training must be inactive: long training temporarily overwrites
// the bottom32MiB and requires a separate save/restore protocol.
// Discovery reserves the ENTIRE captured source region, not just binary bytes.
// Failed/incomplete capture or binary validation leaves missing evidence.
//
// MissingEvidence returns a conservative partial plan (complete=false).
// All other failures zero out. Ok/complete does NOT prove exclusive ownership,
// that firmware has stopped, or that omitted memory is safe. Bootstrap/reset/
// quiescence and complete inventory remain externally established prerequisites.
// No arena or GART aperture is chosen. The ranges can be passed directly to the
// allocator for a separately owned whole-VRAM arena; a smaller arena requires
// the caller to intersect these reservations with that arena first.
// Input, pointed-to arrays and output must not alias and remain stable.
VramReservationStatus buildVramReservations(const VramReservationInput& input,
                                           VramReservationPlan& out);
const char* vramReservationStatusString(VramReservationStatus status);
} // namespace hackynavi
#endif
