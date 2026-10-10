// SPDX-License-Identifier: MIT
// Original packet construction with bounded caller-owned output.
// Wire-format references, Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h
// See sdma_v6_0_emit_copy_buffer, sdma_v6_0_emit_fill_buffer, and
// sdma_v6_0_ring_emit_fence in the same directory's sdma_v6_0.c.

#include "hackynavi/sdma.hpp"

namespace hackynavi {
namespace sdma6 {
namespace {

constexpr uint32_t kCopyHeader = 0x00000001U; // COPY op=1, LINEAR subop=0.
constexpr uint32_t kFillHeader = 0x0000000bU; // CONST_FILL op=11, other fields=0.
constexpr uint32_t kFenceHeader = 0x00030005U; // FENCE op=5, MTYPE=3 (uncached).
constexpr uint32_t kTrapHeader = 0x00000006U; // TRAP op=6.

uint32_t low(uint64_t value) { return static_cast<uint32_t>(value); }
uint32_t high(uint64_t value) { return static_cast<uint32_t>(value >> 32); }

bool rangeValid(uint64_t address, uint32_t bytes) {
    return bytes != 0 && static_cast<uint64_t>(bytes - 1) <= UINT64_MAX - address;
}

bool transferValid(uint32_t bytes) {
    return bytes != 0 && bytes <= kMaxTransferBytes;
}

bool rangesOverlap(uint64_t source, uint64_t destination, uint32_t bytes) {
    // Difference-based comparison avoids constructing a wrapping end address.
    return source <= destination ? destination - source < bytes
                                 : source - destination < bytes;
}

Status append(CommandBuffer& buffer, const uint32_t* packet, size_t count) {
    if (!buffer.words ||
        reinterpret_cast<uintptr_t>(buffer.words) % alignof(uint32_t) != 0 ||
        buffer.capacity > SIZE_MAX / sizeof(uint32_t) || buffer.used > buffer.capacity)
        return Status::InvalidArgument;
    if (count > buffer.capacity - buffer.used)
        return Status::CapacityExceeded;
    // All validation occurs before any destination write. packet always refers
    // to a private local array, so its reads cannot alias this output buffer.
    for (size_t index = 0; index < count; ++index)
        buffer.words[buffer.used + index] = packet[index];
    buffer.used += count;
    return Status::Ok;
}

} // namespace

Status appendCopyLinear(CommandBuffer& buffer, uint64_t source,
                        uint64_t destination, uint32_t byteCount) {
    if (!transferValid(byteCount) || !rangeValid(source, byteCount) ||
        !rangeValid(destination, byteCount) || rangesOverlap(source, destination, byteCount))
        return Status::InvalidArgument;
    const uint32_t packet[kCopyLinearWords] = {
        kCopyHeader, byteCount - 1, 0, low(source), high(source),
        low(destination), high(destination)
    };
    return append(buffer, packet, kCopyLinearWords);
}

Status appendConstantFill(CommandBuffer& buffer, uint64_t destination,
                          uint32_t value, uint32_t byteCount) {
    if (!transferValid(byteCount) || (byteCount & 3U) != 0 ||
        (destination & 3U) != 0 || !rangeValid(destination, byteCount))
        return Status::InvalidArgument;
    const uint32_t packet[kConstantFillWords] = {
        kFillHeader, low(destination), high(destination), value, byteCount - 1
    };
    return append(buffer, packet, kConstantFillWords);
}

Status appendFence32(CommandBuffer& buffer, uint64_t address,
                     uint32_t value, bool trap) {
    if ((address & 3U) != 0 || !rangeValid(address, 4))
        return Status::InvalidArgument;
    const uint32_t packet[kFenceWords + kTrapWords] = {
        kFenceHeader, low(address), high(address), value, kTrapHeader, 0
    };
    return append(buffer, packet, kFenceWords + (trap ? kTrapWords : 0));
}

Status appendFence64(CommandBuffer& buffer, uint64_t address,
                     uint64_t value, bool trap) {
    if ((address & 3U) != 0 || !rangeValid(address, 8))
        return Status::InvalidArgument;
    const uint64_t highAddress = address + 4; // Eight-byte range validated above.
    const uint32_t packet[kFenceWords * 2 + kTrapWords] = {
        kFenceHeader, low(address), high(address), low(value),
        kFenceHeader, low(highAddress), high(highAddress), high(value),
        kTrapHeader, 0
    };
    return append(buffer, packet, kFenceWords * 2 + (trap ? kTrapWords : 0));
}

} // namespace sdma6
} // namespace hackynavi
