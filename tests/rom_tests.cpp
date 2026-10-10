// SPDX-License-Identifier: MIT
// Synthetic wire-format fixtures and adversarial bounds tests. No firmware blobs.
#include "hackynavi/rom.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace hackynavi;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::cerr << "FAIL line " << __LINE__ << ": " #expr "\n"; } } while (0)

void put16(std::vector<uint8_t>& b, size_t at, uint16_t value) {
    b[at] = static_cast<uint8_t>(value); b[at + 1] = static_cast<uint8_t>(value >> 8);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t value) {
    put16(b, at, static_cast<uint16_t>(value)); put16(b, at + 2, static_cast<uint16_t>(value >> 16));
}
void header(std::vector<uint8_t>& b, size_t at, uint16_t size, uint8_t major, uint8_t minor) {
    put16(b, at, size); b[at + 2] = major; b[at + 3] = minor;
}
void checksum(std::vector<uint8_t>& b) {
    b[0x60] = 0;
    unsigned sum = 0;
    for (uint8_t v : b) sum += v;
    b[0x60] = static_cast<uint8_t>(0u - sum);
}
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> b(4096, 0);
    b[0] = 0x55; b[1] = 0xaa; b[2] = 8;
    put16(b, 0x18, 0x80);
    std::memcpy(&b[0x80], "PCIR", 4);
    put16(b, 0x84, 0x1002); put16(b, 0x86, 0x744c);
    put16(b, 0x8a, 24); put16(b, 0x90, 8); b[0x95] = 0x80;
    put16(b, 0x48, 0x100);
    header(b, 0x100, 44, 2, 3); std::memcpy(&b[0x104], "ATOM", 4);
    put16(b, 0x118, 0x148c); put16(b, 0x11a, 0x2422);
    put16(b, 0x11e, 0x190); put16(b, 0x120, 0x140);
    header(b, 0x190, 8, 2, 1); header(b, 0x140, 74, 2, 1);
    put16(b, 0x144 + kFirmwareInfoTableIndex * 2, 0x600);
    put16(b, 0x144 + kVramUsageTableIndex * 2, 0x700);
    header(b, 0x600, 108, 3, 4);
    put32(b, 0x604, 0x12345678); put32(b, 0x610, 0x104400);
    put32(b, 0x618, 0x2c90);
    put32(b, 0x654, 4096);
    header(b, 0x700, 12, 2, 1);
    put32(b, 0x704, 0x80012345); put16(b, 0x708, 32); put16(b, 0x70a, 20);
    put16(b, 0x144 + 22 * 2, 0x500); put16(b, 0x144 + 28 * 2, 0x200);
    header(b, 0x200, 200, 3, 0);
    b[0x214] = 2; b[0x218] = 0x70; b[0x219] = 24; b[0x21a] = 4;
    for (size_t i = 0; i < 2; ++i) {
        const size_t m = 0x240 + i * 68;
        b[m] = 0x72; b[m + 3] = static_cast<uint8_t>(i + 1);
        put32(b, m + 12, 1024);
        std::memcpy(&b[m + 28], i ? "TEST-PART-B" : "TEST-PART-A", 11);
    }
    header(b, 0x500, 200, 1, 5); put16(b, 0x504, 0x688); b[0x506] = 4;
    for (size_t i = 0; i < 4; ++i) {
        const size_t p = 0x508 + i * 16, record = 72 + i * 32;
        put16(b, p, static_cast<uint16_t>(0x3113 + i * 0x100));
        put16(b, p + 2, static_cast<uint16_t>(record)); put16(b, p + 4, 0x211e);
        put16(b, p + 12, static_cast<uint16_t>(1u << i));
        b[0x500 + record] = 0xff; // terminal FF00
    }
    checksum(b);
    return b;
}
ImageInfo imageFor(const std::vector<uint8_t>& b) {
    ImageInfo image = {};
    CHECK(parseImage(b.data(), b.size(), 0, image) == Status::Ok);
    return image;
}
void basics() {
    std::vector<uint8_t> b = fixture();
    ImageInfo image = imageFor(b);
    CHECK(image.vendorId == 0x1002 && image.deviceId == 0x744c);
    CHECK(image.checksumApplicable && image.checksumValid && image.lastImage);
    AtomInfo a = {}; VramInfo v = {}; DisplayInfo d = {};
    CHECK(parseAtom(b.data(), b.size(), image, a) == Status::Ok);
    CHECK(a.header.contentRevision == 3 && a.subsystemVendorId == 0x148c);
    CHECK(a.tables[kVramTableIndex].offset == 0x200);
    CHECK(parseVram(b.data(), b.size(), image, v) == Status::Ok);
    CHECK(v.busWidthBits == 384 && v.channelCount == 24 && v.moduleCount == 2);
    CHECK(v.modules[1].sizePerChannel == 1024);
    CHECK(std::strcmp(v.modules[0].partNumber, "TEST-PART-A") == 0);
    CHECK(parseDisplay(b.data(), b.size(), image, d) == Status::Ok);
    CHECK(d.pathCount == 4 && d.paths[3].recordOffset == 168);
    b[0x61] ^= 1;
    CHECK(parseImage(b.data(), b.size(), 0, image) == Status::Ok);
    CHECK(!image.checksumValid); // checksum is diagnostic, not a rejection gate
    b[0x94] = 3;
    CHECK(parseImage(b.data(), b.size(), 0, image) == Status::Ok);
    CHECK(!image.checksumApplicable);
    CHECK(parseAtom(b.data(), b.size(), image, a) == Status::NotFound);
    CHECK(a.header.offset == 0);
    CHECK(parseImage(nullptr, 4096, 0, image) == Status::InvalidArgument);
    CHECK(parseImage(b.data(), b.size(), static_cast<size_t>(-1), image) == Status::InvalidArgument);
}
void scans() {
    const std::vector<uint8_t> b = fixture();
    std::vector<uint8_t> dump(123, 0);
    dump[0] = 0x55; dump[1] = 0xaa; // misleading broken candidate before valid image
    dump.insert(dump.end(), b.begin(), b.end());
    dump.insert(dump.end(), 73, 0);
    dump.insert(dump.end(), b.begin(), b.end());
    ImageInfo image = {};
    CHECK(findNextImage(dump.data(), dump.size(), 0, image) == Status::Ok);
    CHECK(image.offset == 123 && image.size == 4096);
    VramInfo v = {};
    CHECK(parseVram(dump.data(), dump.size(), image, v) == Status::Ok);
    CHECK(findNextImage(dump.data(), dump.size(), image.offset + image.size, image) == Status::Ok);
    CHECK(image.offset == 123 + 4096 + 73);
    CHECK(findNextImage(dump.data(), dump.size(), image.offset + image.size, image) == Status::NotFound);
}
void malformed() {
    const std::vector<uint8_t> original = fixture();
    ImageInfo image = imageFor(original);
    AtomInfo a = {}; VramInfo v = {}; DisplayInfo d = {};
    std::vector<uint8_t> b = original;
    b[0x102] = 9;
    CHECK(parseAtom(b.data(), b.size(), image, a) == Status::UnsupportedVersion);
    CHECK(a.header.formatRevision == 9 && a.subsystemVendorId == 0);
    b = original; b[0x142] = 9;
    CHECK(parseAtom(b.data(), b.size(), image, a) == Status::UnsupportedVersion);
    CHECK(a.tables[28].offset == 0);
    b = original; b[0x203] = 9;
    CHECK(parseVram(b.data(), b.size(), image, v) == Status::UnsupportedVersion);
    CHECK(v.table.contentRevision == 9 && v.moduleCount == 0);
    b = original; b[0x503] = 4;
    CHECK(parseDisplay(b.data(), b.size(), image, d) == Status::UnsupportedVersion);
    CHECK(d.pathCount == 0);
    b = original; b[0x214] = 255;
    CHECK(parseVram(b.data(), b.size(), image, v) != Status::Ok);
    CHECK(v.moduleCount == 0);
    b = original; b[0x506] = 255;
    CHECK(parseDisplay(b.data(), b.size(), image, d) != Status::Ok);
    b = original; put16(b, 0x144 + 28 * 2, 4094); b.resize(8192, 0);
    header(b, 4094, 200, 3, 0);
    CHECK(parseAtom(b.data(), b.size(), image, a) != Status::Ok); // cannot escape image into later dump
    b = original; put16(b, 0x140, 73);
    CHECK(parseAtom(b.data(), b.size(), image, a) == Status::Malformed);
    b = original; put16(b, 0x120, 0);
    CHECK(parseAtom(b.data(), b.size(), image, a) == Status::Malformed);
    b = original; put16(b, 0x90, 0);
    CHECK(parseImage(b.data(), b.size(), 0, image) == Status::Malformed);
    image = imageFor(original);
    b = original; put16(b, 0x8a, 0xffff);
    CHECK(parseImage(b.data(), b.size(), 0, image) == Status::Malformed);
    image = imageFor(original);
    b = original; put16(b, 0x50a, 199); // incomplete record header
    CHECK(parseDisplay(b.data(), b.size(), image, d) != Status::Ok);
    b = original; b[0x548] = 1; b[0x549] = 0; // record makes no forward progress
    CHECK(parseDisplay(b.data(), b.size(), image, d) == Status::Malformed);
    b = original; put16(b, 0x244, 199); // module pointer overlaps module definitions
    CHECK(parseVram(b.data(), b.size(), image, v) == Status::Malformed);
    b = original; std::memset(&b[0x240 + 28], 'X', 40);
    CHECK(parseVram(b.data(), b.size(), image, v) == Status::Ok);
    CHECK(std::strlen(v.modules[0].partNumber) == 40);
    b[0x240 + 28] = 0x1b;
    CHECK(parseVram(b.data(), b.size(), image, v) == Status::Ok);
    CHECK(v.modules[0].partNumber[0] == '?');
    // A hostile caller cannot enlarge the image boundary with a forged descriptor.
    b = original; image.size = static_cast<size_t>(-1); image.pcirOffset = static_cast<size_t>(-1);
    CHECK(parseVram(b.data(), b.size(), image, v) == Status::Ok);
    CHECK(v.busWidthBits == 384);
}

