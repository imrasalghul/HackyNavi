// SPDX-License-Identifier: MIT
#include "hackynavi/native_abi.hpp"
namespace hackynavi { namespace nativeabi {
namespace {
bool powerOfTwo(uint64_t v) { return v && !(v & (v - 1)); }
bool range(uint64_t offset, uint64_t bytes, uint64_t bound) {
    return bytes && offset < bound && bytes <= bound - offset;
}
void copyBytes(void* dst, const void* src, size_t bytes) {
    auto* d = static_cast<uint8_t*>(dst); const auto* s = static_cast<const uint8_t*>(src);
    for (size_t n = 0; n < bytes; ++n) d[n] = s[n];
}
}
Header makeHeader(size_t bytes, uint64_t epoch, uint16_t minor) {
    return { kMagic, kMajor, minor, uint32_t(bytes), 0, epoch };
}
bool validHandle(Handle h) { return h.id && h.generation; }
bool sameHandle(Handle a, Handle b) { return a.id == b.id && a.generation == b.generation; }
Validation validateHeader(const Header& h, size_t bytes, uint64_t epoch, uint16_t minor) {
    if (bytes < sizeof(Header) || bytes > kMaxMessage || h.bytes != bytes) return Validation::Size;
    if (h.magic != kMagic || h.flags) return Validation::Header;
    if (minor > kMaxMinor || h.major != kMajor || h.minor != minor) return Validation::Version;
    if (h.epoch != epoch) return Validation::Epoch;
    return Validation::Ok;
}
Validation validateHelloRequest(const HelloRequest& r, uint64_t supported) {
    auto v = validateHeader(r.header, sizeof(r), 0, r.header.minor);
    if (v != Validation::Ok) return v;
    if (r.reserved) return Validation::Reserved;
    if (!r.requiredCapabilities || (r.requiredCapabilities & ~kKnownCapabilities) ||
        (r.requiredCapabilities & supported) != r.requiredCapabilities ||
        (r.header.minor == kGenericMinor && (r.requiredCapabilities & ~kRequiredCapabilities)))
        return Validation::Unsupported;
    if (bool(r.requiredCapabilities & CompilerFixtureCompute) != (r.header.minor == kCompilerMinor) ||
        bool(r.requiredCapabilities & ProductLeafCompute) != (r.header.minor == kProductMinor || reusableProfile(r.header.minor)) ||
        bool(r.requiredCapabilities & ReusableCompute) != reusableProfile(r.header.minor) ||
        bool(r.requiredCapabilities & PositionConstantColorGraphics) != graphicsProfile(r.header.minor) ||
        bool(r.requiredCapabilities & PositionFragmentBufferGraphics) != bufferedGraphicsProfile(r.header.minor) ||
        bool(r.requiredCapabilities & PositionVertexBufferGraphics) != (r.header.minor == kVertexBufferedGraphicsMinor) ||
        bool(r.requiredCapabilities & PositionVertexFragmentBufferGraphics) != (r.header.minor == kVertexBufferedGraphicsMinor) ||
        (reusableProfile(r.header.minor) && (r.requiredCapabilities & SealedSingleJob)))
        return Validation::Unsupported;
    return Validation::Ok;
}
Validation validateHello(const HelloReply& h, uint64_t id, uint64_t required) {
    auto v = validateHeader(h.header, sizeof(h), h.header.epoch, h.header.minor);
    if (v != Validation::Ok) return v;
    if (!h.header.epoch || !h.registryId || (id && h.registryId != id)) return Validation::Epoch;
    if (h.reserved[0] || h.reserved[1]) return Validation::Reserved;
    if (!required || (required & ~kKnownCapabilities) ||
        (h.capabilities & required) != required || h.vendorId != 0x1002 ||
        h.deviceId != 0x744c || h.gcVersion != 0x0b0000) return Validation::Unsupported;
    const bool reusable = (required & ReusableCompute) != 0;
    const bool builtIn = (required & (BuiltInCompute | CopiedBufferIO | SealedSingleJob)) != 0;
    const bool compiler = (required & CompilerFixtureCompute) != 0;
    const bool product = (required & ProductLeafCompute) != 0;
    const bool graphics = (required & PositionConstantColorGraphics) != 0;
    const bool buffered = (required & PositionFragmentBufferGraphics) != 0;
    const bool vertex = (required & PositionVertexBufferGraphics) != 0;
    if (bool(h.capabilities & PositionVertexBufferGraphics) != vertex ||
        bool(h.capabilities & PositionVertexFragmentBufferGraphics) != vertex ||
        bool(required & PositionVertexFragmentBufferGraphics) != vertex ||
        (h.header.minor == kVertexBufferedGraphicsMinor) != vertex || (vertex && !buffered))
        return Validation::Unsupported;
    if (bool(h.capabilities & PositionFragmentBufferGraphics) != buffered ||
        bufferedGraphicsProfile(h.header.minor) != buffered || (buffered && !graphics))
        return Validation::Unsupported;
    if (bool(h.capabilities & PositionConstantColorGraphics) != graphics ||
        graphicsProfile(h.header.minor) != graphics || (graphics && (!product || !reusable)))
        return Validation::Unsupported;
    if (bool(h.capabilities & ReusableCompute) != reusable ||
        reusableProfile(h.header.minor) != reusable) return Validation::Unsupported;
    if (reusable && (!product || compiler || (h.capabilities & SealedSingleJob) ||
        (h.capabilities & kReusableCapabilities) != kReusableCapabilities)) return Validation::Unsupported;
    const uint16_t productMinor = vertex ? kVertexBufferedGraphicsMinor : buffered ? kBufferedGraphicsMinor :
        graphics ? kGraphicsMinor : reusable ? kReusableMinor : kProductMinor;
    const uint64_t productCapabilities = graphics ? graphicsCapabilities(h.header.minor) : reusable ? kReusableCapabilities : kProductCapabilities;
    if (product && (compiler || h.header.minor != productMinor ||
        (h.capabilities & productCapabilities) != productCapabilities ||
        (h.capabilities & CompilerFixtureCompute))) return Validation::Unsupported;
    if (!product && (h.header.minor == kProductMinor || (h.capabilities & ProductLeafCompute)))
        return Validation::Unsupported;
    if (compiler && (h.header.minor != kCompilerMinor ||
        (h.capabilities & kCompilerCapabilities) != kCompilerCapabilities)) return Validation::Unsupported;
    if (!compiler && (h.header.minor == kCompilerMinor || (h.capabilities & CompilerFixtureCompute)))
        return Validation::Unsupported;
    const uint64_t baseCapabilities = reusable ? kReusableCapabilities : kBuiltInCapabilities;
    if ((builtIn || compiler || product) && (h.header.minor != (product ? productMinor : compiler ? kCompilerMinor : kMinor) ||
        (h.capabilities & baseCapabilities) != baseCapabilities ||
        (h.capabilities & (CapturedValidatedIb | CpuMapping)))) return Validation::Unsupported;
    if (!powerOfTwo(h.pageBytes) || h.pageBytes < 4096 || h.pageBytes > 65536 ||
        !h.vaBegin || h.vaBegin >= h.vaEnd || h.vaEnd > (1ULL << 48) ||
        (h.vaBegin % h.pageBytes) || (h.vaEnd % h.pageBytes) ||
        !h.maxAllocationBytes || (h.maxAllocationBytes % h.pageBytes) ||
        h.maxAllocationBytes > h.vaEnd - h.vaBegin || !h.vramBytes ||
        h.cpuVisibleVramBytes > h.vramBytes || !h.systemBudget ||
        !h.maxResources || h.maxResources > kMaxResources) return Validation::Bounds;
    if ((builtIn || compiler || product) ? (h.maxIbs || h.maxIbBytes) : (!h.maxIbs || h.maxIbs > kMaxIbs ||
        !h.maxIbBytes || h.maxIbBytes > kMaxIbBytes || h.maxIbBytes % 4)) return Validation::Bounds;
    return Validation::Ok;
}
Validation validateAllocate(const AllocateRequest& r, const HelloReply& h) {
    auto v = validateHeader(r.header, sizeof(r), h.header.epoch, h.header.minor);
    if (v != Validation::Ok) return v;
    if (r.reserved) return Validation::Reserved;
    if (r.flags & ~uint32_t(CpuVisible)) return Validation::Unsupported;
    if (r.domain != uint32_t(Domain::Vram) && r.domain != uint32_t(Domain::System)) return Validation::Unsupported;
    if ((h.capabilities & (SealedSingleJob | ReusableCompute)) && (r.domain != uint32_t(Domain::System) || r.flags))
        return Validation::Unsupported;
    if (!h.pageBytes || !r.bytes || r.bytes > h.maxAllocationBytes || r.bytes % h.pageBytes ||
        !powerOfTwo(r.alignment) || r.alignment < h.pageBytes || r.alignment > h.maxAllocationBytes)
        return Validation::Bounds;
    const uint64_t budget = r.domain == uint32_t(Domain::System) ? h.systemBudget :
        ((r.flags & CpuVisible) ? h.cpuVisibleVramBytes : h.vramBytes);
    return r.bytes <= budget ? Validation::Ok : Validation::Bounds;
}
Validation validateVa(const VaRequest& r, const HelloReply& h, uint64_t allocationBytes, bool unmap) {
    auto v = validateHeader(r.header, sizeof(r), h.header.epoch, h.header.minor);
    if (v != Validation::Ok) return v;
    if (r.reserved) return Validation::Reserved;
    if (!validHandle(r.handle)) return Validation::Handle;
    if ((unmap && r.access) || (!unmap && (!(r.access & Read) || (r.access & ~kAccessMask))))
        return Validation::Access;
    if (!h.pageBytes || r.va < h.vaBegin || r.va % h.pageBytes || r.offset % h.pageBytes ||
        r.bytes % h.pageBytes || !range(r.offset, r.bytes, allocationBytes) ||
        !range(r.va, r.bytes, h.vaEnd)) return Validation::Bounds;
    return Validation::Ok;
}
Validation decodeSubmit(const void* bytes, size_t size, const HelloReply& h, CapturedSubmit& out) {
    out = {};
    if (!bytes || size < sizeof(SubmitRequest) || size > kMaxMessage) return Validation::Size;
    SubmitRequest request{}; copyBytes(&request, bytes, sizeof(request));
    auto v = validateHeader(request.header, size, h.header.epoch, h.header.minor);
    if (v != Validation::Ok) return v;
    if (request.reserved) return Validation::Reserved;
    if (!request.ibCount || request.ibCount > kMaxIbs || request.ibCount > h.maxIbs ||
        !request.resourceCount || request.resourceCount > kMaxResources || request.resourceCount > h.maxResources)
        return Validation::Bounds;
    const size_t expected = sizeof(request) + request.ibCount * sizeof(Ib) + request.resourceCount * sizeof(Resource);
    if (size != expected) return Validation::Size;
    CapturedSubmit captured{}; captured.request = request;
    auto* cursor = static_cast<const uint8_t*>(bytes) + sizeof(request);
    copyBytes(captured.ibs, cursor, request.ibCount * sizeof(Ib)); cursor += request.ibCount * sizeof(Ib);
    copyBytes(captured.resources, cursor, request.resourceCount * sizeof(Resource));
    for (uint32_t n = 0; n < request.resourceCount; ++n) {
        const auto& r = captured.resources[n];
        if (!validHandle(r.handle)) return Validation::Handle;
        if (r.reserved) return Validation::Reserved;
        if (!r.access || (r.access & ~kAccessMask)) return Validation::Access;
        for (uint32_t p = 0; p < n; ++p)
            if (r.handle.id == captured.resources[p].handle.id) return Validation::Handle;
    }
    for (uint32_t n = 0; n < request.ibCount; ++n) {
        const auto& ib = captured.ibs[n];
        if (!validHandle(ib.handle)) return Validation::Handle;
        if (ib.reserved) return Validation::Reserved;
        if (ib.offset % 4 || !ib.words || ib.words > kMaxIbBytes / 4 || ib.words > h.maxIbBytes / 4 ||
            ib.offset > UINT64_MAX - uint64_t(ib.words) * 4) return Validation::Bounds;
        bool found = false;
        for (uint32_t r = 0; r < request.resourceCount; ++r)
            if (sameHandle(ib.handle, captured.resources[r].handle) &&
                (captured.resources[r].access & (Read | Execute)) == (Read | Execute)) found = true;
        if (!found) return Validation::Access;
    }
    out = captured;
    return Validation::Ok;
}
Validation validateWait(const WaitReply& r, uint64_t epoch, uint64_t requestedSequence, uint16_t minor) {
    auto v = validateHeader(r.header, sizeof(r), epoch, minor);
    if (v != Validation::Ok) return v;
    if (r.reserved) return Validation::Reserved;
    if (!requestedSequence || r.sequence != requestedSequence) return Validation::Bounds;
    if (r.state > uint32_t(FenceState::DeviceLost)) return Validation::Bounds;
    if ((r.state == uint32_t(FenceState::Complete) && r.completedSequence < requestedSequence) ||
        (r.state == uint32_t(FenceState::Pending) && r.completedSequence >= requestedSequence)) return Validation::Bounds;
    return Validation::Ok;
}
Validation validateBufferIo(const BufferIoRequest& r, size_t bytes, uint64_t epoch, bool write, uint16_t minor) {
    auto v = validateHeader(r.header, bytes, epoch, minor);
    if (v != Validation::Ok) return v;
    if (r.reserved) return Validation::Reserved;
    if (!validHandle(r.handle)) return Validation::Handle;
    if (!r.bytes || r.bytes > kMaxCopyBytes || r.offset > UINT64_MAX - r.bytes) return Validation::Bounds;
    if (bytes != sizeof(r) + (write ? r.bytes : 0)) return Validation::Size;
    return Validation::Ok;
}
Validation validateCompute(const ComputeRequest& r, uint64_t epoch, uint16_t minor) {
    auto v = validateHeader(r.header, sizeof(r), epoch, minor);
    if (v != Validation::Ok) return v;
    if (r.reserved) return Validation::Reserved;
    if (!validHandle(r.destination)) return Validation::Handle;
    if (!r.bytes || (r.bytes & 15) || (r.destinationOffset & 15) ||
        r.destinationOffset > UINT64_MAX - r.bytes) return Validation::Bounds;
    if (r.operation == uint32_t(ComputeOperation::Clear)) {
        if (r.source.id || r.source.generation || r.sourceOffset) return Validation::Reserved;
    } else if (r.operation == uint32_t(ComputeOperation::Copy)) {
        if (!validHandle(r.source)) return Validation::Handle;
        if ((r.sourceOffset & 15) || r.sourceOffset > UINT64_MAX - r.bytes) return Validation::Bounds;
        for (auto pattern : r.pattern) if (pattern) return Validation::Reserved;
        if (sameHandle(r.source, r.destination) && r.sourceOffset < r.destinationOffset + r.bytes &&
            r.destinationOffset < r.sourceOffset + r.bytes) return Validation::Bounds;
    } else return Validation::Unsupported;
    return Validation::Ok;
}
Validation validateReadResult(const ReadResultRequest& r, uint64_t epoch, uint16_t minor) {
    auto v = validateHeader(r.header, sizeof(r), epoch, minor);
    if (v != Validation::Ok) return v;
    if (r.reserved) return Validation::Reserved;
    if (!r.sequence || !r.bytes || r.bytes > kMaxCopyBytes || r.offset > UINT64_MAX - r.bytes)
        return Validation::Bounds;
    return Validation::Ok;
}
Validation validateCompilerCompute(const CompilerComputeRequest& r, const HelloReply& h) {
    auto v = validateHeader(r.header, sizeof(r), h.header.epoch, kCompilerMinor);
    if (v != Validation::Ok) return v;
    if (h.header.minor != kCompilerMinor || (h.capabilities & kCompilerCapabilities) != kCompilerCapabilities ||
        r.shaderClass != uint32_t(CompilerShaderClass::AppleAddOneGfx1100V1)) return Validation::Unsupported;
    if (r.reserved) return Validation::Reserved;
    if (!validHandle(r.source) || !validHandle(r.destination)) return Validation::Handle;
    if ((r.sourceOffset & 3) || (r.destinationOffset & 3) ||
        r.sourceOffset > UINT64_MAX - kCompilerResultBytes ||
        r.destinationOffset > UINT64_MAX - kCompilerResultBytes) return Validation::Bounds;
    if (sameHandle(r.source, r.destination) && r.sourceOffset < r.destinationOffset + kCompilerResultBytes &&
        r.destinationOffset < r.sourceOffset + kCompilerResultBytes) return Validation::Bounds;
    return Validation::Ok;
}
Validation validateProductCompute(const CapturedProductCompute& captured, uint64_t epoch, uint16_t minor) {
    if (minor != kProductMinor && !reusableProfile(minor)) return Validation::Unsupported;
    const auto& r = captured.request;
    if (!r.variantCount || r.variantCount > kMaxProductVariants ||
        !r.resourceCount || r.resourceCount > kMaxProductResources) return Validation::Bounds;
    const size_t bytes = sizeof(r) + r.variantCount * sizeof(ProductVariant) +
        r.resourceCount * sizeof(ProductResource);
    auto status = validateHeader(r.header, bytes, epoch, minor);
    if (status != Validation::Ok) return status;
    if (r.reserved) return Validation::Reserved;
    if (!validHandle(r.result)) return Validation::Handle;
    if (!r.resultBytes || (r.resultOffset & 3) || (r.resultBytes & 3) ||
        r.resultOffset > UINT64_MAX - r.resultBytes) return Validation::Bounds;
    uint64_t gridThreads = 1, localThreads = 1;
    for (unsigned d = 0; d < 3; ++d) {
        if (!r.grid[d] || !r.nominalLocal[d] || r.nominalLocal[d] > kMaxProductLocalThreads ||
            gridThreads > UINT64_MAX / r.grid[d]) return Validation::Bounds;
        gridThreads *= r.grid[d]; localThreads *= r.nominalLocal[d];
    }
    if (localThreads > kMaxProductLocalThreads) return Validation::Bounds;
    for (uint32_t n = 0; n < r.variantCount; ++n) {
        const auto& variant = captured.variants[n];
        if (!validHandle(variant.code)) return Validation::Handle;
        if (variant.reserved[0] || variant.reserved[1]) return Validation::Reserved;
        if ((variant.offset & 3) || !variant.codeBytes || variant.codeBytes > kMaxProductCodeBytes ||
            (variant.codeBytes & 3) || !variant.executableBytes || (variant.executableBytes & 3) ||
            variant.executableBytes > variant.codeBytes || variant.offset > UINT64_MAX - variant.codeBytes)
            return Validation::Bounds;
        uint64_t threads = 1;
        for (unsigned d = 0; d < 3; ++d) {
            if (!variant.local[d] || variant.local[d] > kMaxProductLocalThreads) return Validation::Bounds;
            threads *= variant.local[d];
        }
        if (threads > kMaxProductLocalThreads || variant.waveSize != 32 ||
            variant.numSgprs < 8 || variant.numSgprs > 128 ||
            !variant.numVgprs || variant.numVgprs > 256) return Validation::Bounds;
        for (uint32_t p = 0; p < n; ++p) {
            const auto& previous = captured.variants[p];
            if (variant.local[0] == previous.local[0] && variant.local[1] == previous.local[1] &&
                variant.local[2] == previous.local[2]) return Validation::Bounds;
            if (variant.code.id == previous.code.id) {
                if (!sameHandle(variant.code, previous.code)) return Validation::Handle;
                if (variant.offset < previous.offset + previous.codeBytes &&
                    previous.offset < variant.offset + variant.codeBytes) return Validation::Bounds;
            }
        }
    }
    // The kernel's builder checks this again against admitted shader metadata.
    // Ensure this envelope supplies exactly the distinct full/tail local sizes.
    bool used[kMaxProductVariants] = {};
    for (unsigned mask = 0; mask < 8; ++mask) {
        uint32_t local[3] = {}; bool nonempty = true;
        for (unsigned d = 0; d < 3; ++d) {
            const uint32_t full = r.grid[d] / r.nominalLocal[d], tail = r.grid[d] % r.nominalLocal[d];
            if (mask & (1U << d)) { local[d] = tail; if (!tail) nonempty = false; }
            else { local[d] = r.nominalLocal[d]; if (!full) nonempty = false; }
        }
        if (!nonempty) continue;
        uint32_t n = 0;
        while (n < r.variantCount && !(local[0] == captured.variants[n].local[0] &&
               local[1] == captured.variants[n].local[1] && local[2] == captured.variants[n].local[2])) ++n;
        if (n == r.variantCount) return Validation::Bounds;
        used[n] = true;
    }
    for (uint32_t n = 0; n < r.variantCount; ++n) if (!used[n]) return Validation::Bounds;
    bool resultFound = false;
    for (uint32_t n = 0; n < r.resourceCount; ++n) {
        const auto& resource = captured.resources[n];
        if (!validHandle(resource.buffer)) return Validation::Handle;
        if (resource.reserved) return Validation::Reserved;
        if (resource.binding >= kMaxProductResources || !resource.bytes || resource.bytes > UINT32_MAX ||
            (resource.offset & 3) || (resource.bytes & 3) || resource.offset > UINT64_MAX - resource.bytes)
            return Validation::Bounds;
        for (uint32_t p = 0; p < n; ++p) {
            if (resource.buffer.id == captured.resources[p].buffer.id) return Validation::Handle;
            if (resource.binding == captured.resources[p].binding) return Validation::Bounds;
        }
        for (uint32_t p = 0; p < r.variantCount; ++p)
            if (resource.buffer.id == captured.variants[p].code.id) return Validation::Handle;
        if (resource.buffer.id == r.result.id) {
            if (!sameHandle(resource.buffer, r.result)) return Validation::Handle;
            if (r.resultOffset < resource.offset || r.resultOffset - resource.offset > resource.bytes ||
                r.resultBytes > resource.bytes - (r.resultOffset - resource.offset)) return Validation::Bounds;
            resultFound = true;
        }
    }
    return resultFound ? Validation::Ok : Validation::Handle;
}
Validation decodeProductCompute(const void* bytes, size_t size, const HelloReply& h,
                                 CapturedProductCompute& out) {
    out = {};
    if (!bytes || size < sizeof(ProductComputeRequest) || size > kMaxMessage) return Validation::Size;
    ProductComputeRequest request{}; copyBytes(&request, bytes, sizeof(request));
    if (h.header.minor != kProductMinor && !reusableProfile(h.header.minor)) return Validation::Unsupported;
    auto status = validateHeader(request.header, size, h.header.epoch, h.header.minor);
    if (status != Validation::Ok) return status;
    const bool reusable = reusableProfile(h.header.minor);
    const bool graphics = graphicsProfile(h.header.minor);
    const uint64_t required = graphics ? graphicsCapabilities(h.header.minor) : reusable ? kReusableCapabilities : kProductCapabilities;
    const uint64_t forbidden = CompilerFixtureCompute | CapturedValidatedIb | CpuMapping |
        (graphics ? uint64_t(0) : uint64_t(PositionConstantColorGraphics)) |
        (bufferedGraphicsProfile(h.header.minor) ? uint64_t(0) : uint64_t(PositionFragmentBufferGraphics)) |
        (h.header.minor == kVertexBufferedGraphicsMinor ? uint64_t(0) :
            uint64_t(PositionVertexBufferGraphics | PositionVertexFragmentBufferGraphics)) |
        (reusable ? uint64_t(SealedSingleJob) : uint64_t(ReusableCompute));
    if ((h.capabilities & required) != required || (h.capabilities & forbidden)) return Validation::Unsupported;
    if (!request.variantCount || request.variantCount > kMaxProductVariants || !request.resourceCount ||
        request.resourceCount > kMaxProductResources || request.resourceCount > h.maxResources) return Validation::Bounds;
    const size_t expected = sizeof(request) + request.variantCount * sizeof(ProductVariant) +
        request.resourceCount * sizeof(ProductResource);
    if (size != expected) return Validation::Size;
    // The native caller provides heap-owned output. Do not add another 1552B
    // capture to the kernel stack merely to clear it on validation failure.
    out.request = request;
    const auto* cursor = static_cast<const uint8_t*>(bytes) + sizeof(request);
    copyBytes(out.variants, cursor, request.variantCount * sizeof(ProductVariant));
    cursor += request.variantCount * sizeof(ProductVariant);
    copyBytes(out.resources, cursor, request.resourceCount * sizeof(ProductResource));
    status = validateProductCompute(out, h.header.epoch, h.header.minor);
    if (status != Validation::Ok) out = {};
    return status;
}
Validation validateReleaseResult(const ReleaseResultRequest& r, const HelloReply& h) {
    if (!reusableProfile(h.header.minor) ||
        validateHello(h, 0, graphicsProfile(h.header.minor) ? graphicsCapabilities(h.header.minor) : kReusableCapabilities)
            != Validation::Ok) return Validation::Unsupported;
    const auto status = validateHeader(r.header, sizeof(r), h.header.epoch, h.header.minor);
    if (status != Validation::Ok) return status;
    if (r.reserved) return Validation::Reserved;
    return r.sequence ? Validation::Ok : Validation::Bounds;
}
Validation validateGraphicsInformation(const GraphicsInformationReply& r, const HelloReply& h) {
    if (!graphicsProfile(h.header.minor) || validateHello(h, 0, graphicsCapabilities(h.header.minor)) != Validation::Ok)
        return Validation::Unsupported;
    const auto status = validateHeader(r.header, sizeof(r), h.header.epoch, h.header.minor);
    if (status != Validation::Ok) return status;
    return r.reserved ? Validation::Reserved : Validation::Ok;
}
static Validation validateGraphicsFields(const ProductGraphicsRequest& r) {
    if (r.reserved) return Validation::Reserved;
    if (!validHandle(r.code) || !validHandle(r.target) || r.code.id == r.target.id) return Validation::Handle;
    if ((r.codeOffset & 3) || r.codeOffset > UINT64_MAX - kGraphicsImageBytes ||
        r.vertexBytes <= 20 || r.vertexBytes > kGraphicsStageBytes || (r.vertexBytes & 3) ||
        r.fragmentBytes <= 20 || r.fragmentBytes > kGraphicsStageBytes || (r.fragmentBytes & 3))
        return Validation::Bounds;
    const auto& l = r.targetLayout;
    // Bound CPU result reservation before any native allocation/admission.
    // The kernel separately checks every actual Mesa layout/register field.
    if (!l.width || !l.height || l.width > 16384 || l.height > 16384 ||
        l.pitch_pixels < l.width || l.pitch_pixels > 16384 || (l.pitch_pixels & 63) ||
        l.pitch_bytes != l.pitch_pixels * 4 || l.slice_bytes != uint64_t(l.pitch_bytes) * l.height ||
        l.total_bytes != l.slice_bytes || !l.total_bytes || l.total_bytes > kMaxGraphicsTargetBytes ||
        l.base_alignment != 256 || (r.targetOffset & 4095) || r.targetOffset > UINT64_MAX - l.total_bytes)
        return Validation::Bounds;
    return Validation::Ok;
}
Validation validateProductGraphics(const ProductGraphicsRequest& r, uint64_t epoch, uint16_t minor) {
    if (!graphicsProfile(minor)) return Validation::Unsupported;
    const auto status = validateHeader(r.header, sizeof(r), epoch, minor);
    return status == Validation::Ok ? validateGraphicsFields(r) : status;
}
Validation validateProductBufferedGraphics(const ProductBufferedGraphicsRequest& r, uint64_t epoch, uint16_t minor) {
    if (!bufferedGraphicsProfile(minor)) return Validation::Unsupported;
    auto status = validateHeader(r.draw.header, sizeof(r), epoch, minor);
    if (status != Validation::Ok) return status;
    status = validateGraphicsFields(r.draw);
    if (status != Validation::Ok) return status;
    if (!validHandle(r.fragment) || r.fragment.id == r.draw.code.id || r.fragment.id == r.draw.target.id)
        return Validation::Handle;
    if (r.fragmentBytes != 16 || (r.fragmentOffset & 15) || r.fragmentOffset > UINT64_MAX - 16)
        return Validation::Bounds;
    return Validation::Ok;
}
Validation decodeProductGraphics(const void* bytes, size_t size, const HelloReply& h, ProductGraphicsRequest& out) {
    out = {};
    if (!bytes || size != sizeof(ProductGraphicsRequest)) return Validation::Size;
    if (!graphicsProfile(h.header.minor) || validateHello(h, 0, graphicsCapabilities(h.header.minor)) != Validation::Ok)
        return Validation::Unsupported;
    ProductGraphicsRequest captured{}; copyBytes(&captured, bytes, sizeof(captured));
    const auto status = validateProductGraphics(captured, h.header.epoch, h.header.minor);
    if (status == Validation::Ok) out = captured;
    return status;
}
Validation decodeProductBufferedGraphics(const void* bytes, size_t size, const HelloReply& h,
                                         ProductBufferedGraphicsRequest& out) {
    out = {};
    if (!bytes || size != sizeof(ProductBufferedGraphicsRequest)) return Validation::Size;
    if (!bufferedGraphicsProfile(h.header.minor) || validateHello(h, 0, graphicsCapabilities(h.header.minor)) != Validation::Ok)
        return Validation::Unsupported;
    ProductBufferedGraphicsRequest captured{}; copyBytes(&captured, bytes, sizeof(captured));
    const auto status = validateProductBufferedGraphics(captured, h.header.epoch, h.header.minor);
    if (status == Validation::Ok) out = captured;
    return status;
}
static Validation validateVertexGraphicsFields(const ProductVertexBufferedGraphicsRequest& r) {
    auto status = validateGraphicsFields(r.draw);
    if (status != Validation::Ok) return status;
    if (!validHandle(r.vertex) || r.vertex.id == r.draw.code.id || r.vertex.id == r.draw.target.id)
        return Validation::Handle;
    if (r.vertexBufferBytes != 48 || (r.vertexOffset & 15) || r.vertexOffset > UINT64_MAX - 48)
        return Validation::Bounds;
    return Validation::Ok;
}
Validation validateProductVertexBufferedGraphics(const ProductVertexBufferedGraphicsRequest& r,
    uint64_t epoch, uint16_t minor) {
    if (minor != kVertexBufferedGraphicsMinor) return Validation::Unsupported;
    const auto status = validateHeader(r.draw.header, sizeof(r), epoch, minor);
    return status == Validation::Ok ? validateVertexGraphicsFields(r) : status;
}
Validation validateProductVertexFragmentBufferedGraphics(const ProductVertexFragmentBufferedGraphicsRequest& r,
    uint64_t epoch, uint16_t minor) {
    if (minor != kVertexBufferedGraphicsMinor) return Validation::Unsupported;
    auto status = validateHeader(r.vertexDraw.draw.header, sizeof(r), epoch, minor);
    if (status != Validation::Ok) return status;
    status = validateVertexGraphicsFields(r.vertexDraw);
    if (status != Validation::Ok) return status;
    if (!validHandle(r.fragment) || r.fragment.id == r.vertexDraw.draw.code.id ||
        r.fragment.id == r.vertexDraw.draw.target.id || r.fragment.id == r.vertexDraw.vertex.id)
        return Validation::Handle;
    if (r.fragmentBufferBytes != 16 || (r.fragmentOffset & 15) || r.fragmentOffset > UINT64_MAX - 16)
        return Validation::Bounds;
    return Validation::Ok;
}
Validation decodeProductVertexBufferedGraphics(const void* bytes, size_t size, const HelloReply& h,
    ProductVertexBufferedGraphicsRequest& out) {
    out = {};
    if (!bytes || size != sizeof(out)) return Validation::Size;
    if (h.header.minor != kVertexBufferedGraphicsMinor ||
        validateHello(h, 0, kVertexBufferedGraphicsCapabilities) != Validation::Ok) return Validation::Unsupported;
    ProductVertexBufferedGraphicsRequest captured{}; copyBytes(&captured, bytes, sizeof(captured));
    const auto status = validateProductVertexBufferedGraphics(captured, h.header.epoch, h.header.minor);
    if (status == Validation::Ok) out = captured;
    return status;
}
Validation decodeProductVertexFragmentBufferedGraphics(const void* bytes, size_t size, const HelloReply& h,
    ProductVertexFragmentBufferedGraphicsRequest& out) {
    out = {};
    if (!bytes || size != sizeof(out)) return Validation::Size;
    if (h.header.minor != kVertexBufferedGraphicsMinor ||
        validateHello(h, 0, kVertexBufferedGraphicsCapabilities) != Validation::Ok) return Validation::Unsupported;
    ProductVertexFragmentBufferedGraphicsRequest captured{}; copyBytes(&captured, bytes, sizeof(captured));
    const auto status = validateProductVertexFragmentBufferedGraphics(captured, h.header.epoch, h.header.minor);
    if (status == Validation::Ok) out = captured;
    return status;
}
} }
