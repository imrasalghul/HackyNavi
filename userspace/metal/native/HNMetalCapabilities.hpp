// SPDX-License-Identifier: MIT
#pragma once
#include "../../runtime/compute_context.hpp"
#include "hackynavi/gfx11_occupancy.hpp"
#include <cstddef>
#include <cstdint>

// Exact anonymous struct encoding measured from macOS 15.8/24H22 Metal.
// Owned data only: no inherited object offsets and no borrowed feature profile.
// See docs/metal-capability-state.md and build/metal-capability-layout.
typedef struct {
    uint32_t maxFramebufferStorageBits; // 0
    uint32_t linearTextureArrayAlignmentBytes; // 4
    uint32_t linearTextureArrayAlignmentSlice; // 8
    uint32_t maxTileBuffers; // 12
    uint32_t maxTileTextures; // 16
    uint32_t maxTileSamplers; // 20
    uint32_t maxTileInlineDataSize; // 24
    uint32_t minTilePixels; // 28
    uint32_t maxColorAttachments; // 32
    uint32_t maxVertexAttributes; // 36
    uint32_t maxVertexBuffers; // 40
    uint32_t maxVertexTextures; // 44
    uint32_t maxVertexSamplers; // 48
    uint32_t maxVertexInlineDataSize; // 52
    uint32_t maxInterpolants; // 56
    uint32_t maxFragmentBuffers; // 60
    uint32_t maxFragmentTextures; // 64
    uint32_t maxFragmentSamplers; // 68
    uint32_t maxFragmentInlineDataSize; // 72
    uint32_t maxComputeBuffers; // 76
    uint32_t maxComputeTextures; // 80
    uint32_t maxComputeSamplers; // 84
    uint32_t maxComputeInlineDataSize; // 88
    uint32_t maxComputeLocalMemorySizes; // 92
    uint32_t maxTotalComputeThreadsPerThreadgroup; // 96
    uint32_t maxComputeThreadgroupMemory; // 100
    float maxLineWidth; // 104
    float maxPointSize; // 108
    uint32_t maxVisibilityQueryOffset; // 112
    uint32_t reserved116; // 116
    uint32_t minConstantBufferAlignmentBytes; // 120
    uint32_t minBufferNoCopyAlignmentBytes; // 124
    uint32_t maxTextureWidth1D; // 128
    uint32_t maxTextureWidth2D; // 132
    uint32_t maxTextureHeight2D; // 136
    uint32_t maxTextureWidth3D; // 140
    uint32_t maxTextureHeight3D; // 144
    uint32_t maxTextureDepth3D; // 148
    uint32_t maxTextureDimensionCube; // 152
    uint32_t maxTextureLayers; // 156
    uint32_t linearTextureAlignmentBytes; // 160
    uint32_t iosurfaceTextureAlignmentBytes; // 164
    uint32_t iosurfaceReadOnlyTextureAlignmentBytes; // 168
    uint32_t deviceLinearTextureAlignmentBytes; // 172
    uint32_t deviceLinearReadOnlyTextureAlignmentBytes; // 176
    uint32_t maxFunctionConstantIndices; // 180
    uint32_t maxComputeThreadgroupMemoryAlignmentBytes; // 184
    uint32_t maxInterpolatedComponents; // 188
    uint32_t maxTessellationFactor; // 192
    uint32_t maxIndirectBuffers; // 196
    uint32_t maxIndirectTextures; // 200
    uint32_t maxIndirectSamplers; // 204
    uint32_t maxIndirectSamplersPerDevice; // 208
    uint32_t maxFenceInstances; // 212
    uint32_t maxViewportCount; // 216
    uint32_t maxCustomSamplePositions; // 220
    uint32_t maxVertexAmplificationFactor; // 224
    uint32_t maxVertexAmplificationCount; // 228
    uint32_t maxTextureBufferWidth; // 232
    uint32_t maxComputeAttributes; // 236
    uint32_t maxIOCommandsInFlight; // 240
    uint32_t maxPredicatedNestingDepth; // 244
    uint32_t maxAccelerationStructureLevels; // 248
    uint32_t maxConstantBufferArguments; // 252
    uint64_t maxBufferLength; // 256
} HNMetalDeviceLimits;