bool firmwarePayloadZero(const FirmwareInfo& f) {
    return !f.firmwareRevision && !f.firmwareCapability && !f.biosScratchRegisterDword && !f.firmwareReservedKiB &&
        !f.twoStageMemoryTraining && !f.protectedRegionSizePresent && !f.protectedRegionSizeKiB;
}
bool regionZero(const VramUsageRegion& r) {
    return !r.startAddressPresent && !r.startAddressKiBRaw && !r.startAddressKiB &&
        !r.sizeKiB && !r.operationFlags;
}
void reservationMetadata() {
    const std::vector<uint8_t> original = fixture();
    const ImageInfo image = imageFor(original);
    auto b = original;
    FirmwareInfo f = {}; VramUsageInfo u = {};
    CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::Ok);
    CHECK(f.present && f.table.offset == 0x600 && f.table.contentRevision == 4);
    CHECK(f.firmwareRevision == 0x12345678 && f.firmwareCapability == 0x104400);
    CHECK(f.biosScratchRegisterDword == 0x2c90);
    CHECK(f.firmwareReservedKiB == 4096 && f.twoStageMemoryTraining);
    CHECK(!f.protectedRegionSizePresent && f.protectedRegionSizeKiB == 0);
    CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::Ok);
    CHECK(u.present && u.table.offset == 0x700 && u.table.contentRevision == 1);
    CHECK(u.firmware.startAddressPresent && u.firmware.startAddressKiBRaw == 0x80012345 &&
          u.firmware.startAddressKiB == 0x12345 && u.firmware.operationFlags == 2 &&
          u.firmware.sizeKiB == 32);
    CHECK(!u.driver.startAddressPresent && u.driver.sizeKiB == 20 &&
          u.driver.startAddressKiBRaw == 0 && u.driver.startAddressKiB == 0 && u.driver.operationFlags == 0);

    header(b, 0x600, 172, 3, 5);
    put32(b, 0x618, 0xfedcba98);
    put32(b, 0x614, 8192); put32(b, 0x610, 0xffffffffu); put32(b, 0x654, 0xffffffffu);
    CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::Ok);
    CHECK(f.protectedRegionSizePresent && f.protectedRegionSizeKiB == 8192 &&
          f.firmwareReservedKiB == 0xffffffffu && f.firmwareCapability == 0xffffffffu);
    CHECK(f.biosScratchRegisterDword == 0xfedcba98);
    put32(b, 0x610, 0); put32(b, 0x614, 0); put32(b, 0x654, 0);
    CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::Ok);
    CHECK(f.present && f.protectedRegionSizePresent && !f.protectedRegionSizeKiB &&
          !f.firmwareReservedKiB && !f.twoStageMemoryTraining);
    header(b, 0x700, 48, 2, 2); put16(b, 0x70a, 0xbeef); // reserved, not driver size
    put32(b, 0x70c, 0x40054321); put32(b, 0x710, 0xfedcba98);
    CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::Ok);
    CHECK(u.driver.startAddressPresent && u.driver.startAddressKiBRaw == 0x40054321 &&
          u.driver.startAddressKiB == 0x54321 && u.driver.operationFlags == 1 &&
          u.driver.sizeKiB == 0xfedcba98);
    for (uint32_t flags = 0; flags < 4; ++flags) {
        put32(b, 0x704, (flags << 30) | 0x3fffffff); put16(b, 0x708, 0xffff);
        put32(b, 0x70c, flags << 30); put32(b, 0x710, 0xffffffffu);
        CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::Ok);
        CHECK(u.firmware.operationFlags == flags && u.firmware.startAddressKiB == 0x3fffffff &&
              u.firmware.sizeKiB == 0xffff && u.driver.operationFlags == flags &&
              u.driver.startAddressKiB == 0 && u.driver.sizeKiB == 0xffffffffu);
    }
    // Zero declarations remain present metadata, never a free-space assertion.
    std::memset(&b[0x704], 0, 44);
    CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::Ok);
    CHECK(u.present && u.firmware.startAddressPresent && u.driver.startAddressPresent &&
          !u.firmware.sizeKiB && !u.driver.sizeKiB);

    for (unsigned which = 0; which < 4; ++which) {
        const bool firmware = which < 2;
        const size_t at = firmware ? 0x600 : 0x700;
        const uint16_t full = which == 0 ? 108 : which == 1 ? 172 : which == 2 ? 12 : 48;
        const uint8_t major = firmware ? 3 : 2;
        const uint8_t minor = static_cast<uint8_t>(firmware ? which + 4 : which - 1);
        for (uint16_t length = 4; length < full; ++length) {
            b = original; header(b, at, length, major, minor);
            if (firmware) {
                f.firmwareReservedKiB = 123;
                CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::Malformed);
                CHECK(f.present && f.table.size == length && firmwarePayloadZero(f));
            } else {
                u.driver.sizeKiB = 123;
                CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::Malformed);
                CHECK(u.present && u.table.size == length && regionZero(u.firmware) && regionZero(u.driver));
            }
        }
        // Unknown trailing bytes in an otherwise known table are not interpreted.
        b = original; header(b, at, full + 1, major, minor);
        CHECK((firmware ? parseFirmwareInfo(b.data(), b.size(), image, f) :
               parseVramUsage(b.data(), b.size(), image, u)) == Status::Ok);
    }
    for (bool firmware : {false, true}) {
        const size_t at = firmware ? 0x600 : 0x700;
        const size_t entry = 0x144 + (firmware ? kFirmwareInfoTableIndex : kVramUsageTableIndex) * 2;
        // Unsupported versions preserve only a valid common header.
        for (unsigned version = 0; version < 3; ++version) {
            b = original;
            header(b, at, 4, version == 0 ? 9 : firmware ? 3 : 2,
                   version == 2 ? 9 : firmware ? 3 : 0);
            if (firmware) {
                CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::UnsupportedVersion);
                CHECK(f.present && f.table.size == 4 && firmwarePayloadZero(f));
            } else {
                CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::UnsupportedVersion);
                CHECK(u.present && u.table.size == 4 && regionZero(u.firmware) && regionZero(u.driver));
            }
        }
        b = original; put16(b, entry, 0);
        if (firmware) {
            CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::NotFound);
            CHECK(!f.present && !f.table.offset && firmwarePayloadZero(f));
        } else {
            CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::NotFound);
            CHECK(!u.present && !u.table.offset && regionZero(u.firmware) && regionZero(u.driver));
        }
        // A table length below the common header cannot be retained as metadata.
        for (uint16_t length = 0; length < 4; ++length) {
            b = original; put16(b, at, length);
            if (firmware) {
                CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::Malformed);
                CHECK(!f.present && !f.table.offset && firmwarePayloadZero(f));
            } else {
                CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::Malformed);
                CHECK(!u.present && !u.table.offset && regionZero(u.firmware) && regionZero(u.driver));
            }
        }
        // Even a larger supplied dump and forged ImageInfo cannot extend the image.
        b = original; b.resize(8192); put16(b, entry, 4090);
        header(b, 4090, firmware ? 108 : 12, firmware ? 3 : 2, firmware ? 4 : 1);
        ImageInfo forged = image; forged.size = SIZE_MAX;
        if (firmware) {
            CHECK(parseFirmwareInfo(b.data(), b.size(), forged, f) == Status::Truncated);
            CHECK(!f.present && !f.table.offset && firmwarePayloadZero(f));
        } else {
            CHECK(parseVramUsage(b.data(), b.size(), forged, u) == Status::Truncated);
            CHECK(!u.present && !u.table.offset && regionZero(u.firmware) && regionZero(u.driver));
        }
    }
    CHECK(parseFirmwareInfo(nullptr, 4096, image, f) == Status::InvalidArgument);
    CHECK(!f.present && !f.table.offset && firmwarePayloadZero(f));
    CHECK(parseVramUsage(nullptr, 4096, image, u) == Status::InvalidArgument);
    CHECK(!u.present && !u.table.offset && regionZero(u.firmware) && regionZero(u.driver));
    // Parse through a nonzero image offset as in a full flash dump.
    b = std::vector<uint8_t>(73, 0); b.insert(b.end(), original.begin(), original.end());
    ImageInfo embedded = {};
    CHECK(parseImage(b.data(), b.size(), 73, embedded) == Status::Ok);
    CHECK(parseFirmwareInfo(b.data(), b.size(), embedded, f) == Status::Ok && f.firmwareReservedKiB == 4096);
    CHECK(parseVramUsage(b.data(), b.size(), embedded, u) == Status::Ok && u.firmware.startAddressKiB == 0x12345);
}

