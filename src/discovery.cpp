// SPDX-License-Identifier: MIT
// Original parser. Wire format reference (AMD MIT-licensed declarations):
// https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/discovery.h
// Checksum/absolute die-offset interpretation:
// https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
#include "hackynavi/discovery.hpp"
namespace hackynavi {
namespace {
bool span(size_t limit, size_t offset, size_t length) {
    return offset <= limit && length <= limit - offset;
}
uint16_t u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t u64(const uint8_t* p) { return u32(p) | (uint64_t(u32(p + 4)) << 32); }
uint16_t sum(const uint8_t* p, size_t size) {
    uint16_t result = 0;
    for (size_t i = 0; i < size; ++i) result = static_cast<uint16_t>(result + p[i]);
    return result;
}
bool overlaps(size_t a, size_t alen, size_t b, size_t blen) {
    return a < b + blen && b < a + alen;
}
DiscoveryStatus walk(const uint8_t* data, DiscoveryInfo& info,
                     DiscoveryVisitor visitor, void* context) {
    const size_t tableEnd = size_t(info.tables[0].offset) + info.tables[0].size;
    const size_t stride = info.baseAddresses64Bit ? 8 : 4;
    uint32_t count = 0;
    for (size_t die = 0; die < info.dieCount; ++die) {
        const DiscoveryDie& d = info.dies[die];
        size_t end = tableEnd;
        for (size_t other = 0; other < info.dieCount; ++other)
            if (info.dies[other].offset > d.offset && info.dies[other].offset < end)
                end = info.dies[other].offset;
        size_t cursor = size_t(d.offset) + 4;
        for (size_t index = 0; index < d.ipCount; ++index) {
            if (!span(end, cursor, 8)) return DiscoveryStatus::Truncated;
            const uint8_t* p = data + cursor;
            const size_t length = 8 + size_t(p[3]) * stride;
            if (!span(end, cursor, length)) return DiscoveryStatus::Truncated;
            if (visitor) {
                DiscoveryIp ip = {};
                ip.dieId = d.id;
                ip.offset = static_cast<uint16_t>(cursor);
                ip.hardwareId = u16(p);
                ip.instance = p[2]; ip.baseAddressCount = p[3];
                ip.major = p[4]; ip.minor = p[5]; ip.revision = p[6];
                if (info.ipVersion <= 2) ip.harvest = p[7] & 15;
                else { ip.subRevision = p[7] & 15; ip.variant = p[7] >> 4; }
                for (size_t base = 0; base < ip.baseAddressCount; ++base)
                    ip.baseAddresses[base] = stride == 8 ? u64(p + 8 + base * stride) :
                                                          u32(p + 8 + base * stride);
                visitor(ip, context);
            }
            cursor += length;
            ++count;
        }
    }
    info.ipCount = count;
    return DiscoveryStatus::Ok;
}
} // namespace
DiscoveryStatus parseDiscovery(const uint8_t* data, size_t size, DiscoveryInfo& out,
                                DiscoveryVisitor visitor, void* context) {
    out = DiscoveryInfo{};
    if (!data) return DiscoveryStatus::InvalidArgument;
    if (size < 12) return DiscoveryStatus::Truncated;
    if (u32(data) != 0x28211407U) return DiscoveryStatus::BadSignature;
    DiscoveryInfo info = {};
    info.binaryMajor = u16(data + 4); info.binaryMinor = u16(data + 6);
    info.binarySize = u16(data + 10);
    if (info.binarySize < 12) return DiscoveryStatus::Malformed;
    if (info.binarySize > size) return DiscoveryStatus::Truncated;
    if (sum(data + 10, info.binarySize - 10) != u16(data + 8))
        return DiscoveryStatus::ChecksumMismatch;
    size_t directory = 12;
    if (info.binaryMajor == 0 || info.binaryMajor == 1) info.tableCount = 6;
    else if (info.binaryMajor == 2) {
        if (info.binarySize < 16) return DiscoveryStatus::Truncated;
        info.tableCount = u16(data + 12); directory = 16;
    } else { out = info; return DiscoveryStatus::UnsupportedVersion; }
    if (info.tableCount == 0) return DiscoveryStatus::Malformed;
    if (info.tableCount > kMaxDiscoveryTables) return DiscoveryStatus::CapacityExceeded;
    const size_t directoryEnd = directory + size_t(info.tableCount) * 8;
    if (directoryEnd > info.binarySize) return DiscoveryStatus::Truncated;
    for (size_t i = 0; i < info.tableCount; ++i) {
        const uint8_t* entry = data + directory + i * 8;
        DiscoveryTable& t = info.tables[i];
        t.offset = u16(entry); t.checksum = u16(entry + 2); t.size = u16(entry + 4);
        if (!t.offset) {
            if (t.size || t.checksum) return DiscoveryStatus::Malformed;
            continue;
        }
        if (!t.size || t.offset < directoryEnd) return DiscoveryStatus::Malformed;
        if (!span(info.binarySize, t.offset, t.size)) return DiscoveryStatus::Truncated;
        for (size_t j = 0; j < i; ++j)
            if (info.tables[j].offset && overlaps(t.offset, t.size, info.tables[j].offset, info.tables[j].size))
                return DiscoveryStatus::Malformed;
        if (sum(data + t.offset, t.size) != t.checksum) return DiscoveryStatus::ChecksumMismatch;
    }
    const DiscoveryTable& ipTable = info.tables[0];
    if (!ipTable.offset || ipTable.size < 8) return DiscoveryStatus::Malformed;
    const uint8_t* table = data + ipTable.offset;
    if (u32(table) != 0x53445049U) return DiscoveryStatus::BadSignature;
    if (u16(table + 6) != ipTable.size) return DiscoveryStatus::Malformed;
    info.ipVersion = u16(table + 4);
    if (info.ipVersion < 1 || info.ipVersion > 4) {
        out = info; return DiscoveryStatus::UnsupportedVersion;
    }
    if (ipTable.size < 80) return DiscoveryStatus::Malformed;
    info.ipTableId = u32(table + 8); info.dieCount = u16(table + 12);
    if (info.dieCount > kMaxDiscoveryDies) return DiscoveryStatus::CapacityExceeded;
    info.baseAddresses64Bit = info.ipVersion == 4 && (table[78] & 1) != 0;
    const size_t tableEnd = size_t(ipTable.offset) + ipTable.size;
    for (size_t i = 0; i < info.dieCount; ++i) {
        DiscoveryDie& d = info.dies[i];
        d.id = u16(table + 14 + i * 4); d.offset = u16(table + 16 + i * 4);
        if (d.offset < size_t(ipTable.offset) + 80) return DiscoveryStatus::Malformed;
        if (!span(tableEnd, d.offset, 4)) return DiscoveryStatus::Truncated;
        for (size_t j = 0; j < i; ++j)
            if (d.id == info.dies[j].id || overlaps(d.offset, 4, info.dies[j].offset, 4))
                return DiscoveryStatus::Malformed;
        if (u16(data + d.offset) != d.id) return DiscoveryStatus::Malformed;
        d.ipCount = u16(data + d.offset + 2);
    }
    DiscoveryStatus status = walk(data, info, nullptr, nullptr);
    if (status != DiscoveryStatus::Ok) return status;
    if (visitor) {
        status = walk(data, info, visitor, context);
        if (status != DiscoveryStatus::Ok) return status;
    }
    out = info;
    return DiscoveryStatus::Ok;
}
const char* discoveryStatusString(DiscoveryStatus s) {
    switch (s) {
    case DiscoveryStatus::Ok: return "ok";
    case DiscoveryStatus::InvalidArgument: return "invalid argument";
    case DiscoveryStatus::Truncated: return "truncated";
    case DiscoveryStatus::BadSignature: return "bad signature";
    case DiscoveryStatus::Malformed: return "malformed";
    case DiscoveryStatus::UnsupportedVersion: return "unsupported version";
    case DiscoveryStatus::ChecksumMismatch: return "checksum mismatch";
    case DiscoveryStatus::CapacityExceeded: return "capacity exceeded";
    }
    return "unknown status";
}
} // namespace hackynavi