static_assert(sizeof(HNMetalDeviceLimits) == 264, "Actual Metal limits size");
static_assert(alignof(HNMetalDeviceLimits) == 8, "Actual Metal limits alignment");
static_assert(offsetof(HNMetalDeviceLimits, maxFramebufferStorageBits) == 0, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, linearTextureArrayAlignmentBytes) == 4, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, linearTextureArrayAlignmentSlice) == 8, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTileBuffers) == 12, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTileTextures) == 16, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTileSamplers) == 20, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTileInlineDataSize) == 24, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, minTilePixels) == 28, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxColorAttachments) == 32, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVertexAttributes) == 36, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVertexBuffers) == 40, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVertexTextures) == 44, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVertexSamplers) == 48, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVertexInlineDataSize) == 52, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxInterpolants) == 56, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxFragmentBuffers) == 60, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxFragmentTextures) == 64, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxFragmentSamplers) == 68, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxFragmentInlineDataSize) == 72, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeBuffers) == 76, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeTextures) == 80, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeSamplers) == 84, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeInlineDataSize) == 88, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeLocalMemorySizes) == 92, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTotalComputeThreadsPerThreadgroup) == 96, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeThreadgroupMemory) == 100, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxLineWidth) == 104, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxPointSize) == 108, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVisibilityQueryOffset) == 112, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, reserved116) == 116, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, minConstantBufferAlignmentBytes) == 120, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, minBufferNoCopyAlignmentBytes) == 124, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureWidth1D) == 128, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureWidth2D) == 132, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureHeight2D) == 136, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureWidth3D) == 140, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureHeight3D) == 144, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureDepth3D) == 148, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureDimensionCube) == 152, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureLayers) == 156, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, linearTextureAlignmentBytes) == 160, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, iosurfaceTextureAlignmentBytes) == 164, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, iosurfaceReadOnlyTextureAlignmentBytes) == 168, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, deviceLinearTextureAlignmentBytes) == 172, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, deviceLinearReadOnlyTextureAlignmentBytes) == 176, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxFunctionConstantIndices) == 180, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeThreadgroupMemoryAlignmentBytes) == 184, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxInterpolatedComponents) == 188, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTessellationFactor) == 192, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxIndirectBuffers) == 196, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxIndirectTextures) == 200, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxIndirectSamplers) == 204, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxIndirectSamplersPerDevice) == 208, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxFenceInstances) == 212, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxViewportCount) == 216, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxCustomSamplePositions) == 220, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVertexAmplificationFactor) == 224, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxVertexAmplificationCount) == 228, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxTextureBufferWidth) == 232, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxComputeAttributes) == 236, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxIOCommandsInFlight) == 240, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxPredicatedNestingDepth) == 244, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxAccelerationStructureLevels) == 248, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxConstantBufferArguments) == 252, "Actual Metal field offset");
static_assert(offsetof(HNMetalDeviceLimits, maxBufferLength) == 256, "Actual Metal field offset");

