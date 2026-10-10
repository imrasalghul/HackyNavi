// SPDX-License-Identifier: MIT
// Simulated callbacks only: this executable never accesses GPU hardware.
#include "hackynavi/gart.hpp"
#include <cstring>
#include <iostream>
#include <map>
#include <type_traits>
#include <vector>

using namespace hackynavi;
using namespace hackynavi::gart;
static_assert(!std::is_copy_constructible<Plan>::value, "plans cannot be cloned for replay");
static_assert(!std::is_copy_assignable<Plan>::value, "plans cannot be assigned for replay");
static_assert(!std::is_move_constructible<Plan>::value, "moving must not leave a replayable plan");
static_assert(!std::is_move_assignable<Plan>::value, "plans cannot be move-assigned");
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    std::cerr << "FAIL line " << __LINE__ << ": " #expression "\n"; } } while (0)
constexpr uint32_t gfxBase = 0x2000, mmBase = 0x18000;
constexpr uint64_t barBytes = 0x200000;
const PollOptions polls = {3, 1};
uint32_t byte(Hub hub, uint32_t reg) { return ((hub == Hub::Gfx ? gfxBase : mmBase) + reg) * 4; }
uint32_t context(Hub hub) { return byte(hub, hub == Hub::Gfx ? 0x1688 : 0x0740); }
uint32_t sem(Hub hub) { return byte(hub, hub == Hub::Gfx ? 0x16aa : 0x0762); }
uint32_t req(Hub hub) { return byte(hub, hub == Hub::Gfx ? 0x16bc : 0x0774); }
uint32_t ack(Hub hub) { return byte(hub, hub == Hub::Gfx ? 0x16ce : 0x0786); }
uint32_t priv() { return byte(Hub::Mm, 0x071b); }

Navi31IpLayout layout() {
    Navi31IpLayout l = {};
    l.valid = true;
    l.blocks[0] = {11, 0, 0, 11, 0, 0, 2, {gfxBase, 0xa000}};
    l.blocks[1] = {34, 0, 0, 3, 0, 0, 2, {mmBase, 0x2408800}};
    return l;
}
Config config() {
    MemoryLayout m = {};
    const MemoryLayoutInput input = {0x100, 0x6ff, 0x1000,
        24ULL << 30, 0x8000000000ULL, 256ULL << 20};
    CHECK(deriveMemoryLayout(input, m) == MemoryLayoutStatus::Ok);
    // MC VRAM starts4GiB, physical VRAM starts64GiB, CPU BAR is512GiB.
    return {m, 0x1000200000ULL, 0x100000, 0x7fff00000000ULL,
            0x20000000, 0x1000001000ULL, 0x123456000ULL, false};
}
struct Event { char kind; uint32_t address, value; };
struct Fake {
    std::map<uint32_t, uint32_t> registers;
    std::vector<Event> events;
    unsigned reads = 0, writes = 0, flushes = 0, delays = 0;
    unsigned failRead = 0, failWrite = 0, failFlush = 0, allOnesRead = 0;
    unsigned semDelay = 0, ackDelay = 0, semSeen = 0, ackSeen = 0;
    uint32_t xorReadAddress = UINT32_MAX, xorReadValue = 0;
    bool noSemaphore = false, noAck = false, wrongPosting = false;
    bool activeRequest[2] = {false, false};
    Fake() {
        for (unsigned h = 0; h < 2; ++h) {
            Hub hub = static_cast<Hub>(h);
            registers[byte(hub, h ? 0x08f3 : 0x167f)] = 0x10000006;
            registers[byte(hub, h ? 0x0700 : 0x15bc)] = 0x80000004;
            registers[byte(hub, h ? 0x0701 : 0x15bd)] = 0x100;
            registers[byte(hub, h ? 0x0709 : 0x15c5)] = 0x20;
            registers[byte(hub, h ? 0x0708 : 0x15c4)] = 0xc0000001;
        }
        registers[priv()] = 0x40;
        registers[byte(Hub::Gfx, 0x1e1f)] = 0x10;
    }
    Io io() { return {this, read, write, delay, hdp}; }
    void clearLog() { events.clear(); reads = writes = flushes = delays = 0; semSeen = ackSeen = 0; }
    static bool read(void* p, uint32_t address, uint32_t& value) {
        Fake& f = *static_cast<Fake*>(p);
        ++f.reads;
        value = f.registers[address];
        if (address == sem(Hub::Mm)) {
            value = !f.noSemaphore && f.semSeen++ >= f.semDelay ? 1 : 0;
        }
        for (unsigned h = 0; h < 2; ++h)
            if (address == ack(static_cast<Hub>(h))) {
                CHECK(f.activeRequest[h]);
                value = !f.noAck && f.ackSeen++ >= f.ackDelay ? 1 : 0;
            }
        if (f.wrongPosting && (address == context(Hub::Gfx) || address == context(Hub::Mm)))
            value ^= 1;
        if (address == f.xorReadAddress) value ^= f.xorReadValue;
        if (f.reads == f.allOnesRead) value = UINT32_MAX;
        f.events.push_back({'r', address, value});
        return f.reads != f.failRead;
    }
    static bool write(void* p, uint32_t address, uint32_t value) {
        Fake& f = *static_cast<Fake*>(p);
        ++f.writes;
        f.events.push_back({'w', address, value});
        // A failed callback may nevertheless have changed hardware.
        f.registers[address] = value;
        for (unsigned h = 0; h < 2; ++h)
            if (address == req(static_cast<Hub>(h))) f.activeRequest[h] = true;
        return f.writes != f.failWrite;
    }
    static void delay(void* p, uint32_t us) {
        Fake& f = *static_cast<Fake*>(p);
        ++f.delays;
        f.events.push_back({'d', 0, us});
    }
    static bool hdp(void* p) {
        Fake& f = *static_cast<Fake*>(p);
        ++f.flushes;
        f.events.push_back({'h', 0, 0});
        return f.flushes != f.failFlush;
    }
};

