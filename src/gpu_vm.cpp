// SPDX-License-Identifier: MIT
/*
 * GMC11 flag definitions and translate-further rules adapted from Linux
 * amdgpu_vm.h and gmc_v11_0.c at
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0.
 * Copyright 2016, 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "hackynavi/gpu_vm.hpp"

namespace hackynavi {
namespace gmc11 {
namespace {

// amdgpu_vm.h:57-87,114-118; navi10_enum.h MTYPE_NC/WC/CC/UC=0/1/2/3.
constexpr uint64_t kValid = 1ULL << 0;
constexpr uint64_t kSystem = 1ULL << 1;
constexpr uint64_t kSnooped = 1ULL << 2;
constexpr uint64_t kExecutable = 1ULL << 4;
constexpr uint64_t kReadable = 1ULL << 5;
constexpr uint64_t kWritable = 1ULL << 6;
constexpr uint64_t kTranslateFurther = 1ULL << 56;
constexpr uint64_t kNoAllocate = 1ULL << 58;
constexpr uint64_t kBlockFragmentNine = 9ULL << 59;

bool domainFlags(MemoryDomain domain, bool snooped, uint64_t& limit,
                 uint64_t& flags) {
    flags = kValid | (snooped ? kSnooped : 0);
    switch (domain) {
    case MemoryDomain::VramPhysical:
        limit = kVramAddressLimit;
        return true;
    case MemoryDomain::SystemDma:
        // gmc_v11_0.c:823-832: 48-bit PA, 44-bit DMA mask/coherent mask.
        limit = kSystemDmaAddressLimit;
        flags |= kSystem;
        return true;
    }
    return false;
}

bool leafFlags(LeafAttributes attributes, uint64_t& limit, uint64_t& flags) {
    if (!domainFlags(attributes.domain, attributes.snooped, limit, flags))
        return false;
    uint64_t memoryType;
    switch (attributes.memoryType) {
    case MemoryType::NonCached: memoryType = 0; break;
    case MemoryType::WriteCombined: memoryType = 1; break;
    case MemoryType::CacheCoherent: memoryType = 2; break;
    case MemoryType::Uncached: memoryType = 3; break;
    default: return false;
    }
    // gmc_v11_0_get_vm_pte: the ordinary 4 KiB subset only, no PRT/fragment.
    flags |= memoryType << 48;
    if (attributes.readable) flags |= kReadable;
    if (attributes.writable) flags |= kWritable;
    if (attributes.executable) flags |= kExecutable;
    if (attributes.noAllocate) flags |= kNoAllocate;
    return true;
}

bool addressValid(uint64_t address, uint64_t alignment, uint64_t limit) {
    return (address & (alignment - 1)) == 0 && address < limit;
}

// Reject a claimed allocation that wraps the host pointer representation.
// The caller still has to provide a real allocation of that length.
bool pointerRangeValid(const void* pointer, size_t bytes) {
    if (pointer == nullptr || bytes == 0) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    return bytes - 1 <= UINTPTR_MAX - begin;
}

Status tableRange(TableBuffer table, size_t firstEntry, size_t count,
                  size_t& byteOffset, size_t& byteCount) {
    if (table.bytes == nullptr || count == 0) return Status::InvalidArgument;
    const size_t capacity = table.byteCapacity / kEntryBytes;
    if (firstEntry > capacity || count > capacity - firstEntry)
        return Status::CapacityExceeded;
    if (!pointerRangeValid(table.bytes, table.byteCapacity))
        return Status::InvalidArgument;
    // Both products are bounded by the supplied byteCapacity above.
    byteOffset = firstEntry * kEntryBytes;
    byteCount = count * kEntryBytes;
    return Status::Ok;
}

bool rangesOverlap(const void* first, size_t firstBytes, const void* second,
                   size_t secondBytes) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(first);
    const uintptr_t b = reinterpret_cast<uintptr_t>(second);
    // Both nonempty ranges have already been checked for pointer overflow.
    return a <= b + (secondBytes - 1) && b <= a + (firstBytes - 1);
}

void writeLittleEndian(uint8_t* output, uint64_t value) {
    for (size_t byte = 0; byte != kEntryBytes; ++byte)
        output[byte] = static_cast<uint8_t>(value >> (byte * 8));
}

} // namespace

Status writeContiguousPtes(TableBuffer table, size_t firstEntry,
                           uint64_t firstAddress, size_t pageCount,
                           LeafAttributes attributes) {
    size_t byteOffset, byteCount;
    Status status = tableRange(table, firstEntry, pageCount, byteOffset, byteCount);
    if (status != Status::Ok) return status;
    uint64_t limit, flags;
    if (!leafFlags(attributes, limit, flags) ||
        !addressValid(firstAddress, kPageBytes, limit))
        return Status::InvalidArgument;
    // Last *page base*, with no count multiplication or unsigned wrap. Because
    // limit and address are 4 KiB aligned the last page also fits completely.
    if (pageCount - 1 > (limit - kPageBytes - firstAddress) / kPageBytes)
        return Status::InvalidArgument;

    uint64_t address = firstAddress;
    for (size_t offset = 0; offset != byteCount; offset += kEntryBytes) {
        writeLittleEndian(table.bytes + byteOffset + offset, address | flags);
        address += kPageBytes;
    }
    return Status::Ok;
}

Status writeScatterPtes(TableBuffer table, size_t firstEntry,
                        const uint64_t* addresses, size_t pageCount,
                        LeafAttributes attributes) {
    size_t byteOffset, byteCount;
    Status status = tableRange(table, firstEntry, pageCount, byteOffset, byteCount);
    if (status != Status::Ok) return status;
    uint64_t limit, flags;
    if (!leafFlags(attributes, limit, flags) ||
        (reinterpret_cast<uintptr_t>(addresses) % alignof(uint64_t)) != 0 ||
        !pointerRangeValid(addresses, byteCount))
        return Status::InvalidArgument;
    uint8_t* output = table.bytes + byteOffset;
    if (rangesOverlap(addresses, byteCount, output, byteCount))
        return Status::InvalidArgument;
    for (size_t index = 0; index != pageCount; ++index)
        if (!addressValid(addresses[index], kPageBytes, limit))
            return Status::InvalidArgument;

    for (size_t index = 0; index != pageCount; ++index)
        writeLittleEndian(output + index * kEntryBytes, addresses[index] | flags);
    return Status::Ok;
}

Status writeDirectoryPointer(TableBuffer table, size_t entry,
                             uint64_t tableAddress,
                             DirectoryAttributes attributes) {
    size_t byteOffset, byteCount;
    Status status = tableRange(table, entry, 1, byteOffset, byteCount);
    if (status != Status::Ok) return status;
    uint64_t limit, flags;
    if (!domainFlags(attributes.domain, attributes.snooped, limit, flags) ||
        !addressValid(tableAddress, 64, limit))
        return Status::InvalidArgument;
    // gmc_v11_0_get_vm_pde: ordinary pointer case (PDE_PTE remains clear).
    switch (attributes.parentLevel) {
    case DirectoryLevel::Pdb3:
    case DirectoryLevel::Pdb2:
        break;
    case DirectoryLevel::Pdb1:
        if (attributes.translateFurther) flags |= kBlockFragmentNine;
        break;
    case DirectoryLevel::Pdb0:
        if (attributes.translateFurther) flags |= kTranslateFurther;
        break;
    default:
        return Status::InvalidArgument;
    }
    writeLittleEndian(table.bytes + byteOffset, tableAddress | flags);
    return Status::Ok;
}

Status writeInvalidGartPtes(TableBuffer table, size_t firstEntry,
                            uint64_t dummyDmaAddress, size_t pageCount) {
    size_t byteOffset, byteCount;
    const Status status = tableRange(table, firstEntry, pageCount, byteOffset, byteCount);
    if (status != Status::Ok) return status;
    if (!addressValid(dummyDmaAddress, kPageBytes, kSystemDmaAddressLimit))
        return Status::InvalidArgument;
    // amdgpu_gart.c:308-338: invalid entries use dummy address and flags=0.
    for (size_t offset = 0; offset < byteCount; offset += kEntryBytes)
        writeLittleEndian(table.bytes + byteOffset + offset, dummyDmaAddress);
    return Status::Ok;
}

} // namespace gmc11
} // namespace hackynavi
