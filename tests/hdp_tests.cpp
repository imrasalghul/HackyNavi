// SPDX-License-Identifier: MIT
// CPU-side protocol tests only: no hardware is touched.
#include "hackynavi/hdp.hpp"
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
    put16(b, p, static_cast<uint16_t>(v)); put16(b, p + 2, static_cast<uint16_t>(v >> 16));
}
uint16_t sum(const std::vector<uint8_t>& b, size_t first) {
    uint16_t value = 0;
    for (size_t i = first; i < b.size(); ++i) value = static_cast<uint16_t>(value + b[i]);
    return value;
}
void fix(std::vector<uint8_t>& b) {
    put16(b, 14, sum(b, 60)); put16(b, 8, sum(b, 10));
}
std::vector<uint8_t> fixture(uint32_t base = 0x1000, bool wide = false, unsigned count = 1) {
    const unsigned recordSize = 8 + 6 * (wide ? 8 : 4);
    std::vector<uint8_t> b(144 + count * recordSize, 0);
    put32(b, 0, 0x28211407); put16(b, 4, 1); put16(b, 6, 4);
    put16(b, 10, static_cast<uint16_t>(b.size()));
    put16(b, 12, 60); put16(b, 16, static_cast<uint16_t>(b.size() - 60));
    put32(b, 60, 0x53445049); put16(b, 64, wide ? 4 : 3);
    put16(b, 66, static_cast<uint16_t>(b.size() - 60));
    put16(b, 72, 1); put16(b, 76, 140); b[138] = wide ? 1 : 0;
    put16(b, 142, static_cast<uint16_t>(count));
    for (unsigned i = 0; i < count; ++i) {
        const size_t p = 144 + i * recordSize;
        put16(b, p, 108); b[p + 3] = 6; b[p + 4] = 4; b[p + 5] = 3;
        put32(b, p + 8 + 2 * (wide ? 8 : 4), base);
    }
    fix(b); return b;
}
HdpLayout validLayout() {
    const auto b = fixture(); HdpLayout layout{};
    CHECK(buildHdpLayout(b.data(), b.size(), 0x100000, layout) == HdpStatus::Ok);
    return layout;
}
bool zero(const HdpLayout& l) {
    return !l.valid && !l.memoryRemapRegister && !l.registerRemapRegister &&
        !l.memorySizeRegister && !l.memoryFlushRegister && !l.registerFlushRegister && !l.barBytes;
}
void reject(const std::vector<uint8_t>& b, uint64_t bar, HdpStatus expected) {
    HdpLayout l = validLayout();
    CHECK(buildHdpLayout(b.data(), b.size(), bar, l) == expected);
    CHECK(zero(l));
}
void layoutTests() {
    HdpLayout l = validLayout();
    CHECK(l.valid && l.memoryRemapRegister == 0x44b4 && l.registerRemapRegister == 0x44b8);
    CHECK(l.memorySizeRegister == 0x430c && l.memoryFlushRegister == 0x7f000 &&
        l.registerFlushRegister == 0x7f004 && l.barBytes == 0x100000);
    auto b = fixture();
    CHECK(buildHdpLayout(b.data(), b.size(), 0x80000, l) == HdpStatus::Ok);
    const uint64_t bars[] = {0, 3, 0x7efff, 0x7f003, 0x7ffff};
    for (uint64_t bar : bars) reject(b, bar, HdpStatus::OutsideBar);
    for (size_t length = 0; length < b.size(); ++length) {
        l = validLayout();
        CHECK(buildHdpLayout(b.data(), length, 0x100000, l) == HdpStatus::DiscoveryRejected);
        CHECK(zero(l));
    }
    b[0] ^= 1; reject(b, 0x100000, HdpStatus::DiscoveryRejected);
    b = fixture(); b[160] ^= 1; reject(b, 0x100000, HdpStatus::DiscoveryRejected);
    b = fixture(); put16(b, 144, 109); fix(b);
    reject(b, 0x100000, HdpStatus::MissingNbio);
    reject(fixture(0x1000, true), 0x100000, HdpStatus::UnsupportedNbio);
    reject(fixture(0x1000, false, 2), 0x100000, HdpStatus::UnsupportedNbio);
    for (size_t field : {size_t(146), size_t(148), size_t(149), size_t(150), size_t(151)}) {
        b = fixture(); ++b[field]; fix(b);
        reject(b, 0x100000, HdpStatus::UnsupportedNbio);
    }
    b = fixture(); put16(b, 74, 1); put16(b, 140, 1); fix(b);
    reject(b, 0x100000, HdpStatus::UnsupportedNbio);
    // A valid discovery record can still resolve out of BAR, overflow, or collide with the remap hole.
    reject(fixture(UINT32_MAX), UINT64_MAX, HdpStatus::OutsideBar);
    reject(fixture(UINT32_MAX / 4 - 0x12e + 1), UINT64_MAX, HdpStatus::OutsideBar);
    reject(fixture(0x40000), 0x100000, HdpStatus::OutsideBar);
    reject(fixture(0x7f000 / 4 - 0x12d), 0x100000, HdpStatus::OutsideBar);
    reject(fixture(0x7f000 / 4 - 0xc3), 0x100000, HdpStatus::OutsideBar);
    b = fixture(0x80000 / 4 - 0xc3);
    CHECK(buildHdpLayout(b.data(), b.size(), 0x100000, l) == HdpStatus::Ok);
    CHECK(l.memorySizeRegister == 0x80000);
}
struct Event { bool write; uint32_t offset, value; };
struct Fake {
    std::map<uint32_t, uint32_t> registers;
    std::vector<Event> events;
    size_t failAt;
    bool overrideRead;
    uint32_t overrideOffset, overrideValue;
    Fake(const HdpLayout& l) : failAt(SIZE_MAX), overrideRead(false),
        overrideOffset(0), overrideValue(0) { registers[l.memorySizeRegister] = 24576; }
    static bool read(void* ctx, uint32_t offset, uint32_t& value) {
        auto& f = *static_cast<Fake*>(ctx);
        value = f.overrideRead && offset == f.overrideOffset ? f.overrideValue : f.registers[offset];
        f.events.push_back(Event{false, offset, value});
        return f.events.size() - 1 != f.failAt;
    }
    static bool write(void* ctx, uint32_t offset, uint32_t value) {
        auto& f = *static_cast<Fake*>(ctx);
        f.registers[offset] = value; // Even a failed transport can have changed hardware.
        f.events.push_back(Event{true, offset, value});
        return f.events.size() - 1 != f.failAt;
    }
    HdpIo io() { return HdpIo{this, read, write}; }
};
void resultCounts(const Fake& f, const HdpResult& result, size_t start = 0) {
    size_t reads = 0, writes = 0;
    for (size_t i = start; i < f.events.size(); ++i) {
        if (f.events[i].write) ++writes; else ++reads;
    }
    CHECK(result.readsAttempted == reads && result.writesAttempted == writes);
    CHECK(result.writesMayHaveOccurred == (writes != 0));
}
void expectedEvent(const Fake& f, size_t i, bool write, uint32_t offset, uint32_t value) {
    CHECK(i < f.events.size());
    if (i < f.events.size()) CHECK(f.events[i].write == write &&
        f.events[i].offset == offset && f.events[i].value == value);
}
void successProtocol() {
    const HdpLayout l = validLayout(); Fake f(l); HdpState state; HdpResult result{};
    CHECK(!state.configured() && !state.faulted());
    CHECK(flushHdp(f.io(), state, result) == HdpStatus::InvalidState && f.events.empty());
    CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::Ok);
    CHECK(state.configured() && !state.faulted() && f.events.size() == 4);
    resultCounts(f, result);
    expectedEvent(f, 0, true, 0x44b4, 0x7f000);
    expectedEvent(f, 1, true, 0x44b8, 0x7f004);
    expectedEvent(f, 2, false, 0x44b4, 0x7f000);
    expectedEvent(f, 3, false, 0x44b8, 0x7f004);
    CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::InvalidState);
    CHECK(f.events.size() == 4); resultCounts(f, result, 4);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        f.events.clear();
        CHECK(flushHdp(f.io(), state, result) == HdpStatus::Ok);
        CHECK(state.configured() && !state.faulted() && f.events.size() == 4);
        expectedEvent(f, 0, false, 0x44b4, 0x7f000);
        expectedEvent(f, 1, false, 0x44b8, 0x7f004);
        expectedEvent(f, 2, true, 0x7f000, 0);
        expectedEvent(f, 3, false, 0x430c, 24576);
        resultCounts(f, result);
    }
    f.events.clear(); state.clearAfterDeviceReset();
    CHECK(!state.configured() && !state.faulted() && f.events.empty());
    CHECK(flushHdp(f.io(), state, result) == HdpStatus::InvalidState && f.events.empty());
    CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::Ok);
}
void failureProtocol() {
    const HdpLayout l = validLayout();
    for (bool duringFlush : {false, true}) for (size_t fail = 0; fail < 4; ++fail) {
        Fake f(l); HdpState state; HdpResult result{};
        if (duringFlush) CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::Ok);
        f.events.clear(); f.failAt = fail;
        CHECK((duringFlush ? flushHdp(f.io(), state, result) :
            configureHdp(l, f.io(), state, result)) == HdpStatus::TransportError);
        CHECK(f.events.size() == fail + 1 && state.faulted() && !state.configured());
        resultCounts(f, result);
        const size_t old = f.events.size();
        CHECK(flushHdp(f.io(), state, result) == HdpStatus::InvalidState);
        CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::InvalidState);
        CHECK(f.events.size() == old); resultCounts(f, result, old);
        // This only models the caller performing the required independent reset.
        state.clearAfterDeviceReset(); f.events.clear(); f.failAt = SIZE_MAX;
        CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::Ok);
    }
    for (bool duringFlush : {false, true}) for (uint32_t reg : {l.memoryRemapRegister, l.registerRemapRegister})
        for (uint32_t bad : {uint32_t(0), uint32_t(0x7f100), uint32_t(UINT32_MAX)}) {
            Fake f(l); HdpState state; HdpResult result{};
            if (duringFlush) CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::Ok);
            f.events.clear(); f.overrideRead = true; f.overrideOffset = reg; f.overrideValue = bad;
            CHECK((duringFlush ? flushHdp(f.io(), state, result) : configureHdp(l, f.io(), state, result)) ==
                (bad == UINT32_MAX ? HdpStatus::DeviceUnavailable : HdpStatus::ReadbackMismatch));
            CHECK(state.faulted() && !state.configured());
            CHECK(f.events.size() == (duringFlush ? 2u : 4u));
            resultCounts(f, result);
            if (duringFlush) CHECK(!result.writesMayHaveOccurred);
        }
    for (uint32_t bad : {uint32_t(0), uint32_t(UINT32_MAX)}) {
        Fake f(l); HdpState state; HdpResult result{};
        CHECK(configureHdp(l, f.io(), state, result) == HdpStatus::Ok);
        f.events.clear(); f.registers[l.memorySizeRegister] = bad;
        CHECK(flushHdp(f.io(), state, result) == HdpStatus::DeviceUnavailable);
        CHECK(state.faulted() && !state.configured() && result.writesMayHaveOccurred);
        resultCounts(f, result);
    }
}
void invalidArguments() {
    const HdpLayout original = validLayout();
    for (unsigned which = 0; which < 8; ++which) {
        HdpLayout l = original; Fake f(l); HdpState state; HdpResult result{};
        if (which == 0) l.valid = false;
        if (which == 1) l.barBytes = 0x7ffff;
        if (which == 2) ++l.memoryRemapRegister;
        if (which == 3) l.registerRemapRegister = l.memoryRemapRegister;
        if (which == 4) l.memorySizeRegister = l.memoryRemapRegister;
        if (which == 5) l.memoryFlushRegister = 0x7f004;
        HdpIo io = f.io();
        if (which == 6) io.read32 = nullptr;
        if (which == 7) io.write32 = nullptr;
        CHECK(configureHdp(l, io, state, result) == HdpStatus::InvalidArgument);
        CHECK(!state.configured() && !state.faulted() && f.events.empty());
        resultCounts(f, result);
        CHECK(configureHdp(original, f.io(), state, result) == HdpStatus::Ok);
        f.events.clear(); io = f.io(); io.read32 = nullptr;
        CHECK(flushHdp(io, state, result) == HdpStatus::InvalidArgument);
        CHECK(state.configured() && !state.faulted() && f.events.empty());
        resultCounts(f, result);
    }
}
void realDiscovery(const char* path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good()); if (!file) return;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    HdpLayout l{};
    CHECK(buildHdpLayout(b.data(), b.size(), 0x100000, l) == HdpStatus::Ok);
    // Independently recorded NBIO[2]=0xd20 from the supplied Navi31 image.
    CHECK(l.memoryRemapRegister == 0x3934 && l.registerRemapRegister == 0x3938);
    CHECK(l.memorySizeRegister == 0x378c && l.memoryFlushRegister == 0x7f000);
}
} // namespace
int main(int argc, char** argv) {
    layoutTests(); successProtocol(); failureProtocol(); invalidArguments();
    if (argc == 2) realDiscovery(argv[1]);
    std::cout << checks << " HDP assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
