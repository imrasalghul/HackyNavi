// SPDX-License-Identifier: MIT
// Admission validation only. No simulated GPU execution or completion.
#include "hackynavi/isolated_compute.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace hackynavi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (false)
gfxisolated::Binding binding(uint32_t id, uint64_t va, uint32_t access) {
    return {0x1234, 11, id, 7, va, 4096, 0, 4096, access};
}
gfxisolated::Request request(bool copy) {
    gfxisolated::Request r{}; r.owner = 0x1234; r.epoch = 11; r.vmid = 1;
    r.shader = binding(1, 0x10000, 5); r.indirect = binding(2, 0x20000, 5);
    r.destination = binding(3, 0x30000, 3);
    if (copy) r.source = binding(4, 0x40000, 1);
    r.dispatch = {copy ? gfxcompute::Operation::Copy : gfxcompute::Operation::Clear,
        {0x10000, 4096}, {copy ? 0x40000ULL : 0, copy ? 256ULL : 0}, {0x30000, 256},
        256, {0x12345678, 0xdeadbeef, 0x01020304, 0xff00ff00}};
    return r;
}
void canonicalAndPackets() {
    for (bool copy : {false, true}) {
        auto r = request(copy); uint32_t commands[128]; size_t words = 0;
        CHECK(gfxcompute::encode(r.dispatch, commands, 128, words) == gfxcompute::Status::Ok);
        for (uint8_t vmid = 1; vmid <= 7; ++vmid) {
            r.vmid = vmid;
            CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::Ok);
        }
        // Every single-bit packet/header/register/address/state mutation is
        // rejected. This grammar has no unchecked payload or padding words.
        for (size_t n = 0; n < words; ++n) for (unsigned bit = 0; bit < 32; ++bit) {
            commands[n] ^= 1U << bit;
            CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidStream);
            commands[n] ^= 1U << bit;
        }
        for (size_t count = 1; count < words; ++count)
            CHECK(gfxisolated::validate(r, commands, count) == gfxisolated::Status::InvalidStream);
        commands[words] = 0xffff1000;
        CHECK(gfxisolated::validate(r, commands, words + 1) == gfxisolated::Status::InvalidStream);
        const uint32_t forbidden[] = {0xc0023f00, 0xc0033700, 0xc0064900, 0xc0044000,
            0xc0002200, 0xc0043c00, 0xc0011602, 0xc0016800, 0xc0016900};
        for (auto packet : forbidden) {
            const auto old = commands[0]; commands[0] = packet;
            CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidStream);
            commands[0] = old;
        }
        r.vmid = 0; CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidArgument);
        r.vmid = 8; CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidArgument);
    }
}
void ownership() {
    auto valid = request(true); uint32_t commands[128]; size_t words = 0;
    CHECK(gfxcompute::encode(valid.dispatch, commands, 128, words) == gfxcompute::Status::Ok);
    for (unsigned selected = 0; selected < 4; ++selected) for (unsigned mutation = 0; mutation < 10; ++mutation) {
        auto r = valid;
        gfxisolated::Binding* buffers[] = {&r.shader, &r.source, &r.destination, &r.indirect};
        auto& b = *buffers[selected];
        if (mutation == 0) b.owner++;
        if (mutation == 1) b.epoch++;
        if (mutation == 2) b.id = 0;
        if (mutation == 3) b.generation = 0;
        if (mutation == 4) b.bytes = 0;
        if (mutation == 5) b.gpu = (1ULL << 47);
        if (mutation == 6) b.bufferOffset = 4096;
        if (mutation == 7) b.gpu++;
        if (mutation == 8) b.access = 0;
        if (mutation == 9) b.access |= 8;
        CHECK(gfxisolated::validate(r, commands, words) != gfxisolated::Status::Ok);
    }
    for (bool shader : {false, true}) {
        auto r = valid; (shader ? r.shader : r.indirect).access |= gfxisolated::Write;
        CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::Permission);
        r = valid; (shader ? r.shader : r.indirect).id = r.destination.id;
        CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::AliasedControlStorage);
    }
    auto r = valid; r.destination.gpu += 4096; // descriptor no longer mapped
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidBinding);
    r = valid; r.dispatch.source.gpu += 4096;
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidBinding);
    r = valid; r.dispatch.destination.bytes = 4096;
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidBinding);
    r = valid; r.indirect.gpu = r.destination.gpu;
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::Overlap);
    // Physically overlapping aliases of one BO remain forbidden even though
    // their virtual addresses are separate and each individual range is valid.
    r = valid; r.source.id = r.destination.id;
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::Overlap);
    r.source.generation++;
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidBinding);
    // Disjoint subranges of a single retained mapping are a valid copy.
    r = valid; r.source = r.destination; r.dispatch.source.gpu = r.destination.gpu + 512;
    CHECK(gfxcompute::encode(r.dispatch, commands, 128, words) == gfxcompute::Status::Ok);
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::Ok);
    r = request(false); r.source = valid.source;
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidBinding);
}
void malformed() {
    auto r = request(false); uint32_t commands[128]; size_t words = 0;
    CHECK(gfxcompute::encode(r.dispatch, commands, 128, words) == gfxcompute::Status::Ok);
    CHECK(gfxisolated::validate(r, nullptr, words) == gfxisolated::Status::InvalidArgument);
    CHECK(gfxisolated::validate(r, commands, 0) == gfxisolated::Status::InvalidArgument);
    CHECK(gfxisolated::validate(r, commands, 129) == gfxisolated::Status::InvalidArgument);
    CHECK(gfxisolated::validate(r, commands, SIZE_MAX) == gfxisolated::Status::InvalidArgument);
    CHECK(gfxisolated::validate(r, reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(commands)+1), words) == gfxisolated::Status::InvalidArgument);
    CHECK(gfxisolated::validate(r, reinterpret_cast<uint32_t*>(UINTPTR_MAX-3), words) == gfxisolated::Status::InvalidArgument);
    r.owner = 0; CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidArgument);
    r = request(false); r.epoch = 0;
    CHECK(gfxisolated::validate(r, commands, words) == gfxisolated::Status::InvalidArgument);
}
}
int main() {
    canonicalAndPackets(); ownership(); malformed();
    std::printf("isolated-compute: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
