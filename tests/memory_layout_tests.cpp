// SPDX-License-Identifier: MIT
#include "hackynavi/memory_layout.hpp"
#include <stdio.h>
using namespace hackynavi;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("line %u: %s\n", unsigned(__LINE__), #x); } } while (0)
const uint64_t GiB = UINT64_C(1) << 30;
MemoryLayoutInput sample() {
    // Deliberately synthetic and not claimed to be this board's live MC map.
    return MemoryLayoutInput{0x8000, 0x85ff, 0x10000, 24 * GiB, 0x2000000000, 256u << 20};
}
void basics() {
    MemoryLayoutInput in = sample(); MemoryLayout out = {};
    CHECK(deriveMemoryLayout(in, out) == MemoryLayoutStatus::Ok);
    CHECK(out.vramStart == 0x8000000000);
    CHECK(out.vramEndInclusive == 0x85ffffffff);
    CHECK(out.vramPageBaseOffset == 0x10000000000);
    CHECK(out.cpuVisibleVramBytes == 256u << 20);
    CHECK(out.bar0PhysicalAddress == 0x2000000000);
    uint64_t address = 0;
    CHECK(vramMcAddress(out, 0, 4096, address) == MemoryLayoutStatus::Ok);
    CHECK(address == out.vramStart);
    CHECK(vramPhysicalAddress(out, 4096, 4096, address) == MemoryLayoutStatus::Ok);
    CHECK(address == 0x10000001000);
    CHECK(vramPhysicalAddress(out, out.vramBytes - 4096, 4096, address) == MemoryLayoutStatus::Ok);
    CHECK(address == out.vramPageBaseOffset + out.vramBytes - 4096);
    CHECK(vramPhysicalAddress(out, out.vramBytes - 1, 2, address) == MemoryLayoutStatus::InvalidWindow);
    CHECK(address == 0);
    const uint64_t pageBase = out.vramPageBaseOffset;
    out.vramPageBaseOffset = UINT64_MAX;
    CHECK(vramPhysicalAddress(out, 0, 4096, address) == MemoryLayoutStatus::InvalidArgument);
    out.vramPageBaseOffset = pageBase;
    CHECK(vramMcAddress(out, out.vramBytes - 1, 1, address) == MemoryLayoutStatus::Ok);
    CHECK(address == out.vramEndInclusive);
    CHECK(vramMcAddress(out, out.vramBytes - 1, 2, address) == MemoryLayoutStatus::InvalidWindow);
    CHECK(address == 0);
    CHECK(vramMcAddress(out, UINT64_MAX, UINT64_MAX, address) == MemoryLayoutStatus::InvalidWindow);
    CHECK(vramMcAddress(out, 0, 0, address) == MemoryLayoutStatus::InvalidArgument);
    out.vramEndInclusive ^= 1;
    CHECK(vramMcAddress(out, 0, 4096, address) == MemoryLayoutStatus::InvalidArgument);
    in.bar0Bytes = 32 * GiB;
    CHECK(deriveMemoryLayout(in, out) == MemoryLayoutStatus::Ok);
    CHECK(out.cpuVisibleVramBytes == 24 * GiB);
    in.framebufferTop = 0x87ff; // 32GiB MC window containing 24GiB actual VRAM.
    CHECK(deriveMemoryLayout(in, out) == MemoryLayoutStatus::Ok);
    CHECK(out.mcWindowEndInclusive == 0x87ffffffff);
    CHECK(out.vramEndInclusive == 0x85ffffffff);
}
void reject() {
    for (unsigned which = 0; which < 13; ++which) {
        auto in = sample(); MemoryLayout out = {};
        MemoryLayoutStatus expected = MemoryLayoutStatus::InvalidArgument;
        switch (which) {
        case 0: in.framebufferBase = UINT32_MAX; expected = MemoryLayoutStatus::DeviceUnavailable; break;
        case 1: in.framebufferTop |= 0x1000000; expected = MemoryLayoutStatus::ReservedBitsSet; break;
        case 2: in.framebufferOffset |= 0x80000000; expected = MemoryLayoutStatus::ReservedBitsSet; break;
        case 3: in.framebufferTop = in.framebufferBase - 1; expected = MemoryLayoutStatus::InvalidWindow; break;
        case 4: in.vramBytes += 4096; expected = MemoryLayoutStatus::VramExceedsWindow; break;
        case 5: in.framebufferOffset = 0xffffff; expected = MemoryLayoutStatus::AddressOverflow; break;
        case 6: in.bar0PhysicalAddress = UINT64_MAX - 4095; expected = MemoryLayoutStatus::AddressOverflow; break;
        case 7: in.vramBytes = 0; break;
        case 8: ++in.vramBytes; break;
        case 9: in.bar0Bytes = 0; break;
        case 10: ++in.bar0Bytes; break;
        case 11: in.bar0PhysicalAddress = 0; break;
        case 12: ++in.bar0PhysicalAddress; break;
        }
        out.vramBytes = 123;
        CHECK(deriveMemoryLayout(in, out) == expected);
        CHECK(out.vramBytes == 0 && out.vramStart == 0 && out.bar0Bytes == 0);
    }
    auto in = sample(); MemoryLayout out = {};
    in.framebufferBase = 0xffff00; in.framebufferTop = 0xffffff;
    in.framebufferOffset = 0xffff00; in.vramBytes = 4 * GiB;
    CHECK(deriveMemoryLayout(in, out) == MemoryLayoutStatus::Ok);
    CHECK(out.vramEndInclusive == (UINT64_C(1) << 48) - 1);
    uint64_t address = 0;
    CHECK(vramMcAddress(out, in.vramBytes - 1, 1, address) == MemoryLayoutStatus::Ok);
    CHECK(address == (UINT64_C(1) << 48) - 1);
    CHECK(vramPhysicalAddress(out, in.vramBytes - 1, 1, address) == MemoryLayoutStatus::Ok);
    CHECK(address == (UINT64_C(1) << 48) - 1);
}
}
int main() {
    basics(); reject();
    printf("Memory layout: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