const Step* findWrite(const Plan& p, uint32_t address, unsigned occurrence = 0) {
    for (size_t i = 0; i < p.stepCount(); ++i) {
        const Step* step = p.step(i);
        if (step->kind == StepKind::Write && step->byteOffset == address) {
            if (!occurrence) return step;
            --occurrence;
        }
    }
    return nullptr;
}
void value(const Plan& p, Hub hub, uint32_t reg, uint32_t expected) {
    const Step* found = findWrite(p, byte(hub, reg));
    CHECK(found != nullptr);
    if (found) CHECK(found->value == expected);
}

void goldenEnable() {
    for (unsigned h = 0; h < 2; ++h) {
        const Hub hub = static_cast<Hub>(h);
        Fake f; Plan p; Result r; Config c = config();
        CHECK(prepareEnable(layout(), barBytes, hub, c, f.io(), p, r) == Status::Ok);
        CHECK(p.valid() && !p.consumed() && p.hub() == hub);
        CHECK(p.stepCount() == (h ? 86 : 87));
        CHECK(p.step(p.stepCount()) == nullptr);
        CHECK(f.writes == 0 && f.flushes == 0 && !r.writesMayHaveOccurred);
        for (unsigned index = 0; index < 16; ++index) {
            CHECK(p.step(index)->kind == StepKind::Write);
            CHECK(p.step(index)->byteOffset == context(hub) + index * 4);
            CHECK(p.step(index)->value == 0);
        }
        // Independent literal field values and offsets from generated headers.
        value(p, hub, h ? 0x07ab : 0x16f3, 0x00200001); // Physical, not MC/CPU BAR.
        value(p, hub, h ? 0x07ac : 0x16f4, 0x10);
        value(p, hub, h ? 0x07cb : 0x1713, 0xfff00000);
        value(p, hub, h ? 0x07cc : 0x1714, 0x7);
        value(p, hub, h ? 0x07eb : 0x1733, 0xfff1ffff);
        value(p, hub, h ? 0x07ec : 0x1734, 0x7);
        value(p, hub, h ? 0x08f0 : 0x167c, 0);
        value(p, hub, h ? 0x08ef : 0x167b, 0xffffff);
        value(p, hub, h ? 0x08ee : 0x167a, 0);
        value(p, hub, h ? 0x08f1 : 0x167d, 0x4000);
        value(p, hub, h ? 0x08f2 : 0x167e, 0x1bfff);
        value(p, hub, h ? 0x08d8 : 0x15a8, 0x1000001);
        value(p, hub, h ? 0x08d9 : 0x15a9, 0);
        value(p, hub, h ? 0x070f : 0x15cb, 0x123456);
        value(p, hub, h ? 0x0710 : 0x15cc, 0);
        value(p, hub, h ? 0x0709 : 0x15c5, 0x40020);
        value(p, hub, h ? 0x08f3 : 0x167f, 0x1000185f);
        value(p, hub, h ? 0x0700 : 0x15bc, 0x80080805);
        value(p, hub, h ? 0x0701 : 0x15bd, 0x103);
        value(p, hub, h ? 0x0702 : 0x15be, 0x80130009);
        value(p, hub, h ? 0x0718 : 0x15d4, 1);
        value(p, hub, h ? 0x071e : 0x15da, 0x3fe0);
        value(p, hub, h ? 0x0712 : 0x15ce, 0xffffffff);
        value(p, hub, h ? 0x0713 : 0x15cf, 0xf);
        value(p, hub, h ? 0x0708 : 0x15c4, 0x1ffd); // Crash bits explicitly cleared.
        if (!h) value(p, hub, 0x1e1f, 0x8010);
        const Step* enabled = findWrite(p, context(hub), 1);
        CHECK(enabled && enabled->value == 1);
        for (unsigned engine = 0; engine < 18; ++engine) {
            value(p, hub, (h ? 0x0787 : 0x16cf) + engine * 2, 0xffffffff);
            value(p, hub, (h ? 0x0788 : 0x16d0) + engine * 2, 0x1f);
        }
        f.clearLog();
        CHECK(executePlan(p, f.io(), polls, r) == Status::Ok);
        CHECK(p.consumed() && r.stage == Stage::Complete && r.writesMayHaveOccurred);
        CHECK(!r.semaphoreMayBeHeld && r.ackPolls == 1);
        CHECK(r.semaphorePolls == (h ? 1 : 0));
        CHECK(f.flushes == 2 && f.delays == 0);
        CHECK(f.registers[req(hub)] == 0x00f80001);
        CHECK(f.registers[context(hub)] == 1);
        for (unsigned index = 1; index < 16; ++index)
            CHECK(f.registers[context(hub) + index * 4] == 0);
        if (h) CHECK(f.registers[priv()] == 0x02000040);
        const size_t eventCount = f.events.size();
        CHECK(executePlan(p, f.io(), polls, r) == Status::PlanConsumed);
        CHECK(f.events.size() == eventCount && !r.writesMayHaveOccurred);
    }
}

