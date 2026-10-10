// SPDX-License-Identifier: MIT
// XNU xnu-11417.140.69: pexpert/i386/pe_init.c:192-229,262;
// osfmk/console/video_console.c:1537-1548,2652-2674,2682-2688,2714-2721;
// osfmk/i386/io_map.c:92-96 (second rounding after physical-page offset).
// PE keeps the physical console base; console mapping length is explicit or
// pitch*height, and the writable pixel base adds v_offset to that mapping.
#include "hackynavi/efi_console.hpp"

namespace hackynavi { namespace eficonsole {
namespace {
constexpr uint64_t kPage = 4096;
constexpr uint64_t kMcLimit = UINT64_C(1) << 48;
bool validMemory(const MemoryLayout& m) {
    if (!m.bar0PhysicalAddress || !m.bar0Bytes ||
        ((m.bar0PhysicalAddress | m.bar0Bytes) & (kPage - 1)) ||
        m.bar0Bytes - 1 > UINT64_MAX - m.bar0PhysicalAddress ||
        (m.mcWindowStart & 0xffffff) ||
        (m.mcWindowEndInclusive & 0xffffff) != 0xffffff ||
        m.mcWindowStart >= kMcLimit || m.mcWindowEndInclusive >= kMcLimit ||
        m.mcWindowEndInclusive < m.mcWindowStart ||
        m.vramStart != m.mcWindowStart ||
        m.vramEndInclusive > m.mcWindowEndInclusive ||
        m.cpuVisibleVramBytes != (m.bar0Bytes < m.vramBytes ? m.bar0Bytes : m.vramBytes))
        return false;
    uint64_t address = 0;
    return vramMcAddress(m, 0, m.vramBytes, address) == MemoryLayoutStatus::Ok &&
        vramPhysicalAddress(m, 0, m.vramBytes, address) == MemoryLayoutStatus::Ok;
}
}
Status buildReservation(const MemoryLayout& m, const Input& in, Result& out) {
    out = Result{};
    if (!validMemory(m)) return Status::InvalidMemoryLayout;
    if (in.rotation) return Status::UnsupportedRotation;
    uint64_t pixelBytes = 0;
    switch (in.depth) {
    case 8: pixelBytes = 1; break;
    case 16: pixelBytes = 2; break;
    case 30: case 32: pixelBytes = 4; break;
    default: return Status::UnsupportedDepth;
    }
    // XNU stores the geometry in unsigned-int vc_info fields. Reject values
    // that would silently truncate there, as well as absent console metadata.
    if (!in.physicalBase || (in.physicalBase & 3) || !in.rowBytes || !in.width || !in.height ||
        in.rowBytes > UINT32_MAX || in.width > UINT32_MAX || in.height > UINT32_MAX ||
        in.width > in.rowBytes / pixelBytes) return Status::InvalidGeometry;
    if (in.height > UINT64_MAX / in.rowBytes) return Status::AddressOverflow;
    const uint64_t imageBytes = in.rowBytes * in.height;
    const uint64_t mappingBytes = in.length ? in.length : imageBytes;
    // initialize_screen stores its rounded mapping size in unsigned int.
    // Reject metadata that would wrap that native mapping size.
    if (mappingBytes > (uint64_t(UINT32_MAX) & ~(kPage - 1)))
        return mappingBytes - 1 > UINT64_MAX - in.physicalBase ?
            Status::AddressOverflow : Status::InvalidGeometry;
    if (in.offset > mappingBytes || imageBytes > mappingBytes - in.offset)
        return Status::InvalidGeometry;
    const uint64_t roundedMapping = (mappingBytes + kPage - 1) & ~(kPage - 1);
    if (roundedMapping - 1 > UINT64_MAX - in.physicalBase) return Status::AddressOverflow;
    const uint64_t mappedFirst = in.physicalBase & ~(kPage - 1);
    const uint64_t mappedLast = (in.physicalBase + roundedMapping - 1) | (kPage - 1);
    const uint64_t barLast = m.bar0PhysicalAddress + m.bar0Bytes - 1;
    if (mappedLast < m.bar0PhysicalAddress || mappedFirst > barLast) return Status::NotOnBar;
    if (mappedFirst < m.bar0PhysicalAddress || mappedLast > barLast) return Status::OutsideBar;
    const uint64_t offset = in.physicalBase - m.bar0PhysicalAddress;
    if (offset >= m.vramBytes || mappingBytes > m.vramBytes - offset) return Status::OutsideVram;
    const uint64_t pageStart = mappedFirst - m.bar0PhysicalAddress;
    const uint64_t pageBytes = mappedLast - mappedFirst + 1;
    if (pageStart >= m.cpuVisibleVramBytes || pageBytes > m.cpuVisibleVramBytes - pageStart)
        return Status::OutsideVram;
    out.bytes = VramRange{offset, mappingBytes};
    out.pages = VramRange{pageStart, pageBytes};
    return Status::Ok;
}
const char* statusString(Status status) {
    switch (status) {
#define NAME(x) case Status::x: return #x
    NAME(Ok); NAME(InvalidMemoryLayout); NAME(InvalidGeometry); NAME(UnsupportedDepth);
    NAME(UnsupportedRotation); NAME(AddressOverflow); NAME(NotOnBar); NAME(OutsideBar); NAME(OutsideVram);
#undef NAME
    }
    return "Unknown";
}
} }
