// SPDX-License-Identifier: MIT
// Synthetic container fixtures, guarded bounds tests, optional real-corpus QA.
#include "hackynavi/firmware.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
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
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

void put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}
void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    put16(bytes, offset, static_cast<uint16_t>(value));
    put16(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
}
std::vector<uint8_t> common(size_t size, uint32_t header, uint32_t payload) {
    std::vector<uint8_t> bytes(size, 0);
    put32(bytes, 0, static_cast<uint32_t>(size));
    put32(bytes, 4, header);
    put16(bytes, 8, 2); put16(bytes, 10, 0);
    put16(bytes, 12, 11); put16(bytes, 14, 0);
    put32(bytes, 16, 0x12345678);
    put32(bytes, 20, static_cast<uint32_t>(size) - payload);
    put32(bytes, 24, payload); put32(bytes, 28, 0xdeadbeef);
    return bytes;
}
std::vector<uint8_t> gfxFixture() {
    std::vector<uint8_t> bytes = common(128, 60, 64);
    put32(bytes, 32, 29);
    put32(bytes, 36, 32); put32(bytes, 40, 64);
    put32(bytes, 44, 32); put32(bytes, 48, 96);
    put32(bytes, 52, 0x3000); put32(bytes, 56, 0x70000);
    return bytes;
}
std::vector<uint8_t> sdmaFixture() {
    std::vector<uint8_t> bytes = common(128, 64, 64);
    put16(bytes, 12, 6); put32(bytes, 32, 60);
    put32(bytes, 36, 32); put32(bytes, 40, 8); put32(bytes, 44, 4);
    put32(bytes, 48, 96); put32(bytes, 52, 32);
    put32(bytes, 56, 16); put32(bytes, 60, 0);
    return bytes;
}
std::vector<uint8_t> pspFixture() {
    // Real PSP 2.0 files report fixed header size 36, excluding descriptors.
    std::vector<uint8_t> bytes = common(320, 36, 256);
    put16(bytes, 12, 13); put32(bytes, 32, 3);
    for (unsigned i = 0; i < 3; ++i) {
        const size_t record = 36 + i * 16;
        put32(bytes, record, i == 2 ? 0xffffffffU : i + 1);
        put32(bytes, record + 4, 0x310036);
        put32(bytes, record + 8, i * 24);
        put32(bytes, record + 12, i == 2 ? 16 : 24);
    }
    return bytes;
}
bool within(const FirmwareRange& range, const FirmwareHeader& header) {
    return range.offset >= header.headerSizeBytes &&
           uint64_t(range.offset) + range.size <= header.sizeBytes;
}

void commonTests() {
    FirmwareHeader header = {};
    std::vector<uint8_t> bytes = gfxFixture();
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Ok);
    CHECK(header.sizeBytes == 128 && header.headerSizeBytes == 60);
    CHECK(header.headerMajor == 2 && header.ipMajor == 11);
    CHECK(header.ucodeVersion == 0x12345678 && header.storedCrc32 == 0xdeadbeef);
    bytes[64] ^= 0xff;
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Ok);
    CHECK(header.storedCrc32 == 0xdeadbeef); // No invented CRC verification.
    CHECK(parseFirmwareHeader(nullptr, 128, header) == Status::InvalidArgument);
    CHECK(header.sizeBytes == 0);
    CHECK(parseFirmwareHeader(bytes.data(), 31, header) == Status::Truncated);
    CHECK(parseFirmwareHeader(bytes.data(), 127, header) == Status::Truncated);
    bytes.push_back(0);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 4, 31);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 4, 129);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 24, 59);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 20, 0xffffffffU);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 24, 0xfffffff0U);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Malformed);
    bytes = gfxFixture(); put16(bytes, 8, 99);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Ok);
    CHECK(header.headerMajor == 99); // Common prefix does not identify family.
    bytes = common(32, 32, 32);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Ok);
    CHECK(header.payloadSize == 0);
    bytes = gfxFixture(); put32(bytes, 20, 32);
    CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Ok);
    CHECK(header.payloadOffset + header.payloadSize < header.sizeBytes);
}

