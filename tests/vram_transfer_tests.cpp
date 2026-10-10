// SPDX-License-Identifier: MIT
// Unit-test MMIO simulation only. Production transfer code uses caller MMIO.
#include "hackynavi/vram_transfer.hpp"
#include <cstring>
#include <iostream>
#include <vector>

using namespace hackynavi;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    std::cerr << "FAIL line " << __LINE__ << ": " #expression "\n"; } } while (0)

struct Owned {
    VramAllocator allocator;
    VramAllocation allocation;
    Owned(uint64_t offset = 0x7ffff000ULL, uint64_t bytes = 8192) : allocator(), allocation{} {
        MemoryLayout memory = {};
        const MemoryLayoutInput input = {0x1000, 0x15ff, 0x2000,
            24ULL << 30, 0x8000000000ULL, 256ULL << 20};
        CHECK(deriveMemoryLayout(input, memory) == MemoryLayoutStatus::Ok);
        CHECK(allocator.configure(memory, {offset, bytes}, nullptr, 0) == VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(bytes, 4096, allocation) == VramAllocatorStatus::Ok);
    }
};
struct Event { bool write; uint32_t reg, value; };
struct MmioSimulation {
    uint64_t base;
    std::vector<uint32_t> words;
    std::vector<Event> events;
    uint32_t hi = 0, low = 0;
    unsigned phase = 0, accesses = 0, failAt = 0;
    bool record = true, aliasAddresses = false;
    uint64_t corruptOffset = UINT64_MAX;
    explicit MmioSimulation(VramRange range)
        : base(range.offset), words(static_cast<size_t>(range.bytes / 4), 0x11223344) {}
    VramTransferIo io(uint64_t barBytes = 0x100000) {
        return {this, barBytes, read, write};
    }
    bool selectedIndex(size_t& index) {
        const uint64_t selected = (static_cast<uint64_t>(hi) << 31) | (low & 0x7ffffffcU);
        const bool valid = selected >= base && (selected - base) / 4 < words.size();
        CHECK(valid && (low & 0x80000000U));
        if (!valid) return false;
        index = aliasAddresses ? 0 : static_cast<size_t>((selected - base) / 4);
        return true;
    }
    static bool write(void* p, uint32_t reg, uint32_t value) {
        MmioSimulation& s = *static_cast<MmioSimulation*>(p);
        ++s.accesses;
        if (s.record) s.events.push_back({true, reg, value});
        if (reg == 0x18) {
            CHECK(s.phase == 0); s.hi = value; s.phase = 1;
        } else if (reg == 0) {
            CHECK(s.phase == 1); s.low = value; s.phase = 2;
        } else if (reg == 4) {
            CHECK(s.phase == 2); size_t index = 0;
            if (!s.selectedIndex(index)) return false;
            s.words[index] = value; s.phase = 0;
        } else { CHECK(false); return false; }
        // A failed callback deliberately may have performed the write already.
        return s.accesses != s.failAt;
    }
    static bool read(void* p, uint32_t reg, uint32_t& value) {
        MmioSimulation& s = *static_cast<MmioSimulation*>(p);
        ++s.accesses;
        CHECK(reg == 4 && s.phase == 2);
        size_t index = 0;
        if (!s.selectedIndex(index)) return false;
        value = s.words[index];
        if (s.base + index * 4 == s.corruptOffset) value ^= 0x100;
        s.phase = 0;
        if (s.record) s.events.push_back({false, reg, value});
        return s.accesses != s.failAt;
    }
};

