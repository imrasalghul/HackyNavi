// SPDX-License-Identifier: MIT
// Synthetic register-layout tests. Optional argument: real discovery binary.
// No hardware is read or written; ROM metadata does not establish live state.
#include "hackynavi/ip_layout.hpp"
#include <fstream>
#include <iostream>
#include <vector>

using namespace hackynavi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "line " << __LINE__ << ": " #x "\n"; } } while (0)
struct Ip {
    uint16_t hw;
    uint8_t instance, major, minor, revision, flags;
    std::vector<uint64_t> bases;
};
std::vector<Ip> records() {
    const uint16_t ids[] = {11, 34, 255, 1, 42, 43};
    const uint8_t versions[] = {11, 3, 13, 13, 6, 6};
    std::vector<Ip> result;
    for (size_t i = 0; i < 6; ++i) {
        Ip ip = {ids[i], 0, versions[i], 0, 0, 0, {}};
        for (size_t base = 0; base < 8; ++base)
            ip.bases.push_back((i + 1) * 0x1000 + base * 0x100);
        result.push_back(ip);
    }
    return result;
}
void put16(std::vector<uint8_t>& b, size_t p, uint16_t v) {
    b[p] = static_cast<uint8_t>(v); b[p + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t p, uint32_t v) {
    put16(b, p, static_cast<uint16_t>(v)); put16(b, p + 2, static_cast<uint16_t>(v >> 16));
}
uint16_t sum(const std::vector<uint8_t>& b, size_t first) {
    uint16_t value = 0;
    for (size_t i = first; i < b.size(); ++i) value = static_cast<uint16_t>(value + b[i]);
    return value;
}
void fix(std::vector<uint8_t>& b) {
    put16(b, 14, sum(b, 60));
    put16(b, 8, sum(b, 10));
}
std::vector<uint8_t> fixture(const std::vector<Ip>& ips, uint16_t version = 3,
                             bool wide = false, uint16_t dieId = 0) {
    size_t bytes = 144;
    for (const Ip& ip : ips) bytes += 8 + ip.bases.size() * (wide ? 8 : 4);
    std::vector<uint8_t> b(bytes, 0);
    put32(b, 0, 0x28211407); put16(b, 4, 1); put16(b, 6, 4);
    put16(b, 10, static_cast<uint16_t>(bytes));
    put16(b, 12, 60); put16(b, 16, static_cast<uint16_t>(bytes - 60));
    put32(b, 60, 0x53445049); put16(b, 64, version);
    put16(b, 66, static_cast<uint16_t>(bytes - 60));
    put16(b, 72, 1); put16(b, 74, dieId); put16(b, 76, 140);
    b[138] = wide ? 1 : 0;
    put16(b, 140, dieId); put16(b, 142, static_cast<uint16_t>(ips.size()));
    size_t cursor = 144;
    for (const Ip& ip : ips) {
        put16(b, cursor, ip.hw); b[cursor + 2] = ip.instance;
        b[cursor + 3] = static_cast<uint8_t>(ip.bases.size());
        b[cursor + 4] = ip.major; b[cursor + 5] = ip.minor;
        b[cursor + 6] = ip.revision; b[cursor + 7] = ip.flags;
        cursor += 8;
        for (uint64_t base : ip.bases) {
            put32(b, cursor, static_cast<uint32_t>(base));
            if (wide) put32(b, cursor + 4, static_cast<uint32_t>(base >> 32));
            cursor += wide ? 8 : 4;
        }
    }
    fix(b);
    return b;
}
bool zero(const Navi31IpLayout& layout) {
    if (layout.valid) return false;
    for (const IpRegisterBlock& b : layout.blocks) {
        if (b.hardwareId || b.dieId || b.instance || b.major || b.minor || b.revision || b.baseCount)
            return false;
        for (uint32_t base : b.basesDwords) if (base) return false;
    }
    return true;
}
void rejected(const std::vector<uint8_t>& bytes, IpLayoutStatus expected,
              Navi31Block block = Navi31Block::Count) {
    Navi31IpLayout layout{};
    layout.valid = true; layout.blocks[0].basesDwords[0] = 0xdeadbeef;
    IpLayoutDiagnostic diagnostic{};
    CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), layout, diagnostic) == expected);
    CHECK(zero(layout));
    CHECK(diagnostic.block == block);
    if (expected != IpLayoutStatus::DiscoveryRejected)
        CHECK(diagnostic.discoveryStatus == DiscoveryStatus::Ok);
}
Navi31IpLayout validLayout() {
    const auto bytes = fixture(records());
    Navi31IpLayout layout{}; IpLayoutDiagnostic diagnostic{};
    CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), layout, diagnostic) == IpLayoutStatus::Ok);
    CHECK(layout.valid && diagnostic.block == Navi31Block::Count);
    return layout;
}