void gfxTests() {
    GfxFirmwareV2 firmware = {};
    std::vector<uint8_t> bytes = gfxFixture();
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Ok);
    CHECK(firmware.instruction.offset == 64 && firmware.instruction.size == 32);
    CHECK(firmware.data.offset == 96 && firmware.data.size == 32);
    CHECK(firmware.startAddress == 0x0007000000003000ULL);
    CHECK(firmware.featureVersion == 29);
    // Independent ranges are not implicitly limited to common payload size.
    put32(bytes, 20, 32);
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Ok);
    bytes = gfxFixture(); put16(bytes, 10, 1);
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::UnsupportedVersion);
    CHECK(firmware.header.headerMinor == 1 && firmware.instruction.size == 0);
    bytes = gfxFixture(); put32(bytes, 4, 59);
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    CHECK(firmware.header.sizeBytes == 0);
    bytes = gfxFixture(); put32(bytes, 40, 56);
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 48, 100);
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 36, 0xfffffffcU);
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = gfxFixture(); put32(bytes, 36, 31);
    CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    CHECK(parseGfxFirmwareV2(nullptr, 0, firmware) == Status::InvalidArgument);
}

void sdmaTests() {
    SdmaFirmwareV2 firmware = {};
    std::vector<uint8_t> bytes = sdmaFixture();
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Ok);
    CHECK(firmware.context.offset == 64 && firmware.control.offset == 96);
    CHECK(firmware.contextJumpTable.offset == 72 && firmware.contextJumpTable.size == 4);
    CHECK(firmware.controlJumpTable.offset == 112 && firmware.controlJumpTable.size == 0);
    bytes = sdmaFixture(); put32(bytes, 40, 28); put32(bytes, 44, 4);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Ok);
    put32(bytes, 44, 8);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    CHECK(firmware.context.size == 0);
    bytes = sdmaFixture(); put32(bytes, 56, 0xffffffffU);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = sdmaFixture(); put32(bytes, 52, 0xffffffffU);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = sdmaFixture(); put32(bytes, 48, 0xffffffffU);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = sdmaFixture(); put32(bytes, 40, 1);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = sdmaFixture(); put32(bytes, 4, 60);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = sdmaFixture(); put16(bytes, 8, 3);
    CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::UnsupportedVersion);
    CHECK(firmware.header.headerMajor == 3 && firmware.control.size == 0);
}

void pspTests() {
    PspFirmwareV2 firmware = {};
    std::vector<uint8_t> bytes = pspFixture();
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Ok);
    CHECK(firmware.header.headerSizeBytes == 36 && firmware.descriptorCount == 3);
    CHECK(firmware.descriptors[1].payloadRelativeOffset == 24);
    CHECK(firmware.descriptors[1].image.offset == 280 && firmware.descriptors[1].image.size == 24);
    CHECK(firmware.descriptors[2].type == 0xffffffffU);
    bytes = pspFixture(); put32(bytes, 32, 0xffffffffU);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::CapacityExceeded);
    CHECK(firmware.descriptorCount == 0);
    bytes = pspFixture(); put32(bytes, 32, 14); // 36+14*16 crosses payload at256.
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = pspFixture(); put32(bytes, 44, 0xffffffffU);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = pspFixture(); put32(bytes, 48, 0xffffffffU);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = pspFixture(); put32(bytes, 44, 64); put32(bytes, 48, 0);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Ok);
    put32(bytes, 48, 1);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = pspFixture(); put32(bytes, 4, 35);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Malformed);
    bytes = pspFixture(); put16(bytes, 10, 1);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::UnsupportedVersion);
    CHECK(firmware.header.headerMinor == 1 && firmware.descriptorCount == 0);
    bytes = common(612, 36, 548); put32(bytes, 32, 32);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::Ok);
    CHECK(firmware.descriptorCount == kMaxPspFirmwareDescriptors);
    put32(bytes, 32, 33);
    CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), firmware) == Status::CapacityExceeded);
}