void goldenUploadAndRead() {
    Owned o;
    MmioSimulation s(o.allocation.range);
    const uint8_t input[] = {0xa5, 0x01,0x23,0x45,0x67, 0x89,0xab,0xcd,0xef,
        0xff,0xff,0xff,0xff, 0xa5};
    uint8_t saved[sizeof(input)]; std::memcpy(saved, input, sizeof(input));
    VramTransferResult r = {};
    CHECK(uploadVram(o.allocator, o.allocation.handle, 4092, input + 1, 12, s.io(), r)
          == VramTransferStatus::Ok);
    CHECK(std::memcmp(input, saved, sizeof(input)) == 0);
    CHECK(r.stage == VramTransferStage::Complete && r.vramOffset == 0x7ffffffcULL);
    CHECK(r.currentOffset == 0x80000004ULL);
    CHECK(r.bytesWritten == 12 && r.bytesRead == 12 && r.bytesVerified == 12);
    CHECK(r.selectorWriteAttempts == 12 && r.dataWriteAttempts == 3 && r.dataReadAttempts == 3);
    CHECK(r.selectorWritesMayHaveOccurred && r.dataWritesMayHaveOccurred);
    // Independent golden31-bit split and little-endian payload values. A high
    // MC/physical base exists in Owned, but selectors use only VRAM offsets.
    const Event expected[] = {
        {true,0x18,0}, {true,0,0xfffffffc}, {true,4,0x67452301},
        {true,0x18,1}, {true,0,0x80000000}, {true,4,0xefcdab89},
        {true,0x18,1}, {true,0,0x80000004}, {true,4,0xffffffff},
        {true,0x18,0}, {true,0,0xfffffffc}, {false,4,0x67452301},
        {true,0x18,1}, {true,0,0x80000000}, {false,4,0xefcdab89},
        {true,0x18,1}, {true,0,0x80000004}, {false,4,0xffffffff}
    };
    CHECK(s.events.size() == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < s.events.size(); ++i) {
        CHECK(s.events[i].write == expected[i].write);
        CHECK(s.events[i].reg == expected[i].reg);
        CHECK(s.events[i].value == expected[i].value);
    }
    CHECK(s.hi == 1 && s.low == 0x80000004); // No selector restoration.
    uint8_t output[14]; std::memset(output, 0xa5, sizeof(output));
    s.events.clear(); s.accesses = 0;
    CHECK(readVram(o.allocator, o.allocation.handle, 4092, output + 1, 12, s.io(), r)
          == VramTransferStatus::Ok);
    CHECK(std::memcmp(input, output, sizeof(output)) == 0);
    CHECK(r.bytesRead == 12 && r.bytesWritten == 0 && r.bytesVerified == 0);
    CHECK(r.selectorWriteAttempts == 6 && r.dataWriteAttempts == 0 && r.dataReadAttempts == 3);
    CHECK(r.selectorWritesMayHaveOccurred && !r.dataWritesMayHaveOccurred);
    CHECK(s.events.size() == 9);
}

void highAddressesAndLastWord() {
    const uint64_t arenaOffsets[] = {0xfffff000ULL, 0x5fffff000ULL};
    for (unsigned which = 0; which < 2; ++which) {
        Owned o(arenaOffsets[which], 4096);
        MmioSimulation s(o.allocation.range);
        const uint8_t data[] = {0xef, 0xbe, 0xad, 0xde};
        VramTransferResult r = {};
        CHECK(uploadVram(o.allocator, o.allocation.handle, 4092, data, 4, s.io(), r)
              == VramTransferStatus::Ok);
        CHECK(s.events.size() == 6);
        CHECK(s.events[0].reg == 0x18 && s.events[0].value == (which ? 11 : 1));
        CHECK(s.events[1].reg == 0 && s.events[1].value == 0xfffffffc);
        CHECK(s.events[2].reg == 4 && s.events[2].value == 0xdeadbeef);
        CHECK(s.words.back() == 0xdeadbeef);
        CHECK(s.words[s.words.size() - 2] == 0x11223344);
    }
}