void layoutValidation() {
    for (uint16_t version = 1; version <= 4; ++version) {
        const auto bytes = fixture(records(), version);
        Navi31IpLayout layout{}; IpLayoutDiagnostic diagnostic{};
        CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), layout, diagnostic) == IpLayoutStatus::Ok);
        for (size_t block = 0; block < 6; ++block) {
            const IpRegisterBlock& b = layout.blocks[block];
            CHECK(b.hardwareId == records()[block].hw && b.dieId == 0 && b.instance == 0);
            CHECK(b.major == records()[block].major && b.minor == 0 && b.revision == 0);
            CHECK(b.baseCount == 8);
            for (size_t index = 0; index < 8; ++index)
                CHECK(b.basesDwords[index] == (block + 1) * 0x1000 + index * 0x100);
        }
    }
    // Record order does not establish block identity. Unneeded HWIDs are ignored.
    auto ips = records();
    const Ip first = ips[0]; ips[0] = ips[5]; ips[5] = first;
    ips.push_back(Ip{1000, 99, 255, 255, 255, 255, std::vector<uint64_t>(32, 0)});
    ips.push_back(ips.back());
    Navi31IpLayout layout{}; IpLayoutDiagnostic diagnostic{};
    auto bytes = fixture(ips);
    CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), layout, diagnostic) == IpLayoutStatus::Ok);
    CHECK(layout.blocks[0].hardwareId == 11 && layout.blocks[5].hardwareId == 43);
    for (size_t i = 0; i < 6; ++i) {
        const Navi31Block block = static_cast<Navi31Block>(i);
        ips = records(); ips.erase(ips.begin() + i);
        rejected(fixture(ips), IpLayoutStatus::MissingIp, block);
        ips = records(); ips.push_back(ips[i]);
        rejected(fixture(ips), IpLayoutStatus::DuplicateIp, block);
        ips = records(); ips[i].instance = 1;
        rejected(fixture(ips), IpLayoutStatus::UnsupportedTopology, block);
        for (unsigned field = 0; field < 5; ++field) {
            ips = records();
            if (field == 0) ++ips[i].major;
            if (field == 1) ips[i].minor = 1;
            if (field == 2) ips[i].revision = 1;
            if (field == 3) ips[i].flags = 1; // subrevision
            if (field == 4) ips[i].flags = 16; // variant
            rejected(fixture(ips), IpLayoutStatus::UnsupportedIpVersion, block);
        }
        ips = records(); ips[i].flags = 1;
        rejected(fixture(ips, 1), IpLayoutStatus::HarvestedIp, block);
        rejected(fixture(ips, 2), IpLayoutStatus::HarvestedIp, block);
        ips = records(); ips[i].bases.clear();
        rejected(fixture(ips), IpLayoutStatus::MissingBase, block);
        ips = records(); ips[i].bases.push_back(1);
        rejected(fixture(ips), IpLayoutStatus::BaseCapacityExceeded, block);
    }
    ips = records(); ips[0].bases.resize(1);
    rejected(fixture(ips), IpLayoutStatus::MissingBase, Navi31Block::Gc);
    rejected(fixture(records(), 3, false, 1), IpLayoutStatus::UnsupportedTopology);
    // Reject wide encoding even when every raw address fits in 32 bits.
    rejected(fixture(records(), 4, true), IpLayoutStatus::UnsupportedAddressEncoding);
    ips = records(); ips[0].bases[0] = UINT64_C(0xabcdef0012345678);
    rejected(fixture(ips, 4, true), IpLayoutStatus::UnsupportedAddressEncoding);
    bytes = fixture(records()); bytes.back() ^= 1;
    rejected(bytes, IpLayoutStatus::DiscoveryRejected);
    CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), layout, diagnostic) == IpLayoutStatus::DiscoveryRejected);
    CHECK(diagnostic.discoveryStatus == DiscoveryStatus::ChecksumMismatch);
    CHECK(buildNavi31IpLayout(nullptr, 100, layout, diagnostic) == IpLayoutStatus::DiscoveryRejected);
    CHECK(diagnostic.discoveryStatus == DiscoveryStatus::InvalidArgument && zero(layout));
    bytes = fixture(records());
    for (size_t length = 0; length < bytes.size(); ++length) {
        CHECK(buildNavi31IpLayout(bytes.data(), length, layout, diagnostic) == IpLayoutStatus::DiscoveryRejected);
        CHECK(zero(layout) && diagnostic.discoveryStatus == DiscoveryStatus::Truncated);
    }
}