namespace hackynavi { namespace metal {
// Metadata admission does not create a Context, execute a shader or grant GPU
// authority. The actual device uses Capabilities::capture on its live owner.
struct CapabilityData {
    uint64_t ownerRegistryID = 0, epoch = 0;
    uint64_t systemBudget = 0, vramBytes = 0, vaBegin = 0, vaEnd = 0;
    uint32_t pageBytes = 0;
    HNMetalDeviceLimits limits{};
};

// Owned query state for the two positive private compute queries implemented
// by this frontend. It is independent of Apple's private feature-profile
// object. Other features need their own implementation before admission.
struct CapabilityQueries {
    uint64_t ownerRegistryID = 0, epoch = 0;
    bool readWriteBufferArguments = false;
    bool nonUniformThreadgroupSize = false;
};

inline compute::Result deriveCapabilityData(const nativeabi::HelloReply& information,
                                             uint64_t expectedOwner, CapabilityData& output) {
    output = {};
    if (!expectedOwner) return {compute::Status::Invalid, 0};
    const bool vertexBuffered = information.header.minor == nativeabi::kVertexBufferedGraphicsMinor;
    const bool buffered = vertexBuffered || information.header.minor == nativeabi::kBufferedGraphicsMinor;
    const bool graphics = buffered || information.header.minor == nativeabi::kGraphicsMinor;
    const uint64_t required = vertexBuffered ? nativeabi::kVertexBufferedGraphicsCapabilities :
        buffered ? nativeabi::kBufferedGraphicsCapabilities :
        graphics ? nativeabi::kGraphicsCapabilities : nativeabi::kReusableCapabilities;
    if (nativeabi::validateHello(information, expectedOwner,
        required) !=
        nativeabi::Validation::Ok) return {compute::Status::Protocol, 0};
    // Additional features must receive an explicit implementation before they
    // can change this frontend profile. No unknown native capability is used.
    if (!nativeabi::reusableProfile(information.header.minor) ||
        information.capabilities != required ||
        information.pageBytes != 4096 || information.maxResources < 31)
        return {compute::Status::Unsupported, 0};
    const uint64_t pageBudget = information.systemBudget & ~uint64_t(4095);
    const uint64_t maximum = information.maxAllocationBytes < pageBudget ?
        information.maxAllocationBytes : pageBudget;
    if (!maximum || maximum > SIZE_MAX) return {compute::Status::Unsupported, 0};
    output.ownerRegistryID = information.registryId;
    output.epoch = information.header.epoch;
    output.systemBudget = information.systemBudget;
    output.vramBytes = information.vramBytes;
    output.vaBegin = information.vaBegin; output.vaEnd = information.vaEnd;
    output.pageBytes = information.pageBytes;
    // General resource bindings, sampling, arrays, blending and Metal feature
    // families are still unsupported. These bounds describe only factories
    // and commands actually implemented by the negotiated profile.
    output.limits.maxComputeBuffers = 31;
    output.limits.maxTotalComputeThreadsPerThreadgroup = gfx11occupancy::kAllAdmittedVariantsMaxThreads;
    output.limits.maxBufferLength = maximum;
    if (vertexBuffered) output.limits.maxVertexBuffers = 1;
    if (graphics) {
        output.limits.maxFramebufferStorageBits = 32;
        output.limits.maxColorAttachments = 1;
        output.limits.maxTextureWidth2D = 16384;
        output.limits.maxTextureHeight2D = 16384;
        output.limits.maxTextureLayers = 1;
        output.limits.maxViewportCount = 1;
        output.limits.maxVertexAmplificationFactor = 1;
        output.limits.maxVertexAmplificationCount = 1;
    }
    if (buffered) output.limits.maxFragmentBuffers = 1;
    return {compute::Status::Ok, 0};
}

// Immutable snapshot obtained from the exact opened reusable native owner.
// It is not liveness or retirement evidence; actual operations still use
// Context, which rejects work after close and owns real native cleanup.
class Capabilities final {
public:
    static compute::Result capture(const std::shared_ptr<compute::Context>&,
                                   uint64_t expectedOwner, Capabilities&);
    bool valid() const { return data_.epoch != 0; }
    uint64_t ownerRegistryID() const { return data_.ownerRegistryID; }
    uint64_t epoch() const { return data_.epoch; }
    uint64_t maxBufferLength() const { return data_.limits.maxBufferLength; }
    uint64_t systemBudget() const { return data_.systemBudget; }
    uint64_t vramBytes() const { return data_.vramBytes; }
    uint32_t pageBytes() const { return data_.pageBytes; }
    uint32_t maxThreadsPerThreadgroup() const { return data_.limits.maxTotalComputeThreadsPerThreadgroup; }
    uint32_t staticThreadgroupMemoryBytes() const { return data_.limits.maxComputeThreadgroupMemory; }
    uint32_t publicBufferSlots() const { return data_.limits.maxComputeBuffers; }
    // The caller owns the returned POD and exposes its stable address through
    // -limits only after actual initLimits has stored this complete value.
    compute::Result makeLimits(HNMetalDeviceLimits& output) const {
        output = {};
        if (!valid()) return {compute::Status::NotOpen, 0};
        output = data_.limits; return {compute::Status::Ok, 0};
    }
    compute::Result makeFeatureQueries(CapabilityQueries& output) const {
        output = {};
        if (!valid()) return {compute::Status::NotOpen, 0};
        output.ownerRegistryID = data_.ownerRegistryID; output.epoch = data_.epoch;
        output.readWriteBufferArguments = true;
        output.nonUniformThreadgroupSize = true;
        return {compute::Status::Ok, 0};
    }
    bool supportsReadWriteBufferArguments() const { return valid(); }
    bool supportsNonUniformThreadgroupSize() const { return valid(); }
private:
    CapabilityData data_{};
};
} }