void actualRom(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    CHECK(file.good()); if (!file) return;
    const std::streamoff bytes = file.tellg();
    CHECK(bytes > 0 && bytes <= 16 * 1024 * 1024); if (bytes <= 0 || bytes > 16 * 1024 * 1024) return;
    file.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(bytes));
    CHECK(bool(file.read(reinterpret_cast<char*>(b.data()), bytes))); if (!file) return;
    unsigned legacyCount = 0;
    ImageInfo image = {};
    for (size_t cursor = 0; findNextImage(b.data(), b.size(), cursor, image) == Status::Ok;
         cursor = image.offset + image.size) {
        if (image.codeType != 0) continue;
        ++legacyCount;
        CHECK(image.offset == (legacyCount == 1 ? 0x40000u : 0x120000u));
        FirmwareInfo f = {}; VramUsageInfo u = {};
        CHECK(parseFirmwareInfo(b.data(), b.size(), image, f) == Status::Ok);
        CHECK(f.present && f.table.formatRevision == 3 && f.table.contentRevision == 4 &&
              f.table.size == 108 && f.firmwareReservedKiB == 4096 && !f.twoStageMemoryTraining);
        CHECK(!f.protectedRegionSizePresent);
        CHECK(parseVramUsage(b.data(), b.size(), image, u) == Status::Ok);
        CHECK(u.present && u.table.formatRevision == 2 && u.table.contentRevision == 1 &&
              u.table.size == 12 && u.firmware.startAddressKiBRaw == 0 &&
              u.firmware.sizeKiB == 0 && u.driver.sizeKiB == 0 && !u.driver.startAddressPresent);
    }
    CHECK(legacyCount == 2);
}