void genericResolution() {
    const Navi31IpLayout layout = validLayout();
    ResolvedRegister resolved{};
    for (size_t block = 0; block < 4; ++block) for (uint32_t index = 0; index < 8; ++index) {
        const uint32_t expected = static_cast<uint32_t>((block + 1) * 0x1000 + index * 0x100 + 0x123);
        CHECK(resolveNavi31Register(layout, static_cast<Navi31Block>(block), index,
              0x123, 1 << 20, resolved) == IpLayoutStatus::Ok);
        CHECK(resolved.registerDword == expected && resolved.mmioByteOffset == expected * 4);
        CHECK(resolveNavi31Register(layout, static_cast<Navi31Block>(block), index,
              0x123, static_cast<uint64_t>(expected) * 4 + 4, resolved) == IpLayoutStatus::Ok);
        CHECK(resolveNavi31Register(layout, static_cast<Navi31Block>(block), index,
              0x123, static_cast<uint64_t>(expected) * 4 + 3, resolved) == IpLayoutStatus::OutsideBar);
        CHECK(resolved.registerDword == 0 && resolved.mmioByteOffset == 0);
    }
    CHECK(resolveNavi31Register(layout, Navi31Block::Gc, 8, 0, UINT64_MAX, resolved) == IpLayoutStatus::MissingBase);
    CHECK(resolveNavi31Register(layout, Navi31Block::Gc, UINT32_MAX, 0, UINT64_MAX, resolved) == IpLayoutStatus::MissingBase);
    CHECK(resolveNavi31Register(layout, Navi31Block::Gc, 0, UINT32_MAX, UINT64_MAX, resolved) == IpLayoutStatus::AddressOverflow);
    CHECK(resolveNavi31Register(layout, Navi31Block::Gc, 0, 0, 0, resolved) == IpLayoutStatus::OutsideBar);
    for (unsigned id : {4u, 5u, 6u, 255u})
        CHECK(resolveNavi31Register(layout, static_cast<Navi31Block>(id), 0, 0,
              UINT64_MAX, resolved) == IpLayoutStatus::InvalidBlock);
    // Successful build preserves any representable DWORD base; the selected
    // register is checked at resolution time, including large indirect regions.
    auto ips = records(); ips[0].bases[0] = 0x3fffffffu;
    auto bytes = fixture(ips); Navi31IpLayout boundary{}; IpLayoutDiagnostic diagnostic{};
    CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), boundary, diagnostic) == IpLayoutStatus::Ok);
    CHECK(resolveNavi31Register(boundary, Navi31Block::Gc, 0, 0,
          UINT64_C(0x100000000), resolved) == IpLayoutStatus::Ok);
    CHECK(resolved.mmioByteOffset == 0xfffffffcu);
    CHECK(resolveNavi31Register(boundary, Navi31Block::Gc, 0, 1,
          UINT64_MAX, resolved) == IpLayoutStatus::AddressOverflow);
    ips[0].bases[0] = UINT32_MAX; bytes = fixture(ips);
    CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), boundary, diagnostic) == IpLayoutStatus::Ok);
    CHECK(resolveNavi31Register(boundary, Navi31Block::Gc, 0, 1,
          UINT64_MAX, resolved) == IpLayoutStatus::AddressOverflow);
    boundary = Navi31IpLayout{};
    CHECK(resolveNavi31Register(boundary, Navi31Block::Gc, 0, 0,
          UINT64_MAX, resolved) == IpLayoutStatus::InvalidLayout);
    boundary = layout; boundary.blocks[0].baseCount = 255;
    CHECK(resolveNavi31Register(boundary, Navi31Block::Gc, 200, 0,
          UINT64_MAX, resolved) == IpLayoutStatus::InvalidLayout);
    CHECK(resolved.registerDword == 0 && resolved.mmioByteOffset == 0);
}