void guardedPrefixes() {
#ifdef _WIN32
    SYSTEM_INFO system = {}; GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(memory != nullptr);
    if (!memory) return;
    DWORD previous = 0;
    const bool protectedPage = VirtualProtect(memory + page, page, PAGE_NOACCESS, &previous) != 0;
#else
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<uint8_t*>(mmap(nullptr, page * 2,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(memory != MAP_FAILED);
    if (memory == MAP_FAILED) return;
    const bool protectedPage = mprotect(memory + page, page, PROT_NONE) == 0;
#endif
    CHECK(protectedPage);
    if (protectedPage) {
        const std::vector<uint8_t> fixtures[] = {gfxFixture(), sdmaFixture(), pspFixture()};
        for (const auto& fixture : fixtures) {
            for (size_t count = 0; count < fixture.size(); ++count) {
                uint8_t* input = memory + page - count;
                if (count) std::memcpy(input, fixture.data(), count);
                FirmwareHeader header = {}; GfxFirmwareV2 gfx = {};
                SdmaFirmwareV2 sdma = {}; PspFirmwareV2 psp = {};
                CHECK(parseFirmwareHeader(input, count, header) == Status::Truncated);
                CHECK(parseGfxFirmwareV2(input, count, gfx) == Status::Truncated);
                CHECK(parseSdmaFirmwareV2(input, count, sdma) == Status::Truncated);
                CHECK(parsePspFirmwareV2(input, count, psp) == Status::Truncated);
            }
        }
    }
#ifdef _WIN32
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, page * 2);
#endif
}

void mutations() {
    uint32_t random = 0xbadc0ffeU;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        std::vector<uint8_t> bytes = iteration % 3 == 0 ? gfxFixture() :
                                     iteration % 3 == 1 ? sdmaFixture() : pspFixture();
        random = random * 1664525U + 1013904223U;
        const size_t offset = (random >> 16) % 80;
        random = random * 1664525U + 1013904223U;
        bytes[offset] ^= static_cast<uint8_t>(random >> 24);
        const std::vector<uint8_t> saved = bytes;
        FirmwareHeader header = {}; GfxFirmwareV2 gfx = {};
        SdmaFirmwareV2 sdma = {}; PspFirmwareV2 psp = {};
        if (parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Ok) {
            CHECK(header.sizeBytes == bytes.size());
            const FirmwareRange payload = {header.payloadOffset, header.payloadSize};
            CHECK(within(payload, header));
        }
        if (parseGfxFirmwareV2(bytes.data(), bytes.size(), gfx) == Status::Ok)
            CHECK(within(gfx.instruction, gfx.header) && within(gfx.data, gfx.header));
        if (parseSdmaFirmwareV2(bytes.data(), bytes.size(), sdma) == Status::Ok) {
            CHECK(within(sdma.context, sdma.header) && within(sdma.control, sdma.header));
            CHECK(within(sdma.contextJumpTable, sdma.header) && within(sdma.controlJumpTable, sdma.header));
        }
        if (parsePspFirmwareV2(bytes.data(), bytes.size(), psp) == Status::Ok) {
            CHECK(psp.descriptorCount <= kMaxPspFirmwareDescriptors);
            for (uint32_t i = 0; i < psp.descriptorCount; ++i)
                CHECK(within(psp.descriptors[i].image, psp.header));
        }
        CHECK(bytes == saved);
    }
}

void corpusTests(const char* directory) {
    const char* names[] = {"gc_11_0_0_pfp.bin", "gc_11_0_0_me.bin", "gc_11_0_0_mec.bin",
        "sdma_6_0_0.bin", "psp_13_0_0_sos.bin", "psp_13_0_0_ta.bin",
        "gc_11_0_0_imu.bin", "smu_13_0_0.bin"};
    for (unsigned i = 0; i < 8; ++i) {
        const std::string path = std::string(directory) + "/" + names[i];
        std::ifstream stream(path.c_str(), std::ios::binary);
        CHECK(stream.good());
        if (!stream) continue;
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                        std::istreambuf_iterator<char>());
        FirmwareHeader header = {};
        CHECK(parseFirmwareHeader(bytes.data(), bytes.size(), header) == Status::Ok);
        if (i < 3) {
            GfxFirmwareV2 gfx = {};
            CHECK(parseGfxFirmwareV2(bytes.data(), bytes.size(), gfx) == Status::Ok);
            CHECK(gfx.header.ipMajor == 11 && gfx.instruction.offset == 256);
        } else if (i == 3) {
            SdmaFirmwareV2 sdma = {};
            CHECK(parseSdmaFirmwareV2(bytes.data(), bytes.size(), sdma) == Status::Ok);
            CHECK(sdma.contextJumpTable.offset == 0x1100);
            CHECK(sdma.controlJumpTable.offset == 0x5500 && sdma.controlJumpTable.size == 0);
        } else if (i == 4 || i == 5) {
            PspFirmwareV2 psp = {};
            CHECK(parsePspFirmwareV2(bytes.data(), bytes.size(), psp) == Status::Ok);
            CHECK(psp.header.headerSizeBytes == 36 && psp.header.payloadOffset == 256);
            CHECK(psp.descriptorCount == (i == 4 ? 6U : 4U));
        }
        std::printf("Corpus inspected: %s\n", names[i]);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) { std::fprintf(stderr, "Usage: firmware-tests [amdgpu-corpus-directory]\n"); return 2; }
    commonTests(); gfxTests(); sdmaTests(); pspTests(); guardedPrefixes(); mutations();
    if (argc == 2) corpusTests(argv[1]);
    std::printf("Firmware parser: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
