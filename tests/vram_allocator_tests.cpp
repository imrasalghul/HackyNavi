// SPDX-License-Identifier: MIT
// Synthetic ownership declarations, never a claim about real free VRAM.
#include "hackynavi/vram_allocator.hpp"
#include <iostream>
#include <vector>

using namespace hackynavi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "line " << __LINE__ << ": " #x "\n"; } } while (0)
constexpr uint64_t page = 4096;
MemoryLayout layout() {
    const MemoryLayoutInput input = {0x8000, 0x85ff, 0x2000,
        UINT64_C(24) << 30, UINT64_C(1) << 32, UINT64_C(256) << 20};
    MemoryLayout result{};
    CHECK(deriveMemoryLayout(input, result) == MemoryLayoutStatus::Ok);
    return result;
}
bool empty(const VramAllocation& a) {
    return !a.handle.owner && !a.handle.serial && !a.handle.slot && !a.range.offset && !a.range.bytes;
}

void declarations() {
    VramAllocator allocator;
    VramAllocation output{}; VramRange range{};
    CHECK(!allocator.isConfigured() && allocator.freeBytes() == 0 && allocator.liveAllocations() == 0);
    CHECK(allocator.allocate(page, page, output) == VramAllocatorStatus::NotConfigured);
    CHECK(allocator.release({}) == VramAllocatorStatus::NotConfigured);
    CHECK(allocator.lookup({}, range) == VramAllocatorStatus::NotConfigured);
    const MemoryLayout memory = layout();
    const VramRange arena = {3 * page, 100 * page};
    const VramRange good[] = {{20 * page, 3 * page}, {3 * page, 2 * page}, {5 * page, page}};
    CHECK(allocator.configure(memory, arena, good, 3) == VramAllocatorStatus::Ok);
    CHECK(allocator.freeBytes() == 94 * page);
    const uint64_t before = allocator.freeBytes();
    CHECK(allocator.configure(memory, arena, nullptr, 1) == VramAllocatorStatus::InvalidArgument);
    CHECK(allocator.configure(memory, arena, nullptr, 33) == VramAllocatorStatus::ReservationCapacityExceeded);
    CHECK(allocator.configure(memory, {0, 0}, nullptr, 0) == VramAllocatorStatus::InvalidArgument);
    CHECK(allocator.configure(memory, {1, page}, nullptr, 0) == VramAllocatorStatus::InvalidAlignment);
    CHECK(allocator.configure(memory, {0, page + 1}, nullptr, 0) == VramAllocatorStatus::InvalidAlignment);
    CHECK(allocator.configure(memory, {UINT64_MAX - page + 1, page}, nullptr, 0) == VramAllocatorStatus::InvalidMemoryLayout);
    CHECK(allocator.configure(memory, {memory.vramBytes, page}, nullptr, 0) == VramAllocatorStatus::InvalidMemoryLayout);
    MemoryLayout invalid = memory; invalid.vramPageBaseOffset = UINT64_MAX;
    CHECK(allocator.configure(invalid, arena, nullptr, 0) == VramAllocatorStatus::InvalidMemoryLayout);
    const VramRange malformed[] = {{3 * page, 0}, {3 * page + 1, page}, {3 * page, page - 1},
        {0, page}, {103 * page, page}, {102 * page, 2 * page}, {UINT64_MAX - page + 1, page}};
    const VramAllocatorStatus expected[] = {VramAllocatorStatus::InvalidArgument,
        VramAllocatorStatus::InvalidAlignment, VramAllocatorStatus::InvalidAlignment,
        VramAllocatorStatus::OutsideArena, VramAllocatorStatus::OutsideArena,
        VramAllocatorStatus::OutsideArena, VramAllocatorStatus::OutsideArena};
    for (size_t i = 0; i < 7; ++i) {
        CHECK(allocator.configure(memory, arena, &malformed[i], 1) == expected[i]);
        CHECK(allocator.isConfigured() && allocator.freeBytes() == before);
    }
    const VramRange duplicate[] = {{3 * page, page}, {3 * page, page}};
    CHECK(allocator.configure(memory, arena, duplicate, 2) == VramAllocatorStatus::OverlappingReservation);
    const VramRange overlap[] = {{3 * page, 2 * page}, {4 * page, 2 * page}};
    CHECK(allocator.configure(memory, arena, overlap, 2) == VramAllocatorStatus::OverlappingReservation);
    CHECK(allocator.freeBytes() == before);
    CHECK(allocator.allocate(page, page, output) == VramAllocatorStatus::Ok);
    CHECK(output.range.offset == 6 * page && output.range.bytes == page);
    CHECK(allocator.configure(memory, arena, nullptr, 0) == VramAllocatorStatus::Busy);
    CHECK(allocator.liveAllocations() == 1 && allocator.freeBytes() == before - page);
    CHECK(allocator.release(output.handle) == VramAllocatorStatus::Ok);
    CHECK(allocator.configure(memory, arena, &arena, 1) == VramAllocatorStatus::Ok);
    CHECK(allocator.freeBytes() == 0);
    CHECK(allocator.allocate(page, page, output) == VramAllocatorStatus::OutOfSpace && empty(output));
}

