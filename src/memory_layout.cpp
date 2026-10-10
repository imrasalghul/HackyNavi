// SPDX-License-Identifier: MIT
// Register field facts checked against Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// include/asic_reg/mmhub/mmhub_3_0_0_sh_mask.h:7129-7131,7216-7221;
// amdgpu/mmhub_v3_0.c:get_fb_location/get_mc_fb_offset;
// amdgpu/gmc_v11_0.c:mc_init/vram_gtt_location (48-bit MC address space).
// This decoder does not choose or program GART placement.
#include "hackynavi/memory_layout.hpp"

namespace hackynavi {
namespace {
const uint64_t kMcLimit = UINT64_C(1) << 48;
const uint32_t kFieldMask = 0x00ffffff;
bool sane(const MemoryLayout& layout) {
    return layout.vramBytes && (layout.vramBytes & 4095u) == 0 &&
        (layout.vramStart & 4095u) == 0 &&
        layout.vramStart < kMcLimit &&
        layout.vramBytes <= kMcLimit - layout.vramStart &&
        layout.vramEndInclusive == layout.vramStart + layout.vramBytes - 1;
}
}
MemoryLayoutStatus deriveMemoryLayout(const MemoryLayoutInput& input,
                                     MemoryLayout& out) {
    out = MemoryLayout{};
    if (input.framebufferBase == UINT32_MAX || input.framebufferTop == UINT32_MAX ||
        input.framebufferOffset == UINT32_MAX)
        return MemoryLayoutStatus::DeviceUnavailable;
    if ((input.framebufferBase | input.framebufferTop | input.framebufferOffset) & ~kFieldMask)
        return MemoryLayoutStatus::ReservedBitsSet;
    if (!input.vramBytes || (input.vramBytes & 4095u) || !input.bar0Bytes ||
        !input.bar0PhysicalAddress || (input.bar0PhysicalAddress & 4095u) ||
        (input.bar0Bytes & 4095u)) return MemoryLayoutStatus::InvalidArgument;
    if (input.bar0Bytes - 1 > UINT64_MAX - input.bar0PhysicalAddress)
        return MemoryLayoutStatus::AddressOverflow;
    const uint64_t start = static_cast<uint64_t>(input.framebufferBase) << 24;
    const uint64_t end = (static_cast<uint64_t>(input.framebufferTop) << 24) | 0xffffffu;
    const uint64_t pageBase = static_cast<uint64_t>(input.framebufferOffset) << 24;
    if (end < start) return MemoryLayoutStatus::InvalidWindow;
    if (input.vramBytes > end - start + 1)
        return MemoryLayoutStatus::VramExceedsWindow;
    if (input.vramBytes > kMcLimit - pageBase)
        return MemoryLayoutStatus::AddressOverflow;
    out.mcWindowStart = start;
    out.mcWindowEndInclusive = end;
    out.vramStart = start;
    out.vramEndInclusive = start + input.vramBytes - 1;
    out.vramBytes = input.vramBytes;
    out.vramPageBaseOffset = pageBase;
    out.bar0PhysicalAddress = input.bar0PhysicalAddress;
    out.bar0Bytes = input.bar0Bytes;
    out.cpuVisibleVramBytes = input.bar0Bytes < input.vramBytes ? input.bar0Bytes : input.vramBytes;
    return MemoryLayoutStatus::Ok;
}
MemoryLayoutStatus vramMcAddress(const MemoryLayout& layout, uint64_t offset,
                                uint64_t length, uint64_t& address) {
    if (!sane(layout) || !length) {
        address = 0;
        return MemoryLayoutStatus::InvalidArgument;
    }
    if (offset >= layout.vramBytes || length > layout.vramBytes - offset) {
        address = 0;
        return MemoryLayoutStatus::InvalidWindow;
    }
    address = layout.vramStart + offset;
    return MemoryLayoutStatus::Ok;
}
MemoryLayoutStatus vramPhysicalAddress(const MemoryLayout& layout, uint64_t offset,
                                      uint64_t length, uint64_t& address) {
    if (!sane(layout) || !length || (layout.vramPageBaseOffset & 4095u) ||
        layout.vramPageBaseOffset >= kMcLimit ||
        layout.vramBytes > kMcLimit - layout.vramPageBaseOffset) {
        address = 0;
        return MemoryLayoutStatus::InvalidArgument;
    }
    if (offset >= layout.vramBytes || length > layout.vramBytes - offset) {
        address = 0;
        return MemoryLayoutStatus::InvalidWindow;
    }
    address = layout.vramPageBaseOffset + offset;
    return MemoryLayoutStatus::Ok;
}
const char* memoryLayoutStatusString(MemoryLayoutStatus status) {
    switch (status) {
    case MemoryLayoutStatus::Ok: return "ok";
    case MemoryLayoutStatus::InvalidArgument: return "invalid memory layout input";
    case MemoryLayoutStatus::DeviceUnavailable: return "memory controller unavailable";
    case MemoryLayoutStatus::ReservedBitsSet: return "reserved framebuffer register bits set";
    case MemoryLayoutStatus::InvalidWindow: return "invalid memory controller range";
    case MemoryLayoutStatus::VramExceedsWindow: return "VRAM exceeds configured MC window";
    case MemoryLayoutStatus::AddressOverflow: return "memory address overflow";
    }
    return "unknown memory layout status";
}
} // namespace hackynavi
