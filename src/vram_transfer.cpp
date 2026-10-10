// SPDX-License-Identifier: MIT
// Selector encoding/order adapted from Cobalt64's amd/amdgpu_regs.h:148-165:
// https://github.com/Almosst-DEV/Cobalt64/blob/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65/src/navi48-bringup/src/amd/amdgpu_regs.h
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
//
// Allocation binding, bounds, byte staging, verification and failure accounting
// are independently authored. The same31-bit selector split and DATA writes
// were checked against Linux amdgpu_device.c:717-752 at
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0. Linux uses INDEX then HI before DATA;
// Cobalt uses HI then INDEX before DATA. Both finish selectors before DATA.
#include "hackynavi/vram_transfer.hpp"

namespace hackynavi {
namespace {
constexpr uint32_t kIndex = 0, kData = 4, kIndexHi = 0x18;
constexpr uint64_t kSelectorLimit = 1ULL << 63; // HI32 plus low31 address bits.

VramTransferStatus finish(VramTransferResult& out, VramTransferStatus status) {
    out.status = status;
    return status;
}
VramTransferStatus validate(const VramAllocator& allocator,
    VramAllocationHandle handle, uint64_t allocationOffset,
    const void* buffer, size_t bytes, const VramTransferIo& io,
    VramTransferResult& out) {
    out = VramTransferResult{};
    out.stage = VramTransferStage::Preflight;
    if (!buffer || !io.read32 || !io.write32 || !bytes ||
        bytes > kVramTransferMaxBytes ||
        bytes - 1 > UINTPTR_MAX - reinterpret_cast<uintptr_t>(buffer))
        return finish(out, VramTransferStatus::InvalidArgument);
    if ((allocationOffset & 3) || (bytes & 3))
        return finish(out, VramTransferStatus::InvalidAlignment);
    if (io.barBytes < kIndexHi + 4)
        return finish(out, VramTransferStatus::OutsideRegisterBar);
    VramRange range = {};
    out.allocationStatus = allocator.lookup(handle, range);
    if (out.allocationStatus != VramAllocatorStatus::Ok)
        return finish(out, VramTransferStatus::InvalidAllocation);
    if (allocationOffset > range.bytes || bytes > range.bytes - allocationOffset)
        return finish(out, VramTransferStatus::OutsideAllocation);
    if (range.offset >= kSelectorLimit || allocationOffset > kSelectorLimit - range.offset)
        return finish(out, VramTransferStatus::OutsideAllocation);
    const uint64_t start = range.offset + allocationOffset;
    if (start >= kSelectorLimit || bytes > kSelectorLimit - start)
        return finish(out, VramTransferStatus::OutsideAllocation);
    out.vramOffset = out.currentOffset = start;
    return VramTransferStatus::Ok;
}

bool select(const VramTransferIo& io, uint64_t offset, VramTransferResult& out) {
    out.currentOffset = offset;
    out.selectorWritesMayHaveOccurred = true;
    out.lastRegister = kIndexHi;
    ++out.selectorWriteAttempts;
    if (!io.write32(io.context, kIndexHi, static_cast<uint32_t>(offset >> 31)))
        return false;
    out.lastRegister = kIndex;
    ++out.selectorWriteAttempts;
    return io.write32(io.context, kIndex,
        (static_cast<uint32_t>(offset) & 0x7ffffffcU) | 0x80000000U);
}

uint32_t loadLittleEndian(const uint8_t* source) {
    return static_cast<uint32_t>(source[0]) |
        (static_cast<uint32_t>(source[1]) << 8) |
        (static_cast<uint32_t>(source[2]) << 16) |
        (static_cast<uint32_t>(source[3]) << 24);
}
void storeLittleEndian(uint8_t* destination, uint32_t value) {
    for (size_t byte = 0; byte < 4; ++byte)
        destination[byte] = static_cast<uint8_t>(value >> (byte * 8));
}
bool readWord(const VramTransferIo& io, uint64_t offset,
              VramTransferResult& out, uint32_t& value) {
    if (!select(io, offset, out)) return false;
    out.lastRegister = kData;
    ++out.dataReadAttempts;
    if (!io.read32(io.context, kData, value)) return false;
    out.observedValue = value;
    out.bytesRead += 4;
    return true;
}
} // namespace

VramTransferStatus uploadVram(const VramAllocator& allocator,
    VramAllocationHandle handle, uint64_t allocationOffset,
    const uint8_t* source, size_t bytes, const VramTransferIo& io,
    VramTransferResult& out) {
    const VramTransferStatus checked = validate(allocator, handle, allocationOffset,
        source, bytes, io, out);
    if (checked != VramTransferStatus::Ok) return checked;
    out.stage = VramTransferStage::Writing;
    for (size_t offset = 0; offset < bytes; offset += 4) {
        out.expectedValue = loadLittleEndian(source + offset);
        if (!select(io, out.vramOffset + offset, out))
            return finish(out, VramTransferStatus::TransportError);
        out.lastRegister = kData;
        out.dataWritesMayHaveOccurred = true;
        ++out.dataWriteAttempts;
        if (!io.write32(io.context, kData, out.expectedValue))
            return finish(out, VramTransferStatus::TransportError);
        out.bytesWritten += 4;
    }
    out.stage = VramTransferStage::Verifying;
    for (size_t offset = 0; offset < bytes; offset += 4) {
        out.expectedValue = loadLittleEndian(source + offset);
        uint32_t observed = 0;
        if (!readWord(io, out.vramOffset + offset, out, observed))
            return finish(out, VramTransferStatus::TransportError);
        if (observed != out.expectedValue)
            return finish(out, VramTransferStatus::VerifyMismatch);
        out.bytesVerified += 4;
    }
    out.stage = VramTransferStage::Complete;
    return finish(out, VramTransferStatus::Ok);
}

VramTransferStatus readVram(const VramAllocator& allocator,
    VramAllocationHandle handle, uint64_t allocationOffset,
    uint8_t* destination, size_t bytes, const VramTransferIo& io,
    VramTransferResult& out) {
    const VramTransferStatus checked = validate(allocator, handle, allocationOffset,
        destination, bytes, io, out);
    if (checked != VramTransferStatus::Ok) return checked;
    out.stage = VramTransferStage::Reading;
    for (size_t offset = 0; offset < bytes; offset += 4) {
        uint32_t word = 0;
        if (!readWord(io, out.vramOffset + offset, out, word))
            return finish(out, VramTransferStatus::TransportError);
        storeLittleEndian(destination + offset, word);
    }
    out.stage = VramTransferStage::Complete;
    return finish(out, VramTransferStatus::Ok);
}

const char* vramTransferStatusString(VramTransferStatus status) {
    switch (status) {
    case VramTransferStatus::Ok: return "ok";
    case VramTransferStatus::InvalidArgument: return "invalid VRAM transfer argument";
    case VramTransferStatus::InvalidAlignment: return "VRAM transfer requires DWORD offset and size";
    case VramTransferStatus::OutsideRegisterBar: return "MM window does not fit register BAR";
    case VramTransferStatus::InvalidAllocation: return "VRAM allocation handle lookup failed";
    case VramTransferStatus::OutsideAllocation: return "VRAM transfer exceeds live allocation";
    case VramTransferStatus::TransportError: return "VRAM transfer MMIO failure";
    case VramTransferStatus::VerifyMismatch: return "VRAM upload readback mismatch";
    }
    return "unknown VRAM transfer status";
}
} // namespace hackynavi