void alignmentAndHandles() {
    VramAllocator allocator, other;
    const MemoryLayout memory = layout();
    CHECK(allocator.configure(memory, {3 * page, 61 * page}, nullptr, 0) == VramAllocatorStatus::Ok);
    CHECK(other.configure(memory, {0, 64 * page}, nullptr, 0) == VramAllocatorStatus::Ok);
    VramAllocation a{}, b{};
    CHECK(allocator.allocate(2 * page, 8 * page, a) == VramAllocatorStatus::Ok);
    CHECK(a.range.offset == 8 * page);
    CHECK(allocator.allocate(4 * page, page, b) == VramAllocatorStatus::Ok);
    CHECK(b.range.offset == 3 * page); // First fit reuses the alignment gap.
    VramRange found{};
    CHECK(allocator.lookup(a.handle, found) == VramAllocatorStatus::Ok);
    CHECK(found.offset == a.range.offset && found.bytes == a.range.bytes);
    CHECK(other.release(a.handle) == VramAllocatorStatus::InvalidHandle);
    VramAllocationHandle corrupted = a.handle; ++corrupted.serial;
    CHECK(allocator.release(corrupted) == VramAllocatorStatus::InvalidHandle);
    corrupted = a.handle; corrupted.slot = UINT32_MAX;
    CHECK(allocator.release(corrupted) == VramAllocatorStatus::InvalidHandle);
    corrupted = a.handle; corrupted.serial = 0;
    CHECK(allocator.release(corrupted) == VramAllocatorStatus::InvalidHandle);
    corrupted = a.handle; corrupted.owner = nullptr;
    CHECK(allocator.release(corrupted) == VramAllocatorStatus::InvalidHandle);
    uint64_t mc = 123, physical = 456;
    CHECK(allocator.addresses(a.handle, mc, physical) == VramAllocatorStatus::Ok);
    CHECK(mc == memory.vramStart + a.range.offset);
    CHECK(physical == memory.vramPageBaseOffset + a.range.offset && mc != physical);
    CHECK(allocator.addresses(a.handle, mc, mc) == VramAllocatorStatus::InvalidArgument && mc == 0);
    const VramAllocationHandle old = a.handle;
    CHECK(allocator.release(old) == VramAllocatorStatus::Ok);
    CHECK(allocator.release(old) == VramAllocatorStatus::InvalidHandle);
    CHECK(allocator.lookup(old, found) == VramAllocatorStatus::InvalidHandle && !found.offset && !found.bytes);
    CHECK(allocator.addresses(old, mc, physical) == VramAllocatorStatus::InvalidHandle && !mc && !physical);
    CHECK(allocator.allocate(2 * page, 8 * page, a) == VramAllocatorStatus::Ok);
    CHECK(a.handle.slot == old.slot && a.handle.serial != old.serial);
    CHECK(allocator.release(old) == VramAllocatorStatus::InvalidHandle);
    CHECK(allocator.release(a.handle) == VramAllocatorStatus::Ok);
    CHECK(allocator.release(b.handle) == VramAllocatorStatus::Ok);
    CHECK(allocator.configure(memory, {0, 64 * page}, nullptr, 0) == VramAllocatorStatus::Ok);
    CHECK(allocator.allocate(page, page, a) == VramAllocatorStatus::Ok);
    CHECK(a.handle.serial > old.serial);
    CHECK(allocator.release(old) == VramAllocatorStatus::InvalidHandle);
    CHECK(allocator.release(a.handle) == VramAllocatorStatus::Ok);
    const uint64_t invalidAlignments[] = {0, 1, page - 1, page + 1, 3 * page, UINT64_MAX};
    const uint64_t before = allocator.freeBytes();
    for (uint64_t alignment : invalidAlignments) {
        CHECK(allocator.allocate(page, alignment, a) == VramAllocatorStatus::InvalidAlignment);
        CHECK(empty(a) && allocator.freeBytes() == before);
    }
    CHECK(allocator.allocate(0, page, a) == VramAllocatorStatus::InvalidArgument);
    CHECK(allocator.allocate(page - 1, page, a) == VramAllocatorStatus::InvalidAlignment);
    CHECK(allocator.allocate(UINT64_MAX - page + 1, page, a) == VramAllocatorStatus::OutOfSpace);
    // Huge power-of-two alignment is valid at zero, cannot fit after page zero.
    CHECK(allocator.allocate(page, UINT64_C(1) << 63, a) == VramAllocatorStatus::Ok);
    CHECK(a.range.offset == 0);
    CHECK(allocator.allocate(page, UINT64_C(1) << 63, b) == VramAllocatorStatus::OutOfSpace);
    CHECK(empty(b));
}