void invalidConfig() {
    const Config good = config();
    for (unsigned defect = 0; defect < 22; ++defect) {
        Config c = good;
        switch (defect) {
        case 0: c.tablePhysicalAddress++; break;
        case 1: c.tableBytes = 0; break;
        case 2: c.tableBytes--; break;
        case 3: c.tableBytes = 4096; break;
        case 4: c.tablePhysicalAddress = c.memory.vramPageBaseOffset - 4096; break;
        case 5: c.tablePhysicalAddress = c.memory.vramPageBaseOffset + c.memory.vramBytes; break;
        case 6: c.apertureStart++; break;
        case 7: c.apertureBytes = 0; break;
        case 8: c.apertureBytes++; break;
        case 9: c.apertureBytes = (4ULL << 30) + 4096; break;
        case 10: c.apertureStart = 1ULL << 47; break;
        case 11: c.apertureStart = c.memory.vramStart; break;
        case 12: c.scratchPhysicalAddress++; break;
        case 13: c.scratchPhysicalAddress = c.tablePhysicalAddress; break;
        case 14: c.scratchPhysicalAddress = c.memory.vramPageBaseOffset - 4096; break;
        case 15: c.scratchPhysicalAddress = c.memory.vramPageBaseOffset + c.memory.vramBytes; break;
        case 16: c.dummyDmaAddress++; break;
        case 17: c.dummyDmaAddress = 1ULL << 44; break;
        case 18: c.memory.mcWindowEndInclusive = c.memory.vramStart; break;
        case 19: c.memory.vramBytes = UINT64_MAX; break;
        case 20: c.tablePhysicalAddress = UINT64_MAX - 4095; break;
        case 21: c.memory.mcWindowStart = 0; break;
        }
        Fake f; Plan p; Result r;
        CHECK(prepareEnable(layout(), barBytes, Hub::Mm, c, f.io(), p, r)
              == Status::InvalidArgument);
        CHECK(!p.valid() && p.stepCount() == 0);
        CHECK(f.events.empty() && !r.writesMayHaveOccurred);
    }
    // At the limits: one page and4GiB both have complete tables. Top valid
    // system DMA page stays in44bits; root pointer can carry SNOOPED.
    for (unsigned large = 0; large < 2; ++large) {
        Config c = good;
        c.apertureBytes = large ? 4ULL << 30 : 4096;
        c.tableBytes = large ? 8ULL << 20 : 4096;
        c.dummyDmaAddress = (1ULL << 44) - 4096;
        c.tableSnooped = true;
        Fake f; Plan p; Result r;
        CHECK(prepareEnable(layout(), barBytes, Hub::Mm, c, f.io(), p, r) == Status::Ok);
        value(p, Hub::Mm, 0x07ab, 0x00200005);
    }
}

