// SPDX-License-Identifier: MIT
// Host-only mailbox model. Fault injection validates protocol boundaries; it
// does not execute PMFW, native callbacks, or demonstrate a working display.
#include "hackynavi/display_smu.hpp"
#include <cstring>
#include <iostream>
#include <vector>
namespace d = hackynavi::displaysmu;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
const uint32_t clocks[] = {1,2,3,8,9,11,12};
const d::Poll poll = {1000, 10};
struct Command { uint32_t message, argument; };
struct Device {
    const d::Layout layout = {true, 0x80000, 0x1628a * 4, 0x16273 * 4, 0x16274 * 4};
    d::Controller controller;
    d::Result result = {};
    d::DpmInventory output = {};
    uint32_t metadata[7] = {}, levels[7][16] = {};
    uint32_t dcMax[7] = {};
    uint32_t response = 1, argument = 0, commandResponse = 1;
    unsigned calls = 0, failAt = 0, onesAt = 0, commandErrorAt = 0;
    unsigned queries = 0, delayCount = 0, delayTotal = 0, responseDelay = 0, remainingDelay = 0;
    unsigned metadataVisits[7] = {};
    int changeClock = -1, changeIndex = -2;
    bool acceptedFailure = false;
    std::vector<Command> commands;
    std::vector<unsigned> readCalls;
    Device() {
        for (unsigned n = 0; n < 7; ++n) {
            metadata[n] = n % 2 ? 0x80000000u : 4;
            for (unsigned level = 0; level < 16; ++level)
                levels[n][level] = 0x12340000u | (100 + n * 100 + level * 10);
            dcMax[n] = uint16_t(levels[n][n % 2 ? 1 : 3]);
        }
        CHECK(controller.initialize(layout, {this,read,write,delay}, poll, result) == d::Status::Ok);
        calls = 0; commands.clear(); readCalls.clear();
    }
    unsigned expectedQueries() const {
        unsigned count = 0;
        for (auto m : metadata) count += 2 + ((m & 0x80000000) ? 2 : (m & 0xff));
        return 2 * count;
    }
    d::Status query(const d::Poll& p = poll) { return controller.queryDpmInventory(p, output, result); }
    bool outputEmpty() const {
        if (output.firmwareVersion) return false;
        for (const auto& t : output.clocks) {
            if (uint8_t(t.clock) || t.metadata || t.levelCount || t.fineGrained || t.dcMaxMHz) return false;
            for (unsigned n = 0; n < 16; ++n) if (t.rawFrequency[n] || t.frequencyMHz[n]) return false;
        }
        return true;
    }
    void poisonOutput() { std::memset(&output, 0x55, sizeof(output)); }
    static bool read(void* context, uint32_t address, uint32_t& value) {
        auto& x = *static_cast<Device*>(context); ++x.calls; x.readCalls.push_back(x.calls);
        CHECK(address == x.layout.argument || address == x.layout.response);
        if (address == x.layout.response && x.remainingDelay) { --x.remainingDelay; value = 0; }
        else value = address == x.layout.argument ? x.argument : x.response;
        if (x.calls == x.onesAt) value = UINT32_MAX;
        return x.calls != x.failAt;
    }
    static bool write(void* context, uint32_t address, uint32_t value) {
        auto& x = *static_cast<Device*>(context); ++x.calls;
        const bool ok = x.calls != x.failAt;
        if (!ok && !x.acceptedFailure) return false;
        if (address == x.layout.response) { CHECK(value == 0); x.response = 0; }
        else if (address == x.layout.argument) x.argument = value;
        else {
            CHECK(address == x.layout.message); CHECK(x.response == 0);
            x.commands.push_back({value, x.argument}); x.response = 1;
            if (value == 2) x.argument = 0x4e6a00;
            else if (value == 4) x.argument = 1;
            else {
                CHECK(value == 0xb || value == 0xc); ++x.queries;
                if (x.queries == x.commandErrorAt) x.response = x.commandResponse;
                const unsigned clock = x.argument >> 16, index = x.argument & 0xffff;
                unsigned n = 0; while (n < 7 && clocks[n] != clock) ++n;
                CHECK(n < 7); CHECK(index == 0xff || index < 16);
                if (n >= 7 || (index != 0xff && index >= 16)) return false;
                if (value == 0xc) { CHECK(index == 0); x.argument = x.dcMax[n]; }
                else if (index == 0xff) { ++x.metadataVisits[n]; x.argument = x.metadata[n]; }
                else x.argument = x.levels[n][index];
                if (int(n) == x.changeClock && x.metadataVisits[n] == 2 &&
                    ((value == 0xc && x.changeIndex == -3) ||
                     (value == 0xb && (x.changeIndex == int(index) || (x.changeIndex == -1 && index == 0xff)))))
                    x.argument ^= index == 0xff ? 0x10000000 : 1;
                x.remainingDelay = x.responseDelay;
            }
        }
        return ok;
    }
    static void delay(void* context, uint32_t us) {
        auto& x = *static_cast<Device*>(context); ++x.delayCount; x.delayTotal += us;
    }
};
void terminal(Device& x) {
    CHECK(x.outputEmpty()); CHECK(x.controller.faulted() && !x.controller.ready());
    CHECK(!x.controller.minimumMayBeActive() && !x.result.minimumRequestAttempted);
    const auto stopped = x.calls; x.failAt = 0; x.onesAt = 0;
    CHECK(x.query() == d::Status::InvalidState); CHECK(x.calls == stopped && x.outputEmpty());
}
}
int main() {
    Device baseline; CHECK(baseline.query() == d::Status::Ok);
    CHECK(baseline.controller.ready() && !baseline.controller.faulted());
    CHECK(!baseline.controller.minimumMayBeActive() && !baseline.result.minimumRequestAttempted);
    CHECK(baseline.output.firmwareVersion == 0x4e6a00);
    CHECK(baseline.queries == baseline.expectedQueries());
    CHECK(baseline.result.responseReads == 2 * baseline.queries && baseline.calls == 6 * baseline.queries);
    CHECK(baseline.result.mailboxWriteAttempted && baseline.result.stage == d::Stage::Complete);
    size_t command = 0;
    for (unsigned pass = 0; pass < 2; ++pass) for (unsigned n = 0; n < 7; ++n) {
        const auto& table = baseline.output.clocks[n];
        CHECK(uint32_t(table.clock) == clocks[n] && table.metadata == baseline.metadata[n]);
        CHECK(table.fineGrained == bool(n % 2) && table.levelCount == (n % 2 ? 2 : 4));
        const auto& meta = baseline.commands[command++];
        CHECK(meta.message == 0xb && meta.argument == ((clocks[n] << 16) | 0xff));
        for (unsigned level = 0; level < table.levelCount; ++level) {
            const auto& entry = baseline.commands[command++];
            CHECK(entry.message == 0xb && entry.argument == ((clocks[n] << 16) | level));
            CHECK(table.rawFrequency[level] == baseline.levels[n][level]);
            CHECK(table.frequencyMHz[level] == uint16_t(baseline.levels[n][level]));
        }
        const auto& dc = baseline.commands[command++];
        CHECK(dc.message == 0xc && dc.argument == (clocks[n] << 16));
        CHECK(table.dcMaxMHz == baseline.dcMax[n]);
    }
    // Every transport call can fail, including a posted write that executed.
    for (bool accepted : {false,true}) for (unsigned at = 1; at <= baseline.calls; ++at) {
        Device x; x.poisonOutput(); x.failAt = at; x.acceptedFailure = accepted;
        const auto status = x.query();
        CHECK(status == d::Status::ReadFailed || status == d::Status::WriteFailed);
        CHECK(x.calls == at); terminal(x);
    }
    // All-ones at every read is rejected; no partial table escapes.
    for (const unsigned at : baseline.readCalls) {
        Device x; x.poisonOutput(); x.onesAt = at;
        CHECK(x.query() == d::Status::DeviceUnavailable); CHECK(x.calls == at); terminal(x);
    }
    // A finite table must be available; reserved metadata never becomes count.
    for (unsigned n = 0; n < 7; ++n) for (unsigned bit = 8; bit < 28; ++bit) {
        Device x; x.metadata[n] |= 1u << bit;
        CHECK(x.query() == d::Status::InvalidDpmInventory); terminal(x);
    }
    for (unsigned n = 0; n < 7; ++n) {
        for (uint32_t count : {0u,17u,255u}) {
            Device x; x.metadata[n] = count;
            CHECK(x.query() == (count ? d::Status::InventoryCapacityExceeded : d::Status::InvalidDpmInventory));
            terminal(x);
        }
        for (int index : {-3,-1,0,1}) {
            Device x; x.changeClock = int(n); x.changeIndex = index;
            CHECK(x.query() == d::Status::ChangedDpmInventory); terminal(x);
        }
        for (uint32_t limit : {0u,65536u}) {
            Device x; x.dcMax[n] = limit;
            CHECK(x.query() == d::Status::InvalidFrequency); terminal(x);
        }
        for (unsigned level = 0; level < (n % 2 ? 2u : 4u); ++level) {
            Device x; x.levels[n][level] = 0x12340000;
            CHECK(x.query() == d::Status::InvalidFrequency); terminal(x);
        }
        Device x; x.metadata[n] = 0x80000000; x.levels[n][1] = x.levels[n][0] - 1;
        CHECK(x.query() == d::Status::InvalidDpmInventory); terminal(x);
    }
    // Mode bits survive unchanged; fine-grained ignores low count like Linux.
    for (uint32_t flags = 0; flags < 16; ++flags) {
        Device x;
        for (unsigned n = 0; n < 7; ++n) x.metadata[n] = (flags << 28) | ((flags & 8) ? 255 : 16);
        CHECK(x.query() == d::Status::Ok);
        for (unsigned n = 0; n < 7; ++n) {
            CHECK(x.output.clocks[n].metadata == x.metadata[n]);
            CHECK(x.output.clocks[n].levelCount == ((flags & 8) ? 2 : 16));
        }
    }
    for (uint32_t error : {0xffu,0xfeu,0xfdu,0xfcu,0x77u,0u}) {
        for (unsigned at = 1; at <= baseline.queries; ++at) {
            Device x; x.commandErrorAt = at; x.commandResponse = error;
            CHECK(x.query() != d::Status::Ok); CHECK(x.queries == at);
            CHECK(x.result.responseReads <= poll.maxReads); terminal(x);
        }
    }
    // One aggregate budget spans metadata, levels and BOTH full observations.
    for (unsigned reads = 1; reads < baseline.result.responseReads; ++reads) {
        Device x; const d::Poll bounded = {reads,10};
        CHECK(x.query(bounded) == d::Status::Timeout);
        CHECK(x.result.responseReads == reads); CHECK(x.queries <= reads / 2); terminal(x);
    }
    { Device x; const d::Poll exact = {baseline.result.responseReads,10}; CHECK(x.query(exact) == d::Status::Ok); }
    { Device x; x.responseDelay = 3; CHECK(x.query() == d::Status::Ok);
      CHECK(x.delayCount == x.queries * 3 && x.delayTotal == x.delayCount * 10);
      CHECK(x.result.responseReads == x.queries * 5); }
    for (const d::Poll invalid : {d::Poll{0,10},d::Poll{1,0},d::Poll{1000001,1},d::Poll{1,1001},d::Poll{2001,1000}}) {
        Device x; x.poisonOutput(); CHECK(x.query(invalid) == d::Status::InvalidArgument);
        CHECK(x.calls == 0 && x.outputEmpty() && x.controller.ready() && !x.controller.faulted());
    }
    { Device x; CHECK(x.query() == d::Status::Ok); x.controller.clearAfterVerifiedReset(); x.poisonOutput();
      const auto before = x.calls; CHECK(x.query() == d::Status::InvalidState); CHECK(x.calls == before && x.outputEmpty()); }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