void sdmaResolution() {
    const Navi31IpLayout layout = validLayout();
    ResolvedRegister resolved{};
    const uint32_t offsets[] = {0, 0x34, 0x100, 0x587f, 0x5880, 0x5881, 0x589a, 0x589b};
    for (uint32_t engine = 0; engine < 2; ++engine) for (uint32_t offset : offsets) {
        const bool hyp = offset >= 0x5880 && offset <= 0x589a;
        const uint32_t expected = (hyp ? 0x1100 : 0x1000) + offset + engine * (hyp ? 0x20 : 0x600);
        CHECK(resolveNavi31SdmaRegister(layout, engine, offset, 1 << 20, resolved) == IpLayoutStatus::Ok);
        CHECK(resolved.registerDword == expected && resolved.mmioByteOffset == expected * 4);
        // SDMA records deliberately contain other bases (0x5000 / 0x6000).
        CHECK(resolved.registerDword != layout.blocks[4 + engine].basesDwords[0] + offset);
        CHECK(resolveNavi31SdmaRegister(layout, engine, offset,
              static_cast<uint64_t>(expected) * 4 + 3, resolved) == IpLayoutStatus::OutsideBar);
        CHECK(resolved.registerDword == 0 && resolved.mmioByteOffset == 0);
    }
    CHECK(resolveNavi31SdmaRegister(layout, 2, 0, UINT64_MAX, resolved) == IpLayoutStatus::InvalidBlock);
    CHECK(resolveNavi31SdmaRegister(layout, UINT32_MAX, 0, UINT64_MAX, resolved) == IpLayoutStatus::InvalidBlock);
    CHECK(resolveNavi31SdmaRegister(layout, 1, UINT32_MAX, UINT64_MAX, resolved) == IpLayoutStatus::AddressOverflow);
    CHECK(resolveNavi31SdmaRegister(layout, 1, 0xfffffa00u, UINT64_MAX, resolved) == IpLayoutStatus::AddressOverflow);
    CHECK(resolveNavi31SdmaRegister(Navi31IpLayout{}, 0, 0,
          UINT64_MAX, resolved) == IpLayoutStatus::InvalidLayout);
}

void realBinary(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(bool(input)); if (!input) return;
    const std::streamoff size = input.tellg();
    CHECK(size > 0 && size <= 65535); if (size <= 0 || size > 65535) return;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), size);
    CHECK(bool(input)); if (!input) return;
    Navi31IpLayout layout{}; IpLayoutDiagnostic diagnostic{};
    CHECK(buildNavi31IpLayout(bytes.data(), bytes.size(), layout, diagnostic) == IpLayoutStatus::Ok);
    if (!layout.valid) return;
    CHECK(layout.blocks[0].hardwareId == 11 && layout.blocks[1].hardwareId == 34);
    CHECK(layout.blocks[4].hardwareId == 42 && layout.blocks[5].hardwareId == 43);
    ResolvedRegister resolved{};
    // Generated MMHUB3 FB_LOCATION_BASE, offset 0x8ec, base-index 0. Expected
    // base comes from the supplied record, never a copied ROM base constant.
    CHECK(resolveNavi31Register(layout, Navi31Block::Mmhub, 0, 0x8ec,
          1 << 20, resolved) == IpLayoutStatus::Ok);
    CHECK(resolved.mmioByteOffset == (layout.blocks[1].basesDwords[0] + 0x8ec) * 4);
    CHECK(resolveNavi31SdmaRegister(layout, 1, 0x34, 1 << 20, resolved) == IpLayoutStatus::Ok);
    CHECK(resolved.registerDword == layout.blocks[0].basesDwords[0] + 0x34 + 0x600);
    std::cout << "Validated optional discovery fixture: " << path << "\n";
}
} // namespace

int main(int argc, char** argv) {
    layoutValidation(); genericResolution(); sdmaResolution();
    if (argc == 2) realBinary(argv[1]);
    else CHECK(argc == 1);
    CHECK(ipLayoutStatusString(IpLayoutStatus::Ok)[0] == 'o');
    CHECK(navi31BlockString(Navi31Block::Sdma1)[0] == 'S');
    std::cout << checks << " assertions, " << failures << " failures; no hardware access.\n";
    return failures ? 1 : 0;
}
