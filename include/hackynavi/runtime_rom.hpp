// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_RUNTIME_ROM_HPP
#define HACKYNAVI_RUNTIME_ROM_HPP
#include "hackynavi/discovery.hpp"
#include "hackynavi/rom.hpp"
namespace hackynavi { namespace runtimerom {
constexpr size_t kRecommendedCapacity=128U*1024U;
enum class Status { Ok, InvalidArgument, DiscoveryRejected, UnsupportedIp,
    InvalidLayout, OutsideBar, AddressOverflow, ReadFailed, WriteFailed,
    DeviceUnavailable, IndirectHighNotZero, RomRejected, BufferTooSmall, BankChanged };
struct Layout {
    bool valid; uint64_t barBytes;
    uint32_t romIndex,romData,romOffsetControl,rsmuIndex,rsmuData,rsmuIndexHigh;
};
struct Io { void* context;
    bool (*read32)(void*,uint32_t barByteOffset,uint32_t&);
    bool (*write32)(void*,uint32_t barByteOffset,uint32_t); };
struct Result {
    Status status; uint32_t reads,writes,lastRegister,lastValue,bankByteOffset;
    size_t bytesRead,bytesRequired;
    bool selectorWritesMayHaveOccurred;
    ImageInfo image; AtomInfo atom; // zero unless status==Ok
};
// Selects exactly one native SMUIO13.0.6 and NBIF4.3.0 from fully validated,
// 32-bit live discovery. SMUIO ROM_INDEX/DATA must fit BAR5; NBIF bank control
// can be accessed through its discovered RSMU window. No MMIO here.
Status buildLayout(const uint8_t* discovery,size_t,uint64_t barBytes,Layout&);
// Acquires the running card's selected legacy VBIOS, not a supplied flash file.
// Requires exclusive ownership of powered bare-metal 1002:744c in D0, PCI
// memory decoding, correct live discovery, and exclusion of ROM/flash users,
// RSMU users, power/reset/removal for the ENTIRE call. Native callbacks enforce
// ordering and device availability. ROM reads write selectors, never flash.
// The caller owns ordinary CPU storage; 128KiB covers all legacy length-byte
// values. Capacity>=64 permits the bounded header probe and BufferTooSmall
// reports the actual required bytes. Partial bytes on failure are untrusted.
// Reads NBIF's ROM bank, writes SMUIO ROM_INDEX with bank<<17, then streams
// auto-incrementing ROM_DATA. All-ones ROM DWORDs are legal payload, not an
// automatic transport failure. RSMU INDEX_HIGH must already be zero and is
// never written. No ROM clock/enable writes or selector restoration occur.
// Verifies the AMD signature, unchanged bank, length, PCI ID/code/checksum and
// bounded ATOM tables before publishing parsed metadata. This is corruption
// and format checking, not authentication, and does not execute ATOM commands.
Status acquire(const Layout&,const Io&,uint8_t* destination,size_t capacity,Result&);
const char* statusString(Status);
} }
#endif
