// SPDX-License-Identifier: MIT
// Original bounded interval allocator. Policy deliberately does not copy the
// fixed Navi48 pools in Cobalt64 gmc_v12_0.cpp:154-176,362-395 at d6c9aedf8e65.
// Reservation prerequisites, NOT universal free-arena constants, reviewed in
// Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu/amdgpu_ttm.c:1741-1808 (firmware tail, memory training, pre-OS scanout)
// amdgpu/amdgpu_gmc.c:1047-1121 and gmc_v11_0.c:515-534 (VGA/scanout)
// amdgpu/amdgpu_atomfirmware.c:1015-1051 (firmwareinfo 3.4/3.5 reservation)
// include/atomfirmware.h:742-799 (runtime pre-OS/driver region declarations)
// amdgpu/amdgpu_psp.c:891-928 (TMR allocation lifetime).
#include "hackynavi/vram_allocator.hpp"

namespace hackynavi {
namespace {
bool alignedRange(VramRange range) {
    return range.bytes != 0 && ((range.offset | range.bytes) & (kVramAllocationPage - 1)) == 0;
}
bool inside(VramRange outer, VramRange inner) {
    return inner.offset >= outer.offset && inner.offset - outer.offset <= outer.bytes &&
           inner.bytes <= outer.bytes - (inner.offset - outer.offset);
}
// Inputs here have been bounded to one validated arena; end additions cannot
// wrap and each range is nonempty. Adjacent intervals do not overlap.
bool overlap(VramRange a, VramRange b) {
    return a.offset < b.offset + b.bytes && b.offset < a.offset + a.bytes;
}
bool alignUp(uint64_t value, uint64_t alignment, uint64_t& result) {
    const uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) return false;
    result = (value + mask) & ~mask;
    return true;
}
} // namespace

VramAllocator::VramAllocator()
    : layout_{}, arena_{}, reservations_{}, slots_{}, reservationCount_(0),
      liveCount_(0), reservedBytes_(0), allocatedBytes_(0), lastSerial_(0),
      configured_(false) {}

VramAllocatorStatus VramAllocator::configure(const MemoryLayout& layout,
    VramRange callerOwnedArena, const VramRange* reservations, size_t count) {
    if (liveCount_) return VramAllocatorStatus::Busy;
    if (count > kVramMaxReservations) return VramAllocatorStatus::ReservationCapacityExceeded;
    if (count && !reservations) return VramAllocatorStatus::InvalidArgument;
    if (!callerOwnedArena.bytes) return VramAllocatorStatus::InvalidArgument;
    if (!alignedRange(callerOwnedArena)) return VramAllocatorStatus::InvalidAlignment;
    uint64_t address = 0;
    if (vramMcAddress(layout, callerOwnedArena.offset, callerOwnedArena.bytes, address) != MemoryLayoutStatus::Ok ||
        vramPhysicalAddress(layout, callerOwnedArena.offset, callerOwnedArena.bytes, address) != MemoryLayoutStatus::Ok)
        return VramAllocatorStatus::InvalidMemoryLayout;
    // Copy/validate before commit, also safe when the source is a separate
    // caller object that will be destroyed immediately after configuration.
    VramRange checked[kVramMaxReservations] = {};
    uint64_t reservedBytes = 0;
    for (size_t i = 0; i < count; ++i) {
        const VramRange range = reservations[i];
        if (!range.bytes) return VramAllocatorStatus::InvalidArgument;
        if (!alignedRange(range)) return VramAllocatorStatus::InvalidAlignment;
        if (!inside(callerOwnedArena, range)) return VramAllocatorStatus::OutsideArena;
        for (size_t j = 0; j < i; ++j)
            if (overlap(range, checked[j])) return VramAllocatorStatus::OverlappingReservation;
        checked[i] = range;
        reservedBytes += range.bytes; // Disjoint subsets: sum <= arena bytes.
    }
    layout_ = layout;
    arena_ = callerOwnedArena;
    for (size_t i = 0; i < kVramMaxReservations; ++i) reservations_[i] = checked[i];
    for (size_t i = 0; i < kVramMaxAllocations; ++i) slots_[i] = Slot{};
    reservationCount_ = count;
    reservedBytes_ = reservedBytes;
    allocatedBytes_ = 0;
    configured_ = true;
    // Deliberately preserve lastSerial_ across every successful reconfigure.
    return VramAllocatorStatus::Ok;
}

