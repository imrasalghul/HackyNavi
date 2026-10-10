// SPDX-License-Identifier: MIT
// Boundary/geometry tests of a pure reservation decoder; no target I/O.
#include "hackynavi/efi_console.hpp"
#include <iostream>
using namespace hackynavi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "line " << __LINE__ << ": " #x "\n"; } } while (0)
constexpr uint64_t MiB = UINT64_C(1) << 20, GiB = UINT64_C(1) << 30;
MemoryLayout memory(uint64_t barBytes = 256 * MiB) {
    MemoryLayout m{};
    CHECK(deriveMemoryLayout({0x8000, 0x85ff, 0x1000, 24 * GiB, 16 * GiB, barBytes}, m) == MemoryLayoutStatus::Ok);
    return m;
}
eficonsole::Input console(const MemoryLayout& m) {
    return {m.bar0PhysicalAddress + 8192, 16384, 3840, 2160, 32, 0, 0, 0};
}
void rejects(const MemoryLayout& m, const eficonsole::Input& in, eficonsole::Status status) {
    eficonsole::Result out{{123, 456}, {789, 123}};
    CHECK(eficonsole::buildReservation(m, in, out) == status);
    CHECK(!out.bytes.offset && !out.bytes.bytes && !out.pages.offset && !out.pages.bytes);
}
void validRanges() {
    const auto m = memory(); auto in = console(m); eficonsole::Result out{};
    CHECK(eficonsole::buildReservation(m, in, out) == eficonsole::Status::Ok);
    CHECK(out.bytes.offset == 8192 && out.bytes.bytes == UINT64_C(16384) * 2160);
    CHECK(out.pages.offset == out.bytes.offset && out.pages.bytes == out.bytes.bytes);
    // Reserve padding and an explicit larger mapping, not just visible pixels.
    in.offset = 4096; in.length = in.rowBytes * in.height + 8192;
    CHECK(eficonsole::buildReservation(m, in, out) == eficonsole::Status::Ok);
    CHECK(out.bytes.offset == 8192 && out.bytes.bytes == in.length);
    in.physicalBase += 4; in.length += 1;
    CHECK(eficonsole::buildReservation(m, in, out) == eficonsole::Status::Ok);
    CHECK(out.pages.offset == 8192 && out.pages.bytes == ((in.length + 4095) & ~UINT64_C(4095)) + 4096);
    // XNU rounds length first, then maps an extra page for an unaligned base.
    in = {m.bar0PhysicalAddress + 8196, 4, 1, 1, 32, 0, 4, 0};
    CHECK(eficonsole::buildReservation(m, in, out) == eficonsole::Status::Ok);
    CHECK(out.pages.offset == 8192 && out.pages.bytes == 8192);
    // An aligned mapping may end at the final visible byte.
    in = {m.bar0PhysicalAddress + m.bar0Bytes - 4096, 4096, 1024, 1, 32, 0, 0, 0};
    CHECK(eficonsole::buildReservation(m, in, out) == eficonsole::Status::Ok);
    CHECK(out.pages.offset == m.bar0Bytes - 4096 && out.pages.bytes == 4096);
    for (uint64_t depth : {8ULL, 16ULL, 30ULL, 32ULL}) {
        in = console(m); in.depth = depth; in.rowBytes = in.width * ((depth + 7) / 8);
        CHECK(eficonsole::buildReservation(m, in, out) == eficonsole::Status::Ok);
        CHECK(out.bytes.bytes == in.rowBytes * in.height);
    }
}
void invalidRanges() {
    auto m = memory(); auto in = console(m);
    in.rowBytes = 3840 * 4 - 1; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.offset = 1; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.length = in.rowBytes * in.height - 1; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.offset = UINT64_MAX; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.length = UINT64_MAX; rejects(m, in, eficonsole::Status::AddressOverflow);
    in = console(m); in.length = UINT64_C(1) << 32; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.physicalBase = UINT64_MAX - 4095; rejects(m, in, eficonsole::Status::AddressOverflow);
    in = console(m); in.rowBytes = UINT64_MAX; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.width = UINT64_MAX; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.height = UINT64_MAX; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.physicalBase = 0; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); ++in.physicalBase; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.height = 0; rejects(m, in, eficonsole::Status::InvalidGeometry);
    in = console(m); in.depth = 24; rejects(m, in, eficonsole::Status::UnsupportedDepth);
    in = console(m); in.depth += UINT64_C(1) << 32; rejects(m, in, eficonsole::Status::UnsupportedDepth);
    in = console(m); in.rotation = 1; rejects(m, in, eficonsole::Status::UnsupportedRotation);
    in = console(m); in.rotation = 3; rejects(m, in, eficonsole::Status::UnsupportedRotation);
    in = console(m); in.physicalBase = m.bar0PhysicalAddress - 4096;
    rejects(m, in, eficonsole::Status::OutsideBar);
    in = console(m); in.physicalBase = m.bar0PhysicalAddress + m.bar0Bytes - 4096;
    rejects(m, in, eficonsole::Status::OutsideBar);
    // Declared pixels fit, but XNU's extra mapped page would escape the BAR.
    in = {m.bar0PhysicalAddress + m.bar0Bytes - 4, 4, 1, 1, 32, 0, 0, 0};
    rejects(m, in, eficonsole::Status::OutsideBar);
    in = console(m); in.physicalBase = m.bar0PhysicalAddress + m.bar0Bytes;
    rejects(m, in, eficonsole::Status::NotOnBar);
    in = console(m); in.physicalBase = m.bar0PhysicalAddress - in.rowBytes * in.height;
    rejects(m, in, eficonsole::Status::NotOnBar);
    m = memory(32 * GiB); in = console(m); in.physicalBase += m.vramBytes;
    rejects(m, in, eficonsole::Status::OutsideVram);
    // A resizable BAR larger than VRAM must not create a fictitious free tail.
    in.physicalBase = m.bar0PhysicalAddress + m.vramBytes - 4096;
    rejects(m, in, eficonsole::Status::OutsideVram);
}
void malformedMemory() {
    const auto good = memory(); const auto in = console(good); auto m = good;
    m.bar0PhysicalAddress = UINT64_MAX - 4095; rejects(m, in, eficonsole::Status::InvalidMemoryLayout);
    m = good; --m.bar0Bytes; rejects(m, in, eficonsole::Status::InvalidMemoryLayout);
    m = good; m.cpuVisibleVramBytes = m.vramBytes; rejects(m, in, eficonsole::Status::InvalidMemoryLayout);
    m = good; ++m.vramEndInclusive; rejects(m, in, eficonsole::Status::InvalidMemoryLayout);
    m = good; m.vramPageBaseOffset = UINT64_MAX; rejects(m, in, eficonsole::Status::InvalidMemoryLayout);
    m = good; m.mcWindowEndInclusive = m.mcWindowStart - 1; rejects(m, in, eficonsole::Status::InvalidMemoryLayout);
    m = good; m.vramBytes = 0; rejects(m, in, eficonsole::Status::InvalidMemoryLayout);
}
}
int main() {
    validRanges(); invalidRanges(); malformedMemory();
    std::cout << checks << " EFI console reservation checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
