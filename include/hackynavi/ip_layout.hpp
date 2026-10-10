// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_IP_LAYOUT_HPP
#define HACKYNAVI_IP_LAYOUT_HPP
#include "hackynavi/discovery.hpp"

namespace hackynavi {
enum class Navi31Block : uint8_t { Gc, Mmhub, Mp0, Mp1, Sdma0, Sdma1, Count };
enum : size_t { kNavi31BlockCount = 6, kNavi31MaxBases = 8 };
enum class IpLayoutStatus {
    Ok, DiscoveryRejected, UnsupportedAddressEncoding, MissingIp, DuplicateIp,
    UnsupportedTopology, UnsupportedIpVersion, HarvestedIp, BaseCapacityExceeded,
    MissingBase, InvalidLayout, InvalidBlock, AddressOverflow, OutsideBar
};
struct IpRegisterBlock {
    uint16_t hardwareId, dieId;
    uint8_t instance, major, minor, revision, baseCount;
    uint32_t basesDwords[kNavi31MaxBases];
};
struct Navi31IpLayout {
    bool valid;
    IpRegisterBlock blocks[kNavi31BlockCount];
};
struct IpLayoutDiagnostic {
    DiscoveryStatus discoveryStatus;
    Navi31Block block; // Count if no particular required block is implicated.
    uint16_t dieId;
    uint8_t instance;
};
struct ResolvedRegister {
    uint32_t registerDword;  // Discovery base + generated register offset.
    uint32_t mmioByteOffset; // registerDword * 4, suitable for BAR callbacks.
};

// Fully validates the binary with parseDiscovery before collecting required
// die-0/instance-0 records: GC 11.0.0, MMHUB 3.0.0, MP0/MP1 13.0.0,
// SDMA0/SDMA1 6.0.0 (distinct HWIDs 42/43, each instance 0). Required IPs with
// nonzero subrevision/variant or legacy harvest flags are unsupported. Other
// IP types are ignored. Requires exactly one die, ID 0. No heap allocation.
// Rejects 64-bit address encoding rather than discarding ASIC-specific bits.
// out is entirely zero/invalid on every failure; diagnostic identifies the
// parser error or first required block failing layout policy. Input must stay
// immutable throughout. This establishes metadata, not live hardware state,
// harvest-table semantics, register access permission, or successful bring-up.
IpLayoutStatus buildNavi31IpLayout(const uint8_t* data, size_t size,
    Navi31IpLayout& out, IpLayoutDiagnostic& diagnostic);

// Resolves a generated *_BASE_IDX + DWORD register-offset pair for GC/MMHUB/
// MP0/MP1. SDMA blocks are intentionally rejected here: use the helper below.
// No MMIO occurs. A complete 32-bit access must fit barBytes; offsets outside
// direct BAR space are rejected, never redirected to an indirect register path.
// Layout must remain unmodified after a successful build. out is zero on error.
IpLayoutStatus resolveNavi31Register(const Navi31IpLayout& layout,
    Navi31Block block, uint32_t baseIndex, uint32_t registerOffsetDwords,
    uint64_t barBytes, ResolvedRegister& out);

// Matches Linux sdma_v6_0_get_reg_offset: engine 0/1, using the generated SDMA0
// internal DWORD offset (not an already adjusted SDMA1 offset). 0x5880..0x589a
// uses GC base[1] with engine*0x20; other offsets use GC base[0] and engine*0x600.
// This arithmetic helper does not certify that an arbitrary offset is an SDMA
// register; callers must supply an audited generated-register definition.
IpLayoutStatus resolveNavi31SdmaRegister(const Navi31IpLayout& layout,
    uint32_t engine, uint32_t sdma0OffsetDwords, uint64_t barBytes,
    ResolvedRegister& out);
const char* ipLayoutStatusString(IpLayoutStatus status);
const char* navi31BlockString(Navi31Block block);
} // namespace hackynavi
#endif
