// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_VRAM_ALLOCATOR_HPP
#define HACKYNAVI_VRAM_ALLOCATOR_HPP
#include <stddef.h>
#include <stdint.h>
#include "hackynavi/memory_layout.hpp"

namespace hackynavi {
enum : size_t { kVramMaxReservations = 32, kVramMaxAllocations = 64 };
constexpr uint64_t kVramAllocationPage = 4096;
enum class VramAllocatorStatus {
    Ok, NotConfigured, InvalidArgument, InvalidAlignment, InvalidMemoryLayout,
    OutsideArena, OverlappingReservation, ReservationCapacityExceeded,
    AllocationCapacityExceeded, OutOfSpace, Busy, InvalidHandle, SerialExhausted
};
struct VramRange { uint64_t offset, bytes; }; // VRAM-relative byte interval.
class VramAllocator;
struct VramAllocationHandle {
    const VramAllocator* owner;
    uint64_t serial;
    uint32_t slot;
};
struct VramAllocation { VramAllocationHandle handle; VramRange range; };

// Pure allocation bookkeeping, NEVER an ownership/occupancy detector. The
// caller must first establish exclusive ownership of an arena and identify all
// firmware, scanout, PSP, discovery, training and other live reservations.
// Passing no reservations asserts that none overlap; it does NOT verify that
// claim. This class does not infer free VRAM from BAR size or total capacity.
//
// All ranges/sizes are nonempty 4 KiB multiples. Arena/reservation offsets are
// 4 KiB aligned. The caller must conservatively round external reservations
// outward and intersect them with the arena first; partially external and
// overlapping reservation declarations are rejected, not silently clipped.
// Reservations may touch. Input ranges are copied; no caller pointer is kept.
//
// The object must stay at a stable address, not be copied, and all calls must be
// externally serialized. A handle is meaningful only during that object's
// lifetime. Freed handles cannot be reused, including after reconfiguration;
// serials never wrap. Handles are misuse guards, not security capabilities.
// Caller must prove that GPU/CPU users retired BEFORE releasing an allocation
// or destroying this object. No fences, mappings, zeroing, VRAM writes, memory
// residency, physical allocation or reservation discovery is performed here.
class VramAllocator {
public:
    VramAllocator();
    VramAllocator(const VramAllocator&) = delete;
    VramAllocator& operator=(const VramAllocator&) = delete;

    // Atomic configuration. Invalid input leaves previous configuration intact.
    // Reconfiguration is refused while any allocation is live. MemoryLayout
    // translations validate the complete arena in both GPU address domains.
    VramAllocatorStatus configure(const MemoryLayout& layout,
        VramRange callerOwnedArena, const VramRange* reservations, size_t count);
    // First-fit allocation; alignment is a power of two >=4 KiB, measured from
    // VRAM-relative address zero. Failure leaves state intact and zeroes out.
    VramAllocatorStatus allocate(uint64_t bytes, uint64_t alignment,
                                 VramAllocation& out);
    VramAllocatorStatus release(VramAllocationHandle handle);
    VramAllocatorStatus lookup(VramAllocationHandle handle, VramRange& out) const;
    // Uses existing MemoryLayout translation helpers for the whole allocation.
    // Both addresses are zero on failure; neither is a CPU physical address.
    // The two output references must be distinct.
    VramAllocatorStatus addresses(VramAllocationHandle handle,
                                  uint64_t& mc, uint64_t& physical) const;
    bool isConfigured() const { return configured_; }
    size_t liveAllocations() const { return liveCount_; }
    uint64_t freeBytes() const;

private:
    struct Slot { VramRange range; uint64_t serial; bool live; };
    bool matches(VramAllocationHandle handle) const;
    MemoryLayout layout_;
    VramRange arena_;
    VramRange reservations_[kVramMaxReservations];
    Slot slots_[kVramMaxAllocations];
    size_t reservationCount_, liveCount_;
    uint64_t reservedBytes_, allocatedBytes_, lastSerial_;
    bool configured_;
};
const char* vramAllocatorStatusString(VramAllocatorStatus status);
} // namespace hackynavi
#endif