void preflightFailures() {
    Config c = config();
    for (unsigned h = 0; h < 2; ++h) {
        const Hub hub = static_cast<Hub>(h);
        for (unsigned which = 1; which <= 6; ++which) {
            for (unsigned allOnes = 0; allOnes < 2; ++allOnes) {
                Fake f; Plan p; Result r;
                if (allOnes) f.allOnesRead = which; else f.failRead = which;
                CHECK(prepareEnable(layout(), barBytes, hub, c, f.io(), p, r)
                      == (allOnes ? Status::DeviceUnavailable : Status::ReadFailed));
                CHECK(!p.valid() && p.stepCount() == 0 && f.writes == 0);
                CHECK(!r.writesMayHaveOccurred && r.stage == Stage::Preflight);
            }
        }
        Fake f; Plan p; Result r;
        CHECK(prepareEnable(layout(), 4, hub, c, f.io(), p, r) == Status::OutsideBar);
        CHECK(f.events.empty());
        Navi31IpLayout bad = layout(); bad.blocks[h].major++;
        CHECK(prepareEnable(bad, barBytes, hub, c, f.io(), p, r) == Status::UnsupportedLayout);
        CHECK(f.events.empty());
        bad = layout(); bad.blocks[h].basesDwords[0] = UINT32_MAX;
        CHECK(prepareFlush(bad, barBytes, hub, f.io(), p, r) == Status::OutsideBar);
        CHECK(f.events.empty());
    }
    Fake f; Plan p; Result r; Io incomplete = f.io(); incomplete.flushHdp = nullptr;
    CHECK(prepareEnable(layout(), barBytes, Hub::Mm, c, incomplete, p, r)
          == Status::InvalidArgument);
    CHECK(prepareDisable(layout(), barBytes, static_cast<Hub>(9), f.io(), p, r)
          == Status::InvalidArgument);
    CHECK(executePlan(p, f.io(), polls, r) == Status::InvalidArgument);
    CHECK(f.events.empty());
}

