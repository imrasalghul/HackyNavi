// SPDX-License-Identifier: MIT
// CPU simulation only. These tests do not map BARs or execute GPU operations.
#include "hackynavi/nbio.hpp"
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
uint16_t sum(const std::vector<uint8_t>& b, size_t p) {
    uint16_t s = 0;
    for (; p < b.size(); ++p) s = static_cast<uint16_t>(s + b[p]);
    return s;
}
void fix(std::vector<uint8_t>& b) { put16(b, 14, sum(b, 60)); put16(b, 8, sum(b, 10)); }
std::vector<uint8_t> fixture(uint32_t indexBase = 0x14, uint32_t doorbellBase = 0xd20,
                           uint32_t strapBase = 0x4040000, bool wide = false,
                           unsigned count = 1, unsigned bases = 6) {
    const unsigned stride = 8 + bases * (wide ? 8 : 4);
    std::vector<uint8_t> b(144 + count * stride, 0);
    put32(b, 0, 0x28211407); put16(b, 4, 1); put16(b, 6, 4);
    put16(b, 10, static_cast<uint16_t>(b.size())); put16(b, 12, 60);
    put16(b, 16, static_cast<uint16_t>(b.size() - 60));
    put32(b, 60, 0x53445049); put16(b, 64, wide ? 4 : 3);
    put16(b, 66, static_cast<uint16_t>(b.size() - 60));
    put16(b, 72, 1); put16(b, 76, 140); b[138] = wide ? 1 : 0;
    put16(b, 142, static_cast<uint16_t>(count));
    for (unsigned i = 0; i < count; ++i) {
        const size_t p = 144 + i * stride;
        put16(b, p, 108); b[p + 3] = static_cast<uint8_t>(bases);
        b[p + 4] = 4; b[p + 5] = 3;
        const uint32_t values[] = {0, indexBase, doorbellBase, 0x10400, 0x241b000, strapBase};
        for (unsigned j = 0; j < bases && j < 6; ++j)
            put32(b, p + 8 + j * (wide ? 8 : 4), values[j]);
    }
    fix(b); return b;
}
NbioLayout layout(bool directStrap = false, bool indirectDoors = false) {
    const auto b = fixture(0x14, indirectDoors ? 0x20000 : 0xd20,
                           directStrap ? 0x10000 : 0x4040000);
    NbioLayout l = {};
    CHECK(buildNbioLayout(b.data(), b.size(), 0x80000, l) == NbioStatus::Ok);
    return l;
}
bool zero(const NbioLayout& l) {
    return !l.valid && !l.strap2Register && !l.doorbellApertureRegister &&
        !l.selfRingBaseLowRegister && !l.selfRingBaseHighRegister && !l.selfRingControlRegister &&
        !l.rsmuIndexRegister && !l.rsmuDataRegister && !l.rsmuIndexHiRegister && !l.barBytes;
}
void reject(const std::vector<uint8_t>& b, uint64_t bar, NbioStatus wanted) {
    NbioLayout l = layout();
    CHECK(buildNbioLayout(b.data(), b.size(), bar, l) == wanted);
    CHECK(zero(l));
}
void layoutTests() {
    NbioLayout l = layout();
    CHECK(l.strap2Register == 0x10134408 && l.doorbellApertureRegister == 0x3780);
    CHECK(l.selfRingBaseLowRegister == 0x3850 && l.selfRingBaseHighRegister == 0x384c &&
          l.selfRingControlRegister == 0x3854);
    CHECK(l.rsmuIndexRegister == 0x50 && l.rsmuDataRegister == 0x54 && l.rsmuIndexHiRegister == 0x58);
    CHECK(l.barBytes == 0x80000);
    auto b = fixture();
    // Only the complete transport window has to fit. All controls can be indirect.
    CHECK(buildNbioLayout(b.data(), b.size(), 0x5c, l) == NbioStatus::Ok);
    for (uint64_t bar : {uint64_t(0), uint64_t(3), uint64_t(0x50), uint64_t(0x57), uint64_t(0x5b)})
        reject(b, bar, NbioStatus::OutsideBar);
    for (size_t i = 0; i < b.size(); ++i) {
        l = layout();
        CHECK(buildNbioLayout(b.data(), i, 0x80000, l) == NbioStatus::DiscoveryRejected);
        CHECK(zero(l));
    }
    b[0] ^= 1; reject(b, 0x80000, NbioStatus::DiscoveryRejected);
    b = fixture(); b[160] ^= 1; reject(b, 0x80000, NbioStatus::DiscoveryRejected);
    b = fixture(); put16(b, 144, 109); fix(b); reject(b, 0x80000, NbioStatus::MissingNbio);
    reject(fixture(0x14, 0xd20, 0x4040000, false, 0), 0x80000, NbioStatus::MissingNbio);
    reject(fixture(0x14, 0xd20, 0x4040000, false, 2), 0x80000, NbioStatus::UnsupportedNbio);
    reject(fixture(0x14, 0xd20, 0x4040000, true), 0x80000, NbioStatus::UnsupportedNbio);
    for (unsigned count = 0; count < 6; ++count)
        reject(fixture(0x14, 0xd20, 0x4040000, false, 1, count), 0x80000, NbioStatus::UnsupportedNbio);
    for (unsigned field : {146u, 148u, 149u, 150u, 151u}) {
        b = fixture(); ++b[field]; fix(b); reject(b, 0x80000, NbioStatus::UnsupportedNbio);
    }
    b = fixture(); put16(b, 74, 1); put16(b, 140, 1); fix(b);
    reject(b, 0x80000, NbioStatus::UnsupportedNbio);
    for (unsigned base : {1u, 2u, 5u}) {
        b = fixture(); put32(b, 152 + base * 4, UINT32_MAX); fix(b);
        reject(b, UINT64_MAX, NbioStatus::AddressOverflow);
    }
    reject(fixture(UINT32_MAX / 4 - 1), UINT64_MAX, NbioStatus::AddressOverflow);
    reject(fixture(0x14, UINT32_MAX / 4 - 0xf5 + 1), UINT64_MAX, NbioStatus::AddressOverflow);
    reject(fixture(0x14, 0xd20, UINT32_MAX / 4 - 0xd102 + 1), UINT64_MAX, NbioStatus::AddressOverflow);
    b = fixture(0x14, 0xd20, UINT32_MAX / 4 - 0xd102);
    CHECK(buildNbioLayout(b.data(), b.size(), 0x80000, l) == NbioStatus::Ok);
    CHECK(l.strap2Register == 0xfffffffc);
    // Discovery addresses must not alias another fixed register or owned window.
    reject(fixture(0), 0x80000, NbioStatus::InvalidLayout);
    reject(fixture(5), 0x80000, NbioStatus::InvalidLayout); // DATA aliases MM_INDEX_HI.
    reject(fixture(0x7f000 / 4), 0x80000, NbioStatus::InvalidLayout);
    reject(fixture(0x14, 0x7f000 / 4 - 0xc0), 0x80000, NbioStatus::InvalidLayout);
    reject(fixture(0x14, 0xd20, 0x7f000 / 4 - 0xd102), 0x80000, NbioStatus::InvalidLayout);
    reject(fixture(0x10000 + 0xd102, 0xd20, 0x10000), 0x100000, NbioStatus::InvalidLayout);
}
struct Event { bool write; uint32_t offset, value; };
struct Fake {
    NbioLayout l;
    std::map<uint32_t, uint32_t> registers;
    std::vector<Event> events;
    uint32_t selected;
    size_t failAt, corruptAt;
    uint32_t corruptMask;
    bool falsePostingValues;
    Fake(const NbioLayout& a) : l(a), selected(0xdead0000), failAt(SIZE_MAX),
        corruptAt(SIZE_MAX), corruptMask(1), falsePostingValues(false) {
        registers[l.strap2Register] = 0x123456f8;
        registers[l.doorbellApertureRegister] = 0xabcdef00;
        registers[l.selfRingControlRegister] = 0;
        registers[0] = 0x87654321; registers[4] = 0x12345678;
        registers[0x18] = 11; registers[l.rsmuIndexHiRegister] = 0;
    }
    static bool read(void* context, uint32_t offset, uint32_t& value) {
        Fake& f = *static_cast<Fake*>(context);
        CHECK(!(offset & 3) && f.l.barBytes >= 4 && offset <= f.l.barBytes - 4);
        const uint32_t target = offset == f.l.rsmuDataRegister ? f.selected : offset;
        value = offset == f.l.rsmuIndexRegister ?
            (f.falsePostingValues ? UINT32_MAX : f.selected) : f.registers[target];
        if (f.events.size() == f.corruptAt) value ^= f.corruptMask;
        f.events.push_back(Event{false, offset, value});
        return f.events.size() - 1 != f.failAt;
    }
    static bool write(void* context, uint32_t offset, uint32_t value) {
        Fake& f = *static_cast<Fake*>(context);
        CHECK(!(offset & 3) && f.l.barBytes >= 4 && offset <= f.l.barBytes - 4);
        if (offset == f.l.rsmuIndexRegister) f.selected = value;
        else f.registers[offset == f.l.rsmuDataRegister ? f.selected : offset] = value;
        f.events.push_back(Event{true, offset, value});
        return f.events.size() - 1 != f.failAt; // A failing write may still have occurred.
    }
    NbioIo io() { return NbioIo{this, read, write}; }
    void freshEvents() { events.clear(); failAt = corruptAt = SIZE_MAX; }
};
void resultCounts(const Fake& f, const NbioResult& out) {
    unsigned reads = 0, writes = 0, selectors = 0;
    for (const Event& e : f.events) {
        if (e.write) { ++writes; if (e.offset == f.l.rsmuIndexRegister) ++selectors; }
        else ++reads;
        CHECK(e.offset != 0 && e.offset != 4 && e.offset != 0x18);
        CHECK(!e.write || e.offset != f.l.rsmuIndexHiRegister);
    }
    CHECK(out.readsAttempted == reads && out.writesAttempted == writes);
    CHECK(out.selectorWritesAttempted == selectors && out.controlWritesAttempted == writes - selectors);
    CHECK(out.writesMayHaveOccurred == (writes != 0));
    CHECK(f.registers.at(0) == 0x87654321 && f.registers.at(4) == 0x12345678 &&
          f.registers.at(0x18) == 11 && f.registers.at(f.l.rsmuIndexHiRegister) == 0);
}
void expected(const Fake& f, size_t n, bool write, uint32_t offset, uint32_t value) {
    CHECK(n < f.events.size());
    if (n < f.events.size()) CHECK(f.events[n].write == write &&
        f.events[n].offset == offset && f.events[n].value == value);
}
const NbioDoorbellAssignment assignment = {true, 0x12345000000ULL, 0x200000};
void prepare(Fake& f, NbioState& s, bool doorbell = true, bool selfRing = false) {
    NbioResult out = {};
    CHECK(initializeNbioRegisters(f.l, f.io(), s, out) == NbioStatus::Ok);
    if (doorbell) CHECK(setNbioDoorbellAperture(f.io(), s, true, out) == NbioStatus::Ok);
    if (selfRing) CHECK(enableNbioSelfRing(f.io(), s, assignment, out) == NbioStatus::Ok);
    f.freshEvents();
}
void successProtocol() {
    for (bool direct : {false, true}) for (bool indirectDoors : {false, true}) {
        const NbioLayout l = layout(direct, indirectDoors);
        Fake f(l); NbioState state; NbioResult out = {};
        CHECK(!state.initialized() && !state.faulted() && !state.doorbellEnabled() && !state.selfRingEnabled());
        CHECK(initializeNbioRegisters(l, f.io(), state, out) == NbioStatus::Ok);
        CHECK(state.initialized() && !state.faulted());
        CHECK(f.registers[l.strap2Register] == 0x12345678);
        CHECK(f.events.size() == (direct ? 3u : 13u));
        resultCounts(f, out);
        if (!direct) {
            expected(f, 0, false, 0x58, 0);
            expected(f, 1, true, 0x50, l.strap2Register);
            expected(f, 2, false, 0x50, l.strap2Register);
            expected(f, 3, false, 0x54, 0x123456f8);
            expected(f, 4, false, 0x58, 0);
            expected(f, 5, true, 0x50, l.strap2Register);
            expected(f, 6, false, 0x50, l.strap2Register);
            expected(f, 7, true, 0x54, 0x12345678);
            expected(f, 8, false, 0x54, 0x12345678);
            expected(f, 9, false, 0x58, 0);
            expected(f, 10, true, 0x50, l.strap2Register);
            expected(f, 11, false, 0x50, l.strap2Register);
            expected(f, 12, false, 0x54, 0x12345678);
        }
        f.freshEvents();
        CHECK(setNbioDoorbellAperture(f.io(), state, true, out) == NbioStatus::Ok);
        CHECK(state.doorbellEnabled() && f.registers[l.doorbellApertureRegister] == 0xabcdef01);
        resultCounts(f, out); f.freshEvents();
        CHECK(enableNbioSelfRing(f.io(), state, assignment, out) == NbioStatus::Ok);
        CHECK(state.selfRingEnabled() && state.doorbellEnabled());
        CHECK(f.registers[l.selfRingBaseLowRegister] == 0x45000000 &&
              f.registers[l.selfRingBaseHighRegister] == 0x123 && f.registers[l.selfRingControlRegister] == 3);
        if (!indirectDoors) {
            CHECK(f.events.size() == 7);
            expected(f, 0, false, l.selfRingControlRegister, 0);
            expected(f, 1, true, l.selfRingBaseLowRegister, 0x45000000);
            expected(f, 2, true, l.selfRingBaseHighRegister, 0x123);
            expected(f, 3, false, l.selfRingBaseLowRegister, 0x45000000);
            expected(f, 4, false, l.selfRingBaseHighRegister, 0x123);
            expected(f, 5, true, l.selfRingControlRegister, 3);
            expected(f, 6, false, l.selfRingControlRegister, 3);
        }
        resultCounts(f, out); f.freshEvents();
        CHECK(setNbioDoorbellAperture(f.io(), state, false, out) == NbioStatus::Ok);
        CHECK(!state.doorbellEnabled() && state.selfRingEnabled());
        CHECK(f.registers[l.selfRingControlRegister] == 3);
        resultCounts(f, out); f.freshEvents();
        CHECK(disableNbioApertures(f.io(), state, out) == NbioStatus::Ok);
        CHECK(!state.initialized() && !state.faulted() && !state.selfRingEnabled());
        CHECK(f.registers[l.selfRingControlRegister] == 0 && f.registers[l.strap2Register] == 0x12345678);
        CHECK(f.registers[l.selfRingBaseLowRegister] == 0x45000000 && f.registers[l.selfRingBaseHighRegister] == 0x123);
        if (!indirectDoors) {
            CHECK(f.events.size() == 5);
            expected(f, 0, false, l.doorbellApertureRegister, 0xabcdef00);
            expected(f, 1, true, l.doorbellApertureRegister, 0xabcdef00);
            expected(f, 2, false, l.doorbellApertureRegister, 0xabcdef00);
            expected(f, 3, true, l.selfRingControlRegister, 0);
            expected(f, 4, false, l.selfRingControlRegister, 0);
        }
        resultCounts(f, out); f.freshEvents();
        CHECK(initializeNbioRegisters(l, f.io(), state, out) == NbioStatus::Ok);
    }
    // Posting reads are for ordering, not assumed selector values/restoration.
    Fake f(layout()); f.falsePostingValues = true; NbioState state; NbioResult out = {};
    CHECK(initializeNbioRegisters(f.l, f.io(), state, out) == NbioStatus::Ok);
    CHECK(f.selected == f.l.strap2Register); resultCounts(f, out);
    // Exact uint32 byte-address ceiling remains usable through the RSMU window.
    auto b = fixture(0x14, 0xd20, UINT32_MAX / 4 - 0xd102); NbioLayout l = {};
    CHECK(buildNbioLayout(b.data(), b.size(), 0x80000, l) == NbioStatus::Ok);
    Fake ceiling(l); NbioState ceilingState;
    CHECK(initializeNbioRegisters(l, ceiling.io(), ceilingState, out) == NbioStatus::Ok);
    CHECK(ceiling.selected == 0xfffffffc); resultCounts(ceiling, out);
}
enum class Operation { Initialize, EnableDoorbell, DisableDoorbell, SelfRing, Fini };
NbioStatus invoke(Operation op, Fake& f, NbioState& s, NbioResult& out) {
    switch (op) {
    case Operation::Initialize: return initializeNbioRegisters(f.l, f.io(), s, out);
    case Operation::EnableDoorbell: return setNbioDoorbellAperture(f.io(), s, true, out);
    case Operation::DisableDoorbell: return setNbioDoorbellAperture(f.io(), s, false, out);
    case Operation::SelfRing: return enableNbioSelfRing(f.io(), s, assignment, out);
    case Operation::Fini: return disableNbioApertures(f.io(), s, out);
    }
    return NbioStatus::InvalidArgument;
}
void operationPrepare(Operation op, Fake& f, NbioState& s) {
    if (op != Operation::Initialize) prepare(f, s, true, op == Operation::Fini);
}
void failuresAtEveryAccess() {
    for (bool direct : {false, true}) for (bool indirectDoors : {false, true})
        for (Operation op : {Operation::Initialize, Operation::EnableDoorbell,
             Operation::DisableDoorbell, Operation::SelfRing, Operation::Fini}) {
            const NbioLayout l = layout(direct, indirectDoors);
            Fake good(l); NbioState goodState; NbioResult out = {};
            operationPrepare(op, good, goodState);
            CHECK(invoke(op, good, goodState, out) == NbioStatus::Ok);
            for (size_t failure = 0; failure < good.events.size(); ++failure) {
                Fake f(l); NbioState s; operationPrepare(op, f, s); f.failAt = failure;
                CHECK(invoke(op, f, s, out) == NbioStatus::TransportError);
                CHECK(f.events.size() == failure + 1 && s.faulted() && !s.initialized());
                CHECK(!s.doorbellEnabled() && !s.selfRingEnabled());
                resultCounts(f, out); f.freshEvents();
                CHECK(initializeNbioRegisters(l, f.io(), s, out) == NbioStatus::InvalidState);
                CHECK(setNbioDoorbellAperture(f.io(), s, false, out) == NbioStatus::InvalidState);
                CHECK(enableNbioSelfRing(f.io(), s, assignment, out) == NbioStatus::InvalidState);
                CHECK(disableNbioApertures(f.io(), s, out) == NbioStatus::InvalidState);
                CHECK(f.events.empty()); resultCounts(f, out);
                s.clearAfterDeviceReset();
                CHECK(!s.faulted() && !s.initialized() && f.events.empty());
                CHECK(initializeNbioRegisters(l, f.io(), s, out) == NbioStatus::Ok);
            }
        }
}
void badReadbacks() {
    for (bool direct : {false, true}) {
        const NbioLayout l = layout(direct);
        for (bool allOnes : {false, true}) {
            Fake f(l); NbioState s; NbioResult out = {};
            if (allOnes) f.registers[l.strap2Register] = UINT32_MAX;
            else { f.corruptAt = direct ? 2 : 12; f.corruptMask = 0x80; }
            CHECK(initializeNbioRegisters(l, f.io(), s, out) ==
                  (allOnes ? NbioStatus::DeviceUnavailable : NbioStatus::ReadbackMismatch));
            CHECK(s.faulted()); resultCounts(f, out);
            if (allOnes) CHECK(out.controlWritesAttempted == 0);
        }
    }
    const NbioLayout l = layout();
    for (unsigned op = 0; op < 4; ++op) {
        Fake f(l); NbioState s; NbioResult out = {}; prepare(f, s);
        if (op == 0) { f.corruptAt = 2; f.corruptMask = 1; }
        if (op == 1) { f.corruptAt = 3; f.corruptMask = 0x1000; }
        if (op == 2) { f.corruptAt = 4; f.corruptMask = 1; }
        if (op == 3) { f.corruptAt = 6; f.corruptMask = 0x100; }
        CHECK((op == 0 ? setNbioDoorbellAperture(f.io(), s, true, out) :
                        enableNbioSelfRing(f.io(), s, assignment, out)) == NbioStatus::ReadbackMismatch);
        CHECK(s.faulted()); resultCounts(f, out);
        if (op == 1 || op == 2) CHECK(f.registers[l.selfRingControlRegister] == 0);
    }
    for (uint32_t inherited : {uint32_t(1), uint32_t(3), uint32_t(0xfff03), uint32_t(UINT32_MAX)}) {
        Fake f(l); NbioState s; NbioResult out = {}; prepare(f, s);
        f.registers[l.selfRingControlRegister] = inherited;
        CHECK(enableNbioSelfRing(f.io(), s, assignment, out) ==
            (inherited == UINT32_MAX ? NbioStatus::DeviceUnavailable : NbioStatus::SelfRingAlreadyEnabled));
        CHECK(s.faulted() && out.controlWritesAttempted == 0); resultCounts(f, out);
    }
    // Fini deliberately clears inherited self-ring even when this state did not enable it.
    Fake f(l); NbioState s; NbioResult out = {}; prepare(f, s);
    f.registers[l.selfRingControlRegister] = 0xfff03;
    CHECK(disableNbioApertures(f.io(), s, out) == NbioStatus::Ok);
    CHECK(f.registers[l.selfRingControlRegister] == 0); resultCounts(f, out);
}
void invalidArguments() {
    const NbioLayout original = layout();
    for (unsigned field = 0; field < 13; ++field) {
        NbioLayout l = original;
        if (field == 0) l.valid = false;
        if (field == 1) ++l.strap2Register;
        if (field == 2) l.strap2Register = l.selfRingControlRegister;
        if (field == 3) l.rsmuDataRegister += 4;
        if (field == 4) l.rsmuIndexHiRegister += 4;
        if (field == 5) l.selfRingBaseLowRegister += 4;
        if (field == 6) l.selfRingBaseHighRegister += 4;
        if (field == 7) l.selfRingControlRegister += 4;
        if (field == 8) l.doorbellApertureRegister += 4;
        if (field == 9) l.barBytes = 0x5b;
        if (field == 10) l.strap2Register = 0x18;
        if (field == 11) l.strap2Register = 0x7f004;
        if (field == 12) l.rsmuIndexRegister = UINT32_MAX - 3;
        Fake f(original); NbioState s; NbioResult out = {};
        CHECK(initializeNbioRegisters(l, f.io(), s, out) != NbioStatus::Ok);
        CHECK(f.events.empty() && !s.faulted()); resultCounts(f, out);
    }
    for (bool missingRead : {false, true}) {
        Fake f(original); NbioState s; NbioResult out = {};
        NbioIo io = f.io(); if (missingRead) io.read32 = nullptr; else io.write32 = nullptr;
        CHECK(initializeNbioRegisters(original, io, s, out) == NbioStatus::InvalidArgument);
        CHECK(f.events.empty() && !s.faulted()); resultCounts(f, out);
        prepare(f, s);
        CHECK(setNbioDoorbellAperture(io, s, false, out) == NbioStatus::InvalidArgument);
        CHECK(enableNbioSelfRing(io, s, assignment, out) == NbioStatus::InvalidArgument);
        CHECK(disableNbioApertures(io, s, out) == NbioStatus::InvalidArgument);
        CHECK(f.events.empty() && s.initialized()); resultCounts(f, out);
    }
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        Fake f(original); NbioState s; NbioResult out = {}; prepare(f, s);
        NbioDoorbellAssignment a = assignment;
        if (invalid == 0) a.externallyVerified = false;
        if (invalid == 1) a.busAddress = 0;
        if (invalid == 2) a.bytes = 0;
        if (invalid == 3) a.bytes = 2;
        if (invalid == 4) a.bytes = 0x300000;
        if (invalid == 5) ++a.busAddress;
        if (invalid == 6) { a.busAddress = UINT64_MAX; a.bytes = 4; }
        CHECK(enableNbioSelfRing(f.io(), s, a, out) == NbioStatus::InvalidDoorbellAssignment);
        CHECK(f.events.empty() && !s.faulted() && s.initialized()); resultCounts(f, out);
    }
    // Absent initialization, absent aperture, or repeated active self-ring has no MMIO.
    Fake f(original); NbioState s; NbioResult out = {};
    CHECK(setNbioDoorbellAperture(f.io(), s, true, out) == NbioStatus::InvalidState);
    CHECK(enableNbioSelfRing(f.io(), s, assignment, out) == NbioStatus::InvalidState);
    CHECK(disableNbioApertures(f.io(), s, out) == NbioStatus::InvalidState);
    CHECK(f.events.empty()); prepare(f, s, false);
    CHECK(enableNbioSelfRing(f.io(), s, assignment, out) == NbioStatus::InvalidState);
    CHECK(initializeNbioRegisters(original, f.io(), s, out) == NbioStatus::InvalidState);
    CHECK(f.events.empty());
    CHECK(setNbioDoorbellAperture(f.io(), s, true, out) == NbioStatus::Ok);
    CHECK(enableNbioSelfRing(f.io(), s, assignment, out) == NbioStatus::Ok); f.freshEvents();
    CHECK(enableNbioSelfRing(f.io(), s, assignment, out) == NbioStatus::InvalidState);
    CHECK(f.events.empty()); resultCounts(f, out);
}
void highIndexGuards() {
    // Reject stale HI before each individual indirect transaction, including
    // later transactions after earlier control writes have already happened.
    for (Operation op : {Operation::Initialize, Operation::EnableDoorbell,
         Operation::DisableDoorbell, Operation::SelfRing, Operation::Fini}) {
        const NbioLayout l = layout(false, true);
        Fake good(l); NbioState gs; NbioResult out = {};
        operationPrepare(op, good, gs); CHECK(invoke(op, good, gs, out) == NbioStatus::Ok);
        for (size_t i = 0; i < good.events.size(); ++i) {
            if (good.events[i].write || good.events[i].offset != l.rsmuIndexHiRegister) continue;
            for (uint32_t high : {uint32_t(1), uint32_t(0xff), uint32_t(0x80000000), uint32_t(UINT32_MAX)}) {
                Fake f(l); NbioState s; operationPrepare(op, f, s);
                f.corruptAt = i; f.corruptMask = high;
                CHECK(invoke(op, f, s, out) == (high == UINT32_MAX ?
                    NbioStatus::DeviceUnavailable : NbioStatus::IndirectHighNotZero));
                CHECK(f.events.size() == i + 1 && s.faulted()); resultCounts(f, out);
                if (i == 0) CHECK(!out.writesMayHaveOccurred);
            }
        }
    }
    const NbioLayout l = layout(); Fake f(l); NbioState s; NbioResult out = {};
    f.registers[l.rsmuIndexHiRegister] = 2;
    CHECK(initializeNbioRegisters(l, f.io(), s, out) == NbioStatus::IndirectHighNotZero);
    CHECK(f.events.size() == 1 && !out.writesMayHaveOccurred && f.registers[l.rsmuIndexHiRegister] == 2);
}
void randomizedRmw() {
    uint32_t random = 0x934aed81;
    for (unsigned trial = 0; trial < 2000; ++trial) {
        random = random * 1664525u + 1013904223u;
        const NbioLayout l = layout((trial & 1) != 0, (trial & 2) != 0);
        Fake f(l); NbioState s; NbioResult out = {};
        const uint32_t strap = random == UINT32_MAX ? random - 1 : random;
        f.registers[l.strap2Register] = strap;
        CHECK(initializeNbioRegisters(l, f.io(), s, out) == NbioStatus::Ok);
        CHECK(f.registers[l.strap2Register] == (strap & ~uint32_t(0x80)));
        random = random * 1664525u + 1013904223u;
        // Keep one unrelated bit zero so enabling bit0 does not mimic device removal.
        const uint32_t aperture = random & ~uint32_t(2);
        f.registers[l.doorbellApertureRegister] = aperture; f.freshEvents();
        CHECK(setNbioDoorbellAperture(f.io(), s, true, out) == NbioStatus::Ok);
        CHECK(f.registers[l.doorbellApertureRegister] == (aperture | 1)); resultCounts(f, out);
        f.freshEvents();
        CHECK(setNbioDoorbellAperture(f.io(), s, false, out) == NbioStatus::Ok);
        CHECK(f.registers[l.doorbellApertureRegister] == (aperture & ~uint32_t(1))); resultCounts(f, out);
    }
    // Highest legal assigned BAR preserves all 64 address bits; high=all-ones is valid data.
    Fake f(layout()); NbioState s; NbioResult out = {}; prepare(f, s);
    NbioDoorbellAssignment last = {true, 0xffffffffffe00000ULL, 0x200000};
    CHECK(enableNbioSelfRing(f.io(), s, last, out) == NbioStatus::Ok);
    CHECK(f.registers[f.l.selfRingBaseHighRegister] == UINT32_MAX &&
          f.registers[f.l.selfRingBaseLowRegister] == 0xffe00000); resultCounts(f, out);
}
void actualDiscovery(const char* path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    CHECK(bool(stream)); if (!stream) return;
    const std::streamoff bytes = stream.tellg();
    CHECK(bytes > 0 && bytes <= 65535); if (bytes <= 0 || bytes > 65535) return;
    stream.seekg(0); std::vector<uint8_t> data(static_cast<size_t>(bytes));
    stream.read(reinterpret_cast<char*>(data.data()), bytes); CHECK(bool(stream));
    NbioLayout l = {};
    CHECK(buildNbioLayout(data.data(), data.size(), 0x100000, l) == NbioStatus::Ok);
    CHECK(l.strap2Register == 0x10134408 && l.rsmuIndexRegister == 0x50 && l.rsmuDataRegister == 0x54);
    CHECK(l.doorbellApertureRegister == 0x3780 && l.selfRingBaseLowRegister == 0x3850 &&
          l.selfRingBaseHighRegister == 0x384c && l.selfRingControlRegister == 0x3854);
    // The real metadata selects the emulated address path, never real MMIO.
    Fake f(l); NbioState s; NbioResult out = {};
    CHECK(initializeNbioRegisters(l, f.io(), s, out) == NbioStatus::Ok);
    resultCounts(f, out);
}
} // namespace
int main(int argc, char** argv) {
    layoutTests(); successProtocol(); failuresAtEveryAccess(); badReadbacks();
    invalidArguments(); highIndexGuards(); randomizedRmw();
    if (argc > 1) actualDiscovery(argv[1]);
    std::cout << checks << " assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