void boundedCapacity() {
    const MemoryLayout memory = layout();
    VramAllocator allocator;
    VramRange reserved[kVramMaxReservations];
    for (size_t i = 0; i < kVramMaxReservations; ++i) reserved[i] = {2 * i * page, page};
    CHECK(allocator.configure(memory, {0, 256 * page}, reserved, kVramMaxReservations) == VramAllocatorStatus::Ok);
    VramAllocation allocations[kVramMaxAllocations]{};
    for (size_t i = 0; i < kVramMaxAllocations; ++i) {
        CHECK(allocator.allocate(page, page, allocations[i]) == VramAllocatorStatus::Ok);
        CHECK(allocations[i].range.offset == (i < 32 ? 2 * i + 1 : i + 32) * page);
    }
    CHECK(allocator.liveAllocations() == 64 && allocator.freeBytes() == 160 * page);
    VramAllocation extra{};
    CHECK(allocator.allocate(page, page, extra) == VramAllocatorStatus::AllocationCapacityExceeded);
    CHECK(empty(extra));
    CHECK(allocator.release(allocations[17].handle) == VramAllocatorStatus::Ok);
    CHECK(allocator.allocate(page, page, extra) == VramAllocatorStatus::Ok);
    CHECK(extra.range.offset == allocations[17].range.offset && extra.handle.serial != allocations[17].handle.serial);
    CHECK(allocator.release(allocations[17].handle) == VramAllocatorStatus::InvalidHandle);
    // A high, explicitly declared arena never truncates to a 32-bit offset.
    VramAllocator high;
    CHECK(high.configure(memory, {memory.vramBytes - 4 * page, 4 * page}, nullptr, 0) == VramAllocatorStatus::Ok);
    CHECK(high.allocate(4 * page, page, extra) == VramAllocatorStatus::Ok);
    CHECK(extra.range.offset == memory.vramBytes - 4 * page);
    uint64_t mc, physical;
    CHECK(high.addresses(extra.handle, mc, physical) == VramAllocatorStatus::Ok);
    CHECK(mc + extra.range.bytes - 1 == memory.vramEndInclusive);
    CHECK(physical == memory.vramPageBaseOffset + extra.range.offset);
}

void modelComparison() {
    const MemoryLayout memory = layout();
    VramAllocator allocator;
    const size_t pages = 256, firstPage = 3;
    const VramRange reservations[] = {{(firstPage + 11) * page, 3 * page},
                                      {(firstPage + 37) * page, 5 * page},
                                      {(firstPage + 190) * page, 10 * page}};
    CHECK(allocator.configure(memory, {firstPage * page, pages * page}, reservations, 3) == VramAllocatorStatus::Ok);
    bool used[pages] = {};
    for (const VramRange& range : reservations)
        for (uint64_t p = range.offset / page; p < (range.offset + range.bytes) / page; ++p)
            used[p - firstPage] = true;
    std::vector<VramAllocation> live;
    uint32_t random = 0x31a110cu;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        random = random * 1664525u + 1013904223u;
        if (!live.empty() && (random & 3u) == 0) {
            const size_t index = (random >> 3) % live.size();
            const VramAllocation released = live[index];
            CHECK(allocator.release(released.handle) == VramAllocatorStatus::Ok);
            for (uint64_t p = released.range.offset / page;
                 p < (released.range.offset + released.range.bytes) / page; ++p) used[p - firstPage] = false;
            CHECK(allocator.release(released.handle) == VramAllocatorStatus::InvalidHandle);
            live.erase(live.begin() + index);
        } else {
            const size_t count = 1 + ((random >> 8) & 7u);
            const size_t alignment = size_t(1) << ((random >> 16) % 5u);
            size_t expected = pages;
            for (size_t start = 0; start + count <= pages; ++start) {
                if ((start + firstPage) % alignment) continue;
                bool fits = true;
                for (size_t i = 0; i < count; ++i) if (used[start + i]) fits = false;
                if (fits) { expected = start; break; }
            }
            VramAllocation allocated{};
            const auto result = allocator.allocate(count * page, alignment * page, allocated);
            if (live.size() == kVramMaxAllocations) {
                CHECK(result == VramAllocatorStatus::AllocationCapacityExceeded && empty(allocated));
            } else if (expected == pages) {
                CHECK(result == VramAllocatorStatus::OutOfSpace && empty(allocated));
            } else {
                CHECK(result == VramAllocatorStatus::Ok);
                CHECK(allocated.range.offset == (expected + firstPage) * page && allocated.range.bytes == count * page);
                if (result == VramAllocatorStatus::Ok) {
                    for (size_t i = 0; i < count; ++i) used[expected + i] = true;
                    live.push_back(allocated);
                }
            }
        }
        size_t freePages = 0;
        for (bool busy : used) if (!busy) ++freePages;
        CHECK(allocator.freeBytes() == freePages * page);
        CHECK(allocator.liveAllocations() == live.size());
    }
    for (const VramAllocation& allocation : live) CHECK(allocator.release(allocation.handle) == VramAllocatorStatus::Ok);
    CHECK(allocator.freeBytes() == (pages - 18) * page && allocator.liveAllocations() == 0);
}
} // namespace

int main() {
    declarations(); alignmentAndHandles(); boundedCapacity(); modelComparison();
    CHECK(vramAllocatorStatusString(VramAllocatorStatus::Ok)[0] == 'o');
    std::cout << checks << " VRAM allocator assertions, " << failures << " failures; synthetic arenas only.\n";
    return failures ? 1 : 0;
}