void flushOrderAndTimeouts() {
    for (unsigned h = 0; h < 2; ++h) {
        Hub hub = static_cast<Hub>(h);
        Fake f; Plan p; Result r;
        CHECK(prepareFlush(layout(), barBytes, hub, f.io(), p, r) == Status::Ok);
        CHECK(p.stepCount() == 4 && f.writes == 0);
        f.clearLog(); f.semDelay = f.ackDelay = 1;
        CHECK(executePlan(p, f.io(), polls, r) == Status::Ok);
        CHECK(f.events[0].kind == 'w' && f.events[0].value == 0xffffffff);
        CHECK(f.events[0].address == byte(hub, h ? 0x07a9 : 0x16f1));
        CHECK(f.events[1].kind == 'w' && f.events[1].value == 0x1f);
        CHECK(f.events[1].address == byte(hub, h ? 0x07aa : 0x16f2));
        CHECK(f.events[2].kind == 'h');
        size_t index = 3;
        if (h) {
            CHECK(f.events[index].kind == 'r' && f.events[index++].address == sem(hub));
            CHECK(f.events[index++].kind == 'd');
            CHECK(f.events[index].kind == 'r' && f.events[index++].address == sem(hub));
        }
        CHECK(f.events[index].kind == 'w' && f.events[index].address == req(hub));
        CHECK(f.events[index++].value == 0x00f80001);
        CHECK(f.events[index].kind == 'r' && f.events[index++].address == ack(hub));
        CHECK(f.events[index++].kind == 'd');
        CHECK(f.events[index].kind == 'r' && f.events[index++].address == ack(hub));
        if (h) {
            CHECK(f.events[index].kind == 'w' && f.events[index].address == sem(hub));
            CHECK(f.events[index++].value == 0);
            CHECK(f.events[index].kind == 'w' && f.events[index++].address == priv());
            CHECK(f.events[index].kind == 'r' && f.events[index++].address == priv());
        }
        CHECK(index == f.events.size());

        Fake timeout; Plan t;
        CHECK(prepareFlush(layout(), barBytes, hub, timeout.io(), t, r) == Status::Ok);
        timeout.clearLog(); timeout.noAck = true;
        CHECK(executePlan(t, timeout.io(), polls, r) == Status::InvalidateTimeout);
        CHECK(r.stage == Stage::InvalidateAck && r.ackPolls == 3);
        CHECK(timeout.delays == 2 && !r.semaphoreMayBeHeld);
        if (h) CHECK(timeout.events.back().kind == 'w' && timeout.events.back().address == sem(hub));
    }
    Fake f; Plan p; Result r;
    CHECK(prepareFlush(layout(), barBytes, Hub::Mm, f.io(), p, r) == Status::Ok);
    f.clearLog(); f.noSemaphore = true;
    CHECK(executePlan(p, f.io(), polls, r) == Status::SemaphoreTimeout);
    CHECK(r.stage == Stage::SemaphoreAcquire && r.semaphorePolls == 3 && r.ackPolls == 0);
    CHECK(f.writes == 2 && f.delays == 2 && !r.semaphoreMayBeHeld);
}