VramAllocatorStatus VramAllocator::allocate(uint64_t bytes, uint64_t alignment,
                                            VramAllocation& out) {
    out = VramAllocation{};
    if (!configured_) return VramAllocatorStatus::NotConfigured;
    if (!bytes) return VramAllocatorStatus::InvalidArgument;
    if ((bytes & (kVramAllocationPage - 1)) || alignment < kVramAllocationPage ||
        (alignment & (alignment - 1))) return VramAllocatorStatus::InvalidAlignment;
    if (liveCount_ == kVramMaxAllocations) return VramAllocatorStatus::AllocationCapacityExceeded;
    if (lastSerial_ == UINT64_MAX) return VramAllocatorStatus::SerialExhausted;
    if (bytes > freeBytes()) return VramAllocatorStatus::OutOfSpace;
    uint64_t cursor = 0;
    if (!alignUp(arena_.offset, alignment, cursor)) return VramAllocatorStatus::OutOfSpace;
    const uint64_t end = arena_.offset + arena_.bytes;
    // Each collision moves past at least one blocker. No loop depends on VRAM
    // byte size, so even highly fragmented input has a fixed small work bound.
    for (size_t attempt = 0; attempt <= reservationCount_ + liveCount_; ++attempt) {
        if (cursor > end || bytes > end - cursor) return VramAllocatorStatus::OutOfSpace;
        const VramRange candidate = {cursor, bytes};
        uint64_t next = cursor;
        for (size_t i = 0; i < reservationCount_; ++i)
            if (overlap(candidate, reservations_[i]) && reservations_[i].offset + reservations_[i].bytes > next)
                next = reservations_[i].offset + reservations_[i].bytes;
        for (size_t i = 0; i < kVramMaxAllocations; ++i)
            if (slots_[i].live && overlap(candidate, slots_[i].range) &&
                slots_[i].range.offset + slots_[i].range.bytes > next)
                next = slots_[i].range.offset + slots_[i].range.bytes;
        if (next == cursor) {
            size_t slot = 0;
            while (slot < kVramMaxAllocations && slots_[slot].live) ++slot;
            if (slot == kVramMaxAllocations) return VramAllocatorStatus::AllocationCapacityExceeded;
            Slot& allocated = slots_[slot];
            allocated.range = candidate;
            allocated.serial = ++lastSerial_;
            allocated.live = true;
            ++liveCount_;
            allocatedBytes_ += bytes;
            out.handle = VramAllocationHandle{this, allocated.serial, static_cast<uint32_t>(slot)};
            out.range = candidate;
            return VramAllocatorStatus::Ok;
        }
        if (!alignUp(next, alignment, cursor)) return VramAllocatorStatus::OutOfSpace;
    }
    return VramAllocatorStatus::OutOfSpace;
}

bool VramAllocator::matches(VramAllocationHandle handle) const {
    return configured_ && handle.owner == this && handle.serial != 0 &&
        handle.slot < kVramMaxAllocations && slots_[handle.slot].live &&
        slots_[handle.slot].serial == handle.serial;
}

VramAllocatorStatus VramAllocator::release(VramAllocationHandle handle) {
    if (!configured_) return VramAllocatorStatus::NotConfigured;
    if (!matches(handle)) return VramAllocatorStatus::InvalidHandle;
    Slot& slot = slots_[handle.slot];
    allocatedBytes_ -= slot.range.bytes;
    --liveCount_;
    slot = Slot{};
    return VramAllocatorStatus::Ok;
}

VramAllocatorStatus VramAllocator::lookup(VramAllocationHandle handle, VramRange& out) const {
    out = VramRange{};
    if (!configured_) return VramAllocatorStatus::NotConfigured;
    if (!matches(handle)) return VramAllocatorStatus::InvalidHandle;
    out = slots_[handle.slot].range;
    return VramAllocatorStatus::Ok;
}

VramAllocatorStatus VramAllocator::addresses(VramAllocationHandle handle,
                                             uint64_t& mc, uint64_t& physical) const {
    mc = 0; physical = 0;
    if (&mc == &physical) return VramAllocatorStatus::InvalidArgument;
    VramRange range{};
    const VramAllocatorStatus status = lookup(handle, range);
    if (status != VramAllocatorStatus::Ok) return status;
    uint64_t translatedMc = 0, translatedPhysical = 0;
    if (vramMcAddress(layout_, range.offset, range.bytes, translatedMc) != MemoryLayoutStatus::Ok ||
        vramPhysicalAddress(layout_, range.offset, range.bytes, translatedPhysical) != MemoryLayoutStatus::Ok)
        return VramAllocatorStatus::InvalidMemoryLayout;
    mc = translatedMc; physical = translatedPhysical;
    return VramAllocatorStatus::Ok;
}

uint64_t VramAllocator::freeBytes() const {
    return configured_ ? arena_.bytes - reservedBytes_ - allocatedBytes_ : 0;
}

const char* vramAllocatorStatusString(VramAllocatorStatus status) {
    switch (status) {
    case VramAllocatorStatus::Ok: return "ok";
    case VramAllocatorStatus::NotConfigured: return "owned arena not configured";
    case VramAllocatorStatus::InvalidArgument: return "invalid allocator argument";
    case VramAllocatorStatus::InvalidAlignment: return "invalid page size or alignment";
    case VramAllocatorStatus::InvalidMemoryLayout: return "arena does not fit validated GPU address domains";
    case VramAllocatorStatus::OutsideArena: return "reservation lies outside owned arena";
    case VramAllocatorStatus::OverlappingReservation: return "reservation declarations overlap";
    case VramAllocatorStatus::ReservationCapacityExceeded: return "reservation capacity exceeded";
    case VramAllocatorStatus::AllocationCapacityExceeded: return "allocation slot capacity exceeded";
    case VramAllocatorStatus::OutOfSpace: return "no suitably aligned free interval";
    case VramAllocatorStatus::Busy: return "live allocations prevent reconfiguration";
    case VramAllocatorStatus::InvalidHandle: return "wrong-owner, stale or invalid allocation handle";
    case VramAllocatorStatus::SerialExhausted: return "allocation serial exhausted";
    }
    return "unknown VRAM allocator status";
}
} // namespace hackynavi
