// SPDX-License-Identifier: MIT
// Host-only MMIO model. Tests register protocols, not actual GPU isolation.
#include "hackynavi/vm_context.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace v = hackynavi::vmcontext;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
const v::Poll poll{3,1};
constexpr uint32_t gcBase = 0x2000, mmBase = 0x18000;
uint32_t reg(v::Hub h,uint32_t gc,uint32_t mm) { return ((h == v::Hub::Gfx ? gcBase + gc : mmBase + mm) * 4); }
uint32_t control(v::Hub h,uint8_t id) { return reg(h,0x1688,0x740) + id * 4; }
uint32_t root(v::Hub h,uint8_t id) { return reg(h,0x16f3,0x7ab) + id * 8; }
uint32_t start(v::Hub h,uint8_t id) { return reg(h,0x1713,0x7cb) + id * 8; }
uint32_t end(v::Hub h,uint8_t id) { return reg(h,0x1733,0x7eb) + id * 8; }
uint32_t request(v::Hub h) { return reg(h,0x16bc,0x774); }
uint32_t ack(v::Hub h) { return reg(h,0x16ce,0x786); }
uint32_t semaphore(v::Hub h) { return reg(h,0x16aa,0x762); }
uint32_t range(v::Hub h) { return reg(h,0x16f1,0x7a9); }
uint32_t privateReg() { return reg(v::Hub::Mm,0,0x71b); }
hackynavi::Navi31IpLayout layout() {
    hackynavi::Navi31IpLayout l{}; l.valid = true;
    l.blocks[0] = {11,0,0,11,0,0,2,{gcBase,0xa000}};
    l.blocks[1] = {34,0,0,3,0,0,2,{mmBase,0x2408800}}; return l;
}
v::Config config(uint8_t id = 1) {
    return {id,{0x1000200000ULL,4096,hackynavi::gmc11::MemoryDomain::VramPhysical,false},0x10000,1ULL << 47,97};
}
struct Event { char kind; uint32_t address,value; };
struct Device {
    v::Hub hub; uint8_t vmid; v::Context context; v::Result result{};
    std::map<uint32_t,uint32_t> regs; std::vector<Event> events;
    size_t reads = 0,writes = 0,failRead = 0,failWrite = 0,onesRead = 0,dropWrite = 0;
    size_t changeRead = 0, failPublish = 0;
    uint32_t requestValue = 0,semPolls = 0,ackPolls = 0,delays = 0,flushes = 0;
    bool noSem = false,noAck = false,badPublish = false,writeOnFailure = false,semHeld = false;
    uint32_t foreignAck = 0;
    Device(v::Hub h,uint8_t id = 1) : hub(h),vmid(id) {
        for (uint8_t n = 0; n < 16; ++n) {
            regs[control(h,n)] = n == id ? 0 : (n == 0 ? 1 : 0x00bb0007);
            for (auto base : {root(h,n),start(h,n),end(h,n)}) { regs[base] = uint32_t(n) * 0x10000; regs[base + 4] = n; }
        }
        regs[reg(h,0x167f,0x8f3)] = 0x1859;
        regs[reg(h,0x15bc,0x700)] = 0x80801;
        regs[reg(h,0x15be,0x702)] = 0x80130009;
        regs[reg(h,0x15ce,0x712)] = UINT32_MAX;
        regs[reg(h,0x15cf,0x713)] = 0xf;
        for (auto r : {reg(h,0x15d0,0x714),reg(h,0x15d1,0x715),reg(h,0x15d2,0x716),reg(h,0x15d3,0x717)}) regs[r] = 0;
        regs[range(h)] = regs[range(h) + 4] = 0;
        regs[request(h)] = regs[ack(h)] = regs[semaphore(h)] = 0;
        if (h == v::Hub::Mm) regs[privateReg()] = 0x40;
    }
    v::Io io() { return {this,read,write,delay,publish}; }
    static bool read(void* p,uint32_t address,uint32_t& value) {
        auto& d = *static_cast<Device*>(p); ++d.reads; CHECK(d.regs.count(address) != 0);
        if (d.changeRead == d.reads) d.regs[address] ^= 1;
        value = d.regs[address];
        if (address == semaphore(d.hub)) { ++d.semPolls; value = d.noSem ? 0 : 1; d.semHeld = bool(value); }
        if (address == ack(d.hub)) { ++d.ackPolls; value = d.noAck ? d.foreignAck : (d.requestValue & 0xffff); }
        if (d.onesRead == d.reads) value = UINT32_MAX;
        d.events.push_back({'r',address,value}); return d.failRead != d.reads;
    }
    static bool write(void* p,uint32_t address,uint32_t value) {
        auto& d = *static_cast<Device*>(p); ++d.writes; CHECK(d.regs.count(address) != 0);
        d.events.push_back({'w',address,value});
        const bool failed = d.failWrite == d.writes;
        if ((!failed || d.writeOnFailure) && d.dropWrite != d.writes) {
            d.regs[address] = value;
            if (address == request(d.hub)) {
                d.requestValue = value; CHECK(value == (0x00f80000 | (1U << d.vmid)));
                if (d.hub == v::Hub::Mm) CHECK(d.semHeld);
            }
            if (address == semaphore(d.hub)) { CHECK(value == 0); CHECK(d.semHeld); d.semHeld = false; }
            if (d.hub == v::Hub::Mm && address == privateReg()) d.regs[address] &= ~0x02000000U;
        }
        return !failed;
    }
    static bool publish(void* p) { auto& d = *static_cast<Device*>(p); ++d.flushes; d.events.push_back({'p',0,0}); return !d.badPublish && d.failPublish != d.flushes; }
    static void delay(void* p,uint32_t us) { auto& d = *static_cast<Device*>(p); CHECK(us == 1); ++d.delays; }
    v::Status enable(v::Config c) { return v::enable(layout(),0x200000,hub,c,io(),poll,context,result); }
    v::Status enable() { return enable(config(vmid)); }
    void clearEvents() { reads = writes = flushes = delays = 0; events.clear(); requestValue = 0; }
};
void unrelated(const Device& d,const std::map<uint32_t,uint32_t>& original) {
    for (const auto& entry : original) {
        const uint32_t a = entry.first;
        bool mutableAddress = a == control(d.hub,d.vmid) || a == range(d.hub) || a == range(d.hub) + 4 ||
            a == request(d.hub) || a == semaphore(d.hub) || (d.hub == v::Hub::Mm && a == privateReg());
        for (auto p : {root(d.hub,d.vmid),start(d.hub,d.vmid),end(d.hub,d.vmid)}) mutableAddress |= a == p || a == p + 4;
        if (!mutableAddress) CHECK(d.regs.at(a) == entry.second);
    }
    for (const auto& event : d.events) if (event.kind == 'w') {
        for (uint8_t n = 0; n < 16; ++n) if (n != d.vmid) {
            CHECK(event.address != control(d.hub,n));
            for (auto p : {root(d.hub,n),start(d.hub,n),end(d.hub,n)}) CHECK(event.address != p && event.address != p + 4);
        }
    }
}
size_t eventIndex(const Device& d,char kind,uint32_t address,uint32_t value,bool compareValue = true) {
    for (size_t n = 0; n < d.events.size(); ++n) {
        const auto& e = d.events[n];
        if (e.kind == kind && e.address == address && (!compareValue || e.value == value)) return n;
    }
    return SIZE_MAX;
}
void successful() {
    for (auto hub : {v::Hub::Gfx,v::Hub::Mm}) for (uint8_t id = 1; id <= 7; ++id)
    for (auto domain : {hackynavi::gmc11::MemoryDomain::VramPhysical,hackynavi::gmc11::MemoryDomain::SystemDma})
    for (bool snoop : {false,true}) {
        Device d(hub,id); const auto before = d.regs; auto c = config(id); c.root.domain = domain; c.root.snooped = snoop;
        CHECK(d.enable(c) == v::Status::Ok); CHECK(d.context.phase() == v::Phase::Enabled);
        CHECK(d.context.requiresHardwareRetirement() && d.result.retentionRequired);
        CHECK(d.context.config().epoch == 97 && d.context.config().vmid == id);
        const uint32_t flags = 1 | (snoop ? 4 : 0) | (domain == hackynavi::gmc11::MemoryDomain::SystemDma ? 2 : 0);
        CHECK(d.regs[root(hub,id)] == (uint32_t(c.root.address) | flags));
        CHECK(d.regs[root(hub,id) + 4] == uint32_t(c.root.address >> 32));
        CHECK(d.regs[start(hub,id)] == 16 && d.regs[start(hub,id) + 4] == 0);
        CHECK(d.regs[end(hub,id)] == UINT32_MAX && d.regs[end(hub,id) + 4] == 7);
        CHECK(d.regs[control(hub,id)] == 0x2aaa07);
        CHECK((d.regs[control(hub,id)] & 0x555580) == 0); // defaults+retry absent
        const auto publish = eventIndex(d,'p',0,0);
        const auto enabled = eventIndex(d,'w',control(hub,id),0x2aaa07);
        const auto invalidated = eventIndex(d,'w',request(hub),0,false);
        const auto acknowledged = eventIndex(d,'r',ack(hub),0,false);
        CHECK(publish < enabled && enabled < invalidated && invalidated < acknowledged);
        unrelated(d,before); d.clearEvents();
        CHECK(v::flush(d.context,d.io(),poll,d.result) == v::Status::Ok);
        CHECK(d.result.retentionRequired); unrelated(d,before); d.clearEvents();
        CHECK(v::detach(d.context,d.io(),poll,d.result) == v::Status::Ok);
        CHECK(d.context.phase() == v::Phase::Detached && d.context.requiresHardwareRetirement());
        CHECK(d.regs[control(hub,id)] == 0 && d.result.retentionRequired);
        CHECK(eventIndex(d,'w',request(hub),0,false) < eventIndex(d,'r',ack(hub),0,false));
        CHECK(eventIndex(d,'r',ack(hub),0,false) < eventIndex(d,'w',control(hub,id),0));
        unrelated(d,before);
    }
}
void arguments() {
    for (unsigned n = 0; n < 256; ++n) {
        Device d(v::Hub::Gfx); auto c = config(); c.vmid = uint8_t(n);
        if (n >= 1 && n <= 7) continue;
        CHECK(d.enable(c) == v::Status::InvalidArgument); CHECK(d.reads == 0 && d.writes == 0);
    }
    for (uint32_t n = 1; n < 13; ++n) {
        Device d(v::Hub::Gfx); auto c = config();
        switch (n) {
        case 1:++c.root.address;break;
        case 2:c.root.address = 1ULL << 48;break; case 3:c.root.bytes = 8192;break;
        case 4:c.root.domain = static_cast<hackynavi::gmc11::MemoryDomain>(9);break;
        case 5:c.root.domain = hackynavi::gmc11::MemoryDomain::SystemDma;c.root.address = 1ULL << 44;break;
        case 6:c.vaBegin = 0;break; case 7:++c.vaBegin;break;case 8:--c.vaEnd;break;
        case 9:c.vaEnd = (1ULL << 47) + 4096;break;case 10:c.vaEnd = c.vaBegin;break;
        case 11:c.vaBegin = UINT64_MAX - 4095;break;case 12:c.epoch = 0;break;
        }
        CHECK(d.enable(c) == v::Status::InvalidArgument); CHECK(d.reads == 0 && d.writes == 0);
    }
    for (auto domain : {hackynavi::gmc11::MemoryDomain::SystemDma,hackynavi::gmc11::MemoryDomain::VramPhysical}) {
        Device d(v::Hub::Gfx); auto c = config(); c.root.domain = domain;
        c.root.address = (1ULL << (domain == hackynavi::gmc11::MemoryDomain::SystemDma ? 44 : 48)) - 4096;
        CHECK(d.enable(c) == v::Status::Ok);
        Device zero(v::Hub::Gfx); c.root.address = 0;
        CHECK(zero.enable(c) == v::Status::Ok);
        CHECK(zero.regs[root(v::Hub::Gfx,1)] == (domain == hackynavi::gmc11::MemoryDomain::SystemDma ? 3U : 1U));
    }
    Device d(v::Hub::Gfx);
    CHECK(v::enable(layout(),4,d.hub,config(),d.io(),poll,d.context,d.result) == v::Status::OutsideBar);
    CHECK(d.reads == 0 && d.writes == 0);
    CHECK(v::enable(layout(),0x200000,d.hub,config(),d.io(),{100000,10},d.context,d.result) == v::Status::InvalidArgument);
    auto l = layout(); l.blocks[0].revision = 1;
    CHECK(v::enable(l,0x200000,d.hub,config(),d.io(),poll,d.context,d.result) == v::Status::UnsupportedLayout);
}
void guardFailures() {
    for (auto hub : {v::Hub::Gfx,v::Hub::Mm}) {
        Device inherited(hub); inherited.regs[control(hub,1)] = 7;
        CHECK(inherited.enable() == v::Status::InheritedContext); CHECK(!inherited.writes && !inherited.context.requiresHardwareRetirement());
        for (uint32_t address : {reg(hub,0x167f,0x8f3),reg(hub,0x15bc,0x700),reg(hub,0x15be,0x702),
            reg(hub,0x15ce,0x712),reg(hub,0x15cf,0x713),reg(hub,0x15d0,0x714),reg(hub,0x15d1,0x715),
            reg(hub,0x15d2,0x716),reg(hub,0x15d3,0x717)}) {
            Device d(hub); d.regs[address] ^= 1;
            CHECK(d.enable() == v::Status::GlobalState); CHECK(!d.writes && !d.context.requiresHardwareRetirement());
        }
        Device identity(hub); identity.regs[reg(hub,0x15bc,0x700)] ^= 0x180000;
        CHECK(identity.enable() == v::Status::GlobalState); CHECK(!identity.writes);
    }
}
void failuresAtEveryCall() {
    for (auto hub : {v::Hub::Gfx,v::Hub::Mm}) {
        Device baseline(hub); CHECK(baseline.enable() == v::Status::Ok);
        for (size_t n = 1; n <= baseline.reads; ++n) {
            Device d(hub); d.failRead = n;
            CHECK(d.enable() == v::Status::ReadFailed);
            CHECK(d.context.requiresHardwareRetirement() == (d.writes != 0));
            if (d.writes) CHECK(d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
        }
        size_t readIndex = 0;
        for (const auto& event : baseline.events) if (event.kind == 'r') {
            ++readIndex;
            if (event.value == UINT32_MAX) continue; // legal identity/end/range low fields
            Device d(hub); d.onesRead = readIndex;
            CHECK(d.enable() == v::Status::DeviceUnavailable);
            CHECK(d.context.requiresHardwareRetirement() == (d.writes != 0));
        }
        for (size_t n = 1; n <= baseline.writes; ++n) for (bool accepted : {false,true}) {
            Device d(hub); d.failWrite = n; d.writeOnFailure = accepted;
            CHECK(d.enable() == v::Status::WriteFailed);
            CHECK(d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
            CHECK(d.context.config().root.address == config().root.address);
        }
        for (size_t n = 1; n <= 9; ++n) {
            Device d(hub); d.dropWrite = n;
            // Zero-valued high/start fields may already agree; every other
            // dropped root/range/control write must fail an actual readback.
            const auto status = d.enable();
            if (status == v::Status::Ok) CHECK(d.regs[root(hub,1)] == uint32_t(config().root.address | 1));
            else CHECK(d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
        }
        Device noAck(hub); noAck.noAck = true; noAck.foreignAck = 1;
        CHECK(noAck.enable() == v::Status::InvalidateTimeout); CHECK(noAck.ackPolls == poll.iterations);
        CHECK(noAck.result.retentionRequired && !noAck.semHeld);
        CHECK(noAck.regs[control(hub,1)] == 0x2aaa07); // no pretend rollback
        Device publish(hub); publish.badPublish = true;
        CHECK(publish.enable() == v::Status::PublishFailed); CHECK(publish.result.retentionRequired);
        Device changed(hub); CHECK(changed.enable() == v::Status::Ok); changed.regs[root(hub,1)] ^= 4096;
        changed.clearEvents(); CHECK(v::flush(changed.context,changed.io(),poll,changed.result) == v::Status::ReadbackMismatch);
        CHECK(changed.writes == 0 && changed.result.retentionRequired && changed.context.phase() == v::Phase::Faulted);
    }
    Device sem(v::Hub::Mm); sem.noSem = true;
    CHECK(sem.enable() == v::Status::SemaphoreTimeout); CHECK(sem.semPolls == poll.iterations);
    CHECK(!sem.result.semaphoreMayBeHeld && sem.result.retentionRequired && !sem.requestValue);
    Device baseline(v::Hub::Mm); CHECK(baseline.enable() == v::Status::Ok); size_t readIndex = 0;
    for (const auto& event : baseline.events) if (event.kind == 'r') {
        ++readIndex;
        if (event.address != semaphore(v::Hub::Mm)) continue;
        Device uncertain(v::Hub::Mm); uncertain.failRead = readIndex;
        CHECK(uncertain.enable() == v::Status::ReadFailed && uncertain.result.semaphoreMayBeHeld);
        CHECK(eventIndex(uncertain,'w',semaphore(v::Hub::Mm),0) == SIZE_MAX);
        break;
    }
}
void laterFailures() {
    for (auto hub : {v::Hub::Gfx,v::Hub::Mm}) for (bool detach : {false,true}) {
        Device baseline(hub); CHECK(baseline.enable() == v::Status::Ok); baseline.clearEvents();
        CHECK((detach ? v::detach(baseline.context,baseline.io(),poll,baseline.result) :
                        v::flush(baseline.context,baseline.io(),poll,baseline.result)) == v::Status::Ok);
        for (size_t n = 1; n <= baseline.reads; ++n) {
            Device d(hub); CHECK(d.enable() == v::Status::Ok); d.clearEvents(); d.failRead = n;
            CHECK((detach ? v::detach(d.context,d.io(),poll,d.result) : v::flush(d.context,d.io(),poll,d.result)) == v::Status::ReadFailed);
            CHECK(d.result.retentionRequired && d.context.phase() == v::Phase::Faulted);
        }
        for (size_t n = 1; n <= baseline.writes; ++n) for (bool accepted : {false,true}) {
            Device d(hub); CHECK(d.enable() == v::Status::Ok); d.clearEvents(); d.failWrite = n; d.writeOnFailure = accepted;
            CHECK((detach ? v::detach(d.context,d.io(),poll,d.result) : v::flush(d.context,d.io(),poll,d.result)) == v::Status::WriteFailed);
            CHECK(d.result.retentionRequired && d.context.phase() == v::Phase::Faulted);
        }
    }
}
void preparedDetached(Device& d) {
    CHECK(d.enable() == v::Status::Ok);
    CHECK(v::detach(d.context,d.io(),poll,d.result) == v::Status::Ok);
    CHECK(d.context.phase() == v::Phase::Detached && d.context.requiresHardwareRetirement());
    d.clearEvents();
}
void reactivation() {
    for (auto hub : {v::Hub::Gfx,v::Hub::Mm}) for (uint8_t id = 1; id <= 7; ++id)
    for (auto domain : {hackynavi::gmc11::MemoryDomain::VramPhysical,hackynavi::gmc11::MemoryDomain::SystemDma})
    for (bool snooped : {false,true}) {
        Device d(hub,id); auto c = config(id); c.root.domain = domain; c.root.snooped = snooped;
        CHECK(d.enable(c) == v::Status::Ok);
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            CHECK(v::detach(d.context,d.io(),poll,d.result) == v::Status::Ok);
            const auto before = d.regs; d.clearEvents();
            CHECK(v::reactivate(d.context,d.io(),poll,d.result) == v::Status::Ok);
            CHECK(d.context.phase() == v::Phase::Enabled && d.result.retentionRequired);
            CHECK(d.context.requiresHardwareRetirement() && d.result.stage == v::Stage::Complete);
            const auto& after = d.context.config();
            CHECK(after.vmid == c.vmid && after.epoch == c.epoch && after.vaBegin == c.vaBegin && after.vaEnd == c.vaEnd);
            CHECK(after.root.address == c.root.address && after.root.bytes == c.root.bytes &&
                after.root.domain == c.root.domain && after.root.snooped == c.root.snooped);
            CHECK(d.flushes == 2);
            CHECK(eventIndex(d,'p',0,0) < eventIndex(d,'w',control(hub,id),0x2aaa07));
            CHECK(eventIndex(d,'w',control(hub,id),0x2aaa07) < eventIndex(d,'w',request(hub),0,false));
            CHECK(eventIndex(d,'w',request(hub),0,false) < eventIndex(d,'r',ack(hub),0,false));
            for (const auto& e : d.events) if (e.kind == 'w')
                for (uint32_t address : {root(hub,id),start(hub,id),end(hub,id)})
                    CHECK(e.address != address && e.address != address + 4);
            unrelated(d,before);
        }
        d.clearEvents();
        CHECK(v::reactivate(d.context,d.io(),poll,d.result) == v::Status::InvalidState);
        CHECK(!d.reads && !d.writes && d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
        CHECK(v::reactivate(d.context,d.io(),poll,d.result) == v::Status::InvalidState);
        CHECK(!d.reads && !d.writes && d.result.retentionRequired);
    }
    for (auto hub : {v::Hub::Gfx,v::Hub::Mm}) {
        Device fresh(hub);
        CHECK(v::reactivate(fresh.context,fresh.io(),poll,fresh.result) == v::Status::InvalidState);
        CHECK(!fresh.reads && !fresh.writes && !fresh.result.retentionRequired);
        Device baseline(hub); preparedDetached(baseline);
        CHECK(v::reactivate(baseline.context,baseline.io(),poll,baseline.result) == v::Status::Ok);
        for (size_t n = 1; n <= baseline.reads; ++n) {
            Device d(hub); preparedDetached(d); d.failRead = n;
            CHECK(v::reactivate(d.context,d.io(),poll,d.result) == v::Status::ReadFailed);
            CHECK(d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
            if (hub == v::Hub::Mm && d.result.lastOffset == semaphore(hub)) {
                CHECK(d.result.semaphoreMayBeHeld);
                CHECK(eventIndex(d,'w',semaphore(hub),0) == SIZE_MAX);
            }
        }
        size_t n = 0;
        for (const auto& event : baseline.events) if (event.kind == 'r') {
            ++n;
            if (event.value != UINT32_MAX) {
                Device d(hub); preparedDetached(d); d.onesRead = n;
                CHECK(v::reactivate(d.context,d.io(),poll,d.result) == v::Status::DeviceUnavailable);
                CHECK(d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
            }
            if (event.address == control(hub,1) || event.address == root(hub,1) ||
                event.address == root(hub,1)+4 || event.address == start(hub,1) ||
                event.address == start(hub,1)+4 || event.address == end(hub,1) ||
                event.address == end(hub,1)+4) {
                Device changed(hub); preparedDetached(changed); changed.changeRead = n;
                CHECK(v::reactivate(changed.context,changed.io(),poll,changed.result) == v::Status::ReadbackMismatch);
                CHECK(changed.context.phase() == v::Phase::Faulted && changed.result.retentionRequired);
            }
            for (uint32_t address : {reg(hub,0x167f,0x8f3),reg(hub,0x15bc,0x700),reg(hub,0x15be,0x702),
                reg(hub,0x15ce,0x712),reg(hub,0x15cf,0x713),reg(hub,0x15d0,0x714),reg(hub,0x15d1,0x715),
                reg(hub,0x15d2,0x716),reg(hub,0x15d3,0x717)}) if (event.address == address) {
                Device changed(hub); preparedDetached(changed); changed.changeRead = n;
                CHECK(v::reactivate(changed.context,changed.io(),poll,changed.result) == v::Status::GlobalState);
                CHECK(changed.context.phase() == v::Phase::Faulted && changed.result.retentionRequired);
            }
        }
        for (size_t index = 1; index <= baseline.writes; ++index) for (bool accepted : {false,true}) {
            Device d(hub); preparedDetached(d); d.failWrite = index; d.writeOnFailure = accepted;
            CHECK(v::reactivate(d.context,d.io(),poll,d.result) == v::Status::WriteFailed);
            CHECK(d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
        }
        for (size_t index = 1; index <= baseline.flushes; ++index) {
            Device d(hub); preparedDetached(d); d.failPublish = index;
            CHECK(v::reactivate(d.context,d.io(),poll,d.result) == v::Status::PublishFailed);
            CHECK(d.context.phase() == v::Phase::Faulted && d.result.retentionRequired);
        }
        Device noAck(hub); preparedDetached(noAck); noAck.noAck = true;
        CHECK(v::reactivate(noAck.context,noAck.io(),poll,noAck.result) == v::Status::InvalidateTimeout);
        CHECK(noAck.result.retentionRequired && noAck.context.phase() == v::Phase::Faulted);
        Device invalid(hub); preparedDetached(invalid);
        CHECK(v::reactivate(invalid.context,invalid.io(),{0,1},invalid.result) == v::Status::InvalidArgument);
        CHECK(!invalid.reads && !invalid.writes && invalid.result.retentionRequired);
        // First-publication entry remains Fresh-only; detach cannot silently
        // authorize replacing its retained root with a newly supplied config.
        Device replacement(hub); preparedDetached(replacement); auto other = config(); other.root.address += 4096;
        CHECK(replacement.enable(other) == v::Status::InvalidState);
        CHECK(!replacement.reads && !replacement.writes && replacement.result.retentionRequired);
        CHECK(replacement.context.config().root.address == config().root.address);
    }
    Device noSem(v::Hub::Mm); preparedDetached(noSem); noSem.noSem = true;
    CHECK(v::reactivate(noSem.context,noSem.io(),poll,noSem.result) == v::Status::SemaphoreTimeout);
    CHECK(noSem.result.retentionRequired && noSem.context.phase() == v::Phase::Faulted);
}
}
int main() {
    successful(); arguments(); guardFailures(); failuresAtEveryCall(); laterFailures(); reactivation();
    std::cout << "vm-context: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
