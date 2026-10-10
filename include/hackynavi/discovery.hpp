// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISCOVERY_HPP
#define HACKYNAVI_DISCOVERY_HPP
#include <stddef.h>
#include <stdint.h>
namespace hackynavi {
enum class DiscoveryStatus {
    Ok, InvalidArgument, Truncated, BadSignature, Malformed,
    UnsupportedVersion, ChecksumMismatch, CapacityExceeded
};
enum : size_t { kMaxDiscoveryTables = 32, kMaxDiscoveryDies = 16,
                kMaxDiscoveryBaseAddresses = 255 };
struct DiscoveryTable { uint16_t offset, size, checksum; };
struct DiscoveryDie { uint16_t id, offset, ipCount; };
struct DiscoveryInfo {
    uint16_t binaryMajor, binaryMinor, binarySize, tableCount;
    uint16_t ipVersion, dieCount;
    uint32_t ipTableId, ipCount;
    bool baseAddresses64Bit;
    DiscoveryTable tables[kMaxDiscoveryTables];
    DiscoveryDie dies[kMaxDiscoveryDies];
};
struct DiscoveryIp {
    uint16_t dieId, offset, hardwareId;
    uint8_t instance, major, minor, revision;
    uint8_t harvest; // Low nibble for discovery versions 1/2 only.
    uint8_t subRevision, variant; // Version 3/4 nibbles only.
    uint8_t baseAddressCount;
    // Raw firmware encoding; NOT CPU physical addresses or normalized MMIO.
    // Only the first baseAddressCount entries are valid.
    uint64_t baseAddresses[kMaxDiscoveryBaseAddresses];
};
typedef void (*DiscoveryVisitor)(const DiscoveryIp& ip, void* context);
// Parse a discovery binary beginning at its binary signature (no PSP prefix).
// Accepts binary headers 0/1 (6 tables) and 2 (up to 32 tables), IP tables 1-4.
// All offsets are binary-relative. Trailing bytes beyond binarySize are ignored.
// Checksums enforce corruption detection, not authenticity. Present table spans
// must be disjoint, outside the directory, and match their declared checksums.
// The directory, table spans/checksums and full IP record stream are validated
// before any visitor call. Auxiliary table contents remain opaque; their own
// internal field schemas are not interpreted or validated. The callback's
// reference is temporary and must not be retained. data must remain unchanged
// throughout this call, including during callbacks. No allocation or writes.
// out is zero on error, except validated version metadata on UnsupportedVersion.
DiscoveryStatus parseDiscovery(const uint8_t* data, size_t size, DiscoveryInfo& out,
    DiscoveryVisitor visitor = nullptr, void* context = nullptr);
const char* discoveryStatusString(DiscoveryStatus status);
} // namespace hackynavi
#endif
