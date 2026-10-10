// SPDX-License-Identifier: MIT
// Offline metadata admission only. No device, transport replacement or GPU.
#include "../native/HNMetalCapabilities.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using namespace hackynavi;
namespace {
size_t checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
nativeabi::HelloReply input() {
    nativeabi::HelloReply h{};
    h.header = nativeabi::makeHeader(sizeof(h), 9, nativeabi::kReusableMinor);
    h.capabilities = nativeabi::kReusableCapabilities;
    h.registryId = 42;
    h.vramBytes = 24ULL * 1024 * 1024 * 1024;
    h.cpuVisibleVramBytes = h.vramBytes;
    h.systemBudget = 512ULL * 1024 * 1024;
    h.vaBegin = 1ULL << 32; h.vaEnd = h.vaBegin + (1ULL << 36);
    h.maxAllocationBytes = 64ULL * 1024 * 1024;
    h.vendorId = 0x1002; h.deviceId = 0x744c; h.gcVersion = 0x0b0000;
    h.pageBytes = 4096; h.maxResources = nativeabi::kMaxResources;
    return h;
}
bool zero(const metal::CapabilityData& data) {
    HNMetalDeviceLimits empty{};
    return !data.ownerRegistryID && !data.epoch && !data.systemBudget && !data.vramBytes &&
        !data.vaBegin && !data.vaEnd && !data.pageBytes &&
        !std::memcmp(&data.limits, &empty, sizeof(empty));
}
void reject(const nativeabi::HelloReply& h, uint64_t owner = 42) {
    metal::CapabilityData out{};
    require(bool(metal::deriveCapabilityData(input(), 42, out)), "prime output with valid metadata");
    require(!bool(metal::deriveCapabilityData(h, owner, out)), "invalid metadata rejected");
    require(zero(out), "failure clears previous capability data");
}
}
int main() {
    const auto valid = input();
    metal::CapabilityData out{};
    require(bool(metal::deriveCapabilityData(valid, 42, out)), "admit exact reusable metadata");
    require(out.ownerRegistryID == 42 && out.epoch == 9, "preserve owner and epoch");
    require(out.systemBudget == valid.systemBudget && out.vramBytes == valid.vramBytes,
            "native budget and VRAM remain distinct");
    require(out.pageBytes == 4096 && out.vaBegin == valid.vaBegin && out.vaEnd == valid.vaEnd,
            "preserve actual geometry");
    HNMetalDeviceLimits expected{};
    expected.maxComputeBuffers = 31;
    expected.maxTotalComputeThreadsPerThreadgroup = 640;
    expected.maxBufferLength = valid.maxAllocationBytes;
    require(!std::memcmp(&out.limits, &expected, sizeof(expected)),
            "only implemented limits advertised, including reserved field zero");
    auto graphics = valid;
    graphics.header.minor = nativeabi::kGraphicsMinor;
    graphics.capabilities = nativeabi::kGraphicsCapabilities;
    require(bool(metal::deriveCapabilityData(graphics,42,out)), "explicit graphics profile admitted");
    auto graphicsLimits = expected;
    graphicsLimits.maxFramebufferStorageBits=32;
    graphicsLimits.maxColorAttachments=1;
    graphicsLimits.maxTextureWidth2D=16384; graphicsLimits.maxTextureHeight2D=16384;
    graphicsLimits.maxTextureLayers=1; graphicsLimits.maxViewportCount=1;
    graphicsLimits.maxVertexAmplificationFactor=1; graphicsLimits.maxVertexAmplificationCount=1;
    require(!std::memcmp(&out.limits,&graphicsLimits,sizeof(graphicsLimits)),
            "graphics advertises only actual restricted target/viewport limits");
    for (unsigned bit=0;bit<64;++bit) {
        auto changed=graphics; changed.capabilities^=uint64_t(1)<<bit; reject(changed);
    }
    for (uint16_t minor=0;minor<8;++minor) {
        if (minor==nativeabi::kGraphicsMinor) continue;
        auto changed=graphics; changed.header.minor=minor; reject(changed);
    }
    auto buffered=graphics;
    buffered.header.minor=nativeabi::kBufferedGraphicsMinor;
    buffered.capabilities=nativeabi::kBufferedGraphicsCapabilities;
    require(bool(metal::deriveCapabilityData(buffered,42,out)), "exact buffered graphics profile admitted");
    auto bufferedLimits=graphicsLimits; bufferedLimits.maxFragmentBuffers=1;
    require(!std::memcmp(&out.limits,&bufferedLimits,sizeof(bufferedLimits)),
            "only the actual single fragment buffer is added to graphics limits");
    for (unsigned bit=0;bit<64;++bit) {
        auto changed=buffered; changed.capabilities^=uint64_t(1)<<bit; reject(changed);
    }
    for (uint16_t minor=0;minor<8;++minor) {
        if (minor==nativeabi::kBufferedGraphicsMinor) continue;
        auto changed=buffered; changed.header.minor=minor; reject(changed);
    }
    auto vertexBuffered=buffered;
    vertexBuffered.header.minor=nativeabi::kVertexBufferedGraphicsMinor;
    vertexBuffered.capabilities=nativeabi::kVertexBufferedGraphicsCapabilities;
    require(bool(metal::deriveCapabilityData(vertexBuffered,42,out)),
            "exact vertex and fragment buffered graphics profile admitted");
    auto vertexBufferedLimits=bufferedLimits; vertexBufferedLimits.maxVertexBuffers=1;
    require(!std::memcmp(&out.limits,&vertexBufferedLimits,sizeof(vertexBufferedLimits)),
            "one actual vertex buffer is added without advertising vertex attributes");
    for (unsigned bit=0;bit<64;++bit) {
        auto changed=vertexBuffered; changed.capabilities^=uint64_t(1)<<bit; reject(changed);
    }
    for (uint16_t minor=0;minor<9;++minor) {
        if (minor==nativeabi::kVertexBufferedGraphicsMinor) continue;
        auto changed=vertexBuffered; changed.header.minor=minor; reject(changed);
    }
    reject(valid, 0); reject(valid, 43);
    auto h = valid;
    h.header.magic ^= 1; reject(h);
    h = valid; ++h.header.bytes; reject(h);
    h = valid; ++h.header.major; reject(h);
    for (uint16_t minor = 0; minor < 8; ++minor) {
        if (minor == nativeabi::kReusableMinor) continue;
        h = valid; h.header.minor = minor; reject(h);
    }
    h = valid; h.header.epoch = 0; reject(h);
    h = valid; h.registryId = 0; reject(h);
    for (unsigned bit = 0; bit < 32; ++bit) {
        h = valid; h.header.flags = uint32_t(1) << bit; reject(h);
    }
    for (unsigned bit = 0; bit < 64; ++bit) {
        h = valid; h.capabilities ^= uint64_t(1) << bit; reject(h);
    }
    h = valid; h.reserved[0] = 1; reject(h);
    h = valid; h.reserved[1] = UINT64_MAX; reject(h);
    h = valid; h.vendorId = 0x10de; reject(h);
    h = valid; h.deviceId = 0x7448; reject(h);
    h = valid; h.gcVersion = 0x0b0001; reject(h);
    const uint32_t pages[] = {0, 1, 2048, 4095, 4097, 8192, 65536, UINT32_MAX};
    for (auto page : pages) { h = valid; h.pageBytes = page; reject(h); }
    const uint32_t resources[] = {0, 1, 30, nativeabi::kMaxResources + 1, UINT32_MAX};
    for (auto count : resources) { h = valid; h.maxResources = count; reject(h); }
    for (auto count : {31U, 32U, nativeabi::kMaxResources}) {
        h = valid; h.maxResources = count;
        require(bool(metal::deriveCapabilityData(h, 42, out)), "31 public slots fit native resource limit");
        require(out.limits.maxComputeBuffers == 31, "native capacity does not expand public Metal slots");
    }
    h = valid; h.maxIbs = 1; reject(h);
    h = valid; h.maxIbBytes = 4; reject(h);
    h = valid; h.vaBegin = 0; reject(h);
    h = valid; ++h.vaBegin; reject(h);
    h = valid; --h.vaEnd; reject(h);
    h = valid; h.vaEnd = h.vaBegin; reject(h);
    h = valid; h.vaEnd = (1ULL << 48) + 4096; reject(h);
    h = valid; h.maxAllocationBytes = 0; reject(h);
    h = valid; --h.maxAllocationBytes; reject(h);
    h = valid; h.maxAllocationBytes = h.vaEnd - h.vaBegin + 4096; reject(h);
    h = valid; h.maxAllocationBytes = UINT64_MAX; reject(h);
    h = valid; h.vramBytes = 0; reject(h);
    h = valid; ++h.cpuVisibleVramBytes; reject(h);
    for (uint64_t budget : {0ULL, 1ULL, 4095ULL}) {
        h = valid; h.systemBudget = budget; reject(h);
    }
    for (uint64_t budget : {4096ULL, 4097ULL, 8191ULL, 8192ULL, 67108863ULL, 67108864ULL, UINT64_MAX}) {
        h = valid; h.systemBudget = budget;
        require(bool(metal::deriveCapabilityData(h, 42, out)), "budget-backed maximum admitted");
        const uint64_t maximum = (budget & ~uint64_t(4095)) < h.maxAllocationBytes ?
            (budget & ~uint64_t(4095)) : h.maxAllocationBytes;
        require(out.limits.maxBufferLength == maximum && maximum <= budget,
                "max buffer fits rounded native allocation and actual budget");
        require(out.systemBudget == budget, "do not round reported native budget");
    }
    h = valid; h.header.epoch = UINT64_MAX; h.registryId = UINT64_MAX;
    require(bool(metal::deriveCapabilityData(h, UINT64_MAX, out)), "nonzero full-width identities preserve bits");
    require(out.epoch == UINT64_MAX && out.ownerRegistryID == UINT64_MAX, "identity not narrowed");
    metal::Capabilities empty;
    HNMetalDeviceLimits neverReady = expected;
    require(!empty.valid() && !empty.supportsReadWriteBufferArguments() &&
            !empty.supportsNonUniformThreadgroupSize(), "unopened helper never advertises positive query");
    require(empty.makeLimits(neverReady).status == compute::Status::NotOpen,
            "limits require real Context capture");
    HNMetalDeviceLimits zeros{};
    require(!std::memcmp(&neverReady, &zeros, sizeof(zeros)), "unopened limits output cleared");
    metal::CapabilityQueries neverQueries{42, 9, true, true};
    require(empty.makeFeatureQueries(neverQueries).status == compute::Status::NotOpen,
            "positive query state requires actual captured Context");
    require(!neverQueries.ownerRegistryID && !neverQueries.epoch &&
            !neverQueries.readWriteBufferArguments && !neverQueries.nonUniformThreadgroupSize,
            "unopened query state cleared");
    std::printf("%zu checks, 0 failures; offline metadata only\n", checks);
}
