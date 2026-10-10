// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

namespace hackynavi { namespace gfx11occupancy {
// Only the current Navi31 / GFX1100 wave32, WGP-mode, zero-LDS, zero-scratch,
// zero-shared-VGPR compute profile. This is register capacity, not ISA or GPU
// admission. The caller must independently establish that exact profile, own
// the actual compiler metadata/code and validate every instruction/resource.
// See docs/gfx11-compute-occupancy.md for pinned source evidence.
constexpr uint32_t kWaveSize = 32;
constexpr uint32_t kSimdsPerWgp = 4;
constexpr uint32_t kPhysicalWave32VgprsPerSimd = 1536;
constexpr uint32_t kMaxWavesPerSimd = 16;
constexpr uint32_t kVgprEncodingGranularity = 8;
constexpr uint32_t kVgprAllocationGranularity = 24;
constexpr uint32_t kMaxEncodedVgprs = 256;
constexpr uint32_t kArchitecturalMaxThreads = 1024;
constexpr uint32_t kStaticThreadgroupMemoryBytes = 0;

struct Capacity {
    uint32_t encodedVgprs;
    uint32_t allocatedVgprs;
    uint32_t wavesPerSimd;
    uint32_t maxThreadsPerWorkgroup;
};

// numVgprs is the actual owned compiler/validated launch count, not a user
// hint. GFX11 SGPR allocation is fixed (108 per wave); the separately admitted
// numSgprs range must not be put through the older variable-SGPR occupancy rule.
// RSRC1 encodes VGPRs in units of eight, but Navi31 physically allocates them
// in units of 24 for wave32. In particular, encoded 256 consumes 264 registers.
// No allocation or hardware access. Failure preserves the entire output.
constexpr bool capacity(uint32_t numVgprs, Capacity& out) {
    if (numVgprs == 0 || numVgprs > kMaxEncodedVgprs) return false;
    const uint32_t encoded = ((numVgprs + kVgprEncodingGranularity - 1) /
                              kVgprEncodingGranularity) * kVgprEncodingGranularity;
    const uint32_t allocated = ((encoded + kVgprAllocationGranularity - 1) /
                                kVgprAllocationGranularity) * kVgprAllocationGranularity;
    const uint32_t registerWaves = kPhysicalWave32VgprsPerSimd / allocated;
    const uint32_t waves = registerWaves < kMaxWavesPerSimd ? registerWaves : kMaxWavesPerSimd;
    const uint32_t residentThreads = waves * kSimdsPerWgp * kWaveSize;
    out = {encoded, allocated, waves,
           residentThreads < kArchitecturalMaxThreads ? residentThreads : kArchitecturalMaxThreads};
    return true;
}

// totalThreads must be the checked product of the actual local dimensions.
// This does not allow changing a fixed shader's compiled workgroup dimensions.
constexpr bool fits(uint32_t numVgprs, uint32_t totalThreads) {
    Capacity result{};
    return totalThreads != 0 && capacity(numVgprs, result) && totalThreads <= result.maxThreadsPerWorkgroup;
}

// A pipeline which generates new variants for future local shapes cannot infer
// a global property from one low-register variant. This bound accommodates
// every VGPR count admitted by the fixed profile; each actual variant still
// needs its own compiler, dimension and ISA admission checks.
constexpr uint32_t kAllAdmittedVariantsMaxThreads = 640;
static_assert(kPhysicalWave32VgprsPerSimd / 264 * kSimdsPerWgp * kWaveSize ==
              kAllAdmittedVariantsMaxThreads, "Navi31 wave32 worst-case register capacity");
static_assert(fits(256, 640) && !fits(256, 641), "Navi31 full-register workgroup boundary");
} }
