// SPDX-License-Identifier: MIT
// CPU construction of Navi31/GMC11 page-table bytes. No GPU or MMIO access.
#ifndef HACKYNAVI_GPU_VM_HPP
#define HACKYNAVI_GPU_VM_HPP

#include "hackynavi/rom.hpp"

namespace hackynavi {
namespace gmc11 {

constexpr size_t kEntryBytes = 8;
constexpr uint64_t kPageBytes = 4096;
constexpr uint64_t kPteAddressMask = 0x0000fffffffff000ULL;
constexpr uint64_t kPdeAddressMask = 0x0000ffffffffffc0ULL;
constexpr uint64_t kVramAddressLimit = 0x0001000000000000ULL; // Exclusive: 48 bits.
constexpr uint64_t kSystemDmaAddressLimit = 0x0000100000000000ULL; // 44 bits.

enum class MemoryDomain { VramPhysical, SystemDma };
enum class MemoryType { NonCached = 0, WriteCombined = 1, CacheCoherent = 2,
                        Uncached = 3 };
enum class DirectoryLevel { Pdb3, Pdb2, Pdb1, Pdb0 };

struct TableBuffer {
    uint8_t* bytes;
    size_t byteCapacity; // Writable CPU bytes, not a GPU address or entry count.
};

struct LeafAttributes {
    MemoryDomain domain;
    MemoryType memoryType;
    bool snooped;
    bool readable;
    bool writable;
    bool executable;
    bool noAllocate; // GMC11 MALL no-allocate, bit 58; not the GFX12 DCC bit.
};

struct DirectoryAttributes {
    MemoryDomain domain;
    bool snooped;
    DirectoryLevel parentLevel; // Level containing this pointer, not its target.
    bool translateFurther; // Must match the actual GMC/VM configuration.
};

// All output is explicitly little endian and may be CPU-byte-unaligned. These
// routines allocate nothing and validate the entire operation before writing;
// any failure leaves all output bytes unchanged. Null pointers, zero pageCount,
// invalid enums, alignment/range/overlap violations return InvalidArgument.
// Insufficient byteCapacity/entry range returns CapacityExceeded. Trailing
// capacity bytes that do not make up a complete entry are never written.
// The caller owns the stated allocations, keeps them stable and exclusively
// writable for the call, and must not race readers or writers of these bytes.
//
// VramPhysical addresses are final GPU physical addresses, not framebuffer/MC
// offsets. In particular Linux converts a VRAM PDE pointer using
//     physical = mcAddress - vramStart + vramBaseOffset.
// That device-specific translation belongs to the caller. SystemDma addresses
// must come from a prepared device DMA mapping, never an assumed CPU physical
// identity mapping. This encoder enforces Linux GMC11's 44-bit DMA limit in
// addition to the hardware's 48-bit field. Neither limit proves residency or
// ownership. memoryType is the effective policy: Linux forces Uncached for
// COHERENT, EXT_COHERENT or UNCACHED BOs, regardless of their requested type.
//
// Only valid ordinary 4 KiB leaf entries (fragment=0) and directory pointers
// are emitted. No PRT, encryption, large-page/PDE-as-PTE, invalid-entry/fault
// policy, page-table allocation, virtual address tree, upload, cache flush,
// TLB invalidation, synchronization or VMID programming is implemented here.

// Physical pages are contiguous; pageCount counts 4 KiB pages, not bytes.
// firstAddress must be 4 KiB aligned and the whole range must fit its domain.
Status writeContiguousPtes(TableBuffer table, size_t firstEntry,
                           uint64_t firstAddress, size_t pageCount,
                           LeafAttributes attributes);

// Each native uint64_t input is one 4 KiB-aligned device address. The array
// must be uint64_t-aligned, remain stable during the call and not overlap the
// written output region. The full array is checked before the first write.
Status writeScatterPtes(TableBuffer table, size_t firstEntry,
                        const uint64_t* addresses, size_t pageCount,
                        LeafAttributes attributes);

// Linux amdgpu_gart_unbind's empty-slot representation: repeat the owned dummy
// DMA page address with ALL flags zero, including VALID and SYSTEM. SYSTEM must
// remain clear for invalid entries on Vega10 and later. This does not grant a
// mapping to that page. The caller keeps the prepared dummy mapping alive.
// Checks 4 KiB alignment/44-bit DMA range and validates the whole output range
// before changing any bytes. Other failure/ownership rules above also apply.
Status writeInvalidGartPtes(TableBuffer table, size_t firstEntry,
                            uint64_t dummyDmaAddress, size_t pageCount);

// Encodes one pointer to a lower table. Its hardware address field permits
// 64-byte alignment; this does not relax the 4 KiB allocation/size requirements
// of an actual Linux-style table tree. With translateFurther enabled, a Pdb1
// pointer gains BFS=9 and a Pdb0 pointer gains TF, as gmc_v11_0_get_vm_pde does.
// Pdb2/Pdb3 retain ordinary directory flags in both modes.
Status writeDirectoryPointer(TableBuffer table, size_t entry,
                             uint64_t tableAddress,
                             DirectoryAttributes attributes);

} // namespace gmc11
} // namespace hackynavi

#endif // HACKYNAVI_GPU_VM_HPP
