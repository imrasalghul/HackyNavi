// SPDX-License-Identifier: MIT
// Original bounded layout/resolution code. Primary format/operation references:
// Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0
// drivers/gpu/drm/amd/include/soc15_hw_ip.h (required HWIDs)
// drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c:1867-1920 (DWORD bases)
// drivers/gpu/drm/amd/amdgpu/soc15_common.h:36 (base-index + register)
// drivers/gpu/drm/amd/amdgpu/amdgpu_reg_access.c:371-381 (DWORD to BAR bytes)
// drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c:60-63,127-142 (SDMA uses GC bases)
// https://github.com/torvalds/linux/tree/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd
// No constants below are taken from the user's ROM register-base values.
#include "hackynavi/ip_layout.hpp"

namespace hackynavi {
namespace {
const uint16_t requiredIds[kNavi31BlockCount] = {11, 34, 255, 1, 42, 43};
const uint8_t requiredMajors[kNavi31BlockCount] = {11, 3, 13, 13, 6, 6};
struct Collect {
    Navi31IpLayout layout;
    bool present[kNavi31BlockCount];
    IpLayoutStatus status;
    IpLayoutDiagnostic* diagnostic;
};

void fail(Collect& c, IpLayoutStatus status, size_t index, const DiscoveryIp& ip) {
    c.status = status;
    c.diagnostic->block = static_cast<Navi31Block>(index);
    c.diagnostic->dieId = ip.dieId;
    c.diagnostic->instance = ip.instance;
}

void collect(const DiscoveryIp& ip, void* context) {
    Collect& c = *static_cast<Collect*>(context);
    if (c.status != IpLayoutStatus::Ok) return;
    size_t index = 0;
    while (index < kNavi31BlockCount && requiredIds[index] != ip.hardwareId) ++index;
    if (index == kNavi31BlockCount) return;
    if (ip.dieId != 0 || ip.instance != 0) {
        fail(c, IpLayoutStatus::UnsupportedTopology, index, ip); return;
    }
    if (c.present[index]) { fail(c, IpLayoutStatus::DuplicateIp, index, ip); return; }
    c.present[index] = true;
    if (ip.major != requiredMajors[index] || ip.minor != 0 || ip.revision != 0 ||
        ip.subRevision != 0 || ip.variant != 0) {
        fail(c, IpLayoutStatus::UnsupportedIpVersion, index, ip); return;
    }
    if (ip.harvest != 0) { fail(c, IpLayoutStatus::HarvestedIp, index, ip); return; }
    if (ip.baseAddressCount > kNavi31MaxBases) {
        fail(c, IpLayoutStatus::BaseCapacityExceeded, index, ip); return;
    }
    const size_t minimum = index == static_cast<size_t>(Navi31Block::Gc) ? 2 : 1;
    if (ip.baseAddressCount < minimum) {
        fail(c, IpLayoutStatus::MissingBase, index, ip); return;
    }
    IpRegisterBlock& block = c.layout.blocks[index];
    block.hardwareId = ip.hardwareId; block.dieId = ip.dieId;
    block.instance = ip.instance; block.major = ip.major;
    block.minor = ip.minor; block.revision = ip.revision;
    block.baseCount = ip.baseAddressCount;
    for (size_t i = 0; i < block.baseCount; ++i) {
        // The final header check also rejects 64-bit encoding when its raw
        // values happen to fit 32 bits. Never silently truncate a raw address.
        if (ip.baseAddresses[i] > UINT32_MAX) {
            fail(c, IpLayoutStatus::UnsupportedAddressEncoding, index, ip); return;
        }
        block.basesDwords[i] = static_cast<uint32_t>(ip.baseAddresses[i]);
    }
}
} // namespace

IpLayoutStatus buildNavi31IpLayout(const uint8_t* data, size_t size,
    Navi31IpLayout& out, IpLayoutDiagnostic& diagnostic) {
    out = Navi31IpLayout{};
    diagnostic = IpLayoutDiagnostic{};
    diagnostic.block = Navi31Block::Count;
    Collect c = {};
    c.status = IpLayoutStatus::Ok;
    c.diagnostic = &diagnostic;
    DiscoveryInfo info = {};
    diagnostic.discoveryStatus = parseDiscovery(data, size, info, collect, &c);
    if (diagnostic.discoveryStatus != DiscoveryStatus::Ok)
        return IpLayoutStatus::DiscoveryRejected;
    if (info.baseAddresses64Bit) {
        diagnostic.block = Navi31Block::Count;
        return IpLayoutStatus::UnsupportedAddressEncoding;
    }
    if (info.dieCount != 1 || info.dies[0].id != 0) {
        diagnostic.block = Navi31Block::Count;
        return IpLayoutStatus::UnsupportedTopology;
    }
    if (c.status != IpLayoutStatus::Ok) return c.status;
    for (size_t i = 0; i < kNavi31BlockCount; ++i) {
        if (!c.present[i]) {
            diagnostic.block = static_cast<Navi31Block>(i);
            return IpLayoutStatus::MissingIp;
        }
    }
    c.layout.valid = true;
    out = c.layout;
    return IpLayoutStatus::Ok;
}

IpLayoutStatus resolveNavi31Register(const Navi31IpLayout& layout,
    Navi31Block block, uint32_t baseIndex, uint32_t registerOffsetDwords,
    uint64_t barBytes, ResolvedRegister& out) {
    out = ResolvedRegister{};
    if (!layout.valid) return IpLayoutStatus::InvalidLayout;
    const size_t index = static_cast<size_t>(block);
    if (index >= kNavi31BlockCount || block == Navi31Block::Sdma0 || block == Navi31Block::Sdma1)
        return IpLayoutStatus::InvalidBlock;
    const IpRegisterBlock& b = layout.blocks[index];
    if (b.baseCount > kNavi31MaxBases) return IpLayoutStatus::InvalidLayout;
    if (baseIndex >= b.baseCount) return IpLayoutStatus::MissingBase;
    const uint64_t dword = static_cast<uint64_t>(b.basesDwords[baseIndex]) + registerOffsetDwords;
    // The public callback address domain is a uint32 byte offset. Compute in
    // uint64 before narrowing, so neither addition nor multiplication wraps.
    if (dword > UINT32_MAX / 4u) return IpLayoutStatus::AddressOverflow;
    const uint64_t byte = dword * 4;
    if (byte > barBytes || barBytes - byte < 4) return IpLayoutStatus::OutsideBar;
    out.registerDword = static_cast<uint32_t>(dword);
    out.mmioByteOffset = static_cast<uint32_t>(byte);
    return IpLayoutStatus::Ok;
}

IpLayoutStatus resolveNavi31SdmaRegister(const Navi31IpLayout& layout,
    uint32_t engine, uint32_t sdma0OffsetDwords, uint64_t barBytes,
    ResolvedRegister& out) {
    out = ResolvedRegister{};
    if (!layout.valid) return IpLayoutStatus::InvalidLayout;
    if (engine > 1) return IpLayoutStatus::InvalidBlock;
    const bool hypervisor = sdma0OffsetDwords >= 0x5880u && sdma0OffsetDwords <= 0x589au;
    const uint64_t adjusted = static_cast<uint64_t>(sdma0OffsetDwords) +
        engine * (hypervisor ? 0x20u : 0x600u);
    if (adjusted > UINT32_MAX) return IpLayoutStatus::AddressOverflow;
    return resolveNavi31Register(layout, Navi31Block::Gc, hypervisor ? 1u : 0u,
                                static_cast<uint32_t>(adjusted), barBytes, out);
}

const char* ipLayoutStatusString(IpLayoutStatus status) {
    switch (status) {
    case IpLayoutStatus::Ok: return "ok";
    case IpLayoutStatus::DiscoveryRejected: return "discovery parser rejected input";
    case IpLayoutStatus::UnsupportedAddressEncoding: return "unsupported discovery address encoding";
    case IpLayoutStatus::MissingIp: return "required IP missing";
    case IpLayoutStatus::DuplicateIp: return "duplicate required IP";
    case IpLayoutStatus::UnsupportedTopology: return "unsupported die or IP instance";
    case IpLayoutStatus::UnsupportedIpVersion: return "unsupported required IP version";
    case IpLayoutStatus::HarvestedIp: return "required IP has harvest flags";
    case IpLayoutStatus::BaseCapacityExceeded: return "register base capacity exceeded";
    case IpLayoutStatus::MissingBase: return "required register base missing";
    case IpLayoutStatus::InvalidLayout: return "invalid IP layout";
    case IpLayoutStatus::InvalidBlock: return "invalid register block or SDMA engine";
    case IpLayoutStatus::AddressOverflow: return "MMIO byte offset exceeds uint32";
    case IpLayoutStatus::OutsideBar: return "complete register access lies outside BAR";
    }
    return "unknown IP layout status";
}

const char* navi31BlockString(Navi31Block block) {
    switch (block) {
    case Navi31Block::Gc: return "GC";
    case Navi31Block::Mmhub: return "MMHUB";
    case Navi31Block::Mp0: return "MP0";
    case Navi31Block::Mp1: return "MP1";
    case Navi31Block::Sdma0: return "SDMA0";
    case Navi31Block::Sdma1: return "SDMA1";
    case Navi31Block::Count: return "none";
    }
    return "unknown";
}
} // namespace hackynavi
