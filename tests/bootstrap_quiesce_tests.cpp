// SPDX-License-Identifier: MIT
// Offline callback tests only; this executable never accesses hardware.
#include "hackynavi/bootstrap_quiesce.hpp"
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::bootstrap;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; } } while (0)
constexpr uint64_t barBytes = 0x80000;
const PollOptions polls = {3, 2};
uint32_t gc(unsigned base, uint32_t offset) { return ((base ? 0xa000 : 0x1260) + offset) * 4; }
uint32_t sdma(unsigned engine, uint32_t offset) {
    return gc(offset >= 0x5880, offset + engine * (offset >= 0x5880 ? 0x20 : 0x600));
}
uint32_t sos() { return (0x16000 + 0x91) * 4; }
uint32_t context(unsigned hub, unsigned index) {
    return ((hub ? 0x1a000 + 0x740 : 0x1260 + 0x1688) + index) * 4;
}
Navi31IpLayout layout() {
    Navi31IpLayout l = {};
    l.valid = true;
    const uint16_t ids[6] = {11,34,255,1,42,43};
    const uint8_t majors[6] = {11,3,13,13,6,6};
    for (unsigned i = 0; i < 6; ++i) l.blocks[i] = {ids[i],0,0,majors[i],0,0,2,{0x1260,0xa000}};
    l.blocks[1].basesDwords[0] = 0x1a000;
    l.blocks[2].basesDwords[0] = 0x16000;
    return l;
}
struct Event { char kind; uint32_t address, value; };
struct Fake {
    std::map<uint32_t,uint32_t> regs;
    std::vector<Event> events;
    unsigned reads = 0, writes = 0, idleReads = 0;
    unsigned failRead = 0, failWrite = 0, allOnesRead = 0, busyPolls = 0;
    uint32_t corruptAddress = UINT32_MAX, corruptMask = 0;
    uint32_t busyAddress = UINT32_MAX;
    bool rs64;
    explicit Fake(bool rs = false) : rs64(rs) {
        regs[gc(1,0x2807)] = 0x40000000; // halted MES, no active pipes
        regs[gc(1,0x2a00)] = rs ? 1 : 0;
        regs[gc(1,0x2903)] = rs ? 1 : 0;
        regs[gc(1,0x803)] = 0x100a000;
        regs[gc(1,0x802)] = 0x8000;
        regs[gc(1,0x2904)] = 0x3c008000;
        for (unsigned e = 0; e < 2; ++e) {
            regs[sdma(e,0x80)] = 0x41;
            regs[sdma(e,0x8a)] = 0x21;
            regs[sdma(e,0x589a)] = 0x10;
            regs[sdma(e,0x25)] = 1;
        }
    }
    Io io() { return {this,read,write,delay}; }
    static bool read(void* p,uint32_t a,uint32_t& v) {
        auto& f = *static_cast<Fake*>(p); ++f.reads;
        v = f.regs[a];
        if (a == gc(0,0xf40)) ++f.idleReads;
        if (f.busyPolls >= f.idleReads && f.idleReads && a == f.busyAddress)
            v = a == gc(0,0xda4) ? 0x80000000 : a == gc(0,0xf40) ? 1 : 0;
        if (f.writes && a == f.corruptAddress) v ^= f.corruptMask;
        if (f.reads == f.allOnesRead) v = UINT32_MAX;
        f.events.push_back({'r',a,v});
        return f.reads != f.failRead;
    }
    static bool write(void* p,uint32_t a,uint32_t v) {
        auto& f = *static_cast<Fake*>(p); ++f.writes;
        f.events.push_back({'w',a,v}); f.regs[a] = v;
        return f.writes != f.failWrite; // failed writes can still land
    }
    static void delay(void* p,uint32_t us) {
        static_cast<Fake*>(p)->events.push_back({'d',0,us});
    }
};
void golden() {
    for (unsigned rs = 0; rs < 2; ++rs) {
        Fake f(rs); Result r;
        CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) == Status::Ok);
        CHECK(r.stage == Stage::Complete && r.rs64 == bool(rs));
        CHECK(r.writesMayHaveOccurred && r.writeAttempts == 8 && r.idlePolls == 1);
        CHECK(r.readAttempts == f.reads && f.idleReads == 1);
        CHECK(f.regs[gc(1,0x803)] == 0x1500a000);
        CHECK(f.regs[gc(1,rs ? 0x2904 : 0x802)] == (rs ? 0x400f8010U : 0x50008000U));
        std::vector<Event> writes;
        unsigned delayCount = 0;
        for (const auto& e : f.events) {
            if (e.kind == 'w') writes.push_back(e);
            if (e.kind == 'd') { ++delayCount; CHECK(e.value == 50); }
        }
        CHECK(delayCount == 1 && writes.size() == 8);
        CHECK(writes[0].address == gc(1,0x803));
        CHECK(writes[1].address == gc(1,rs ? 0x2904 : 0x802));
        for (unsigned e = 0; e < 2; ++e) {
            CHECK(writes[2+e*2].address == sdma(e,0x80) && writes[2+e*2].value == 0x40);
            CHECK(writes[3+e*2].address == sdma(e,0x8a) && writes[3+e*2].value == 0x20);
            CHECK(writes[6+e].address == sdma(e,0x589a) && writes[6+e].value == 0x11);
        }
        size_t firstWrite = 0;
        while (firstWrite < f.events.size() && f.events[firstWrite].kind != 'w') ++firstWrite;
        CHECK(firstWrite == 72); // all exclusions and RMW sources precede mutation
    }
}
void badArguments() {
    const PollOptions bad[] = {{0,1},{100001,1},{1,0},{1,11}};
    for (auto option : bad) {
        Fake f; Result r;
        CHECK(quiesceCpSdma(layout(),barBytes,f.io(),option,r) == Status::InvalidArgument);
        CHECK(f.events.empty() && !r.writesMayHaveOccurred);
    }
    for (unsigned callback = 0; callback < 3; ++callback) {
        Fake f; Result r; auto io = f.io();
        if (callback == 0) io.read32 = nullptr;
        if (callback == 1) io.write32 = nullptr;
        if (callback == 2) io.delayUs = nullptr;
        CHECK(quiesceCpSdma(layout(),barBytes,io,polls,r) == Status::InvalidArgument);
        CHECK(f.events.empty());
    }
    for (unsigned block = 0; block < 6; ++block) {
        Fake f; Result r; auto l = layout(); ++l.blocks[block].revision;
        CHECK(quiesceCpSdma(l,barBytes,f.io(),polls,r) == Status::UnsupportedLayout);
        CHECK(f.events.empty());
    }
    Fake f; Result r;
    CHECK(quiesceCpSdma(layout(),0x100,f.io(),polls,r) == Status::OutsideBar);
    CHECK(f.events.empty());
    auto l = layout(); l.blocks[2].basesDwords[0] = UINT32_MAX;
    CHECK(quiesceCpSdma(l,barBytes,f.io(),polls,r) == Status::OutsideBar);
    CHECK(f.events.empty());
    l = layout(); l.blocks[2].basesDwords[0] = 0xa000 + 0x803 - 0x91;
    CHECK(quiesceCpSdma(l,barBytes,f.io(),polls,r) == Status::AliasedRegisters);
    CHECK(f.events.empty());
}
void rejectGuards() {
    std::vector<Event> invalid = {{'s',sos(),1},{'m',gc(1,0x2807),0}};
    for (unsigned pipe = 0; pipe < 4; ++pipe)
        invalid.push_back({'m',gc(1,0x2807),0x40000000U | (1U << (26 + pipe))});
    for (unsigned h = 0; h < 2; ++h)
        for (unsigned c = 0; c < 16; ++c) invalid.push_back({'v',context(h,c),1});
    for (unsigned e = 0; e < 2; ++e)
        for (unsigned q = 1; q < 8; ++q)
            for (unsigned ib = 0; ib < 2; ++ib)
                invalid.push_back({'q',sdma(e,0x80+q*0x58+ib*0xa),1});
    for (const auto& test : invalid) {
        Fake f; Result r; f.regs[test.address] = test.value;
        const Status wanted = test.kind == 's' ? Status::PriorDriverRequiresReset :
            test.kind == 'm' ? Status::ActiveMes : test.kind == 'v' ? Status::ActiveVmContext : Status::ActiveSdmaQueue;
        CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) == wanted);
        CHECK(!r.writesMayHaveOccurred && !f.writes && r.stage == Stage::BootstrapGuards);
        // A guard changing only after stop must also fail; never emit a stale success.
        Fake late; late.corruptAddress = test.address;
        late.corruptMask = late.regs[test.address] ^ test.value;
        CHECK(quiesceCpSdma(layout(),barBytes,late.io(),polls,r) == wanted);
        CHECK(r.writesMayHaveOccurred && r.stage == Stage::RecheckGuards);
    }
    for (unsigned which = 0; which < 2; ++which) {
        Fake f; Result r; f.regs[gc(1,which ? 0x2903 : 0x2a00)] = 1;
        CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) == Status::UnsupportedArchitecture);
        CHECK(!f.writes);
    }
}
void faults() {
    for (unsigned rs = 0; rs < 2; ++rs) {
        Fake golden(rs); Result r;
        CHECK(quiesceCpSdma(layout(),barBytes,golden.io(),polls,r) == Status::Ok);
        const unsigned reads = golden.reads;
        for (unsigned nth = 1; nth <= reads; ++nth) {
            for (unsigned unavailable = 0; unavailable < 2; ++unavailable) {
                Fake f(rs);
                if (unavailable) f.allOnesRead = nth; else f.failRead = nth;
                CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) ==
                    (unavailable ? Status::DeviceUnavailable : Status::ReadFailed));
                CHECK(r.readAttempts == nth && f.events.back().kind == 'r');
                CHECK(r.writesMayHaveOccurred == (nth > 72));
            }
        }
        for (unsigned nth = 1; nth <= 8; ++nth) {
            Fake f(rs); f.failWrite = nth;
            CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) == Status::WriteFailed);
            CHECK(r.writesMayHaveOccurred && r.writeAttempts == nth);
            CHECK(f.events.back().kind == 'w');
        }
        const Event bad[] = {
            {'r',gc(1,0x803),0x10000000}, {'r',gc(1,0x803),0x4000000},
            {'r',gc(1,rs ? 0x2904 : 0x802),0x40000000},
            {'r',gc(1,0x2a00),1}, {'r',gc(1,0x2903),1},
            {'r',sdma(0,0x80),1}, {'r',sdma(0,0x8a),1}, {'r',sdma(0,0x589a),1},
            {'r',sdma(1,0x80),1}, {'r',sdma(1,0x8a),1}, {'r',sdma(1,0x589a),1}
        };
        for (const auto& test : bad) {
            Fake f(rs); f.corruptAddress = test.address; f.corruptMask = test.value;
            CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) == Status::ControlReadbackMismatch);
            CHECK(r.stage == Stage::VerifyControls && r.idlePolls == 0);
        }
        if (rs) for (unsigned pipe = 0; pipe < 4; ++pipe) {
            for (unsigned active = 0; active < 2; ++active) {
                Fake f(true); f.corruptAddress = gc(1,0x2904);
                f.corruptMask = 1U << (pipe + (active ? 26 : 16));
                CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) == Status::ControlReadbackMismatch);
            }
        }
    }
}
void idle() {
    const uint32_t addresses[] = {gc(0,0xf40),gc(0,0xda4),sdma(0,0x25),sdma(1,0x25)};
    for (uint32_t address : addresses) {
        for (unsigned busy = 0; busy <= 3; ++busy) {
            Fake f; Result r; f.busyAddress = address; f.busyPolls = busy;
            CHECK(quiesceCpSdma(layout(),barBytes,f.io(),polls,r) ==
                (busy == 3 ? Status::IdleTimeout : Status::Ok));
            CHECK(r.idlePolls == (busy == 3 ? 3 : busy + 1));
            unsigned delayCount = 0;
            for (const auto& e : f.events) if (e.kind == 'd' && e.value == 2) ++delayCount;
            CHECK(delayCount == r.idlePolls - 1);
        }
    }
}
void postedState() {
    constexpr uint32_t base = 0x4c, scratch7 = (base + 7) * 4;
    Fake f; Result r; f.regs[scratch7] = 0x80000240;
    auto io = f.io(); io.write32 = nullptr; io.delayUs = nullptr;
    CHECK(checkPostedColdState(layout(), barBytes, base, io, r) == Status::Ok);
    CHECK(r.biosScratch7 == 0x80000240 && !r.sosSignOfLife && r.readAttempts == 3);
    CHECK(!r.writesMayHaveOccurred && !f.writes && f.events.size() == 3);
    CHECK(f.events[0].address == sos() && f.events[1].address == scratch7 && f.events[2].address == sos());
    for (uint32_t other : {0U, 0x100U, 0x400U, 0x10000000U}) {
        Fake g; g.regs[scratch7] = other;
        CHECK(checkPostedColdState(layout(), barBytes, base, g.io(), r) == Status::AtomPostRequired);
        g.regs[scratch7] |= 0x200;
        CHECK(checkPostedColdState(layout(), barBytes, base, g.io(), r) == Status::Ok);
    }
    for (uint32_t running : {1U, 0x100U, 0xdeadbeefU}) {
        Fake g; g.regs[sos()] = running;
        CHECK(checkPostedColdState(layout(), barBytes, base, g.io(), r) == Status::PriorDriverRequiresReset);
        CHECK(g.reads == 1 && !g.writes);
    }
    for (unsigned failure = 1; failure <= 3; ++failure) {
        Fake g; g.regs[scratch7] = 0x200; g.failRead = failure;
        CHECK(checkPostedColdState(layout(), barBytes, base, g.io(), r) == Status::ReadFailed);
        CHECK(g.reads == failure && !g.writes);
        Fake h; h.regs[scratch7] = 0x200; h.allOnesRead = failure;
        CHECK(checkPostedColdState(layout(), barBytes, base, h.io(), r) == Status::DeviceUnavailable);
        CHECK(h.reads == failure && !h.writes);
    }
    // Every malformed address/layout fails before reading hardware.
    for (uint32_t invalid : {0U, UINT32_MAX, 0x1fffffffU, 0x1fc00U}) {
        Fake g;
        CHECK(checkPostedColdState(layout(), barBytes, invalid, g.io(), r) != Status::Ok);
        CHECK(!g.reads && !g.writes);
    }
    Fake g;
    CHECK(checkPostedColdState(layout(), barBytes, sos() / 4 - 7, g.io(), r) == Status::AliasedRegisters);
    CHECK(!g.reads && !g.writes);
    auto bad = layout(); bad.valid = false;
    CHECK(checkPostedColdState(bad, barBytes, base, g.io(), r) == Status::UnsupportedLayout);
    CHECK(!g.reads && !g.writes);
    // Sign of life may appear between the two reads; the second observation
    // must prevent interpreting a previously posted card as a cold device.
    struct Transition {
        unsigned calls;
        static bool read(void* c, uint32_t a, uint32_t& v) {
            auto& s = *static_cast<Transition*>(c); ++s.calls;
            v = a == sos() ? (s.calls == 3 ? 1 : 0) : 0x200;
            return true;
        }
    } transition = {};
    const Io changing = {&transition, Transition::read, nullptr, nullptr};
    CHECK(checkPostedColdState(layout(), barBytes, base, changing, r) == Status::PriorDriverRequiresReset);
    CHECK(transition.calls == 3 && r.sosSignOfLife == 1);
}
} // namespace
int main() {
    golden(); badArguments(); rejectGuards(); faults(); idle(); postedState();
    std::cout << checks << " bootstrap quiescence assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
