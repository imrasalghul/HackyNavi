// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_MEMORY_LAYOUT_HPP
#define HACKYNAVI_MEMORY_LAYOUT_HPP
#include <stdint.h>

namespace hackynavi {
enum class MemoryLayoutStatus {
    Ok, InvalidArgument, DeviceUnavailable, ReservedBitsSet, InvalidWindow,
    VramExceedsWindow, AddressOverflow
};
struct MemoryLayoutInput {
    uint32_t framebufferBase, framebufferTop, framebufferOffset;
    uint64_t vramBytes;
    // Assigned PCI BAR0 resource, not a CPU virtual or GPU address.
    uint64_t bar0PhysicalAddress, bar0Bytes;
};
struct MemoryLayout {
    uint64_t mcWindowStart, mcWindowEndInclusive;
    uint64_t vramStart, vramEndInclusive, vramBytes;
    uint64_t vramPageBaseOffset;
    uint64_t bar0PhysicalAddress, bar0Bytes, cpuVisibleVramBytes;
};
// Decode MMHUB 3.0.0 framebuffer registers using their 24-bit/16-MiB units.
// Exact chip/version selection and coherent register capture belong to caller.
// No allocation or mapping is performed. The visible aperture is not a free
// allocation pool: display, firmware, discovery and PSP reservations are not
// deducted here. vramPageBaseOffset and vramStart are distinct GPU domains.
// Input/output must not alias. out is zero on failure.
MemoryLayoutStatus deriveMemoryLayout(const MemoryLayoutInput& input,
                                     MemoryLayout& out);
// Translate a nonempty VRAM-relative range into the GPU MC address used by
// VRAM BO placement, checking the entire range. Does not establish ownership.
MemoryLayoutStatus vramMcAddress(const MemoryLayout& layout, uint64_t offset,
                                uint64_t length, uint64_t& address);
// Same VRAM-relative range, translated to the GPU physical page address used
// in a VRAM-domain PTE/PDE. This is NOT a system-memory DMA or CPU address.
MemoryLayoutStatus vramPhysicalAddress(const MemoryLayout& layout, uint64_t offset,
                                      uint64_t length, uint64_t& address);
const char* memoryLayoutStatusString(MemoryLayoutStatus status);
} // namespace hackynavi
#endif
