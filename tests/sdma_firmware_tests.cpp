// SPDX-License-Identifier: MIT
// Simulation only: real firmware may be read offline; no GPU is accessed.
#include "hackynavi/sdma_firmware.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "line " << __LINE__ << ": " #x "\n"; } } while (0)
void put16(std::vector<uint8_t>& b, size_t p, uint16_t v) {
    b[p] = static_cast<uint8_t>(v); b[p + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[p + i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
Navi31IpLayout ips() {
    Navi31IpLayout l = {}; l.valid = true;
    const uint16_t ids[] = {11,34,255,1,42,43};
    const uint8_t major[] = {11,3,13,13,6,6};
    for (unsigned i = 0; i < 6; ++i) {
        l.blocks[i].hardwareId = ids[i]; l.blocks[i].major = major[i]; l.blocks[i].baseCount = 2;
    }
    return l;
}
std::vector<uint8_t> fixture(uint32_t context = 12, uint32_t control = 20) {
    std::vector<uint8_t> b(64 + context + control, 0);
    put32(b, 0, static_cast<uint32_t>(b.size())); put32(b, 4, 64);
    put16(b, 8, 2); put16(b, 12, 6); put32(b, 16, 27);
    put32(b, 20, context + control); put32(b, 24, 64); put32(b, 32, 20);
    put32(b, 36, context); put32(b, 40, context); // Empty jump tables at segment end.
    put32(b, 48, 64 + context); put32(b, 52, control); put32(b, 56, control);
    for (size_t i = 64; i < b.size(); ++i) b[i] = static_cast<uint8_t>(i * 39 + 17);
    return b;
}
SdmaFirmwarePlan plan(const std::vector<uint8_t>& b) {
    SdmaFirmwarePlan p = {}; Status parser = Status::InvalidArgument;
    CHECK(planSdmaFirmware(b.data(), b.size(), ips(), p, parser) == SdmaFirmwareStatus::Ok);
    CHECK(parser == Status::Ok && p.valid); return p;
}
struct Simulation {
    VramAllocator allocator;
    VramAllocation allocation;
    HdpState hdp;
    std::vector<uint8_t> memory;
    std::map<uint32_t, uint32_t> registers;
    uint32_t hi, low;
    size_t accesses, failAt, dataWrites, dataReads, flushes;
    bool mismatch;
    Simulation(uint64_t bytes = 8192, uint64_t offset = 0x5ffff0000ULL)
        : allocation{}, memory(static_cast<size_t>(bytes), 0xcc), hi(0), low(0),
          accesses(0), failAt(SIZE_MAX), dataWrites(0), dataReads(0), flushes(0), mismatch(false) {
        MemoryLayout m = {};
        CHECK(deriveMemoryLayout({0x1000,0x15ff,0x2000,24ULL<<30,0x8000000000ULL,256ULL<<20}, m)
              == MemoryLayoutStatus::Ok);
        CHECK(allocator.configure(m, {offset,bytes}, nullptr, 0) == VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(bytes, 4096, allocation) == VramAllocatorStatus::Ok);
        const HdpLayout l = {true,0x44b4,0x44b8,0x430c,0x7f000,0x7f004,0x100000};
        registers[0x430c] = 24576; HdpResult out = {};
        CHECK(configureHdp(l, hdpIo(), hdp, out) == HdpStatus::Ok);
        resetCounts();
    }
    void resetCounts() { accesses = dataWrites = dataReads = flushes = 0; failAt = SIZE_MAX; }
    size_t selected() {
        const uint64_t address = (uint64_t(hi) << 31) | (low & 0x7ffffffc);
        CHECK((low & 0x80000000) && address >= allocation.range.offset &&
            address <= allocation.range.offset + allocation.range.bytes - 4);
        return static_cast<size_t>(address - allocation.range.offset);
    }
    static bool read(void* ctx, uint32_t reg, uint32_t& value) {
        Simulation& s = *static_cast<Simulation*>(ctx); ++s.accesses;
        if (reg == 4) {
            const size_t p = s.selected(); value = le32(&s.memory[p]); ++s.dataReads;
            if (s.mismatch) value ^= 1;
        } else value = s.registers[reg];
        return s.accesses != s.failAt;
    }
    static bool write(void* ctx, uint32_t reg, uint32_t value) {
        Simulation& s = *static_cast<Simulation*>(ctx); ++s.accesses;
        if (reg == 0x18) s.hi = value;
        else if (reg == 0) s.low = value;
        else if (reg == 4) {
            const size_t p = s.selected(); ++s.dataWrites;
            for (unsigned i = 0; i < 4; ++i) s.memory[p+i] = static_cast<uint8_t>(value >> (8*i));
        } else { s.registers[reg] = value; if (reg == 0x7f000) ++s.flushes; }
        return s.accesses != s.failAt;
    }
    HdpIo hdpIo() { return HdpIo{this,read,write}; }
    VramTransferIo transferIo() { return VramTransferIo{this,0x100000,read,write}; }
};
bool isZero(const SdmaFirmwarePlan& p) {
    return !p.valid && !p.ucodeVersion && !p.featureVersion && !p.allocationBytes &&
        !p.fileSegments[0].offset && !p.fileSegments[0].size &&
        !p.fileSegments[1].offset && !p.fileSegments[1].size &&
        !p.pspTypes[0] && !p.pspTypes[1] && !p.stagingOffsets[0] && !p.stagingOffsets[1];
}
void planTests() {
    const auto original = fixture(); const auto p = plan(original);
    CHECK(p.ucodeVersion == 27 && p.featureVersion == 20 && p.allocationBytes == 8192);
    CHECK(p.fileSegments[0].offset == 64 && p.fileSegments[0].size == 12 &&
        p.fileSegments[1].offset == 76 && p.fileSegments[1].size == 20);
    CHECK(p.pspTypes[0] == 71 && p.pspTypes[1] == 72 && !p.stagingOffsets[0] && p.stagingOffsets[1] == 4096);
    for (size_t length = 0; length < original.size(); ++length) {
        SdmaFirmwarePlan out = p; Status parser = Status::Ok;
        CHECK(planSdmaFirmware(original.data(), length, ips(), out, parser) == SdmaFirmwareStatus::FirmwareRejected);
        CHECK(parser != Status::Ok && isZero(out));
    }
    for (unsigned field = 0; field < 8; ++field) {
        auto b = original; SdmaFirmwarePlan out = p; Status parser = Status::Ok;
        if (field == 0) put16(b,8,3);
        if (field == 1) put16(b,10,1);
        if (field == 2) put16(b,12,7);
        if (field == 3) put16(b,14,1);
        if (field == 4) put32(b,36,0);
        if (field == 5) { put32(b,52,0); put32(b,56,0); }
        if (field == 6) { put32(b,48,64); put32(b,40,0); }
        if (field == 7) put32(b,48,UINT32_MAX);
        const SdmaFirmwareStatus got = planSdmaFirmware(b.data(), b.size(), ips(), out, parser);
        const SdmaFirmwareStatus wanted = field < 2 || field == 4 || field == 7 ? SdmaFirmwareStatus::FirmwareRejected :
            field < 4 ? SdmaFirmwareStatus::UnsupportedFirmware :
            field == 5 ? SdmaFirmwareStatus::EmptySegment : SdmaFirmwareStatus::OverlappingSegments;
        CHECK(got == wanted && isZero(out));
    }
    auto empty = fixture(0,20); SdmaFirmwarePlan out = {}; Status parser = Status::Ok;
    CHECK(planSdmaFirmware(empty.data(), empty.size(), ips(), out, parser) == SdmaFirmwareStatus::EmptySegment);
    auto huge = fixture(static_cast<uint32_t>(kVramTransferMaxBytes + 4),4);
    CHECK(planSdmaFirmware(huge.data(), huge.size(), ips(), out, parser) == SdmaFirmwareStatus::CapacityExceeded);
    auto maximum = fixture(static_cast<uint32_t>(kVramTransferMaxBytes), static_cast<uint32_t>(kVramTransferMaxBytes));
    CHECK(plan(maximum).allocationBytes == 2 * kVramTransferMaxBytes);
    for (unsigned field = 0; field < 8; ++field) {
        Navi31IpLayout ip = ips();
        if (field == 0) ip.valid = false;
        if (field == 1) ip.blocks[0].major = 12;
        if (field == 2) ip.blocks[2].major = 14;
        if (field == 3) ip.blocks[4].major = 5;
        if (field == 4) ip.blocks[5].minor = 1;
        if (field == 5) ip.blocks[4].instance = 1;
        if (field == 6) ip.blocks[5].hardwareId = 42;
        if (field == 7) ip.blocks[4].baseCount = 0;
        CHECK(planSdmaFirmware(original.data(), original.size(), ip, out, parser) == SdmaFirmwareStatus::UnsupportedIp);
        CHECK(isZero(out));
    }
}
SdmaFirmwareStatus stage(const std::vector<uint8_t>& b, Simulation& s,
                        StagedSdmaFirmware& staged, SdmaFirmwareResult& out) {
    return stageSdmaFirmware(b.data(), b.size(), ips(), s.allocator, s.allocation.handle,
        s.transferIo(), s.hdpIo(), s.hdp, staged, out);
}
void verifyCommands(Simulation& s, const StagedSdmaFirmware& staged, const SdmaFirmwarePlan& p) {
    uint64_t mc = 0, physical = 0;
    CHECK(s.allocator.addresses(s.allocation.handle, mc, physical) == VramAllocatorStatus::Ok);
    for (unsigned i = 0; i < 2; ++i) {
        std::vector<uint8_t> command(1026,0xa5), expected(1026,0);
        CHECK(encodeSdmaPspLoadCommand(staged,s.allocator,i,command.data()+1,1024) == SdmaFirmwareStatus::Ok);
        put32(expected,9,6); put32(expected,29,static_cast<uint32_t>(mc+p.stagingOffsets[i]));
        put32(expected,33,static_cast<uint32_t>((mc+p.stagingOffsets[i])>>32));
        put32(expected,37,p.fileSegments[i].size); put32(expected,41,i ? 72 : 71);
        expected[0] = expected[1025] = 0xa5;
        CHECK(command == expected);
        CHECK((uint64_t(le32(command.data()+29)) | (uint64_t(le32(command.data()+33)) << 32)) !=
            physical+p.stagingOffsets[i]);
    }
}
void goodStage(const std::vector<uint8_t>& b) {
    const auto p = plan(b);
    // Last64KiB of the simulated arena is convenient; no real ownership inference.
    Simulation s(p.allocationBytes, 0x400000000ULL); StagedSdmaFirmware staged; SdmaFirmwareResult out = {};
    CHECK(stage(b,s,staged,out) == SdmaFirmwareStatus::Ok);
    CHECK(staged.stagedForPsp() && out.segmentsVerified == 2 && out.visibilityAttempted);
    CHECK(out.visibilityStatus == HdpStatus::Ok && s.flushes == 1);
    CHECK(s.dataWrites == (p.fileSegments[0].size+p.fileSegments[1].size)/4 && s.dataReads == s.dataWrites);
    for (unsigned i = 0; i < 2; ++i) {
        for (size_t j = 0; j < p.fileSegments[i].size; ++j)
            CHECK(s.memory[static_cast<size_t>(p.stagingOffsets[i])+j] == b[p.fileSegments[i].offset+j]);
        const uint64_t end = i == 0 ? p.stagingOffsets[1] : p.allocationBytes;
        for (uint64_t j = p.stagingOffsets[i]+p.fileSegments[i].size; j < end; ++j)
            CHECK(s.memory[static_cast<size_t>(j)] == 0xcc); // Padding never submitted.
    }
    verifyCommands(s,staged,p);
    uint8_t command[1024]; std::memset(command,0xa5,sizeof(command));
    CHECK(encodeSdmaPspLoadCommand(staged,s.allocator,2,command,sizeof(command)) == SdmaFirmwareStatus::InvalidArgument);
    CHECK(encodeSdmaPspLoadCommand(staged,s.allocator,0,command,1023) == SdmaFirmwareStatus::InvalidArgument);
    CHECK(command[0] == 0xa5 && command[1023] == 0xa5);
    CHECK(s.allocator.release(s.allocation.handle) == VramAllocatorStatus::Ok);
    CHECK(encodeSdmaPspLoadCommand(staged,s.allocator,0,command,sizeof(command)) == SdmaFirmwareStatus::InvalidAllocation);
    CHECK(command[0] == 0xa5 && command[1023] == 0xa5);
    staged.invalidate(); CHECK(!staged.stagedForPsp());
}
void failureStages() {
    const auto b = fixture(); Simulation good; StagedSdmaFirmware gs; SdmaFirmwareResult out = {};
    CHECK(stage(b,good,gs,out) == SdmaFirmwareStatus::Ok);
    for (size_t fail = 1; fail <= good.accesses; ++fail) {
        Simulation s; StagedSdmaFirmware staged; s.failAt = fail;
        const auto status = stage(b,s,staged,out);
        CHECK(status == (fail <= good.accesses-4 ? SdmaFirmwareStatus::TransferFailed : SdmaFirmwareStatus::VisibilityFailed));
        CHECK(s.accesses == fail && !staged.stagedForPsp() && s.allocator.liveAllocations() == 1);
        CHECK(out.visibilityAttempted == (fail > good.accesses-4));
        uint8_t cmd[1024]; std::memset(cmd,0xa5,sizeof(cmd));
        CHECK(encodeSdmaPspLoadCommand(staged,s.allocator,0,cmd,sizeof(cmd)) == SdmaFirmwareStatus::InvalidState);
        CHECK(cmd[0] == 0xa5 && cmd[1023] == 0xa5);
    }
    Simulation bad; StagedSdmaFirmware staged; bad.mismatch = true;
    CHECK(stage(b,bad,staged,out) == SdmaFirmwareStatus::TransferFailed);
    CHECK(out.transfer.status == VramTransferStatus::VerifyMismatch && !out.visibilityAttempted && bad.flushes == 0);
    // Retry starts by invalidating the old staged state, including preflight failures.
    good.resetCounts(); auto truncated = b; truncated.pop_back();
    CHECK(stage(truncated,good,gs,out) == SdmaFirmwareStatus::FirmwareRejected);
    CHECK(!gs.stagedForPsp() && !good.accesses);
}
void preflightTests() {
    const auto b = fixture(); StagedSdmaFirmware staged; SdmaFirmwareResult out = {};
    Simulation small(4096);
    CHECK(stage(b,small,staged,out) == SdmaFirmwareStatus::OutsideAllocation && !small.accesses);
    Simulation released;
    CHECK(released.allocator.release(released.allocation.handle) == VramAllocatorStatus::Ok);
    CHECK(stage(b,released,staged,out) == SdmaFirmwareStatus::InvalidAllocation && !released.accesses);
    Simulation noHdp; noHdp.hdp.clearAfterDeviceReset();
    CHECK(stage(b,noHdp,staged,out) == SdmaFirmwareStatus::InvalidState && !noHdp.accesses);
    for (unsigned which = 0; which < 5; ++which) {
        Simulation s; auto transfer = s.transferIo(); auto hdp = s.hdpIo();
        if (which == 0) transfer.read32 = nullptr;
        if (which == 1) transfer.write32 = nullptr;
        if (which == 2) transfer.barBytes = 0x1b;
        if (which == 3) hdp.read32 = nullptr;
        if (which == 4) hdp.write32 = nullptr;
        CHECK(stageSdmaFirmware(b.data(),b.size(),ips(),s.allocator,s.allocation.handle,transfer,hdp,
            s.hdp,staged,out) == SdmaFirmwareStatus::InvalidArgument);
        CHECK(!s.accesses && !staged.stagedForPsp());
    }
    for (uint32_t remap : {uint32_t(0x7f100), uint32_t(UINT32_MAX)}) {
        Simulation s; s.registers[0x44b4] = remap;
        CHECK(stage(b,s,staged,out) == SdmaFirmwareStatus::VisibilityFailed);
        CHECK(out.segmentsVerified == 2 && !staged.stagedForPsp() && s.hdp.faulted());
    }
}
void actualFirmware(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate); CHECK(bool(file)); if (!file) return;
    const auto bytes = file.tellg(); CHECK(bytes > 0 && bytes < 4*1024*1024); if (bytes <= 0 || bytes >= 4*1024*1024) return;
    std::vector<uint8_t> b(static_cast<size_t>(bytes)); file.seekg(0);
    file.read(reinterpret_cast<char*>(b.data()),bytes); CHECK(bool(file));
    const auto p = plan(b); CHECK(p.ucodeVersion == 27); goodStage(b);
}
} // namespace
int main(int argc,char** argv) {
    planTests(); goodStage(fixture()); goodStage(fixture(4100,8188)); failureStages(); preflightTests();
    if (argc > 1) actualFirmware(argv[1]);
    std::cout << checks << " assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