void executionFailures() {
    Config c = config();
    for (unsigned h = 0; h < 2; ++h) {
        const Hub hub = static_cast<Hub>(h);
        Fake base; Plan bp; Result r;
        CHECK(prepareEnable(layout(), barBytes, hub, c, base.io(), bp, r) == Status::Ok);
        base.clearLog();
        CHECK(executePlan(bp, base.io(), polls, r) == Status::Ok);
        // Inject failure at every write attempt, including request/release/
        // private invalidate. Some failures also attempt semaphore cleanup.
        for (unsigned at = 1; at <= base.writes; ++at) {
            Fake f; Plan p;
            CHECK(prepareEnable(layout(), barBytes, hub, c, f.io(), p, r) == Status::Ok);
            f.clearLog(); f.failWrite = at;
            CHECK(executePlan(p, f.io(), polls, r) == Status::WriteFailed);
            CHECK(r.writesMayHaveOccurred && p.consumed());
            CHECK(r.writeAttempts == f.writes && f.writes >= at && f.writes <= at + 1);
        }
        for (unsigned at = 1; at <= base.reads; ++at) {
            Fake f; Plan p;
            CHECK(prepareEnable(layout(), barBytes, hub, c, f.io(), p, r) == Status::Ok);
            f.clearLog(); f.failRead = at;
            CHECK(executePlan(p, f.io(), polls, r) == Status::ReadFailed);
            CHECK(r.writesMayHaveOccurred && r.readAttempts == at);
            if (h && at == 1) CHECK(r.semaphoreMayBeHeld);
        }
        for (unsigned at = 1; at <= 2; ++at) {
            Fake f; Plan p;
            CHECK(prepareEnable(layout(), barBytes, hub, c, f.io(), p, r) == Status::Ok);
            f.clearLog(); f.failFlush = at;
            CHECK(executePlan(p, f.io(), polls, r) == Status::HdpFlushFailed);
            CHECK(r.stage == Stage::HdpVisibility && r.writesMayHaveOccurred);
            CHECK(f.flushes == at && r.ackPolls == 0);
        }
    }
    Fake f; Plan p; Result r;
    CHECK(prepareEnable(layout(), barBytes, Hub::Gfx, c, f.io(), p, r) == Status::Ok);
    f.clearLog(); f.wrongPosting = true;
    CHECK(executePlan(p, f.io(), polls, r) == Status::ReadbackMismatch);
    CHECK(r.stage == Stage::PostingRead);
    // An all-ones semaphore read must not be mistaken for successful acquire.
    Fake gone; Plan gp;
    CHECK(prepareFlush(layout(), barBytes, Hub::Mm, gone.io(), gp, r) == Status::Ok);
    gone.clearLog(); gone.allOnesRead = 1;
    CHECK(executePlan(gp, gone.io(), polls, r) == Status::DeviceUnavailable);
    CHECK(r.semaphoreMayBeHeld && gone.writes == 2);
    // Known-held semaphore is released after failed request or ACK.
    Fake badAck; Plan ap;
    CHECK(prepareFlush(layout(), barBytes, Hub::Mm, badAck.io(), ap, r) == Status::Ok);
    badAck.clearLog(); badAck.failRead = 2;
    CHECK(executePlan(ap, badAck.io(), polls, r) == Status::ReadFailed);
    CHECK(!r.semaphoreMayBeHeld && badAck.events.back().address == sem(Hub::Mm));
    // A failed release leaves lock ownership uncertain, including when it is
    // secondary to a timeout. The original failure stage is preserved.
    Fake badRelease; Plan rp;
    CHECK(prepareFlush(layout(), barBytes, Hub::Mm, badRelease.io(), rp, r) == Status::Ok);
    badRelease.clearLog(); badRelease.failWrite = 4;
    CHECK(executePlan(rp, badRelease.io(), polls, r) == Status::WriteFailed);
    CHECK(r.semaphoreMayBeHeld && r.stage == Stage::SemaphoreRelease);
    Fake both; Plan bothPlan;
    CHECK(prepareFlush(layout(), barBytes, Hub::Mm, both.io(), bothPlan, r) == Status::Ok);
    both.clearLog(); both.failWrite = 4; both.noAck = true;
    CHECK(executePlan(bothPlan, both.io(), polls, r) == Status::InvalidateTimeout);
    CHECK(r.semaphoreMayBeHeld && r.stage == Stage::InvalidateAck);
}

void disableAndOptions() {
    for (unsigned h = 0; h < 2; ++h) {
        const Hub hub = static_cast<Hub>(h);
        Fake f; Plan p; Result r;
        CHECK(prepareDisable(layout(), barBytes, hub, f.io(), p, r) == Status::Ok);
        CHECK(p.stepCount() == 42);
        value(p, hub, h ? 0x08f3 : 0x167f, 0x10000006);
        value(p, hub, h ? 0x0700 : 0x15bc, 0x80000004);
        value(p, hub, h ? 0x0702 : 0x15be, 0);
        f.clearLog();
        const PollOptions bad[] = {{0,1},{100001,1},{1,0},{1,11}};
        for (const auto& option : bad) {
            CHECK(executePlan(p, f.io(), option, r) == Status::InvalidArgument);
            CHECK(!p.consumed() && !r.writesMayHaveOccurred && f.events.empty());
        }
        CHECK(executePlan(p, f.io(), polls, r) == Status::Ok);
        CHECK(f.events[2].kind == 'h');
        CHECK(f.events.back().kind == 'r' && f.events.back().value == 0);
        for (unsigned index = 0; index < 16; ++index)
            CHECK(f.registers[context(hub) + index * 4] == 0);
        // Independent shutdown evidence: all contexts, both L1 bits, and L2
        // enable must be read after every disabling write has completed.
        const size_t firstRead = p.stepCount() - 19;
        for (unsigned index = 0; index < 19; ++index) {
            const Step* s = p.step(firstRead + index);
            const uint32_t address = index < 16 ? context(hub) + index * 4 :
                byte(hub, index == 16 ? (h ? 0x08f3 : 0x167f) :
                          index == 17 ? (h ? 0x0700 : 0x15bc) : (h ? 0x0702 : 0x15be));
            CHECK(s && s->kind == StepKind::PostingRead && s->byteOffset == address);
            CHECK(s && s->value == 0);
            CHECK(s && s->readMask == (index == 16 ? 0x41U : index == 17 ? 1U : UINT32_MAX));
            const size_t eventIndex = f.events.size() - 19 + index;
            CHECK(f.events[eventIndex].kind == 'r' && f.events[eventIndex].address == address);
        }
    }
}

