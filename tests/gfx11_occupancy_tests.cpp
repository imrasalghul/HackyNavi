// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_occupancy.hpp"
#include <cstdio>
#include <cstdint>

namespace {
using namespace hackynavi::gfx11occupancy;
uint32_t checks = 0, failures = 0;
void check(bool condition, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        if (failures <= 20) std::printf("failure at line %d\n", line);
    }
}
#define CHECK(value) check((value), __LINE__)
bool equal(const Capacity& a, const Capacity& b) {
    return a.encodedVgprs == b.encodedVgprs && a.allocatedVgprs == b.allocatedVgprs &&
        a.wavesPerSimd == b.wavesPerSimd && a.maxThreadsPerWorkgroup == b.maxThreadsPerWorkgroup;
}
void boundaries() {
    // Expected physical tiers from the pinned Navi31 register geometry, with
    // explicit encoding/allocation distinctions at 8, 24, 192 and 256.
    struct Case { uint32_t vgprs, encoded, allocated, waves, threads; };
    const Case cases[] = {
        {1,8,24,16,1024}, {8,8,24,16,1024}, {9,16,24,16,1024},
        {24,24,24,16,1024}, {25,32,48,16,1024},
        {96,96,96,16,1024}, {97,104,120,12,1024},
        {120,120,120,12,1024}, {121,128,144,10,1024},
        {168,168,168,9,1024}, {169,176,192,8,1024}, {192,192,192,8,1024},
        {193,200,216,7,896}, {216,216,216,7,896},
        {217,224,240,6,768}, {240,240,240,6,768},
        {241,248,264,5,640}, {248,248,264,5,640}, {249,256,264,5,640}, {256,256,264,5,640},
    };
    for (const auto& c : cases) {
        Capacity actual{};
        CHECK(capacity(c.vgprs, actual));
        CHECK(equal(actual, {c.encoded,c.allocated,c.waves,c.threads}));
        CHECK(fits(c.vgprs, c.threads));
        CHECK(!fits(c.vgprs, c.threads + 1));
        CHECK(fits(c.vgprs, 1));
        CHECK(!fits(c.vgprs, 0));
        CHECK(!fits(c.vgprs, UINT32_MAX));
    }
    const Capacity sentinel{0x12345678,0x23456789,0x3456789a,0x456789ab};
    const uint32_t invalidCounts[] = {0u, 257u, UINT32_MAX};
    for (uint32_t invalid : invalidCounts) {
        Capacity actual = sentinel;
        CHECK(!capacity(invalid, actual));
        CHECK(equal(actual, sentinel));
        CHECK(!fits(invalid, 1));
    }
    CHECK(kWaveSize == 32 && kStaticThreadgroupMemoryBytes == 0);
}
void exhaustiveWavePacking() {
    uint32_t minimum = UINT32_MAX, previousMaximum = UINT32_MAX;
    for (uint32_t vgprs = 1; vgprs <= 256; ++vgprs) {
        Capacity actual{};
        CHECK(capacity(vgprs, actual));
        CHECK(actual.encodedVgprs >= vgprs && actual.encodedVgprs - vgprs < 8);
        CHECK(actual.allocatedVgprs >= actual.encodedVgprs && actual.allocatedVgprs - actual.encodedVgprs < 24);
        CHECK(actual.allocatedVgprs % 24 == 0);
        CHECK(actual.wavesPerSimd * actual.allocatedVgprs <= 1536);
        CHECK(actual.wavesPerSimd == 16 || (actual.wavesPerSimd + 1) * actual.allocatedVgprs > 1536);
        CHECK(actual.maxThreadsPerWorkgroup <= previousMaximum);
        previousMaximum = actual.maxThreadsPerWorkgroup;
        if (actual.maxThreadsPerWorkgroup < minimum) minimum = actual.maxThreadsPerWorkgroup;

        // Independent residency oracle: assign each complete/partial wave in
        // round-robin order to the four SIMD register files. A workgroup must
        // fit simultaneously, including the extra partial wave at max+1.
        for (uint32_t threads = 1; threads <= 1025; ++threads) {
            uint32_t registers[4]{}, waveSlots[4]{};
            uint32_t lane = 0, wave = 0;
            while (lane < threads) {
                const uint32_t simd = wave % 4;
                registers[simd] += actual.allocatedVgprs;
                ++waveSlots[simd];
                lane += 32;
                ++wave;
            }
            bool resident = threads <= 1024;
            for (unsigned simd = 0; simd < 4; ++simd)
                resident = resident && registers[simd] <= 1536 && waveSlots[simd] <= 16;
            CHECK(fits(vgprs, threads) == resident);
        }
        CHECK(fits(vgprs, kAllAdmittedVariantsMaxThreads));
    }
    CHECK(minimum == kAllAdmittedVariantsMaxThreads);
}
}
int main() {
    boundaries(); exhaustiveWavePacking();
    std::printf("gfx11_occupancy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