class GuardBuffer {
public:
    uint8_t* memory;
    size_t page;
    GuardBuffer() : memory(nullptr), page(0) {
#ifdef _WIN32
        SYSTEM_INFO system = {}; GetSystemInfo(&system); page = system.dwPageSize;
        memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (memory && !VirtualProtect(memory + page, page, PAGE_NOACCESS, &old)) {
            VirtualFree(memory, 0, MEM_RELEASE); memory = nullptr;
        }
#else
        const long queried = sysconf(_SC_PAGESIZE);
        if (queried <= 0) return;
        page = static_cast<size_t>(queried);
        void* mapped = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) return;
        memory = static_cast<uint8_t*>(mapped);
        if (mprotect(memory + page, page, PROT_NONE) != 0) {
            munmap(memory, page * 2); memory = nullptr;
        }
#endif
    }
    ~GuardBuffer() {
        if (!memory) return;
#ifdef _WIN32
        VirtualFree(memory, 0, MEM_RELEASE);
#else
        munmap(memory, page * 2);
#endif
    }
};

void exercise(const uint8_t* bytes, size_t size) {
    ImageInfo image = {};
    if (parseImage(bytes, size, 0, image) == Status::Ok) {
        CHECK(image.size <= size);
        AtomInfo a = {}; VramInfo v = {}; DisplayInfo d = {};
        parseAtom(bytes, size, image, a);
        parseVram(bytes, size, image, v);
        parseDisplay(bytes, size, image, d);
        FirmwareInfo f = {}; VramUsageInfo u = {};
        parseFirmwareInfo(bytes, size, image, f);
        parseVramUsage(bytes, size, image, u);
    }
    findNextImage(bytes, size, 0, image);
}
void boundaries() {
    GuardBuffer guard;
    CHECK(guard.memory != nullptr && guard.page >= 4096);
    if (!guard.memory || guard.page < 4096) return;
    const std::vector<uint8_t> b = fixture();
    // Put each full known reservation table immediately before the inaccessible
    // page, then declare it one byte short. Both structure and image bounds
    // remain enforced even though the descriptor originally came from elsewhere.
    for (unsigned which = 0; which < 4; ++which) {
        const bool firmware = which < 2;
        const uint16_t full = which == 0 ? 108 : which == 1 ? 172 : which == 2 ? 12 : 48;
        const uint8_t minor = static_cast<uint8_t>(firmware ? which + 4 : which - 1);
        auto endTable = b;
        const size_t entry = 0x144 + (firmware ? kFirmwareInfoTableIndex : kVramUsageTableIndex) * 2;
        const size_t at = endTable.size() - full;
        put16(endTable, entry, static_cast<uint16_t>(at));
        header(endTable, at, full, firmware ? 3 : 2, minor);
        const ImageInfo image = imageFor(endTable);
        for (unsigned shortBy = 0; shortBy < 2; ++shortBy) {
            put16(endTable, at, static_cast<uint16_t>(full - shortBy));
            uint8_t* bytes = guard.memory + guard.page - endTable.size();
            std::memcpy(bytes, endTable.data(), endTable.size());
            FirmwareInfo f = {}; VramUsageInfo u = {};
            const Status status = firmware ? parseFirmwareInfo(bytes, endTable.size(), image, f) :
                parseVramUsage(bytes, endTable.size(), image, u);
            CHECK(status == (shortBy ? Status::Malformed : Status::Ok));
            if (shortBy) CHECK(firmware ? firmwarePayloadZero(f) : regionZero(u.firmware) && regionZero(u.driver));
        }
    }
    for (size_t n = 0; n <= b.size(); ++n) {
        uint8_t* at = guard.memory + guard.page - n;
        if (n) std::memcpy(at, b.data(), n);
        exercise(at, n); // next byte is an inaccessible page, even at zero length
        if (n >= 512 && n % 512 == 0) {
            at[0x90] = static_cast<uint8_t>(n / 512); at[0x91] = 0;
            exercise(at, n); // valid image envelope but truncated nested tables
        }
    }
    uint32_t random = 0x7900744c;
    for (size_t trial = 0; trial < 5000; ++trial) {
        uint8_t* at = guard.memory + guard.page - b.size();
        std::memcpy(at, b.data(), b.size());
        for (unsigned j = 0; j < 8; ++j) {
            random = random * 1664525u + 1013904223u;
            const size_t location = (random >> 8) % b.size();
            random = random * 1664525u + 1013904223u;
            at[location] ^= static_cast<uint8_t>(random >> 24);
        }
        exercise(at, b.size());
    }
    CHECK(true); // reached end without guard-page fault
}
} // namespace

int main(int argc, char** argv) {
    basics(); scans(); malformed(); reservationMetadata(); boundaries();
    // Optional evidence test for the user's supplied Navi31 flash dump only.
    if (argc == 2) actualRom(argv[1]);
    std::cout << checks << " assertions, " << failures
              << " failures; 4097 guarded truncations and 5000 mutated fixtures exercised.\n";
    return failures ? 1 : 0;
}