void ownershipAndPreflight() {
    Owned o;
    const uint8_t source[8] = {};
    uint8_t destination[8]; std::memset(destination, 0xa5, sizeof(destination));
    for (unsigned defect = 0; defect < 16; ++defect) {
        MmioSimulation s(o.allocation.range);
        VramTransferIo io = s.io();
        VramAllocationHandle handle = o.allocation.handle;
        uint64_t offset = 0;
        size_t bytes = 8;
        const uint8_t* pointer = source;
        VramTransferStatus expected = VramTransferStatus::InvalidArgument;
        switch (defect) {
        case 0: pointer = nullptr; break;
        case 1: bytes = 0; break;
        case 2: bytes = kVramTransferMaxBytes + 4; break;
        case 3: bytes = SIZE_MAX; break;
        case 4: pointer = reinterpret_cast<const uint8_t*>(UINTPTR_MAX - 3); break;
        case 5: io.read32 = nullptr; break;
        case 6: io.write32 = nullptr; break;
        case 7: io.barBytes = 27; expected = VramTransferStatus::OutsideRegisterBar; break;
        case 8: offset = 1; expected = VramTransferStatus::InvalidAlignment; break;
        case 9: bytes = 7; expected = VramTransferStatus::InvalidAlignment; break;
        case 10: offset = 8192; expected = VramTransferStatus::OutsideAllocation; break;
        case 11: offset = 8188; expected = VramTransferStatus::OutsideAllocation; break;
        case 12: offset = UINT64_MAX - 3; expected = VramTransferStatus::OutsideAllocation; break;
        case 13: handle.serial++; expected = VramTransferStatus::InvalidAllocation; break;
        case 14: handle.slot = 64; expected = VramTransferStatus::InvalidAllocation; break;
        case 15: handle.owner = nullptr; expected = VramTransferStatus::InvalidAllocation; break;
        }
        VramTransferResult r = {};
        CHECK(uploadVram(o.allocator, handle, offset, pointer, bytes, io, r) == expected);
        CHECK(s.events.empty() && r.stage == VramTransferStage::Preflight);
        CHECK(r.bytesWritten == 0 && r.bytesRead == 0 && r.bytesVerified == 0);
        CHECK(!r.selectorWritesMayHaveOccurred && !r.dataWritesMayHaveOccurred);
        // Read path shares the same preflight contract; use the intentionally
        // wrapping/null pointer only in cases rejected before any access.
        uint8_t* dest = defect == 0 || defect == 4 ? const_cast<uint8_t*>(pointer) : destination;
        CHECK(readVram(o.allocator, handle, offset, dest, bytes, io, r) == expected);
        CHECK(s.events.empty());
        for (uint8_t value : destination) CHECK(value == 0xa5);
    }
    VramTransferResult r = {};
    MmioSimulation s(o.allocation.range);
    VramAllocator unconfigured;
    CHECK(readVram(unconfigured, o.allocation.handle, 0, destination, 4, s.io(), r)
          == VramTransferStatus::InvalidAllocation);
    CHECK(r.allocationStatus == VramAllocatorStatus::NotConfigured && s.events.empty());
    Owned another;
    CHECK(uploadVram(another.allocator, o.allocation.handle, 0, source, 4, s.io(), r)
          == VramTransferStatus::InvalidAllocation);
    CHECK(r.allocationStatus == VramAllocatorStatus::InvalidHandle && s.events.empty());
    const VramAllocationHandle stale = o.allocation.handle;
    CHECK(o.allocator.release(stale) == VramAllocatorStatus::Ok);
    CHECK(o.allocator.allocate(8192, 4096, o.allocation) == VramAllocatorStatus::Ok);
    CHECK(o.allocation.handle.slot == stale.slot && o.allocation.handle.serial != stale.serial);
    CHECK(uploadVram(o.allocator, stale, 0, source, 4, s.io(), r)
          == VramTransferStatus::InvalidAllocation);
    CHECK(s.events.empty());
    // Minimum BAR extent and an existing live handle are accepted. Caller-side
    // VramAllocation.range metadata cannot redirect the live slot's true range.
    o.allocation.range = {UINT64_MAX, UINT64_MAX};
    CHECK(uploadVram(o.allocator, o.allocation.handle, 0, source, 4, s.io(28), r)
          == VramTransferStatus::Ok);
    CHECK(r.vramOffset == 0x7ffff000ULL);
}

