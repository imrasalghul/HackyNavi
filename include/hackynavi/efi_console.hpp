// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_EFI_CONSOLE_HPP
#define HACKYNAVI_EFI_CONSOLE_HPP
#include "hackynavi/vram_allocator.hpp"

namespace hackynavi { namespace eficonsole {
enum class Status {
    Ok, InvalidMemoryLayout, InvalidGeometry, UnsupportedDepth,
    UnsupportedRotation, AddressOverflow, NotOnBar, OutsideBar, OutsideVram
};
// A successful, coherent IOPlatformExpert::getConsoleInfo(PE_Video*) snapshot.
// Copy the unsigned-long fields without truncation. physicalBase must already
// be decoded as a CPU physical address, not raw tagged PE v_baseAddr, an MC
// address, DMA address, or virtual pointer. Its low two bits must be zero.
struct Input {
    uint64_t physicalBase, rowBytes, width, height, depth, offset, length;
    uint8_t rotation;
};
struct Result {
    VramRange bytes; // Full declared console mapping, relative to VRAM byte0.
    VramRange pages; // XNU's actual mapped pages, including its two roundings.
};
// Unrotated XNU pixel depths8/16/30/32 only. Reserve length bytes when nonzero,
// otherwise rowBytes*height, including row padding. The active image starting
// at offset must fit the declared mapping; zero length therefore needs zero
// offset. Reject geometry/rounded mapping sizes that truncate XNU's uint32
// fields. Do not infer capacity from display resolution or refresh rate.
//
// XNU rounds length before io_map then rounds again for an unaligned physical
// base. pages includes that possible extra page, not just rounded pixel bytes.
// NotOnBar means the complete validated mapped-page interval is disjoint from
// this BAR; it does not prove which device owns it or that Navi31 has no other
// live surfaces. Partial BAR overlap and BAR bytes beyond VRAM are rejected.
// Ok establishes ONE console reservation, never complete framebuffer inventory,
// exclusive ownership, CPU console retirement, or GPU quiescence. Union it
// with actual display/firmware reservations and retain it until both GPU users
// and the platform console have relinquished it. No I/O, allocation or mapping.
// Input/output must not alias. Output is zero on every non-Ok return.
Status buildReservation(const MemoryLayout&, const Input&, Result&);
const char* statusString(Status);
} }
#endif
