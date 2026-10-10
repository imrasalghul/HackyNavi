// SPDX-License-Identifier: MIT
// Offline callback tests: no PCI, MMIO, reset, or real delays are performed.
#include "hackynavi/mode1_reset.hpp"
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::mode1;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; } } while (0)
constexpr uint32_t kParam = (0x16000 + 0x275) * 4;
constexpr uint32_t kMessage = (0x16000 + 0x28b) * 4;
constexpr uint32_t kResponse = (0x16000 + 0x276) * 4;
constexpr uint32_t kMemory = (0x19000 + 0xc3) * 4;
constexpr uint32_t kScratch = (0x15c00 + 3) * 4;
const Options options = {3,2,0x15c00};
Navi31IpLayout layout() {
    Navi31IpLayout l = {}; l.valid = true;
    const uint16_t ids[6] = {11,34,255,1,42,43};
    const uint8_t majors[6] = {11,3,13,13,6,6};
    for (unsigned i = 0; i < 6; ++i) l.blocks[i] = {ids[i],0,0,majors[i],0,0,2,{0x1260,0xa000}};
    l.blocks[1].basesDwords[0] = 0x1a000;
    l.blocks[2].basesDwords[0] = l.blocks[3].basesDwords[0] = 0x16000;
    return l;
}
HdpLayout hdp() {
    return {true,(0x19000+0x12d)*4,(0x19000+0x12e)*4,kMemory,0x7f000,0x7f004,0x80000};
}
struct Event { char kind; uint32_t offset, value; };
struct Fake {
    std::map<uint32_t,uint32_t> mmio, pci, saved;
    std::vector<Event> events;
    unsigned mmioReads = 0, mmioWrites = 0, pciReads = 0, pciWrites = 0;
    unsigned saves = 0, restores = 0, delays = 0, memoryReads = 0;
    unsigned failMmioRead = 0, failMmioWrite = 0, failPciRead = 0, failPciWrite = 0;
    unsigned allOnesMmioRead = 0, allOnesPciRead = 0, unavailableMemoryReads = 0;
    uint32_t restoreCorruptOffset = UINT32_MAX, restoreCorruptMask = 0;
    bool failSave = false, failRestore = false, ignoreCommandWrite = false;
    bool triggerSeen = false, blackoutDone = false, forbiddenAccess = false;
    Fake() {
        pci[0] = 0x744c1002; pci[4] = 0x00100007; pci[8] = 0x030000c8;
        pci[0xc] = 0x00800010; // normal endpoint, multifunction
        pci[0x10] = 0x8000000c; pci[0x14] = 8;
        pci[0x18] = 0x9000000c; pci[0x1c] = 0;
        pci[0x20] = 0x301; pci[0x24] = 0xa0000000;
        mmio[kScratch] = 0x104002; mmio[kMemory] = 24576;
    }
    Io io() { return {this,read,write,readPci,writePci,save,restore,delayMs,delayUs}; }
    void access(bool responseWrite = false) {
        if (triggerSeen && !blackoutDone && !responseWrite) forbiddenAccess = true;
    }
    static bool read(void* p,uint32_t a,uint32_t& v) {
        auto& f = *static_cast<Fake*>(p); f.access(); ++f.mmioReads;
        v = f.mmio[a];
        if (a == kMemory && ++f.memoryReads <= f.unavailableMemoryReads) v = UINT32_MAX;
        if (f.mmioReads == f.allOnesMmioRead) v = UINT32_MAX;
        f.events.push_back({'r',a,v}); return f.mmioReads != f.failMmioRead;
    }
    static bool write(void* p,uint32_t a,uint32_t v) {
        auto& f = *static_cast<Fake*>(p); f.access(a == kResponse); ++f.mmioWrites;
        f.mmio[a] = v; f.events.push_back({'w',a,v});
        if (a == kMessage) f.triggerSeen = true;
        return f.mmioWrites != f.failMmioWrite; // failure may still land
    }
    static bool readPci(void* p,uint32_t a,uint32_t& v) {
        auto& f = *static_cast<Fake*>(p); f.access(); ++f.pciReads;
        v = f.pci[a]; if (f.pciReads == f.allOnesPciRead) v = UINT32_MAX;
        f.events.push_back({'p',a,v}); return f.pciReads != f.failPciRead;
    }
    static bool writePci(void* p,uint32_t a,uint16_t v) {
        auto& f = *static_cast<Fake*>(p); f.access(); ++f.pciWrites;
        f.events.push_back({'c',a,v});
        if (!f.ignoreCommandWrite) f.pci[a] = (f.pci[a] & 0xffff0000) | v;
        return f.pciWrites != f.failPciWrite;
    }
    static bool save(void* p) {
        auto& f = *static_cast<Fake*>(p); f.access(); ++f.saves;
        f.saved = f.pci; f.events.push_back({'s',0,0}); return !f.failSave;
    }
    static bool restore(void* p) {
        auto& f = *static_cast<Fake*>(p); f.access(); ++f.restores;
        f.pci = f.saved; // actual snapshot was captured with BME already off
        if (f.restoreCorruptOffset != UINT32_MAX) f.pci[f.restoreCorruptOffset] ^= f.restoreCorruptMask;
        f.events.push_back({'t',0,0}); return !f.failRestore;
    }
    static void delayMs(void* p,uint32_t ms) {
        auto& f = *static_cast<Fake*>(p); ++f.delays;
        f.events.push_back({'m',0,ms}); f.blackoutDone = true;
    }
    static void delayUs(void* p,uint32_t us) {
        auto& f = *static_cast<Fake*>(p); f.access(); f.events.push_back({'u',0,us});
    }
};
void invariants(const Fake& f, const Result& r) {
    CHECK(!f.forbiddenAccess);
    CHECK(r.mmioReads == f.mmioReads && r.mmioWrites == f.mmioWrites);
    CHECK(r.pciReads == f.pciReads && r.pciWrites == f.pciWrites);
    CHECK(r.resetMayHaveOccurred == f.triggerSeen);
    CHECK(r.blackoutCompleted == f.blackoutDone);
    CHECK(f.delays == (f.triggerSeen ? 1U : 0U));
    for (const auto& e : f.events) {
        if (e.kind == 'm') CHECK(e.value == 500);
        if (e.kind == 'c') CHECK(e.offset == 4 && !(e.value & 4));
        if (e.kind == 'r') CHECK(e.offset == kMemory || e.offset == kScratch);
    }
}
void golden() {
    for (unsigned scratch = 0; scratch < 2; ++scratch)
        for (unsigned bme = 0; bme < 2; ++bme) {
            Fake f; Result r; auto o = options;
            if (!scratch) o.biosScratchBaseDwords = 0;
            if (!bme) f.pci[4] &= ~4U;
            CHECK(reset(layout(),hdp(),f.io(),o,r) == Status::Ok);
            CHECK(r.stage == Stage::Complete && r.memorySizeMiB == 24576 && r.memoryPolls == 1);
            CHECK(r.pciStateSaved && r.pciStateRestored && r.busMasterDisableVerified);
            CHECK(r.restoredCommand == 3 && r.originalCommand == (bme ? 7 : 3));
            CHECK(f.pciWrites == bme && (f.pci[4] & 0xffff0000) == 0x00100000);
            CHECK(f.mmioWrites == (scratch ? 5U : 3U));
            CHECK(f.mmio[kScratch] == 0x104002 && f.saves == 1 && f.restores == 1);
            std::vector<Event> protocol;
            for (const auto& e : f.events)
                if (e.kind != 'p' && e.kind != 'r') protocol.push_back(e);
            size_t i = 0;
            if (scratch) CHECK(protocol[i++].offset == kScratch);
            if (bme) CHECK(protocol[i++].kind == 'c');
            CHECK(protocol[i++].kind == 's');
            CHECK(!(f.saved[4] & 4));
            CHECK(protocol[i].kind == 'w' && protocol[i].offset == kParam && protocol[i++].value == 0);
            CHECK(protocol[i].kind == 'w' && protocol[i].offset == kMessage && protocol[i++].value == 2);
            CHECK(protocol[i].kind == 'w' && protocol[i].offset == kResponse && protocol[i++].value == 0);
            CHECK(protocol[i++].kind == 'm'); CHECK(protocol[i++].kind == 't');
            if (scratch) CHECK(protocol[i++].offset == kScratch);
            CHECK(i == protocol.size());
            invariants(f,r);
        }
    // ENGINE_HUNG is cleared as upstream does, even if set before entry.
    Fake f; Result r; f.mmio[kScratch] |= 0x20000000;
    CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::Ok);
    CHECK(f.mmio[kScratch] == 0x104002);
}
void invalidArguments() {
    const Options bad[] = {{0,1,0},{1000001,1,0},{1,0,0},{1,11,0},{100001,10,0}};
    for (auto o : bad) {
        Fake f; Result r; CHECK(reset(layout(),hdp(),f.io(),o,r) == Status::InvalidArgument);
        CHECK(f.events.empty() && r.stage == Stage::Preflight);
    }
    for (unsigned i = 0; i < 8; ++i) {
        Fake f; auto io = f.io(); Result r;
        if (i == 0) io.read32 = nullptr;
        if (i == 1) io.write32 = nullptr;
        if (i == 2) io.readPci32 = nullptr;
        if (i == 3) io.writePci16 = nullptr;
        if (i == 4) io.savePciState = nullptr;
        if (i == 5) io.restorePciState = nullptr;
        if (i == 6) io.delayMs = nullptr;
        if (i == 7) io.delayUs = nullptr;
        CHECK(reset(layout(),hdp(),io,options,r) == Status::InvalidArgument);
        CHECK(f.events.empty());
    }
    for (unsigned b = 0; b < 6; ++b)
        for (unsigned field = 0; field < 8; ++field) {
            Fake f; auto l = layout(); Result r;
            auto& ip = l.blocks[b];
            if (field == 0) ++ip.hardwareId;
            if (field == 1) ++ip.major;
            if (field == 2) ++ip.minor;
            if (field == 3) ++ip.revision;
            if (field == 4) ++ip.dieId;
            if (field == 5) ++ip.instance;
            if (field == 6) ip.baseCount = 0;
            if (field == 7) ip.baseCount = 9;
            CHECK(reset(l,hdp(),f.io(),options,r) == Status::UnsupportedLayout);
            CHECK(f.events.empty());
        }
    for (unsigned variant = 0; variant < 11; ++variant) {
        Fake f; auto l = layout(); auto h = hdp(); auto o = options; Result r;
        Status wanted = Status::UnsupportedLayout;
        if (variant == 0) l.valid = false;
        if (variant == 1) h.valid = false;
        if (variant == 2) h.barBytes = 0x7ffff;
        if (variant == 3) ++h.memoryFlushRegister;
        if (variant == 4) ++h.registerFlushRegister;
        if (variant == 5) ++h.memorySizeRegister;
        if (variant == 6) ++h.registerRemapRegister;
        if (variant == 7) { l.blocks[3].basesDwords[0] = UINT32_MAX; wanted = Status::OutsideBar; }
        if (variant == 8) { o.biosScratchBaseDwords = UINT32_MAX; wanted = Status::OutsideBar; }
        if (variant == 9) { o.biosScratchBaseDwords = kParam/4-3; wanted = Status::AliasedRegisters; }
        if (variant == 10) { o.biosScratchBaseDwords = 0x7f000/4-3; wanted = Status::AliasedRegisters; }
        CHECK(reset(l,h,f.io(),o,r) == wanted); CHECK(f.events.empty());
    }
}
void pciGuards() {
    const Event bad[] = {{'i',0,0x744d1002},{'c',4,5},{'h',0xc,0x00010000},
        {'b',0x24,0},{'b',0x24,0xa0000001},{'b',0x24,0xa0000004},{'b',0x20,0x100004}};
    for (const auto& v : bad) {
        Fake f; Result r; f.pci[v.offset] = v.value;
        CHECK(reset(layout(),hdp(),f.io(),options,r) ==
              (v.kind == 'i' ? Status::WrongDevice : Status::InvalidPciState));
        CHECK(!r.writesMayHaveOccurred && !f.saves && !f.mmioReads);
    }
    Fake f; Result r; f.ignoreCommandWrite = true;
    CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::BusMasterDisableFailed);
    CHECK(!r.resetMayHaveOccurred && !f.restores && !r.busMasterDisableVerified);
    invariants(f,r);
}
void accessFailures() {
    Fake gold; Result result;
    CHECK(reset(layout(),hdp(),gold.io(),options,result) == Status::Ok);
    for (unsigned i = 1; i <= gold.pciReads; ++i) {
        Fake f; Result r; f.failPciRead = i;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::PciReadFailed);
        CHECK(f.pciReads == i); invariants(f,r);
        if (i <= 11) CHECK(!f.restores && !r.resetMayHaveOccurred);
        else CHECK(!f.memoryReads);
    }
    for (unsigned i = 1; i <= gold.pciReads; ++i) {
        Fake f; Result r; f.allOnesPciRead = i;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::DeviceUnavailable);
        CHECK(f.pciReads == i); invariants(f,r);
    }
    for (unsigned i = 1; i <= gold.mmioReads; ++i) {
        Fake f; Result r; f.failMmioRead = i;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::ReadFailed);
        CHECK(f.mmioReads == i); invariants(f,r);
    }
    for (unsigned i = 1; i <= gold.mmioWrites; ++i) {
        Fake f; Result r; f.failMmioWrite = i;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::WriteFailed);
        CHECK(f.mmioWrites == i);
        if (i <= 4) CHECK(!f.restores);
        if (i == 3 || i == 4) CHECK(r.resetMayHaveOccurred && r.blackoutCompleted);
        if (i <= 2) CHECK(!r.resetMayHaveOccurred);
        invariants(f,r);
    }
    for (unsigned i : {1U,3U}) {
        Fake f; Result r; f.allOnesMmioRead = i;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::DeviceUnavailable);
        invariants(f,r);
    }
    for (unsigned kind = 0; kind < 3; ++kind) {
        Fake f; Result r;
        if (kind == 0) f.failPciWrite = 1;
        if (kind == 1) f.failSave = true;
        if (kind == 2) f.failRestore = true;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == (kind == 0 ? Status::PciWriteFailed :
              kind == 1 ? Status::PciSaveFailed : Status::PciRestoreFailed));
        if (kind < 2) CHECK(!f.triggerSeen && !f.restores);
        else CHECK(!f.memoryReads && !r.pciStateRestored);
        invariants(f,r);
    }
}
void restoreMismatch() {
    const uint32_t offsets[] = {0,4,8,0xc,0x10,0x14,0x18,0x1c,0x20,0x24};
    for (uint32_t a : offsets) {
        Fake f; Result r; f.restoreCorruptOffset = a;
        f.restoreCorruptMask = a == 0xc ? 0x10000 : a == 4 ? 4 : 0x10;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::PciRestoreMismatch);
        CHECK(!f.memoryReads && r.pciStateRestored && r.resetMayHaveOccurred);
        invariants(f,r);
    }
    // Status W1C and cache-line/latency fields need not be byte-identical.
    for (unsigned mode = 0; mode < 2; ++mode) {
        Fake f; Result r; f.restoreCorruptOffset = mode ? 0xc : 4;
        f.restoreCorruptMask = mode ? 0x80 : 0x08000000;
        CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::Ok);
        invariants(f,r);
    }
}
void memoryWait() {
    for (unsigned unavailable = 0; unavailable < 5; ++unavailable) {
        Fake f; Result r; f.unavailableMemoryReads = unavailable;
        CHECK(reset(layout(),hdp(),f.io(),options,r) ==
              (unavailable < 3 ? Status::Ok : Status::MemoryTimeout));
        CHECK(r.memoryPolls == (unavailable < 3 ? unavailable+1 : 3));
        unsigned delays = 0;
        for (const auto& e : f.events) if (e.kind == 'u') { ++delays; CHECK(e.value == 2); }
        CHECK(delays + 1 == r.memoryPolls);
        CHECK((f.mmio[kScratch] & 0x20000000) == (unavailable < 3 ? 0U : 0x20000000U));
        invariants(f,r);
    }
    Fake f; Result r; f.mmio[kMemory] = 0;
    CHECK(reset(layout(),hdp(),f.io(),options,r) == Status::Ok && r.memorySizeMiB == 0);
    invariants(f,r);
}
} // namespace
int main() {
    golden(); invalidArguments(); pciGuards(); accessFailures(); restoreMismatch(); memoryWait();
    std::cout << checks << " assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