void transportFailures() {
    Owned o;
    const uint8_t data[16] = {0,1,2,3, 4,5,6,7, 8,9,10,11, 12,13,14,15};
    // Four upload DWORDs and four independent verification DWORDs: every one
    // of24MMIO accesses fails in turn, including writes that nevertheless land.
    for (unsigned at = 1; at <= 24; ++at) {
        MmioSimulation s(o.allocation.range); s.failAt = at;
        VramTransferResult r = {};
        CHECK(uploadVram(o.allocator, o.allocation.handle, 0, data, sizeof(data), s.io(), r)
              == VramTransferStatus::TransportError);
        CHECK(s.accesses == at && s.events.size() == at);
        CHECK(r.selectorWritesMayHaveOccurred);
        CHECK(r.dataWritesMayHaveOccurred == (at >= 3));
        if (at <= 12) {
            CHECK(r.stage == VramTransferStage::Writing);
            CHECK(r.bytesWritten == 4 * ((at - 1) / 3));
            CHECK(r.bytesVerified == 0 && r.bytesRead == 0);
            CHECK(r.dataWriteAttempts == at / 3);
            CHECK(r.selectorWriteAttempts + r.dataWriteAttempts == at);
        } else {
            CHECK(r.stage == VramTransferStage::Verifying);
            CHECK(r.bytesWritten == 16);
            CHECK(r.bytesRead == 4 * ((at - 13) / 3));
            CHECK(r.bytesVerified == r.bytesRead);
            CHECK(r.selectorWriteAttempts + r.dataWriteAttempts + r.dataReadAttempts == at);
        }
        const unsigned word = at <= 12 ? (at - 1) / 3 : (at - 13) / 3;
        CHECK(r.currentOffset == s.base + word * 4);
        const uint32_t regCycle[] = {0x18, 0, 4};
        CHECK(r.lastRegister == regCycle[(at - 1) % 3]);
    }
    for (unsigned at = 1; at <= 12; ++at) {
        MmioSimulation s(o.allocation.range); s.failAt = at;
        uint8_t output[18]; std::memset(output, 0xa5, sizeof(output));
        VramTransferResult r = {};
        CHECK(readVram(o.allocator, o.allocation.handle, 0, output + 1, 16, s.io(), r)
              == VramTransferStatus::TransportError);
        const size_t prefix = 4 * ((at - 1) / 3);
        CHECK(r.bytesRead == prefix && r.bytesWritten == 0 && r.bytesVerified == 0);
        CHECK(!r.dataWritesMayHaveOccurred && r.stage == VramTransferStage::Reading);
        CHECK(output[0] == 0xa5 && output[17] == 0xa5);
        const uint8_t value[] = {0x44,0x33,0x22,0x11};
        for (size_t i = 0; i < 16; ++i)
            CHECK(output[i + 1] == (i < prefix ? value[i % 4] : 0xa5));
    }
}

void verificationFailures() {
    Owned o;
    const uint8_t source[] = {1,2,3,4, 5,6,7,8, 9,10,11,12};
    for (size_t badWord = 0; badWord < 3; ++badWord) {
        MmioSimulation s(o.allocation.range); s.corruptOffset = s.base + badWord * 4;
        VramTransferResult r = {};
        CHECK(uploadVram(o.allocator, o.allocation.handle, 0, source, sizeof(source), s.io(), r)
              == VramTransferStatus::VerifyMismatch);
        CHECK(r.stage == VramTransferStage::Verifying && r.bytesWritten == sizeof(source));
        CHECK(r.bytesVerified == badWord * 4 && r.bytesRead == (badWord + 1) * 4);
        CHECK(r.expectedValue != r.observedValue && (r.expectedValue ^ r.observedValue) == 0x100);
        CHECK(r.currentOffset == s.corruptOffset && r.lastRegister == 4);
    }
    // Emulate an addressing fault that aliases every selector to one location.
    // Immediate per-word verification would pass; the required full second pass
    // detects that the last write destroyed the first word.
    MmioSimulation alias(o.allocation.range); alias.aliasAddresses = true;
    VramTransferResult r = {};
    CHECK(uploadVram(o.allocator, o.allocation.handle, 0, source, sizeof(source), alias.io(), r)
          == VramTransferStatus::VerifyMismatch);
    CHECK(r.bytesWritten == sizeof(source) && r.bytesVerified == 0 && r.bytesRead == 4);
    CHECK(r.expectedValue == 0x04030201 && r.observedValue == 0x0c0b0a09);
}

void maximumTransfer() {
    Owned o(0x400000000ULL, kVramTransferMaxBytes);
    MmioSimulation s(o.allocation.range); s.record = false;
    std::vector<uint8_t> data(kVramTransferMaxBytes);
    for (size_t index = 0; index < data.size(); ++index)
        data[index] = static_cast<uint8_t>((index * 13) ^ (index >> 9));
    VramTransferResult r = {};
    CHECK(uploadVram(o.allocator, o.allocation.handle, 0, data.data(), data.size(), s.io(), r)
          == VramTransferStatus::Ok);
    CHECK(r.bytesWritten == data.size() && r.bytesVerified == data.size());
    CHECK(r.dataWriteAttempts == data.size() / 4 && r.dataReadAttempts == data.size() / 4);
    CHECK(r.selectorWriteAttempts == data.size());
    std::vector<uint8_t> readback(data.size());
    CHECK(readVram(o.allocator, o.allocation.handle, 0, readback.data(), readback.size(), s.io(), r)
          == VramTransferStatus::Ok);
    CHECK(readback == data);
}
} // namespace
int main() {
    goldenUploadAndRead(); highAddressesAndLastWord(); ownershipAndPreflight();
    transportFailures(); verificationFailures(); maximumTransfer();
    std::cout << checks << " VRAM transfer assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