void disableReadbackFailures() {
    for (unsigned h = 0; h < 2; ++h) {
        const Hub hub = static_cast<Hub>(h);
        // Both individual L1 shutdown bits are checked. Every context is exact
        // zero; even an inherited non-enable flag prevents retirement.
        for (unsigned index = 0; index < 20; ++index) {
            Fake f; Plan p; Result r;
            CHECK(prepareDisable(layout(), barBytes, hub, f.io(), p, r) == Status::Ok);
            f.clearLog();
            const uint32_t address = index < 16 ? context(hub) + index * 4 :
                byte(hub, index < 18 ? (h ? 0x08f3 : 0x167f) :
                          index == 18 ? (h ? 0x0700 : 0x15bc) : (h ? 0x0702 : 0x15be));
            f.xorReadAddress = address;
            f.xorReadValue = index < 16 ? 0x80000000U : index == 17 ? 0x40U : 1U;
            CHECK(executePlan(p, f.io(), polls, r) == Status::ReadbackMismatch);
            CHECK(r.stage == Stage::PostingRead && r.lastOffset == address);
            CHECK(r.writesMayHaveOccurred && !r.semaphoreMayBeHeld && p.consumed());
            CHECK(f.events.back().kind == 'r' && f.events.back().address == address);
        }
        // Transport errors or device removal at any retirement read also fail.
        for (unsigned index = 0; index < 19; ++index) {
            for (unsigned unavailable = 0; unavailable < 2; ++unavailable) {
                Fake f; Plan p; Result r;
                CHECK(prepareDisable(layout(), barBytes, hub, f.io(), p, r) == Status::Ok);
                f.clearLog();
                const unsigned readNumber = (h ? 3 : 1) + index + 1;
                if (unavailable) f.allOnesRead = readNumber; else f.failRead = readNumber;
                CHECK(executePlan(p, f.io(), polls, r) ==
                      (unavailable ? Status::DeviceUnavailable : Status::ReadFailed));
                CHECK(r.stage == Stage::PostingRead && r.readAttempts == readNumber);
                CHECK(r.writesMayHaveOccurred && !r.semaphoreMayBeHeld && p.consumed());
            }
        }
        // Unrelated L1/L2 bits may change without weakening the shutdown proof.
        for (unsigned l2 = 0; l2 < 2; ++l2) {
            Fake f; Plan p; Result r;
            CHECK(prepareDisable(layout(), barBytes, hub, f.io(), p, r) == Status::Ok);
            f.clearLog();
            f.xorReadAddress = byte(hub, l2 ? (h ? 0x0700 : 0x15bc) : (h ? 0x08f3 : 0x167f));
            f.xorReadValue = 0x100;
            CHECK(executePlan(p, f.io(), polls, r) == Status::Ok);
        }
    }
}
} // namespace
int main() {
    goldenEnable(); invalidConfig(); preflightFailures(); flushOrderAndTimeouts();
    executionFailures(); disableAndOptions(); disableReadbackFailures();
    std::cout << checks << " GART assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
